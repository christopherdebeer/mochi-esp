/*
 * Battery voltage + percent sense.
 *
 * The Waveshare board exposes the LiPo through a 1:2 resistor
 * divider gated by GPIO 17 (the "VBAT_PWR" pin we drive HIGH at
 * boot in main.cpp's peripheral_rails_on()). The divider's output
 * lands on ADC1 channel 3 — that's the channel the vendor's
 * 01_ADC_Test sample reads, with full atten (0..3.3V range) and
 * 12-bit width.
 *
 * battery_init() configures the ADC oneshot unit + curve-fitting
 * calibration; battery_read() returns the most recent sample,
 * scaled back through the 1:2 divider to give VBAT in millivolts
 * and a coarse percent on a LiPo discharge curve.
 *
 * Idempotent — safe to call init multiple times.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

bool battery_init(void);

/* Take one ADC sample. Returns true and fills *out_mv with VBAT in
 * millivolts (typical 3300..4200 for a single-cell LiPo) and
 * *out_pct with a coarse 0..100 charge estimate. Either output
 * pointer may be NULL. */
bool battery_read(uint16_t *out_mv, uint8_t *out_pct);
