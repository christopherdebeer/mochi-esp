#include "wifi_prov.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include "epd_ui.h"
#include "nvs_creds.h"

namespace wifi_prov {

static const char *TAG = "wifi_prov";

/* Event group bits for signalling between the HTTP/Wi-Fi callbacks

 * and the run() loop. */
static constexpr int BIT_CREDS_RECEIVED = (1 << 0);

struct State {
    EventGroupHandle_t events = nullptr;
    epaper_driver_display *epd = nullptr;
    httpd_handle_t httpd = nullptr;
    esp_netif_t *ap_netif = nullptr;
    TaskHandle_t dns_task = nullptr;
    int dns_sock = -1;
    /* Buffer the submitted creds between POST handler and main
     * thread. Plain global is fine — only one provisioning attempt
     * is in flight at a time. */
    struct mochi_wifi_creds pending_creds = {};
    char pending_openai_key[MOCHI_OPENAI_KEY_MAX + 1] = {};
    char ap_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};
};

static State g;

/* -------------------------------------------------------------- */
/* Helpers                                                          */
/* -------------------------------------------------------------- */

void make_softap_ssid(char *out_buf, size_t buf_len) {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out_buf, buf_len, "Mochi-%02X%02X",
             mac[4], mac[5]);
}

/* URL decode in place. Handles %xx and '+'. Truncates to dst_len-1. */
static void url_decode(const char *src, char *dst, size_t dst_len) {
    size_t di = 0;
    while (*src && di + 1 < dst_len) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[di++] = (char)strtol(hex, nullptr, 16);
            src += 3;
        } else if (*src == '+') {
            dst[di++] = ' ';
            src++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = 0;
}

/* Find a key=value pair in a urlencoded body. Returns true if found.
 * tmp[] sized for the longest field we accept (the OpenAI API key,
 * MOCHI_OPENAI_KEY_MAX = 256). */
static bool form_get(const char *body, const char *key,
                     char *out, size_t out_len) {
    size_t klen = strlen(key);
    const char *p = body;
    while (*p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = strchr(v, '&');
            size_t vlen = end ? (size_t)(end - v) : strlen(v);
            char tmp[MOCHI_OPENAI_KEY_MAX + 1];
            if (vlen >= sizeof(tmp)) vlen = sizeof(tmp) - 1;
            memcpy(tmp, v, vlen);
            tmp[vlen] = 0;
            url_decode(tmp, out, out_len);
            return true;
        }
        const char *amp = strchr(p, '&');
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

/* -------------------------------------------------------------- */
/* DNS hijack — returns A=192.168.4.1 for every query.             */
/* -------------------------------------------------------------- */

static void dns_task(void *arg) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        ESP_LOGE(TAG, "dns: socket() failed");
        vTaskDelete(nullptr);
        return;
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(53);
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns: bind() failed");
        close(s);
        vTaskDelete(nullptr);
        return;
    }
    g.dns_sock = s;
    ESP_LOGI(TAG, "dns: hijack listening on :53");

    /* 192.168.4.1 in network byte order. */
    const uint32_t hijack_ip = htonl((192 << 24) | (168 << 16) | (4 << 8) | 1);

    uint8_t buf[512];
    while (true) {
        struct sockaddr_in src = {};
        socklen_t slen = sizeof(src);
        int n = recvfrom(s, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 12) continue;          /* shorter than DNS header */

        /*
         * Build response in place. DNS header is 12 bytes:
         *   ID (2), flags (2), QD/AN/NS/AR counts (2 each).
         * Set QR=1 (response), RA=1, RCODE=0; ANCOUNT=1.
         * Then echo the question section, then append:
         *   NAME = pointer 0xC00C (offset to question name)
         *   TYPE = A (1), CLASS = IN (1), TTL = 60s, RDLENGTH = 4,
         *   RDATA = 192.168.4.1
         */
        buf[2] = 0x81;   /* QR=1, OPCODE=0, RD=1 (echoed), TC=0 */
        buf[3] = 0x80;   /* RA=1, Z=0, RCODE=0 */
        buf[6] = 0x00; buf[7] = 0x01;  /* ANCOUNT = 1 */
        buf[8] = 0x00; buf[9] = 0x00;  /* NSCOUNT = 0 */
        buf[10] = 0x00; buf[11] = 0x00; /* ARCOUNT = 0 */

        if ((size_t)n + 16 > sizeof(buf)) continue;
        uint8_t *ans = buf + n;
        ans[0] = 0xC0; ans[1] = 0x0C;       /* NAME pointer */
        ans[2] = 0x00; ans[3] = 0x01;       /* TYPE = A */
        ans[4] = 0x00; ans[5] = 0x01;       /* CLASS = IN */
        ans[6] = 0x00; ans[7] = 0x00;       /* TTL = 60 (high) */
        ans[8] = 0x00; ans[9] = 0x3C;       /* TTL = 60 (low) */
        ans[10] = 0x00; ans[11] = 0x04;     /* RDLENGTH = 4 */
        memcpy(ans + 12, &hijack_ip, 4);

        sendto(s, buf, n + 16, 0, (struct sockaddr *)&src, slen);
    }
}

