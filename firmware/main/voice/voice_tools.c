/*
 * Voice tool-call dispatch — async POST to /api/voice/tool, feed
 * the result back to OpenAI as function_call_output.
 *
 * Threading model: a single dedicated FreeRTOS task drains a queue
 * of tool-call requests. The peer's main loop (running on the
 * voice_peer worker task) just enqueues and returns; HTTPS to
 * val.run happens off-thread so DTLS keepalives keep firing during
 * the ~150-300 ms round-trip.
 *
 * Lifecycle:
 *   voice_tools_init()       — once at boot. Idempotent.
 *   voice_tools_set_pet_id() — once per session start.
 *   voice_tools_dispatch()   — enqueue. Non-blocking.
 *   voice_tools_shutdown()   — drain + stop the worker on stop_session.
 *
 * Each queue entry owns its own copies of call_id / name / args; the
 * caller passes char* and we strdup. Worker frees on consumption.
 */

#include "voice_tools.h"
#include "voice_https.h"
#include "voice_peer.h"
#include "imagine.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

#include "voice_diag.h"

#define TAG "voice_tools"

#define LOGI_DIAG(fmt, ...) do {              \
    ESP_LOGI(TAG, fmt, ##__VA_ARGS__);        \
    voice_diag_log(fmt, ##__VA_ARGS__);       \
} while (0)
#define LOGW_DIAG(fmt, ...) do {              \
    ESP_LOGW(TAG, fmt, ##__VA_ARGS__);        \
    voice_diag_log("WARN: " fmt, ##__VA_ARGS__); \
} while (0)
#define LOGE_DIAG(fmt, ...) do {              \
    ESP_LOGE(TAG, fmt, ##__VA_ARGS__);        \
    voice_diag_log("ERR: " fmt, ##__VA_ARGS__); \
} while (0)

#define WORKER_STACK_BYTES  (8 * 1024)
#define WORKER_PRIO         5
#define QUEUE_DEPTH         4   /* concurrent in-flight tool calls */

#define VAL_RUN_TOOL_URL    "https://mochi.val.run/api/voice/tool"
#define PET_ID_BUF_BYTES    65   /* MOCHI_PET_ID_MAX (64) + NUL */

typedef struct {
    char *call_id;
    char *name;
    char *args_json;
} tool_req_t;

static QueueHandle_t s_queue;
static TaskHandle_t  s_worker;
static atomic_bool   s_running;
static char          s_pet_id[PET_ID_BUF_BYTES];

/* The /api/voice/tool response. We strip `state` before forwarding
 * to OpenAI — that field is for client renderers, not the model. */
typedef struct {
    bool   ok;
    char  *message;   /* malloc'd or NULL */
    char  *reason;    /* malloc'd or NULL */
} tool_result_t;

static void tool_req_free(tool_req_t *r) {
    if (!r) return;
    free(r->call_id);
    free(r->name);
    free(r->args_json);
    free(r);
}

static void tool_result_clear(tool_result_t *r) {
    if (!r) return;
    free(r->message); r->message = NULL;
    free(r->reason);  r->reason = NULL;
}

/* val.run response handler — parses {ok, message?, reason?} into the
 * tool_result_t we'll forward to OpenAI. */
static void on_tool_response(http_resp_t *resp, void *ctx) {
    tool_result_t *out = (tool_result_t *)ctx;
    cJSON *root = cJSON_Parse(resp->data);
    if (!root) {
        LOGE_DIAG("tool response not JSON");
        return;
    }
    cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "ok");
    if (cJSON_IsBool(ok)) {
        out->ok = cJSON_IsTrue(ok);
    }
    cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "message");
    if (cJSON_IsString(msg) && msg->valuestring) {
        out->message = strdup(msg->valuestring);
    }
    cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
    if (cJSON_IsString(reason) && reason->valuestring) {
        out->reason = strdup(reason->valuestring);
    }
    cJSON_Delete(root);
}

/* POST to val.run. Returns true if val.run replied 2xx. The
 * tool_result_t reflects val.run's structured response either way;
 * caller checks `out->ok` to know if val.run accepted the call. */
static bool dispatch_to_valrun(const tool_req_t *req, tool_result_t *out) {
    if (!s_pet_id[0]) {
        LOGE_DIAG("no pet_id set; cannot dispatch '%s'", req->name);
        return false;
    }

    /* Build POST body: { "name": "<name>", "args": <args_json> }.
     * The args_json is already a JSON object string from OpenAI; we
     * parse + reattach so cJSON does the escaping correctly. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", req->name);
    cJSON *args = NULL;
    if (req->args_json && *req->args_json) {
        args = cJSON_Parse(req->args_json);
    }
    if (!args) {
        args = cJSON_CreateObject();
    }
    cJSON_AddItemToObject(root, "args", args);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;

    char content_type[] = "Content-Type: application/json";
    char pet_header[80];
    snprintf(pet_header, sizeof(pet_header), "X-Pet-Id: %s", s_pet_id);
    char *headers[] = { content_type, pet_header, NULL };

    char url[] = VAL_RUN_TOOL_URL;
    int rc = https_post(url, headers, body, on_tool_response, out);
    free(body);
    return rc == 0;
}

/* Send `function_call_output` + `response.create` events to OpenAI
 * via the data channel. Refused calls and successful calls both get
 * their tool_result_t serialised here so the model sees a uniform
 * shape. */
static void send_function_call_output(const char *call_id, const tool_result_t *result) {
    /* Build the model-facing JSON: {"ok":bool, "message"?:..., "reason"?:...}.
     * Stringified once and embedded as the .output field. */
    cJSON *model_root = cJSON_CreateObject();
    cJSON_AddBoolToObject(model_root, "ok", result->ok);
    if (result->message) {
        cJSON_AddStringToObject(model_root, "message", result->message);
    }
    if (result->reason) {
        cJSON_AddStringToObject(model_root, "reason", result->reason);
    }
    char *output_str = cJSON_PrintUnformatted(model_root);
    cJSON_Delete(model_root);
    if (!output_str) return;

    /* Wrap as conversation.item.create function_call_output. */
    cJSON *wrap = cJSON_CreateObject();
    cJSON_AddStringToObject(wrap, "type", "conversation.item.create");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddItemToObject(wrap, "item", item);
    cJSON_AddStringToObject(item, "type", "function_call_output");
    cJSON_AddStringToObject(item, "call_id", call_id);
    cJSON_AddStringToObject(item, "output", output_str);
    char *wrap_str = cJSON_PrintUnformatted(wrap);
    cJSON_Delete(wrap);
    free(output_str);
    if (!wrap_str) return;

    /* Then a response.create so the model continues. */
    cJSON *resp_root = cJSON_CreateObject();
    cJSON_AddStringToObject(resp_root, "type", "response.create");
    char *resp_str = cJSON_PrintUnformatted(resp_root);
    cJSON_Delete(resp_root);

    int r1 = voice_peer_send_dc_json(wrap_str);
    int r2 = resp_str ? voice_peer_send_dc_json(resp_str) : -1;
    LOGI_DIAG("tool result sent: ok=%d r1=%d r2=%d", result->ok, r1, r2);
    free(wrap_str);
    free(resp_str);
}

static void worker_task(void *arg) {
    (void)arg;
    LOGI_DIAG("tools worker_task started");
    while (atomic_load(&s_running)) {
        tool_req_t *req = NULL;
        if (xQueueReceive(s_queue, &req, pdMS_TO_TICKS(200)) != pdTRUE) {
            continue;
        }
        if (!req) continue;

        LOGI_DIAG("dispatch: name=%s call_id=%s args=%u B",
            req->name, req->call_id,
            (unsigned)(req->args_json ? strlen(req->args_json) : 0));

        tool_result_t result = { .ok = false, .message = NULL, .reason = NULL };

        /* Locally-handled tools intercept here before reaching the
         * val.run dispatch. They're tools whose effect is on-device
         * (e.g. "imagine_place" kicks off scene generation). The
         * model-facing reply still flows through the same
         * function_call_output path so the conversational shape is
         * uniform. See design/16-on-device-imagine.md. */
        bool intercepted = false;
        if (strcmp(req->name, "imagine_place") == 0) {
            /* Args follow shared/voice-tools-spec.ts: { name, vibe,
             * revising? }. We translate `revising` (a place name)
             * into `from_place_id` later — the v0 stub doesn't
             * care since it doesn't actually call the queue
             * endpoint yet. */
            imagine_req_t ireq = {0};
            cJSON *args = req->args_json ? cJSON_Parse(req->args_json) : NULL;
            if (args) {
                const cJSON *name_j = cJSON_GetObjectItemCaseSensitive(args, "name");
                const cJSON *vibe_j = cJSON_GetObjectItemCaseSensitive(args, "vibe");
                const cJSON *rev_j  = cJSON_GetObjectItemCaseSensitive(args, "revising");
                if (cJSON_IsString(name_j)) {
                    strncpy(ireq.seed_name, name_j->valuestring, sizeof(ireq.seed_name) - 1);
                }
                if (cJSON_IsString(vibe_j)) {
                    strncpy(ireq.seed_vibe, vibe_j->valuestring, sizeof(ireq.seed_vibe) - 1);
                }
                if (cJSON_IsString(rev_j)) {
                    strncpy(ireq.from_place_id, rev_j->valuestring, sizeof(ireq.from_place_id) - 1);
                }
                cJSON_Delete(args);
            }
            if (ireq.seed_name[0] && ireq.seed_vibe[0] && imagine_start(&ireq)) {
                result.ok = true;
                result.message = strdup("painting now — give me a minute");
            } else {
                result.reason = strdup(
                    ireq.seed_name[0] && ireq.seed_vibe[0]
                        ? "I can't paint right now — too busy"
                        : "I need both a name and a vibe");
            }
            intercepted = true;
        }

        bool ok = intercepted ? true : dispatch_to_valrun(req, &result);
        if (!ok && !result.reason) {
            /* Network / val.run failure — fabricate a model-friendly
             * reason so the model can react gracefully. */
            result.reason = strdup("couldn't reach my body");
        }

        send_function_call_output(req->call_id, &result);

        tool_result_clear(&result);
        tool_req_free(req);
    }
    LOGI_DIAG("tools worker_task exiting");
    s_worker = NULL;
    vTaskDelete(NULL);
}

bool voice_tools_init(void) {
    if (s_queue) return true;  /* idempotent */
    s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(tool_req_t *));
    if (!s_queue) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return false;
    }
    atomic_store(&s_running, true);
    BaseType_t ok = xTaskCreate(
        worker_task, "voice_tools", WORKER_STACK_BYTES,
        NULL, WORKER_PRIO, &s_worker);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        vQueueDelete(s_queue);
        s_queue = NULL;
        atomic_store(&s_running, false);
        return false;
    }
    return true;
}

