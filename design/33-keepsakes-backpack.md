# 33 — Keepsakes & the backpack (memory-driven companion)

Status: **in progress** (design + wire-format foundation). See design/32
(world nav), design/17 (travel), design/19 (world-building), design/27
(voice observability).

## Vision

Mochi is a little traveller. As you explore the connected world (home ↔
forest ↔ village ↔ treetops — design/32) she picks up **keepsakes** — small
wonders from each place — into a **backpack**. Keepsakes aren't puzzle
inventory; they are *memory anchors*. Each one the pet remembers and brings up
later in conversation ("remember the copper bell from the treetops?"), and over
time they seed her dreams and even new imagined places. The "narrative you
uncover" is the **accreting relationship + the world's lore revealed one object
at a time** — no quest log, no fail states.

This is the chosen direction: **memory-driven companion + a light discovery
layer (a backpack of keepsakes)**, built **device-native** (collect on the
e-ink device, offline, with an on-device backpack screen).

## What rides existing substrate

- **`pet.knowledge`** (facts / dreams / diary) + **consolidate**: collected
  keepsakes live here, so the **voice pet already references them** with no new
  plumbing in the voice layer.
- **`imagine`** (origin `dream`, `from_place_id`): a keepsake can later make the
  pet *dream up a new place* — discovery → world growth (the expansion loop).
- **scene `talk_seed` zones**: the discovery surface generalises to a new
  COLLECT action that pockets the keepsake on first tap.

## Data model

A **keepsake definition** (global registry, studio/substrate-authored):

```
{ id, name, icon?, place, cell, lore }
```

- `id` — stable slug, e.g. `treetops-bell`. Carried in the pack zone.
- `name` — display ("a copper bell").
- `place` / `cell` — where it's found (for the backpack's "found in…" line).
- `lore` — one evocative line the pet says on pickup + can recall later.
- `icon?` — optional 1-bit icon cell (design/30 icon-sheet pipeline); the
  backpack falls back to the name when absent.

A **collected keepsake** (per pet) is stored in a dedicated `keepsakes` table
(one row per pet+keepsake — collection STATE only; the definitions stay in
`shared/keepsakes.ts`):

```
keepsakes(pet_id, id, found_at, found_place, found_cell)  PK (pet_id, id)
```

A dedicated table (vs. nesting in `pet.knowledge`) keeps collection isolated,
idempotent (composite PK), and trivially queryable for `/api/state` + the
backpack. First-time collects also write a memory **event** (`via:"keepsakes"`)
so there's a discovery trail immediately; richer voice-context surfacing +
consolidate/imagine weaving is the step-5 hook.

Signature set (one hero keepsake per place, grounded in painted objects):

| place    | id              | keepsake          | from cell                     |
|----------|-----------------|-------------------|-------------------------------|
| forest   | `forest-charm`  | a woven charm     | dreamcatcher (cell 10)        |
| village  | `village-cup`   | a little clay cup | pottery room (cell 10)        |
| treetops | `treetops-bell` | a copper bell     | humming platform bells (cell 7)|
| home     | `home-bloom`    | a pressed flower  | flowering garden (cell 14)    |

(Room to grow — several per place later; the treetops travel-sack, cell 15, is
a natural "journeys begin here" beat.)

## Wire format — `MPK_ACTION_COLLECT = 8`

A new action kind, carrying the keepsake `id` in the **label table** exactly
like `nav_place` (kind 5). The 24-byte zone layout is unchanged
(`x,y,w,h` u16 ×4, `kind`@8, `data`@9, `label_idx`@10 u16; ~12 spare bytes).

Layers that must agree (bump the encoder pin in lockstep — design/15):

1. **firmware `mochi_pack.h`**: `MPK_ACTION_COLLECT = 8`; `mpk_zone_get`
   resolves `seed_text` for kind 8 too (the keepsake id).
2. **encoder `mpk.ts`** (c15r/mochi-device): `MpkAction.Collect = 8`;
   `packMpkV1` adds the zone's keepsake id to the label-table set (same path as
   talk_seed / nav_place).
3. **`shared/scenes-spec.ts`**: `normaliseDeviceAction` accepts kind 8 with a
   `keepsake` id (stored where the projector can read it).
4. **server projection** (`backend/devsprite.ts projectPlanZone`): emit kind 8
   with the keepsake id into the label table.
