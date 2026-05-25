#include "ui_dialog.h"

#include <string.h>
#include "font8x8.h"

namespace ui_dialog {

/* 1bpp MSB-first: leftmost pixel of a byte is bit 7. Ink (black) = 0,
 * paper (white) = 1 — same convention as the compositor. */
static inline void px_black(uint8_t *fb, size_t stride, int x, int y) {
    fb[(size_t)y * stride + ((size_t)x >> 3)] &= (uint8_t)~(0x80u >> (x & 7));
}
static inline void px_white(uint8_t *fb, size_t stride, int x, int y) {
    fb[(size_t)y * stride + ((size_t)x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
}

static void fill_rect(uint8_t *fb, size_t stride, size_t w, size_t h,
                      int x0, int y0, int x1, int y1, bool black) {
    for (int y = y0; y < y1; y++) {
        if (y < 0 || y >= (int)h) continue;
        for (int x = x0; x < x1; x++) {
            if (x < 0 || x >= (int)w) continue;
            if (black) px_black(fb, stride, x, y);
            else       px_white(fb, stride, x, y);
        }
    }
}

/* Draw `s` at scale `sc` with top-left (x, y). ink_black picks the
 * glyph colour: black-on-white for body, white-on-black for the
 * inverted action button. Clipped to the framebuffer. */
static void blit_text(uint8_t *fb, size_t stride, size_t w, size_t h,
                      int x, int y, int sc, const char *s, bool ink_black) {
    for (size_t i = 0; s && s[i]; i++) {
        const uint8_t *g = font8x8_glyph(s[i]);
        const int ox = x + (int)i * 8 * sc;
        for (int row = 0; row < 8; row++) {
            const uint8_t bits = g[row];
            for (int col = 0; col < 8; col++) {
                if (!((bits >> col) & 1)) continue;
                for (int dy = 0; dy < sc; dy++) {
                    for (int dx = 0; dx < sc; dx++) {
                        const int px = ox + col * sc + dx;
                        const int py = y + row * sc + dy;
                        if (px < 0 || py < 0 ||
                            px >= (int)w || py >= (int)h) continue;
                        if (ink_black) px_black(fb, stride, px, py);
                        else           px_white(fb, stride, px, py);
                    }
                }
            }
        }
    }
}

static int text_w(const char *s, int sc) {
    return s ? (int)strlen(s) * 8 * sc : 0;
}

void render(uint8_t *dst, size_t dst_w, size_t dst_h,
            const char *title, const char *line1, const char *line2,
            const char *action, HitRect *out_action) {
    const size_t stride = (dst_w + 7) / 8;

    /* Centered card. Sized to comfortably hold a scale-2 title, two
     * body lines, and an action bar on the 200×200 panel. */
    const int card_w = 176;
    const int card_h = 100;
    const int cx0 = ((int)dst_w - card_w) / 2;
    const int cy0 = ((int)dst_h - card_h) / 2;
    const int cx1 = cx0 + card_w;
    const int cy1 = cy0 + card_h;

    /* White fill, then a 2px black border so the card reads as a
     * distinct surface over the (possibly busy) scene behind it. */
    fill_rect(dst, stride, dst_w, dst_h, cx0, cy0, cx1, cy1, false);
    fill_rect(dst, stride, dst_w, dst_h, cx0, cy0, cx1, cy0 + 2, true);
    fill_rect(dst, stride, dst_w, dst_h, cx0, cy1 - 2, cx1, cy1, true);
    fill_rect(dst, stride, dst_w, dst_h, cx0, cy0, cx0 + 2, cy1, true);
    fill_rect(dst, stride, dst_w, dst_h, cx1 - 2, cy0, cx1, cy1, true);

    int y = cy0 + 10;

    /* Title, scale 2, centered in the card. */
    if (title) {
        const int tw = text_w(title, 2);
        blit_text(dst, stride, dst_w, dst_h,
                  cx0 + (card_w - tw) / 2, y, 2, title, true);
        y += 16 + 6;
    }

    /* Body lines, scale 1, centered. */
    const char *lines[2] = { line1, line2 };
    for (int i = 0; i < 2; i++) {
        const char *ln = lines[i];
        if (!ln || !ln[0]) continue;
        const int lw = text_w(ln, 1);
        blit_text(dst, stride, dst_w, dst_h,
                  cx0 + (card_w - lw) / 2, y, 1, ln, true);
        y += 8 + 4;
    }

    /* Action button: inverted bar near the bottom of the card. */
    if (out_action) { out_action->x0 = out_action->y0 = 0;
                      out_action->x1 = out_action->y1 = 0; }
    if (action && action[0]) {
        const int bh = 20;
        const int bx0 = cx0 + 10;
        const int bx1 = cx1 - 10;
        const int by0 = cy1 - 8 - bh;
        const int by1 = by0 + bh;
        fill_rect(dst, stride, dst_w, dst_h, bx0, by0, bx1, by1, true);
        const int aw = text_w(action, 1);
        blit_text(dst, stride, dst_w, dst_h,
                  bx0 + ((bx1 - bx0) - aw) / 2, by0 + (bh - 8) / 2,
                  1, action, false);
        if (out_action) {
            const int pad = 6;
            out_action->x0 = bx0 - pad;
            out_action->y0 = by0 - pad;
            out_action->x1 = bx1 + pad;
            out_action->y1 = by1 + pad;
        }
    }
}

bool hit_contains(const HitRect *r, int x, int y) {
    if (!r) return false;
    return x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1;
}

}  /* namespace ui_dialog */
