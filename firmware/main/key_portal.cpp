#include "key_portal.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_netif.h"

#include "openai_key.h"
#include "epd_ui.h"

namespace key_portal {

static const char *TAG = "key_portal";

/* 5 minute auto-dismiss. If the user wanders off after triggering
 * the gesture, we don't want the server sitting open forever on
 * their LAN. */
static constexpr int64_t IDLE_TIMEOUT_US = 5LL * 60 * 1000 * 1000;

/* Post-submit "Saved!" linger before auto-stop. Long enough for the
 * user to see the success message on the EPD, short enough that
 * they don't sit watching it. */
static constexpr int64_t SUBMIT_LINGER_US = 2LL * 1000 * 1000;

static httpd_handle_t       s_server = nullptr;
static epaper_driver_display *s_epd = nullptr;
static int64_t              s_started_us = 0;
static int64_t              s_submit_at_us = 0;  /* 0 = not submitted */

/* Tiny single-input form. No JS, no styling — keeps the page small
 * (under 1 KB) so the device serves it without buffering issues
 * over a flaky LAN. */
static const char FORM_HTML[] =
    "<!doctype html>"
    "<html><head>"
    "<meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Mochi key</title>"
    "<style>"
    "body{font-family:system-ui;max-width:480px;margin:2em auto;padding:1em}"
    "input,button{font-size:1em;padding:0.5em;width:100%;box-sizing:border-box}"
    "label{display:block;margin:1em 0 0.3em}"
    "</style>"
    "</head><body>"
    "<h2>Mochi: set OpenAI key</h2>"
    "<p>Paste your <code>sk-...</code> key. It's stored on this "
    "device only and never leaves it.</p>"
    "<form method=POST action=/key>"
    "<label for=k>Key</label>"
    "<input id=k name=k type=password autocomplete=off required>"
    "<p><button type=submit>Save</button></p>"
    "</form>"
    "</body></html>";

static const char DONE_HTML[] =
    "<!doctype html>"
    "<html><head><meta charset=utf-8><title>Saved</title>"
    "<style>body{font-family:system-ui;text-align:center;margin-top:4em}</style>"
    "</head><body>"
    "<h2>Saved!</h2>"
    "<p>Your key is on the device. You can close this tab.</p>"
    "<p>Tap mochi to dismiss the QR screen.</p>"
    "</body></html>";

static const char ERR_HTML[] =
    "<!doctype html>"
    "<html><head><meta charset=utf-8><title>Error</title></head>"
    "<body><h2>Couldn't save</h2><p>Try again or factory-reset.</p></body></html>";

static esp_err_t handle_get_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, FORM_HTML, sizeof(FORM_HTML) - 1);
}

/* URL-decode a "k=...&..." form body in place. Returns pointer to
 * the value of "k", or null if absent. Mutates buf. */
static char *extract_key_value(char *buf) {
    /* Look for "k=" at the start or after a '&'. */
    char *p = buf;
    while (*p) {
        if ((p == buf || *(p - 1) == '&') && p[0] == 'k' && p[1] == '=') {
            char *val = p + 2;
            /* Truncate at the next '&'. */
            char *end = strchr(val, '&');
            if (end) *end = 0;
            /* In-place URL-decode: %xx → byte, '+' → ' '. */
            char *src = val, *dst = val;
            while (*src) {
                if (*src == '+') {
                    *dst++ = ' ';
                    src++;
                } else if (*src == '%' && src[1] && src[2]) {
                    char hex[3] = { src[1], src[2], 0 };
                    *dst++ = (char)strtol(hex, nullptr, 16);
                    src += 3;
                } else {
                    *dst++ = *src++;
                }
            }
            *dst = 0;
            return val;
        }
        p++;
    }
    return nullptr;
}

