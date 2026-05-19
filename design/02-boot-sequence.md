# 02 — Boot sequence

Status: live, last refreshed 2026-05-18 against shipped M1–M8 firmware.

**Terminology note.** The original draft called the SoftAP captive-
portal screen "PAIRING". That branch is actually **provisioning**
(handing the device WiFi creds — see `03-provisioning.md`). Pairing in
the post-M5 sense is the *device-to-pet* bind that happens after WiFi
is up — see `04-pairing.md`. This doc has been updated to use the
distinction consistently; the old "PAIRING screen" is now
"PROVISIONING screen", and "WAITING-FOR-PAIRING" keeps that name.

What the device does from power-on to "Mochi is on screen". E-paper has
one underused property worth designing around — **the display persists
across power-off**. The boot UI is a state that outlives the boot itself.

## State machine

```
            ┌─────────────────────┐
            │     power on        │
            └──────────┬──────────┘
                       ▼
            ┌─────────────────────┐
            │  read RTC + NVS     │
            │  • wake reason?     │
            │  • have wifi_creds? │
            │  • have pairing?    │
            │  • have cached      │
            │    sprite?          │
            └──────────┬──────────┘
                       │
                       ▼
        ┌──────────────┼──────────────────────┐
        │              │                      │
        ▼              ▼                      ▼
  no wifi creds   wifi creds present     paired + cached sprite
        │              │                      │
        ▼              ▼                      ▼
  PROVISIONING   WAITING-FOR-                WARM
  screen         PAIRING screen              BOOT
  (SoftAP)       (QR + short                 (sprite
                  code, poll                  immediately
                  server)                     from cache)
        │              │                      │
        ▼              ▼                      ▼
   [user runs    [user pairs on              [WiFi connect
    captive       phone → server              + sync in
    portal]       binds device]               background]
        │              │                      │
        └──────────────┴──────────────────────┘
                       │
                       ▼
            ┌─────────────────────┐
            │   READY: pet on     │
            │   screen, event     │
            │   loop running      │
            └─────────────────────┘
```

## Branches in detail

### Cold boot, no WiFi creds → PROVISIONING

This is the first-power-on case. The device has nothing.

1. RTC has no meaningful state (could be set, could be reset to epoch).
2. NVS has no `wifi_creds` blob.
3. NVS has no `pairing_token`.
4. LittleFS has no cached sprite.

Action: render the PROVISIONING screen, start SoftAP, wait. Provisioning
detail is in `03-provisioning.md`. The screen shows:
- "Hi! I'm Mochi."
- SSID: `Mochi-XXXX` (last 4 of MAC)
- "Join this WiFi to set me up"
- A small placeholder pet sprite (canonical species, no costume)

This screen stays up until the user joins the SoftAP and completes the
captive portal. If the user walks away, the screen stays — e-paper is
ambient-friendly. Tomorrow they can come back and finish setup with the
exact same screen still there.

### Cold boot, WiFi creds but no pairing → WAITING-FOR-PAIRING

WiFi creds were entered, the device joined home WiFi, but the user hasn't
yet paired the device to a Mochi pet on mochi.val.run.

Action: connect WiFi, render WAITING-FOR-PAIRING screen, poll
mochi.val.run for a pairing-completion signal. Screen shows:
- "Almost ready!"
- A QR code encoding a one-time short code
- Below: "Or visit mochi.val.run/pair and enter: ABC-123"
- The same canonical placeholder sprite

Polling cadence: every 5 seconds for the first minute, then every 30
seconds. Use the cheap `/api/device/pair-check?token=…` endpoint that
mochi.val.run will serve (see `04-pairing.md`).

### Wake from deep sleep, paired + cached → WARM BOOT

The common case during normal operation.

1. RTC says we slept for N minutes.
2. NVS has `wifi_creds`, `pairing_token`, `pet_id`.
3. LittleFS has the current costume sprite.

Action: load cached sprite to PSRAM, render to e-paper *immediately* —
no network gate. Then kick WiFi + sync in the background. The screen
update happens in under 300ms via partial refresh; the pet is "there"
the moment you press the wake button.

Background work after the initial render:
- WiFi connect (typical: 2–5s on a known network)
- Fetch state delta from mochi.val.run (any updates while we slept)
- Replay decay locally using `now - last_active_ts` from RTC
- Update screen if state actually changed

