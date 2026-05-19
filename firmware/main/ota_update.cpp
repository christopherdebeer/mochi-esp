#include "ota_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"

#include "cJSON.h"

static const char *TAG = "ota";

/* Boot-to-first-check delay. WiFi + sprite + pairing all settle in
 * the first ~10s; we don't want OTA's TLS handshake competing for
 * the radio with the boot-time sprite fetches. */
static constexpr int OTA_BOOT_DELAY_MS = 30 * 1000;

/* Interval between subsequent checks. 24 hours matches the user's
 * "auto-check on boot + daily" choice; tightening this is mostly
 * pointless for a hobbyist device. */
static constexpr int OTA_CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000;

/* Manifest payload cap. A well-formed manifest is <512 bytes — 4 KB
 * gives plenty of headroom and protects against a runaway response. */
static constexpr size_t MANIFEST_MAX_BYTES = 4096;

namespace {

const char *g_manifest_url = nullptr;
volatile bool g_reboot_ready = false;
volatile bool g_task_started = false;

struct manifest_ctx {
    char  *buf;
    size_t cap;
    size_t written;
    bool   overflowed;
};

esp_err_t manifest_event(esp_http_client_event_t *evt) {
    auto *ctx = static_cast<manifest_ctx *>(evt->user_data);
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (ctx->overflowed) return ESP_OK;
    size_t want = (size_t)evt->data_len;
    if (ctx->written + want >= ctx->cap) {
        ESP_LOGW(TAG, "manifest body > %u bytes; truncating", (unsigned)ctx->cap);
        ctx->overflowed = true;
        return ESP_OK;
    }
    memcpy(ctx->buf + ctx->written, evt->data, want);
    ctx->written += want;
    return ESP_OK;
}

/* Fetch the manifest JSON and pull out { version, url }. Returns true
 * iff both fields parsed cleanly. Caller owns out_version and out_url
 * buffers; we copy into them. */
bool fetch_manifest(const char *url,
                    char *out_version, size_t version_cap,
                    char *out_url, size_t url_cap) {
    char *body = (char *)calloc(1, MANIFEST_MAX_BYTES);
    if (!body) {
        ESP_LOGE(TAG, "manifest buf alloc failed");
        return false;
    }

    manifest_ctx ctx = { body, MANIFEST_MAX_BYTES, 0, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = manifest_event;
    cfg.user_data = &ctx;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "manifest http init failed");
        free(body);
        return false;
    }
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200 || ctx.overflowed) {
        ESP_LOGW(TAG, "manifest fetch failed: err=%d status=%d written=%u overflow=%d",
            err, status, (unsigned)ctx.written, ctx.overflowed);
        free(body);
        return false;
    }
    body[ctx.written] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "manifest parse failed");
        return false;
    }
    cJSON *jv = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *ju = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(jv) || !cJSON_IsString(ju)) {
        ESP_LOGW(TAG, "manifest missing version/url");
        cJSON_Delete(root);
        return false;
    }
    snprintf(out_version, version_cap, "%s", jv->valuestring);
    snprintf(out_url,     url_cap,     "%s", ju->valuestring);
    cJSON_Delete(root);
    return true;
}

/* esp_https_ota client config callback — invoked by the OTA helper
 * for each underlying HTTP request (manifest follow-redirects from
 * github.com → objects.githubusercontent.com). We need the cert
 * bundle on every leg, so attach it here. */
esp_err_t ota_http_init_cb(esp_http_client_handle_t client) {
    /* Most config is set on cfg below; nothing extra needed per-leg. */
    (void)client;
    return ESP_OK;
}

/* Stream the .bin from `bin_url` into the inactive OTA slot. Returns
 * true on success — on success the inactive slot has been activated
 * (otadata flipped) and a reboot will boot the new image. */
bool perform_update(const char *bin_url) {
    ESP_LOGI(TAG, "downloading firmware from %s", bin_url);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = bin_url;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.timeout_ms = 60000;
    http_cfg.keep_alive_enable = true;
    /* GitHub releases redirect to objects.githubusercontent.com; the
     * default 10 redirects is plenty. */

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;
    ota_cfg.http_client_init_cb = ota_http_init_cb;

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "OTA image written + activated");
    return true;
}

void ota_task(void *) {
    ESP_LOGI(TAG, "task started; running version=%s", ota_update::current_version());

    /* Boot settle. Let the sprite cache + scene + pet fetches at boot
     * complete before we eat radio bandwidth on a manifest poll. */
    vTaskDelay(pdMS_TO_TICKS(OTA_BOOT_DELAY_MS));

    while (true) {
        char remote_version[32] = {};
        char bin_url[256] = {};

        if (!fetch_manifest(g_manifest_url,
                            remote_version, sizeof(remote_version),
                            bin_url, sizeof(bin_url))) {
            ESP_LOGI(TAG, "no manifest this cycle; sleeping");
            vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
            continue;
        }

        const char *running = ota_update::current_version();
        if (strcmp(remote_version, running) == 0) {
            ESP_LOGI(TAG, "version unchanged (%s); sleeping", running);
            vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
            continue;
        }
        ESP_LOGI(TAG, "update available: %s → %s", running, remote_version);

        if (perform_update(bin_url)) {
            ESP_LOGI(TAG, "update staged; signalling main to reboot at idle");
            g_reboot_ready = true;
            /* Done — main will reboot us. Park here forever. */
            while (true) vTaskDelay(pdMS_TO_TICKS(60000));
        }
        /* Update failed; back off a long time before retrying so a
         * persistently broken release doesn't hammer the network. */
        ESP_LOGW(TAG, "update failed; sleeping before retry");
        vTaskDelay(pdMS_TO_TICKS(OTA_CHECK_INTERVAL_MS));
    }
}

}  // namespace

namespace ota_update {

bool mark_valid_if_pending() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return false;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "pending OTA image promoted to valid");
            return true;
        }
        ESP_LOGW(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
        return false;
    }
    /* Any other state (VALID, NEW, INVALID, UNDEFINED for factory-style
     * boots) is fine; nothing to do. */
    return false;
}

void start_background_task(const char *manifest_url) {
    if (g_task_started) {
        ESP_LOGW(TAG, "task already started; ignoring");
        return;
    }
    if (!manifest_url || !manifest_url[0]) {
        ESP_LOGE(TAG, "manifest URL empty; not starting task");
        return;
    }
    g_manifest_url = manifest_url;
    /* 6 KB stack — TLS handshakes inside esp_https_ota use a lot of
     * mbedtls scratch space. Lower priority than the touch/render
     * loop (tskIDLE_PRIORITY+1) so OTA work never starves the UI. */
    BaseType_t ok = xTaskCreate(ota_task, "ota", 6 * 1024, nullptr,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(ota) failed");
        return;
    }
    g_task_started = true;
}

bool reboot_ready() {
    return g_reboot_ready;
}

const char *current_version() {
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}

}  // namespace ota_update
