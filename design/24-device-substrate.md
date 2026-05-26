# 24 — Device substrate (clean break)

**Status:** plan, 2026-05-26. Plan-of-record for moving the device's API +
canonical data into `c15r/mochi-device`, dissolving the
`places`/`scene_plans`/`zones` triple, and giving the firmware ONE base URL
for everything device-facing. Continues design/13–17 + design/19; supersedes
the cross-val pin (design/15) for device-rendering code.

**Predecessors:**
[05-sprite-format.md](./05-sprite-format.md) ·
[06-scene-contracts.md](./06-scene-contracts.md) ·
[13-build-time-asset-packs.md](./13-build-time-asset-packs.md) ·
[14-mpk1-edges-and-actions.md](./14-mpk1-edges-and-actions.md) ·
[15-device-sprite-consolidation.md](./15-device-sprite-consolidation.md) ·
[17-location-embodiment.md](./17-location-embodiment.md) ·
[19-studio-world-consolidation.md](./19-studio-world-consolidation.md) ·
[20-splash-and-chrome-zones.md](./20-splash-and-chrome-zones.md) ·
`c15r/mochi:design/diegetic-interfaces.md` · `vision.md` ·
`mochi-systems-design.md`

## Why — three views of one tree

Today the same diegetic concept lives in three tables and is glued back
together at request time:

| System (today) | Owner val | Scope | Carries | Geometry | Device sees |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `scene_plans` | `c15r/mochi` | per-sheet | richest: per-cell zones + `intent` + gestures + `petAnchor` + `grammar` + seed + exemplar | sheet's authoring geometry | projected at pack time |
| `places` | `c15r/mochi` | per-pet | name + vibe + day/night + lifecycle (`pending`→`ready`); **no zones** | reference to `sheet_id` | flat `move_to_location` |
| `user_sheets.zones` | `c15r/mochi` | per-sheet | thin: `{x,y,w,h,kind,data,seed}` only | cell-local px | yes (legacy path) |

The pack route already has to read one and fall back to the other
(design/19 P2). The studio writes both. Authoring tools project from one;
the device reads from the other. **A place IS a sheet IS a plan** — that
identity should be a row, not three rows joined by id at every request.

Layered on top: device must hit `mochi.val.run` for sprites, plans,
places, identity, costumes, world edges; the encoder is **re-exported
cross-val** (`backend/devsprite-encode.ts` → `esm.town/v/c15r/mochi-device@N-main`)
with a pin that has to be bumped in lockstep — confirmed brittle in
practice (design/15 + recent v4 / `enabled` filter rollouts).

**Verdict:** clean break. New substrate in `c15r/mochi-device`, new
device API rooted there, new firmware build that points at it. Migrate
only the four sheets we actually want carried over; re-author the rest or
retire them.

## What's in scope

Three asks, one move:

1. **Unify** `places` + `scene_plans` + `zones` into one canonical row per
   place, one immutable history per artwork upload, one immutable history
   per plan edit.
2. **Move** the device API surface to `c15r/mochi-device` so the firmware
   has a single base URL.
3. **Re-author** the four sheets we want to keep (`pet-v1`, `ui-v1` /
   `splash-v1`, `scene-bundle-a`, `the-forest-a`) directly in the new
   substrate. Everything else is dropped or re-imagined later.

## Substrate — tables

SQLite via std/sqlite (mochi-device val). Names are deliberately not
prefixed with `device_` — this val IS the device's substrate now.

