# 22 — Settings actions + PWR single-tap sleep

Status: firmware in progress. Built against ESP-IDF v5.3.5; on-hardware
validation pending. 2026-05-25.

## Context

The dev_menu (design landed with the `dev_menu` consolidation) gives a
BOOT-button wheel of screens — `Live → Splash → Settings`. The key
portal moved into Settings as a tappable action, which frees the PWR
button from the triple-tap gesture it used to carry. This doc covers two
follow-ups: simplifying the PWR sleep gesture, and growing Settings into
a real action surface.

## Gesture model (now)

| Input | Action |
|-------|--------|
| **BOOT** short press | advance the dev_menu wheel (Live → Splash → Settings → Actions → wrap; 60 s inactivity or a touch returns to Live) |
| **PWR** single tap | sleep (deep sleep; wake on PWR/BOOT) |
| **PWR + BOOT** 10 s hold | factory reset |
| touch centre long-press | start/stop a voice session |

### PWR: 3 s hold → single tap

Sleep was a 3 s hold, and PWR triple-tap opened the key portal. The
triple-tap existed only because the key portal had no home; it forced
sleep to be a long hold so the two couldn't collide. With the key portal
in Settings, both go away and PWR gets one meaning: **tap = sleep**.

A "tap" is a PWR press↘release where:
- the press is ≤ `TAP_MAX_MS` (a deliberate tap, not a hold),
- **BOOT was never down during the press** — so the PWR+BOOT factory
  reset combo is never misread as a sleep tap,
- we're past a startup grace — so the PWR press that *woke* the device
  from deep sleep can't immediately re-sleep it.

It's edge-triggered: a PWR already held when the watcher starts (a
wake-hold) is ignored until released and tapped afresh. (`sleep_gesture.cpp`.)

## Settings information architecture

One read-only screen can't also hold a stack of generous touch targets
on a 200×200 panel, so the wheel splits into screens:

- **Settings** — read-only device info (fw, pet, net/ip/ssid, heap/psram,
  battery). No buttons. "BOOT: Actions".
- **Actions** — a vertical stack of tappable action buttons. A tap runs
  the action; a tap that misses every button exits to Live (unchanged
  miss semantics).
- **WiFi** — the stored networks, tap one to switch to it (below).

```
Live ─BOOT▶ Splash ─BOOT▶ Settings(info) ─BOOT▶ Actions ─BOOT▶ WiFi ─BOOT▶ (wrap to Splash)
```

## Actions (shipped)

| Button | Effect |
|--------|--------|
| **Change WiFi** | `prov_on_boot` + reboot into the SoftAP captive portal — add a new network. |
| **Forget WiFi** | `nvs_creds_forget(joined_ssid)` + reboot. connect_any then joins the next-strongest *known* network (or the no-creds path runs provisioning if that was the last one). |
| **Update now** | `ota_update::check_now()` — cuts the 24 h inter-check sleep short so the OTA poll runs immediately. |
| **Re-pair device** | `pair_creds_clear()` + reboot into the pairing flow. |
| **Go home** | `pet_sync_enter_place("home")` — resets the pet's location to home; the travel block swaps to the embedded home bundle next tick. Network POST; no-op offline. |
| **OpenAI key** | open the key-entry portal (moved here from the old Settings button + the retired PWR triple-tap). |

### WiFi screen — switch to a known network, no reboot

The WiFi wheel screen lists the stored networks (the joined one marked
`*`); tapping one runs a **runtime STA→STA switch** — `wifi_sta::switch_to`
(`esp_wifi_set_config` + connect, 15 s) — with no reboot and no AP↔STA
hand-off, so it sidesteps the v5.3 hang. On success the network is
promoted to MRU (so the next boot prefers it); on failure the device
falls back to `connect_any` so a failed switch doesn't strand it offline.

No scan: listing the stored creds *is* "the networks we already have
creds for", and `esp_wifi_connect` does a directed probe, so it reaches a
network even if it isn't beaconing (the Apple-hotspot case). "Add a new
network" remains the Actions → **Change WiFi** (SoftAP) path.

Reboot actions show a one-line toast before restarting so the tap gets
feedback. Each is reachable only with intent (BOOT to the Actions screen,
then a deliberate tap), so no confirm dialog for now — except factory
reset, which stays a gesture rather than a button (too destructive to sit
one tap from the home screen).

### Split: "Change" vs "Forget"

Per the request to flip quickly to a network we already have creds for:

- **Change WiFi** = *add* a brand-new network. Needs the SoftAP portal,
  hence the reboot into provisioning.
- **Forget WiFi** = *drop* the current one. Because the device stores an
  MRU list of up to 8 networks and `connect_any` scans + joins the
  strongest known network on boot, "forget current + reboot" lands you
  on the next-best known network with **no retyping** — the cheap path
  to "flip to a known SSID".

## Server side (`c15r/mochi`)

"home" is a canonical seed place (`backend/places.ts` `CANONICAL_SEEDS`
← `shared/locations.ts`), so `POST /api/places/home/enter` already
resolves, sets `pets.location='home'`, and returns the sheet. One
defensive edit landed in `backend/places-device.ts`: the `/enter`
handler now `backfillCanonicalPlaces()` + retries when a place isn't
found, so "Go home" (and any seed enter) resolves even for a pet whose
seeds weren't materialised yet this process — instead of a 404.

## Still deferred

- **Scan + visibility in the WiFi list.** The list shows stored networks
  but doesn't yet mark which are in range or sort by signal. A passive
  scan would add that (and a ~1.5 s stall on screen entry). Directed
  connect already reaches non-beaconing networks, so this is polish, not
  function.
- **On-hardware validation.** The runtime WiFi switch and Go-home were
  built + compile-checked but not flashed; the STA→STA transition and
  the offline-recovery fallback want a device pass.

## Files

- `firmware/main/sleep_gesture.{h,cpp}` — single-tap detector.
- `firmware/main/dev_menu.{h,cpp}` — `Mode::Actions` + the action buttons.
- `firmware/main/main.cpp` — action dispatch; retired the triple-tap path.
- `firmware/main/nvs_creds.{h,cpp}` — `nvs_creds_forget(ssid)`.
- `firmware/main/ota_update.{h,cpp}` — `check_now()` + interruptible wait.

## Cross-references

- `02-boot-sequence.md`, `03-provisioning.md` — the flows the WiFi
  actions reboot into.
- `21-nonblocking-wifi.md` — the offline dialog also routes to SoftAP
  provisioning; "Change WiFi" is the deliberate, always-available twin.
