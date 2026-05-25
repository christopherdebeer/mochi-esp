#include "scene_pack.h"

#include <string.h>

#include "esp_log.h"

#include "mochi_pack.h"
#include "pack_cache.h"
#include "pet_state.h"      /* event_kind_t */
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

/* The "home" bundle bytes resolved at init (server-synced or embedded).
 * Kept so scene_pack_load_home() can restore the bundle after a travel
 * swapped the active pack to a place (design/17). */
static const uint8_t *s_home_bytes;

/* True when the active pack IS the embedded scenes_a bundle, so the
 * format=0 SCENES_A_ZONES meta table applies. False after a travel swap
 * to a foreign pack (a place) — that pack's zones, if any, are inline
 * (format=1); its format=0 cells have NO meta zones, so we must not
 * match the bundle's zone table against its indices (design/17). Set by
 * init / load_home (bundle) and cleared by load_bytes (foreign). */
static bool      s_is_bundle;

/* Open `bytes` into the active s_pack and seed the home/index state.
 * Used by both init paths so the validation + logging stays in one
 * place. */
static bool open_into_active(const uint8_t *bytes, const char *src) {
    int rc = mpk_open(bytes, &s_pack);
    if (rc != 0) {
        ESP_LOGE(TAG, "mpk_open(%s) rc=%d", src, rc);
        return false;
    }
    s_home_bytes = bytes;
    s_is_bundle  = true;
    if (s_pack.count != SCENES_A_COUNT) {
        ESP_LOGW(TAG, "pack count %u != meta count %u — meta header"
                      " out of sync with .mpk binary",
            (unsigned)s_pack.count, (unsigned)SCENES_A_COUNT);
    }
    ESP_LOGI(TAG, "scene_pack: %ux%u %u cells (mask=%d, src=%s) ready",
        (unsigned)s_pack.cell_w, (unsigned)s_pack.cell_h,
        (unsigned)s_pack.count, (int)s_pack.has_mask, src);
    s_open    = true;
    s_current = 0;
    return true;
}

bool scene_pack_init_embedded(void) {
    if (s_open) return true;
    return open_into_active(_binary_scenes_a_mpk_start, "embedded");
}

bool scene_pack_init(void) {
    /* "Sync at boot" path: pack_cache_active hits the network. Idempotent
     * once WiFi is up — re-running after a prior embedded init upgrades
     * the active bytes to the server-authored pack. Pre-WiFi callers
     * must use scene_pack_init_embedded(). */
    const uint8_t *bytes =
        pack_cache_active("scene-bundle-a", _binary_scenes_a_mpk_start);
    return open_into_active(bytes, "synced");
}

bool scene_pack_load_home(void) {
    if (!s_home_bytes) return false;
    mpk_t pack;
    int rc = mpk_open(s_home_bytes, &pack);
    if (rc != 0) {
        ESP_LOGE(TAG, "scene_pack_load_home: mpk_open rc=%d", rc);
        return false;
    }
    s_pack      = pack;
    s_open      = true;
    s_current   = 0;
    s_is_bundle = true;
    ESP_LOGI(TAG, "scene_pack: restored home bundle (%u cells)",
        (unsigned)s_pack.count);
    return true;
}

bool scene_pack_load_bytes(const uint8_t *mpk) {
    if (!mpk) return false;
    mpk_t pack;
    int rc = mpk_open(mpk, &pack);
    if (rc != 0) {
        ESP_LOGE(TAG, "scene_pack_load_bytes: mpk_open rc=%d", rc);
        return false;
    }
    s_pack      = pack;
    s_open      = true;
    s_current   = 0;
    s_is_bundle = false;   /* foreign pack — SCENES_A_ZONES meta no longer applies */
    ESP_LOGI(TAG, "scene_pack: swapped to %ux%u %u cells (fmt=%u) — imagined",
        (unsigned)s_pack.cell_w, (unsigned)s_pack.cell_h,
        (unsigned)s_pack.count, (unsigned)s_pack.format);
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
    if (s_pack.format == 1) {
        /* format=1 zones are inline; the name lookup callers want
         * doesn't apply here (the wire format carries an action,
         * not a name string). Use scene_pack_action_at for fmt 1. */
        return false;
    }
    if (!s_is_bundle) return false;   /* meta zones apply to the bundle only */
    return mpk_zone_test(SCENES_A_ZONES, SCENES_A_ZONES_COUNT,
                         s_current, x, y, out_name) >= 0;
}

bool scene_pack_current_has_zones(void) {
    if (!s_open) return false;
    if (s_pack.format == 1) {
        return mpk_zone_count(&s_pack, s_current) > 0;
    }
    if (!s_is_bundle) return false;   /* a travelled-to format=0 place has no meta zones */
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
    if (!s_open || s_pack.format == 1 || !s_is_bundle) return NULL;
    for (size_t i = 0; i < SCENES_A_ZONES_COUNT; i++) {
        if (SCENES_A_ZONES[i].sprite_idx == s_current) {
            if (out_count) *out_count = SCENES_A_ZONES[i].count;
            return SCENES_A_ZONES[i].items;
        }
    }
    return NULL;
}