/* -------------------------------------------------------------- */
/* HTTP handlers                                                    */
/* -------------------------------------------------------------- */

static esp_err_t handler_redirect_to_portal(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/portal");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

/*
 * Apple's hotspot-detect URL is fingerprinted: if the body matches
 * the magic string, iOS considers the network "online" and does NOT
 * pop the captive portal. We deliberately return a 302 instead so
 * iOS sees "captive" and auto-launches the browser.
 */
static esp_err_t handler_apple_probe(httpd_req_t *req) {
    return handler_redirect_to_portal(req);
}

/* Android probes connectivitycheck.gstatic.com with a request that
 * expects HTTP 204. Returning anything else (we send 302) marks the
 * network as captive and triggers portal auto-launch. */
static esp_err_t handler_android_probe(httpd_req_t *req) {
    return handler_redirect_to_portal(req);
}

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Mochi setup</title>"
    "<style>"
    "body{font-family:-apple-system,system-ui,sans-serif;"
    "max-width:480px;margin:2em auto;padding:0 1em;color:#222}"
    "h1{font-size:1.4em;margin:0 0 0.4em}"
    "p{color:#666;margin:0.4em 0 1.4em}"
    "label{display:block;margin:1em 0 0.3em;font-weight:600}"
    "input,select{width:100%;box-sizing:border-box;padding:0.7em;"
    "font-size:1em;border:1px solid #ccc;border-radius:6px}"
    "button{margin-top:1.4em;width:100%;padding:0.9em;"
    "background:#222;color:#fff;border:0;border-radius:6px;"
    "font-size:1em}"
    "</style></head><body>"
    "<h1>Hi! I'm Mochi.</h1>"
    "<p>Pick the WiFi network and paste your OpenAI API key. "
    "The key stays on this device and only ever talks to "
    "api.openai.com — mochi.val.run never sees it.</p>"
    "<form method='POST' action='/portal'>"
    "<label for=ssid>Network name</label>"
    "<input id=ssid name=ssid required maxlength=32 "
    "placeholder='e.g. Home_5G'>"
    "<label for=pass>Password</label>"
    "<input id=pass name=pass type=password maxlength=64>"
    "<label for=key>OpenAI API key</label>"
    "<input id=key name=key type=password maxlength=256 "
    "placeholder='sk-…' autocomplete=off spellcheck=false>"
    "<label for=tz>Timezone</label>"
    "<select id=tz name=tz>"
    "<option value='GMT0BST,M3.5.0/1,M10.5.0' selected>UK (London)</option>"
    "<option value='CET-1CEST,M3.5.0,M10.5.0/3'>Central Europe (Paris/Berlin)</option>"
    "<option value='EET-2EEST,M3.5.0/3,M10.5.0/4'>Eastern Europe (Athens/Helsinki)</option>"
    "<option value='EST5EDT,M3.2.0,M11.1.0'>US Eastern (NYC)</option>"
    "<option value='CST6CDT,M3.2.0,M11.1.0'>US Central (Chicago)</option>"
    "<option value='MST7MDT,M3.2.0,M11.1.0'>US Mountain (Denver)</option>"
    "<option value='PST8PDT,M3.2.0,M11.1.0'>US Pacific (LA/SF)</option>"
    "<option value='UTC0'>UTC (no DST)</option>"
    "</select>"
    "<button type=submit>Connect</button>"
    "</form></body></html>";

static esp_err_t handler_portal_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, sizeof(PORTAL_HTML) - 1);
    return ESP_OK;
}

