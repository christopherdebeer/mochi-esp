/*
 * imagine — on-device scene generation pipeline (design/16).
 *
 * A voice tool call (imagine_place) enqueues a request; the worker
 * task drives the full flow with the user's BYO OpenAI key, which
 * never leaves the device:
 *
 *   1. queue        POST /api/places/queue            → placeId
 *   2. orchestration GET /api/places/:id/orchestration → prompt + urls
 *   3. fetch refs   GET guide.png + exemplar source.png
 *   4. generate     POST api.openai.com/v1/images/edits (multipart)
 *   5. upload       POST /sheets/:sheet/png (raw image/png)
 *   6. ready        POST /api/places/:id/ready
 *   7. fetch pack   GET /devsprite/pack/:sheet         → MPK1
 *   8. swap         scene_pack_load_bytes()            → re-render
 *
 * The server computes the prompt (versioned style preamble), the gen
 * size, and the exemplar — the firmware just follows the URLs the
 * orchestration step hands back. Only the generated PNG crosses to
 * mochi.val.run; the OpenAI key stays in NVS. See design/16.
 *
 * Concurrency: one imagine at a time (bounded PSRAM); refused while
 * another is in flight or inside the debounce window. The heavy
 * buffers live in PSRAM and are freed as each phase completes.
 */

#include "imagine.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "mbedtls/base64.h"
#include "cJSON.h"

#include "voice/voice_https.h"
#include "sprite_fetch.h"
#include "scene_pack.h"
#include "openai_key.h"
#include "pair_creds.h"

static const char *TAG = "imagine";

#define MOCHI_BASE "https://mochi.val.run"
#define OPENAI_EDITS_URL "https://api.openai.com/v1/images/edits"

/* Per-attempt debounce: don't kick a new imagine within this window of
 * the last attempt. Guards against the model looping the tool; the
 * server's day_cap is the durable cap, this is the cheap local one. */
#define IMAGINE_DEBOUNCE_US (60LL * 1000 * 1000)

#define WORKER_STACK_BYTES 8192
#define WORKER_QUEUE_DEPTH 1   /* one in-flight imagine at a time */

/* PSRAM working-set caps. Generous; a pack/place stays well under. */
#define GUIDE_MAX        (64u   * 1024)
#define EXEMPLAR_MAX     (512u  * 1024)
#define OPENAI_RESP_MAX  (1280u * 1024)   /* ~600-900 KB b64 + envelope */
#define PNG_MAX          (768u  * 1024)   /* decoded 1504x720 PNG       */
#define PACK_MAX         (320u  * 1024)

#define MP_BOUNDARY "----mochiimagineKLuaBOUNDARY"

static QueueHandle_t s_queue;
static atomic_int    s_phase;          /* imagine_phase_t */
static atomic_bool   s_in_flight;
static atomic_bool   s_running;
static int64_t       s_last_attempt_us;

/* Result + reason buffers — borrowed by callers via the accessors.
 * Only consult them when phase reads DONE / FAILED. */
static char          s_place_id[40];
static char          s_pack_path[96];
static char          s_reason[120];

/* The active imagined pack lives forever once swapped in (mpk pointers
 * reference it). Freed only when a new imagine replaces it. */
static uint8_t      *s_active_pack;

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

/* ─── small HTTP/JSON helpers (text bodies via voice_https) ───────── */

typedef struct { char *body; int len; } capture_t;

static void on_capture(http_resp_t *resp, void *ctx) {
    capture_t *c = (capture_t *)ctx;
    if (!resp || !resp->data || resp->size <= 0) return;
    c->body = (char *)malloc((size_t)resp->size + 1);
    if (!c->body) return;
    memcpy(c->body, resp->data, (size_t)resp->size);
    c->body[resp->size] = '\0';
    c->len = resp->size;
}

/* POST a JSON body with X-Pet-Id; returns the response body (malloc'd,
 * caller frees) or NULL. `rel` is a path under MOCHI_BASE. */