/* Chebyshev distance from (x,y) to a rect — 0 inside, else max
 * per-axis gap. Cheap, intuitive on a square panel. Used by the
 * forgiving snap-to-nearest path. */
static int chebyshev_to_rect(int16_t x, int16_t y,
                             int rx, int ry, int rw, int rh) {
    int dx = 0, dy = 0;
    if      (x < rx)              dx = rx - x;
    else if (x >= rx + rw)        dx = x - (rx + rw - 1);
    if      (y < ry)              dy = ry - y;
    else if (y >= ry + rh)        dy = y - (ry + rh - 1);
    return dx > dy ? dx : dy;
}

bool scene_pack_zone_near(int16_t x, int16_t y, int slop_px,
                          const char **out_name) {
    if (!s_open || s_pack.format == 1) return false;
    uint8_t n = 0;
    const mpk_zone_t *zones = scene_pack_current_zones(&n);
    if (!zones || n == 0) return false;

    int best_d = -1;
    const mpk_zone_t *best = NULL;
    for (uint8_t i = 0; i < n; i++) {
        const mpk_zone_t *r = &zones[i];
        int d = chebyshev_to_rect(x, y, r->x, r->y, r->w, r->h);
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best   = r;
            if (d == 0) break;  /* direct hit; can't beat zero */
        }
    }
    if (!best || best_d > slop_px) return false;
    if (out_name) *out_name = best->name;
    return true;
}

/* Translate a format=0 zone name (the strings in scenes_a_meta.h)
 * into the typed action the firmware used to compute via strcmp in
 * main.cpp's touch handler. Putting the table here keeps the
 * dispatch shape uniform with format=1 — main.cpp only sees
 * scene_pack_action_t regardless of pack format.
 *
 * Names not in the table fall through to event/EVENT_TAPPED so the
 * tap is at least observable in the event log. */
static void name_to_action(const char *name, scene_pack_action_t *out) {
    out->name      = name;
    out->seed_text = NULL;
    out->seed_len  = 0;
    if (!name) {
        out->kind = MPK_ACTION_EVENT;
        out->data = EVENT_TAPPED;
        return;
    }
    if (strcmp(name, "door") == 0 || strcmp(name, "stairs") == 0) {
        out->kind = MPK_ACTION_NAV_RELATIVE;
        out->data = +1;
    } else if (strcmp(name, "back") == 0) {
        out->kind = MPK_ACTION_NAV_RELATIVE;
        out->data = -1;
    } else if (strcmp(name, "food") == 0 || strcmp(name, "bowl") == 0) {
        out->kind = MPK_ACTION_EVENT;
        out->data = EVENT_FED;
    } else if (strcmp(name, "heart") == 0) {
        out->kind = MPK_ACTION_EVENT;
        out->data = EVENT_COMFORTED;
    } else if (strcmp(name, "ball") == 0) {
        out->kind = MPK_ACTION_EVENT;
        out->data = EVENT_PLAYED;
    } else if (strcmp(name, "ornament") == 0 ||
               strcmp(name, "light") == 0 ||
               strcmp(name, "star") == 0) {
        out->kind = MPK_ACTION_EVENT;
        out->data = EVENT_CHEERED;
    } else {
        out->kind = MPK_ACTION_EVENT;
        out->data = EVENT_TAPPED;
    }
}

bool scene_pack_action_at(int16_t x, int16_t y, int slop_px,
                          scene_pack_action_t *out) {
    if (!s_open || !out) return false;
    memset(out, 0, sizeof(*out));

    if (s_pack.format == 1) {
        /* Direct hit-test over inline zones. */
        mpk_zone_v1_t z;
        int hit = mpk_zone_hit(&s_pack, s_current, x, y, &z);
        if (hit < 0) {
            /* Snap-to-nearest fallback. Same Chebyshev rule as the
             * format=0 path so behaviour matches across formats. */
            uint8_t zc = mpk_zone_count(&s_pack, s_current);
            int best_d = -1;
            for (uint8_t i = 0; i < zc; i++) {
                mpk_zone_v1_t cand;
                if (!mpk_zone_get(&s_pack, s_current, i, &cand)) continue;
                int d = chebyshev_to_rect(x, y, cand.x, cand.y, cand.w, cand.h);
                if (best_d < 0 || d < best_d) {
                    best_d = d;
                    z = cand;
                    if (d == 0) break;
                }
            }
            if (best_d < 0 || best_d > slop_px) return false;
        }
        out->kind      = (int)z.kind;
        out->data      = z.data;
        out->seed_text = z.seed_text;
        out->seed_len  = z.seed_len;
        return true;
    }

    /* format=0: name-based lookup with the same forgiving snap. */
    const char *name = NULL;
    if (!scene_pack_zone_at(x, y, &name) &&
        !scene_pack_zone_near(x, y, slop_px, &name)) {
        return false;
    }
    name_to_action(name, out);
    return true;
}
