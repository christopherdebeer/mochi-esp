#include "nvs_creds.h"

#include <stdio.h>
#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "nvs_creds";
static const char *NS = "wifi";
static const char *KEY_COUNT = "count";

/* NVS key names: ssid_<i>, pass_<i>. NVS keys are bounded to 15 chars
 * + NUL, so "ssid_99" is fine for any plausible MOCHI_WIFI_CREDS_MAX. */
static void key_for(char *buf, size_t buf_len, const char *prefix, size_t i) {
    snprintf(buf, buf_len, "%s_%u", prefix, (unsigned)i);
}

/* One-shot migration from the pre-multi single-slot schema (keys
 * "ssid"/"password" in the same namespace, no "count") to the
 * indexed-list schema. Idempotent: if "count" already exists we do
 * nothing. If it doesn't but the legacy keys do, we promote them to
 * slot 0 and set count=1. */
static void migrate_legacy_single_slot(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;

    uint8_t count = 0;
    if (nvs_get_u8(h, KEY_COUNT, &count) == ESP_OK) {
        nvs_close(h);
        return;   /* already migrated */
    }

    char ssid[MOCHI_WIFI_SSID_MAX + 1] = {};
    char pass[MOCHI_WIFI_PASS_MAX + 1] = {};
    size_t ssid_len = sizeof(ssid);
    size_t pass_len = sizeof(pass);
    esp_err_t e1 = nvs_get_str(h, "ssid",     ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(h, "password", pass, &pass_len);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        nvs_close(h);
        return;   /* nothing to migrate */
    }

    /* Promote into slot 0. */
    if (nvs_set_str(h, "ssid_0", ssid) == ESP_OK &&
        nvs_set_str(h, "pass_0", pass) == ESP_OK &&
        nvs_set_u8 (h, KEY_COUNT, 1)   == ESP_OK) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "password");
        nvs_commit(h);
        ESP_LOGI(TAG, "migrated legacy single-slot creds for SSID '%s'", ssid);
    } else {
        ESP_LOGW(TAG, "migration write failed; leaving legacy keys in place");
    }
    nvs_close(h);
}

void nvs_creds_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash erase + reinit");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    migrate_legacy_single_slot();
}

size_t nvs_creds_count(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint8_t n = 0;
    esp_err_t err = nvs_get_u8(h, KEY_COUNT, &n);
    nvs_close(h);
    if (err != ESP_OK) return 0;
    if (n > MOCHI_WIFI_CREDS_MAX) n = MOCHI_WIFI_CREDS_MAX;
    return n;
}

bool nvs_creds_load_at(size_t i, struct mochi_wifi_creds *out) {
    if (i >= MOCHI_WIFI_CREDS_MAX) return false;
    if (i >= nvs_creds_count()) return false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;

    char ssid_key[16], pass_key[16];
    key_for(ssid_key, sizeof(ssid_key), "ssid", i);
    key_for(pass_key, sizeof(pass_key), "pass", i);

    size_t ssid_len = sizeof(out->ssid);
    size_t pass_len = sizeof(out->password);
    memset(out, 0, sizeof(*out));
    esp_err_t e1 = nvs_get_str(h, ssid_key, out->ssid,     &ssid_len);
    esp_err_t e2 = nvs_get_str(h, pass_key, out->password, &pass_len);
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGW(TAG, "load_at(%u) failed: ssid=%d pass=%d",
            (unsigned)i, e1, e2);
        return false;
    }
    return true;
}

/* Read all stored creds into a caller buffer, returning the count. */
static size_t read_all(struct mochi_wifi_creds out[MOCHI_WIFI_CREDS_MAX]) {
    size_t n = nvs_creds_count();
    for (size_t i = 0; i < n; i++) {
        if (!nvs_creds_load_at(i, &out[i])) {
            return i;   /* truncate at first failure */
        }
    }
    return n;
}

/* Write all creds + count atomically(ish): open one handle, write
 * everything, single commit. NVS doesn't give us true transactions
 * but a single commit is the closest thing. */
