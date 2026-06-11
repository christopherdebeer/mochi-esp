/*
 * fb1bpp — the one 1-bit framebuffer drawing core (design/36).
 *
 * Before this module the same handful of primitives — the 8×8 glyph
 * blit, centred text, fill_rect, borders — existed as SEVEN separate
 * hand-rolled copies: main.cpp's render_chrome and render_asleep,
 * epd_ui's opaque and overlay blits, dev_menu's transparent blit,
 * ui_dialog's scaled blit, and thought.cpp's centred blit (the
 * design/25 A2 refactor, grown). They all target the same buffer
 * convention, so they all converge here.
 *
 * Buffer convention (identical across the EPD driver RAM, the main
 * loop's composite, and every overlay buffer):
 *   packed 1-bit, MSB-first within each byte (leftmost pixel = bit 7),
 *   stride = (width + 7) / 8 bytes per row,
 *   0 = ink (black), 1 = paper (white).
 *
 * All functions clip per-pixel against the buffer bounds, so callers
 * can place text/shapes partially off-panel without checks.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace fb1bpp {

/* Width in pixels of `s` rendered at `scale` (glyphs are 8 px wide). */
int text_width(const char *s, int scale);

/* Solid rectangle, black or white. w/h in pixels; clipped. */
void fill_rect(uint8_t *fb, int fbw, int fbh,
               int x, int y, int w, int h, bool black);

/* Rectangle outline, `thickness` px, black. */
void border(uint8_t *fb, int fbw, int fbh,
            int x, int y, int w, int h, int thickness);

/*
 * Glyph string at (x, y), integer `scale`.
 *   black:  glyph colour (false = white, for inverted labels).
 *   opaque: true paints the glyph's off-bits in the opposite colour
 *           (a solid label that overwrites what's behind it); false
 *           leaves them untouched (transparent overlay — artwork or
 *           tile fill shows through the gaps).
 */
void text(uint8_t *fb, int fbw, int fbh,
          int x, int y, int scale, const char *s,
          bool black, bool opaque);

/* `text` centred across the full buffer width. */
void text_centered(uint8_t *fb, int fbw, int fbh,
                   int y, int scale, const char *s,
                   bool black, bool opaque);

/* `text` centred within the horizontal span [x, x+w). */
void text_centered_in(uint8_t *fb, int fbw, int fbh,
                      int x, int w, int y, int scale, const char *s,
                      bool black, bool opaque);

}  /* namespace fb1bpp */
