# 17 — Location, embodiment & the world model (reconciliation)

**Status:** sketch, 2026-05-22. Reconciles four tracks that grew
independently and now overlap on the device: **world-building** (places +
`move_to_location` + imagine), **costumes** (wardrobe + `current_costume_id`),
**idle drift** (autonomous movement), and **diegetic interfaces** (zones
as UI). Triggered by on-device imagine (design/16) landing — the first
time the device reaches into the substrate world model — which surfaced a
geometry seam and an authority question neither side had answered.

**Predecessors:** world-building (`c15r/mochi:design/world-building.md`),
idle-drift (`c15r/mochi:design/idle-drift.md`), wardrobe
(`c15r/mochi:design/wardrobe.md`), [06-scene-contracts.md](./06-scene-contracts.md)
(diegetic interfaces, device side), [13](./13-build-time-asset-packs.md)–[16](./16-on-device-imagine.md)
(the device-sprite pipeline).

## What's broken — the seams

### 1. Two (really three) location/scene systems that don't reconcile

| System | Geometry | Where | Navigation | Substrate? |
| :--- | :--- | :--- | :--- | :--- |
| `scene-bundle-a` | 200×200, 16 cells | firmware (embedded MPK1) | authored MPK1 zones (`nav_relative` door/back, `nav_scene`) | no — pure device-local |
| `places` | 360×336, day/night | web (`c15r/mochi`) | `move_to_location(id)`, idle drift, imagine | yes — `places` table, `pets.location` |
| `scene_plans` | 200×172, device-native | substrate (`scene_plans` table) | zones (design/06 diegetic) | yes — but no device consumer yet |

The **substrate authority for "where is mochi" is `pets.location`** (a
place id; `db.ts:76`). The **device authority is a bundle index**. They
never exchange a byte:

- `move_to_location("kitchen")` (voice) sets `pets.location` and changes
  the **web** view; the device shows nothing.
- Tapping a door on the device walks the bundle; the substrate never
  learns; the web still shows the old place.
- **Idle drift** (`maybeDriftPet`, `pets.last_drift_at`) relocates mochi
  server-side on its own; the device is oblivious.

### 2. Geometry mismatch — the device can't render a place

The device renders 200×200 scene cells (`scene_pack_blit_current` is a
verbatim row-copy; it *rejects* a width ≠ 200). Places are authored at
360×336 (web geometry). So **on-device imagine (v0.0.17) swaps the device
to a place pack it cannot blit** — the generated scene wouldn't display.
This is the load-bearing gap: the imagine feature isn't actually visible
on hardware until the geometry is reconciled.

The encoder already resizes arbitrarily (`cellToNativePipeline(png,
crop, cellW, cellH, …)`), so the fix is a **device-geometry projection**
of the same source art, not new art — squarely the design/15 principle
("one artifact, two renderers").

### 3. Costumes don't reach the device at all

`pets.current_costume_id` + the `costumes` table + `wear_costume` /
`take_off_costume` voice tools all work web-side; the costume sheet is a
full 18-cell pet sheet (`costume-{pet}-{costume}-v1`) that the renderer
treats as a species-sheet swap. The device renders the pet from the
embedded `pet_a` pack via `pet_pack` and **ignores `current_costume_id`**.
So a kid who dresses mochi sees it on the web but not on the toy.

### 4. `/api/state` already carries the world; the device drops it

The state response includes `location`, `places[]` (each with `sheetId`),
`currentCostumeId`, and `lastDriftAt`. `pet_sync`'s parser reads stats +
transient mood and **discards all four**. The wire is already there.

## The unified model

**One authority, one source, two renderers.**

1. **`pets.location` is the single source of truth for "where".** The
   device becomes a *renderer* of it, not a parallel authority. Web and
   device both project the current place.
2. **One source artwork per place, projected per consumer.** The place
   PNG in substrate (360×336 day/night) is projected to the web at
   360×336 and to the device at 200×200 by the same encoder — a
   `/devsprite/pack/<sheet>` device-geometry variant. No second art set.
3. **`pets.current_costume_id` is the single source of truth for "who".**
   Both renderers swap the pet sheet to the costume sheet when set; the
   device pulls the costume's device cells the same way it pulls `pet-v1`.
4. **Movement is authored, typed, and bidirectional.** A device tap that
   means "go to place X" uses a new MPK1 action `nav_place(place_id)`
   (design/14 reserved 5..255) that swaps the device scene *and* POSTs the
   move so substrate follows. Voice `move_to_location` and idle drift flow
   the other way: `pet_sync` sees `location` change and re-renders.

So all three movers — voice, drift, imagine — converge on `pets.location`,
and the device re-renders whenever it changes. `scene-bundle-a` and
`scene_plans` become *device-geometry scene sources a place can point at*,
not separate worlds.

## Per-track reconciliation

### Location / world-building

- `pet_sync` parses `location` + `places[]` (id→sheetId map) from the
  state response it already fetches. On a `location` change it fetches the
  place's pack at device geometry and `scene_pack_load_bytes`-swaps. This
  single wire makes `move_to_location`, drift, and imagine all visible on
  the device and **durable across reboot** (location lives in substrate,
  not the in-RAM swap).
