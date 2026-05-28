# 26 — Idle tiers & light sleep

Status: draft / proposal, 2026-05-28. No firmware yet; this doc exists
to settle the state model and the config tradeoffs before any code, and
to frame the on-hardware power measurements that should drive the
timeout values.

Author note: prompted by the question "is a low-power mode between deep
sleep and live possible?" — yes, and the e-paper + capacitive-touch
architecture makes light sleep the natural intermediate primitive.

## Context

Power today is **binary**:

- **Live** — CPU at full clock (no `CONFIG_PM_ENABLE`, so no DFS / no
  automatic light sleep). The main loop blocks in `touch::wait_event`
  ~1 s at a time (100 ms during voice); the touch poll task wakes at
  10 Hz; the substrate re-projects every 60 s
  (`SUBSTRATE_REFRESH_US`). WiFi runs on IDF's default modem-sleep
  (no explicit `esp_wifi_set_ps`).
- **Deep sleep** — `commit_sleep()` (`sleep_gesture.cpp`) parks the SoC
  on `esp_deep_sleep_start()`, wake on PWR (GPIO18) or BOOT (GPIO0) via
  ext1 any-low. **Waking is a full reboot**: the app restarts and
  rebuilds state from NVS (this is why `pet_sync_restore_snapshot_from_nvs`
  + pre-render-last-place exist — wake == cold boot).

The gap: there is no resting state. Either we burn full power keeping a
static e-paper image lit by a fully-awake SoC, or we drop to a floor
that costs a multi-second reboot to climb out of. A pet toy spends most
of its life *idle but glanceable* — exactly the regime neither state
serves.

The pivotal hardware facts that make an intermediate tier cheap:

1. **The e-paper holds its image with zero power.** A resting screen
   needs no SoC involvement at all — unlike an LCD, "keep showing the
   pet" is free.
2. **Touch already has an ISR on the INT line (GPIO21).** GPIO21 is an
   RTC-capable pin on the ESP32-S3, so it can serve as a light-sleep
   *and* deep-sleep wake source. A finger-tap can wake the SoC in
   microseconds.

Together these mean the SoC can sleep while the pet stays drawn, and a
touch wakes it **with full RAM and live state intact** — no reboot, no
re-pair, no scene re-fetch.

## Goals / non-goals

**Goals**
- Add at least one resting tier between Live and Deep sleep that
  preserves in-RAM state and wakes near-instantly on touch/button.
- Keep the pet glanceable at all times (screen never blanks for idle).
- Behave well on battery; degrade gracefully (no worse than today) when
  USB is plugged in.
- Make the tier transitions observable (diag events) so power can be
  measured.

**Non-goals (this iteration)**
- Replacing deep sleep — PWR-tap → deep sleep stays the explicit "put
  it away" action.
- Animation/refresh changes (orthogonal; see design/05).
- A timer-wake deep-sleep "check in overnight" behavior — possible
  later, tracked as an open question, not built here.

## Proposed power-state model

```
            no touch ~Tdoze            no touch ~Tsleep        PWR tap
   ┌──────┐ ───────────────▶ ┌──────┐ ───────────────▶ ┌────────────┐
   │ LIVE │                  │ DOZE │                  │ DEEP SLEEP │
   └──────┘ ◀─────────────── └──────┘ ◀ (reboot) ───── └────────────┘
       ▲   touch / button /      │   touch / button
       │   timer heartbeat       │
       └─────────────────────────┘
```

Three live-ish tiers + the existing floor:

### Tier L — Live (unchanged)
Active interaction. Full clock, 1 s (100 ms voice) touch cadence, WiFi
connected, 60 s substrate refresh. Entered from any wake.

### Tier D — Doze (new, primary "between" state)
Entered after `T_doze` of no touch/button (candidate: 45–90 s — set by
measurement). The pet shows its resting/sleeping face (already what the
substrate projects when idle); the screen does **not** change on entry
beyond that natural resting render, so entry is silent and flicker-free.

