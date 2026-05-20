#include "pet_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "cJSON.h"

extern "C" {
#include "voice/voice_https.h"
#include "mood.h"
}
#include "pair_creds.h"

static const char *TAG = "pet_sync";

#define STATE_URL   "https://mochi.val.run/api/state"
#define MUTATE_URL  "https://mochi.val.run/api/mutate"

#define PUSH_QUEUE_LEN     32
#define WORKER_STACK_BYTES (8 * 1024)
#define WORKER_PRIO        4

/* Periodic resync — every 5 min when the device is otherwise quiet.
 * Catches server-side events the device didn't initiate (web app
 * taps, voice tools, drift, transient expirations). */
#define RESYNC_INTERVAL_MS (5 * 60 * 1000)

/* Authoritative snapshot. Mutated by pull/push response handlers,
 * read by everyone else through pet_sync_get_snapshot. */
static SemaphoreHandle_t s_mtx;
static pet_t s_pet;
static bool  s_have_snapshot;

/* Push queue + worker. */
typedef struct {
    event_kind_t kind;
    int64_t      at_ms;
} push_msg_t;
static QueueHandle_t s_queue;
static bool          s_started;

/* ─── parsing ─────────────────────────────────────────────────── */

static int64_t json_int64(cJSON *parent, const char *key, int64_t fallback) {
    cJSON *n = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsNumber(n)) return fallback;
    /* cJSON stores numbers as double; safe for ms-since-epoch up to
     * 2^53 ≈ year 287396 AD. */
    return (int64_t)n->valuedouble;
}

static uint8_t json_u8(cJSON *parent, const char *key, uint8_t fallback) {
    cJSON *n = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (!cJSON_IsNumber(n)) return fallback;
    int v = (int)n->valuedouble;
    if (v < 0)   return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}

static bool json_bool(cJSON *parent, const char *key, bool fallback) {
    cJSON *n = cJSON_GetObjectItemCaseSensitive(parent, key);
    if (cJSON_IsBool(n)) return cJSON_IsTrue(n);
    return fallback;
}

static bool parse_state_response(const char *body, int len,
                                 pet_t *out_pet,
                                 pet_event_t *out_events, size_t cap,
                                 size_t *out_count) {
    cJSON *root = cJSON_ParseWithLength(body, (size_t)len);
    if (!root) {
        ESP_LOGW(TAG, "json parse failed");
        return false;
    }

    cJSON *pet = cJSON_GetObjectItemCaseSensitive(root, "pet");
    if (!cJSON_IsObject(pet)) {
        ESP_LOGW(TAG, "no `pet` in response");
        cJSON_Delete(root);
        return false;
    }

    out_pet->born_at             = json_int64(pet, "bornAt", 0);
    out_pet->stats_at            = json_int64(pet, "statsAt", 0);
    out_pet->last_interaction_at = json_int64(pet, "lastInteractionAt", 0);
    out_pet->asleep              = json_bool(pet, "asleep", false);

    cJSON *stats = cJSON_GetObjectItemCaseSensitive(pet, "stats");
    if (cJSON_IsObject(stats)) {
        out_pet->stats.happiness = json_u8(stats, "happiness", 50);
        out_pet->stats.fullness  = json_u8(stats, "fullness",  50);
        out_pet->stats.energy    = json_u8(stats, "energy",    50);
    } else {
        out_pet->stats = (pet_stats_t){50, 50, 50};
    }

    /* Transient mood: { sprite, until }. Ignored when missing. */
    out_pet->transient.sprite = SPRITE_NONE;
    out_pet->transient.until  = 0;
    cJSON *trans = cJSON_GetObjectItemCaseSensitive(pet, "transient");
    if (cJSON_IsObject(trans)) {
        cJSON *jspr = cJSON_GetObjectItemCaseSensitive(trans, "sprite");
        if (cJSON_IsString(jspr)) {
            /* Walk the sprite-name table for a match. mood.c has
             * sprite_to_name; we need the inverse here. Linear scan
             * is fine — table has ~17 entries. */
            for (int i = 0; i < SPRITE_COUNT; i++) {
                const char *n = sprite_to_name((sprite_key_t)i);
                if (n && strcmp(n, jspr->valuestring) == 0) {
                    out_pet->transient.sprite = (sprite_key_t)i;
                    break;
                }
            }
        }
        out_pet->transient.until = json_int64(trans, "until", 0);
    }

    /* Recent events slice. */
    *out_count = 0;
    cJSON *events = cJSON_GetObjectItemCaseSensitive(root, "events");
    if (cJSON_IsArray(events) && out_events && cap > 0) {
        cJSON *e = NULL;
        cJSON_ArrayForEach(e, events) {
            if (*out_count >= cap) break;
            cJSON *jk = cJSON_GetObjectItemCaseSensitive(e, "kind");
            cJSON *ja = cJSON_GetObjectItemCaseSensitive(e, "at");
            if (!cJSON_IsString(jk) || !cJSON_IsNumber(ja)) continue;
            event_kind_t k = event_kind_from_name(jk->valuestring);
            if (k == EVENT_NONE) continue;
            out_events[*out_count].kind = k;
            out_events[*out_count].at   = (int64_t)ja->valuedouble;
            (*out_count)++;
        }
    }

    cJSON_Delete(root);
    return true;
}

