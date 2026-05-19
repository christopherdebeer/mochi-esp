/*
 * Long-press PWR → enter deep sleep with PWR as wake source.
 *
 * Watches GPIO 18 (PWR) for a 3-second hold WHILE BOOT is NOT also
 * held. The "BOOT also held" exclusion is so a user trying to
 * trigger the factory-reset gesture (PWR + BOOT for 10 s) doesn't
 * accidentally fall asleep partway through. Factory-reset wins
 * by being checked in its own task on a different polling cadence.
 *
 * The watcher task only sets a "sleep requested" flag — it does
 * NOT itself render or call esp_deep_sleep_start. The main task
 * polls the flag, does the e-paper render of the asleep screen
 * with all its in-scope buffers, then calls commit_sleep() to
 * enter deep sleep. Cleaner than passing a function pointer with
 * captured state through; main keeps the full buffer view.
 *
 * E-paper persists across deep sleep with no power; the asleep
 * screen rendered before sleep stays visible the entire time.
 */

#pragma once

#include <stdint.h>

namespace sleep_gesture {

/* Spawn the watcher task. Idempotent. */
void start(void);

/* Returns true if the long-press has fired and the main task
 * should now render the asleep screen and call commit_sleep().
 * Returns false otherwise. Latches once true; resets only across
 * a full reboot. */
bool requested(void);

/* Enter deep sleep with PWR/BOOT as wake sources. Does NOT
 * return. Caller is responsible for having rendered whatever
 * they want visible during sleep. */
[[noreturn]] void commit_sleep(void);

}  /* namespace sleep_gesture */