Behaviour:
- **SoC**: automatic light sleep via `esp_pm` (see Config). The SoC
  sleeps in the gaps between FreeRTOS ticks and during the main loop's
  `wait_event` block, transparently. RAM + peripherals retained.
- **Touch INT (GPIO21)** registered as a GPIO light-sleep wake source →
  a tap wakes the SoC in µs, the existing ISR fires, the loop dispatches
  the tap normally. Wake → Tier L.
- **WiFi**: dropped, or set to `WIFI_PS_MAX_MODEM` — see WiFi section.
- **Substrate refresh**: stretched (candidate 5–10 min) so the SoC
  mostly stays asleep; a heartbeat timer wake handles it.

This is the state the device lives in most of the time. It looks
identical to Live to the user (pet on screen) but the SoC is asleep
between events.

### Tier S — Deep sleep (unchanged)
Entered by explicit PWR tap, or after a long secondary timeout
`T_sleep` in Doze (candidate 30–60 min) to protect the battery if the
device is left untouched for hours. Wake = reboot + NVS restore, as
today.

## Wake sources

| Source | Pin | Doze (light) | Deep | Notes |
|---|---|---|---|---|
| Touch INT | GPIO21 | ✅ GPIO wake | (not today) | RTC-capable; ISR already installed |
| PWR button | GPIO18 | ✅ GPIO wake | ✅ ext1 any-low | |
| BOOT button | GPIO0 | ✅ GPIO wake | ✅ ext1 any-low | |
| Heartbeat timer | — | ✅ timer wake | (open q) | drives substrate refresh while dozing |

Doze adds touch INT as a wake source — the key difference from deep
sleep, which deliberately wakes only on the physical buttons.

## Config changes required (`sdkconfig.defaults`)

```
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_PM_DFS_INIT_AUTO=y          # or set freqs explicitly at runtime
# esp_pm_config: max=240, min=80 (or 40), light_sleep_enable=true
```

Two existing settings interact and must be revisited:

- **`CONFIG_FREERTOS_HZ=1000`** works *against* tickless idle — a 1 kHz
  tick means up to 1000 idle wakeups/s, capping the achievable sleep
  residency. The current 1 kHz was chosen "for animation frame pacing
  later" (it has no consumer yet). Recommend dropping to 100 Hz for the
  doze win, or measuring both. Tickless idle suppresses the ticks while
  idle regardless, but a lower base HZ reduces wake churn.
- **`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`** — an attached USB-Serial-JTAG
  host keeps the peripheral clock requested and can block light sleep
  entirely. Acceptable: on battery there's no host; on the bench you see
  Live-like draw. Worth a diag note so it's not mistaken for a
  regression during measurement.

## WiFi handling in Doze

Two options; pick by measurement + product feel:

1. **Drop WiFi on doze entry, reconnect on wake.** Simplest, lowest
   doze power. The design/21 non-blocking worker already reconnects
   gracefully off the critical path; the pet only needs periodic sync,
   so a reconnect on the next interaction (or heartbeat) is fine. Risk:
   server-initiated pushes (voice-driven travel) won't land until the
   next wake — acceptable, the device is idle by definition.
2. **Keep WiFi at `WIFI_PS_MAX_MODEM`.** Connection stays alive,
   coexists with light sleep by waking on DTIM beacons. Higher doze
   power, but inbound state stays current. Heavier; probably not worth
   it for a kid's pet that's idle.

Lean: option 1, with the heartbeat timer doing an opportunistic
reconnect + `pet_sync` resync, then back to sleep.

## Implementation sketch

State lives in the main loop, which already tracks `last_event_us`
(updated on every dispatched touch) and gates idle work on it (e.g. the
`OTA_IDLE_GATE_US` check at `main.cpp:2423` is prior art for
"only when the user's been idle a while").

- Add an `idle_us = now_us - last_event_us` computation at the top of
  the loop and derive a `PowerTier` from it (`Live` < `T_doze` ≤ `Doze`
  < `T_sleep` ≤ deep-sleep request).
