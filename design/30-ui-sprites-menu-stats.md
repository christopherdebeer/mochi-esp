# 30 — UI sprites for the dev-menu + stats (and the icons we still need)

Status: stats page implemented (firmware); studio keying editor shipped;
menu-tile restyle + new icons are spec + backlog below.

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

### Generation half — still needed

- **Per-icon prompts.** Each cell wants its own short prompt ("a simple
  1-bit line-art %s glyph, centred, thick strokes, transparent
  background") rather than the scene style preamble. A `ui-v1` prompt
  template (sibling to the scene/places templates in `scenes-spec.ts` /
  `places-spec.ts`) keyed per icon name.
- **Geometry.** Icons are square (native 80×80 → 48×48 cached, and we now
  also want a ~24px stats variant). The UI sheet config should pin the
  native cell size + the downsample targets the firmware expects, so a
  regenerated sheet stays drop-in for `sprite_cache`'s
  `<key>_icon_<w>x<h>_{ink,mask}` keys.
- **Review.** The keyed-on-checker preview above already catches a
  too-light stroke before it ships; a paper-white-vs-tile toggle would
  finish this off.

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
