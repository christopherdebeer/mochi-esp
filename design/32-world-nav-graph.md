# 32 — World navigation graph (studio reachability view)

## Problem

Navigation between cells and places is authored implicitly: each scene's
`plan_json` carries per-cell zones, and a `nav_place` zone points at a target.
There was **no way to review reachability** — which places reach which, what's
orphaned, what's a dead end — so debugging "why can't I get back from here?"
meant reading JSON by hand.

## Data model (read-only, derived)

- **`scene_plans`** (one row per `sheet_id`): `plan_json` has `zones[]`
  (shared) and `cellZones{ cellKey: zone[] }`.
- A zone's action is `deviceAction.kind`:
  - `5` = **`nav_place`**, target id in `deviceAction.place` — the cross-place
    travel edges.
  - `2` = **nav_relative** (door/back within a sheet; `data` = cell offset) —
    intra-sheet, counted but not a cross edge.
- **Target resolution** for a `nav_place` target string:
  1. a known `scene_plans.sheet_id`,
  2. a canonical location id (`home`→`scene-v1`, …; `shared/locations.ts`),
  3. (with `?petId`) a `places` row id → its `sheetId`,
  4. else **unresolved** (a real bug — flagged).

## The view — `GET /devsprite/graph`

Server-rendered HTML (no client deps). Query: `?root=<sheet>&petId=<id>`.

- **Nodes** = sheets (scene plans + referenced canonical locations + the pet's
  places). Each card shows a cell thumbnail (`/devsprite/cell/<sheet>/<cell>/
  preview`), status, grammar, `↗` nav_place exit count, `↔` relative-nav count.
- **Edges** = `nav_place` zones, drawn as directed arrows; forward / back-loop /
  broken are colour-coded. Hover shows `from[cell] · zone → target`.
- **Layout** = layered by BFS depth from `root` (default `scene-bundle-a`);
  unreachable nodes sit in a trailing band and are flagged.
