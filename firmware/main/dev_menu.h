/*
 * dev_menu — BOOT-button-driven debug screen wheel.
 *
 * Live-by-default home state with a wheel of diagnostic / utility
 * screens reachable by short-pressing the BOOT button. While not in
 * Live, a 5-second inactivity timeout snaps back so the device can't
 * get stranded in a debug screen.
 *
 *   Live (default)  ── BOOT ─▶ Splash ── BOOT ─▶ Diagnostics ─┐
 *      ▲                                                       │
 *      │                                          BOOT ────────┘
 *      │                              (wraps back to Splash)
 *      │
 *      └── 5 s inactivity / external "exit" call
 *
 * "Live" is a HOME STATE, not a wheel position. Pressing BOOT while
 * already showing the live pet jumps to the first debug screen
 * (Splash); subsequent presses cycle the rest.
 *
 * The module owns:
 *   - the current mode (Live / Splash / Diagnostics / …)
 *   - debounced edge detection on BOOT
 *   - inactivity timeout
 *   - render dispatchers that draw a complete frame for each mode
 *
 * It does NOT own:
 *   - the pet render path (that's render_resting in main.cpp). When
 *     the wheel exits to Live, dev_menu just clears its mode and
 *     main.cpp redraws the pet on the next tick.
 *
 * Future modes (WifiSelect, Provision, Pairing, OtaForce) drop into
 * the same shape — add to the Mode enum and the dispatcher.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "epaper_driver_bsp.h"

namespace dev_menu {

enum class Mode : uint8_t {
    Live = 0,        /* not in the wheel — home state */
    Splash,
    Settings,        /* read-only device info / diagnostics */
    Actions,         /* tappable action buttons (design/22) */
    Wifi,            /* known-network list — tap to switch (design/22) */
    /* future modes plug in here. */
    _Count,
};

/* Initialises debounce state. Call once at boot, after gpio_config
 * for MOCHI_BOOT_BUTTON_GPIO has run. */
void init(epaper_driver_display *epd);

/* One-shot poll: read BOOT, advance the wheel on a falling edge,
 * apply the inactivity timeout. Call from the main loop's tick
 * (1 Hz cadence is fine — gestures are at human-press scale).
 *
 * Returns true if the mode CHANGED this tick (advanced via BOOT, or
 * timed out back to Live). main.cpp uses this to decide whether to
 * re-draw the pet (mode just returned to Live) or stay quiet.
 *
 * `paired` is forwarded to the splash render so it composites the
 * pet zone correctly. `pet_name` / `version` / `ip_str` / `ssid`
 * feed the diagnostics screen. Pass nullptr for any string that
 * isn't yet known. */
bool tick(epaper_driver_display *epd, bool paired,
          const char *pet_name, const char *version,
          const char *ip_str, const char *ssid,
          int net_phase, int batt_pct);

/* Current wheel mode. Mode::Live means main.cpp owns the screen. */
Mode current(void);

/* True iff current() != Live — main.cpp uses this to skip its own
 * render path while a debug screen is up. */
bool active(void);

/* Force-exit any debug screen back to Live. main.cpp calls this
 * when something else needs the screen (e.g. a touch gesture or
 * a sleep request). The next main-loop tick will redraw the pet. */
void exit_to_live(void);

/* Touch-driven actions on the active wheel screen. Returned values
 * tell main.cpp what (if anything) to do AFTER dev_menu has
 * exited the wheel back to Live. Buttons are debug-only — actions
 * the device couldn't otherwise reach without a serial cable. */
enum class TouchResult : uint8_t {
    None = 0,         /* no button hit; main.cpp may exit_to_live itself */
    OpenKeyPortal,    /* open the OpenAI key-entry portal */
    ChangeWifi,       /* reboot into SoftAP provisioning (add a network) */
    ForgetWifi,       /* forget the joined SSID + reboot (flip to next known) */
    UpdateNow,        /* force an immediate OTA manifest check */
    RePair,           /* clear pairing + reboot into the pairing flow */
    GoHome,           /* reset the pet's location to home */
    WifiSwitch,       /* switch to the picked known network — see picked_ssid() */
};

/* The SSID picked by the most recent WifiSwitch dispatch (the WiFi
 * screen). Valid right after dispatch_touch returns WifiSwitch; empty
 * otherwise. main.cpp reads this to know which stored network to join. */
const char *picked_ssid(void);

/* Forward an (x, y) touch event into the active wheel screen. Returns
 * the action the user requested, or None if the touch landed outside
 * any button. Caller still owns the "exit to live on miss" semantics
 * — this only reports the hit. Safe to call when current() == Live;
 * always returns None. */
TouchResult dispatch_touch(int x, int y);

}  /* namespace dev_menu */
