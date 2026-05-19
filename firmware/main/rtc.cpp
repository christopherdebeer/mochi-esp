#include "rtc.h"

#include <string.h>
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_pins.h"
#include "i2c_bus.h"

static const char *TAG = "rtc";

#define PCF85063_ADDR    0x51

/* Register map (subset we care about). */
#define REG_CONTROL_1    0x00
#define REG_SECONDS      0x04   /* bit 7 = OSC stop flag */
#define REG_MINUTES      0x05
#define REG_HOURS        0x06
#define REG_DAYS         0x07
#define REG_WEEKDAYS     0x08
#define REG_MONTHS       0x09
#define REG_YEARS        0x0A

#define OSC_STOP_FLAG    0x80   /* in REG_SECONDS bit 7 */

static i2c_master_dev_handle_t s_dev = nullptr;
static bool s_inited = false;
static bool s_lost_power = false;

/* BCD helpers — PCF85063 stores values as packed BCD in lo/hi
 * nibbles. Range is naturally bounded by the hardware so we don't
 * defensively check the inputs here. */
static uint8_t bin_to_bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }
static uint8_t bcd_to_bin(uint8_t v) { return ((v >> 4) * 10) + (v & 0x0F); }

static bool write_reg(uint8_t reg, uint8_t v) {
    uint8_t buf[2] = { reg, v };
    return i2c_master_transmit(s_dev, buf, 2, 100) == ESP_OK;
}

static bool read_regs(uint8_t reg, uint8_t *buf, size_t len) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 100) == ESP_OK;
}

bool rtc_init(void) {
    if (s_inited) return true;

    auto bus_handle = i2c_bus::handle();
    if (!bus_handle) return false;

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = PCF85063_ADDR;
    dev_cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed");
        return false;
    }

    /* Read seconds register first to inspect the OSC-stop flag. If
     * it's set the oscillator was halted (e.g. coin cell missing),
     * which means the time we read until the next rtc_set is bogus. */
    uint8_t sec = 0;
    if (!read_regs(REG_SECONDS, &sec, 1)) {
        ESP_LOGE(TAG, "initial seconds read failed");
        return false;
    }
    s_lost_power = (sec & OSC_STOP_FLAG) != 0;
    if (s_lost_power) {
        ESP_LOGW(TAG, "OSC stop flag set — clock lost power; need rtc_set");
    }

    /* Make sure CONTROL_1 is in the expected state (24h mode, normal
     * STOP=0). The chip defaults to this on power-on but a previous
     * weird write or coin-cell glitch could have left STOP=1, which
     * would freeze the counters. Force the bit clear. */
    if (!write_reg(REG_CONTROL_1, 0x00)) {
        ESP_LOGE(TAG, "control_1 write failed");
        return false;
    }

    s_inited = true;
    ESP_LOGI(TAG, "ready (addr=0x%02X, lost_power=%d)",
        PCF85063_ADDR, s_lost_power ? 1 : 0);
    return true;
}

bool rtc_get(struct mochi_datetime *out) {
    if (!s_inited || !out) return false;
    uint8_t buf[7] = {};
    if (!read_regs(REG_SECONDS, buf, sizeof(buf))) return false;

    /* Mask off the status bits in seconds (bit 7) and other reserved
     * bits per the datasheet. Years are BCD 0..99 which we map onto
     * 2000..2099 — fine for the lifetime of this device. */
    out->second  = bcd_to_bin(buf[0] & 0x7F);
    out->minute  = bcd_to_bin(buf[1] & 0x7F);
    out->hour    = bcd_to_bin(buf[2] & 0x3F);
    out->day     = bcd_to_bin(buf[3] & 0x3F);
    out->weekday = buf[4] & 0x07;
    out->month   = bcd_to_bin(buf[5] & 0x1F);
    out->year    = 2000 + bcd_to_bin(buf[6]);
    return true;
}

bool rtc_set(const struct mochi_datetime *in) {
    if (!s_inited || !in) return false;
    if (in->year < 2000 || in->year > 2099) return false;

    /*
     * Write all seven time registers in one transaction. The chip
     * supports auto-increment, so a single transmit of [reg, ...7 bytes]
     * walks 0x04..0x0A in order. This is also how we clear the
     * OSC-stop flag — by writing seconds with bit 7 = 0.
     */
    uint8_t buf[8] = {
        REG_SECONDS,
        (uint8_t)(bin_to_bcd(in->second) & 0x7F),   /* clears OSC stop flag */
        (uint8_t)(bin_to_bcd(in->minute) & 0x7F),
        (uint8_t)(bin_to_bcd(in->hour)   & 0x3F),
        (uint8_t)(bin_to_bcd(in->day)    & 0x3F),
        (uint8_t)(in->weekday & 0x07),
        (uint8_t)(bin_to_bcd(in->month)  & 0x1F),
        bin_to_bcd((uint8_t)(in->year - 2000)),
    };
    if (i2c_master_transmit(s_dev, buf, sizeof(buf), 100) != ESP_OK) {
        ESP_LOGE(TAG, "rtc_set transmit failed");
        return false;
    }
    s_lost_power = false;
    return true;
}

bool rtc_lost_power(void) { return s_lost_power; }
