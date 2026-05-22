#include "pet_pack.h"

#include <string.h>

#include "esp_log.h"

#include "mochi_pack.h"
#include "pack_cache.h"

static const char *TAG = "pet_pack";

/* Embedded blob — the binary at firmware/main/assets/pet_a.mpk gets
 * bundled by EMBED_FILES in CMakeLists.txt. The symbol naming follows
 * the IDF convention: dots and dashes in the filename get translated
 * to underscores. */
extern const uint8_t _binary_pet_a_mpk_start[]
    asm("_binary_pet_a_mpk_start");

static mpk_t s_pack;
static bool  s_open;

bool pet_pack_init(void) {
    if (s_open) return true;
    /* "Sync at boot": prefer the server pack (substrate-authored) over
     * the cached copy over the embedded baseline. pet_a.mpk is the
     * pet-v1 sheet. See pack_cache.h / design/15. */
    const uint8_t *bytes = pack_cache_active("pet-v1", _binary_pet_a_mpk_start);
    int rc = mpk_open(bytes, &s_pack);
    if (rc != 0) {
        ESP_LOGE(TAG, "mpk_open rc=%d", rc);
        return false;
    }
    if (!s_pack.has_mask) {
        ESP_LOGW(TAG, "pet pack has no mask plane — pet would render"
                      " opaque against the scene; refusing to use it");
        return false;
    }
    ESP_LOGI(TAG, "pet_pack: %ux%u %u cells (mask=%d) ready",
        (unsigned)s_pack.cell_w, (unsigned)s_pack.cell_h,
        (unsigned)s_pack.count, (int)s_pack.has_mask);
    s_open = true;
    return true;
}

bool pet_pack_has(const char *expr) {
    if (!s_open || !expr) return false;
    return mpk_find(&s_pack, expr) >= 0;
}

bool pet_pack_load(const char *expr,
                   uint8_t *dst_ink, uint8_t *dst_mask,
                   size_t dst_bytes,
                   uint16_t *out_w, uint16_t *out_h) {
    if (!s_open || !expr || !dst_ink || !dst_mask) return false;
    int idx = mpk_find(&s_pack, expr);
    if (idx < 0) return false;
    if (dst_bytes < s_pack.plane_bytes) {
        ESP_LOGW(TAG, "dst %u < plane %u", (unsigned)dst_bytes,
            (unsigned)s_pack.plane_bytes);
        return false;
    }
    const uint8_t *ink  = mpk_ink(&s_pack, (uint16_t)idx);
    const uint8_t *mask = mpk_mask(&s_pack, (uint16_t)idx);
    if (!mask) return false;
    memcpy(dst_ink,  ink,  s_pack.plane_bytes);
    memcpy(dst_mask, mask, s_pack.plane_bytes);
    if (out_w) *out_w = s_pack.cell_w;
    if (out_h) *out_h = s_pack.cell_h;
    return true;
}
