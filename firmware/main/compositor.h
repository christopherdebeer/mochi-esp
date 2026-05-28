/*
 * On-device 1-bit compositor.
 *
 * The Mochi pet UI composites a scene background, a pet sprite, and
 * 4 care icons into a single 200×200 framebuffer that gets pushed to
 * the SSD1681. Composition rule for each layer beyond the scene:
 *
 *   for every pixel in the source rect:
 *     if source bit == 0 (ink):  clear matching dst bit  (draw black)
 *     if source bit == 1 (paper): leave dst bit alone    (transparent)
 *
 * "1 == transparent" is the trick that makes our cells composable.
 * A pet cell with cream background reads as `1` everywhere outside
 * the inked drawing — and `1` means "leave the scene visible." For
 * panels with no scene, the same buffer renders as black-on-white
 * because the underlying frame was initialised to all-paper (`1`).
 *
 * All buffers are MSB-first within each byte, row-major — same
 * packing as the SSD1681 RAM and the existing 5000-byte fetches.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace compositor {

/* Initialise dst to all-paper (white). Convenience for callers
 * that don't already have a base layer. */
void clear_to_paper(uint8_t *dst, size_t dst_w, size_t dst_h);

/*
 * Downsample a 1-bit packed plane from (src_w × src_h) to
 * (dst_w × dst_h) using any-clear-wins per destination pixel —
 * a destination bit becomes 0 if ANY source pixel mapping to it
 * is 0. Preserves thin line work. Both planes packed MSB-first.
 *
 * Used at boot to shrink the 80×80 ui-v1 icons to 32×32 once,
 * then reuse forever. Same logic the server uses to downsample
 * PNG-pixel cells to template-space cells.
 */
void downsample_plane(uint8_t *dst, size_t dst_w, size_t dst_h,
                      const uint8_t *src, size_t src_w, size_t src_h);

/*
 * Two-plane blit. Behaves like blit_mask except the source provides
 * a separate alpha-style mask:
 *
 *   mask[xy] == 0 (opaque): write ink[xy] verbatim into dst
 *                            (0 → black, 1 → paper-white)
 *   mask[xy] == 1 (transparent): leave dst alone
 *
 * This is what we need for cells whose body is paper but whose
 * margins are transparent — the pet draws its full silhouette in
 * white-on-scene, with line work in black on top, while corners
 * outside the body keep showing the scene.
 */
void blit_two_plane(uint8_t *dst, size_t dst_w, size_t dst_h,
                    const uint8_t *ink, const uint8_t *mask,
                    size_t src_w, size_t src_h,
                    int dx, int dy);

/*
 * Nearest-neighbour scaled two-plane blit. Like blit_two_plane, but the
 * source (src_w × src_h) is resampled into an out_w × out_h rectangle at
 * (dx, dy). Nearest-neighbour keeps 1-bit art crisp (no grey from
 * interpolation). Used to place a pet sprite scaled to fit a scene/splash
 * pet zone (design/20). Out-of-bounds destination pixels are clipped.
 */
void blit_two_plane_scaled(uint8_t *dst, size_t dst_w, size_t dst_h,
                           const uint8_t *ink, const uint8_t *mask,
                           size_t src_w, size_t src_h,
                           int dx, int dy, size_t out_w, size_t out_h);

}  /* namespace compositor */
