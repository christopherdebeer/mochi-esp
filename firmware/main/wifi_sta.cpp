#include "wifi_sta.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

namespace wifi_sta {

static const char *TAG = "wifi_sta";

static constexpr int BIT_GOT_IP = (1 << 0);
static constexpr int BIT_FAIL   = (1 << 1);

static EventGroupHandle_t s_events = nullptr;
static esp_netif_t *s_netif = nullptr;
static int s_retry = 0;
static constexpr int MAX_RETRY = 8;
static char s_ip[16] = {};
static bool s_inited = false;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "disconnect; retry %d/%d", s_retry, MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, BIT_FAIL);
        }
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "got IP %s", s_ip);
        s_retry = 0;
        xEventGroupSetBits(s_events, BIT_GOT_IP);
    }
}

void init_stack(void) {
    if (s_inited) return;
    s_inited = true;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
        on_wifi, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
        on_ip, nullptr, nullptr);

    s_events = xEventGroupCreate();
}

/* Bring up the STA netif + start the WiFi driver in STA mode if we
 * haven't already. Subsequent calls are no-ops (or just reuse the
 * existing netif/driver, which is what we want — multi-attempt joins
 * must not tear down the netif between tries). */
static void sta_stack_up(void) {
    if (!s_events) {
        s_events = xEventGroupCreate();
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
            on_wifi, nullptr, nullptr);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
            on_ip, nullptr, nullptr);
    }
    if (!s_netif) {
        s_netif = esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
}

/* Single-cred join attempt with a fixed timeout. Caller has already
 * called sta_stack_up(). Returns true on IP-acquired. */
static bool try_one(const struct mochi_wifi_creds *creds,
                    char *ip_str, size_t ip_len,
                    int timeout_ms) {
    s_retry = 0;
    s_ip[0] = 0;
    xEventGroupClearBits(s_events, BIT_GOT_IP | BIT_FAIL);

    wifi_config_t wcfg = {};
    strncpy((char *)wcfg.sta.ssid, creds->ssid, sizeof(wcfg.sta.ssid));
    strncpy((char *)wcfg.sta.password, creds->password,
            sizeof(wcfg.sta.password));
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wcfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(s_events,
        BIT_GOT_IP | BIT_FAIL, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & BIT_GOT_IP) {
        strncpy(ip_str, s_ip, ip_len);
        return true;
    }
    /* Make sure the driver is in a clean disconnected state for the
     * next attempt. */
    esp_wifi_disconnect();
    ip_str[0] = 0;
    return false;
}

bool connect(const struct mochi_wifi_creds *creds,
             char *ip_str, size_t ip_len) {
    sta_stack_up();
    return try_one(creds, ip_str, ip_len, 30000);
}

bool connect_any(char *ip_str, size_t ip_len,
                 char *out_ssid, size_t out_ssid_len) {
    sta_stack_up();
    if (out_ssid && out_ssid_len > 0) out_ssid[0] = 0;
    ip_str[0] = 0;

    size_t n = nvs_creds_count();
    if (n == 0) {
        ESP_LOGW(TAG, "connect_any: no stored creds");
        return false;
    }

    /* Active scan, ~120 ms per channel by default = ~1.5 s on 13
     * channels. Quick enough to do every boot without it being
     * felt in the warm-boot UX. */
    wifi_scan_config_t scan = {};
    scan.show_hidden = false;
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed; trying stored creds blind in MRU order");
        for (size_t i = 0; i < n; i++) {
            struct mochi_wifi_creds c = {};
            if (!nvs_creds_load_at(i, &c)) continue;
            ESP_LOGI(TAG, "blind try [%u] '%s'", (unsigned)i, c.ssid);
            if (try_one(&c, ip_str, ip_len, 15000)) {
                if (out_ssid && out_ssid_len > 0) {
                    strncpy(out_ssid, c.ssid, out_ssid_len - 1);
                    out_ssid[out_ssid_len - 1] = 0;
                }
                return true;
            }
        }
        return false;
    }

    uint16_t ap_n = 0;
    esp_wifi_scan_get_ap_num(&ap_n);
    ESP_LOGI(TAG, "scan: %u APs visible", (unsigned)ap_n);

    /* Cap to keep the on-stack array bounded. 32 is plenty for any
     * realistic environment; truncation just means we skip the
     * weakest tail. */
    constexpr int MAX_APS = 32;
    wifi_ap_record_t aps[MAX_APS] = {};
    uint16_t want = ap_n > MAX_APS ? MAX_APS : ap_n;
    if (want > 0) {
        esp_wifi_scan_get_ap_records(&want, aps);
    }

    /* Walk APs in scan order — esp_wifi_scan_get_ap_records returns
     * them sorted by RSSI desc. For each AP, find the matching
     * stored cred (if any) and try it. Skip already-tried entries
     * via a small bitmap. */
    uint32_t tried = 0;
    for (int a = 0; a < (int)want; a++) {
        const char *visible = (const char *)aps[a].ssid;
        if (visible[0] == 0) continue;
        for (size_t i = 0; i < n; i++) {
            if (tried & (1u << i)) continue;
            struct mochi_wifi_creds c = {};
            if (!nvs_creds_load_at(i, &c)) continue;
            if (strncmp(c.ssid, visible, MOCHI_WIFI_SSID_MAX) != 0) continue;
            ESP_LOGI(TAG, "match [%u] '%s' (rssi=%d) — trying",
                (unsigned)i, c.ssid, (int)aps[a].rssi);
            tried |= (1u << i);
            if (try_one(&c, ip_str, ip_len, 15000)) {
                if (out_ssid && out_ssid_len > 0) {
                    strncpy(out_ssid, c.ssid, out_ssid_len - 1);
                    out_ssid[out_ssid_len - 1] = 0;
                }
                return true;
            }
            ESP_LOGW(TAG, "join '%s' failed; continuing", c.ssid);
            break;  /* next AP */
        }
    }

    /* Scan-walk found no match (or the matches all failed to join).
     * Blind-try the MRU credential before bailing: Apple Personal
     * Hotspot stops beaconing when no client is connected — the exact
     * state we land in after a post-provisioning reboot — so the SSID
     * never appears in the scan, but esp_wifi_connect's directed probe
     * will still find it. Only the MRU is worth the extra 15s; trying
     * every stored cred blind would balloon the bail-to-prov path. */
    if (!(tried & 1u)) {
        struct mochi_wifi_creds c = {};
        if (nvs_creds_load_at(0, &c)) {
            ESP_LOGI(TAG, "blind try MRU (not in scan): '%s'", c.ssid);
            if (try_one(&c, ip_str, ip_len, 15000)) {
                if (out_ssid && out_ssid_len > 0) {
                    strncpy(out_ssid, c.ssid, out_ssid_len - 1);
                    out_ssid[out_ssid_len - 1] = 0;
                }
                return true;
            }
            ESP_LOGW(TAG, "blind MRU join failed");
        }
    }

    ESP_LOGW(TAG, "connect_any: no stored network reachable");
    return false;
}

}  /* namespace wifi_sta */