static char *api_post_json(const char *rel, const char *pet_id,
                           const char *json_body) {
    char url[160];
    snprintf(url, sizeof(url), "%s%s", MOCHI_BASE, rel);
    char hdr_pet[96];
    snprintf(hdr_pet, sizeof(hdr_pet), "X-Pet-Id: %s", pet_id);
    char hdr_ct[] = "Content-Type: application/json";
    char *headers[] = { hdr_pet, hdr_ct, NULL };
    capture_t cap = { NULL, 0 };
    int rc = https_post(url, headers, (char *)json_body, on_capture, &cap);
    if (rc != 0) {
        ESP_LOGW(TAG, "POST %s rc=%d", rel, rc);
        free(cap.body);
        return NULL;
    }
    return cap.body;
}

/* GET with X-Pet-Id; returns the response body (malloc'd) or NULL. */
static char *api_get(const char *rel, const char *pet_id) {
    char url[200];
    snprintf(url, sizeof(url), "%s%s", MOCHI_BASE, rel);
    char hdr_pet[96];
    snprintf(hdr_pet, sizeof(hdr_pet), "X-Pet-Id: %s", pet_id);
    char *headers[] = { hdr_pet, NULL };
    capture_t cap = { NULL, 0 };
    int rc = https_get(url, headers, on_capture, &cap);
    if (rc != 0) {
        ESP_LOGW(TAG, "GET %s rc=%d", rel, rc);
        free(cap.body);
        return NULL;
    }
    return cap.body;
}

static bool json_str(cJSON *o, const char *k, char *out, size_t cap) {
    cJSON *n = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!cJSON_IsString(n) || !n->valuestring) return false;
    snprintf(out, cap, "%s", n->valuestring);
    return true;
}

/* ─── OpenAI multipart edit ───────────────────────────────────────── */

typedef struct { uint8_t *buf; size_t cap; size_t len; bool overflow; } rcap_t;

static esp_err_t on_openai_data(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    rcap_t *r = (rcap_t *)evt->user_data;
    if (r->overflow) return ESP_OK;
    size_t want = (size_t)evt->data_len;
    if (r->len + want + 1 > r->cap) { r->overflow = true; return ESP_OK; }
    memcpy(r->buf + r->len, evt->data, want);
    r->len += want;
    return ESP_OK;
}

/* Extract the (unescaped) base64 string for "b64_json" and decode it
 * into `out`. We scan rather than cJSON-parse the ~900 KB response so
 * we don't blow the internal heap. The base64 alphabet has no JSON
 * escapes, so the value runs verbatim to the next quote. */
static bool decode_b64_json(const char *resp, uint8_t *out, size_t out_cap,
                            size_t *out_len) {
    const char *k = strstr(resp, "\"b64_json\"");
    if (!k) return false;
    const char *colon = strchr(k, ':');
    if (!colon) return false;
    const char *q = strchr(colon, '"');
    if (!q) return false;
    const char *start = q + 1;
    const char *end = strchr(start, '"');
    if (!end || end <= start) return false;
    size_t b64len = (size_t)(end - start);
    int rc = mbedtls_base64_decode(out, out_cap, out_len,
                                   (const unsigned char *)start, b64len);
    if (rc != 0) {
        ESP_LOGE(TAG, "base64 decode rc=-0x%04x (b64len=%u)",
            -rc, (unsigned)b64len);
        return false;
    }
    return true;
}

/* Build the multipart body in PSRAM and POST it to OpenAI. On success
 * the decoded PNG lands in out_png/out_len. */
