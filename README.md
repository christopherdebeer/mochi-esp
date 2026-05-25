# mochi-esp

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
mochi-esp/
├── README.md            this file
├── AGENTS.md            val.town code-gen guidelines (server side)
├── main.tsx             server-side root (HTTP val; currently a stub)
├── design/              architecture + per-decision design docs (00–16)
│   ├── 00–06                   architecture · bring-up · boot seq · provisioning · pairing · sprite format · scene contracts
│   ├── 07-voice-architecture.md   device → OpenAI Realtime, BYO key in NVS (M9–M10)
│   ├── 08-ota-updates.md          A/B partitions + GitHub Releases manifest (M14)
│   ├── 09–12                   deep review · QR codes · pet-state-in-C · thought bubble
│   ├── 13-build-time-asset-packs.md  MPK1 embedded sprite packs
│   ├── 14-mpk1-edges-and-actions.md  format=1 inline zones + typed actions
│   ├── 15-device-sprite-consolidation.md  one encoder/studio/format + boot-sync packs
│   └── 16-on-device-imagine.md    speak a place into being (gpt-image-2, BYO key)
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

The val.town side (`c15r/mochi` at mochi.val.run, plus the
`c15r/mochi-device` studio + canonical encoder) hosts: the `/devsprite/*`
family (panel bitmaps, native cells with mask plane, scenes, and MPK1
`/devsprite/pack/<sheet>`), the device-pairing flow, the on-device
imagine endpoints (`/api/places/:id/orchestration`, `/sheets/:id/guide.png`),
and the realtime-voice token mint. The realtime voice path (device →
OpenAI Realtime, BYO key on-device) has landed — see
`design/07-voice-architecture.md`. See `design/00-architecture.md` for
the device-vs-server partition.

## Building and flashing

The firmware isn't built by val.town — it's an ESP-IDF project that
compiles locally. From a clean `vt clone`, one command installs the
toolchain (ESP-IDF v5.3 + esp32s3), and it's safe to re-run in a fresh
container or web session:

```sh
firmware/scripts/setup-esp-idf.sh               # install ESP-IDF + toolchain (~2 GB, first run)
. ~/esp/esp-idf/export.sh                        # activate the IDF env
cd firmware
idf.py set-target esp32s3                        # one-time
idf.py reconfigure                               # fetches managed_components/
idf.py -p /dev/cu.usbmodem* flash monitor        # build + flash + tail
```

Repeatable setup (incl. remote/web sessions): **`firmware/TOOLCHAIN.md`**.
Port-name conventions per OS, flashing, and troubleshooting:
**`firmware/README.md`**.

## Status (as of 2026-05-24, release v0.0.19 · v0.0.20 pending)

The architectural spine through realtime voice, OTA, and on-device pet
state is complete. The **device-sprite pipeline** (`design/13`–`16`) —
one encoder/studio/format, boot-sync MPK1 packs, on-device imagine —
landed, then grew into **location & world** (`design/17`): the device
follows `pets.location`, travels between places (`nav_place` /
`nav_scene`), and renders costumes. The current track is the **studio
world consolidation** (`design/19`): the Device Studio is now the world-
authoring surface — author/generate a scene *plan* (per-cell zoned rooms
+ a `nav_place` graph), project it to the device pack, and register
global world places the device travels to. **v0.0.20** packages the
travelled-to scene-pack fix + travel telemetry (device code for the
consolidation already shipped through v0.0.19; the rest is server-side
and boot-syncs).

| Milestone | Status | What it delivered |
|---|---|---|
| M1–M8 | ✅ 05-18 | Toolchain → e-paper (SSD1681) → SoftAP provisioning → first HTTPS sprite → `(name,PIN)` pairing → FT6336 touch → PCF85063 RTC → SHTC3 temp/humidity |
| M8.5 — "Feels like Mochi" | ✅ 05-18 | 2-plane compositor, scene + cell endpoints, status bar, LittleFS sprite cache w/ ETag invalidation, sleep gesture, factory reset, battery sense |
| M9 — Audio loopback | ✅ | ES8311 codec; input side of the realtime agent loop |
| M10 — Realtime voice | ✅ | Device → OpenAI Realtime over WebRTC, BYO key on-device (`design/07`) |
| M11 — Pet state in C | ✅ | Ported `decay` + `engagement` + `mood`; `design/11` |
| M12 — Event log | ✅ | LittleFS-backed on-device event log |
| M13 — Sync model | ✅ | Push events / pull deltas (`pet_sync`) |
| M14 — OTA updates | ✅ 05-19 | A/B partitions, `esp_https_ota` against the GitHub Releases manifest, daily check + idle reboot, rollback on boot failure (`design/08`) |

### Device-sprite pipeline (design/13–16)

| Track | Status | What it delivered |
|---|---|---|
| Build-time packs | ✅ | MPK1 packs embedded via `EMBED_FILES`, byte-identical to `/devsprite/cell` (`design/13`) |
| Format=1 zones | ✅ | Inline tap zones + typed actions (event / nav / talk-seed) in the pack (`design/14`) |
| Consolidation | ✅ | One canonical encoder + the Device Sprite Studio; SPRITE·FORGE retired (`design/15`) |
| Boot-sync packs | ✅ v0.0.16 | `pack_cache` pulls substrate-authored packs at boot (server → cache → embedded fail-safe) — studio edits reach the device with no reflash |
| On-device imagine | ✅ v0.0.17* | Voice tool runs gpt-image-2 with the BYO key on-device, uploads the PNG, swaps the new scene in (`design/16`). *Pending on-device validation + the re-render-on-DONE wire. |

> **The corner-icon UI is being retired.** Scenes now carry authored
> tap zones with typed actions (MPK1 `format=1`, `design/14`); the fixed
> corner quadrants remain only as a fallback for unzoned scenes.

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

- workspace `1d6a2a83d3194f` — proj_eink_pet / mochi-esp (project node)
- workspace `71770d4f810341` — device-sprite consolidation + imagine shipped receipt
- workspace `c6a8376def6946` — hardware delivery + decision shape
- workspace `c86d955c774340` — proj_mochi
- mochi.val.run / `c15r/mochi` — the web prototype this device embodies; `c15r/mochi-device` — studio + canonical encoder
