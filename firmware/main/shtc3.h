/*
 * SHTC3 temperature + humidity sensor.
 *
 * Sensirion SHTC3 lives at I²C address 0x70 on the shared bus.
 * Powered by the same Audio_PWR rail as RTC + touch (GPIO 42).
 *
 * Operation: wake → measure → sleep, ~30 ms total. Caller can call
 * shtc3_read() at whatever cadence makes sense; the chip is in
 * sleep between reads so it draws ~0.3 µA.
 *
 * Values are returned in float — temperature in °C, humidity in
 * percentage. Both are sensor-frame; no compensation for the local
 * board self-heating yet (the vendor sample subtracts a magic 4 °C
 * fudge — we'll do that later if real readings warrant it).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool shtc3_init(void);

/* Take one measurement. Returns true and fills the outputs on
 * success. On CRC failure or I²C error returns false and outputs
 * are unchanged. Blocks for ~25 ms (wake delay + measurement). */
bool shtc3_read(float *out_temp_c, float *out_rh_pct);

#ifdef __cplusplus
}
#endif
