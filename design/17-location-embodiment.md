# 17 — Location, embodiment & the world model (reconciliation)

**Status:** sketch, 2026-05-22 (rev 2 — converged model). Reconciles four
tracks that grew independently and now overlap on the device:
**world-building** (places + `move_to_location` + imagine), **costumes**
(wardrobe + `current_costume_id`), **idle drift** (autonomous movement),
and **diegetic interfaces** (zones as UI). Triggered by on-device imagine
(design/16) landing — the first time the device reached into the substrate
world model — which surfaced a geometry seam and an authority question
neither side had answered.

**Predecessors:** world-building (`c15r/mochi:design/world-building.md`),
idle-drift (`c15r/mochi:design/idle-drift.md`), wardrobe
(`c15r/mochi:design/wardrobe.md`), [06-scene-contracts.md](./06-scene-contracts.md),
[13](./13-build-time-asset-packs.md)–[16](./16-on-device-imagine.md).

## What's broken — the seams

**Three scene/location systems that don't reconcile:**

| System | Geometry | Where | Navigation | Substrate? |
| :--- | :--- | :--- | :--- | :--- |
| `scene-bundle-a` | 200×200, 16 cells | firmware (embedded MPK1) | authored MPK1 zones (`nav_relative`, `nav_scene`) | no — device-local |
| `places` | 360×336 day/night | web (`c15r/mochi`) | `move_to_location`, idle drift, imagine | yes — `places`, `pets.location` |
| `scene_plans` | 200×172 device-native | substrate | zones (design/06) | yes — but no device consumer |

The substrate authority for "where" is `pets.location` (`db.ts:76`); the
device authority is a bundle index. They never exchange a byte: voice
`move_to_location` / idle drift change the web, the device shows nothing;
device door-taps walk the bundle, the substrate never learns.

**Geometry mismatch.** The device renders 200×200 cells
(`scene_pack_blit_current` is a verbatim row-copy that *rejects* width ≠
200). Places are 360×336. So on-device imagine (v0.0.17) swapped the
device to a 360-wide pack it cannot blit — fixed by a device-geometry
projection (below).

**Costumes never reach the device.** `pets.current_costume_id` + the
`costumes` table + `wear_costume`/`take_off_costume` work web-side; the
device renders the pet from embedded `pet_a` and ignores the field.

**`/api/state` already carries the world** — `location`, `places[]` (each
with `sheetId`), `currentCostumeId`, `lastDriftAt` — and `pet_sync`'s
parser discards all four.

## The converged model

**A place is a multi-cell pack. `pets.location` is the only authority for
"where". The device renders it; travel is the only thing that swaps.**

