# 19 — Studio world consolidation (scene plan → generate → bundle)

**Status:** sketch, 2026-05-23. Plan-of-record for folding scene-plan
authoring, generative source-image production, extraction/post-process,
serving, and MPK1 bundling into one studio pipeline. Continues the device-
sprite consolidation track (design/13–16) and the location/world
reconciliation (design/17).

**Predecessors:** [06-scene-contracts.md](./06-scene-contracts.md) ·
[14-mpk1-edges-and-actions.md](./14-mpk1-edges-and-actions.md) ·
[15-device-sprite-consolidation.md](./15-device-sprite-consolidation.md) ·
[16-on-device-imagine.md](./16-on-device-imagine.md) ·
[17-location-embodiment.md](./17-location-embodiment.md) ·
`c15r/mochi:design/diegetic-interfaces.md` · `world-building.md`

## Why — four systems, one seam

Scene/world authoring smeared across four systems that never reconciled.
The device "scene bundle" is a navigable, zoned, multi-cell *level*;
"places"/imagine are single disconnected scenes; the richest semantic
model is dead.

| System | Geometry | Carries | Nav | Gen | Device? |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `scene_plans` (`shared/scenes-spec.ts`) | 200×172 | **richest:** zones+`intent`+gesture vocab+`petAnchor`+`grammar`+`locationLineage`+`cellDirectives` | semantic intents | guide built **from the plan's zones** | **no consumer** |
| `places` (`backend/places.ts`) | 360×336→200×200 | name+vibe+day/night, **no zones** | flat `move_to_location`; `from_place_id` unused | live imagine (voice + `/dev`) | yes |
| `scene-bundle-a` (MPK1 `format=1`) | 200×200, 16 cells | rect + typed action (`event`/`nav_scene`/`nav_relative`/`talk_seed`/`nav_place`) | authored intra-pack nav | none (hand-authored) | yes |
| studio (`c15r/mochi-device`) | per-sheet | the MPK1 subset **minus `nav_place`** | — | none | authors the above |

**The deployment is inverted:** the most complete diegetic contract
(`scenes-spec.ts`) is the one nothing consumes; the device runs a lossy
subset (`format=1`); the studio authors a still-thinner slice of that. And
there are **two unreconciled substrate zone stores**:

- **thin** `mochi-sheets:zones:<id>` (`user-sheets.ts:124`) — `{cellKey:
  ZoneRec[]}`, `ZoneRec = {x,y,w,h,kind,data,seed}`. Studio-authored
  (`/sheets/:id/zones`), **device-consumed**: `/devsprite/pack/:sheet`
  (`devsprite.ts:757`) reads it via `loadZones` (`:770`) and packs
  `packMpkV1` (`:781`), ETag `…-z{fnv1a(zones)}` (`:785`).
- **rich** `scene_plans.plan_json` (`backend/scene-plans.ts`) — full
  `ScenePlan`. Read by the `getScenePlan` block (`devsprite.ts:893–1092`)
  for a device-facing zones read that **no device pack path consumes**.

Two concrete defects fall out and gate everything:

1. `studio/panels/Zones.tsx:11` `KIND_OPTS` stops at kind 4 — `nav_place`
   (kind 5, `mpk.ts:171`) can't be authored. The cross-place edge that
   turns disconnected scenes into a *world* has no authoring surface.
2. `user-sheets.ts:144` `saveZones` validates `kind ∈ [0,1,2,3,4]` — so
   even if the dropdown offered kind 5, the server would silently strip
   it. Both halves must change together.

## Decisions (2026-05-23)

1. **Single source of truth: `scene_plans`.** `scenes-spec.ts` is the rich
   authoring contract; a place's `sheet_id` maps to a `scene_plans` row.
   The device gets an MPK1 `format=1` **projection**; the web gets the
   360×336 art. This resurrects the richest existing model, matches
   design/17's "a place IS a multi-cell pack," and lets us harvest the old
   `devsprite-dashboard.ts` plan UI rather than reinvent it.