static bool openai_edit(const char *key, const char *prompt,
                        const char *size_str,
                        const uint8_t *guide, size_t glen,
                        const uint8_t *exemplar, size_t elen,
                        uint8_t *out_png, size_t out_cap, size_t *out_len) {
    size_t plen = strlen(prompt);
    size_t body_cap = plen + glen + elen + 2048;
    uint8_t *body = (uint8_t *)heap_caps_malloc(body_cap, MALLOC_CAP_SPIRAM);
    if (!body) { ESP_LOGE(TAG, "multipart PSRAM alloc failed"); return false; }

    size_t pos = 0;
    #define APP(p, n) do { memcpy(body + pos, (p), (n)); pos += (n); } while (0)
    #define APPS(s)   do { const char *_s = (s); APP(_s, strlen(_s)); } while (0)
    char field[256];
    /* simple text fields */
    snprintf(field, sizeof(field),
        "--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"model\""
        "\r\n\r\ngpt-image-2\r\n");
    APPS(field);
    APPS("--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"prompt\"\r\n\r\n");
    APP(prompt, plen);
    APPS("\r\n");
    APPS("--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"n\"\r\n\r\n1\r\n");
    snprintf(field, sizeof(field),
        "--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"size\""
        "\r\n\r\n%s\r\n", size_str);
    APPS(field);
    APPS("--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"quality\"\r\n\r\nlow\r\n");
    /* guide image */
    APPS("--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"image[]\";"
         " filename=\"guide.png\"\r\nContent-Type: image/png\r\n\r\n");
    APP(guide, glen);
    APPS("\r\n");
    /* exemplar image */
    APPS("--" MP_BOUNDARY "\r\nContent-Disposition: form-data; name=\"image[]\";"
         " filename=\"exemplar.png\"\r\nContent-Type: image/png\r\n\r\n");
    APP(exemplar, elen);
    APPS("\r\n--" MP_BOUNDARY "--\r\n");
    #undef APP
    #undef APPS

    rcap_t rctx = {
        .buf = (uint8_t *)heap_caps_malloc(OPENAI_RESP_MAX, MALLOC_CAP_SPIRAM),
        .cap = OPENAI_RESP_MAX, .len = 0, .overflow = false,
    };
    if (!rctx.buf) {
        ESP_LOGE(TAG, "openai resp PSRAM alloc failed");
        heap_caps_free(body);
        return false;
    }

    esp_http_client_config_t cfg = {0};
    cfg.url = OPENAI_EDITS_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.event_handler = on_openai_data;
    cfg.user_data = &rctx;
    cfg.timeout_ms = 90000;   /* gen ~25 s; allow slack */
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        heap_caps_free(body); heap_caps_free(rctx.buf);
        return false;
    }
    char auth[MOCHI_OPENAI_KEY_MAX + 16];
    snprintf(auth, sizeof(auth), "Bearer %s", key);
    esp_http_client_set_header(cli, "Authorization", auth);
    esp_http_client_set_header(cli, "Content-Type",
        "multipart/form-data; boundary=" MP_BOUNDARY);
    esp_http_client_set_post_field(cli, (const char *)body, (int)pos);

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    heap_caps_free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "openai perform: %s", esp_err_to_name(err));
        heap_caps_free(rctx.buf);
        return false;
    }
    if (rctx.overflow) {
        ESP_LOGE(TAG, "openai response exceeded %u", (unsigned)OPENAI_RESP_MAX);
        heap_caps_free(rctx.buf);
        return false;
    }
    rctx.buf[rctx.len] = 0;
    if (status != 200) {
        ESP_LOGE(TAG, "openai HTTP %d: %.200s", status, (char *)rctx.buf);
        heap_caps_free(rctx.buf);
        return false;
    }
    ESP_LOGI(TAG, "openai 200 in %lld ms, %u bytes resp",
        (long long)((esp_timer_get_time() - t0) / 1000), (unsigned)rctx.len);

    bool ok = decode_b64_json((const char *)rctx.buf, out_png, out_cap, out_len);
    heap_caps_free(rctx.buf);
    if (ok) ESP_LOGI(TAG, "decoded PNG %u bytes", (unsigned)*out_len);
    return ok;
}

