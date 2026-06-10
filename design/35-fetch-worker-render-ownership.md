# 35 — Fetch worker: the main loop never blocks on the network

Status: implemented, 2026-06-10 (0.3.26)
Branch: `claude/mochi-beta-ota-patch-dbnf2p` (PR #25)
Closes: design/25 C1 (remaining cold case), C2, C6.

## The problem (design/25 §C, restated)

The main loop is the single owner of touch + panel. Anything
multi-second it calls inline freezes ALL input for that long — taps,
PWR gestures, voice start, renders. The C-series backlog worked this
down case by case (the 5 s hold, C3's post-voice pull in 0.3.25, the
#24 train's cache-first travel), but four inline round-trips remained:

| Path | Worst case | When it bites |
|---|---|---|
| `pet_sync_enter_place` on a nav_place tap | ~8 s | doze wake, radio still down |
| Cold travel-pack GET (first visit to a place) | ~8–23 s | every newly imagined place |
| Post-arrival ETag confirm (`pack_cache_refresh_geom`) | ~8 s HEAD | every travel, slow link |
| Cache-miss pet cell (`sprite_fetch_cell` in `render_with_expression`) | ~10 s | costume cells, esoteric expressions |

Plus the design/29 neighbour warm — already idle-gated, but still a
blocking HEAD on the loop's task.

## The shape: one worker, cache-only writes, main-owned render state

`fetch_worker` (new module) is a single serial task + request queue.
Every network round-trip the loop used to make inline becomes a queued
request; the loop polls a small result queue each tick.

**The ownership rule that makes this safe with no new locks:** the
worker never touches live render state. Packs are fetched into the
LittleFS cache only (`pack_cache_prefetch_geom[_ex]` — fetch, validate,
persist, free); cells go into `sprite_cache` only. The main loop
reloads from cache on its own task when a result or dirty flag tells it
to. The active `mpk_t`, framebuffers, and EPD remain main-owned exactly
as before. (LittleFS itself is mutex-protected at the VFS layer; a torn
read of a half-written blob fails MPK validation / size checks and
falls back, self-healing on the next tick.)

Serial on purpose: at most one TLS handshake at a time competing with
voice/imagine for mbedtls heap — same posture as the pet_sync push
worker.

## Per-path behaviour

**nav_place tap → enter.** Tap renders the "off to the X..." ack
(design/29), enqueues the POST, returns to `wait_event` immediately.
On the ok result the loop clears `last_location` so the travel block
re-evaluates (this also covers the deliberate tapped-the-place-I'm-in
re-blit, which used to ride on the inline clear). On failure the kid
gets the travel-fail bubble — **the old inline path failed silently**
(C6).

**Cold travel pack.** Travel block does the cache-only load; on miss it
enqueues the fetch (deduped by a pending-sheet latch) and stays quietly
on the current scene. Result ok → clear `last_location` → re-evaluate →
warm cache hit → swap + hybrid-refresh render. Result fail → the
existing fail bubble + 30 s backoff, warned-loc-gated so retries don't
re-flash the panel. Voice/drift travel gets the same treatment with no
bubble (matching idle-drift's *silent + faint trace* posture — the
scene just changes when it's ready).

**Post-arrival confirm.** Enqueued instead of inline. The worker uses
the new `pack_cache_prefetch_geom_ex(..., bool *out_changed)` so only a
*genuinely changed* pack posts a result; the loop then hot-swaps from
cache, preserving the current cell index. Unchanged/offline are silent.

**Pet cells (C2).** `render_with_expression` never fetches inline now.
On a pack+cache miss it enqueues the cell (per-key 60 s cooldown so a
cell the server doesn't have can't be re-asked every render) and
renders a fallback NOW: a costumed miss falls back to the base sheet's
same expression (the pet momentarily undresses rather than freezing —
and the costume pops in when the worker lands it, via the render-dirty
flag); a base miss falls back to the embedded pack's `neutral`. C6
resolves by construction: every tap visibly acts.

**Keepsake mirror.** The design/33 collect POST rides the worker too;
NVS was already the source of truth, so the tap stays instant and the
mirror is honestly best-effort.

**Neighbour warm (design/29).** Same one-per-idle-tick pacing, but the
warm itself runs on the worker; the idle gating now just keeps the
worker's TLS slot free for live requests.

## Deliberately left inline

- `render_asleep`'s cell fetch: pre-power-down, one-shot, and the
  embedded pack carries `sleeping` so the path is all but dead.
- `pet_sync_push_now` before deep sleep: the whole point is a
  synchronous best-effort flush.
- `dev_menu`'s sprite fetch: dev-only surface.
- The boot-time `net_worker` / `pet_sync` workers: already off-loop.

## Verification

- `idf.py build` green (esp32s3, ESP-IDF v5.3) at 0.3.26; no warnings
  in touched files.
- Not hardware-validated. The things to feel out on the beta device:
  travel to a *never-visited* place (ack bubble → scene appears a few
  seconds later, input live the whole time), tapping during what used
  to be the freeze windows, costume-cell pop-in, and the travel-fail
  bubble on airplane-mode taps.