static esp_err_t handle_post_key(httpd_req_t *req) {
    /* +1 for our NUL, +16 slack so a key right at the limit doesn't
     * trip the "too large" branch on the trailing '&' nobody sent. */
    char buf[MOCHI_OPENAI_KEY_MAX + 32];
    if ((size_t)req->content_len >= sizeof(buf)) {
        ESP_LOGW(TAG, "POST body too large: %d", req->content_len);
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, ERR_HTML, sizeof(ERR_HTML) - 1);
        return ESP_OK;
    }
    int got = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (got <= 0) {
        ESP_LOGW(TAG, "POST recv failed: %d", got);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, ERR_HTML, sizeof(ERR_HTML) - 1);
        return ESP_OK;
    }
    buf[got] = 0;

    char *key = extract_key_value(buf);
    if (!key || !*key) {
        ESP_LOGW(TAG, "POST missing 'k' field");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, ERR_HTML, sizeof(ERR_HTML) - 1);
        return ESP_OK;
    }
    if (strlen(key) >= MOCHI_OPENAI_KEY_MAX) {
        ESP_LOGW(TAG, "key too long: %u", (unsigned)strlen(key));
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_send(req, ERR_HTML, sizeof(ERR_HTML) - 1);
        return ESP_OK;
    }

    bool ok = openai_key_save(key);
    /* Wipe the local copy now the NVS write is done. The buffer was
     * stack-allocated so it'll go out of scope shortly anyway, but
     * being explicit removes any window where a later stack reuse
     * might leave the bytes dangling. */
    memset(buf, 0, sizeof(buf));

    if (!ok) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_send(req, ERR_HTML, sizeof(ERR_HTML) - 1);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "key saved via portal");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, DONE_HTML, sizeof(DONE_HTML) - 1);

    /* Trigger the post-submit linger. tick() will pick this up and
     * call stop() once the timer expires. We render the success
     * screen here so the user sees it the instant the POST returns,
     * even if main's tick is paused on a long render. */
    if (s_epd) {
        epd_ui::clear(s_epd);
        epd_ui::draw_text_centered(s_epd, 50, 3, "Saved!");
        epd_ui::draw_text_centered(s_epd, 110, 1, "Voice ready.");
        epd_ui::draw_text_centered(s_epd, 130, 1, "Long-press to talk.");
        s_epd->EPD_Init_Partial();
        s_epd->EPD_DisplayPart();
    }
    s_submit_at_us = esp_timer_get_time();
    return ESP_OK;
}

static void render_portal(const char *ip_str) {
    if (!s_epd) return;
    char url[40];
    snprintf(url, sizeof(url), "http://%s/", ip_str);
    epd_ui::render_key_portal(s_epd, ip_str, url);
    s_epd->EPD_Init();
    s_epd->EPD_Display();
    /* Seed partial-refresh base so the success screen is partial. */
    s_epd->EPD_DisplayPartBaseImage();
}

static void resolve_ip(char out[16]) {
    out[0] = 0;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return;
    snprintf(out, 16, IPSTR, IP2STR(&info.ip));
}

void start(epaper_driver_display *epd) {
    if (s_server) {
        ESP_LOGI(TAG, "start() called while already active — no-op");
        return;
    }
    s_epd = epd;
    s_started_us = esp_timer_get_time();
    s_submit_at_us = 0;

    char ip[16];
    resolve_ip(ip);
    if (!ip[0]) {
        ESP_LOGW(TAG, "no STA IP yet — portal needs WiFi up first");
        return;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 4;
    cfg.stack_size = 6 * 1024;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = nullptr;
        return;
    }
    httpd_uri_t get_root = {
        .uri = "/", .method = HTTP_GET, .handler = handle_get_root,
        .user_ctx = nullptr,
    };
    httpd_uri_t post_key = {
        .uri = "/key", .method = HTTP_POST, .handler = handle_post_key,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(s_server, &get_root);
    httpd_register_uri_handler(s_server, &post_key);

    ESP_LOGI(TAG, "portal up at http://%s/", ip);
    render_portal(ip);
}

void stop() {
    if (!s_server) return;
    ESP_LOGI(TAG, "portal stopping");
    httpd_stop(s_server);
    s_server = nullptr;
    s_submit_at_us = 0;
}

bool active() {
    return s_server != nullptr;
}

void tick() {
    if (!s_server) return;
    int64_t now = esp_timer_get_time();

    if (s_submit_at_us != 0 &&
        (now - s_submit_at_us) >= SUBMIT_LINGER_US) {
        ESP_LOGI(TAG, "submit linger elapsed — stopping");
        stop();
        return;
    }
    if ((now - s_started_us) >= IDLE_TIMEOUT_US) {
        ESP_LOGI(TAG, "idle timeout — stopping");
        stop();
        return;
    }
}

}  /* namespace key_portal */