/* POST the raw PNG to /sheets/:id/png as image/png. */
static bool upload_png(const char *rel, const char *pet_id,
                       const uint8_t *png, size_t len) {
    char url[200];
    snprintf(url, sizeof(url), "%s%s", MOCHI_BASE, rel);
    esp_http_client_config_t cfg = {0};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 30000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;
    char hdr_pet[96];
    snprintf(hdr_pet, sizeof(hdr_pet), "%s", pet_id);
    esp_http_client_set_header(cli, "X-Pet-Id", hdr_pet);
    esp_http_client_set_header(cli, "Content-Type", "image/png");
    esp_http_client_set_post_field(cli, (const char *)png, (int)len);
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "upload perform: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "upload HTTP %d", status);
        return false;
    }
    return true;
}

/* ─── failure helper ──────────────────────────────────────────────── */

static void fail_reason(const char *reason, const char *pet_id,
                        const char *failed_url) {
    snprintf(s_reason, sizeof(s_reason), "%s", reason ? reason : "unknown");
    ESP_LOGW(TAG, "imagine failed: %s", s_reason);
    if (pet_id && failed_url && failed_url[0]) {
        char body[160];
        snprintf(body, sizeof(body), "{\"reason\":\"%s\"}", s_reason);
        char *r = api_post_json(failed_url, pet_id, body);
        free(r);
    }
    set_phase(IMAGINE_FAILED);
    atomic_store(&s_in_flight, false);
}

/* ─── worker ──────────────────────────────────────────────────────── */

