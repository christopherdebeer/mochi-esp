# 30 — UI sprites for the dev-menu + stats (and the icons we still need)

Status: stats page implemented (firmware); studio keying editor, cell-title
editor + green-chroma icon generation shipped (in a dedicated **Icons**
panel); `ui-icons-a` seeded as the single consolidated icon sheet;
menu-tile restyle is spec + backlog below.

## Consolidation: one icon sheet (`ui-icons-a`)

Rather than grow a second sheet, `ui-icons-a` (4×4 = 16 cells) is seeded as
the **single** UI icon set — the existing `ui-v1` four (`heart`, `star`,
`bowl`, `ball`; stats + care chrome) plus the menu wishlist, row-major:

```
heart   star     bowl   ball
home    memories places voice
wifi    wifi_off key    models
update  channel  repair consolidate
```

Generate them (Icons panel) then the firmware migrates its `ui-v1` fetches
(`CARE_ICON_KEYS`, the stat-row `get_icon` keys) to `ui-icons-a` — one
sheet, one keying config, one generation pass. (That firmware pointer swap
is the remaining step; `ui-v1` stays as the fallback until then.)

### Icon cell size MUST be 80×80 (not the scene default 200)

The device UI-icon path expects **80×80 native** cells (`main.cpp`
`UI_CELL_NATIVE_W/H = 80`, an 800-byte buffer) which it downsamples to the
48px `ICON`; it **rejects** any other size (`w != 80`). `ui-icons-a` was
created at the scene default **200×200** — 2.5× oversized for a 48px target
*and* incompatible with the firmware. Fixed: resized to **80×80**
(= `ui-v1`), titles preserved — so the migration is now a one-line
sheet-id swap (same native size). And the studio create default is now
**category-aware** (`NewSheet.tsx`): scene→200, pet→96, ui/item→80, so new
icon sheets start correct.

**Dev-menu use — all shipped (firmware 0.3.10):** (1) size — done; (2) art
generated + committed (Icons panel); (3) firmware now points at
`ui-icons-a` and the **dev-menu tiles draw icons** too:

- Shared `MOCHI_UI_SHEET "ui-icons-a"` (`board_pins.h`); `main.cpp` fetches
  the care/stat icons (heart/star/bowl/ball) + ETag-probes + caches under
  it (the device-critical keys are present).
- `dev_menu.cpp` `get_icon` now **lazy-fetches on a cache miss** (native
  80×80 → downsample 48 → cache), so any tile glyph loads the first time
  its page opens (care/stats are pre-warmed at boot, so only menu-only
  glyphs fetch on open). `ICON_SLOTS` raised to 18.
- `Tile` gained an `icon` key; `draw_tile` renders **icon-top + label-
  below, flat** for icon actions (falls back to the filled style if the
  icon isn't available yet — e.g. offline first paint; toggles keep their
  pill). Per-page mapping: home/memories/places · wifi/secret_key/
  sparkle_stars · update/(channel toggle)/wifi/wifi_off/repair/dream.

Builds clean (esp32s3, -Werror); 0.3.10 baked. Needs on-device review —
the line-screen icon dither + tile layout are unverified on the panel,
and the first open of each menu page does a brief blocking icon fetch.

## Studio panels (this pass)

To avoid bloating the Sheet panel, the **Icons** panel (`studio/panels/
Icons.tsx`, shown for ui/item) owns the icon-authoring workflow — **cell
titles** + **generation** — while `SheetPanel` keeps only art mechanics
(source upload / geometry / keying / cell grid). Scene-only panels (Plan,
Zones, Places, the plan-driven Generate) are hidden for non-scene sheets.

## Goal

Move the device chrome from text-label tiles toward an **icon-forward**
look: a `ui-v1` sprite *inside* each control with the **text label centred
below**, **no persistent stroke** (a tile is just icon + label on paper),
and a **stroke/invert flash only on tap** to confirm the press registered.
Page-1 stats likewise become **icon + progress bar** instead of
`happy 72  full 58  energy 63` text.