2. **Web-canonical source, projected to device.** Author the plan + art at
   web cell geometry; the existing encoder `cw`/`ch` override
   (`/devsprite/pack/<sheet>?cw=200&ch=200`, design/17) projects to device
   200×200. One source artwork, two consumers.
3. **Doc first, then build.** This file is plan-of-record; implementation
   follows it phase by phase, confirming before each change to the live
   `c15r/mochi` serving path.

## Progress (2026-05-23)

- **P0 + P1 + P3 shipped** to the live vals (see phases). The studio now
  authors `nav_place` world edges and the rich per-cell diegetic contract
  (`scene_plans`), generates source art from a plan (guide + exemplar →
  gpt-image-2, BYO key), and the existing upload/extract/post-process/
  serve/bundle path carries it the rest of the way. The full spine —
  **Sheet → Plan → Generate → Zones → Export** — is in one surface.
- **Dither workbench** reworked: defaults to previewing the per-type
  post-process on the *selected sheet's* source art (preset auto-picked
  from category); custom upload (with configurable target dims) stays as
  the ad-hoc mode.
- **Plan-first / generative loop shipped** (P4 core). The keystone reframe:
  **a sheet's source art is an *output*, not an input** — New Sheet needs no
  image; authoring a plan declares the place; Generate materialises it.
  Landed: a **planner** (`draftScenePlan`, client BYO, JSON-mode chat) that
  drafts zones + intents + gestures + device actions **incl. `nav_place`
  edges** from a seed; a `✨ draft plan` button in the Plan panel; an
  **exemplar picker**; the panel order corrected to **Sheet → Plan → Zones
  → Generate** (zones drive the guide, so they precede generation);
  **author-without-art** (ZoneCanvas/Sheet tolerate a sourceless sheet);
  and the **`cellZones` → guide/prompt bridge** so generation steers
  per-cell. So: 100% generative with manual refinement — every step is an
  editable draft.
- **Verified:** all studio files transpile; `scenes-spec.ts` normalises the
  live kitchen plans + `guide.svg` still renders post-bridge; format=1
  conformance 57/57; the exemplar `source.png` path is CORS-open.
  **In-browser smoke tests still pending** — the MCP endpoint tester can't
  send `application/json` (plan POST) or run BYO-key OpenAI calls (planner
  + image gen): drafting a plan, saving it, and a generation run each need
  a check in the studio.

## The unified model

**A place = a `places` row (identity / location / world-graph node) +
its sheet's `scene_plans` row (diegetic contract + cell variants), joined
on `sheet_id`.** Three projections of one source:

```
                        scene_plans.plan_json  (SOURCE: scenes-spec ScenePlan)
                        zones[] · petAnchor · grammar · cellDirectives · lineage
                                       │
        ┌──────────────────────────────┼──────────────────────────────┐
        ▼                              ▼                               ▼
  layout guide (SVG→PNG)        web render 360×336            device MPK1 format=1
  buildGuideSvg(from zones)     SceneBackground + pet         /devsprite/pack ?cw=200&ch=200
        │                       composited at petAnchor       zones projected from plan
        ▼                                                     (incl. nav_place edges)
  gpt-image-2 (BYO key) ── upload /sheets/:id/png ── keying + per-category
                                       │                 post-process (presets.ts)
                                       ▼
                              extracted device cells (encode.ts) — byte-identical
                              fetch (/devsprite/cell) AND bundle (mpk.ts)
```

Because zones are authored in the **same coordinate space as the source
art** and projected *together* with the pixels (same `cw`/`ch`
transform), the diegetic-interfaces invariant — *the image suggests, the
scene graph decides, and they must agree* — holds across web **and**
device for free. No second alignment step.

### Levels vs variants (per-cell zones)

Two cell semantics must coexist: a **level** (the bundle — each cell is a
*different room* with its own affordances) and a **variant scene** (places
— one room, cells are day/night/weather, affordances shared).
`scene_plans` modelled only the variant case (`plan.zones` shared across
cells). P1 adds `ScenePlan.cellZones` (per-cell `SceneZone[]`): when a
cell has an entry it wins for that cell's projection + guide; otherwise the
shared `zones` broadcast to every cell. Bundles author `cellZones`; places
keep `zones`. Both live in one `scene_plans` row.