1. **A place = an N-cell MPK1 pack** (1..N, not pinned to 2), with
   `format=1` zones for intra-place navigation *and* variant resolution
   (below). `scene-bundle-a` is just the `home` place that happens to
   have 16 cells; dynamic imagined places are peers. This is why the old
   "bundle vs places" layering (the earlier draft's model B) **dissolves**
   — `scene-bundle-a` + `format=1` already prove the mechanism, so a place
   and the bundle are the same kind of thing.
2. **`home` = `scene-bundle-a`, the default on pet birth.** The canonical
   seeds are the one exception to "one source, two renderers": `home` has
   a device-native art (the 16-cell bundle) distinct from its web art
   (`scene-v1`). Seeds predate the one-source model; *dynamic* places stay
   one-source-projected.
3. **One source artwork per dynamic place, projected per consumer.** The
   place PNG (web geometry) is projected to the web at 360×336 and to the
   device at 200×200 by the same encoder (the `/devsprite/pack` `cw`/`ch`
   override — landed). No second art set.
4. **`pets.current_costume_id` is the only authority for "who"** — both
   renderers swap the pet sheet to the costume sheet's cells when set.

### Movement: imagine notifies, travel renders

- **Imagine does not travel.** Web `generatePlace` ends at
  `markPlaceReady` and never moves the pet; it surfaces an *in-progress
  talk / thought* hinting a new place is available to travel to. The
  device follows this: imagine reaches DONE and notifies (a thought
  carrying a travel action, ideally set by `markPlaceReady` so web +
  device share the existing thought-bubble path — model-proposes,
  substrate-disposes). It does **not** swap the scene.
- **Travel is the only scene-swap**, and it follows `pets.location`.
  `pet_sync` sees `location` change (voice `move_to_location`, idle drift,
  or accepting an imagine hint) → fetches the place's pack at device
  geometry → `scene_pack_load_bytes`. This makes all three movers visible
  on the device and durable across reboot (location lives in substrate,
  not the in-RAM swap).

### Variant resolution (replaces the fixed 2-cell day/night)

A place's cells can include *variants* of the same view — day vs night,
lit vs dark, future non-time conditions. Two complementary links, both
expressed in the existing `format=1` zone trailer (no envelope change —
use the 12 reserved bytes, design/14):

- **Meta links (non-tappable, generally).** A renderer-resolved link:
  "cell *i*'s night variant is cell *j*." The device picks *i* vs *j* by
  the PCF85063 RTC today; the same mechanism extends to non-binary and
  non-time conditions later (weather, mood, season) without a format
  change — the resolver just grows. This generality is the argument for a
  meta link over hard-coding day/night.
- **Tappable variants.** The same "link to a variant cell" surfaced as an
  affordance — e.g. a *lightbulb* zone that does a `nav_scene` to the
  dark/lit variant index (interior light toggle), not strictly a `door`
  but the same effect. Just an authored `nav_scene`/`nav_relative` to the
  variant; no new kind needed.

**No location index suffix.** Which *cell* within a place is showing is
render-resolved (RTC + meta link) or device-local (nav taps, reset on
travel) — and the web keys on the *place*, not a sub-cell, so a suffix
would be device-only state with nothing to sync to. `location = place_id`.
Add a suffix only later if sub-position must persist/surface on the web
("the pet's out on the balcony").

## e-ink refresh on navigate

A scene swap is a whole-screen change, so partial refresh leaves residue
from the previous scene (why navigate forced full before). Full is clean
but ~1 s and flashes. Hybrid policy (`SCENE_NAV_FULL_EVERY`): partial on
each navigate (fast), a clean full every Nth to clear accumulated
ghosting. The vendor SSD1681 driver is frozen and exposes only full +
partial (no fast LUT), so this is the available lever; N is a hardware
visual tuning knob.

## Per-track status

- **Geometry projection** — *landed.* `/devsprite/pack/<sheet>?cw=200&ch=200`
  crops per the authoring grid, encodes each cell at device size
  (`deriveOrLoadCell` `outW`/`outH`). Verified: scene-v1 200×200/20064 B.
- **`POST /api/places/:id/enter`** — *landed.* Sets `pets.location` (the
  travel write the device/voice/web all funnel through).
- **Imagine notify-not-travel** — *to do.* Walk back the device auto-swap
  added in v0.0.17 so imagine ends at ready+notify; land it *with* the
  travel-render wire so the device doesn't regress to silent.
- **Location-driven re-render** — *to do.* `pet_sync` parses `location` +
  `places[]`; on change, fetch at device geometry + swap (+ the
  re-render-on-change wire, which also covers design/16's deferred
  re-render-on-DONE). Day/night via RTC + meta link.
- **Costumes on device** — *to do.* Render `current_costume_id` via the
  costume sheet's device cells.
- **Idle drift** — no new mechanism; it's already a `pets.location`
  writer, so it "just works" once the device renders location. Only
  tuning: drift cadence vs e-ink full-refresh cost.
- **Variant meta link** — *to do.* The non-tappable day/night (→general)
  resolver in the `format=1` trailer + the device-side RTC resolution.

## Implementation phases

1. **Geometry projection + `/enter`** — *done* (server). Unblocks
   rendering a place at device geometry.
2. **Travel render + imagine notify.** `pet_sync` location→swap; walk back
   imagine auto-travel to notify; re-render-on-change. Day/night via RTC +
   meta link. (Lands the imagine loop visibly + correctly together.)
3. **Costumes on device** — render `current_costume_id`.
4. **`nav_place` + variant authoring polish** — authored device→substrate
   travel (door-to-a-place); studio support for meta links + tappable
   variants.

## Cross-references

- `06-scene-contracts.md` · `13`–`16` — the device-sprite pipeline this builds on
- `c15r/mochi:design/world-building.md` · `idle-drift.md` · `wardrobe.md`
- `firmware/main/scene_pack.{c,h}` · `pet_pack.c` · `pet_sync.cpp` · `pack_cache.cpp` · `imagine.c`
- `c15r/mochi:backend/db.ts` — `pets.location` / `current_costume_id` / `last_drift_at`; `places` / `costumes` / `scene_plans`
- `c15r/mochi:backend/devsprite.ts` (`cw`/`ch` projection) · `places-device.ts` (`/enter`)
