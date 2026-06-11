#include "fetch_worker.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "pack_cache.h"
#include "sprite_cache.h"
#include "sprite_fetch.h"
#include "pet_sync.h"
#include "device_diag.h"

static const char *TAG = "fetch_worker";

/* Request shapes. One queue, tagged union — the worker is strictly
 * serial, which is what we want: at most one TLS handshake at a time
 * competing with voice/imagine for mbedtls heap. */
typedef enum {
    REQ_ENTER_PLACE = 0,
    REQ_TRAVEL_PACK,
    REQ_REFRESH_PACK,
    REQ_WARM_PACK,
    REQ_PET_CELL,
    REQ_KEEPSAKE,
} req_kind_t;

typedef struct {
    req_kind_t kind;
    char place_id[40];   /* ENTER / TRAVEL / KEEPSAKE id           */
    char sheet[120];     /* packs + cells (costume sheets run long) */
    char expr[32];       /* PET_CELL                                */
    uint16_t cw, ch;
} req_t;

#define REQ_QUEUE_LEN     8
#define RESULT_QUEUE_LEN  4
#define WORKER_STACK      16384   /* TLS via esp_http_client, like net_worker */
#define WORKER_PRIO       4       /* peer of the pet_sync push worker */

static QueueHandle_t s_req_q;
static QueueHandle_t s_res_q;
static bool s_started;
static volatile bool s_cell_dirty;

/* Per-(sheet,expr) cell-fetch cooldown so a cell the server doesn't
 * have (or an offline stretch) can't be re-enqueued by every render.
 * Touched only from the main task (the sole enqueuer), so no lock. */
#define CELL_COOLDOWN_US   (60LL * 1000 * 1000)
#define CELL_RECENT_SLOTS  4
static struct {
    char key[152];
    int64_t at_us;
} s_cell_recent[CELL_RECENT_SLOTS];

