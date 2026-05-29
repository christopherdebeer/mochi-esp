# 30 — UI sprites for the dev-menu + stats (and the icons we still need)

Status: stats page implemented (this branch); menu-tile restyle + new icons
are spec + backlog below.

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
distinct from scene-sheet generation:

- **Keying / transparency.** Scene cells are opaque backdrops; UI icons are
  **transparent-margin glyphs** (the 2-plane ink+mask the device blits —
  mask=1 transparent, ink=0 black, design/05 + compositor.cpp). The gen +
  keying pipeline must emit a clean alpha so the downsample preserves a
  tight silhouette on paper, not a filled box. This is the opposite keying
  intent from scenes and needs its own keying-pixel / threshold config.
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
- **Review.** Because keying for line-art is finicky, the studio should
  preview each icon on **both** paper-white and a tile (the device draws
  them on white tiles) so a too-light stroke is caught before it ships.

This is a studio + server change (devsprite keying + a UI prompt template +
a sheet-type flag); scoped here as the design, not yet built.

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
