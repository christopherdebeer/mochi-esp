# 15 — Device-sprite consolidation (encoder home + conformance)

Status: in progress, 2026-05-22. Phase 1 landed (single encoder + the
live serve path delegating to it + conformance). Phases 2–3 planned.

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
frozen — but flows through the one encoder. (The cross-val import is
unpinned during active consolidation; pin to a version before treating
the serve path as frozen for production.)

## One encoder, two providers, one format

A device cell is delivered two ways, byte-identical, so the firmware
composites both through the same `compositor::blit_two_plane`:

- **Runtime fetch** — `GET /devsprite/cell/<sheet>/<cell>` (8-byte BE
  header + ink [+ mask]), cached in LittleFS, ETag-versioned.
- **Build-time pack** — an MPK1 `.bin` embedded via `EMBED_FILES` and read
  by `firmware/main/mochi_pack.h` (`pet_pack.c`, `scene_pack.c`). Each
  entry's cell blob is byte-identical to a `/devsprite/cell` response.

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
2. **One studio surface** — fold `/dev` upload + chroma-key + grid
   override, the devsprite dashboard, the Dither Workbench, and the
   sprite-forge MPK1/zone authoring into one device-sprite studio in the
   device val. Adopt `design/14` `format=1` so zones + typed actions are
   authored from the start (rather than `_meta.h` C tables).
3. **Unified generation → substrate** — device-side image generation
   writes canonical sheets back to substrate (the "one artifact, two
   renderers" model); web-gen and device-gen share one flow.

## Cross-references

- `05-sprite-format.md` — the cell wire format the encoder emits
- `13-build-time-asset-packs.md` — the MPK1 pack the encoder feeds
- `14-mpk1-edges-and-actions.md` — `format=1` zones + actions (Phase 2)
- `firmware/main/mochi_pack.h` — the device reader the packer targets
- `firmware/scripts/verify-mpk.py` — firmware-side pack validator
- `c15r/mochi-device` — the val that now owns the encoder + packer