static bool enqueue(const req_t *r) {
    if (!s_started || !s_req_q) return false;
    if (xQueueSend(s_req_q, r, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping req kind=%d", (int)r->kind);
        return false;
    }
    return true;
}

static void post_result(fetch_result_kind_t kind, bool ok,
                        const char *place_id, const char *sheet) {
    fetch_result_t res = {};
    res.kind = kind;
    res.ok = ok;
    snprintf(res.place_id, sizeof(res.place_id), "%s",
             place_id ? place_id : "");
    snprintf(res.sheet, sizeof(res.sheet), "%s", sheet ? sheet : "");
    /* The result queue only backs up if the main loop is wedged; in
     * that case dropping the oldest keeps the newest travel truthful. */
    if (xQueueSend(s_res_q, &res, 0) != pdTRUE) {
        fetch_result_t scrap;
        xQueueReceive(s_res_q, &scrap, 0);
        xQueueSend(s_res_q, &res, 0);
        ESP_LOGW(TAG, "result queue full — dropped oldest");
    }
}

static void do_pet_cell(const req_t *r) {
    const size_t bytes = ((size_t)r->cw / 8) * r->ch;
    uint8_t *ink  = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    uint8_t *mask = (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!ink || !mask) {
        free(ink);
        free(mask);
        ESP_LOGW(TAG, "cell '%s/%s' alloc failed", r->sheet, r->expr);
        return;
    }
    char url[224];
    snprintf(url, sizeof(url),
        "https://mochi.val.run/devsprite/cell/%s/%s", r->sheet, r->expr);
    uint16_t w = 0, h = 0;
    uint32_t ms = 0;
    bool ok = sprite_fetch_cell(url, ink, mask, bytes, &w, &h, &ms) &&
              w == r->cw && h == r->ch;
    if (ok) {
        char ink_s[48], mask_s[48];
        snprintf(ink_s,  sizeof(ink_s),  "%s_cell_ink",  r->expr);
        snprintf(mask_s, sizeof(mask_s), "%s_cell_mask", r->expr);
        ok = sprite_cache::store(r->sheet, ink_s,  ink,  bytes) &&
             sprite_cache::store(r->sheet, mask_s, mask, bytes);
        if (ok) {
            s_cell_dirty = true;
            ESP_LOGI(TAG, "cell '%s/%s' fetched + cached (%u ms)",
                r->sheet, r->expr, (unsigned)ms);
        }
    }
    if (!ok) {
        /* The render already showed its fallback; this is diagnostics,
         * not user feedback (design/25 C6 resolves by construction —
         * the tap visibly acted via the fallback cell). */
        ESP_LOGW(TAG, "cell '%s/%s' fetch failed", r->sheet, r->expr);
        device_diag_eventf(DIAG_WARN, "render", NULL,
            "cell fetch fail %s/%s", r->sheet, r->expr);
    }
    free(ink);
    free(mask);
}

static void worker(void *) {
    req_t r;
    while (true) {
        if (xQueueReceive(s_req_q, &r, portMAX_DELAY) != pdTRUE) continue;
        switch (r.kind) {
            case REQ_ENTER_PLACE: {
                bool ok = pet_sync_enter_place(r.place_id);
                post_result(FETCH_RESULT_ENTER, ok, r.place_id, "");
                break;
            }
            case REQ_TRAVEL_PACK: {
                bool ok = pack_cache_prefetch_geom(r.sheet, r.cw, r.ch);
                post_result(FETCH_RESULT_TRAVEL_PACK, ok, r.place_id, r.sheet);
                break;
            }
            case REQ_REFRESH_PACK: {
                bool changed = false;
                if (pack_cache_prefetch_geom_ex(r.sheet, r.cw, r.ch,
                                                &changed) && changed) {
                    post_result(FETCH_RESULT_REFRESH_PACK, true, "", r.sheet);
                }
                break;   /* unchanged / offline / failure: silent */
            }
            case REQ_WARM_PACK:
                pack_cache_prefetch_geom(r.sheet, r.cw, r.ch);
                break;
            case REQ_PET_CELL:
                do_pet_cell(&r);
                break;
            case REQ_KEEPSAKE:
                if (!pet_sync_collect_keepsake(r.place_id)) {
                    ESP_LOGW(TAG, "keepsake mirror '%s' failed (NVS holds it)",
                        r.place_id);
                }
                break;
        }
    }
}

bool fetch_worker_init(void) {
    if (s_started) return true;
    s_req_q = xQueueCreate(REQ_QUEUE_LEN, sizeof(req_t));
    s_res_q = xQueueCreate(RESULT_QUEUE_LEN, sizeof(fetch_result_t));
    if (!s_req_q || !s_res_q) {
        ESP_LOGE(TAG, "queue create failed");
        return false;
    }
    if (xTaskCreate(worker, "fetch_worker", WORKER_STACK, nullptr,
                    WORKER_PRIO, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return false;
    }
    s_started = true;
    ESP_LOGI(TAG, "started");
    return true;
}

bool fetch_worker_enter_place(const char *place_id) {
    if (!place_id || !place_id[0]) return false;
    req_t r = {};
    r.kind = REQ_ENTER_PLACE;
    snprintf(r.place_id, sizeof(r.place_id), "%s", place_id);
    return enqueue(&r);
}

bool fetch_worker_fetch_travel_pack(const char *place_id, const char *sheet,
                                    uint16_t cw, uint16_t ch) {
    if (!sheet || !sheet[0]) return false;
    req_t r = {};
    r.kind = REQ_TRAVEL_PACK;
    snprintf(r.place_id, sizeof(r.place_id), "%s", place_id ? place_id : "");
    snprintf(r.sheet, sizeof(r.sheet), "%s", sheet);
    r.cw = cw;
    r.ch = ch;
    return enqueue(&r);
}

bool fetch_worker_refresh_pack(const char *sheet, uint16_t cw, uint16_t ch) {
    if (!sheet || !sheet[0]) return false;
    req_t r = {};
    r.kind = REQ_REFRESH_PACK;
    snprintf(r.sheet, sizeof(r.sheet), "%s", sheet);
    r.cw = cw;
    r.ch = ch;
    return enqueue(&r);
}

bool fetch_worker_warm_pack(const char *sheet, uint16_t cw, uint16_t ch) {
    if (!sheet || !sheet[0]) return false;
    req_t r = {};
    r.kind = REQ_WARM_PACK;
    snprintf(r.sheet, sizeof(r.sheet), "%s", sheet);
    r.cw = cw;
    r.ch = ch;
    return enqueue(&r);
}

bool fetch_worker_fetch_cell(const char *sheet, const char *expr,
                             uint16_t cw, uint16_t ch) {
    if (!sheet || !sheet[0] || !expr || !expr[0]) return false;

    char key[152];
    snprintf(key, sizeof(key), "%s/%s", sheet, expr);
    const int64_t now = esp_timer_get_time();
    int slot = 0;
    for (int i = 0; i < CELL_RECENT_SLOTS; i++) {
        if (strcmp(s_cell_recent[i].key, key) == 0) {
            if (now - s_cell_recent[i].at_us < CELL_COOLDOWN_US) {
                return false;   /* already asked moments ago */
            }
            slot = i;
            break;
        }
        if (s_cell_recent[i].at_us < s_cell_recent[slot].at_us) slot = i;
    }

    req_t r = {};
    r.kind = REQ_PET_CELL;
    snprintf(r.sheet, sizeof(r.sheet), "%s", sheet);
    snprintf(r.expr, sizeof(r.expr), "%s", expr);
    r.cw = cw;
    r.ch = ch;
    if (!enqueue(&r)) return false;
    snprintf(s_cell_recent[slot].key, sizeof(s_cell_recent[slot].key),
             "%s", key);
    s_cell_recent[slot].at_us = now;
    return true;
}

bool fetch_worker_collect_keepsake(const char *keepsake_id) {
    if (!keepsake_id || !keepsake_id[0]) return false;
    req_t r = {};
    r.kind = REQ_KEEPSAKE;
    snprintf(r.place_id, sizeof(r.place_id), "%s", keepsake_id);
    return enqueue(&r);
}

bool fetch_worker_take_result(fetch_result_t *out) {
    if (!s_started || !s_res_q || !out) return false;
    return xQueueReceive(s_res_q, out, 0) == pdTRUE;
}

bool fetch_worker_take_cell_dirty(void) {
    bool v = s_cell_dirty;
    s_cell_dirty = false;
    return v;
}
