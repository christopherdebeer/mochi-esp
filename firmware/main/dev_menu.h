/*
 * dev_menu — PWR-driven settings + diagnostics wheel.
 *
 * Live-by-default home state. Pages are ordered by increasing risk;
 * a deliberate double-tap is what pages deeper into more destructive
 * territory, while a single-tap always pops straight back to Live.
 *
 *   Live ── PWR×2 ─▶ MenuP1 ── PWR×2 ─▶ MenuP2 ── PWR×2 ─▶ MenuP3 ──┐
 *    ▲                 │                  │                  │  (PWR×2 wraps to P1)
 *    │                 │ PWR×1            │ PWR×1            │ PWR×1
 *    │                 ▼                  ▼                  ▼
 *    └────────────── Live ───────────── Live ─────────────── Live
 *                          │
 *                          ├── tap action ─▶ exit
 *                          └── tap "Switch WiFi" (P2) ─▶ WifiModal
 *                                                          │ PWR×1
 *                                                          ▼
 *                                                         Live
 *
 * "Live" is a HOME STATE, not a menu position. Gesture model:
 *   - PWR×1 in Live    → sleep (handled by main.cpp, not us)
 *   - PWR×2 in Live    → MenuP1 (kid-facing main page)
 *   - PWR×1 in any menu→ exit to Live (escape hatch — never destroys)
 *   - PWR×2 in any menu→ page deeper into riskier territory
 *
 * Pagination beats scrolling on e-ink — the FT6336 doesn't report
 * enough intra-press position movement for LVGL to recognise drag,
 * and 200 px / partial-refresh hardware can't show smooth scroll
 * anyway. Three pages of 28-px buttons is significantly more
 * tappable than one page of 18-px buttons squeezed in.
 *
 * Page split (kid → settings → destructive):
 *   MenuP1 (kid):  pet status header · Memories · Places · Go home
 *   MenuP2 (set):  network header · Switch WiFi · OpenAI key
 *   MenuP3 (risk): warning · Update now · Add WiFi · Forget WiFi · Re-pair
 *
 * BOOT is reserved for voice start/stop and never touches the menu.
 *
 * Modals (WifiModal today; future ones plug in here) push from a
 * Menu button-tap and dismiss on the next PWR tap or a button-tap.
 *
 * History:
 *   v0.1.5 — 4-slot wheel (Splash / Settings / Actions / Wifi).
 *   v0.1.6 — collapsed to 2 slots (Info / Actions); WifiModal off Actions.
 *   v0.1.7 — single Menu screen (info header + actions); LVGL-backed.
 *
 * The module owns:
 *   - the current mode (Live / Menu / WifiModal)
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
    Live = 0,        /* not a menu mode — home state */
    MenuP1,          /* page 1 — kid-facing main page (pet status + memories/places/go home) */
    MenuP2,          /* page 2 — settings (network info + Switch WiFi + OpenAI key) */
    MenuP3,          /* page 3 — destructive actions (update / re-pair / forget) */
    /* WifiModal is pushed from the "Switch WiFi" button on MenuP2
     * and torn down on either a button-tap (do) or PWR×1 (Live). */
    WifiModal,
    /* ModelsModal is pushed from the "AI models" button on MenuP2 —
     * cycle the voice + text model selections (model_prefs). */
    ModelsModal,
    _Count,
};

/* Initialises wheel state. Idempotent. Call once at boot. */
void init(epaper_driver_display *epd);

/* Latch a "page-deeper request" — main.cpp calls this:
 *   - on a PWR double-tap from Live (enters MenuP1)
 *   - on a PWR double-tap while a menu page is up (advances to the
 *     next, riskier page; MenuP3 wraps back to MenuP1)
 * Single-tap-in-menu is NOT routed through here — that's an exit
 * straight to Live via exit_to_live() (see main.cpp's gesture
 * handling). tick() consumes the latch on its next pass; multiple
 * requests within a single main-loop tick collapse to one. */
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
          int net_phase, int batt_pct,
          /* Pet status snapshot for MenuP1's kid-facing header.
           * Formatted by the caller (mood + happiness/fullness/energy);
           * dev_menu just renders it verbatim. nullptr → blank. */
          const char *pet_status);

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
    ToggleChannel,    /* flip OTA channel stable<->beta (handled in dev_menu) */
    RePair,           /* clear pairing + reboot into the pairing flow */
    GoHome,           /* reset the pet's location to home */
    SwitchWifi,       /* push the WifiModal sub-screen (in-place) */
    WifiSwitch,       /* (modal) commit a switch to the picked SSID — see picked_ssid() */
    OpenModels,       /* push the ModelsModal sub-screen (in-place) */
    CycleVoiceModel,  /* (modal) cycle the voice model — handled in dev_menu */
    CycleTextModel,   /* (modal) cycle the text model — handled in dev_menu */
    /* Placeholders surfaced on the kid-facing MenuP1. Both currently
     * land in main.cpp as a "not implemented yet" toast — the rows
     * exist so the page has shape while the underlying data (memory
     * ledger / world places list) is wired up. */
    Memories,
    Places,
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