- **Issues panels**: broken nav targets, unreachable-from-root sheets, and
  reachable-but-not-ready sheets (pending/failed → won't load on device).
- A full nav-edge table follows.

## What it immediately surfaced

Against live substrate: 9 nodes, 51 `nav_place` edges, **0 broken** but **8
unreachable from `scene-bundle-a`** — because the home bundle has **0
`nav_place` exits** (it navigates internally via 48 relative-nav zones), while
`scene-kitchen-eink-v1` is the hub (48 exits). Everything points *into* home;
nothing routes *out* of it via `nav_place`. Exactly the reachability picture
that was previously invisible.

## Network redesign — from grid mesh to spine + portals (2026-06-06)

The per-cell graph view (`/graph/cells`, `/graph/cy`) made the real shape of
`scene-bundle-a` ("home") legible, and it was bad: a **mechanical 4×4 grid
mesh**. Every cell emitted a `nav_scene` to each grid neighbour regardless of
theme — 48 intra edges — with exactly **one** `nav_place` (`sprite_06 → forest`).
Adjacency was an artifact of how 16 cells were packed into a sheet, not the
world: you walked "east" out of the **beach** (07) into the **snowy room** (08);
the **spaceship observatory** (11) opened onto the **library** (10). Coherent
exits were accidental.

Root cause was the planner prompt, not the data. `DEFAULT_PLANNER_SYSTEM`
(`shared/scenes-spec.ts`) carried the rule *"EMIT A NAV-EXIT ZONE FOR EVERY
ADJACENT DIRECTION,"* so the LLM wired by grid geometry. `nav_place` was
"use sparingly" and silently dropped when its target wasn't a registered place.

### Prompt changes (`shared/scenes-spec.ts`)

- **Mesh → spine.** Replaced the "exit for every direction" rule with
  *CONNECTED, NOT MESHED*: grid adjacency is the only *allowed* set of
  directions, but connect two rooms **only where their scenes diegetically
  continue** (shared path, doorway, same terrain/interior). Aim for a
  mostly-linear spine, 1–2 exits/room, **every exit reciprocated** (a way back).
- **Thresholds are portals.** New goal: a room depicting an *edge of the world*
  (gate, doorway, window, shore, wilderness path) should carry a `nav_place` to
  the world it opens onto, matched to what's painted — replacing "use sparingly."
- **Zone budget 5–7 → 4–6** (target 5), freeing outer sub-cells for
  care/play/talk affordances now that nav no longer fills them.
- User prompt gained an explicit **reachability check** ("every room reachable
  AND has a way back").

### `scene-bundle-a` rewired in place (live plan)

A one-shot rewrite of the stored `plan_json` (by zone id, not regenerated)
turned the mesh into a theme-grounded network:

- **48 → 30 `nav_scene` edges** = 15 reciprocal bidirectional pairs, no
  dangling/duplicate exits.
- **Interior spine:** `00↔01↔{02,04}`, `02↔08`, `04↔{05,10}`, `10↔{11,13}`.
- **Outdoor hub (05, the twin-arch courtyard):** `05↔{04,14,09,12}`, with
  `09↔03`, `14↔{15,07}`, `12↔06`.
- **1 → 4 `nav_place` portals:** `06 forest-campsite → forest`,
  `12 lantern-path → forest`, `15 cottage-gate → village`,
  `09 city-terrace → village`. Pruned nav zones became `talk_seed`s on the
  depicted object (no dead taps); two mislabelled nav zones (a bench, a swivel
  chair) became `comfort`.
- Verified: all 16 cells reachable from cell 0, every edge reciprocal.

`treetops` was deliberately **not** force-fit into home — its natural entrance
is from the forest, so that portal belongs in a forest-bundle plan, not here
(home grows connections over time per design/28, hub model).

### Live-places caveat (important)

Portal targets must be **non-deprecated** world places. The world registry
(`mochi-places:bundles`) lists `forest` / `village` (scene-bundle-b) /
`treetops` (scene-bundle-c) / `home`; the canonical seeds **`kitchen` /
`window` / `garden` are deprecated** (`mochi-places:deprecated`) and were
intentionally avoided — a portal to a deprecated id is a broken edge. The
planner's `navList` is built from live `navTargets`, so future generation
already respects this, but any hand-wiring must check the deprecated set.

## `home` unified on scene-bundle-a; legacy scene-v1 dropped (2026-06-06)

Reviewing the graph surfaced a nav_place `home` edge resolving to **`scene-v1`**
— the legacy 2-cell home sheet (no `scene_plans` row, an orphan node). Root
cause: `places.ts` `CANONICAL_SEEDS` derived `home → scene-v1` from
`shared/locations.ts`, and `worldPlaces()`'s "a bundle can't shadow a seed" rule
let that seed win over the registered bundle `home → scene-bundle-a`. So the
graph resolver (`resolveTarget` reads the `places` table), the nav picker, the
planner's `navTargets`, and `/enter` all pointed `home` at the orphan. The
**device was unaffected** — firmware special-cases go-home to the embedded
bundle (`main.cpp`: `strcmp(loc,"home")==0 → scene_pack_load_home()`), never
fetching `scene-v1`.

Fix (substrate-only, low risk): `CANONICAL_SEEDS` now maps `home → scene-bundle-a`
and the 11 existing `places.home` rows were `UPDATE`d to match. Verified: 0 rows
reference `scene-v1`; `home` resolves to `scene-bundle-a` everywhere. `scene-v1`
remains *only* as the web renderer's legacy home template
(`shared/locations.ts` `LOCATIONS.home.sheet` → `resolveScene`); fully retiring
it means migrating the web home to a bundle-aware template — a separate
follow-up (ties into design/28 "home as a fetched growable place").

## All bundles rewired + a connected world (2026-06-06)

The same grid-mesh pathology held in every bundle (48 nav_scene edges each,
sparse/incoherent portals), and the *world* didn't loop: nothing routed back to
`home` and nothing reached `treetops`. All three remaining bundles were rewired
in place (by zone id, not regenerated), with grid-adjacent spines chosen for
thematic continuity (so baked nav-arrow directions stay correct) and surplus
nav zones converted to `talk_seed` on the depicted object:

| bundle (place)            | nav_scene | portals (cell → place)                         |
|---------------------------|-----------|------------------------------------------------|
| `scene-bundle-a` (home)   | 30        | 06→forest, 12→forest, 15→village, 09→village   |
| `scene-bundle-b` (village)| 36        | 00→home, 04→forest                             |
| `scene-bundle-c` (treetops)| 38       | 00→forest, 15→forest                           |
| `the-forest-a` (forest)   | 38        | 07→village, 12→treetops, 15→home               |

The resulting **world graph is connected and fully reciprocal** — every place
reaches every other and every link has a return:

```
home ⇄ forest ⇄ village
         ⇅
      treetops
home ⇄ village        (home↔village direct, too)
```

Verified across all four: 16 cells each, **0 stranded cells** (every cell has
≥1 exit), **0 one-way/unreciprocated nav edges**.

## Embedded home re-baked as a format=1 pack (firmware)

`scene-bundle-a` is the **embedded** home (`firmware/main/assets/scenes_a.mpk`,
the offline cold-boot fallback — design/28/31). The old embedded binary was a
**format=0** SPRITE·FORGE export whose zones came from the hand-authored
`SCENES_A_ZONES` table in `scenes_a_meta.h` (only 4 cells zoned, name-based
actions) — it did not carry the redesigned nav at all.

Re-baked it from substrate so the offline fallback matches the served pack:

```
curl -s https://mochi.val.run/devsprite/pack/scene-bundle-a \
  -o firmware/main/assets/scenes_a.mpk     # 164 KB, MPK1, x-mpk-format:1, 16 cells
```

This is now a **format=1** pack with zones + nav (incl. the portals) inline.
`scene_pack.c` already gates on `s_pack.format == 1` everywhere, so the embedded
bundle hit-tests its inline zones and the `SCENES_A_ZONES` meta table is bypassed
(retained only as a legacy fallback + because the symbols are referenced;
`SCENES_A_COUNT` still 16, matching the binary). `firmware/version.txt` bumped
**0.3.19 → 0.3.20** so the embedded-asset change ships via OTA. (Online devices
≥0.3.18 already hot-refresh home from `/pack` per design/31; this bake updates
the *offline* path + the factory image.)

## Possible follow-ups

- Model `kind:2` relative-nav as intra-sheet cell edges for a per-cell view.
- Per-cell nav badges overlaid on the existing dashboard previews.
- Optional `nav_place` → target *cell* pinning (design/28) once shipped.
- **Log invalid `nav_place` drops** in the studio (`draftScenePlan`/
  `mapDraftZone`) instead of silently zeroing them, so the planner's intended
  portals are visible.
- A **reachability lint** on the graph view (flag one-way exits + orphans).
