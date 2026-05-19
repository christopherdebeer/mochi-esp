# 00 — Architecture

Status: draft, 2026-05-17

## Framing

The web prototype `c15r/mochi` is **server-canonical**: mochi.val.run holds
the source of truth for every pet, the React frontend is a viewport that
fetches state and posts mutations through `apiFetch`. Latency is hidden by
optimistic updates but the substrate lives on the server.

The device target inverts this. **Device-canonical, server-as-services.**

- The pet lives on the device. Its state, its event log, its sprite cache,
  its decay clock — all on flash and PSRAM. Taps respond locally with no
  round-trip. Power off, power on, the pet is still there at the same age.
- mochi.val.run becomes three services the device calls *out to*:
  1. **Voice proxy.** WebSocket between device and a server-mediated
     Realtime API session, opened only when voice is invoked.
  2. **Sync target.** Periodic event-log upload + delta-state pull, so the
     pet has cross-device durability and the web app stays usable as a
     co-equal client.
  3. **Sprite generator.** Costume generation (LLM-driven) and conversion
     to device-native format remain server-side; device fetches finished
     1-bit bitmaps over HTTPS.

The web frontend (`c15r/mochi`) keeps working unchanged through this; it
remains a server-canonical client. Device adds a *second* client to the
substrate, with its own caching and authority semantics.

## Partition: what lives where

```
┌─────────────────────────── device ────────────────────────────┐
│  pet state (NVS)         current costume sheet (LittleFS)     │
│  event log (LittleFS)    decay clock (RTC)                    │
│  stats projection        mood projection                       │
│  engagement signal       sprite renderer                       │
│  touch handler           power management                      │
└──────────────────┬────────────────────────────────────────────┘
                   │  HTTPS (REST) for sync + sprites
                   │  WSS for voice (on demand)
                   ▼
┌──────────────────────── mochi.val.run ────────────────────────┐
│  pet rows (SQLite, source of truth for cross-device)          │
│  full event history       sleep consolidation (LLM)            │
│  costume generation       sprite bitmap conversion             │
│  device pairing endpoint  voice proxy (WebSocket → Realtime)   │
└───────────────────────────────────────────────────────────────┘
```

## What ports cleanly from the web prototype

The `shared/*` discipline in `c15r/mochi` is paying off here. Each pure
module is environment-free and ports to C with mechanical translation:

| Web module                | Device equivalent              | Notes                              |
| ------------------------- | ------------------------------ | ---------------------------------- |
| `shared/decay.ts`         | `decay.c`                      | Fixed-point for `ageMultiplier`    |
| `shared/engagement.ts`    | `engagement.c`                 | Ring buffer over recent events     |
| `shared/mood.ts`          | `mood.c`                       | Identical floor/gate semantics     |
| event log (SQLite rows)   | append-only file in LittleFS   | Same kind+ts+payload shape         |

The web app's `apiFetch` model does *not* port — the device authors its own
state, only syncing when it chooses.

## What re-architects

- **State authority + sync.** Today: server-canonical, client lazy-fetches.
  Device: locally canonical, sync is event-log push + delta-state pull
  when network is available. Conflict resolution becomes a real concern
  when the same pet is touched on web and device simultaneously — see
  `06-sync-model.md` (TODO).
- **Sprite delivery.** Today: PNG sheets (50–200KB) with chroma-keying done
  at render time. Device: server pre-renders to 1-bit packed bitmaps at
  sheet-creation time; device fetches the cheap format. See
  `05-sprite-format.md` (TODO).
- **Identity.** Today: name+PIN on a real keyboard, localStorage holds the
  pet ID after auth. Device: name+PIN doesn't translate; a pairing flow
  binds the device hardware ID to an existing pet. See `04-pairing.md`
  (TODO).

## What stays server-only

- Realtime voice (OpenAI Realtime API, accepted as network-required)
- Costume generation (LLM-driven, expensive, slow, runs once per costume)
- Sleep consolidation (LLM-driven, only runs during sleep, graceful
  degradation if offline — device just gets no new dreams)

## Decisions still open

1. Sprite format — per-frame 1-bit vs per-sheet 1-bit + grid metadata
   (`05-sprite-format.md`)
2. Pairing endpoint scope — add to mochi.val.run as part of bring-up, or
   hardcode a test pet for M4 (`04-pairing.md`)
3. Pet plurality per device — 1:1 default, or multi-pet from day one
4. Wake gesture — tap-to-talk button vs touch long-press vs wake-word

These are tracked in their respective design docs; this overview only
names them.

## Cross-references

- workspace `c6a8376def6946` — hardware delivery + decision shape
- workspace `bd8d86b98e6b46` — substrate/blob split (load-bearing for
  sprite format)
- workspace `a57fe5ca05484f` — engagement-aware tempo (RTC requirement)
- workspace `ffeedccaaea343` — name+PIN identity (needs device-side
  pairing extension)
