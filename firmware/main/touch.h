/*
 * FT6336 capacitive-touch wrapper.
 *
 * Initialises the I²C master bus on the shared SDA/SCL pair, brings
 * up the FT6336 controller behind it, and surfaces a blocking
 * `wait_event(timeout)` that returns the next finger-down position
 * via an ISR + FreeRTOS queue.
 *
 * The vendor singletons (I2cMasterBus, I2cFt6336Dev) live in
 * vendor/waveshare-eink/. This wrapper hides them so the rest of
 * the firmware speaks plain `touch::Event` and never touches
 * vendor-specific types directly.
 *
 * Future RTC (M7) and SHTC3 (M8) drivers will reuse the same
 * I2cMasterBus singleton — bring up i2c once, attach each device.
 * For now this wrapper is the only owner.
 */

#pragma once

#include <stdint.h>

namespace touch {

struct Event {
    uint16_t x;        /* 0..199 in panel coords */
    uint16_t y;
};

/* One-time bring-up. Idempotent: safe to call once at boot. Sets up
 * I²C bus, resets FT6336, installs ISR on the touch INT line. */
void init();

/* Block waiting for a touch event up to `timeout_ms` (or pass -1 to
 * wait forever). Returns true and fills `out` when a finger-down
 * event arrives; returns false on timeout. Multiple events that
 * queue up between calls are coalesced — only the latest is
 * returned, since debouncing finger-still events into one position
 * is what the user means by "a tap." */
bool wait_event(Event *out, int timeout_ms);

/* Read the current finger-down position without blocking. Returns
 * true and fills `out` if the FT6336 currently reports a finger,
 * false otherwise. Used to distinguish tap from hold: poll this at
 * ~10 Hz after a tap and watch how long the finger stays in the
 * same zone. */
bool current_point(Event *out);

}  /* namespace touch */