## What we have vs. what we need

The `ui-v1` sheet today exposes exactly four cells the firmware fetches +
downsamples (main.cpp `CARE_ICON_KEYS`): **`heart` · `star` · `bowl` ·
`ball`** (80×80 native → 48×48 cached, 2-plane ink+mask). That's enough to
icon the **stats** (happiness→heart, fullness→bowl, energy→star) but covers
**none** of the dev-menu actions. So this pass "makes do": it ships the
stats redesign with the icons we have and lays the dev_menu icon plumbing
(blit + cache loader), and the menu actions stay text-only until the icons
below exist.

### New-icon wishlist (to generate into `ui-v1`)

Per dev-menu action / surface. Keys are suggestions; keep them short +
lower-case to match the existing four.

| key | for | notes |
| --- | --- | --- |
| `memories` | MenuP1 "Memories" | book / photo stack |
| `places` | MenuP1 "Places" | map pin / signpost (echoes nav_place) |
| `home` | MenuP1 "Go home" | little house |
| `wifi` | P2 "Switch WiFi", P3 "Add WiFi" | signal arcs |
| `wifi_off` | P3 "Forget WiFi" | arcs with slash |
| `key` | P2 "OpenAI key" | key |
| `models` | P2 "AI models" | chip / sparkle |
| `update` | P3 "Update now" | down-arrow into tray |
| `channel` | P3 "Channel" toggle | two-way / beta flask |
| `repair` | P3 "Re-pair" | link / QR |
| `consolidate` | P3 "Consolidate" | moon / broom (sleep reflection) |
| `voice` | (future) voice toggle | mic |
| `play` | stats / play affordance | `ball` may already serve |

Mood glyphs for the stats title are a separate set if we ever want them
(happy / sleepy / hungry / lonely faces) — out of scope here.

## Studio: a `ui-v1` sheet generation surface

Today `ui-v1` cells are authored ad-hoc; the studio has no first-class way
to (re)generate icon cells the way it generates scene sheets. To fill the
wishlist repeatably we want a small **UI-sheet config** in the studio,
distinct from scene-sheet generation. The **keying** half is **shipped**;
the **generation** half (per-icon prompts) is the remaining gap.

### Keying editor — ported into the studio (done)

