/*
 * ui_dialog — a dismissible modal overlay drawn into the composite
 * framebuffer.
 *
 * Unlike the canned full-frame screens in epd_ui (provisioning,
 * pairing, key-portal) which own the whole panel, a dialog is an
 * overlay: the caller composes the normal pet frame first, then
 * stamps the dialog on top and pushes one frame. This keeps the pet
 * visible behind the dialog and lets the dialog be dismissed by
 * simply re-rendering the pet without it.
 *
 * Rendering is a thin 1bpp blit into the existing 200×200 MSB-first
 * composite buffer — same packing as the compositor and thought
 * bubble. The function returns the touch hit rectangle for the single
 * action button so the caller's touch classifier can route a tap on
 * it; a tap anywhere else is the caller's cue to dismiss.
 *
 * See design/21-nonblocking-wifi.md.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace ui_dialog {

/* Tap hit rectangle, panel coordinates. Half-open: [x0,x1) × [y0,y1). */
struct HitRect {
    int x0, y0, x1, y1;
};

/* Stamp a centered modal card into `dst` (dst_w × dst_h, 1bpp
 * MSB-first). The card is a white panel with a black border holding:
 *   - `title`  : bold-ish header line (centered)
 *   - `line1`  : body line (centered, may be NULL)
 *   - `line2`  : body line (centered, may be NULL)
 *   - `action` : an inverted (black-on-… i.e. white-on-black) button
 *                at the bottom (may be NULL for an info-only card)
 *
 * Strings are drawn with the 8×8 font at scale 1; keep each ≤ ~20
 * chars so they fit the card width. Only the card rect is written —
 * the rest of the framebuffer (the pet behind it) is untouched.
 *
 * `out_action` receives the button hit rect (enlarged a few px for
 * fat fingers). May be NULL. If `action` is NULL the rect is zeroed. */
void render(uint8_t *dst, size_t dst_w, size_t dst_h,
            const char *title, const char *line1, const char *line2,
            const char *action, HitRect *out_action);

/* Convenience bounds check. Safe with r == NULL (returns false). */
bool hit_contains(const HitRect *r, int x, int y);

}  /* namespace ui_dialog */
