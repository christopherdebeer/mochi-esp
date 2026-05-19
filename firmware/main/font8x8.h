/*
 * 8x8 bitmap font for the e-paper UI layer.
 *
 * Covers printable ASCII 0x20..0x7E. Glyphs are stored row-major,
 * row 0 = top, with bit 0 = leftmost column (this matches the
 * dhepper public-domain font; the opposite of the hand-drawn M2
 * glyphs that have since been retired). Bit set = ink (black),
 * bit clear = paper (white).
 */

#pragma once

#include <stdint.h>

extern const uint8_t font8x8_basic[96][8];

/* Returns pointer to the 8-row glyph for `c`, or to the '?' glyph
 * for any byte outside the 0x20..0x7E range. */
const uint8_t *font8x8_glyph(char c);
