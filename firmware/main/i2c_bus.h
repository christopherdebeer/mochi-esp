/*
 * Project I²C bus singleton.
 *
 * One physical bus serves five peripherals: FT6336 touch (0x38),
 * PCF85063 RTC (0x51), SHTC3 temp/humidity (0x70), and the ES8311
 * audio codec (0x18) plus its connection through codec_board's own
 * driver. ESP-IDF v5.3's i2c_master driver allows only one master
 * per port, so all five must share the same bus handle.
 *
 * Implementation routes through codec_board's `init_i2c(port)` so
 * that codec_board's `init_codec()` finds an already-initialised
 * bus and skips its own attempt to acquire a master. The pin/port
 * mapping for the codec_board call comes from `board_cfg.txt`'s
 * `WAVESHARE_S3_EPAPER_1_54` entry — the SDA/SCL pins there must
 * match `MOCHI_I2C_SDA_GPIO` / `MOCHI_I2C_SCL_GPIO` /
 * `MOCHI_I2C_PORT` in `board_pins.h`.
 *
 * Replaces the vendor `i2c_bsp.cpp/.h`, which is no longer compiled.
 * See project memory `project-eink-codec-i2c-collision`.
 */

#pragma once

#include <driver/i2c_master.h>

namespace i2c_bus {

/* Idempotent. Calls codec_board::init_i2c() once and caches the
 * handle. Returns the same handle on every subsequent call. Returns
 * nullptr only on a first-call failure. */
i2c_master_bus_handle_t handle(void);

}  /* namespace i2c_bus */