### Collapsing the two zone stores

`scene_plans` becomes the source; the thin `mochi-sheets:zones:<id>` store
is retired (or kept only as a derived projection cache). `/devsprite/pack`
derives its `format=1` zones from `getScenePlan(sheet).plan` via the
projection below instead of `loadZones`. The pack ETag's `z{…}` term keys
on the projected zones so boot-sync still invalidates on a plan edit
(design/15). Old per-sheet-zones blobs migrate by a one-shot lift into a
`scene_plans` row (the same recovery design/15 already did for
`scene-bundle-a`).

### Contract projection (rich → device-lossy)

The plan zone carries the semantic layer (for the realtime agent) **and**
a device-projectable typed action (for the firmware hit-test). Extend
`SceneZone` with an optional `deviceAction`; when absent, infer from
`intent` where unambiguous, else `none` (decorative / voice-only).

| plan `SceneZone` | MPK1 `format=1` zone | notes |
| :--- | :--- | :--- |
| `rect` (source space) | `x,y,w,h` (device px) | projected by `cw`/`ch` |
| `intent` + `gestures` | — | agent-only; not on device |
| `deviceAction {kind:event,data}` | kind 1 `event` + `event_kind_t` | care loop |
| `deviceAction {kind:nav_scene,data}` | kind 2 abs cell idx | within-place |
| `deviceAction {kind:nav_relative,data}` | kind 3 signed delta | within-place |
| `deviceAction {kind:talk_seed,seed}` | kind 4 + label table | voice seed |
| `deviceAction {kind:nav_place,place}` | kind 5 + label table | **world edge** |
| `petAnchor` | — | guide + web composite; device renders pet from its own pack |

`nav_place` edges are the world graph: `from_place_id` (today captured-
but-unused, world-building.md) becomes a real, authored adjacency.

## The studio pipeline (end-to-end)

One surface, left-to-right, each stage already has a home to harvest:

1. **Plan** — author the `ScenePlan` for a sheet: zones (rect + role +
   intent + gesture vocab + `deviceAction`, **incl. nav_place**),
   `petAnchor`, `grammar`, per-cell `cellDirectives` (time-of-day /
   weather). **Drafted by the planner** (`✨ draft plan`, client BYO) from
   seed name+vibe+grammar, then refined by hand — the sheet needs no art
   yet; the plan declares the place. (`scene-plans.ts` anticipated this.)
2. **Generate source image** — `buildGuideSvg(plan)` → PNG; pick an
   exemplar sheet (style anchor); `buildScenePrompt(plan, grammar)`;
   `POST /v1/images/edits` with guide+exemplar, **BYO key in the browser**
   (keys-off-server, same as `places-client.ts`). Multi-cell sheet in one
   call (`computeGenerationSize`).
3. **Upload / extract / post-process** — `POST /sheets/:id/png`; server
   keying (`keying-pixel.ts`) + per-category post-process
   (`presets.ts` / `dither-pipeline.ts`). Unchanged.
4. **Serve** — `/devsprite/cell` + `/devsprite/pack` via the one encoder
   (`encode.ts`). Unchanged on the wire (device URLs stay frozen).
5. **Bundle** — `packMpkV1` (`mpk.ts`) with zones projected from the plan
   (the existing Export panel, fed from the plan instead of the thin
   store). Byte-identical to the boot-sync pack.

## Phased migration (each independently shippable)

- **P0 — unblock the world edge (smallest).** ✓ *Shipped 2026-05-23.*
  `nav_place` added to `studio/panels/Zones.tsx` `KIND_OPTS` + a target
  place-id field; `user-sheets.ts` `saveZones` accepts kind 5 + persists
  `place`; `Export.tsx`/`api.ts` thread it; `/devsprite/pack` already
  spreads it into `packMpkV1`. (A place-id *picker* from `/api/sheets` is
  later polish; a text field ships now.)