- On the Live→Doze edge: register the touch INT GPIO wake
  (`gpio_wakeup_enable(GPIO21, GPIO_INTR_LOW_LEVEL)` +
  `esp_sleep_enable_gpio_wakeup()`), set WiFi PS / drop WiFi, lengthen
  the effective substrate interval, emit a `DIAG_INFO "power" "doze"`
  event. With `esp_pm` + tickless idle enabled, the existing
  `wait_event(1000ms)` block then *is* the sleep — no explicit
  `esp_light_sleep_start()` call needed; the PM lock framework sleeps
  whenever no task holds a lock.
- On any touch/button (Doze→Live edge): undo the above, restore the
  fast cadence, emit `"wake"`.
- On the Doze→Deep edge (`T_sleep` exceeded): render the asleep frame
  and `commit_sleep()` exactly as the PWR-tap path does today.

Because automatic light sleep is transparent, the bulk of the work is
the tier state machine + the WiFi/refresh-cadence policy, not explicit
sleep calls. An explicit `esp_light_sleep_start()` with timer wake (the
"heartbeat" variant) is the fallback if `esp_pm` residency proves poor.

## Risks / validate on hardware

- **Light-sleep residency vs `FREERTOS_HZ`** — measure 1 kHz vs 100 Hz.
- **Touch INT as a wake source** — confirm GPIO21 actually wakes the SoC
  from light sleep on this board rev (the same wiring caveat the touch
  bring-up already flagged: if GPIO21 isn't wired to FT6336_INT, doze
  falls back to button-only wake + the 10 Hz poll, which is degraded but
  not broken).
- **e-paper driver state across light sleep** — the panel is idle and
  retains, but confirm the SSD1681/SPI peripheral doesn't need
  re-init on wake (light sleep retains peripheral config; expected fine,
  verify).
- **Voice / OTA / consolidate** must hold a PM lock (or keep the device
  in Live) while running so light sleep can't cut a TLS handshake or an
  active session. `esp_pm_lock_acquire` around those, or simply gate
  doze on `!voice::is_active() && !ota_in_flight() && !consolidate_in_flight()`
  (matches existing idle-work gates).

## Measurement plan (drives the timeouts)

Before fixing `T_doze` / `T_sleep`, measure on hardware (battery, USB
detached):

1. Live, idle screen, WiFi connected — baseline mA.
2. Doze, WiFi dropped, light sleep — target mA.
3. Doze, WiFi `MAX_MODEM` — comparison.
4. Deep sleep — floor µA (and the wake-reboot energy cost, to know how
   often a timer-wake deep sleep would be worth it).

The ratio of (1) to (2) sets whether Doze is worth a short or long
`T_doze`; the ratio of (2) to (4) sets `T_sleep`.

## Open questions

1. Timer-wake **deep** sleep for an overnight "check in + update splash"
   without staying in doze — worth it once (2)/(4) is known?
2. Should Doze entry change the pet's face deliberately (a visible
   "settling" cue) or stay silent? Silent avoids an e-paper flash;
   a cue is more legible. Lean silent.
3. `T_doze` user-configurable in Settings (design/22), or fixed?

## Cross-references

- `design/22-settings-actions-and-sleep.md` — PWR-tap sleep gesture,
  the Live/Splash/Settings wheel (doze must not fight the wheel).
- `design/21-nonblocking-wifi.md` — the reconnect-off-critical-path
  worker that makes "drop WiFi on doze" safe.
- `firmware/main/sleep_gesture.cpp` — `commit_sleep()`, the deep-sleep
  wake-source setup this builds on.
- `firmware/main/main.cpp` — `last_event_us`, `SUBSTRATE_REFRESH_US`,
  the `OTA_IDLE_GATE_US` idle gate (prior art for tier detection).
- `firmware/main/board_pins.h` — GPIO21 touch INT, GPIO18 PWR,
  GPIO0 BOOT, GPIO17 VBAT sense.
- `firmware/sdkconfig.defaults` — `FREERTOS_HZ`, console, where the
  `esp_pm` knobs land.
