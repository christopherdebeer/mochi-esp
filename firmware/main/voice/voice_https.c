#include "voice_https.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

static const char *TAG = "voice_https";

/*
 * Body assembly. esp_http_client fires HTTP_EVENT_ON_DATA with chunks
 * of the response body; we accumulate them into a single contiguous
 * heap buffer so the upstream-style body callback sees the whole
 * thing in one shot. The buffer grows as needed; capped at 64 KB to
 * stop a misbehaving server from eating all our memory. The OpenAI
 * realtime endpoints return SDP answers (~1-3 KB) and a session-mint
 * JSON (~300 B), so this is generous.
 */
#define VOICE_HTTPS_MAX_BODY  (64 * 1024)

typedef struct {
    char *buf;
    int   len;
    int   cap;
    bool  overflow;
} body_acc_t;

static esp_err_t on_event(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_DATA) {
        return ESP_OK;
    }
    body_acc_t *acc = (body_acc_t *)evt->user_data;
    if (acc->overflow) {
        return ESP_OK;
    }
    int need = acc->len + evt->data_len + 1;  /* +1 for trailing NUL */
    if (need > VOICE_HTTPS_MAX_BODY) {
        ESP_LOGE(TAG, "body would exceed %d bytes; truncating",
            VOICE_HTTPS_MAX_BODY);
        acc->overflow = true;
        return ESP_OK;
    }
    if (need > acc->cap) {
        int new_cap = acc->cap ? acc->cap : 1024;
        while (new_cap < need) {
            new_cap *= 2;
        }
        if (new_cap > VOICE_HTTPS_MAX_BODY) {
            new_cap = VOICE_HTTPS_MAX_BODY;
        }
        char *grown = (char *)realloc(acc->buf, (size_t)new_cap);
        if (!grown) {
            ESP_LOGE(TAG, "realloc to %d failed", new_cap);
            acc->overflow = true;
            return ESP_OK;
        }
        acc->buf = grown;
        acc->cap = new_cap;
    }
    memcpy(acc->buf + acc->len, evt->data, (size_t)evt->data_len);
    acc->len += evt->data_len;
    acc->buf[acc->len] = 0;
    return ESP_OK;
}

/* Common header parser. headers[i] strings are temporarily mutated
 * (split on ':') so esp_http_client_set_header sees clean key + value;
 * we restore the colon afterwards so the caller's buffers come back
 * unchanged. */
static void apply_headers(esp_http_client_handle_t cli, char **headers) {
    if (!headers) return;
    for (int i = 0; headers[i]; i++) {
        char *line = headers[i];
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = 0;
        char *value = colon + 1;
        while (*value == ' ') value++;
        esp_http_client_set_header(cli, line, value);
        *colon = ':';
    }
}

int https_post(char *url, char **headers, char *data,
               http_body_t body, void *ctx) {
    if (!url) {
        return -1;
    }

    body_acc_t acc = { 0 };

    esp_http_client_config_t cfg = { 0 };
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.event_handler = on_event;
    cfg.user_data = &acc;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "init failed");
        return -1;
    }

    apply_headers(cli, headers);

    if (data) {
        esp_http_client_set_post_field(cli, data, (int)strlen(data));
    }

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);

    int rc = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "perform %s: %s", url, esp_err_to_name(err));
        rc = -1;
    } else if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP %d from %s", status, url);
        if (acc.len > 0) {
            ESP_LOGE(TAG, "error body: %.*s",
                acc.len > 512 ? 512 : acc.len, acc.buf);
        }
        rc = -1;
    } else if (acc.overflow) {
        rc = -1;
    } else if (body) {
        http_resp_t resp = { acc.buf, acc.len };
        body(&resp, ctx);
    }

    esp_http_client_cleanup(cli);
    free(acc.buf);
    return rc;
}

int https_get(char *url, char **headers,
              http_body_t body, void *ctx) {
    if (!url) return -1;

    body_acc_t acc = { 0 };

    esp_http_client_config_t cfg = { 0 };
    cfg.url = url;
    cfg.method = HTTP_METHOD_GET;
    cfg.event_handler = on_event;
    cfg.user_data = &acc;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "init failed");
        return -1;
    }

    apply_headers(cli, headers);

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);

    int rc = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "perform GET %s: %s", url, esp_err_to_name(err));
        rc = -1;
    } else if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP %d from %s", status, url);
        if (acc.len > 0) {
            ESP_LOGE(TAG, "error body: %.*s",
                acc.len > 256 ? 256 : acc.len, acc.buf);
        }
        rc = -1;
    } else if (acc.overflow) {
        rc = -1;
    } else if (body) {
        http_resp_t resp = { acc.buf, acc.len };
        body(&resp, ctx);
    }

    esp_http_client_cleanup(cli);
    free(acc.buf);
    return rc;
}