static void run_imagine(const imagine_req_t *req) {
    char pet_id[MOCHI_PET_ID_MAX + 1] = {0};
    struct mochi_pair_creds creds;
    if (!pair_creds_load(&creds) || !creds.pet_id[0]) {
        fail_reason("no pet_id", NULL, NULL);
        return;
    }
    snprintf(pet_id, sizeof(pet_id), "%s", creds.pet_id);

    char key[MOCHI_OPENAI_KEY_MAX + 1] = {0};
    if (!openai_key_load(key, sizeof(key)) || strlen(key) < 10) {
        fail_reason("no openai key", NULL, NULL);
        return;
    }

    /* 1 — queue ----------------------------------------------------- */
    set_phase(IMAGINE_QUEUEING);
    char qbody[600];
    /* JSON-escaping: seed_name/seed_vibe come from the model; they may
     * contain quotes. Build via cJSON to escape safely. */
    cJSON *qj = cJSON_CreateObject();
    cJSON_AddStringToObject(qj, "seedName", req->seed_name);
    cJSON_AddStringToObject(qj, "seedVibe", req->seed_vibe);
    if (req->from_place_id[0])
        cJSON_AddStringToObject(qj, "fromPlaceId", req->from_place_id);
    char *qstr = cJSON_PrintUnformatted(qj);
    cJSON_Delete(qj);
    snprintf(qbody, sizeof(qbody), "%s", qstr ? qstr : "{}");
    free(qstr);

    char *qresp = api_post_json("/api/places/queue", pet_id, qbody);
    if (!qresp) { fail_reason("queue request failed", NULL, NULL); return; }
    cJSON *qroot = cJSON_Parse(qresp);
    free(qresp);
    if (!qroot) { fail_reason("queue parse failed", NULL, NULL); return; }
    if (!json_str(qroot, "placeId", s_place_id, sizeof(s_place_id))) {
        char reason[120] = "queue rejected";
        json_str(qroot, "reason", reason, sizeof(reason));
        cJSON_Delete(qroot);
        fail_reason(reason, NULL, NULL);
        return;
    }
    cJSON_Delete(qroot);
    ESP_LOGI(TAG, "queued place %s", s_place_id);

    /* 2 — orchestration -------------------------------------------- */
    char orel[96];
    snprintf(orel, sizeof(orel), "/api/places/%s/orchestration", s_place_id);
    char *oresp = api_get(orel, pet_id);
    if (!oresp) { fail_reason("orchestration failed", NULL, NULL); return; }
    cJSON *oroot = cJSON_Parse(oresp);
    free(oresp);
    if (!oroot) { fail_reason("orchestration parse failed", NULL, NULL); return; }

    char prompt[1200] = {0};
    char size_str[16] = "1504x720";
    char guide_url[160] = {0}, exemplar_url[160] = {0}, upload_url[160] = {0};
    char pack_url[160] = {0}, ready_url[160] = {0}, failed_url[160] = {0};
    char sheet_id[80] = {0};
    bool ok_fields =
        json_str(oroot, "prompt", prompt, sizeof(prompt)) &&
        json_str(oroot, "guide_url", guide_url, sizeof(guide_url)) &&
        json_str(oroot, "exemplar_url", exemplar_url, sizeof(exemplar_url)) &&
        json_str(oroot, "upload_url", upload_url, sizeof(upload_url)) &&
        json_str(oroot, "pack_url", pack_url, sizeof(pack_url)) &&
        json_str(oroot, "ready_url", ready_url, sizeof(ready_url)) &&
        json_str(oroot, "sheet_id", sheet_id, sizeof(sheet_id));
    json_str(oroot, "failed_url", failed_url, sizeof(failed_url));
    {
        cJSON *gw = cJSON_GetObjectItemCaseSensitive(oroot, "gen_w");
        cJSON *gh = cJSON_GetObjectItemCaseSensitive(oroot, "gen_h");
        if (cJSON_IsNumber(gw) && cJSON_IsNumber(gh)) {
            snprintf(size_str, sizeof(size_str), "%dx%d",
                (int)gw->valuedouble, (int)gh->valuedouble);
        }
    }
    cJSON_Delete(oroot);
    if (!ok_fields) { fail_reason("orchestration missing fields", pet_id, failed_url); return; }

    /* 3 — fetch guide + exemplar ----------------------------------- */
    set_phase(IMAGINE_FETCHING_GUIDE);
    uint8_t *guide = (uint8_t *)heap_caps_malloc(GUIDE_MAX, MALLOC_CAP_SPIRAM);
    uint8_t *exemplar = (uint8_t *)heap_caps_malloc(EXEMPLAR_MAX, MALLOC_CAP_SPIRAM);
    uint8_t *png = (uint8_t *)heap_caps_malloc(PNG_MAX, MALLOC_CAP_SPIRAM);
    if (!guide || !exemplar || !png) {
        heap_caps_free(guide); heap_caps_free(exemplar); heap_caps_free(png);
        fail_reason("psram alloc failed", pet_id, failed_url);
        return;
    }
    char url[200];
    size_t glen = 0, elen = 0, png_len = 0;
    uint32_t ms = 0;
    snprintf(url, sizeof(url), "%s%s", MOCHI_BASE, guide_url);
    if (!sprite_fetch_blob(url, guide, GUIDE_MAX, &glen, &ms)) {
        heap_caps_free(guide); heap_caps_free(exemplar); heap_caps_free(png);
        fail_reason("guide fetch failed", pet_id, failed_url);
        return;
    }

    set_phase(IMAGINE_FETCHING_EXEMPLAR);
    snprintf(url, sizeof(url), "%s%s", MOCHI_BASE, exemplar_url);
    if (!sprite_fetch_blob(url, exemplar, EXEMPLAR_MAX, &elen, &ms)) {
        heap_caps_free(guide); heap_caps_free(exemplar); heap_caps_free(png);
        fail_reason("exemplar fetch failed", pet_id, failed_url);
        return;
    }

    /* 4 — generate ------------------------------------------------- */
    set_phase(IMAGINE_GENERATING);
    bool gen_ok = openai_edit(key, prompt, size_str,
                              guide, glen, exemplar, elen,
                              png, PNG_MAX, &png_len);
    heap_caps_free(guide);
    heap_caps_free(exemplar);
    if (!gen_ok) {
        heap_caps_free(png);
        fail_reason("openai generation failed", pet_id, failed_url);
        return;
    }

    /* 5 — upload --------------------------------------------------- */
    set_phase(IMAGINE_UPLOADING);
    bool up_ok = upload_png(upload_url, pet_id, png, png_len);
    heap_caps_free(png);
    if (!up_ok) { fail_reason("upload failed", pet_id, failed_url); return; }

    /* 6 — mark ready ---------------------------------------------- */
    set_phase(IMAGINE_FINALISING);
    char *rresp = api_post_json(ready_url, pet_id, "{}");
    if (!rresp) { fail_reason("mark-ready failed", pet_id, failed_url); return; }
    free(rresp);

    /* 7 — fetch the assembled pack (server crops day/night cells) -- */
    set_phase(IMAGINE_FETCHING_PACK);
    uint8_t *pack = (uint8_t *)heap_caps_malloc(PACK_MAX, MALLOC_CAP_SPIRAM);
    if (!pack) { fail_reason("pack alloc failed", pet_id, failed_url); return; }
    size_t pack_len = 0;
    snprintf(url, sizeof(url), "%s%s", MOCHI_BASE, pack_url);
    if (!sprite_fetch_blob(url, pack, PACK_MAX, &pack_len, &ms)) {
        heap_caps_free(pack);
        fail_reason("pack fetch failed", pet_id, failed_url);
        return;
    }

    /* 8 — swap into the live scene pack --------------------------- */
    set_phase(IMAGINE_SWAPPING);
    if (!scene_pack_load_bytes(pack)) {
        heap_caps_free(pack);
        fail_reason("pack invalid", pet_id, failed_url);
        return;
    }
    /* Keep the pack alive (scene_pack holds pointers into it); free the
     * previous imagined pack if any. */
    if (s_active_pack) heap_caps_free(s_active_pack);
    s_active_pack = pack;
    snprintf(s_pack_path, sizeof(s_pack_path), "%s", sheet_id);

    set_phase(IMAGINE_DONE);
    atomic_store(&s_in_flight, false);
    ESP_LOGI(TAG, "imagine done: %s (%u-byte pack)",
        s_place_id, (unsigned)pack_len);
}