static esp_err_t handler_portal_post(httpd_req_t *req) {
    /* SSID (32) + pass (64) + key (256) + url-encoding overhead +
     * &-separators + key names. 768 is comfortable; httpd default
     * recv timeout is fine for a single form POST. */
    char body[768];
    int total = req->content_len > (int)sizeof(body) - 1
                ? (int)sizeof(body) - 1
                : req->content_len;
    int read = httpd_req_recv(req, body, total);
    if (read <= 0) return ESP_FAIL;
    body[read] = 0;

    char ssid[MOCHI_WIFI_SSID_MAX + 1] = {};
    char pass[MOCHI_WIFI_PASS_MAX + 1] = {};
    char key[MOCHI_OPENAI_KEY_MAX + 1] = {};
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) ||
        strlen(ssid) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "missing ssid");
        return ESP_FAIL;
    }
    form_get(body, "pass", pass, sizeof(pass));   /* optional */
    form_get(body, "key",  key,  sizeof(key));    /* optional, surfaces at first voice session */

    /* Capture the timezone selection. Optional: if the field isn't
     * in the body (older browsers, hand-crafted POST), we leave NVS
     * untouched and time_sync falls back to its compiled default. */
    char tz[MOCHI_TZ_MAX] = {};
    form_get(body, "tz", tz, sizeof(tz));
    if (tz[0]) {
        nvs_creds_set_tz(tz);
        ESP_LOGI(TAG, "portal: tz='%s' persisted", tz);
    }

    strncpy(g.pending_creds.ssid, ssid, sizeof(g.pending_creds.ssid));
    strncpy(g.pending_creds.password, pass, sizeof(g.pending_creds.password));
    strncpy(g.pending_openai_key, key, sizeof(g.pending_openai_key));
    ESP_LOGI(TAG, "portal: received SSID '%s', key (len=%u)",
        ssid, (unsigned)strlen(key));

    /* Acknowledge the form so the user's browser shows something,
     * then signal the main thread to try the creds. */
    const char *ack =
        "<html><body style='font-family:sans-serif;max-width:480px;"
        "margin:3em auto;padding:0 1em'>"
        "<h2>Got it.</h2><p>Mochi is trying your WiFi. "
        "Watch the e-paper screen.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, ack, strlen(ack));

    xEventGroupSetBits(g.events, BIT_CREDS_RECEIVED);
    return ESP_OK;
}

static httpd_uri_t URI_ROOT      = { "/",                       HTTP_GET,  handler_redirect_to_portal,  nullptr };
static httpd_uri_t URI_APPLE     = { "/hotspot-detect.html",    HTTP_GET,  handler_apple_probe,         nullptr };
static httpd_uri_t URI_GENERATE  = { "/generate_204",           HTTP_GET,  handler_android_probe,       nullptr };
static httpd_uri_t URI_GCONNECT  = { "/gen_204",                HTTP_GET,  handler_android_probe,       nullptr };
static httpd_uri_t URI_PORTAL_G  = { "/portal",                 HTTP_GET,  handler_portal_get,          nullptr };
static httpd_uri_t URI_PORTAL_P  = { "/portal",                 HTTP_POST, handler_portal_post,         nullptr };

static httpd_handle_t httpd_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 12;
    httpd_handle_t s = nullptr;
    if (httpd_start(&s, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return nullptr;
    }
    httpd_register_uri_handler(s, &URI_ROOT);
    httpd_register_uri_handler(s, &URI_APPLE);
    httpd_register_uri_handler(s, &URI_GENERATE);
    httpd_register_uri_handler(s, &URI_GCONNECT);
    httpd_register_uri_handler(s, &URI_PORTAL_G);
    httpd_register_uri_handler(s, &URI_PORTAL_P);
    return s;
}