/* ─── pull ────────────────────────────────────────────────────── */

typedef struct {
    char  *body;
    int    len;
} body_capture_t;

static void capture_body(http_resp_t *resp, void *ctx) {
    body_capture_t *cap = (body_capture_t *)ctx;
    if (!resp || !resp->data || resp->size <= 0) return;
    cap->body = (char *)malloc((size_t)resp->size + 1);
    if (!cap->body) return;
    memcpy(cap->body, resp->data, (size_t)resp->size);
    cap->body[resp->size] = '\0';
    cap->len = resp->size;
}

static bool do_state_pull(pet_t *out_pet,
                          pet_event_t *out_events, size_t cap,
                          size_t *out_count) {
    struct mochi_pair_creds creds;
    if (!pair_creds_load(&creds) || !creds.pet_id[0]) {
        ESP_LOGW(TAG, "no pet_id; skipping state pull");
        return false;
    }
    char hdr[96];
    snprintf(hdr, sizeof(hdr), "X-Pet-Id: %s", creds.pet_id);
    char *headers[] = { hdr, NULL };

    body_capture_t cap_state = {NULL, 0};
    char url[64];
    snprintf(url, sizeof(url), "%s", STATE_URL);
    int rc = https_get(url, headers, capture_body, &cap_state);
    if (rc != 0 || !cap_state.body) {
        ESP_LOGW(TAG, "state GET rc=%d", rc);
        free(cap_state.body);
        return false;
    }

    bool ok = parse_state_response(cap_state.body, cap_state.len,
                                   out_pet, out_events, cap, out_count);
    free(cap_state.body);
    return ok;
}

bool pet_sync_pull_now(pet_t *out_pet,
                      pet_event_t *out_events, size_t slice_cap,
                      size_t *out_count) {
    if (!out_pet || !out_count) return false;
    *out_count = 0;
    pet_t tmp;
    pet_event_t evs[12];
    size_t n = 0;
    if (!do_state_pull(&tmp, evs, sizeof(evs)/sizeof(evs[0]), &n)) {
        return false;
    }

    /* Update authoritative snapshot. */
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_pet = tmp;
    s_have_snapshot = true;
    xSemaphoreGive(s_mtx);

    *out_pet = tmp;
    if (out_events && slice_cap > 0) {
        size_t copy = n < slice_cap ? n : slice_cap;
        for (size_t i = 0; i < copy; i++) out_events[i] = evs[i];
        *out_count = copy;
    }
    ESP_LOGI(TAG,
        "pulled snapshot: stats=h%u/f%u/e%u asleep=%d born_at=%lld "
        "last_int=%lld n_events=%u",
        tmp.stats.happiness, tmp.stats.fullness, tmp.stats.energy,
        (int)tmp.asleep, (long long)tmp.born_at,
        (long long)tmp.last_interaction_at, (unsigned)n);
    return true;
}

bool pet_sync_get_snapshot(pet_t *out_pet) {
    if (!out_pet || !s_mtx) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool have = s_have_snapshot;
    if (have) *out_pet = s_pet;
    xSemaphoreGive(s_mtx);
    return have;
}

