/*
 * imagine — on-device scene generation pipeline.
 *
 * v0 scaffolding. The worker advances through phases but each
 * network step is a stub that logs and sleeps; later commits flesh
 * each one out (see design/16-on-device-imagine.md "v0 scope" for
 * the order). Wiring this up first lets us exercise the voice-tool
 * trigger and the UI hook (thought-bubble text driven off
 * imagine_phase()) end-to-end before the heavy network plumbing.
 */

#include "imagine.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"

static const char *TAG = "imagine";

/* Per-phase debounce target: don't kick off a new imagine within
 * this window of the last attempt. Protects against the model
 * looping the tool. The cap also lives server-side (see
 * /api/places/queue's day_cap) — this is the cheap local guard. */
#define IMAGINE_DEBOUNCE_US (60LL * 1000 * 1000)

#define WORKER_STACK_BYTES 6144
#define WORKER_QUEUE_DEPTH 1   /* one in-flight imagine at a time */

static QueueHandle_t s_queue;
static atomic_int    s_phase;          /* imagine_phase_t */
static atomic_bool   s_in_flight;
static atomic_bool   s_running;
static int64_t       s_last_attempt_us;

/* Result + reason buffers — borrowed by callers via the public
 * accessors. Worker writes; main thread reads. The atomic_int phase
 * is the synchronisation point: only consult these strings when
 * phase reads DONE / FAILED. */
static char          s_place_id[40];
static char          s_pack_path[64];
static char          s_reason[120];

static imagine_req_t *clone_req(const imagine_req_t *src) {
    imagine_req_t *dst = (imagine_req_t *)heap_caps_malloc(
        sizeof(*dst), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!dst) return NULL;
    memcpy(dst, src, sizeof(*dst));
    return dst;
}

static void set_phase(imagine_phase_t p) {
    atomic_store(&s_phase, (int)p);
    ESP_LOGI(TAG, "phase → %d", (int)p);
}

/* Fail-out helper — used by phase implementations as they land in
 * follow-up commits. Marked unused so the v0 scaffold (which never
 * fails — every phase is a sleep) compiles clean. */
__attribute__((unused))
static void fail_reason(const char *reason) {
    snprintf(s_reason, sizeof(s_reason), "%s", reason ? reason : "unknown");
    set_phase(IMAGINE_FAILED);
    atomic_store(&s_in_flight, false);
}

/* esp_random returns 0 only before the bootloader's RNG seeded; we
 * don't need cryptographic randomness here — only "varies across
 * boots" — and the timer is fine as a fallback. */
static uint32_t random_id_seed(void) {
    uint32_t r = esp_random();
    if (r == 0) r = (uint32_t)esp_timer_get_time();
    return r;
}

static void worker_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "worker_task started");
    while (atomic_load(&s_running)) {
        imagine_req_t *req = NULL;
        if (xQueueReceive(s_queue, &req, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (!req) continue;

        s_reason[0] = '\0';
        s_place_id[0] = '\0';
        s_pack_path[0] = '\0';
        ESP_LOGI(TAG, "imagine: name='%s' from='%s' vibe.len=%u",
            req->seed_name, req->from_place_id,
            (unsigned)strlen(req->seed_vibe));

        /* Phase 1 — queue. POST /api/places/queue, capture place_id.
         *
         * Stubbed: pretend the server gave us a deterministic id
         * derived from the seed name so we can wire the UI without
         * the network. Real call lands in a follow-up.  */
        set_phase(IMAGINE_QUEUEING);
        snprintf(s_place_id, sizeof(s_place_id),
            "place_%08lx", (unsigned long)random_id_seed());
        ESP_LOGI(TAG, "stub: queued as %s", s_place_id);
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Phase 2 — fetch guide.png. New endpoint required server-
         * side: GET /api/places/:id/guide.png. */
        set_phase(IMAGINE_FETCHING_GUIDE);
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Phase 3 — fetch exemplar. GET /sheets/:exemplar/source.png
         * (already public). The exemplar id comes from the queue
         * response. */
        set_phase(IMAGINE_FETCHING_EXEMPLAR);
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Phase 4 — generate. Multipart POST to OpenAI. The user's
         * BYO key is loaded from openai_key.cpp. ~25 s wall, ~600 KB
         * b64 response in PSRAM. The most consequential step; the
         * other phases are plumbing. */
        set_phase(IMAGINE_GENERATING);
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP_LOGW(TAG, "stub: skipping real OpenAI call");

        /* Phase 5 — upload to val.run. POST /sheets/:id/png with
         * Content-Type: image/png. */
        set_phase(IMAGINE_UPLOADING);
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Phase 6 — flip the place to ready so val.run runs
         * extraction on the new sheet. */
        set_phase(IMAGINE_FINALISING);
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Phase 7 — fetch the assembled .mpk.  */
        set_phase(IMAGINE_FETCHING_PACK);
        vTaskDelay(pdMS_TO_TICKS(500));

        /* Phase 8 — persist + swap into scene_pack. */
        set_phase(IMAGINE_SWAPPING);
        snprintf(s_pack_path, sizeof(s_pack_path),
            "/littlefs/imagined/%s.mpk", s_place_id);
        ESP_LOGI(TAG, "stub: would persist to %s", s_pack_path);

        set_phase(IMAGINE_DONE);
        atomic_store(&s_in_flight, false);

        free(req);
    }
    vTaskDelete(NULL);
}

bool imagine_init(void) {
    if (s_queue) return true;
    s_queue = xQueueCreate(WORKER_QUEUE_DEPTH, sizeof(imagine_req_t *));
    if (!s_queue) {
        ESP_LOGE(TAG, "queue alloc failed");
        return false;
    }
    atomic_store(&s_phase, IMAGINE_IDLE);
    atomic_store(&s_in_flight, false);
    atomic_store(&s_running, true);
    BaseType_t ok = xTaskCreate(
        worker_task, "imagine", WORKER_STACK_BYTES, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "worker_task create failed");
        return false;
    }
    return true;
}

bool imagine_start(const imagine_req_t *req) {
    if (!req || !s_queue) return false;
    if (atomic_load(&s_in_flight)) {
        ESP_LOGW(TAG, "refused: already in flight");
        return false;
    }
    int64_t now_us = esp_timer_get_time();
    if (s_last_attempt_us != 0 &&
        (now_us - s_last_attempt_us) < IMAGINE_DEBOUNCE_US) {
        ESP_LOGW(TAG, "refused: debounce window");
        return false;
    }
    imagine_req_t *cloned = clone_req(req);
    if (!cloned) return false;
    if (xQueueSend(s_queue, &cloned, 0) != pdTRUE) {
        free(cloned);
        return false;
    }
    s_last_attempt_us = now_us;
    atomic_store(&s_in_flight, true);
    return true;
}

bool imagine_in_flight(void) {
    return atomic_load(&s_in_flight);
}

imagine_phase_t imagine_phase(void) {
    return (imagine_phase_t)atomic_load(&s_phase);
}

const char *imagine_place_id(void) {
    return s_place_id;
}

const char *imagine_last_pack_path(void) {
    return s_pack_path;
}

const char *imagine_last_reason(void) {
    return s_reason;
}
