#include "ui_dialog.h"

#include <string.h>
#include "fb1bpp.h"

namespace ui_dialog {

void render(uint8_t *dst, size_t dst_w, size_t dst_h,
            const char *title, const char *line1, const char *line2,
            const char *action, HitRect *out_action) {
    const int W = (int)dst_w;
    const int H = (int)dst_h;

    /* Centered card. Sized to comfortably hold a scale-2 title, two
     * body lines, and an action bar on the 200×200 panel. */
    const int card_w = 176;
    const int card_h = 100;
    const int cx0 = (W - card_w) / 2;
    const int cy0 = (H - card_h) / 2;
    const int cy1 = cy0 + card_h;

    /* White fill, then a 2px black border so the card reads as a
     * distinct surface over the (possibly busy) scene behind it. */
    fb1bpp::fill_rect(dst, W, H, cx0, cy0, card_w, card_h, false);
    fb1bpp::border(dst, W, H, cx0, cy0, card_w, card_h, 2);

    int y = cy0 + 10;

    /* Title, scale 2, centered in the card. */
    if (title) {
        fb1bpp::text_centered_in(dst, W, H, cx0, card_w, y, 2, title,
                                 /*black=*/true, /*opaque=*/false);
        y += 16 + 6;
    }

    /* Body lines, scale 1, centered. */
    const char *lines[2] = { line1, line2 };
    for (int i = 0; i < 2; i++) {
        const char *ln = lines[i];
        if (!ln || !ln[0]) continue;
        fb1bpp::text_centered_in(dst, W, H, cx0, card_w, y, 1, ln,
                                 /*black=*/true, /*opaque=*/false);
        y += 8 + 4;
    }

    /* Action button: inverted bar near the bottom of the card. */
    if (out_action) { out_action->x0 = out_action->y0 = 0;
                      out_action->x1 = out_action->y1 = 0; }
    if (action && action[0]) {
        const int bh = 20;
        const int bx0 = cx0 + 10;
        const int bw = card_w - 20;
        const int by0 = cy1 - 8 - bh;
        fb1bpp::fill_rect(dst, W, H, bx0, by0, bw, bh, true);
        fb1bpp::text_centered_in(dst, W, H, bx0, bw, by0 + (bh - 8) / 2,
                                 1, action, /*black=*/false,
                                 /*opaque=*/false);
        if (out_action) {
            const int pad = 6;
            out_action->x0 = bx0 - pad;
            out_action->y0 = by0 - pad;
            out_action->x1 = bx0 + bw + pad;
            out_action->y1 = by0 + bh + pad;
        }
    }
}

bool hit_contains(const HitRect *r, int x, int y) {
    if (!r) return false;
    return x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1;
}

}  /* namespace ui_dialog */