/* -------------------------------------------------------------- */
/* Wi-Fi event handlers                                             */
/* -------------------------------------------------------------- */

static void wifi_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "softAP: client connected");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "softAP: client disconnected");
    }
    /* No STA path here — provisioning never enters STA. The post-
     * reboot already-provisioned branch in main.cpp + wifi_sta.cpp
     * handles all STA logic. */
}

/* -------------------------------------------------------------- */
/* Stack lifecycle                                                  */
/* -------------------------------------------------------------- */

static void start_softap(void) {
    g.ap_netif = esp_netif_create_default_wifi_ap();

    wifi_config_t cfg = {};
    strncpy((char *)cfg.ap.ssid, g.ap_ssid, sizeof(cfg.ap.ssid));
    cfg.ap.ssid_len = strlen(g.ap_ssid);
    cfg.ap.channel = 1;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_OPEN;
    cfg.ap.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "softAP up: SSID '%s'", g.ap_ssid);
}

/*
 * No tear-down: we never leave AP mode. esp_restart() in main.cpp
 * does a clean SoC reset which dismantles the WiFi stack reliably
 * (the kernel-level reset is far more dependable than ESP-IDF's
 * intra-process AP→STA hand-off in 5.3.x).
 */

/* -------------------------------------------------------------- */
/* Public entry point                                               */
/* -------------------------------------------------------------- */

bool run(epaper_driver_display *epd,
         struct mochi_wifi_creds *out_creds,
         char *out_key_buf,
         size_t out_key_buf_len) {
    g.epd = epd;
    g.events = xEventGroupCreate();
    make_softap_ssid(g.ap_ssid, sizeof(g.ap_ssid));
    ESP_LOGI(TAG, "starting provisioning, AP SSID '%s'", g.ap_ssid);

    /* TCP/IP + event loop are global one-shot inits. WiFi driver
     * init is also one-shot; mode/config get set per phase. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t winit = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&winit));
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        wifi_event_cb, nullptr, nullptr);

    /* --- AP phase --- */
    start_softap();
    g.httpd = httpd_start();
    xTaskCreate(dns_task, "dns_hijack", 4096, nullptr, 5, &g.dns_task);

    epd_ui::render_prov_idle(epd, g.ap_ssid);
    epd->EPD_Init();
    epd->EPD_Display();
    epd->EPD_DisplayPartBaseImage();

    ESP_LOGI(TAG, "waiting for portal submission...");
    xEventGroupWaitBits(g.events, BIT_CREDS_RECEIVED,
                        pdTRUE, pdFALSE, portMAX_DELAY);

    /*
     * Accept the creds and let the caller reboot. The original M3
     * implementation tried to validate creds here by tearing down
     * the AP and bringing up STA inline — that worked once but the
     * AP→STA hand-off (esp_netif_destroy_default_wifi + create
     * + esp_wifi_set_mode) deadlocks intermittently on ESP-IDF v5.3
     * with no obvious cause. We bailed on inline validation and
     * adopted the same shape M5 pairing uses: persist + reboot.
     *
     * If the user typed a wrong password, the post-reboot STA path
     * fails its retries → main.cpp wipes NVS → we re-enter
     * provisioning. Slower than inline validation by one reboot
     * cycle (~5s) but robust. See project memory
     * project-eink-wifi-handover.
     */
    epd_ui::render_prov_connecting(epd);
    epd->EPD_Init_Partial();
    epd->EPD_DisplayPart();

    *out_creds = g.pending_creds;
    if (out_key_buf && out_key_buf_len > 0) {
        strncpy(out_key_buf, g.pending_openai_key, out_key_buf_len - 1);
        out_key_buf[out_key_buf_len - 1] = 0;
    }
    /* Wipe the in-memory copy of the key once it's been handed off
     * to the caller. NVS becomes the only place it lives. */
    memset(g.pending_openai_key, 0, sizeof(g.pending_openai_key));
    ESP_LOGI(TAG, "provisioning: creds + key captured, returning to main");
    return true;
}

}  /* namespace wifi_prov */