```sql
-- A graph of places belonging to a pet (or a canonical/global world).
CREATE TABLE world (
  id            TEXT PRIMARY KEY,            -- 'world_<...>' or 'canonical'
  scope         TEXT NOT NULL CHECK (scope IN ('canonical', 'pet')),
  pet_id        TEXT,                        -- null when scope='canonical'
  boot_place_id TEXT NOT NULL,               -- where a fresh device wakes
  created_at    INTEGER NOT NULL,
  updated_at    INTEGER NOT NULL
);

-- A named location = ONE authored sheet. Subsumes today's `places` row
-- AND its `scene_plans` row AND its zone store. Carries pointers to the
-- current immutable sprite + plan; flip those to roll back / forward.
CREATE TABLE place (
  id                        TEXT PRIMARY KEY,  -- 'place_<...>'
  world_id                  TEXT NOT NULL REFERENCES world(id),
  kind                      TEXT NOT NULL CHECK (kind IN ('scene','panel','pet','ui','exemplar')),
  name                      TEXT NOT NULL,
  vibe                      TEXT,
  grammar                   TEXT,              -- 'care-loop' | 'voice-primary' | ...
  cell_w                    INTEGER NOT NULL,  -- authoring geometry
  cell_h                    INTEGER NOT NULL,
  cols                      INTEGER NOT NULL,
  rows                      INTEGER NOT NULL,
  pet_anchor_x              INTEGER,           -- firmware-fixed default if NULL (design/23)
  pet_anchor_foot_y         INTEGER,
  lifecycle                 TEXT NOT NULL CHECK (lifecycle IN ('pending','ready','revising','retired')),
  current_sprite_version_id INTEGER REFERENCES sprite_version(id),
  current_plan_version_id   INTEGER REFERENCES plan_version(id),
  created_at                INTEGER NOT NULL,
  updated_at                INTEGER NOT NULL
);
CREATE INDEX place_world ON place(world_id);

-- Per-cell content. Zones live HERE inline (one store, not two). Each
-- row mirrors a cell of the place's grid. `zones_json` is the diegetic
-- contract: rect + intent + gestures + optional deviceAction. The device
-- pack carries only the actionable subset.
CREATE TABLE cell (
  place_id        TEXT NOT NULL REFERENCES place(id),
  cell_key        TEXT NOT NULL,             -- 'sprite_03', 'cell_07', 'boot', etc.
  col             INTEGER NOT NULL,
  row             INTEGER NOT NULL,
  description     TEXT,                      -- per-cell directive (steers gen)
  zones_json      TEXT NOT NULL DEFAULT '[]',
  PRIMARY KEY (place_id, cell_key)
);

-- Directed nav graph: from a cell of a place via a gesture, you arrive
-- somewhere. Replaces the loose `nav_place` zone-data convention with a
-- queryable graph. Built from zones at save time; the device gets it
-- baked into the pack so taps resolve locally.
CREATE TABLE world_edge (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  from_place_id TEXT NOT NULL REFERENCES place(id),
  from_cell_key TEXT NOT NULL,
  zone_idx      INTEGER NOT NULL,            -- which zone in cell.zones_json
  to_place_id   TEXT NOT NULL REFERENCES place(id),
  to_entry_cell TEXT,                        -- defaults to place's first cell
  bidirectional INTEGER NOT NULL DEFAULT 0   -- 1 = also create the reciprocal edge
);
CREATE INDEX edge_from ON world_edge(from_place_id);

-- Immutable history of source artwork. Every upload / generation appends
-- a row. place.current_sprite_version_id points at the live one.
-- Content-addressed: blob lives in std/blob keyed by content_etag.
CREATE TABLE sprite_version (
  id                INTEGER PRIMARY KEY AUTOINCREMENT,
  place_id          TEXT NOT NULL REFERENCES place(id),
  content_etag      TEXT NOT NULL,            -- sha256 of the PNG bytes
  mime              TEXT NOT NULL DEFAULT 'image/png',
  bytes             INTEGER NOT NULL,
  width             INTEGER NOT NULL,
  height            INTEGER NOT NULL,
  parent_version_id INTEGER REFERENCES sprite_version(id),  -- for revising
  origin            TEXT NOT NULL CHECK (origin IN ('upload','imagine','reauthor','baseline')),
  author            TEXT,                     -- pet_id / 'studio' / 'voice-imagine'
  created_at        INTEGER NOT NULL
);

-- Immutable history of the diegetic plan (zones + grammar + anchor + ...).
-- One row per save. Same pattern as sprite_version. plan_json is the full
-- canonical snapshot — re-derive everything from it.
CREATE TABLE plan_version (
  id                INTEGER PRIMARY KEY AUTOINCREMENT,
  place_id          TEXT NOT NULL REFERENCES place(id),
  plan_json         TEXT NOT NULL,            -- {cells:{key→{zones,desc}}, edges, anchor, ...}
  prompt            TEXT,                     -- generation recipe text (if any)
  exemplar_place_id TEXT REFERENCES place(id),
  parent_version_id INTEGER REFERENCES plan_version(id),
  author            TEXT,
  created_at        INTEGER NOT NULL
);

-- Pet identity. Unchanged in shape from today; just moved.
CREATE TABLE pet (
  id                  TEXT PRIMARY KEY,
  name                TEXT NOT NULL,
  species             TEXT NOT NULL,
  born_at             INTEGER NOT NULL,
  current_world_id    TEXT REFERENCES world(id),
  current_place_id    TEXT REFERENCES place(id),
  current_costume_id  TEXT,                   -- references a place where kind='pet'
  bond                INTEGER NOT NULL DEFAULT 0,
  created_at          INTEGER NOT NULL,
  updated_at          INTEGER NOT NULL
);

-- Append-only history. Truth carrier. Decay, bond, nav, generation.
CREATE TABLE event (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  pet_id    TEXT NOT NULL REFERENCES pet(id),
  kind      TEXT NOT NULL,                    -- 'fed' | 'nav' | 'imagined' | 'paired' | ...
  data_json TEXT NOT NULL DEFAULT '{}',
  at        INTEGER NOT NULL
);
CREATE INDEX event_pet ON event(pet_id, at);
```

