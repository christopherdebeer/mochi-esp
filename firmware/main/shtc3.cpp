#include "shtc3.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_pins.h"
#include "i2c_bus.h"

static const char *TAG = "shtc3";

#define SHTC3_ADDR     0x70

/* All commands are 16-bit big-endian writes. */
#define CMD_WAKE       0x3517
#define CMD_SLEEP      0xB098
#define CMD_RESET      0x805D
#define CMD_READ_ID    0xEFC8
/* Measurement command. Two variants exist:
 *   0x7CA2 = clock-stretching, chip holds SCL low until ready
 *   0x7866 = polling, chip NACKs read-back attempts until ready
 * The clock-stretching variant is cleaner in principle but ESP-IDF
 * v5's i2c_master defaults to a stingy stretch-tolerance that's
 * shorter than the SHTC3's ~12 ms measurement time, producing
 * ESP_ERR_INVALID_STATE timeouts. We use the polling variant
 * instead: send the command, wait long enough for the measurement
 * to finish (~15 ms is the documented worst case for full
 * resolution), then issue a read. No bus-config tuning needed and
 * the call site only differs in adding a vTaskDelay between write
 * and read. */
#define CMD_MEAS_TR_POLLING 0x7866

/* CRC-8 polynomial 0x131 = x^8 + x^5 + x^4 + 1, init 0xFF. Per
 * Sensirion datasheet §4.4. */
static uint8_t crc8(const uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static i2c_master_dev_handle_t s_dev = nullptr;
static bool s_inited = false;

static bool send_cmd(uint16_t cmd) {
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return i2c_master_transmit(s_dev, buf, 2, 100) == ESP_OK;
}

bool shtc3_init(void) {
    if (s_inited) return true;

    auto bus_handle = i2c_bus::handle();
    if (!bus_handle) return false;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = SHTC3_ADDR;
    dev_cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed");
        return false;
    }

    /*
     * Wake → soft-reset → check ID. The ID register is one of the
     * few that's directly readable; lower 6 bits should be
     * 0b000111 (0x07) for the SHTC3 specifically. We log it but
     * don't gate init on it — our probe in touch::init already
     * confirmed the device ACKs at 0x70.
     */
    if (!send_cmd(CMD_WAKE)) { ESP_LOGE(TAG, "wake failed"); return false; }
    vTaskDelay(pdMS_TO_TICKS(1));    /* tWAKE = 240 µs typ */
    if (!send_cmd(CMD_RESET)) { ESP_LOGE(TAG, "reset failed"); return false; }
    vTaskDelay(pdMS_TO_TICKS(1));

    uint8_t id_cmd[2] = { (uint8_t)(CMD_READ_ID >> 8), (uint8_t)(CMD_READ_ID & 0xFF) };
    uint8_t id_buf[3] = {};
    if (i2c_master_transmit_receive(s_dev, id_cmd, 2, id_buf, 3, 100) != ESP_OK) {
        ESP_LOGW(TAG, "id read failed");
    } else if (crc8(id_buf, 2) != id_buf[2]) {
        ESP_LOGW(TAG, "id CRC mismatch");
    } else {
        uint16_t id = (id_buf[0] << 8) | id_buf[1];
        ESP_LOGI(TAG, "id=0x%04X (low6=0x%02X, expect 0x07)",
            id, id & 0x3F);
    }

    /* Park the chip in sleep until the first measurement. */
    send_cmd(CMD_SLEEP);
    s_inited = true;
    return true;
}

bool shtc3_read(float *out_temp_c, float *out_rh_pct) {
    if (!s_inited || !out_temp_c || !out_rh_pct) return false;

    /* Wake. Datasheet says minimum 240 µs before next command;
     * pdMS_TO_TICKS(1) at 1 kHz tick rate is 1 ms — plenty. */
    if (!send_cmd(CMD_WAKE)) { ESP_LOGE(TAG, "wake failed"); return false; }
    vTaskDelay(pdMS_TO_TICKS(1));

    /* Issue measurement, wait for it to finish, then read 6 bytes.
     * Splitting write and read avoids the clock-stretch tolerance
     * problem (see CMD_MEAS_TR_POLLING comment above). The 15 ms
     * delay is the datasheet's worst-case measurement duration at
     * full resolution. */
    if (!send_cmd(CMD_MEAS_TR_POLLING)) {
        send_cmd(CMD_SLEEP);
        ESP_LOGE(TAG, "measure cmd failed");
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(15));

    uint8_t buf[6] = {};
    esp_err_t err = i2c_master_receive(s_dev, buf, sizeof(buf), 100);
    /* Best-effort sleep regardless of outcome — leaving the chip
     * awake costs ~600 µA. */
    send_cmd(CMD_SLEEP);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "measure read failed: %s", esp_err_to_name(err));
        return false;
    }

    /* Two CRC-8 checks: one over temp bytes, one over RH bytes. */
    if (crc8(&buf[0], 2) != buf[2]) {
        ESP_LOGW(TAG, "temp CRC mismatch");
        return false;
    }
    if (crc8(&buf[3], 2) != buf[5]) {
        ESP_LOGW(TAG, "rh CRC mismatch");
        return false;
    }

    uint16_t raw_t = (buf[0] << 8) | buf[1];
    uint16_t raw_h = (buf[3] << 8) | buf[4];

    /*
     * Datasheet conversion:
     *   T[°C] = -45 + 175 * raw_T / 2^16
     *   RH[%] = 100 * raw_H / 2^16
     * No self-heating subtraction here. Vendor sample subtracted
     * 4 °C unconditionally; we'll do that only if our readings
     * are systematically too high in real testing.
     */
    *out_temp_c = -45.0f + 175.0f * (float)raw_t / 65536.0f;
    *out_rh_pct = 100.0f * (float)raw_h / 65536.0f;
    return true;
}