5. **studio Zones panel**: author a COLLECT zone (keepsake picker).

`data`@9 is free for kind 8 — reserve it for an optional icon index later.

## Firmware UX

- **Tap a COLLECT zone** (`scene_pack_action_at` already returns kind +
  seed_text for format=1):
  - **first time** (id not in the NVS collected-set): add it, show a brief
    toast ("✦ kept: a copper bell"), and **sync** to the server. The pet's
    expression can do a small delight beat.
  - **already collected**: a `talk_seed`-style remark ("the bell's spot") — no
    duplicate.
- **Backpack screen**: opened from the dev-menu (and later a dedicated
  gesture). A grid of collected keepsakes — icon (or name), with "found in
  <place>". Renders via the existing compositor; no network needed (reads the
  NVS set).
- **Offline persistence**: a small NVS blob (`keepsakes` namespace) holding the
  collected id set + found metadata. Idempotent add.
- **Sync**: `POST /api/keepsake/collect { hwId|petId, id, foundPlace, foundCell }`
  → server records into `pet.knowledge.keepsakes` (idempotent), logs an event,
  returns the merged set. `/api/state` carries the collected ids so a fresh
  device / the web backpack converge. Server is authoritative on merge; the
  device's NVS set is the offline cache.

## Memory + world-growth tie-in

- On collect, the server writes a **diary/fact** entry ("found a copper bell in
  the treetops") so the **voice pet** can recall it unprompted.
- **consolidate** may weave keepsakes into dreams.
- A keepsake can seed **imagine** (origin `dream`, `from_place_id = <place>`):
  "the shell makes her dream of a tide-pool cove" → a new place grows, linked
  back. This is the discovery → expansion loop.

## Web/studio backpack view

`GET /devsprite/backpack?petId=<id>` — read-only, server-rendered (like the
graph views): the pet's collected keepsakes + the full registry (greyed =
not-yet-found), so authoring and play state are both visible.

## World + content prep (2026-06-06)

- **Spaceship promoted into the world.** A studio-authored spaceship habitat
  (scratch-authored as `test-bundle-a`) was duplicated to a clean id
  **`scene-spaceship-a`** (all stores: user template, source/derived PNGs,
  etags, keying, zones, scene_plan), registered as world place **`spaceship`**,
  backfilled into every pet, and `test-bundle-a` deleted. Its nav was rewired
  to a reciprocal spine (30 edges, 0 one-way) with a single `cell_15 → home`
  return (the shuttle nose bay). The matching `home → spaceship` portal (home
  cell 11, the spaceship **observatory**) lands in step 4's bake. Notably the
  new planner prompts (design/32) already had it emitting threshold portals
  instead of a uniform mesh.
- **Five keepsakes** now (added `spaceship-star` "a glass star", found in the
  spaceship observation nook). One signature keepsake per world place.
- **Icon sheet `keepsake-icons-a`** created (design/30 pipeline, 80×80 `ui`
  cells), one cell per keepsake keyed by id (hyphens → underscores via the
  template normaliser, so `forest-charm` → cell `forest_charm`, etc.). Open it
  in the studio **Icons** panel, set titles, and generate with a BYO key;
  render-time maps `keepsake.icon` → the underscored cell key. Backpack falls
  back to the name until generated.

## Build order

1. **Wire-format foundation** — **done**: firmware `mochi_pack.h` kind 8 +
   seed resolve; shared `normaliseDeviceAction`; encoder `mpk.ts`; server
   projection. A COLLECT zone can be authored, packed, decoded; firmware
   treats it as inert until step 3.
2. **Substrate** — **done**: `shared/keepsakes.ts` registry; `keepsakes` table
   (db.ts schema v2); `backend/keepsakes.ts` (recordKeepsake/listKeepsakes);
   `POST /api/keepsake/collect` (idempotent, `firstTime`, unknown-id 400);
   `keepsakes` ids on `/api/state`; `GET /devsprite/backpack` view; pet-delete
   cleanup. Verified end-to-end against the live val.
3. **Firmware behaviour**: tap-to-collect (NVS set, toast, sync), backpack
   screen, expression beat. Version bump.
4. **Content**: COLLECT zones on the 5 live bundles (the signature set) +
   `home → spaceship` observatory portal; re-bake `scenes_a.mpk`; bump encoder
   pin; rebuild.
5. **Memory/growth**: diary/fact on collect; consolidate + imagine hooks.