### Invariants

- **Identity:** a place is one row. Its sprite + plan history are
  immutable; `place.current_*_version_id` is the only mutable pointer.
- **Zones live in one place:** `cell.zones_json`. The pack route filters
  to actionable zones at projection time; intent-only zones (kind 0) stay
  in the substrate but never reach the device.
- **Edges are queryable:** `world_edge` is derived from zones at save
  time. The device pack still embeds them inline (no extra fetch); the
  table exists so the studio can render a graph + auto-wire reciprocal
  edges (design/19 P4 deferred item).
- **Content addressing:** sprite blobs are keyed by sha256 of the PNG
  bytes (`content_etag`). Two places with identical artwork share storage.
- **No `current_*` writes without a new row:** every artwork or plan edit
  appends; the pointer flips after. Rollback = flip the pointer back. No
  destructive updates.

## Device API (`c15r/mochi-device.val.run/api/device/*`)

The device pulls a *projection* — never raw rows. Geometry-canonical to
the device (200×200, 1bpp, MPK1 v4). All responses set
`ETag: "<sprite_version_id>-<plan_version_id>-pp4"`; firmware HEAD-probes
on boot resync.

```
GET  /api/device/world                       → World (places list + edges, pet-scoped)
GET  /api/device/place/:id/pack              → MPK1 bytes (cells + inline zones)
GET  /api/device/place/:id/zones             → flat actionable zones (no plan)
GET  /api/device/state                       → PetState snapshot
POST /api/device/event                       → append to event log
POST /api/device/imagine                     → seed a new place (returns place_id, pending)
GET  /api/device/place/:id/status            → poll lifecycle (pending → ready)
POST /api/device/pair/start                  → unchanged from today
POST /api/device/pair/confirm                → unchanged from today
GET  /api/device/diag                        → diagnostics (device-diag.ts equivalent)
GET  /api/device/ota/manifest                → OTA pointer (later)
```

`Authorization` / `X-Pet-Id` header carries the device identity, same as
today's mochi backend.

## Studio API (`/api/studio/*`)

Reads/writes the raw rows; the studio is the *authoring* surface, so it
sees more than the device.

```
GET  /api/studio/worlds
POST /api/studio/world
GET  /api/studio/place/:id                   → full row + current sprite + current plan
POST /api/studio/place                       → create
POST /api/studio/place/:id/sprite            → upload PNG → new sprite_version
POST /api/studio/place/:id/plan              → save plan → new plan_version
POST /api/studio/place/:id/promote           → flip current_*_version_id
POST /api/studio/place/:id/rollback          → flip to previous version
GET  /api/studio/place/:id/history           → versions list (sprite + plan)
```

The existing **dither workbench live-preview cfg** (design/23) keeps
working: it's a query param on the pack/preview route, not a substrate
row. No table change needed there.

## What about the rest of mochi's API surface?

The user's open question: clean-break everything, or just the
device-rendering layer? After inventorying `c15r/mochi/backend/*`:

