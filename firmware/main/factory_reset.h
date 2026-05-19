/*
 * Factory-reset gesture.
 *
 * Hardware: PWR + BOOT held simultaneously for 10 continuous seconds.
 * Visible-progress UX: every 2 seconds during the hold, a partial
 * refresh updates the e-paper with "Hold for N more seconds..." so
 * the user can see the countdown and back out by releasing either
 * button.
 *
 * Why hardware buttons (and not touch): touch can mis-fire under
 * normal handling, and touch is itself only available after I²C +
 * power rails come up — so it can't reset a device that's stuck in
 * provisioning. PWR + BOOT work from the moment app_main runs.
 *
 * On commit: clears NVS pair + wifi creds and reboots. The next
 * boot will see no creds and re-enter provisioning.
 */

#pragma once

#include "epaper_driver_bsp.h"

namespace factory_reset {

/* Spawn the watchdog task. Uses the supplied epd to render the
 * countdown and the "resetting..." confirmation screens; the epd
 * driver must already be initialised. Idempotent. */
void start(epaper_driver_display *epd);

}  /* namespace factory_reset */
