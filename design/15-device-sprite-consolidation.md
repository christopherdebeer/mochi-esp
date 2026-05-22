# 15 — Device-sprite consolidation (encoder home + conformance)

Status: in progress, 2026-05-22. Phases 1–2 landed (single encoder + the
live serve path delegating to it + conformance; the consolidated studio +
`format=1` authoring + SPRITE·FORGE retired; boot-sync pack refresh).
Phase 3 (device-side generation → substrate) planned, tracked in `design/16`.

**Predecessors:** [05-sprite-format.md](./05-sprite-format.md) (the cell
wire format), [13-build-time-asset-packs.md](./13-build-time-asset-packs.md)
(the MPK1 build-time pack), [14-mpk1-edges-and-actions.md](./14-mpk1-edges-and-actions.md)
(format=1 zones + typed actions).

## Why

Device sprite *authoring*, *generation*, and *serving* had smeared across
four runtimes — the val backend (`devsprite*`, `sheets`, `scene-plans`),
the val web frontend (`/dev`, `places-client`), the GitHub static site
(`sprite-forge.html`), and the firmware — and the device-cell encoder had
begun to fork (the server `devsprite-encode.ts` vs a re-implementation in
the browser packer). Maintaining two consumers of one substrate while the
encoder existed in more than one place is the unwieldiness this
consolidation removes.

The principle: **the canonical sheet PNG in substrate is the single
source of truth; the device cell is the single device representation;
everything else is a derived projection produced by one encoder.**

## Where the encoder lives now

The single device-cell encoder lives in the **`c15r/mochi-device`** val
(`encode.ts`), alongside the MPK1 packer (`mpk.ts`) and the conformance
harness (`conformance.ts`). It reads the same account-scoped substrate
(`std/blob` + `std/sqlite`) that `c15r/mochi` (mochi.val.run) uses, so the
two vals share one set of pet/scene sheets with no migration.

`c15r/mochi`'s `backend/devsprite-encode.ts` is now a thin re-export of
`c15r/mochi-device/encode.ts`. The runtime serve path
(`mochi.val.run/devsprite/*`) is unchanged on the wire — device URLs stay
frozen — but flows through the one encoder. The cross-val re-export is
**version-pinned** (`@40-main`): an unpinned `esm.town` import caches at
first resolution and silently serves a stale snapshot after the source
val is edited (this bit us once — every `/devsprite/cell/*` 500'd against
a snapshot missing a newly added export). Bump the pin in lockstep when
`mochi-device` ships a change the serve path needs.

## One encoder, two providers, one format

A device cell is delivered two ways, byte-identical, so the firmware
composites both through the same `compositor::blit_two_plane`:

- **Runtime fetch** — `GET /devsprite/cell/<sheet>/<cell>` (8-byte BE
  header + ink [+ mask]), cached in LittleFS, ETag-versioned.
- **Build-time pack** — an MPK1 `.bin` embedded via `EMBED_FILES` and read
  by `firmware/main/mochi_pack.h` (`pet_pack.c`, `scene_pack.c`). Each
  entry's cell blob is byte-identical to a `/devsprite/cell` response.

### Boot-sync pack refresh

The embedded pack is the *baseline*, not the ceiling. `firmware/main/pack_cache.cpp`
(`pack_cache_active`) lets a device pick up studio-authored sheet edits
without a reflash: at boot each pack consumer asks for its sheet
(`pet_pack` → `pet-v1`, `scene_pack` → `scene-bundle-a`) and gets back the
freshest available bytes, preferring **server → LittleFS cache → embedded
baseline**:

1. HEAD `GET /devsprite/pack/<sheet>` for the server ETag
   (`src-s{STROKE}-pp{POSTPROCESS}-z{fnv1a(zones)}` — changes whenever the
   sheet PNG, post-process, or zones change in substrate).
2. ETag differs (or no cache) → GET the pack into a persistent PSRAM
   buffer, validate the MPK1 envelope, write blob + ETag to LittleFS.
3. ETag matches → load the cached pack from LittleFS (no body fetch).
4. Any failure (offline, bad body, FS error) → cached pack if present,
   else the embedded baseline. A device with no network always boots.

So the loop the studio closes is: author a sheet → it lands in substrate →
the server pack endpoint re-derives + re-ETags → the device pulls it on its
next boot. Cutting a firmware release only updates the *baseline* (the
fail-safe); the live artwork tracks substrate. The pack ETag is a separate
cache key (`<sheet>.pack`) from the per-cell ETag probe, so the two don't
collide.

## Conformance

Two halves pin the format so the producers can't drift:

- **Val side** — `c15r/mochi-device/conformance.ts` asserts (a) the lifted
  `encode.ts` is byte-identical to the previous `c15r/mochi` encoder
  across the whole API, and (b) `mpk.ts` output round-trips through a
  line-faithful TS port of `mochi_pack.h` (`mpk_open/find/ink/mask`),
  with container math pinned to the shipped packs (`label_len=16`,
  pet stride `2328`, scene `cell_bytes 10008`).
- **Firmware side** — `firmware/scripts/verify-mpk.py` validates the
  embedded `assets/*.mpk` against the `mochi_pack.h` contract (header,
  stride math, per-entry cell-header consistency, size reconciliation).

The shipped `pet_a.mpk` (96×96, 18 cells) and `scenes_a.mpk` (200×200,
16 cells) both validate; the val packer reproduces their container shape.

## Phases

1. **One encoder** — *done*. Encoder lifted to `c15r/mochi-device`,
   live serve path delegates to it (verified byte-perfect end-to-end),
   MPK1 packer aligned to the device reader, substrate sharing confirmed.
   The canonical per-type post-process (scene line-screen, pet diagonal
   line-screen, icon flat threshold) is the default for every sheet kind.
   Plus **boot-sync pack refresh** (above): the firmware pulls
   substrate-authored packs over the network with the embedded pack as
   fail-safe, so a studio edit reaches the device on its next boot.
2. **One studio surface** — *done*. The Device Sprite Studio (the
   `c15r/mochi-device` HTTP val) folds upload→substrate, grid override,
   cell preview, and `format=1` zone/action authoring into one surface,
   backed by the single encoder. SPRITE·FORGE is retired to a redirect
   (`site/tools/sprite-forge.html`); `scene-bundle-a`'s zones were
   recovered from the firmware-bundled `scenes_a.mpk` into substrate.
3. **Unified generation → substrate** — device-side image generation
   writes canonical sheets back to substrate (the "one artifact, two
   renderers" model); web-gen and device-gen share one flow. (See
   `design/16-on-device-imagine.md`.)

## Cross-references

- `05-sprite-format.md` — the cell wire format the encoder emits
- `13-build-time-asset-packs.md` — the MPK1 pack the encoder feeds
- `14-mpk1-edges-and-actions.md` — `format=1` zones + actions (Phase 2)
- `firmware/main/mochi_pack.h` — the device reader the packer targets
- `firmware/main/pack_cache.cpp` — boot-sync pack refresh (server → cache → embedded)
- `firmware/scripts/verify-mpk.py` — firmware-side pack validator
- `c15r/mochi-device` — the val that now owns the encoder + packer
