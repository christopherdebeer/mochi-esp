# 29 — nav_place responsiveness: eager neighbour prefetch

Status: firmware implemented (this branch), needs on-device verification.

## Problem

Tapping a `nav_place` zone (cross-place travel, design/17) feels slow and
often "unresponsive" on the device. Two distinct stalls, both blocking:

1. **The tap itself blocks on a network POST.** `main.cpp`'s touch handler
   calls `pet_sync_enter_place(place_id)` synchronously
   (`firmware/main/main.cpp:3007`). That POST to `/api/places/:id/enter`
   (`pet_sync.cpp:404`) does a full TLS round-trip before *anything*
   visible happens — so the tap reads as a dead button for ~300–800 ms.

2. **The scene swap does a cold pack fetch on the main loop.** The travel
   block (`main.cpp:2579`) calls `pack_cache_active_geom(sheet, 200, 200,
   nullptr)`, which HEAD-probes the ETag and, on a cold cache, GETs the
   whole 150–320 KB pack over HTTPS (`pack_cache.cpp:105`) — ~1–2 s — then
   repaints e-ink (~1 s). Cold, every first visit to a place is ~2–4 s.

There is **no prefetch**: nothing warms a place pack until the moment you
travel to it.

## Key enabling fact

The device already knows, on every `/api/state` pull, the `sheetId` of
every reachable place: the response carries a root-level `places[]` of
`{id, sheetId}` (`pet_sync.cpp:184`). Today the parser only uses it to
resolve the *current* location's sheet. So mapping a `nav_place` target id
→ device sheet id needs **no new server endpoint** — it's already on the
device. That makes prefetch a firmware-only change.

And the reachable targets are enumerable from the loaded pack itself:
every `nav_place` zone (kind 5) stores its target place id in
`seed_text`/`seed_len`, readable for *any* cell via `mpk_zone_get`
(`mochi_pack.h:198,244`) without disturbing the current index.

## Design

Warm the LittleFS pack cache for the places reachable from the **currently
loaded** pack, in the background, so onward travel hits the warm path
(ETag-match → LittleFS load, ~50–100 ms, no body GET) instead of a cold
GET.

### 1. Enumerate reachable targets — `scene_pack`

`scene_pack_collect_place_targets(char ids[][SCENE_PLACE_ID_MAX], max)`
walks every cell × every zone of the active pack, collects the distinct
`MPK_ACTION_NAV_PLACE` target ids, and returns the count. Format=0 packs
(the embedded `scenes_a` home bundle) have no inline `nav_place` zones, so
this naturally returns 0 there — home navigates within itself via
`nav_relative`, no fetch, nothing to prefetch.

### 2. Resolve id → sheet — `pet_sync`

The `/api/state` parser now caches the full `places[]` map (id → sheetId,
up to `PS_MAX_PLACES`) on every pull/mutate, and
`pet_sync_resolve_place_sheet(id, out, cap)` looks a neighbour up. A target
with no resolvable sheet (e.g. `"home"`, or a world place not yet linked to
this pet) is skipped.

### 3. Warm the cache without leaking PSRAM — `pack_cache`

`pack_cache_active_geom` returns a *never-freed* PSRAM buffer (it's the
live pack). Prefetching N neighbours that way would leak N×~160 KB. So
`pack_cache_prefetch_geom(sheet, cw, ch)` is a sibling that HEAD-probes,
GETs + stores to LittleFS **only if the ETag changed/missing**, and then
**frees** the working buffer. When the ETag already matches it returns
immediately (a cheap HEAD, no alloc, no GET). Best-effort: any failure
just means the eventual real load does the cold fetch as before.

### 4. Drive it from the main loop, not a new task

A dedicated FreeRTOS task doing TLS fetches concurrently with the main
loop's own fetches risks mbedTLS heap pressure (two handshakes at once) —
and the codebase elsewhere assumes one fetch in flight. Instead, on a
successful travel swap the main loop fills a small prefetch queue (the new
pack's neighbour ids) and drains **one entry per idle tick** — only when
online, not in a voice/imagine/portal/menu state, and with no touch
pending. Each drain blocks that single tick (~0.1 s warm HEAD, up to
~1.5 s cold GET), but touches between ticks are still serviced, so the
cost is spread and never stacks. Re-filled on every travel.

### 5. Immediate tap feedback (the "unresponsive" symptom)

Before the blocking `enter_place` POST, the `nav_place` handler now paints
a transient "off to the …" thought bubble so the tap is acknowledged
instantly instead of reading as a dead button. The travel block repaints
the real scene a tick later as before.

## Why not the alternatives

- **Server neighbour-manifest endpoint** — unnecessary; `places[]` already
  ships in `/api/state`.
- **Skip the HEAD on a just-prefetched pack** (`pack_cache_load_geom_only`
  on the travel path) — would shave the warm-path HEAD too, but risks
  serving a stale pack if the author republished between prefetch and
  travel. Left out; the warm ETag-match path is already fast.
- **Prefetch on a background task** — TLS concurrency / heap risk; the
  idle-tick drain achieves the same warmth with one fetch in flight.

## Status / verification

Implemented firmware-side; **builds clean** under ESP-IDF v5.3
(`idf.py build`, esp32s3, `-Werror`, no warnings — verified locally).
**Not yet verified on hardware** — needs a device run to confirm: (a)
onward travel from a
freshly-loaded place is warm (look for `pack_cache: prefetch` diag events
and a `ETag unchanged` on the subsequent real travel), and (b) the idle
drain doesn't visibly hitch the pet animation.