void voice_tools_set_pet_id(const char *pet_id) {
    if (!pet_id) {
        s_pet_id[0] = 0;
        return;
    }
    strncpy(s_pet_id, pet_id, sizeof(s_pet_id) - 1);
    s_pet_id[sizeof(s_pet_id) - 1] = 0;
}

bool voice_tools_dispatch(const char *call_id, const char *name, const char *args_json) {
    if (!s_queue) {
        ESP_LOGW(TAG, "dispatch: not initialised");
        return false;
    }
    if (!call_id || !name) {
        ESP_LOGW(TAG, "dispatch: bad args");
        return false;
    }
    tool_req_t *req = (tool_req_t *)calloc(1, sizeof(tool_req_t));
    if (!req) return false;
    req->call_id = strdup(call_id);
    req->name = strdup(name);
    req->args_json = args_json ? strdup(args_json) : NULL;
    if (!req->call_id || !req->name) {
        tool_req_free(req);
        return false;
    }
    if (xQueueSend(s_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full; dropping '%s'", name);
        tool_req_free(req);
        return false;
    }
    return true;
}

void voice_tools_shutdown(void) {
    if (!s_queue) return;
    atomic_store(&s_running, false);
    /* Wait briefly for the worker to exit. */
    for (int i = 0; i < 30 && s_worker; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* Drain any remaining queued requests so we don't leak. */
    tool_req_t *req = NULL;
    while (xQueueReceive(s_queue, &req, 0) == pdTRUE) {
        tool_req_free(req);
    }
    vQueueDelete(s_queue);
    s_queue = NULL;
    s_pet_id[0] = 0;
}
