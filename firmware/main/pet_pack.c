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

/* Open `bytes` into s_pack and validate the mask-plane requirement.
 * Used by both init paths. Returns true on success. */
static bool open_into_active(const uint8_t *bytes, const char *src) {
    int rc = mpk_open(bytes, &s_pack);
    if (rc != 0) {
        ESP_LOGE(TAG, "mpk_open(%s) rc=%d", src, rc);
        return false;
    }
    if (!s_pack.has_mask) {
        ESP_LOGW(TAG, "pet pack has no mask plane — pet would render"
                      " opaque against the scene; refusing to use it");
        return false;
    }
    ESP_LOGI(TAG, "pet_pack: %ux%u %u cells (mask=%d, src=%s) ready",
        (unsigned)s_pack.cell_w, (unsigned)s_pack.cell_h,
        (unsigned)s_pack.count, (int)s_pack.has_mask, src);
    s_open = true;
    return true;
}

bool pet_pack_init_embedded(void) {
    if (s_open) return true;
    return open_into_active(_binary_pet_a_mpk_start, "embedded");
}

bool pet_pack_init(void) {
    /* "Sync at boot": prefer the server pack (substrate-authored) over
     * the cached copy over the embedded baseline. pet_a.mpk is the
     * pet-v1 sheet. See pack_cache.h / design/15.
     *
     * Idempotent on success: if a previous embedded-only init opened
     * the pack we re-resolve the active bytes (server may now be
     * reachable) and re-validate. The fast path (already on the
     * server-synced pack) is a near-no-op — pack_cache_active just
     * reads the cached ETag and returns the cached buffer. */
    const uint8_t *bytes = pack_cache_active("pet-v1", _binary_pet_a_mpk_start);
    return open_into_active(bytes, "synced");
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