| File / route family | Move to mochi-device? | Why |
| :--- | :--- | :--- |
| `devsprite.ts`, `devsprite-encode.ts`, `devsprite-dashboard.ts` | ✅ replace | Replaced by new device API + studio API; encoder is local in mochi-device anyway. |
| `sheets.ts`, `user-sheets.ts` | ✅ replace | Folded into `place` + `sprite_version`. |
| `scene-plans.ts` | ✅ replace | Folded into `place` + `plan_version`. |
| `places.ts`, `places-device.ts` | ✅ replace | Folded into `place` + `world_edge` + `world`. |
| `costumes.ts` | ✅ replace | Costumes become `place` rows where `kind = 'pet'` scoped to a pet. Same lifecycle, no new shape. |
| `identity.ts` | ✅ move | Pet identity is device-canonical; carrying it on mochi just means another base URL for the firmware. |
| `device-pair.ts`, `device-diag.ts` | ✅ move | Device-related; same base URL principle. |
| `memory.ts` | ✅ move | Append-only event log = `event` table. |
| `keying-pixel.ts` | ✅ move | Part of the sprite upload pipeline (server-side keying). |
| `substrate.ts`, `consolidate.ts`, `api.ts` | depends | Inspect contents during migration; most likely move or fold. |
| `voice-tools.ts`, `voice-session.ts`, `voice-instructions.ts`, `voice-tools-spec-route.ts` | ❌ keep on mochi | OpenAI realtime broker + tool specs. Different lifecycle (long-lived sessions), different deps, different scaling. Reads from mochi-device's API for state, doesn't own data. |
| `threshold-preview.ts`, `sprites.ts` (debug) | ❌ retire | Dev-only debug routes. Replace with the studio's dither workbench (already shipped). |
| `frontend/`, top-level `main.ts`, `index_html.ts` | ❌ keep on mochi | The legacy web app + landing. Keep for now; rewrite later if needed. |

**Net:** device + storage + authoring move; **voice/realtime stays on
mochi** and consumes the device API as a client. Firmware has one base
URL (`mochi-device.val.run`); the voice realtime broker on
`mochi.val.run` keeps its own URL because the device only talks to it
during a voice session (different lifecycle, different transport).

Two URLs the firmware knows: `MOCHI_DEVICE` (everything) and `MOCHI_VOICE`
(realtime sessions only). That's the smallest split that respects what's
genuinely different.

## Migration — minimum viable port

Re-author, don't backfill. Four sheets carry over:

| Sheet (today) | New place id | kind | grid | Provenance |
| :--- | :--- | :--- | :--- | :--- |
| `pet-v1` | `place_pet_v1` | pet | 96×96 / N expressions | re-author from existing PNG |
| `splash-v1` (boot) | `place_splash_v1` | panel | 200×200 / 1 cell | re-author; kind 6/7 zones (design/20) |
| `scene-bundle-a` (home) | `place_home` | scene | 200×200 / 16 cells | re-author plan; reuse existing PNG as baseline `sprite_version` |
| `the-forest-a` (forest) | `place_forest` | scene | 200×200 / 16 cells | same — keep art, re-author plan in the new editor |

UI icons are optional — if they all live in `splash-v1`, no separate
sheet. If there are stand-alone icon sheets (battery, mic, etc.) they
become `kind='ui'` places. Decide once we've inventoried which are
actually drawn by the firmware vs. authored-but-unused.

Everything else (`scene-kitchen-eink-v1/v2`, `scene-garden-v1`,
`scene-window-v1`, `the-forest-b/c/d`, `scene-v1`, `scene-bundle-b/c/d`)
is **left in mochi until retired**. The firmware's new build doesn't
fetch them.

## Cutover — checklist

**Phase A · scaffolding (mochi-device, no firmware change yet)**
- [ ] Create tables (DDL above) behind a feature flag URL prefix
      (`/api/v2/...`) so production isn't touched.
- [ ] Implement the device API + studio API routes against the new
      tables.
- [ ] Build a small *re-author* surface in the studio (or reuse the
      existing zones canvas, pointed at the new endpoints).
- [ ] Re-author the four sheets natively in the new substrate.
- [ ] Verify by hand: pack + tap + nav for each.

**Phase B · firmware build**
- [ ] New firmware constant: `MOCHI_DEVICE_BASE` = mochi-device val URL.
- [ ] Update every fetch in `firmware/main/*.cpp` / `*.h` that currently
      uses `mochi.val.run/devsprite/...` to use `MOCHI_DEVICE_BASE`.
