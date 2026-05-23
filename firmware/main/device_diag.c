#include "device_diag.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_system.h"      /* esp_reset_reason */
#include "esp_app_desc.h"    /* esp_app_get_description */
#include "esp_heap_caps.h"

#include "cJSON.h"

#include "voice/voice_https.h"
#include "pair_creds.h"

static const char *TAG = "device_diag";

#define DIAG_CAP       64    /* ring depth; matches the server per-POST cap */
#define DIAG_TAG_MAX   24
#define DIAG_MSG_MAX   120
#define DIAG_CTX_MAX   200

typedef struct {
    int64_t at_ms;
    uint8_t level;
    char    tag[DIAG_TAG_MAX];
    char    msg[DIAG_MSG_MAX];
    char    ctx[DIAG_CTX_MAX];   /* "" = none */
} diag_rec_t;

static SemaphoreHandle_t s_mtx;
static diag_rec_t       *s_ring;
static int               s_count;
static uint32_t          s_dropped;
static char              s_boot_id[20];
static char              s_fw_version[48];

static const char *reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "poweron";
        case ESP_RST_EXT:       return "ext";
        case ESP_RST_SW:        return "sw";        /* esp_restart (e.g. OTA) */
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int_wdt";
        case ESP_RST_TASK_WDT:  return "task_wdt";
        case ESP_RST_WDT:       return "wdt";
        case ESP_RST_DEEPSLEEP: return "deepsleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

bool device_diag_init(void) {
    if (s_mtx) return true;
    s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return false;
    s_ring = (diag_rec_t *)heap_caps_malloc(sizeof(diag_rec_t) * DIAG_CAP,
                                            MALLOC_CAP_SPIRAM);
    if (!s_ring) {
        ESP_LOGE(TAG, "ring PSRAM alloc failed");
        return false;
    }
    s_count = 0;
    s_dropped = 0;
    snprintf(s_boot_id, sizeof(s_boot_id), "b%08lx%08lx",
        (unsigned long)esp_random(), (unsigned long)esp_random());
    const esp_app_desc_t *d = esp_app_get_description();
    snprintf(s_fw_version, sizeof(s_fw_version), "%s", d ? d->version : "?");

    char ctx[DIAG_CTX_MAX];
    snprintf(ctx, sizeof(ctx),
        "{\"reset\":\"%s\",\"heap\":%u,\"psram\":%u,\"ver\":\"%s\"}",
        reset_reason_str(esp_reset_reason()),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        s_fw_version);
    device_diag_event(DIAG_INFO, "boot", "device boot", ctx);
    ESP_LOGI(TAG, "diag boot_id=%s fw=%s", s_boot_id, s_fw_version);
    return true;
}

void device_diag_event(int level, const char *tag, const char *msg,
                       const char *ctx) {
    /* Mirror to serial so an attached console still sees it. */
    esp_log_level_t ll = level == DIAG_ERROR ? ESP_LOG_ERROR
                       : level == DIAG_WARN  ? ESP_LOG_WARN
                                             : ESP_LOG_INFO;
    ESP_LOG_LEVEL(ll, TAG, "[%s] %s %s",
        tag ? tag : "?", msg ? msg : "", ctx ? ctx : "");

    if (!s_mtx || !s_ring) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_count >= DIAG_CAP) {
        /* Drop oldest. */
        memmove(&s_ring[0], &s_ring[1], (DIAG_CAP - 1) * sizeof(diag_rec_t));
        s_count = DIAG_CAP - 1;
        s_dropped++;
    }
    diag_rec_t *r = &s_ring[s_count++];
    r->at_ms = esp_timer_get_time() / 1000;
    r->level = (uint8_t)level;
    snprintf(r->tag, sizeof(r->tag), "%s", tag ? tag : "?");
    snprintf(r->msg, sizeof(r->msg), "%s", msg ? msg : "");
    snprintf(r->ctx, sizeof(r->ctx), "%s", ctx ? ctx : "");
    xSemaphoreGive(s_mtx);
}

void device_diag_eventf(int level, const char *tag, const char *ctx,
                        const char *fmt, ...) {
    char msg[DIAG_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    device_diag_event(level, tag, msg, ctx);
}

static void on_diag_resp(http_resp_t *resp, void *ctx) {
    (void)resp;
    (void)ctx;
}

int device_diag_flush(void) {
    if (!s_mtx || !s_ring) return 0;
    struct mochi_pair_creds creds;
    if (!pair_creds_load(&creds) || !creds.pet_id[0]) return 0;

    /* Build the batch JSON under the lock (fast, no I/O), snapshotting the
     * current count. New events may append during the POST; we remove
     * exactly the first `n` on success. */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    int n = s_count;
    if (n == 0) { xSemaphoreGive(s_mtx); return 0; }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "boot_id", s_boot_id);
    cJSON_AddStringToObject(root, "fw_version", s_fw_version);
    cJSON *arr = cJSON_AddArrayToObject(root, "records");
    for (int i = 0; i < n; i++) {
        const diag_rec_t *r = &s_ring[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "at", (double)r->at_ms);
        cJSON_AddNumberToObject(o, "level", r->level);
        cJSON_AddStringToObject(o, "tag", r->tag);
        cJSON_AddStringToObject(o, "msg", r->msg);
        if (r->ctx[0]) cJSON_AddStringToObject(o, "ctx", r->ctx);
        cJSON_AddItemToArray(arr, o);
    }
    xSemaphoreGive(s_mtx);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return 0;

    char hdr_pet[96];
    snprintf(hdr_pet, sizeof(hdr_pet), "X-Pet-Id: %s", creds.pet_id);
    char hdr_ct[] = "Content-Type: application/json";
    char *headers[] = { hdr_pet, hdr_ct, NULL };
    char url[] = "https://mochi.val.run/api/device/diag";
    int rc = https_post(url, headers, body, on_diag_resp, NULL);
    free(body);
    if (rc != 0) {
        ESP_LOGW(TAG, "diag flush rc=%d (kept %d records)", rc, n);
        return 0;   /* keep records, retry next flush */
    }

    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_count >= n) {
        memmove(&s_ring[0], &s_ring[n], (s_count - n) * sizeof(diag_rec_t));
        s_count -= n;
    } else {
        s_count = 0;
    }
    xSemaphoreGive(s_mtx);
    return n;
}
