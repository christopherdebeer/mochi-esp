#include "keepsakes.h"

#include <string.h>

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "keepsakes";

#define KS_NS  "mochi-keep"
#define KS_KEY "mask"

// Mirrors the substrate registry (shared/keepsakes.ts). ids MUST match — they
// are the payload of an MPK_ACTION_COLLECT zone. Names are short for the
// e-ink backpack. Index = bit position in the NVS mask, so APPEND only
// (re-ordering would re-map already-collected bits on shipped devices).
typedef struct { const char *id; const char *name; } ks_def_t;
static const ks_def_t DEFS[] = {
    { "forest-charm",   "woven charm" },
    { "village-cup",    "clay cup" },
    { "treetops-bell",  "copper bell" },
    { "home-bloom",     "pressed flower" },
    { "spaceship-star", "glass star" },
};
#define KS_N ((int)(sizeof(DEFS) / sizeof(DEFS[0])))

static bool     s_loaded;
static uint32_t s_mask;

static void ks_load(void) {
    if (s_loaded) return;
    s_loaded = true;
    nvs_handle_t h;
    if (nvs_open(KS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint32_t v = 0;
        if (nvs_get_u32(h, KS_KEY, &v) == ESP_OK) s_mask = v;
        nvs_close(h);
    }
}

static void ks_save(void) {
    nvs_handle_t h;
    if (nvs_open(KS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, KS_KEY, s_mask);
        nvs_commit(h);
        nvs_close(h);
    } else {
        ESP_LOGW(TAG, "nvs_open(%s) for save failed", KS_NS);
    }
}

int keepsakes_total(void) { return KS_N; }

const char *keepsakes_id(int idx) {
    return (idx >= 0 && idx < KS_N) ? DEFS[idx].id : NULL;
}

const char *keepsakes_name(int idx) {
    return (idx >= 0 && idx < KS_N) ? DEFS[idx].name : NULL;
}

int keepsakes_index(const char *id) {
    if (!id) return -1;
    for (int i = 0; i < KS_N; i++) {
        if (strcmp(DEFS[i].id, id) == 0) return i;
    }
    return -1;
}

bool keepsakes_have(int idx) {
    if (idx < 0 || idx >= KS_N) return false;
    ks_load();
    return (s_mask >> idx) & 1u;
}

bool keepsakes_add(int idx) {
    if (idx < 0 || idx >= KS_N) return false;
    ks_load();
    if ((s_mask >> idx) & 1u) return false;  // already had it
    s_mask |= (1u << idx);
    ks_save();
    ESP_LOGI(TAG, "collected %s (mask=0x%02x)", DEFS[idx].id, (unsigned)s_mask);
    return true;
}

int keepsakes_count_collected(void) {
    ks_load();
    int c = 0;
    for (int i = 0; i < KS_N; i++) if ((s_mask >> i) & 1u) c++;
    return c;
}

uint32_t keepsakes_mask(void) { ks_load(); return s_mask; }

void keepsakes_merge_mask(uint32_t mask) {
    ks_load();
    const uint32_t known = (KS_N >= 32) ? 0xFFFFFFFFu : ((1u << KS_N) - 1u);
    const uint32_t merged = s_mask | (mask & known);
    if (merged != s_mask) {
        s_mask = merged;
        ks_save();
        ESP_LOGI(TAG, "merged server mask -> 0x%02x", (unsigned)s_mask);
    }
}