- [ ] Keep `MOCHI_VOICE_BASE` pointing at mochi (unchanged).
- [ ] Compile-time embed of the four new packs (re-extract via the new
      studio).
- [ ] Conformance test: device renders home + forest, taps work, splash
      shows.

**Phase C · OTA cutover (atomic with Phase B)**
- [ ] Ship firmware OTA. Devices flip to new base URL in one go.
- [ ] Watch device-diag for fetch errors during the first hour.
- [ ] Keep mochi's old devsprite routes alive for 30 days as fallback
      (firmware doesn't hit them, but useful if rollback needed).

**Phase D · cleanup**
- [ ] Retire `c15r/mochi/backend/devsprite*`, `sheets.ts`,
      `user-sheets.ts`, `scene-plans.ts`, `places.ts`, `places-device.ts`,
      `costumes.ts`, `identity.ts`, `device-pair.ts`, `device-diag.ts`,
      `memory.ts`, `keying-pixel.ts`.
- [ ] Delete `c15r/mochi/backend/devsprite-encode.ts` (cross-val pin).
- [ ] Drop the unused tables from mochi's SQLite.
- [ ] Update `c15r/mochi:README.md` to reflect the narrower scope (voice
      + legacy web only).

## Live-val safety

The current devices boot-sync packs from `mochi.val.run`. Until Phase C
ships, **nothing about the existing URLs or wire format can change.**
Rules:

- New work happens behind `/api/v2/...` in mochi-device. Production
  device URLs are untouched.
- Phase C is the *only* atomic moment. Firmware OTA + base-URL flip ship
  together. No partial state where some devices hit v1 and some hit v2.
- `POSTPROCESS_VERSION` does **not** bump during this transition. Both
  vals serve v4. If we want to bump (e.g. for auto-tone presets from
  design/23), do it AFTER cutover, as a separate change.
- The encoder pin in mochi (currently `@319-main`) stays put until Phase
  D removes the file entirely.

## Risks

- **Re-authoring is real work, not overhead.** Each kept sheet's zones
  get rebuilt in the new model. Budget a day per scene sheet (home,
  forest); panels (splash, pet) are quick.
- **Pet identity ownership.** Moving `pet` to mochi-device means voice
  realtime sessions on mochi need to fetch identity from there. Add a
  `GET /api/device/state` call to voice-session bootstrap; cache it
  per-session. Trivial change but easy to forget.
- **Event log split.** If we move `event` and `memory` to mochi-device
  but voice events are written from mochi, we need a cross-val POST. Two
  options: (a) voice writes via `POST /api/device/event` (uniform), or
  (b) keep a slim event log on mochi for voice events and sync. (a) is
  cleaner; pick it.
- **Sprite re-extraction non-determinism.** If the keying pipeline isn't
  byte-exact between the old and new vals, freshly-re-extracted cells
  won't match the old packs byte-for-byte. Run the existing conformance
  harness (`c15r/mochi-device/conformance.ts`) against the new pipeline
  to confirm or pin the differences.
- **No backfill = no rollback.** If Phase C fails, we re-publish the old
  firmware. We don't re-import the dropped sheets. Make peace with that
  upfront.

## Open questions / deferred

- **Costume slots** (design/17 P3). The proposed `place.kind = 'pet'` row
  is the costume. `pet.current_costume_id` flips it. Zone actions for
  `wear_costume` / `take_off_costume` (kinds 6/7, design/20) stay
  deferred — the schema supports them, the firmware code path lands when
  authored.
- **World scope: per-pet vs. canonical.** A canonical home (the same for
  every pet) vs. a per-pet world. The schema allows both
  (`world.scope`). MVP: canonical world for pet-v1 + splash + home +
  forest; per-pet worlds only when voice-imagine creates them.
- **Multi-species pets.** `pet.species` is free-form; the pet sheet's
  expressions are per-species. The `place_pet_<species>` naming convention
  is enough for now; a proper `species` table is later work.
- **Dither workbench live cfg + the substrate.** The cfg-override path
  (design/23) reads from the request, not the substrate — unchanged by
  this move. The per-kind presets (`POSTPROCESS[kind]`) still live in
  mochi-device source code.
- **Migration scripts.** Phase A includes "re-author by hand." If we
  later decide to backfill more, write a one-shot importer that reads
  mochi rows + emits substrate rows. Not on the critical path.
