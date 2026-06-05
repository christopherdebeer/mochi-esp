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

## Possible follow-ups

- Model `kind:2` relative-nav as intra-sheet cell edges for a per-cell view.
- Per-cell nav badges overlaid on the existing dashboard previews.
- Optional `nav_place` → target *cell* pinning (design/28) once shipped.
