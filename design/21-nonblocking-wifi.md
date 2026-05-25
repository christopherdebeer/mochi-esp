# 21 — Non-blocking WiFi boot + dismissible re-provisioning

Status: firmware in progress (Phase 1 + Phase 2 dialog shipped, built
against ESP-IDF v5.3; on-hardware validation pending). 2026-05-25.

## Problem

`design/02-boot-sequence.md` promises that a paired device WARM BOOTs by
rendering the cached pet **immediately — "no network gate" — then kicks
WiFi + sync in the background**. The shipped firmware did the opposite:
`app_main` was one long synchronous function, and the first pet frame sat
*behind* the blocking WiFi connect, SNTP, a 2 s battery diagnostic, and
ETag HEAD probes. WiFi was on the critical path by sequence, not by
necessity — the embedded MPK packs already make the first render
network-independent.

We want two things:

1. **WiFi access is non-blocking.** The pet is on screen in well under a
   second on a warm boot; connectivity comes up behind it.
2. **We can still flip to SoftAP for (re)provisioning** — surfaced as a
   *dismissible dialog*, never a silent screen-takeover or auto-reboot.

## Phase 1 — non-blocking warm boot

The boot path now splits on pairing state:

| State | Path | WiFi |
|-------|------|------|
| no creds (or `prov_on_boot`) | `wifi_prov::run()` → persist → reboot | n/a (SoftAP) |
| creds, **no pairing** | WAITING-FOR-PAIRING — synchronous connect → pair → reboot | on critical path *by necessity* (pairing needs the network, and there's no pet to show yet) |
| creds **+ paired** | **WARM BOOT** — render first, connect after | off the critical path |

On the paired path `app_main` does only **local** init before the first
frame: RTC, SHTC3, codec, battery sense, LittleFS cache + event log,
PSRAM buffers, embedded scene/pet packs, care icons **from cache only**.
It renders `neutral` from the packs, starts the touch loop, and spawns
`net_worker`.

`net_worker` (`main.cpp`) owns everything that needs the radio, in order:

1. `wifi_sta::init_stack()` + `connect_any()`
2. on success → `s_net_phase = Online`; then OTA promote + poller,
   `time_sync_init()`, per-sheet ETag refresh, cold-cache care-icon
   fetch, `pet_sync_pull_now()`
3. on failure → `s_net_phase = Offline` (raises the dialog, below)

It **never renders** — the main loop stays the single owner of the panel.
The worker signals the loop through two `volatile` flags:

- `s_net_phase` — `Connecting` / `Online` / `Offline`
- `s_net_render_dirty` — set when fresh state, invalidated cache, or
  freshly-fetched icons mean the resting pet should be re-rendered

The loop tolerates "not connected yet" because the render path already
falls back pack → cache → network, the status bar gates the clock on
`time_sync_synced()`, and the dev-pet projection is seeded
(`init_dev_pet`) until the first `/api/state` pull lands.

### Things that moved off the critical path

- The 10-sample / 2 s battery LiPo-presence diagnostic (kept a single
  read for the boot log).
- ETag HEAD probes + cache invalidation → `net_worker`.
- Cold-cache care-icon network fetch → `net_worker` (warm boots load the
  downsampled planes straight from LittleFS).
- The key-portal autostart now waits for `Online` (it shows the device
  IP), one-shot, inside the loop.

## Phase 2 — dismissible re-provisioning dialog

When `net_worker` can't reach any stored network, the device does **not**
reboot into a SoftAP takeover (the old behaviour). The pet stays on
screen — fully usable offline (embedded art + local decay projection) —
and the loop overlays a dismissible card:

```
        ┌────────────────────────┐
        │        No WiFi         │
        │   Mochi is offline.    │
        │  Tap below to set up.  │
        │  ┌──────────────────┐  │
        │  │   Set up WiFi    │  │   ← inverted action button
        │  └──────────────────┘  │
        └────────────────────────┘
```

- **Tap the action** → set `prov_on_boot` + reboot into the proven
  SoftAP captive-portal flow (`wifi_prov::run`).
- **Tap anywhere else** → dismiss; the pet keeps running offline. Latched
  (`wifi_dialog_dismissed`) so it doesn't immediately re-appear.

The dialog is a new UI primitive — `ui_dialog::render()` — that stamps a
bordered white card into the existing composite framebuffer (1bpp
MSB-first, same as the compositor + thought bubble) and returns the
action's touch hit-rect. Unlike the canned full-frame screens in
`epd_ui` (provisioning, pairing, key-portal), a dialog is an **overlay**:
the caller composes the pet first, stamps the card, pushes one frame, and
dismisses by simply re-rendering the pet without it. This is the first
piece of an overlay/modal layer the design system previously lacked
(every prior "screen" was a mutually-exclusive takeover).

## Why the dialog action reboots (for now)

Mode transitions on this firmware are deliberately reboot-based:
`wifi_sta` and `wifi_prov` each call `esp_netif_init()` +
`esp_event_loop_create_default()`, so they can't coexist in one process,
and ESP-IDF v5.3's intra-process AP↔STA hand-off hangs intermittently
(project memory `project-eink-wifi-handover`,
`firmware/README.md` troubleshooting). Rebooting into SoftAP is the
robust path and reuses code that already works.

### Follow-up: live SoftAP flip without reboot

The fuller Phase-2 goal — bring SoftAP up *alongside* STA so the portal
hosts without losing the on-screen pet — needs a single `wifi_manager`
that owns netif + event-loop + driver init exactly once, creates both
STA and AP netifs up front, and flips to `WIFI_MODE_APSTA` on demand
(adding the AP to a running STA is more stable than the teardown the
v5.3 hang bites on). That refactor is deferred until it can be validated
on hardware — a clean build does not prove the radio transition. The
reboot path stays as the fallback regardless.

## Concurrency notes

- The main loop is the **only** task that draws to the panel during
  normal operation (`factory_reset` / `sleep_gesture` render only at
  terminal hand-off moments, as before).
- `net_worker` only writes: the care-icon PSRAM buffers (benign one-frame
  read/write race with `render_chrome`, self-healing next tick) and the
  LittleFS cache (joltwallet littlefs serialises FS ops internally;
  invalidate-then-miss is the intended cache-refresh semantics).
- `pet_sync` (mutex-protected snapshot + queue) and `device_diag`
  (mutex) are already built for multi-task use.
- `net_worker` runs TLS (sprite fetch, state pull, SNTP, OTA) so it gets
  a 16 KB stack, matching the 8 KB+ the main task needed for mbedTLS.

## Files

- `firmware/main/main.cpp` — boot split, `net_worker`, dialog wiring.
- `firmware/main/ui_dialog.{h,cpp}` — dismissible overlay primitive.

## Cross-references

- `02-boot-sequence.md` — the WARM BOOT promise this implements.
- `03-provisioning.md` — the SoftAP flow the dialog reboots into.
- `09-deep-review-2026-05-20.md` §1.1 — halt-loops; the unpaired path's
  pair-failure halts now name the PWR+BOOT reset gesture.