void pet_sync_touch(int64_t at_ms) {
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_have_snapshot) s_pet.last_interaction_at = at_ms;
    xSemaphoreGive(s_mtx);
}

/* ─── push ────────────────────────────────────────────────────── */

static bool do_mutate_post(event_kind_t kind, int64_t at_ms) {
    struct mochi_pair_creds creds;
    if (!pair_creds_load(&creds) || !creds.pet_id[0]) {
        return false;
    }
    const char *kind_name = event_kind_to_name(kind);
    if (!kind_name) return false;

    /* Compose body. {"kind": "<name>"} — small, fits stack. */
    char body[96];
    int blen = snprintf(body, sizeof(body), "{\"kind\":\"%s\"}", kind_name);
    if (blen <= 0 || (size_t)blen >= sizeof(body)) return false;

    char hdr_pet[96];
    snprintf(hdr_pet, sizeof(hdr_pet), "X-Pet-Id: %s", creds.pet_id);
    char hdr_ct[] = "Content-Type: application/json";
    char *headers[] = { hdr_pet, hdr_ct, NULL };

    body_capture_t cap_mut = {NULL, 0};
    char url[64];
    snprintf(url, sizeof(url), "%s", MUTATE_URL);
    int rc = https_post(url, headers, body, capture_body, &cap_mut);
    if (rc != 0 || !cap_mut.body) {
        ESP_LOGW(TAG, "mutate POST rc=%d kind=%s", rc, kind_name);
        free(cap_mut.body);
        return false;
    }

    /* Mutate's response shape == /api/state's shape; reuse the parser
     * to refresh the snapshot from the authoritative reply. */
    pet_t tmp;
    pet_event_t evs[12];
    size_t n = 0;
    bool ok = parse_state_response(cap_mut.body, cap_mut.len, &tmp,
                                   evs, sizeof(evs)/sizeof(evs[0]), &n);
    free(cap_mut.body);
    if (!ok) return false;

    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_pet = tmp;
    s_have_snapshot = true;
    xSemaphoreGive(s_mtx);
    (void)at_ms;  /* server stamps its own at; we don't need ours */
    ESP_LOGI(TAG,
        "mutate %s ok: stats=h%u/f%u/e%u",
        kind_name, tmp.stats.happiness, tmp.stats.fullness, tmp.stats.energy);
    return true;
}

static void push_worker(void *) {
    push_msg_t msg;
    int64_t last_resync_us = esp_timer_get_time();
    while (true) {
        /* Wait up to RESYNC_INTERVAL_MS for a queued mutate. If none,
         * the timeout fires and we do a periodic /api/state pull. */
        if (xQueueReceive(s_queue, &msg,
                pdMS_TO_TICKS(RESYNC_INTERVAL_MS)) == pdTRUE) {
            do_mutate_post(msg.kind, msg.at_ms);
            last_resync_us = esp_timer_get_time();
        } else {
            /* Idle resync — catches server-side drift the device
             * didn't initiate. */
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_resync_us
                    >= (int64_t)RESYNC_INTERVAL_MS * 1000) {
                ESP_LOGI(TAG, "periodic resync");
                pet_t tmp;
                pet_event_t evs[12];
                size_t n = 0;
                if (do_state_pull(&tmp, evs,
                        sizeof(evs)/sizeof(evs[0]), &n)) {
                    last_resync_us = now_us;
                }
            }
        }
    }
}

void pet_sync_start(void) {
    if (s_started) return;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(PUSH_QUEUE_LEN, sizeof(push_msg_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue create failed");
        return;
    }
    BaseType_t ok = xTaskCreate(push_worker, "pet_sync",
        WORKER_STACK_BYTES, NULL, WORKER_PRIO, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return;
    }
    s_started = true;
    ESP_LOGI(TAG, "push worker started");
}

bool pet_sync_enqueue(event_kind_t kind, int64_t at_ms) {
    if (!s_started || !s_queue) return false;
    push_msg_t msg = { kind, at_ms };
    return xQueueSend(s_queue, &msg, 0) == pdTRUE;
}
