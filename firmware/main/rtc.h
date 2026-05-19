/*
 * PCF85063 real-time clock driver.
 *
 * The chip lives at I²C address 0x51 on the shared bus (GPIO 47/48).
 * Powered by GPIO 42 (Audio_PWR) — see project memory
 * project-eink-power-rails. Backed by a coin-cell on the board, so
 * time persists across power-off provided the cell is in.
 *
 * The PCF85063 stores wall-clock time as BCD in registers 0x04..0x0A.
 * This wrapper hides that — callers see plain integers.
 *
 * Use: rtc_init() once at boot, then rtc_set() / rtc_get() as needed.
 * Future M11 (decay clock) is the primary consumer; for now M7 just
 * proves we can read / write.
 */

#pragma once

#include <stdint.h>

struct mochi_datetime {
    uint16_t year;       /* full year, e.g. 2026 */
    uint8_t  month;      /* 1..12 */
    uint8_t  day;        /* 1..31 */
    uint8_t  hour;       /* 0..23 (24h mode) */
    uint8_t  minute;     /* 0..59 */
    uint8_t  second;     /* 0..59 */
    uint8_t  weekday;    /* 0..6 (caller convention; we just round-trip) */
};

/* One-time bring-up. Idempotent. Brings up the I²C bus singleton if
 * not already up. Clears the OSC-stop flag if set. Returns true on
 * success. */
bool rtc_init(void);

/* Read the current time. Returns true on success. */
bool rtc_get(struct mochi_datetime *out);

/* Set the clock. Returns true on success. */
bool rtc_set(const struct mochi_datetime *in);

/* Whether the chip's oscillator-stop flag was set at boot — true
 * means the clock lost time (e.g. battery removed) and the values
 * read from rtc_get are unreliable until a rtc_set call. Cleared
 * once rtc_set has been called. */
bool rtc_lost_power(void);