static void worker_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "worker_task started");
    while (atomic_load(&s_running)) {
        imagine_req_t *req = NULL;
        if (xQueueReceive(s_queue, &req, pdMS_TO_TICKS(200)) != pdTRUE) continue;
        if (!req) continue;

        s_reason[0] = s_place_id[0] = s_pack_path[0] = '\0';
        ESP_LOGI(TAG, "imagine: name='%s' from='%s' vibe.len=%u",
            req->seed_name, req->from_place_id,
            (unsigned)strlen(req->seed_vibe));

        run_imagine(req);

        /* run_imagine clears s_in_flight on its own terminal paths; be
         * defensive in case a path missed it. */
        atomic_store(&s_in_flight, false);
        free(req);
    }
    vTaskDelete(NULL);
}

bool imagine_init(void) {
    if (s_queue) return true;
    s_queue = xQueueCreate(WORKER_QUEUE_DEPTH, sizeof(imagine_req_t *));
    if (!s_queue) { ESP_LOGE(TAG, "queue alloc failed"); return false; }
    atomic_store(&s_phase, IMAGINE_IDLE);
    atomic_store(&s_in_flight, false);
    atomic_store(&s_running, true);
    BaseType_t ok = xTaskCreate(worker_task, "imagine",
        WORKER_STACK_BYTES, NULL, 5, NULL);
    if (ok != pdPASS) { ESP_LOGE(TAG, "worker_task create failed"); return false; }
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
        heap_caps_free(cloned);
        return false;
    }
    s_last_attempt_us = now_us;
    atomic_store(&s_in_flight, true);
    return true;
}

bool imagine_in_flight(void) { return atomic_load(&s_in_flight); }
imagine_phase_t imagine_phase(void) { return (imagine_phase_t)atomic_load(&s_phase); }
const char *imagine_place_id(void) { return s_place_id; }
const char *imagine_last_pack_path(void) { return s_pack_path; }
const char *imagine_last_reason(void) { return s_reason; }
