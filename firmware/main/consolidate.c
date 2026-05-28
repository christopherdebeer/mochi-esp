/*
 * consolidate — on-device sleep consolidation worker (design/19).
 * See consolidate.h for the flow + trust boundary. Mirrors imagine.c's
 * structure: a single worker task drains a depth-1 queue and runs the
 * full server-orchestrated pass with the BYO key.
 */

#include "consolidate.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "cJSON.h"

#include "openai_key.h"
#include "pair_creds.h"
#include "device_diag.h"
#include "model_prefs.h"

static const char *TAG = "consolidate";

#define MOCHI_BASE       "https://mochi.val.run"
#define ORCH_PATH        "/api/consolidate/orchestration"
#define PERSIST_PATH     "/api/consolidate/persist"
#define USAGE_PATH       "/api/usage/event"
#define OPENAI_CHAT_URL  "https://api.openai.com/v1/chat/completions"

/* Response accumulator cap. The orchestration prompt (system rules +
 * up to 30 transcript turns + facts) runs ~10-15 KB; the chat result is
 * small (max_tokens 900 ≈ 4 KB). 48 KB is generous for both. */
#define HTTP_BODY_MAX    (48u * 1024)

/* Timeouts. The orchestration GET hits getPet → a cold mochi.val.run
 * isolate can spend tens of seconds in initDb's serial migrations, so
 * it gets a generous budget (voice_https' 10 s would falsely fail). The
 * chat call can take a while on a reasoning-class model. */
#define ORCH_TIMEOUT_MS  60000
#define CHAT_TIMEOUT_MS  90000
#define POST_TIMEOUT_MS  30000

/* Local debounce. The 6 h server cooldown is the durable cap; this is
 * the cheap local guard against re-firing in the window between a pass
 * completing and the next /api/state pull clearing the advice (≤ the
 * 5-min resync). 10 min comfortably covers it. */
#define DEBOUNCE_US      (10LL * 60 * 1000 * 1000)

#define WORKER_STACK_BYTES 8192
#define WORKER_QUEUE_DEPTH 1

static QueueHandle_t s_queue;
static atomic_bool   s_in_flight;
static atomic_bool   s_running;
static int64_t       s_last_attempt_us;

/* ─── HTTP helper (generous timeout, growing response buffer) ──────── */

typedef struct { char *buf; int len; int cap; bool overflow; } acc_t;

static esp_err_t on_data(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    acc_t *a = (acc_t *)evt->user_data;
    if (a->overflow) return ESP_OK;
    int need = a->len + evt->data_len + 1;   /* +1 for trailing NUL */
    if (need > (int)HTTP_BODY_MAX) { a->overflow = true; return ESP_OK; }
    if (need > a->cap) {
        int nc = a->cap ? a->cap : 1024;
        while (nc < need) nc *= 2;
        if (nc > (int)HTTP_BODY_MAX) nc = (int)HTTP_BODY_MAX;
        char *g = (char *)realloc(a->buf, (size_t)nc);
        if (!g) { a->overflow = true; return ESP_OK; }
        a->buf = g; a->cap = nc;
    }
    memcpy(a->buf + a->len, evt->data, (size_t)evt->data_len);
    a->len += evt->data_len;
    a->buf[a->len] = 0;
    return ESP_OK;
}

/* Perform one request. `headers` is a NULL-terminated array of
 * "Name: Value" strings (mutated in place then restored, like
 * voice_https). Returns the HTTP status (>0) or -1 on transport error /
 * overflow. On a non-NULL out_body the caller owns + frees the buffer
 * (set even for non-2xx so the caller can log the error body). */
