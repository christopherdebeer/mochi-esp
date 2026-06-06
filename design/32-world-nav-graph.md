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

## Possible follow-ups

- Model `kind:2` relative-nav as intra-sheet cell edges for a per-cell view.
- Per-cell nav badges overlaid on the existing dashboard previews.
- Optional `nav_place` → target *cell* pinning (design/28) once shipped.
- **Log invalid `nav_place` drops** in the studio (`draftScenePlan`/
  `mapDraftZone`) instead of silently zeroing them, so the planner's intended
  portals are visible.
- A **reachability lint** on the graph view (flag one-way exits + orphans).
