# 36 — UI consolidation: one drawing core, one toast

Status: implemented, 2026-06-11 (0.3.27)
Branch: `claude/mochi-beta-ota-patch-dbnf2p` (PR #25)
Closes: design/25 refactor A2 (grown), plus the toast posture gap.

## Why now

A comprehensive surface audit (this doc's prerequisite) counted ~28
user-facing UI surfaces drawn by **seven** independent copies of the
same primitives — the 8×8 glyph blit, centred text, fill_rect,
borders — spread across `main.cpp` (render_chrome AND render_asleep),
`epd_ui`, `dev_menu`, `ui_dialog`, and `thought.cpp`. design/25's A2
refactor named five copies; two more had grown since. Each copy
re-derived the same packed-1bpp bit math, and several had drifted in
small ways (whole-glyph vs per-pixel edge clipping, x vs x+1 centring
clamps).

Separately, the menu-triggered feedback messages ("Checking for
updates...", "Consolidating in background...", "Memories — coming
soon...", "Switching to <ssid>") were full-panel takeovers — a
`clear()` + two text lines — which contradicts the posture design/21
committed to when it introduced `ui_dialog` ("an overlay card, not a
full-screen takeover") and reads especially wrong on a diegetic
device: the world blinks out of existence to acknowledge a tap.

## fb1bpp — the one drawing core

New module `fb1bpp` owns the primitives over any packed 1-bit buffer
(MSB-first, stride = ⌈w/8⌉, 0 = ink): `text` (scale, black/white,
opaque/transparent), `text_centered`, `text_centered_in`,
`text_width`, `fill_rect`, `border`. Everything clips per-pixel.

The seven call sites converge as thin wrappers:

| Caller | Was | Now |
|---|---|---|
| `main.cpp` status bar + asleep text | two inline bit-loops on `composite` | `fb1bpp::text` / `text_centered` |
| `epd_ui` draw_text / splash overlay / QR modules | per-pixel `EPD_DrawColorPixel` loops | fb1bpp into `EPD_Buffer()` |
| `dev_menu` tiles / stats / glyphs | own fill/border/blit trio | same names, fb1bpp bodies |
| `ui_dialog` card | own px/fill/blit/text_w quartet | fb1bpp calls |
| `thought.cpp` centred line blit | own copy | `fb1bpp::text` |

To let the `EPD_DrawColorPixel`-based callers (epd_ui, dev_menu) use
the same buffer-oriented core, the vendored driver gains a one-line
`EPD_Buffer()` accessor — same precedent as the earlier non-vendor
`EPD_LoadBuffer` addition, documented in the header. This is also a
real speed win: the old path paid a bounds-checked virtual-ish call
per pixel; the menu's text now writes bytes directly.

Net: −289/+120 lines, and exactly one place where the bit math lives.

The bubble *shape* drawing in `thought.cpp` (cloud discs, scalloped
union outline) and the status bar's WiFi-bars / mic glyph stay
hand-rolled — they're genuinely bespoke graphics, not copies of a
shared primitive.

## epd_ui::toast — feedback without a takeover

`epd_ui::toast(epd, line1, line2)`: a compact bordered card (168×52)
stamped over whatever the driver buffer currently holds, pushed with
a partial refresh. The menu (or pet frame) stays visible around it,
so a tap acknowledgement no longer blanks the world. No hit rect —
it's feedback only; the caller keeps owning any linger/drain/repaint
(Memories/Places keep their 1.2 s + `dev_menu::repaint()` flow; the
others are overwritten by the next live render exactly as before).

Converted: UpdateNow, ConsolidateNow, Memories/Places placeholders,
WifiSwitch. **Not** converted: the pre-reboot messages ("Restarting
for WiFi setup...") — the device is leaving; a full clear is the
honest terminal state for a panel that persists unpowered.

## Deliberately untouched (and why)

- **Touch dispatch** stays three-model (live-loop zones, dev_menu
  button registry, dialog hit rect): unifying it is design/06 M11.5
  territory (diegetic scene zones subsume the corner icons), and a
  premature abstraction now would be churned by that work.
- **WiFi-dialog state ownership** (loose flags in main.cpp) — small,
  works, and a candidate for the same M11.5 pass.
- **The two icon caches** (main.cpp care corners vs dev_menu
  IconSlot): the corner icons are scheduled for retirement
  (design/06), so unifying caches buys little before then.
- **dev_menu navigation docs**: design/22 still describes the old
  BOOT-wheel; reality is PWR double-tap paging. Noted here as drift;
  fix the doc when the menu next changes shape.

## Verification

- `idf.py build` green (esp32s3, ESP-IDF v5.3) at 0.3.27, no
  warnings in touched files.
- Rendering is byte-identical in intent but not bit-identical at
  edges: the shared core clips partial glyphs per-pixel where two of
  the old copies dropped whole glyphs at the right edge, and centred
  text in spans clamps at x+1 (dev_menu's convention) everywhere.
  Both differences only matter for strings that already didn't fit.
- On-device checklist: menu pages + toggles render correctly; toast
  card over MenuP1/P2; provisioning/pairing screens (QR + text);
  boot splash text zones; thought bubble paging; asleep banner.
