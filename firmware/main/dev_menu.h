/*
 * dev_menu — PWR-double-tap-driven debug screen wheel.
 *
 * Live-by-default home state with a wheel of diagnostic / utility
 * screens reachable by double-tapping PWR (single-tap is sleep). A
 * 60-second inactivity timeout snaps back to Live so the device
 * can't get stranded in a debug screen.
 *
 *   Live (default)  ── PWR×2 ─▶ Info ── PWR ─▶ Actions ──┐
 *      ▲                                                  │
 *      │                                       PWR ───────┘
 *      │                                  (wraps back to Info)
 *      │
 *      └── 60 s inactivity / touch outside any button / external "exit"
 *
 * From Actions, tapping a button can push a *modal* sub-screen (e.g.
 * the WiFi-switch list) inline rather than adding more wheel slots.
 * Modals exit on a button-tap (do the action) or on the next PWR tap
 * (which advances back to Info, leaving the modal behind).
 *
 * "Live" is a HOME STATE, not a wheel position. PWR-double-tap from
 * Live enters at the first wheel screen (Info); each subsequent
 * single PWR tap (while the wheel is up — sleep is suspended in this
 * mode) cycles. BOOT is reserved for voice start/stop and never
 * touches the wheel.
 *
 * Pre-v0.1.6 the wheel had four slots (Splash, Settings, Actions,
 * Wifi). Splash + Settings overlapped (both showed read-only device
 * info) and the Wifi slot duplicated what the Actions/Change-WiFi
 * button could push as a modal. v0.1.6 collapsed to two slots: Info
 * (merged splash + settings info) and Actions (with WifiSwitch as a
 * modal off the "Switch WiFi" button).
 *
 * The module owns:
 *   - the current mode (Live / Info / Actions / WifiModal)
 *   - inactivity timeout
 *   - render dispatchers + per-screen tap-rect hit-testing
 *
 * It does NOT own:
 *   - input detection. main.cpp drives request_advance() from PWR
 *     gesture observations.
 *   - the pet render path (render_resting in main.cpp). When the
 *     wheel exits to Live, dev_menu just clears its mode and
 *     main.cpp redraws the pet on the next tick.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "epaper_driver_bsp.h"

namespace dev_menu {

enum class Mode : uint8_t {
    Live = 0,        /* not in the wheel — home state */
    Info,            /* read-only device info / diagnostics */
    Actions,         /* tappable action buttons (design/22) */
    /* WifiModal isn't part of the PWR-tap cycle — it's pushed from
     * the "Switch WiFi" button on Actions and torn down on either a
     * button-tap (do) or a PWR-tap (advance back to Info). Listed
     * after Actions in the enum so advance() can skip it. */
    WifiModal,
    _Count,
};

/* Initialises wheel state. Idempotent. Call once at boot. */
void init(epaper_driver_display *epd);

/* Latch a "advance the wheel" request. main.cpp calls this:
 *   - on a PWR double-tap from Live → enters at Info
 *   - on a PWR single-tap while the wheel is up → cycles
 * tick() consumes the latch on its next pass. Multiple requests
 * within a single main-loop tick collapse to one (intentional —
 * fast-tap doesn't double-advance). */
void request_advance(void);

/* One-shot poll: consume a pending advance request, apply the
 * inactivity timeout. Call from the main loop's tick (1 Hz cadence
 * is fine — gestures are at human-press scale).
 *
 * Returns true if the mode CHANGED this tick (advanced via
 * request_advance, or timed out back to Live). main.cpp uses this
 * to decide whether to re-draw the pet (mode just returned to Live)
 * or stay quiet.
 *
 * `paired` is forwarded to the Info render so it composites the
 * pet zone correctly. `pet_name` / `version` / `ip_str` / `ssid`
 * feed the Info screen. Pass nullptr for any string that isn't
 * yet known. */
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
    SwitchWifi,       /* push the WifiModal sub-screen (in-place) */
    WifiSwitch,       /* (modal) commit a switch to the picked SSID — see picked_ssid() */
};

/* The SSID picked by the most recent WifiSwitch dispatch (the WiFi
 * modal). Valid right after dispatch_touch returns WifiSwitch; empty
 * otherwise. main.cpp reads this to know which stored network to join. */
const char *picked_ssid(void);

/* Forward an (x, y) touch event into the active wheel screen. Returns
 * the action the user requested, or None if the touch landed outside
 * any button. Caller still owns the "exit to live on miss" semantics
 * — this only reports the hit. Safe to call when current() == Live;
 * always returns None.
 *
 * Note: SwitchWifi is handled internally by dev_menu (it pushes the
 * WifiModal sub-screen and returns None to the caller); main.cpp
 * never sees a SwitchWifi result. */
TouchResult dispatch_touch(int x, int y);

}  /* namespace dev_menu */
