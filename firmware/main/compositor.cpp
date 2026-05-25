#include "compositor.h"

#include <string.h>

namespace compositor {

void clear_to_paper(uint8_t *dst, size_t dst_w, size_t dst_h) {
    /* Stride in bytes per row. */
    size_t stride = (dst_w + 7) >> 3;
    memset(dst, 0xff, stride * dst_h);
}

void copy_full(uint8_t *dst, const uint8_t *scene, size_t bytes) {
    memcpy(dst, scene, bytes);
}

void blit_mask(uint8_t *dst, size_t dst_w, size_t dst_h,
               const uint8_t *src, size_t src_w, size_t src_h,
               int dx, int dy) {
    size_t dst_stride = (dst_w + 7) >> 3;
    size_t src_stride = (src_w + 7) >> 3;

    for (size_t sy = 0; sy < src_h; sy++) {
        int py = dy + (int)sy;
        if (py < 0 || (size_t)py >= dst_h) continue;
        for (size_t sx = 0; sx < src_w; sx++) {
            int px = dx + (int)sx;
            if (px < 0 || (size_t)px >= dst_w) continue;

            const uint8_t s_byte = src[sy * src_stride + (sx >> 3)];
            const uint8_t s_bit_mask = (uint8_t)(1u << (7 - (sx & 7)));
            /*
             * src bit 0 → ink → clear dst bit (draw black).
             * src bit 1 → paper/transparent → leave dst alone.
             * No bytewise fast path here; hot pixel loops on a 200×200
             * pet (~800 ms in the per-pixel API) used to be the bottleneck,
             * but our composite is roughly 200×200 = 40k iterations and
             * the inner body is ~3 instructions — finishes in a few ms.
             */
            if ((s_byte & s_bit_mask) == 0) {
                const size_t d_off = (size_t)py * dst_stride + ((size_t)px >> 3);
                const uint8_t d_bit_mask = (uint8_t)(1u << (7 - ((size_t)px & 7)));
                dst[d_off] &= (uint8_t)~d_bit_mask;
            }
        }
    }
}

void downsample_plane(uint8_t *dst, size_t dst_w, size_t dst_h,
                      const uint8_t *src, size_t src_w, size_t src_h) {
    const size_t dst_stride = (dst_w + 7) >> 3;
    const size_t src_stride = (src_w + 7) >> 3;
    /* Caller pre-fills dst as needed; we don't memset here because
     * the rule is monotonic — we only ever clear bits. */
    /* Float-free: scale factors as ratios. For each dst pixel, the
     * mapped source span is [floor(d * src/dst), floor((d+1) * src/dst)). */
    for (size_t dy = 0; dy < dst_h; dy++) {
        size_t sy0 = (dy * src_h) / dst_h;
        size_t sy1 = ((dy + 1) * src_h) / dst_h;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        if (sy1 > src_h) sy1 = src_h;
        for (size_t dx = 0; dx < dst_w; dx++) {
            size_t sx0 = (dx * src_w) / dst_w;
            size_t sx1 = ((dx + 1) * src_w) / dst_w;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            if (sx1 > src_w) sx1 = src_w;

            bool any_clear = false;
            for (size_t yy = sy0; yy < sy1 && !any_clear; yy++) {
                for (size_t xx = sx0; xx < sx1; xx++) {
                    const uint8_t b = src[yy * src_stride + (xx >> 3)];
                    const uint8_t m = (uint8_t)(1u << (7 - (xx & 7)));
                    if ((b & m) == 0) { any_clear = true; break; }
                }
            }
            if (any_clear) {
                const size_t off = dy * dst_stride + (dx >> 3);
                const uint8_t m = (uint8_t)(1u << (7 - (dx & 7)));
                dst[off] &= (uint8_t)~m;
            }
        }
    }
}

void blit_two_plane(uint8_t *dst, size_t dst_w, size_t dst_h,
                    const uint8_t *ink, const uint8_t *mask,
                    size_t src_w, size_t src_h,
                    int dx, int dy) {
    size_t dst_stride = (dst_w + 7) >> 3;
    size_t src_stride = (src_w + 7) >> 3;

    for (size_t sy = 0; sy < src_h; sy++) {
        int py = dy + (int)sy;
        if (py < 0 || (size_t)py >= dst_h) continue;
        for (size_t sx = 0; sx < src_w; sx++) {
            int px = dx + (int)sx;
            if (px < 0 || (size_t)px >= dst_w) continue;

            const size_t s_byte_off = sy * src_stride + (sx >> 3);
            const uint8_t s_bit_mask = (uint8_t)(1u << (7 - (sx & 7)));

            /* Skip transparent pixels (mask bit == 1). */
            if ((mask[s_byte_off] & s_bit_mask) != 0) continue;

            /* Opaque — write ink bit verbatim into dst. */
            const size_t d_off = (size_t)py * dst_stride + ((size_t)px >> 3);
            const uint8_t d_bit_mask = (uint8_t)(1u << (7 - ((size_t)px & 7)));
            if ((ink[s_byte_off] & s_bit_mask) == 0) {
                /* ink == 0 → draw black: clear dst bit. */
                dst[d_off] &= (uint8_t)~d_bit_mask;
            } else {
                /* ink == 1 → draw paper: set dst bit. */
                dst[d_off] |= d_bit_mask;
            }
        }
    }
}

void blit_two_plane_scaled(uint8_t *dst, size_t dst_w, size_t dst_h,
                           const uint8_t *ink, const uint8_t *mask,
                           size_t src_w, size_t src_h,
                           int dx, int dy, size_t out_w, size_t out_h) {
    if (out_w == 0 || out_h == 0 || src_w == 0 || src_h == 0) return;
    const size_t dst_stride = (dst_w + 7) >> 3;
    const size_t src_stride = (src_w + 7) >> 3;

    for (size_t oy = 0; oy < out_h; oy++) {
        int py = dy + (int)oy;
        if (py < 0 || (size_t)py >= dst_h) continue;
        size_t sy = (oy * src_h) / out_h;
        if (sy >= src_h) sy = src_h - 1;
        for (size_t ox = 0; ox < out_w; ox++) {
            int px = dx + (int)ox;
            if (px < 0 || (size_t)px >= dst_w) continue;
            size_t sx = (ox * src_w) / out_w;
            if (sx >= src_w) sx = src_w - 1;

            const size_t s_off = sy * src_stride + (sx >> 3);
            const uint8_t s_bit = (uint8_t)(1u << (7 - (sx & 7)));
            /* Skip transparent source pixels (mask bit == 1). */
            if ((mask[s_off] & s_bit) != 0) continue;

            const size_t d_off = (size_t)py * dst_stride + ((size_t)px >> 3);
            const uint8_t d_bit = (uint8_t)(1u << (7 - ((size_t)px & 7)));
            if ((ink[s_off] & s_bit) == 0) {
                dst[d_off] &= (uint8_t)~d_bit;   /* ink → black */
            } else {
                dst[d_off] |= d_bit;             /* paper → white */
            }
        }
    }
}

}  /* namespace compositor */
