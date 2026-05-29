/*
 * Pin map for the Waveshare ESP32-S3-Touch-ePaper-1.54 V2.
 *
 * Mirrors the relevant lines from
 * vendor/waveshare-eink/user_config.h (commit 3f96beedd2e8 in
 * waveshareteam/ESP32-S3-ePaper-1.54). The vendor file is left
 * untouched so future diffs are clean; this header is what the
 * firmware actually consumes.
 *
 * V1 boards have a different ESP32-S3 SoC (FH4R2) and a different
 * pin map; this file is V2-specific.
 */

#pragma once

#include "driver/gpio.h"

/* On-board user LED. Strapping pin (JTAG_SEL) but free for runtime
 * output once boot ROM has finished sampling straps. */
#define MOCHI_LED_GPIO          GPIO_NUM_3

/* User buttons. BOOT also doubles as the M2 cycle-pattern input. */
#define MOCHI_BOOT_BUTTON_GPIO  GPIO_NUM_0
#define MOCHI_PWR_BUTTON_GPIO   GPIO_NUM_18

/* SSD1681 e-paper. SPI2 host. Datasheet says up to 20 MHz; vendor
 * driver pushes 40 MHz in spi_port_init() — we leave that alone. */
#define MOCHI_EPD_DC_GPIO       GPIO_NUM_10
#define MOCHI_EPD_CS_GPIO       GPIO_NUM_11
#define MOCHI_EPD_SCK_GPIO      GPIO_NUM_12
#define MOCHI_EPD_MOSI_GPIO     GPIO_NUM_13
#define MOCHI_EPD_RST_GPIO      GPIO_NUM_9
#define MOCHI_EPD_BUSY_GPIO     GPIO_NUM_8

/* Power rails. Active-low: drive 0 to enable. EPD_PWR must be
 * enabled before EPD_Init or the panel never responds. */
#define MOCHI_EPD_PWR_GPIO      GPIO_NUM_6
#define MOCHI_AUDIO_PWR_GPIO    GPIO_NUM_42
#define MOCHI_VBAT_SENSE_GPIO   GPIO_NUM_17

/* I²C bus shared by FT6336 touch (0x38), RTC PCF85063 (0x51), and
 * SHTC3 temp/humidity (0x70). One bus, three devices — bring up
 * I²C once and let each driver attach. */
#define MOCHI_I2C_SDA_GPIO      GPIO_NUM_47
#define MOCHI_I2C_SCL_GPIO      GPIO_NUM_48
#define MOCHI_I2C_PORT          0           /* I2C_NUM_0 */

/* FT6336 capacitive touch controller. INT is active-low (falling
 * edge fires on touch begin); RST is active-low pulse to reset. */
#define MOCHI_TOUCH_INT_GPIO    GPIO_NUM_21
#define MOCHI_TOUCH_RST_GPIO    GPIO_NUM_7
#define MOCHI_TOUCH_I2C_ADDR    0x38

/* E-paper geometry. */
#define MOCHI_EPD_WIDTH         200
#define MOCHI_EPD_HEIGHT        200
#define MOCHI_EPD_BUFFER_LEN    ((MOCHI_EPD_WIDTH * MOCHI_EPD_HEIGHT) / 8)

/* ES8311 audio codec. Sits on the same I²C bus as touch + RTC + SHTC3
 * (port MOCHI_I2C_PORT). I²S is full-duplex on a single bus: TX (DOUT
 * = MCU → codec) drives the speaker; RX (DIN = codec → MCU) carries
 * mic samples. MCLK is the codec's master clock; the codec is the
 * I²S slave, MCU drives BCLK + WS.
 *
 * 24 kHz both directions matches the OpenAI Realtime API's PCM16 wire
 * format exactly — no resampling needed in or out. See
 * design/07-voice-architecture.md and the upstream XiaoZhi reference
 * (waveshareteam/ESP32-S3-ePaper-1.54: 02_Example/XiaoZhi/V2 config.h).
 *
 * AUDIO_PA enables the speaker amp downstream of the codec; toggle to
 * mute the speaker without touching codec state. AUDIO_PWR (GP42) is
 * the rail powering the codec + I²C peripherals; it must be on
 * before any I²S or codec-I²C traffic. */
#define MOCHI_I2S_MCLK_GPIO     GPIO_NUM_14
#define MOCHI_I2S_BCLK_GPIO     GPIO_NUM_15
#define MOCHI_I2S_WS_GPIO       GPIO_NUM_38
#define MOCHI_I2S_DIN_GPIO      GPIO_NUM_16   /* codec → MCU (mic) */
#define MOCHI_I2S_DOUT_GPIO     GPIO_NUM_45   /* MCU → codec (speaker) */
#define MOCHI_AUDIO_PA_GPIO     GPIO_NUM_46   /* speaker-amp enable, active-high */
#define MOCHI_ES8311_I2C_ADDR   0x18          /* ES8311_CODEC_DEFAULT_ADDR */
#define MOCHI_AUDIO_SAMPLE_RATE 24000         /* OpenAI Realtime PCM16 native */

/* Consolidated UI icon sheet (design/30). The device's care/stat icons and
 * the dev-menu tile icons all fetch + cache from this one sheet (80×80
 * cells → downsampled to 48). Was "ui-v1"; ui-icons-a folds those four plus
 * the menu glyphs into a single keyed sheet. */
#define MOCHI_UI_SHEET "ui-icons-a"
