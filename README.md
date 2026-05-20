# eink-pet

[![firmware build](https://github.com/christopherdebeer/mochi-esp/actions/workflows/firmware-build.yml/badge.svg?branch=main)](https://github.com/christopherdebeer/mochi-esp/actions/workflows/firmware-build.yml)

A hardware embodiment of [Mochi](https://mochi.val.run) — a virtual pet that
runs on a small e-paper device, talks via the OpenAI Realtime API over WiFi,
and works in airplane mode for everything except voice.

This val started as a quick e-ink-shaped experiment in 2026-05 and was
absorbed into the Mochi web prototype almost immediately. With the arrival
of a Waveshare ESP32-S3-Touch-ePaper-1.54 V2 dev board (workspace entry
`c6a8376def6946`), it now has a real device target — and grows here.

## Target hardware

Waveshare ESP32-S3-Touch-ePaper-1.54 V2:

- ESP32-S3-PICO-1 N8R8 (8MB flash, 8MB PSRAM, WiFi+BLE)
- 1.54" touch e-paper, 200×200 px (SSD1681 controller, capacitive touch)
- ES8311 audio codec + onboard mic + speaker pins
- PMIC ETA6098 + battery connector (LiPo, MX1.25) + USB-C
- RTC PCF85063 (I²C)
- SHTC3 temperature/humidity (I²C)
- microSD slot, 12-pin GPIO header

> **Heads-up if you order:** the "with LiPo" SKU we received shipped with
> the cell connector empty. Visually inspect before debugging battery
> behaviour in firmware — the ADC reads a misleading ~4150 mV from a
> leakage path through the charge IC even with no cell installed. See
> `firmware/`'s memory of project notes (project memory
> `project-eink-missing-battery`).

## Repository shape

```
eink-pet/
├── README.md            this file
├── AGENTS.md            val.town code-gen guidelines (server side)
├── main.tsx             server-side root (HTTP val; currently a stub)
├── design/              architecture + per-decision design docs
│   ├── 00-architecture.md     device vs server partition
│   ├── 01-bring-up-plan.md    M1..M8.5..M11+ milestones (see Status below)
│   ├── 02-boot-sequence.md    runtime state machine
│   ├── 03-provisioning.md     SoftAP + captive portal (M3)
│   ├── 04-pairing.md          device-to-pet bind (M5)
│   ├── 05-sprite-format.md    panel + cell + scene wire formats
│   └── 06-scene-contracts.md  M11.5 stub: diegetic-interfaces device side
└── firmware/            ESP-IDF firmware project (built locally with idf.py)
    ├── README.md        toolchain setup + build/flash instructions
    ├── CMakeLists.txt
    ├── sdkconfig.defaults     baseline IDF config (target, PSRAM, console, …)
    ├── partitions.csv         8MB partition table (factory + storage/littlefs)
    ├── dependencies.lock      pinned IDF component versions
    ├── main/                  app source — see firmware/README.md
    └── vendor/
        └── waveshare-eink/    frozen vendor SSD1681 driver + provenance
```

The val.town side hosts: a `/devsprite/*` family of endpoints (panel
bitmaps, native cells with mask plane, panel-area scenes), a
`/api/device/pair-*` flow, and HTML for `mochi.val.run/pair-device`. The
realtime voice proxy is on the M9–M10 horizon. See `design/00-architecture.md`
for the device-vs-server partition.

## Building and flashing

The firmware isn't built by val.town — it's an ESP-IDF project that
compiles locally. From a clean `vt clone`:

```sh
. ~/esp/esp-idf/export.sh                       # set up the IDF env
cd firmware
idf.py set-target esp32s3                       # one-time
idf.py reconfigure                              # fetches managed_components/
idf.py -p /dev/cu.usbmodem* flash monitor       # build + flash + tail
```

Full toolchain setup, port-name conventions per OS, and troubleshooting:
**`firmware/README.md`**.

## Status (as of 2026-05-18)

The architectural spine — toolchain → display → WiFi → network sprite →
pairing → touch → RTC → temp/humidity → on-device compositor + sprite
cache — is complete. M9 (audio loopback) is the next bring-up step,
and M9–M11.5 is where the diegetic-interfaces vision starts landing on
device.

| Milestone | Status | What it delivered |
|---|---|---|
| M1 — LED + USB serial | ✅ 05-18 | Toolchain validated, USB CDC heartbeat |
| M2 — E-paper hello | ✅ 05-18 | SSD1681 driver vendored, full + partial refresh |
| M3 — Provisioning | ✅ 05-18 | SoftAP + captive portal, NVS WiFi creds, WPA3-SAE verified |
| M4 — First sprite | ✅ 05-18 | HTTPS GET → 5000-byte panel bitmap → memcpy framebuffer (~1.0–1.2 s) |
| M5 — Pairing | ✅ 05-18 | `(name, PIN)` bind via `mochi.val.run/pair-device`, `pet_id` in NVS |
| M6 — Touch | ✅ 05-18 | FT6336 capacitive, ISR + queue, 5-zone routing |
| M7 — RTC | ✅ 05-18 | PCF85063 hand-rolled, coin-cell-backed across reflashes |
| M8 — Temp/humidity | ✅ 05-18 | SHTC3 hand-rolled, polling-mode (avoids ESP-IDF v5 stretch trap) |
| M8.5 — "Feels like Mochi" | ✅ 05-18 | Compositor (2-plane), scene + cell endpoints, status bar, LittleFS sprite cache w/ ETag invalidation, sleep gesture, factory reset, battery sense |
| M9 — Audio loopback | next | ES8311 codec; input side of the realtime agent loop |
| M10 — Realtime voice proxy | | Server-side realtime agent that holds the live scene contract |
| M11 — Pet state in C | | Port `decay.ts` + `engagement.ts` + `mood.ts` |
| M11.5 — Scene contracts | | Replaces hardcoded corner-icon UI; see `design/06-scene-contracts.md` |
| M12 — Event log | | LittleFS-backed |
| M13 — Sync model | | Push events / pull deltas |
| M14 — OTA updates | ✅ 05-19 | A/B partition table, `esp_https_ota` against GitHub Releases manifest, daily check + idle reboot, rollback on boot failure (`design/08-ota-updates.md`) |

> **The M8.5 corner-icon UI is scaffolding.** Care actions are fixed
> tap-zones in firmware today; the long-term shape is per-scene
> semantic regions supplied by the server (see
> `c15r/mochi:design/diegetic-interfaces.md` and our stub at
> `design/06-scene-contracts.md`). Expect M11.5 to retire the corner
> icons and the iOS-style status bar chrome.

## Editing the codebase

- **Server-side** (`main.tsx`, future routes under val.town): standard
  val.town conventions in `AGENTS.md`. Pushed via `vt push`.
- **Firmware** (`firmware/`): edit, then `idf.py build flash monitor`.
  `firmware/sdkconfig` is generated and ignored — change `sdkconfig.defaults`
  and re-run `idf.py reconfigure` to apply config tweaks.
- **`firmware/managed_components/`** is auto-fetched from the ESP
  Component Registry by `idf.py reconfigure` (driven by
  `firmware/dependencies.lock` + each component's `idf_component.yml`).
  It's `.vtignore`d and not committed. If a clean clone is missing it,
  run `idf.py reconfigure` once.
- **Memory / project notes for future Claude sessions** live under
  `~/.claude/projects/-Users-cdbeer-dev-mochi-esp-firmware/memory/`.
  These capture non-obvious gotchas (power rails, stack sizes,
  ESP-IDF v5 quirks, the empty-LiPo-connector trap) and are
  cross-referenced with `[[name]]` links.

## Cross-references

- workspace `c6a8376def6946` — hardware delivery + decision shape
- workspace `6985ca591aaa4e` — proj_eink_pet anchor
- workspace `c86d955c774340` — proj_mochi
- mochi.val.run / `c15r/mochi` — the web prototype this device embodies
- `c15r/mochi:design/diegetic-interfaces.md` — vision driving M9+