Investigated the legacy `/dev` keying (design notes in this doc's history):
it's a **corner-sample chroma key** (`c15r/mochi` `backend/keying-pixel.ts`)
— average the four PNG corners → key colour, `alpha=0` within `tolerance`
(0–100 → RGB distance), optional **corner-feather** edge smoothing — then
the device 1-bit ink+mask threshold (`c15r/mochi-device` `encode.ts` +
`presets.ts`, `icon` preset = flat threshold 128). It's already
**category-aware**: `ui`/`pet`/`item` default to `corner-feather`, `scene`
to `off` (`shared/sheets/keying.ts`), with per-sheet params stored at
`mochi-sheets:keying:<id>` and a server re-derive on save
(`GET/PUT/DELETE /sheets/:id/keying`, `backend/sheets.ts`).

The studio now exposes this directly (no more bouncing to `/dev`):
`ui` is already a selectable category in `NewSheet.tsx` (so uploads key
with `corner-feather` by default), and `SheetPanel` gained a **keying
editor** (`studio/panels/Sheet.tsx`) — algorithm + tolerance/featherPx/
cornerSamplePx controls, a **source-vs-keyed preview** (keyed shown on a
checkerboard so transparency is visible), Save (→ `PUT /keying`, server
re-derives) and Reset-to-default (→ `DELETE /keying`). Backed by new
`fetchKeying` / `saveKeying` / `resetKeying` / `sheetDerivedURL` in
`studio/api.ts` hitting the same endpoints.

### Cell titles = per-icon prompts (done)

For a UI icon sheet, a cell's **key IS its title** — the device fetches
`/devsprite/cell/<sheet>/<key>`, and the key doubles as the one-word
generation target (`memories`, `places`, `home`, …). The server's
`saveUserTemplate` already accepts custom cell keys (sanitises to
`[A-Za-z0-9_]`, dedups). The studio `SheetPanel` now has a **cell-titles
editor** (`studio/panels/Sheet.tsx`, user sheets only): one text field per
cell, Save → `saveCellKeys` (re-POSTs the template with new keys, geometry
preserved) → `onReload` refetches the sheet list (renaming changes the
cell set, so a `bust` bump isn't enough). Verified the round-trip is a
clean idempotent `200`. So `ui-icons-a`'s `cell_00…` can be renamed to the
wishlist icon names, ready to drive generation.

### Icon generation — green chroma (done)

`SheetPanel` (ui/item sheets) now has a **generate-icons** section:
`generateIconSheet` (`studio/api.ts`) builds the prompt from the cell
**titles as per-icon targets**, rasterises the **labelled grid guide**
(`layout.svg` — cell titles, no zone overlays) and fetches a **style
exemplar** (default `ui-v1`), calls **gpt-image-2** (`/v1/images/edits`,
BYO key, mirroring `generateSceneSheet`), previews, then commits via
`uploadSheetPngBase64` → the server keys it + extracts cells.

**Chroma key is GREEN, not cream** (`#00FF00`, `buildIconPrompt`): green is
never a colour used in black line art, and — crucially — a solid green fill
*inside enclosed shapes* (ring centres, keyholes) lets the keyer drop those
**holes**, which a cream key sampled from corners can't guarantee. The
prompt forbids any green in the artwork and demands a solid green field
everywhere (gutters + holes). The sheet's `corner-feather` keying
(corner-sampled → green key colour; green is far from black/white so a
modest tolerance is safe) removes it cleanly; tune in the keying editor if
needed. gpt-image sizes snap to the nearest supported (`pickGptImageSize`:
square/landscape/portrait); the grid-scales-to-art extractor handles the
1024² output against the 840² grid spec.

- **Geometry.** Icons are square (native → 48×48 cached, plus the ~24px
  stats variant), drop-in for `sprite_cache`'s
  `<key>_icon_<w>x<h>_{ink,mask}` keys.
- **Verification.** Transpiles (200); `layout.svg` guide + keying endpoints
  confirmed live. The gpt-image-2 round-trip itself runs browser-side with
  the user's key — unverified here, but mirrors the proven scene path.

### Icon prompt is an editable template (done)

The prompt is no longer hardcoded — it's a registered template
**`icon.imagegen.v1`** in the prompt-template system, so it's
**editable / overridable / previewable** in the studio Templates panel
alongside the scene/planner ones. Wiring:

- `shared/scenes-spec.ts`: `PROMPT_TEMPLATE_IDS.iconImagegen`,
  `DEFAULT_ICON_IMAGEGEN`, the `PromptTemplates.iconImagegen` field +
  default, `PROMPT_PLACEHOLDERS` (`cols` / `rows` / `chroma` / `titleList`),
  and `mergePromptTemplates`.
- `backend/prompt-templates.ts`: a new **`icon`** family in the
  `GET /api/prompt-templates` listing + `defaultBodyFor` / `labelFor` /
  `familyFor`, so `PUT` (override) and `DELETE` (revert) work for it.
- `studio/api.ts`: `generateIconSheet` now fetches the template body
  (override or default) and resolves `{cols}{rows}{chroma}{titleList}`
  **client-side** (`buildIconPrompt`) — the BYO key stays off the server,
  and the studio avoids importing a new cross-val `scenes-spec` symbol
  (sidesteps the esm-cache lag); a built-in fallback covers an unreachable
  listing.
- `studio/panels/Templates.tsx`: an **icon-gen** picker group + a local
  `resolveIconPlaceholders(sheet)` so the preview resolves against the
  selected sheet's cell titles.

Verified live: listing includes `icon.imagegen.v1` (family `icon`), and
PUT→override / DELETE→revert round-trips cleanly.

### Icon dither preset — ui cells now run the pipeline (done)

`POSTPROCESS.icon` was **dead config**: the serve path
(`devsprite.ts`) only ran the dither pipeline for `pet`/`scene`
(`usesPostProcess`), routing `ui`/`item` through `cellToNativeFB` (flat
threshold) — so editing the preset did nothing on-device (the Dither
workbench previews it client-side via a `cfgOverride`, which is why it
*looked* applied). Wired up + tuned:

- `presets.ts` `POSTPROCESS.icon`: flat `threshold` → **line-screen**
  (`algorithm: "screen"`, `threshold 128`, `contrast 0.44`, `angle 45`,
  `spacing 4`). The studio export omitted `angle`/`spacing` because they
  equal the `dither-pipeline` defaults (45°/4px); pinned explicit here.
- `devsprite.ts`: `usesPostProcess` now includes `icon`, so ui/item cells
  run `cellToNativePipeline(POSTPROCESS.icon)` and their cache keys gain
  the `:pp<ver>` suffix.
- `encode.ts`: `POSTPROCESS_VERSION` 5 → 6 (cache-bust; now also keys icon
  cells), and the three cross-val pins in `backend/devsprite-encode.ts`
  bumped `@366` → `@491` so the server serve path picks up the new
  presets/version.

Verified: `/devsprite/cell/ui-v1/heart?nocache=1` re-encodes `200`, 80×80,
2-plane. **Note:** this also re-dithers the live `ui-v1` care/stats chrome
(heart/star/bowl/ball) to the line-screen on next device refetch — and a
4px screen on a 48px glyph is coarse, so the `contrast 0.44` mix may want
revisiting once seen on-panel (tune in the workbench → paste into
`POSTPROCESS.icon` → bump version + pins).

## Firmware: what shipped this pass

- **MenuP1 stats** (`dev_menu.cpp` `render_menu_p1`): the
  `happy/full/energy` text line is replaced by three rows, each a small
  `ui-v1` icon (heart / bowl / star) + a **progress bar** filled to the
  stat's 0–100 value + the numeric value. The bars are plain `fill_rect`
  (no new sprite dependency); the icons load from `sprite_cache` (`ui-v1`,
  the same cached cells the home chrome uses) and **fall back to a one-
  letter tag** (`H`/`F`/`E`) when a cell isn't cached yet (offline first
  boot). Numeric stats are plumbed into `dev_menu::tick()` (decayed
  snapshot, same values the old text line showed).
- **Icon plumbing** (`dev_menu.cpp`): a tiny lazy `ui-v1` icon cache +
  `blit_icon` (per-pixel, nearest-neighbour scaled, matching
  compositor.cpp's MSB-first / mask=1-transparent / ink=0-black
  convention) so any dev-menu surface can draw a cached icon.

## Firmware: spec for the next (on-device-reviewed) pass

The dev-menu **tile restyle** — icon centred in the tile, label centred
below, no persistent border, invert/stroke flash on tap (for actions too,
not just toggles) — is deferred until the wishlist icons exist (an icon-
less flat tile is just centred text, which regresses the legibility of
today's filled-black action tiles for no gain). When the icons land:

- extend `Tile` with an `icon` key (nullable); `draw_tile` draws
  icon-top + label-below when present, else today's style;
- give plain **actions** a tap-flash too: render one inverted frame from
  `dispatch_touch` before returning the action (toggles already flash via
  `toggle_flash`);
- drop the persistent action fill / toggle border in favour of the
  flash-on-tap ack once every tile has an icon to carry the "tappable"
  affordance.
