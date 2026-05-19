#include "sprite_cache.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_littlefs.h"

static const char *TAG = "sprite_cache";

#define MOUNT_POINT       "/lfs"
#define PARTITION_LABEL   "storage"

namespace sprite_cache {

static bool s_inited = false;

bool init(void) {
    if (s_inited) return true;

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = MOUNT_POINT;
    conf.partition_label = PARTITION_LABEL;
    conf.format_if_mount_failed = true;     /* first boot: format */
    conf.dont_mount = false;

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0, used = 0;
    if (esp_littlefs_info(PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    }

    /* Ensure base dirs exist; mkdir is harmless if they're already
     * there (LittleFS returns -1 with errno=EEXIST). */
    mkdir(MOUNT_POINT "/sprites", 0777);
    mkdir(MOUNT_POINT "/etags",   0777);

    s_inited = true;
    return true;
}

/* All paths get composed from short fixed-length pieces, so a
 * 96-byte buffer is comfortably more than enough. */
static void blob_path(const char *sheet, const char *suffix,
                      char *out, size_t out_cap) {
    snprintf(out, out_cap, "%s/sprites/%s_%s.bin",
        MOUNT_POINT, sheet, suffix);
}
static void etag_path(const char *sheet, char *out, size_t out_cap) {
    snprintf(out, out_cap, "%s/etags/%s.tag", MOUNT_POINT, sheet);
}

bool load(const char *sheet, const char *suffix,
          uint8_t *out_buf, size_t max_bytes, size_t *out_size) {
    if (!s_inited) return false;
    char path[96];
    blob_path(sheet, suffix, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(out_buf, 1, max_bytes, f);
    fclose(f);
    if (out_size) *out_size = n;
    return n > 0;
}

bool store(const char *sheet, const char *suffix,
           const uint8_t *bytes, size_t size) {
    if (!s_inited) return false;
    char path[96];
    blob_path(sheet, suffix, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "store fopen failed: %s", path);
        return false;
    }
    size_t n = fwrite(bytes, 1, size, f);
    fclose(f);
    return n == size;
}

bool load_etag(const char *sheet, char *out, size_t out_cap) {
    if (!s_inited || out_cap == 0) return false;
    char path[96];
    etag_path(sheet, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(out, 1, out_cap - 1, f);
    fclose(f);
    out[n] = 0;
    /* Trim trailing whitespace/newlines that might have crept in
     * from a manual edit or a partial write. */
    while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r' ||
                     out[n-1] == ' '  || out[n-1] == '\t')) {
        out[--n] = 0;
    }
    return n > 0;
}

bool store_etag(const char *sheet, const char *etag) {
    if (!s_inited || !etag) return false;
    char path[96];
    etag_path(sheet, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t len = strlen(etag);
    size_t n = fwrite(etag, 1, len, f);
    fclose(f);
    return n == len;
}

void invalidate_sheet(const char *sheet) {
    if (!s_inited) return;
    /*
     * Walk /sprites and remove anything whose filename starts with
     * "<sheet>_". This is a small directory (a few dozen entries
     * at most) so the linear scan is fine. Also drop the ETag.
     */
    char dir_path[64];
    snprintf(dir_path, sizeof(dir_path), "%s/sprites", MOUNT_POINT);
    DIR *d = opendir(dir_path);
    if (d) {
        char prefix[40];
        int prefix_len = snprintf(prefix, sizeof(prefix), "%s_", sheet);
        struct dirent *ent;
        /* Worst-case path = "<MOUNT_POINT>/sprites/<256-char d_name>".
         * sizeof(MOUNT_POINT "/sprites/") is small; making `full`
         * 320 bytes covers any d_name LittleFS could hand us. */
        while ((ent = readdir(d)) != nullptr) {
            if (strncmp(ent->d_name, prefix, prefix_len) == 0) {
                char full[320];
                snprintf(full, sizeof(full), "%s/%s", dir_path, ent->d_name);
                unlink(full);
            }
        }
        closedir(d);
    }
    char path[96];
    etag_path(sheet, path, sizeof(path));
    unlink(path);
    ESP_LOGI(TAG, "invalidated cache for sheet '%s'", sheet);
}

}  /* namespace sprite_cache */