static bool write_all(const struct mochi_wifi_creds *creds, size_t n) {
    if (n > MOCHI_WIFI_CREDS_MAX) n = MOCHI_WIFI_CREDS_MAX;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;

    /* Erase any old slots beyond the new count so stale entries
     * don't survive. NVS will return ESP_ERR_NVS_NOT_FOUND for
     * unused indices; that's expected and not fatal. */
    for (size_t i = 0; i < MOCHI_WIFI_CREDS_MAX; i++) {
        char ssid_key[16], pass_key[16];
        key_for(ssid_key, sizeof(ssid_key), "ssid", i);
        key_for(pass_key, sizeof(pass_key), "pass", i);
        if (i < n) {
            if (nvs_set_str(h, ssid_key, creds[i].ssid)     != ESP_OK ||
                nvs_set_str(h, pass_key, creds[i].password) != ESP_OK) {
                nvs_close(h);
                return false;
            }
        } else {
            nvs_erase_key(h, ssid_key);
            nvs_erase_key(h, pass_key);
        }
    }

    if (nvs_set_u8(h, KEY_COUNT, (uint8_t)n) != ESP_OK) {
        nvs_close(h);
        return false;
    }

    esp_err_t ec = nvs_commit(h);
    nvs_close(h);
    return ec == ESP_OK;
}

bool nvs_creds_append(const struct mochi_wifi_creds *creds) {
    struct mochi_wifi_creds list[MOCHI_WIFI_CREDS_MAX] = {};
    size_t n = read_all(list);

    /* Drop any existing entry with the same SSID — we'll re-insert
     * at MRU index 0, refreshing the password to whatever the user
     * just typed. */
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        if (strncmp(list[i].ssid, creds->ssid,
                    MOCHI_WIFI_SSID_MAX) == 0) {
            continue;
        }
        if (kept != i) list[kept] = list[i];
        kept++;
    }
    n = kept;

    /* Shift everything down by one, drop the tail if we'd overflow. */
    size_t to_keep = n;
    if (to_keep >= MOCHI_WIFI_CREDS_MAX) {
        to_keep = MOCHI_WIFI_CREDS_MAX - 1;
    }
    for (size_t i = to_keep; i > 0; i--) {
        list[i] = list[i - 1];
    }
    list[0] = *creds;
    n = to_keep + 1;

    if (!write_all(list, n)) {
        ESP_LOGE(TAG, "append failed");
        return false;
    }
    ESP_LOGI(TAG, "appended creds for SSID '%s' (count=%u)",
        creds->ssid, (unsigned)n);
    return true;
}

bool nvs_creds_clear_all(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    /* Wipe every slot + the count. */
    for (size_t i = 0; i < MOCHI_WIFI_CREDS_MAX; i++) {
        char ssid_key[16], pass_key[16];
        key_for(ssid_key, sizeof(ssid_key), "ssid", i);
        key_for(pass_key, sizeof(pass_key), "pass", i);
        nvs_erase_key(h, ssid_key);
        nvs_erase_key(h, pass_key);
    }
    nvs_erase_key(h, KEY_COUNT);
    /* Also erase the legacy "ssid"/"password" keys from the
     * pre-multi era; nvs_erase_key on a missing key is harmless. */
    nvs_erase_key(h, "ssid");
    nvs_erase_key(h, "password");
    esp_err_t ec = nvs_commit(h);
    nvs_close(h);
    return ec == ESP_OK;
}

/* --- Legacy shims --- */

bool nvs_creds_load(struct mochi_wifi_creds *out) {
    return nvs_creds_load_at(0, out);
}

bool nvs_creds_save(const struct mochi_wifi_creds *creds) {
    return nvs_creds_append(creds);
}

bool nvs_creds_clear(void) {
    return nvs_creds_clear_all();
}

/* --- prov-on-boot flag --- */
static const char *KEY_PROV_FLAG = "prov_on_boot";

bool nvs_creds_set_prov_on_boot(bool flag) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err;
    if (flag) {
        err = nvs_set_u8(h, KEY_PROV_FLAG, 1);
    } else {
        err = nvs_erase_key(h, KEY_PROV_FLAG);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
}

bool nvs_creds_get_prov_on_boot(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    esp_err_t err = nvs_get_u8(h, KEY_PROV_FLAG, &v);
    nvs_close(h);
    return (err == ESP_OK) && (v != 0);
}
