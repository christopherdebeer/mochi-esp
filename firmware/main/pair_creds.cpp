#include "pair_creds.h"

#include <string.h>
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "pair_creds";
static const char *NS = "pair";
static const char *KEY_PET_ID   = "pet_id";
static const char *KEY_PET_NAME = "pet_name";

bool pair_creds_load(struct mochi_pair_creds *out) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t id_len = sizeof(out->pet_id);
    size_t nm_len = sizeof(out->pet_name);
    memset(out, 0, sizeof(*out));
    esp_err_t e1 = nvs_get_str(h, KEY_PET_ID,   out->pet_id,   &id_len);
    esp_err_t e2 = nvs_get_str(h, KEY_PET_NAME, out->pet_name, &nm_len);
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK) {
        ESP_LOGI(TAG, "no pairing in NVS");
        return false;
    }
    ESP_LOGI(TAG, "loaded pairing: pet_id=%s pet_name='%s'",
        out->pet_id, out->pet_name);
    return true;
}

bool pair_creds_save(const struct mochi_pair_creds *creds) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t e1 = nvs_set_str(h, KEY_PET_ID,   creds->pet_id);
    esp_err_t e2 = nvs_set_str(h, KEY_PET_NAME, creds->pet_name);
    esp_err_t ec = nvs_commit(h);
    nvs_close(h);
    if (e1 != ESP_OK || e2 != ESP_OK || ec != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %d %d %d", e1, e2, ec);
        return false;
    }
    ESP_LOGI(TAG, "saved pairing: pet_id=%s pet_name='%s'",
        creds->pet_id, creds->pet_name);
    return true;
}

bool pair_creds_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_erase_key(h, KEY_PET_ID);
    nvs_erase_key(h, KEY_PET_NAME);
    esp_err_t ec = nvs_commit(h);
    nvs_close(h);
    return ec == ESP_OK;
}
