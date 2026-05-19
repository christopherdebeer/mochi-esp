#include "battery.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "battery";

/* ESP32-S3 ADC1 channel 3 = GPIO 4. The Waveshare board routes the
 * VBAT 1:2 divider here. ADC1 only — ADC2 conflicts with WiFi. */
static constexpr adc_channel_t BATT_CHAN = ADC_CHANNEL_3;

/* The divider halves VBAT before the ADC sees it. With 12 dB
 * attenuation the ADC's full-scale is ~3.3V; the divided voltage
 * fits well within that even at a fully charged LiPo (4.2V →
 * 2.1V at the ADC). */
static constexpr int VBAT_DIVIDER_NUM = 2;

static adc_oneshot_unit_handle_t s_adc = nullptr;
static adc_cali_handle_t s_cali = nullptr;
static bool s_inited = false;

bool battery_init(void) {
    if (s_inited) return true;

    adc_oneshot_unit_init_cfg_t init_cfg = {};
    init_cfg.unit_id = ADC_UNIT_1;
    if (adc_oneshot_new_unit(&init_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
        return false;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {};
    chan_cfg.atten = ADC_ATTEN_DB_12;     /* 0..~3.3V range */
    chan_cfg.bitwidth = ADC_BITWIDTH_12;  /* 0..4095 raw */
    if (adc_oneshot_config_channel(s_adc, BATT_CHAN, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed");
        return false;
    }

    /*
     * Curve-fitting calibration. The chip stores per-unit
     * calibration constants in eFuse; the curve-fitting scheme
     * applies a polynomial to map raw → mV. Without it readings
     * are off by ~50-100 mV across the range.
     */
    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = ADC_UNIT_1;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "curve-fitting cali unavailable; using raw scaling");
        s_cali = nullptr;
    }

    s_inited = true;
    ESP_LOGI(TAG, "ready (ADC1 ch3, 1:2 divider, curve-fit cal=%s)",
        s_cali ? "yes" : "no");
    return true;
}

/*
 * LiPo discharge curve (single-cell). Approximate, derived from
 * typical "knee" shape — LiPos sit near 4.0-4.2V when full, drop
 * sharply below 3.7V, near-empty around 3.3V. Linear-piecewise:
 *
 *   ≥ 4150 mV → 100 %
 *   3700 mV   →  50 %    (knee)
 *   3300 mV   →   0 %
 *
 * Below 3300 mV we clamp at 0; above 4150 we clamp at 100. The
 * piecewise fit isn't great science but it's plenty for a
 * status-bar percent display and matches what users intuit
 * (full near 4.0V, low at 3.5V).
 */
static uint8_t mv_to_pct(uint16_t mv) {
    if (mv >= 4150) return 100;
    if (mv <  3300) return 0;
    if (mv >= 3700) {
        /* 3700..4150 → 50..100 */
        return (uint8_t)(50 + (50 * (mv - 3700)) / (4150 - 3700));
    }
    /* 3300..3700 → 0..50 */
    return (uint8_t)((50 * (mv - 3300)) / (3700 - 3300));
}

bool battery_read(uint16_t *out_mv, uint8_t *out_pct) {
    if (!s_inited) return false;

    int raw = 0;
    if (adc_oneshot_read(s_adc, BATT_CHAN, &raw) != ESP_OK) {
        return false;
    }

    int adc_mv = 0;
    if (s_cali) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &adc_mv) != ESP_OK) {
            return false;
        }
    } else {
        /* Fallback: raw 0..4095 maps to ~0..3300 mV at 12dB atten. */
        adc_mv = (raw * 3300) / 4095;
    }

    /* Multiply back through the 1:2 divider. */
    uint16_t vbat_mv = (uint16_t)(adc_mv * VBAT_DIVIDER_NUM);
    if (out_mv)  *out_mv = vbat_mv;
    if (out_pct) *out_pct = mv_to_pct(vbat_mv);
    return true;
}