static int http_do(const char *method, const char *url, char **headers,
                   const char *body, int timeout_ms,
                   char **out_body, int *out_len) {
    if (out_body) *out_body = NULL;
    if (out_len) *out_len = 0;

    acc_t acc = {0};
    esp_http_client_config_t cfg = {0};
    cfg.url = url;
    cfg.method = (strcmp(method, "POST") == 0) ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    cfg.event_handler = on_data;
    cfg.user_data = &acc;
    cfg.timeout_ms = timeout_ms;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return -1;

    if (headers) {
        for (int i = 0; headers[i]; i++) {
            char *line = headers[i];
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = 0;
            char *val = colon + 1;
            while (*val == ' ') val++;
            esp_http_client_set_header(cli, line, val);
            *colon = ':';
        }
    }
    if (body) esp_http_client_set_post_field(cli, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s %s: %s", method, url, esp_err_to_name(err));
        free(acc.buf);
        return -1;
    }
    if (acc.overflow) {
        ESP_LOGW(TAG, "%s %s response exceeded %u", method, url,
            (unsigned)HTTP_BODY_MAX);
        free(acc.buf);
        return -1;
    }
    if (out_body) *out_body = acc.buf; else free(acc.buf);
    if (out_len) *out_len = acc.len;
    return status;
}

/* ─── worker ──────────────────────────────────────────────────────── */

