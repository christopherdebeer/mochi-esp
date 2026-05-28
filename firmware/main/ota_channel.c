#include "ota_channel.h"

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "ota_channel";
static const char *NS  = "ota";
static const char *K_CHANNEL = "channel";

ota_channel_t ota_channel_get(void) {
    ota_channel_t ch = OTA_CHANNEL_STABLE;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ch;
    uint8_t v = 0;
    if (nvs_get_u8(h, K_CHANNEL, &v) == ESP_OK && v == OTA_CHANNEL_BETA) {
        ch = OTA_CHANNEL_BETA;
    }
    nvs_close(h);
    return ch;
}

void ota_channel_set(ota_channel_t ch) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e1 = nvs_set_u8(h, K_CHANNEL, (uint8_t)ch);
    esp_err_t ec = nvs_commit(h);
    nvs_close(h);
    if (e1 == ESP_OK && ec == ESP_OK) ESP_LOGI(TAG, "channel → %s", ota_channel_name(ch));
    else ESP_LOGW(TAG, "channel save failed: %d %d", e1, ec);
}

ota_channel_t ota_channel_toggle(void) {
    ota_channel_t next = (ota_channel_get() == OTA_CHANNEL_STABLE)
        ? OTA_CHANNEL_BETA : OTA_CHANNEL_STABLE;
    ota_channel_set(next);
    return next;
}

const char *ota_channel_name(ota_channel_t ch) {
    return ch == OTA_CHANNEL_BETA ? "beta" : "stable";
}
