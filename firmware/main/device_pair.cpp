#include "device_pair.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

namespace device_pair {

static const char *TAG = "device_pair";

#define PAIR_INIT_URL  "https://mochi.val.run/api/device/pair-init"
#define PAIR_CHECK_URL "https://mochi.val.run/api/device/pair-check?hw_id=%s"

/* Poll cadence and the small budget of "still waiting" log lines we
 * emit so a watching USB monitor knows we're alive without spamming
 * a debug-grade firehose. */
static constexpr int POLL_INTERVAL_MS = 5000;
static constexpr int LOG_EVERY_N_POLLS = 6;   /* every ~30 s */

/* Response-body buffer. Server returns short JSON (~120 bytes max).
 * 512 is comfortable headroom and still tiny. */
static constexpr size_t RESP_BUF = 512;

/* Per-fetch state for esp_http_client's event handler. */
struct fetch_ctx {
    char  *buf;
    size_t cap;
    size_t written;
    bool   overflow;
};

static esp_err_t on_event(esp_http_client_event_t *evt) {
    auto *ctx = static_cast<fetch_ctx *>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && !ctx->overflow) {
        size_t want = (size_t)evt->data_len;
        if (ctx->written + want >= ctx->cap) {
            ctx->overflow = true;
            return ESP_OK;
        }
        memcpy(ctx->buf + ctx->written, evt->data, want);
        ctx->written += want;
        ctx->buf[ctx->written] = 0;
    }
    return ESP_OK;
}

/* Format the device's WiFi base MAC as 12 lowercase hex chars
 * (no colons), matching the server's HW_ID_RE. */
static void format_hw_id(char *out, size_t out_len) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_len, "%02x%02x%02x%02x%02x%02x",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/*
 * Tiny ad-hoc JSON field extractor — finds "<key>":"<value>" in
 * `json` and copies the value into out. Returns true on success.
 * Adequate because the server responses are flat objects with
 * known keys and short string values, all on a trusted endpoint
 * (TLS-pinned via the cert bundle). For anything more complex we'd
 * pull in cJSON, but this is two routes returning four fields
 * total — not worth the bytes.
 */
static bool extract_str(const char *json, const char *key,
                        char *out, size_t out_len) {
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return false;
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += n;
    const char *q = strchr(p, '"');
    if (!q) return false;
    size_t len = (size_t)(q - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return true;
}

/* For booleans like "paired":true / "paired":false. */
static bool extract_bool(const char *json, const char *key, bool *out) {
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return false;
    const char *p = strstr(json, needle);
    if (!p) return false;
    p += n;
    while (*p == ' ') p++;
    if (strncmp(p, "true", 4) == 0)  { *out = true;  return true; }
    if (strncmp(p, "false", 5) == 0) { *out = false; return true; }
    return false;
}

/* ------------------------------------------------------------------ */

bool request_code(InitResult *out) {
    if (!out) return false;

    char hw_id[16];
    format_hw_id(hw_id, sizeof(hw_id));

    char body[64];
    int blen = snprintf(body, sizeof(body),
        "{\"hw_id\":\"%s\"}", hw_id);

    char buf[RESP_BUF] = {};
    fetch_ctx ctx = { buf, sizeof(buf), 0, false };

    esp_http_client_config_t cfg = {};
    cfg.url = PAIR_INIT_URL;
    cfg.method = HTTP_METHOD_POST;
    cfg.event_handler = on_event;
    cfg.user_data = &ctx;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { ESP_LOGE(TAG, "init failed"); return false; }
    esp_http_client_set_header(cli, "Content-Type", "application/json");
    esp_http_client_set_post_field(cli, body, blen);
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pair-init perform: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "pair-init HTTP %d body=%s", status, buf);
        return false;
    }
    if (!extract_str(buf, "code", out->code, sizeof(out->code))) {
        ESP_LOGE(TAG, "pair-init: no code in response: %s", buf);
        return false;
    }
    ESP_LOGI(TAG, "pair-init: hw_id=%s code=%s", hw_id, out->code);
    return true;
}

bool wait_for_user(const InitResult *init,
                   struct mochi_pair_creds *out,
                   int timeout_ms) {
    (void)init;  /* code reserved for future use (e.g. abort if mismatch) */
    if (!out) return false;

    char hw_id[16];
    format_hw_id(hw_id, sizeof(hw_id));

    char url[128];
    snprintf(url, sizeof(url), PAIR_CHECK_URL, hw_id);

    int64_t start_us = esp_timer_get_time();
    int polls = 0;

    while (true) {
        if (timeout_ms >= 0 &&
            (esp_timer_get_time() - start_us) / 1000 > timeout_ms) {
            ESP_LOGW(TAG, "pair-check timed out after %d ms", timeout_ms);
            return false;
        }

        char buf[RESP_BUF] = {};
        fetch_ctx ctx = { buf, sizeof(buf), 0, false };

        esp_http_client_config_t cfg = {};
        cfg.url = url;
        cfg.method = HTTP_METHOD_GET;
        cfg.event_handler = on_event;
        cfg.user_data = &ctx;
        cfg.timeout_ms = 10000;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;

        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (cli) {
            esp_err_t err = esp_http_client_perform(cli);
            int status = esp_http_client_get_status_code(cli);
            esp_http_client_cleanup(cli);

            if (err == ESP_OK && status == 200) {
                bool paired = false;
                if (extract_bool(buf, "paired", &paired) && paired) {
                    if (extract_str(buf, "pet_id",   out->pet_id,   sizeof(out->pet_id)) &&
                        extract_str(buf, "pet_name", out->pet_name, sizeof(out->pet_name))) {
                        ESP_LOGI(TAG, "paired! pet_id=%s name='%s'",
                            out->pet_id, out->pet_name);
                        return true;
                    }
                    ESP_LOGW(TAG, "paired:true but no pet fields: %s", buf);
                }
            } else if (err == ESP_OK && status == 410) {
                /* Server says our code expired. Caller should call
                 * request_code() again and refresh the screen. */
                ESP_LOGW(TAG, "pair-check 410 expired");
                return false;
            } else {
                ESP_LOGW(TAG, "pair-check transient: err=%s status=%d",
                    esp_err_to_name(err), status);
            }
        }

        polls++;
        if (polls % LOG_EVERY_N_POLLS == 0) {
            ESP_LOGI(TAG, "still waiting (poll %d, ~%d s)",
                polls, polls * POLL_INTERVAL_MS / 1000);
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

}  /* namespace device_pair */
