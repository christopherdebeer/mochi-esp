/*
 * Long-press PWR → enter deep sleep with PWR as wake source.
 *
 * Watches GPIO 18 (PWR) for a 3-second hold WHILE BOOT is NOT also
 * held. The "BOOT also held" exclusion is so a user trying to
 * trigger the factory-reset gesture (PWR + BOOT for 10 s) doesn't
 * accidentally fall asleep partway through. Factory-reset wins
 * by being checked in its own task on a different polling cadence.
 *
 * Two paths for committing:
 *
 *   1) Rich path — main loop polls requested(), calls mark_handled()
 *      to claim ownership, then renders the pet's `sleeping` cell
 *      over the current scene (using all the buffers main has in
 *      scope) and calls commit_sleep(). This is what the user sees
 *      from the steady-state main screen.
 *
 *   2) Fallback path — if no one claims the gesture within a grace
 *      window (main is blocked in provisioning, pair-wait, or one
 *      of the halt loops), the watcher task itself renders a
 *      generic "Asleep" screen on the e-paper and calls
 *      commit_sleep(). This is what makes the gesture reachable
 *      from every screen, not just the main one.
 *
 * E-paper persists across deep sleep with no power; whichever
 * screen was rendered before sleep stays visible the entire time.
 */

#pragma once

#include <stdint.h>

#include "epaper_driver_bsp.h"

namespace sleep_gesture {

/* Spawn the watcher task. Idempotent. epd is used only by the
 * fallback render path; the rich path renders via main's own
 * buffers. */
void start(epaper_driver_display *epd);

/* Returns true if the long-press has fired and the main task
 * should now render the asleep screen and call commit_sleep().
 * Returns false otherwise. Latches once true; resets only across
 * a full reboot. */
bool requested(void);

/* Tell the watcher "I'm going to handle this — don't fire the
 * fallback render". Main calls this as the first thing inside its
 * requested() branch, before the (potentially slow) network fetch
 * of the sleeping pet cell. */
void mark_handled(void);

/* Enter deep sleep with PWR/BOOT as wake sources. Does NOT
 * return. Caller is responsible for having rendered whatever
 * they want visible during sleep. */
[[noreturn]] void commit_sleep(void);

}  /* namespace sleep_gesture */
