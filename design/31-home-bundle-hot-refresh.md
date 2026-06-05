# 31 — Home-bundle hot refresh

## Problem

Authored edits to the home bundle (`scene-bundle-a`) weren't reaching the
device reliably — "timely or otherwise."

The only refresh path was a **once-per-boot** ETag probe in `net_worker`
(`scene_pack_init()` → `pack_cache_active()`). Two things broke it:

1. **Deep sleep (design/26 + the 0.3.15–0.3.18 work)** means the device only
   boots ~every 2 h unattended — so even the best case was a ~2 h propagation
   delay.
2. The per-boot probe **races the 60 s doze** that drops WiFi. Telemetry
   (`device_logs`) showed `scene-bundle-a` repeatedly loading
   `{"src":"cache","why":"offline"}` — the probe ran after the radio was
   already dropped, so it never even reached the server and the stale cache
   persisted indefinitely.

There was no force path and no live swap: a change could only appear after a
clean online boot.

## Design

Make `/api/state` carry a cheap signal, and have the device act on it live.

### Server (`c15r/mochi` val)

- `devsprite.ts` exports **`packContentSig(sheet)`** — the change-detecting
  subset of the `/pack` wire ETag (`srcEtag` + stroke/postprocess versions +
  `zoneSig`), computed **without building the pack** (no per-cell derive, no
  geometry projection). Memoised ~10 s.
- `api.ts buildStateResponse()` surfaces it as **`homeEtag`** on every
  `/api/state` (and every mutate response). Best-effort — a substrate hiccup
  never breaks `/state`.
- `/pack` drops `stale-while-revalidate=86400` (→ `max-age=60`). Otherwise the
  edge (Cloudflare) could serve the device's authoritative ETag HEAD a
  day-stale ETag, and a triggered refresh would probe, see "unchanged," and
  skip the GET — defeating the whole mechanism.

`homeEtag` is only a **trigger**; it deliberately omits `navTag`/`geomTag`.
The device's own HEAD to `/pack` remains the authoritative change check.

### Firmware (0.3.18)

- `pet_sync`: parse `homeEtag`, store under `s_mtx`, expose
  `pet_sync_home_etag()`.
- **`pack_cache_refresh(sheet, *out_synced)`** — non-geom sibling of
  `pack_cache_refresh_geom`: online HEAD probe, GET-on-change, persist to
  LittleFS, return PSRAM bytes for a live swap. `out_synced` distinguishes
  *fetched / confirmed-unchanged* from *offline / failed* so a transient
  failure retries instead of being recorded as current.
- **`scene_pack_reload_home(mpk, *swapped)`** — hot-swap that also updates the
  home baseline (`s_home_bytes`), so a later return-from-travel shows the new
  bundle. Swaps the active pack only when currently at home (`s_is_bundle`).
- Main loop: on a `homeEtag` change (a cheap string compare — **no per-tick
  HEAD**), refresh + cache + hot-swap the bundle **without waiting for the next
  boot**. Records the synced signature only on a definitive server answer.

## Result

Edit the home bundle → within one `/api/state` poll (or any mutate, e.g. a
tap) the device sees the new `homeEtag`, pulls the new pack, and repaints
live. The "up to 2 h, or never" path is gone.

Activates once the device runs ≥ 0.3.18; the server field is harmless to
older firmware (ignored).
