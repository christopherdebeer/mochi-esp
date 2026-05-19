#include "openai_key.h"

#include <string.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "openai_key";
static const char *NS = "openai";
static const char *KEY_API_KEY = "api_key";

bool openai_key_load(char *out, size_t out_len) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = out_len;
    memset(out, 0, out_len);
    esp_err_t err = nvs_get_str(h, KEY_API_KEY, out, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no openai key in NVS");
        return false;
    }
    /* Don't log the key value itself — log a length-and-prefix
     * digest so debug output stays useful without leaking secrets. */
    ESP_LOGI(TAG, "loaded openai key (len=%u, prefix='%c%c%c%c…')",
        (unsigned)strlen(out),
        out[0], out[1], out[2], out[3]);
    return true;
}

bool openai_key_save(const char *key) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e1 = nvs_set_str(h, KEY_API_KEY, key);
    esp_err_t ec = nvs_commit(h);
    nvs_close(h);
    if (e1 != ESP_OK || ec != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %d %d", e1, ec);
        return false;
    }
    ESP_LOGI(TAG, "saved openai key (len=%u)", (unsigned)strlen(key));
    return true;
}

bool openai_key_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_erase_key(h, KEY_API_KEY);
    esp_err_t ec = nvs_commit(h);
    nvs_close(h);
    return ec == ESP_OK;
}
