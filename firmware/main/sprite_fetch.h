/*
 * One-shot HTTPS GET for a fixed-size 1-bit packed bitmap.
 *
 * The wire contract (design/05-sprite-format.md): GET returns
 * exactly `expected_len` bytes with Content-Type
 * application/octet-stream. The function copies them into `out` and
 * returns true. Any deviation — wrong status, wrong length, TLS
 * error, transport timeout — returns false and logs the cause.
 *
 * Caller owns `out`; it must be at least `expected_len` bytes.
 *
 * Synchronous. Blocks the calling task for the duration of the
 * fetch. The Mochi pet UI is button-driven, so blocking the click
 * handler is the right model — we want "click → fox" to feel like
 * one operation. If we ever need background refresh, wrap this in
 * a task or use the async esp_http_client API.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

bool sprite_fetch(const char *url, uint8_t *out, size_t expected_len,
                  uint32_t *elapsed_ms);

/*
 * Variable-length GET. Fetches up to max_bytes into `out` and reports
 * the actual body length via *out_size. Returns false on transport
 * error, non-200, empty body, or a body that exceeds max_bytes.
 * Used by the pack cache (pack_cache.cpp) where the MPK1 size isn't
 * known ahead of time.
 */
bool sprite_fetch_blob(const char *url, uint8_t *out, size_t max_bytes,
                       size_t *out_size, uint32_t *elapsed_ms);

/*
 * Variant that fetches /devsprite/cell/<sheet>/<cell> responses.
 *
 * Wire format (eink-pet:design/05-sprite-format.md, "cell" section):
 *
 *   header (8 bytes):
 *     width  : u16 big-endian
 *     height : u16 big-endian
 *     flags  : u8   (bit 0 = "has mask plane")
 *     reserved : 3 bytes, must be zero
 *   ink plane:  ((w+7)/8) * h bytes — bit 0 = draw black (line work)
 *   mask plane: ((w+7)/8) * h bytes — bit 0 = opaque (pet pixel),
 *                                     bit 1 = transparent (let scene
 *                                     show through)
 *
 * The mask plane is currently always present (flag bit 0 = 1).
 *
 * On success: fills *out_w / *out_h with the cell dimensions, copies
 * the ink plane into `out_ink` and the mask plane into `out_mask`.
 * Both buffers must be at least plane_cap bytes; we fail if the
 * server returned a plane larger than that.
 */
bool sprite_fetch_cell(const char *url,
                       uint8_t *out_ink, uint8_t *out_mask,
                       size_t plane_cap,
                       uint16_t *out_w, uint16_t *out_h,
                       uint32_t *elapsed_ms);

/*
 * Cheap ETag probe via HEAD. Hits the URL with HTTP_METHOD_HEAD,
 * pulls the ETag response header, and copies it into `out`
 * (caller-owned, NUL-terminated). Returns true on 200 + ETag
 * present. Used by the sprite cache layer to decide whether
 * locally-stored bytes are still valid.
 */
bool sprite_fetch_head_etag(const char *url, char *out, size_t out_cap);