static void run_consolidate(void) {
    struct mochi_pair_creds creds;
    if (!pair_creds_load(&creds) || !creds.pet_id[0]) {
        ESP_LOGW(TAG, "no pet_id; skip");
        return;
    }
    char key[MOCHI_OPENAI_KEY_MAX + 1] = {0};
    if (!openai_key_load(key, sizeof(key)) || strlen(key) < 10) {
        ESP_LOGW(TAG, "no openai key; skip");
        return;
    }

    char hdr_pet[96];
    snprintf(hdr_pet, sizeof(hdr_pet), "X-Pet-Id: %s", creds.pet_id);
    char hdr_ct[] = "Content-Type: application/json";

    /* 1 — orchestration: the server builds the prompt + picks params.
     * Pass the on-device text-model choice (model_prefs); the server
     * validates it against its allowlist and falls back to the default
     * for an unknown id. */
    char text_model[48];
    model_prefs_text(text_model, sizeof(text_model));
    char url[200];
    snprintf(url, sizeof(url), "%s%s?model=%s", MOCHI_BASE, ORCH_PATH, text_model);
    char *get_headers[] = { hdr_pet, NULL };
    char *obody = NULL;
    int ostatus = http_do("GET", url, get_headers, NULL, ORCH_TIMEOUT_MS,
                          &obody, NULL);
    if (ostatus != 200 || !obody) {
        ESP_LOGW(TAG, "orchestration HTTP %d", ostatus);
        free(obody);
        return;
    }
    cJSON *oroot = cJSON_Parse(obody);
    free(obody);
    if (!oroot) { ESP_LOGW(TAG, "orchestration parse failed"); return; }

    bool eligible = cJSON_IsTrue(
        cJSON_GetObjectItemCaseSensitive(oroot, "eligible"));
    if (!eligible) {
        cJSON *jr = cJSON_GetObjectItemCaseSensitive(oroot, "reason");
        ESP_LOGI(TAG, "not eligible: %s",
            cJSON_IsString(jr) ? jr->valuestring : "?");
        cJSON_Delete(oroot);
        return;   /* not a failure — nothing to reflect on right now */
    }

    cJSON *jsys = cJSON_GetObjectItemCaseSensitive(oroot, "system");
    cJSON *jusr = cJSON_GetObjectItemCaseSensitive(oroot, "user");
    if (!cJSON_IsString(jsys) || !cJSON_IsString(jusr)) {
        ESP_LOGW(TAG, "orchestration missing prompt");
        cJSON_Delete(oroot);
        return;
    }
    cJSON *jmodel = cJSON_GetObjectItemCaseSensitive(oroot, "model");
    cJSON *jtemp  = cJSON_GetObjectItemCaseSensitive(oroot, "temperature");
    cJSON *jmax   = cJSON_GetObjectItemCaseSensitive(oroot, "max_tokens");
    const char *model = cJSON_IsString(jmodel) ? jmodel->valuestring
                                               : "gpt-4o-mini";
    double temperature = cJSON_IsNumber(jtemp) ? jtemp->valuedouble : 0.6;
    int max_tokens = cJSON_IsNumber(jmax) ? (int)jmax->valuedouble : 900;

    /* 2 — build the chat request (cJSON handles escaping the prompts). */
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "model", model);
    cJSON *msgs = cJSON_AddArrayToObject(req, "messages");
    cJSON *m1 = cJSON_CreateObject();
    cJSON_AddStringToObject(m1, "role", "system");
    cJSON_AddStringToObject(m1, "content", jsys->valuestring);
    cJSON_AddItemToArray(msgs, m1);
    cJSON *m2 = cJSON_CreateObject();
    cJSON_AddStringToObject(m2, "role", "user");
    cJSON_AddStringToObject(m2, "content", jusr->valuestring);
    cJSON_AddItemToArray(msgs, m2);
    cJSON *rf = cJSON_CreateObject();
    cJSON_AddStringToObject(rf, "type", "json_object");
    cJSON_AddItemToObject(req, "response_format", rf);
    cJSON_AddNumberToObject(req, "temperature", temperature);
    cJSON_AddNumberToObject(req, "max_tokens", max_tokens);
    char *reqstr = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);

    /* Snapshot the model for telemetry before oroot (which owns the
     * `model` string) is freed. */
    char model_buf[48];
    snprintf(model_buf, sizeof(model_buf), "%s", model);
    cJSON_Delete(oroot);   /* jsys/jusr/model now dangling; reqstr copied */
    if (!reqstr) { ESP_LOGW(TAG, "request build failed"); return; }

    /* 3 — chat completion with the BYO key (stays on-device). */
    char auth[MOCHI_OPENAI_KEY_MAX + 40];
    snprintf(auth, sizeof(auth), "Authorization: Bearer %s", key);
    char *oai_headers[] = { auth, hdr_ct, NULL };
    char *cbody = NULL;
    int64_t t0 = esp_timer_get_time();
    int cstatus = http_do("POST", OPENAI_CHAT_URL, oai_headers, reqstr,
                          CHAT_TIMEOUT_MS, &cbody, NULL);
    int64_t gen_ms = (esp_timer_get_time() - t0) / 1000;
    free(reqstr);
    if (cstatus != 200 || !cbody) {
        ESP_LOGW(TAG, "openai chat HTTP %d", cstatus);
        if (cbody) ESP_LOGW(TAG, "  %.200s", cbody);
        free(cbody);
        return;
    }

    cJSON *croot = cJSON_Parse(cbody);
    free(cbody);
    if (!croot) { ESP_LOGW(TAG, "chat parse failed"); return; }
    cJSON *choices = cJSON_GetObjectItemCaseSensitive(croot, "choices");
    cJSON *c0 = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON *msg = c0 ? cJSON_GetObjectItemCaseSensitive(c0, "message") : NULL;
    cJSON *content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content")
                         : NULL;
    if (!cJSON_IsString(content) || !content->valuestring) {
        ESP_LOGW(TAG, "chat: no content");
        cJSON_Delete(croot);
        return;
    }
    /* The content IS the {facts,dreams,diary,place} JSON object the
     * persist route expects (response_format=json_object guarantees it).
     * Copy it out; the server defaults `since` to the same value the
     * orchestration used, so we POST the result verbatim. */
    char *result = strdup(content->valuestring);
    int in_tok = 0, out_tok = 0;
    cJSON *usage = cJSON_GetObjectItemCaseSensitive(croot, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON *pt = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
        cJSON *ot = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
        if (cJSON_IsNumber(pt)) in_tok = (int)pt->valuedouble;
        if (cJSON_IsNumber(ot)) out_tok = (int)ot->valuedouble;
    }
    cJSON_Delete(croot);
    if (!result) { ESP_LOGW(TAG, "result dup failed"); return; }

    /* 4 — persist (server re-validates, clamps, merges). */
    char *post_headers[] = { hdr_pet, hdr_ct, NULL };
    char purl[160];
    snprintf(purl, sizeof(purl), "%s%s", MOCHI_BASE, PERSIST_PATH);
    char *pbody = NULL;
    int pstatus = http_do("POST", purl, post_headers, result, POST_TIMEOUT_MS,
                          &pbody, NULL);
    free(result);
    if (pstatus != 200) ESP_LOGW(TAG, "persist HTTP %d", pstatus);
    free(pbody);

    /* 5 — telemetry (fire-and-forget). The spend happened regardless of
     * the persist outcome, so it's always recorded. The server estimates
     * cost from model + tokens. */
    {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "kind", "consolidate");
        cJSON_AddStringToObject(t, "model", model_buf);
        cJSON_AddNumberToObject(t, "latency_ms", (double)gen_ms);
        cJSON_AddNumberToObject(t, "http_status", cstatus);
        cJSON_AddNumberToObject(t, "input_tokens", in_tok);
        cJSON_AddNumberToObject(t, "output_tokens", out_tok);
        cJSON *ctx = cJSON_CreateObject();
        cJSON_AddStringToObject(ctx, "trigger", "device_consolidate");
        cJSON_AddItemToObject(t, "context", ctx);
        char *tstr = cJSON_PrintUnformatted(t);
        cJSON_Delete(t);
        if (tstr) {
            char uurl[160];
            snprintf(uurl, sizeof(uurl), "%s%s", MOCHI_BASE, USAGE_PATH);
            char *ub = NULL;
            http_do("POST", uurl, post_headers, tstr, POST_TIMEOUT_MS, &ub, NULL);
            free(ub);
            free(tstr);
        }
    }

    {
        char ctx[160];
        snprintf(ctx, sizeof(ctx),
            "{\"model\":\"%s\",\"in\":%d,\"out\":%d,\"ms\":%lld}",
            model_buf, in_tok, out_tok, (long long)gen_ms);
        device_diag_event(DIAG_INFO, "consolidate", "done", ctx);
    }
    ESP_LOGI(TAG, "consolidation done: model=%s in=%d out=%d %lldms persist=%d",
        model_buf, in_tok, out_tok, (long long)gen_ms, pstatus);
}

