#include "fb1bpp.h"

#include <string.h>
#include "font8x8.h"

namespace fb1bpp {

static inline void px(uint8_t *fb, int stride, int x, int y, bool black) {
    uint8_t *b = &fb[(size_t)y * stride + ((size_t)x >> 3)];
    const uint8_t mask = (uint8_t)(0x80u >> (x & 7));
    if (black) *b &= (uint8_t)~mask;
    else       *b |= mask;
}

int text_width(const char *s, int scale) {
    return s ? (int)strlen(s) * 8 * scale : 0;
}

void fill_rect(uint8_t *fb, int fbw, int fbh,
               int x, int y, int w, int h, bool black) {
    if (!fb) return;
    const int stride = (fbw + 7) / 8;
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > fbw ? fbw : x + w;
    int y1 = y + h > fbh ? fbh : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++)
            px(fb, stride, xx, yy, black);
}

void border(uint8_t *fb, int fbw, int fbh,
            int x, int y, int w, int h, int thickness) {
    fill_rect(fb, fbw, fbh, x, y, w, thickness, true);
    fill_rect(fb, fbw, fbh, x, y + h - thickness, w, thickness, true);
    fill_rect(fb, fbw, fbh, x, y, thickness, h, true);
    fill_rect(fb, fbw, fbh, x + w - thickness, y, thickness, h, true);
}

void text(uint8_t *fb, int fbw, int fbh,
          int x, int y, int scale, const char *s,
          bool black, bool opaque) {
    if (!fb || !s || scale < 1) return;
    const int stride = (fbw + 7) / 8;
    int cur = x;
    for (const char *p = s; *p; p++, cur += 8 * scale) {
        if (cur >= fbw) break;          /* fully off the right edge */
        const uint8_t *g = font8x8_glyph(*p);
        for (int row = 0; row < 8; row++) {
            const uint8_t bits = g[row];
            for (int col = 0; col < 8; col++) {
                const bool on = (bits >> col) & 1;
                if (!on && !opaque) continue;
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        const int xx = cur + col * scale + dx;
                        const int yy = y + row * scale + dy;
                        if (xx < 0 || yy < 0 || xx >= fbw || yy >= fbh)
                            continue;
                        px(fb, stride, xx, yy, on ? black : !black);
                    }
                }
            }
        }
    }
}

void text_centered(uint8_t *fb, int fbw, int fbh,
                   int y, int scale, const char *s,
                   bool black, bool opaque) {
    int x = (fbw - text_width(s, scale)) / 2;
    if (x < 0) x = 0;
    text(fb, fbw, fbh, x, y, scale, s, black, opaque);
}

void text_centered_in(uint8_t *fb, int fbw, int fbh,
                      int x, int w, int y, int scale, const char *s,
                      bool black, bool opaque) {
    int tx = x + (w - text_width(s, scale)) / 2;
    if (tx < x + 1) tx = x + 1;
    text(fb, fbw, fbh, tx, y, scale, s, black, opaque);
}

}  /* namespace fb1bpp */