- Imagine sets `pets.location` to the new place on success (a "travel
  there" step) — today it only marks the place *ready*, so web + device
  disagree on "where am I". Add `POST /api/places/:id/enter` (device-facing,
  in `places-device.ts`) and call it after the ready step.

### Geometry projection

- `/devsprite/pack/<sheet>` gains an output-cell-size override (e.g.
  `?cw=200&ch=200`): crop per the real template grid, but encode each
  cell at the requested device size via `cellToNativePipeline`. Cache key
  folds in the output size. `pack_cache` already speaks this route; the
  device just appends the device geometry.
- Day/night: a place pack has 2 cells. The device picks by **RTC**
  (PCF85063) — day vs night by local hour — rather than door/back. A
  small `scene_pack` policy for 2-cell packs; ignored for the 16-cell
  bundle.

### Costumes

- The device renders `pets.current_costume_id`: when set, `pet_pack`/the
  cell-fetch path pulls the costume sheet's device cells (same encoder,
  same `/devsprite/cell` path) instead of embedded `pet_a`. When null,
  the base species pack. `pet_sync` already needs to read the field (it's
  in the state response); the render swap is the new part.
- Deferred to a later phase than location (location is the prerequisite
  embodiment; costumes layer on the pet overlay, which is orthogonal).

### Idle drift

- No new mechanism — drift is already a `pets.location` writer. Once the
  device renders `location`, drift "just works" on the device: mochi
  wanders between places while idle and the toy follows at the next sync.
  The only tuning: drift cadence vs the device's e-ink refresh cost (full
  refresh ≈ 1 s; don't drift so often the panel is always redrawing).

### Diegetic interfaces

- The corner-icon UI is already being retired for authored MPK1 zones
  (design/14, `format=1`). This doc adds `nav_place` to the action
  vocabulary so a scene's *door* can mean "go to the kitchen" (a
  substrate place), not just "+1 within this pack". `event` / `talk_seed`
  / `nav_relative` / `nav_scene` are unchanged.
- `scene_plans` (the substrate device-native 200×172 scene system from
  design/06) and `places` (360×336 web) are the duplication to resolve:
  **fold them** — a place's device projection *is* its device-native
  scene; `scene_plans` becomes the per-place device-geometry + zone
  contract rather than a parallel table. (Bigger; flagged, not scoped here.)

## The world-model decision (needs a call)

What is the device's *default* world, and what happens to `scene-bundle-a`?

- **A — Device adopts the places graph.** Device always renders
  `pets.location`; `scene-bundle-a` demotes to offline/boot fallback (or
  becomes the "home" place's device scene). Cleanest authority story; but
  the 16 hand-authored interlinked bundle scenes collapse to single-scene
  places unless re-authored, and canonical seeds (home→`scene-v1`) must
  get device-geometry art.
- **B — Two layers.** `pets.location` picks *which* device scene; each
  place maps to a device-native scene (200×200 + zones). The bundle's 16
  scenes are re-homed as the device projections of places. Most work;
  cleanest end state.
- **C — Bundle is default; places overlay via travel (recommended for
  now).** Normal device experience stays `scene-bundle-a` (unchanged,
  non-breaking). When you *imagine* or *travel to* a dynamic place, the
  device swaps to that place at device geometry; returning ("home"/back)
  restores the bundle. `pets.location` tracks dynamic places; the
  canonical-seed↔bundle mapping is future work. Keeps the authored bundle
  richness, makes imagine actually render, smallest blast radius.

Recommendation: **C now, B as the end state.** C is non-destructive and
unblocks imagine on hardware immediately; B is where "one world model"
lands once canonical scenes are re-authored at device geometry.

## Implementation phases

1. **Geometry projection + imagine renders (this change).** Device-geometry
   pack variant; imagine fetches its pack at 200×200 and sets
   `pets.location` via `POST /api/places/:id/enter`; the existing
   `scene_pack_load_bytes` swap now blits. Fixes the v0.0.17 render gap.
   Path C: only dynamic places swap the device; the bundle stays default.
2. **Location-driven re-render.** `pet_sync` parses `location` + `places[]`;
   on change, fetch + swap (+ the main.cpp re-render-on-change wire, which
   also covers design/16's deferred re-render-on-DONE). Day/night via RTC.
3. **Costumes on device.** Render `current_costume_id` via the costume
   sheet's device cells.
4. **`nav_place` action + scene_plans/places fold (model B).** Authored
   device→substrate travel; unify the device-native scene contract.

## Cross-references

- `06-scene-contracts.md` — diegetic device scenes (the `scene_plans` side)
- `13`–`16` — the device-sprite pipeline this builds on
- `c15r/mochi:design/world-building.md` · `idle-drift.md` · `wardrobe.md`
- `firmware/main/scene_pack.{c,h}` · `pet_pack.c` · `pet_sync.cpp` · `pack_cache.cpp`
- `c15r/mochi:backend/db.ts` — `pets.location` / `current_costume_id` / `last_drift_at`; `places` / `costumes` / `scene_plans` tables
