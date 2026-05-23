# 18 — Device observability (telemetry + diagnostics to substrate)

**Status:** sketch, 2026-05-23. The device is now a first-class actor —
voice, imagine, travel, OTA, BYO-key spend — but everything it does or
fails at is visible only over a USB serial cable (`ESP_LOGx`) or a
post-hoc `voice_diag` dump. This doc adds an over-the-air observability
layer so a device in the field can be root-caused from SQL.

**Predecessors:** [16](./16-on-device-imagine.md) (imagine cost
telemetry, the first `cost_events` writer), [17](./17-location-embodiment.md)
(device telemetry gap), `c15r/mochi:backend/db.ts` (`cost_events` /
`realtime_sessions`).

## Two axes

Observability splits cleanly:

1. **Cost telemetry** — "what did it spend." Existing substrate tables
   `cost_events` (one-shot OpenAI calls) + `realtime_sessions`/`_turns`
   (voice). The device already writes imagine → `cost_events` (design/16);
   voice sessions remain web-only.
2. **Diagnostic log** — "what is it doing / what failed." NEW: a
   `device_logs` sink the firmware POSTs so boot reasons, errors, and
   subsystem outcomes are queryable without serial.

This doc owns axis 2 + closes the voice gap on axis 1.

## Diagnostic log

### Wire / storage

New substrate table `device_logs` (one row per record):

```
pet_id      TEXT     -- X-Pet-Id (denormalised; survives pet deletion)
boot_id     TEXT     -- random per boot; groups one power-cycle's records
fw_version  TEXT     -- esp_app_get_description()->version
at          INTEGER  -- device uptime ms at emit (monotonic; wall clock
                     --   is unreliable pre-time-sync)
level       INTEGER  -- 1 error · 2 warn · 3 info (esp_log order)
tag         TEXT     -- subsystem ("boot","wifi","pack_cache","imagine",…)
msg         TEXT     -- short human line
ctx         TEXT     -- optional JSON (reset_reason, heap, etag, …)
server_at   INTEGER  -- server receive time (wall clock)
```

Index `(pet_id, at DESC)`. Query shape:
`SELECT * FROM device_logs WHERE pet_id=? ORDER BY server_at DESC LIMIT 200`.

### Endpoint

`POST /api/device/diag` (X-Pet-Id), in a new `backend/device-diag.ts`
router mounted at `/api` (api.ts is at the file ceiling — same pattern as
places-device.ts). Body: `{ boot_id, fw_version, records: [...] }`, capped
(≤64/POST). Best-effort insert; returns `{ ok, inserted }`.

### Firmware: `device_diag` module

- `device_diag_init()` — alloc a small PSRAM ring buffer (≈64 records),
  mint a `boot_id`, snapshot `esp_reset_reason()` + free heap/PSRAM +
  fw version, and queue the **boot record** (tag `boot`). The reset reason
  is the single highest-value signal: it distinguishes a clean OTA reboot
  from a panic / watchdog / brownout — the first question when a field
  device misbehaves.
- `device_diag_event(level, tag, msg, ctx)` — append to the ring (mutex;
  drop-oldest on overflow with a truncation marker). Cheap; safe from any
  task. Also mirrors to `ESP_LOGx` so serial still works when attached.
- `device_diag_flush()` — batch the ring into one JSON POST to
  `/api/device/diag` (via `https_post`, X-Pet-Id). Clear on success;
  keep + retry on failure. Called periodically from the main loop (every
  few minutes, once WiFi is up) and opportunistically after the boot
  record so a crash-looping device still reports each boot.

Curated, not chatty: emit errors/warns + lifecycle outcomes, not every
`ESP_LOGI`. An e-ink device on battery shouldn't stream logs.

### Initial instrumentation (high-value sites)

- `boot` — reset reason, fw version, free heap/PSRAM (in `init`).
- `wifi` — join result (ssid, rssi) or failure.
- `pack_cache` — per sheet: server-fetched / cache / embedded + etag
  (is the sheet-OTA loop working?).
- `imagine` — terminal phase + fail reason.
- `voice` — session end summary (model, duration, end_reason) — also the
  cheap first cut of axis-1 voice telemetry.
- `ota` — check / stage / apply outcome.

## Voice cost telemetry (axis 1)

Fuller voice cost lands as a `realtime_sessions` row POSTed on
`stop_session` (model, voice, duration, turn_count, end_reason, est
tokens) — heavier (mirrors the web schema). The `voice` diag event above
is the lightweight stand-in until then; per-turn `realtime_turns` is a
later refinement.

## Phases

1. **Spine** — *done* (pending on-device validation). `device_logs` table
   + `/api/device/diag` (verified) + the `device_diag` firmware module +
   boot record + flush wiring (post-WiFi + every ~2 min).
2. **Instrument** — *done* (pending validation). Emits at boot, wifi,
   pack_cache (server/cache/embedded + why), imagine (ready/fail), voice
   (session end), ota (up-to-date / update / staged / failed), sleep,
   pair, render (cell-fetch fail), low battery, plus a periodic **health
   heartbeat** (~5 min: internal heap free + min watermark, PSRAM free,
   battery mv/pct, temp, uptime) so slow degradation has context.
3. **Voice cost** — *session-level done* (pending validation). On every
   voice-session end, `main.cpp` brackets the session off `is_active()`
   transitions and POSTs `/api/device/voice-session` → a
   `realtime_sessions` row (model `gpt-realtime`, voice `marin`, duration,
   end_reason; `config_snapshot.source="device"`). Zero changes to the
   WebRTC voice path. **3b** (deferred): per-turn token totals + est cost
   — needs parsing `response.usage` in the data-channel handler (the
   delicate, hardware-validation-only bit) and a turn counter.
4. **Crash detail** — surface the ESP-IDF core-dump summary (panic PC /
   backtrace) on the next boot, beyond the reset reason. **Gated on a
   `coredump` partition** (CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH) — a
   partitions.csv change, which is *not* OTA-safe (the partition table
   isn't OTA'd), so it needs a one-time USB reflash. Deferred until the
   next USB session; the boot record's reset reason (panic/watchdog/
   brownout) already gives the crash *class* over the air.

## Cross-references

- `c15r/mochi:backend/db.ts` — `cost_events` / `realtime_sessions` / (new) `device_logs`
- `firmware/main/device_diag.{c,h}` (new) · `voice/voice_diag.c` (the local serial dump this complements)
- `16-on-device-imagine.md` · `17-location-embodiment.md`