- **P1 — studio reads/writes `scene_plans`.** ✓ *Shipped 2026-05-23.*
  `SceneZone.deviceAction` + `ScenePlan.cellZones` added to
  `scenes-spec.ts` (`normaliseZone`/`normalisePlan`). New studio Plan
  panel (grammar · seed · pet anchor) + per-zone role/intent/gestures; one
  save writes the rich `scene_plans` row (per-cell `cellZones`) AND the
  thin device store. Thin store still authoritative for the pack.
- **P2 — pack projects from the plan.** `/devsprite/pack` derives
  `format=1` zones from `getScenePlan` via the projection table; ETag
  keys on projected zones; one-shot lift of existing thin blobs into
  `scene_plans`. Thin store retired. **The two stores are now one.**
- **P3 — generation in the studio.** ✓ *Shipped 2026-05-23.* New Generate
  panel (`panels/Generate.tsx`) + `generateSceneSheet` (`api.ts`): fetches
  the plan recipe, rasterises the plan's `guide.svg` (built from its zones)
  to PNG in-browser, fires gpt-image-2 with the BYO key (localStorage; key
  only ever goes to OpenAI), uploads via `/sheets/:id/png`, marks the plan
  ready. Reuses the proven `places-client.ts` multipart shape; the existing
  keying/post-process/serve/bundle path carries it on. A `genVersion`
  cache-bust refreshes the cell thumbnails after a run. *Still to do:*
  `places`/imagine adopting the plan guide (replace the zoneless `scene-v1`
  guide in `places-device.ts:72`) so born places are zoned by construction.
- **P4 — generative planner + world graph.** ✓ *Planner shipped
  2026-05-23* (`draftScenePlan`, client BYO; plan-first reorder;
  author-without-art; `cellZones`→guide bridge; exemplar picker). *Still to
  do:* auto-wire **reciprocal `nav_place` edges** (door A→B ⇒ door B→A) so
  drafts build a real graph; a **places/edges view**; the **voice-imagine**
  path running the planner (so born places are zoned, not single scenes);
  per-cell multi-room planning (the planner currently drafts one room
  applied across the selected cells); costume zone kinds 6/7 (design/17).

### Live-val safety

`c15r/mochi` serves real devices that boot-sync packs. Rules: device
URLs + the cell/pack wire format stay frozen; pack ETag must change
whenever projected zones change (or a device shows stale taps); the
cross-val `devsprite-encode` re-export pin (design/15) bumps in lockstep;
each phase ships behind a verifiable conformance step before the next.

## Open questions / deferred

- **Status chrome.** scenes-spec reserves 200×172; the bundle uses full
  200×200. design/06 wants chrome retired into the scene. Pick the source
  origin in P1; the projector handles the device offset.
- **Intent/gesture on device.** Long-press/double-tap aren't in the FT6336
  dispatch yet; `deviceAction` is tap-only until the gesture layer
  (design/06) lands. The plan still records gestures for the agent.
- **Generative planner shape** — prompt, grammar selection, zone-count
  discipline (diegetic-interfaces caps 2–5; `normalisePlan` clamps 8).
- **Multi-cell beyond day/night** — `cellDirectives` already model
  time-of-day/weather; the device variant resolver is design/17 phase 4.

## Cross-references

- `shared/scenes-spec.ts` — `ScenePlan` / `SceneZone` / `buildScenePrompt` / `buildGuideSvg` / `normalisePlan`
- `backend/scene-plans.ts` — the source table + lifecycle
- `backend/devsprite.ts` — `/pack` (`:757`, `loadZones`) vs scene_plans block (`:893`)
- `backend/user-sheets.ts` — thin zones store (`:124`) + `saveZones` kind gate (`:144`)
- `backend/places-device.ts` — `/orchestration` + `/enter` (device travel)
- `c15r/mochi-device` — `mpk.ts` (`packMpkV1`, kind 5) · `encode.ts` · `studio/` panels
- design/14 (zones/actions) · 16 (imagine) · 17 (location/world reconciliation)
