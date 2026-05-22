#include "scene_pack.h"

#include <string.h>

#include "esp_log.h"

#include "mochi_pack.h"
#include "scenes_a_meta.h"

static const char *TAG = "scene_pack";

/* Embedded blob — the binary at firmware/main/assets/scenes_a.mpk
 * gets bundled by the EMBED_FILES line in CMakeLists.txt. The
 * symbol naming follows the IDF convention: dots and dashes in the
 * filename get translated to underscores. */
extern const uint8_t _binary_scenes_a_mpk_start[]
    asm("_binary_scenes_a_mpk_start");

static mpk_t     s_pack;
static bool      s_open;
static uint16_t  s_current;

bool scene_pack_init(void) {
    if (s_open) return true;
    int rc = mpk_open(_binary_scenes_a_mpk_start, &s_pack);
    if (rc != 0) {
        ESP_LOGE(TAG, "mpk_open rc=%d", rc);
        return false;
    }
    if (s_pack.count != SCENES_A_COUNT) {
        ESP_LOGW(TAG, "pack count %u != meta count %u — meta header"
                      " out of sync with .mpk binary",
            (unsigned)s_pack.count, (unsigned)SCENES_A_COUNT);
    }
    ESP_LOGI(TAG, "scene_pack: %ux%u %u cells (mask=%d) ready",
        (unsigned)s_pack.cell_w, (unsigned)s_pack.cell_h,
        (unsigned)s_pack.count, (int)s_pack.has_mask);
    s_open = true;
    s_current = 0;
    return true;
}

uint16_t scene_pack_count(void) {
    return s_open ? s_pack.count : 0;
}

uint16_t scene_pack_current(void) {
    return s_current;
}

uint16_t scene_pack_set(uint16_t idx) {
    if (!s_open || s_pack.count == 0) return 0;
    s_current = (uint16_t)(idx % s_pack.count);
    return s_current;
}

uint16_t scene_pack_advance(int delta) {
    if (!s_open || s_pack.count == 0) return 0;
    int next = (int)s_current + delta;
    int n = (int)s_pack.count;
    /* C's % is sign-preserving; normalise to a positive index. */
    next = ((next % n) + n) % n;
    s_current = (uint16_t)next;
    return s_current;
}

bool scene_pack_blit_current(uint8_t *dst, size_t scene_w, size_t scene_h) {
    if (!s_open || !dst) return false;
    if (scene_w != s_pack.cell_w) {
        /* Destination width must match cell width — we're not
         * resampling, just row-copying. The current device geometry
         * (200) and the pack's cell_w (200) line up; bail otherwise
         * to surface mismatches loudly. */
        ESP_LOGE(TAG, "scene_pack_blit_current: dst width %u != cell %u",
            (unsigned)scene_w, (unsigned)s_pack.cell_w);
        return false;
    }
    const uint8_t *ink = mpk_ink(&s_pack, s_current);
    size_t row_bytes = (s_pack.cell_w + 7) / 8;
    size_t copy_h = scene_h < s_pack.cell_h ? scene_h : s_pack.cell_h;
    /* Cells are stored row-major; we just memcpy the top copy_h rows
     * of the ink plane straight into dst. The mask plane is unused
     * for backgrounds — rendering a scene as opaque ink-on-paper
     * matches the existing scene-fetch path's monoplane semantics. */
    memcpy(dst, ink, row_bytes * copy_h);
    return true;
}

bool scene_pack_zone_at(int16_t x, int16_t y, const char **out_name) {
    if (!s_open) return false;
    return mpk_zone_test(SCENES_A_ZONES, SCENES_A_ZONES_COUNT,
                         s_current, x, y, out_name) >= 0;
}

bool scene_pack_current_has_zones(void) {
    if (!s_open) return false;
    /* Linear scan — zone-set table is tiny (~4 entries). */
    for (size_t i = 0; i < SCENES_A_ZONES_COUNT; i++) {
        if (SCENES_A_ZONES[i].sprite_idx == s_current &&
            SCENES_A_ZONES[i].count > 0) {
            return true;
        }
    }
    return false;
}

const mpk_zone_t *scene_pack_current_zones(uint8_t *out_count) {
    if (out_count) *out_count = 0;
    if (!s_open) return NULL;
    for (size_t i = 0; i < SCENES_A_ZONES_COUNT; i++) {
        if (SCENES_A_ZONES[i].sprite_idx == s_current) {
            if (out_count) *out_count = SCENES_A_ZONES[i].count;
            return SCENES_A_ZONES[i].items;
        }
    }
    return NULL;
}