The decay replay uses the RTC, not the server's clock. This is what makes
the device authoritative for moment-to-moment state.

### Paired, no cached sprite → degraded warm boot

Edge case: pairing happened on a different device, this one has never
fetched a sprite yet. Or the cache was wiped.

Action: connect WiFi → fetch current costume sheet for the paired pet →
render. The screen shows a "loading…" placeholder for ~2–3 seconds.

This case will be rare in practice (post-pairing, the device fetches and
caches immediately) but the path must exist.

## Crash safety

E-paper persists across crash. This is mostly a gift — but it has a sharp
edge: if the device crashes mid-transition, whatever was on screen stays
on screen. A frozen "loading…" or partial sprite is *worse* than a frozen
"your pet is sleeping".

Design rules:

1. **Every rendered screen must be interpretable as a final state.** No
   transitional UI. "Loading…" is banned; use "Connecting" or "Catching
   up — almost there" — phrasings the user can sit with for an hour and
   not be confused by.
2. **Sprite renders are atomic at the controller level.** Never push a
   half-composed frame. Compose in PSRAM, then push a complete frame to
   the display.
3. **Boot sequence avoids screen updates until a meaningful first frame
   is ready.** If we crash before that first frame, the *previous boot's*
   screen is still there. Better than a half-update.

## Deep sleep policy

Power management is its own document, but the boot path interacts with it.
Three relevant wake sources:

- **Button press** — user wants attention, wake fully, full event loop
- **Touch event** — same as button (but expect false positives, debounce)
- **RTC alarm** — periodic check-in (every ~30 min) to:
  - Project decay forward
  - Update display if mood changed enough to warrant a refresh
  - Sync with mochi.val.run if WiFi is still credentialed
  - Go back to deep sleep

The RTC-alarm wake path is a *headless* boot: WiFi may or may not run,
display may not refresh, the device sleeps again within seconds. Decay
projection happens on every RTC wake regardless of whether the display
updates.

## Cross-references

- `00-architecture.md` — what state lives on device
- `01-bring-up-plan.md` — M3 implements the PROVISIONING branch; M4
  proves the WARM-BOOT-style sprite-fetch path against a hardcoded
  test pet (no real pairing); M5 implements WAITING-FOR-PAIRING
- `03-provisioning.md` — PROVISIONING branch detail
- `04-pairing.md` — WAITING-FOR-PAIRING branch detail and the
  device-to-pet bind protocol
- workspace `a57fe5ca05484f` — engagement-aware tempo, depends on RTC

## What's implemented vs deferred (as of 2026-05-18, post-M8.5)

| Branch                     | State                                  |
|----------------------------|----------------------------------------|
| PROVISIONING (no creds)    | ✅ M3                                  |
| WAITING-FOR-PAIRING        | ✅ M5                                  |
| WARM BOOT (paired+cached)  | ✅ M8.5: LittleFS sprite cache hits at boot, scene + icons + neutral pet load from disk in ~10 ms each, full panel composite + render; ETag HEAD probes detect server-side artwork changes and invalidate per-sheet |
| Degraded warm boot         | ✅ implicit: cache miss path falls through to network fetch + write-back to cache |
| Sleep gesture (PWR 3 s)    | ✅ M8.5: render `sleeping` + "Asleep — PWR to wake" → `esp_deep_sleep_start`; e-paper persists with no power |
| Wake from deep sleep       | ✅ M8.5: PWR or BOOT triggers cold reset → standard boot path → warm cache hits → ~3 s back to neutral |
| RTC-alarm wake             | not yet — periodic check-ins for decay tick + sync land with M11–M13 |
| Crash-safe rendering rules | ✅ enforced by current screens (every screen is a final state, no "loading…") |
| Atomic sprite renders      | ✅ via `EPD_LoadBuffer` + full/partial refresh |
| Factory reset (PWR+BOOT)   | ✅ M8.5: 10 s hold wipes NVS (wifi + pair) and reboots into provisioning |

The state machine in this doc is the long-term shape. Today the right
edge of the diagram (WARM BOOT) is the common case after first-time
setup, with deep-sleep wake working but the periodic-RTC-alarm
headless wake reserved for the M11+ era when there's actual decay
state to project forward.