static void worker_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "worker_task started");
    while (atomic_load(&s_running)) {
        int tok;
        if (xQueueReceive(s_queue, &tok, pdMS_TO_TICKS(500)) != pdTRUE) continue;
        run_consolidate();
        atomic_store(&s_in_flight, false);
    }
    vTaskDelete(NULL);
}

bool consolidate_init(void) {
    if (s_queue) return true;
    s_queue = xQueueCreate(WORKER_QUEUE_DEPTH, sizeof(int));
    if (!s_queue) { ESP_LOGE(TAG, "queue alloc failed"); return false; }
    atomic_store(&s_in_flight, false);
    atomic_store(&s_running, true);
    BaseType_t ok = xTaskCreate(worker_task, "consolidate",
        WORKER_STACK_BYTES, NULL, 4, NULL);
    if (ok != pdPASS) { ESP_LOGE(TAG, "worker_task create failed"); return false; }
    return true;
}

bool consolidate_start(void) {
    if (!s_queue) return false;
    if (atomic_load(&s_in_flight)) return false;
    int64_t now_us = esp_timer_get_time();
    if (s_last_attempt_us != 0 &&
        (now_us - s_last_attempt_us) < DEBOUNCE_US) {
        return false;
    }
    int tok = 1;
    if (xQueueSend(s_queue, &tok, 0) != pdTRUE) return false;
    s_last_attempt_us = now_us;
    atomic_store(&s_in_flight, true);
    return true;
}

bool consolidate_in_flight(void) { return atomic_load(&s_in_flight); }
