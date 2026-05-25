/*
 * pet_pack — runtime accessor for the embedded pet_a MPK1 pack.
 *
 * Bundles the device's pet expression cells (96×96 mochi 2-plane) into
 * the firmware image so `render_with_expression("neutral", …)` etc.
 * can be served from flash before WiFi is up — and stays serving
 * after, on every cache miss. Network fetch becomes a fallback for
 * expressions that aren't in the bundled pack rather than the primary
 * path. Means the device can boot, render its pet, and drive the
 * substrate offline once it's been paired.
 *
 * Pet labels mirror SPRITE_NAMES in main/mood.c — re-export the pack
 * with the SPRITE·FORGE "pet" preset and the labels arrive named
 * (`neutral`, `happy`, `sleeping`, …) so this module can do
 * mpk_find by expression string directly.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-shot bring-up. Validates the embedded pack header. Idempotent.
 * Talks to the network through pack_cache_active to refresh the
 * server-authored pack — DO NOT call this before WiFi is up (the
 * lwip mbox isn't initialised yet at boot splash time and lwip
 * asserts inside getaddrinfo). For pre-WiFi callers (the boot
 * splash) use pet_pack_init_embedded(). Returns true if the pack
 * opened cleanly. */
bool pet_pack_init(void);

/* Embedded-only bring-up. Opens the bundled pet_a.mpk blob without
 * any network probe. Safe to call before WiFi is up. A subsequent
 * pet_pack_init() call will upgrade to the server-synced pack.
 * Idempotent: a later call (embedded or full) finds s_open already
 * set and returns true. */
bool pet_pack_init_embedded(void);

/* True if the pack is open and contains a cell whose label matches
 * `expr`. Callers use this to decide whether to skip the network
 * fetch for an expression (see render_with_expression). */
bool pet_pack_has(const char *expr);

/* Copy the named expression's ink + mask planes into `dst_ink` and
 * `dst_mask`. Buffers must be at least dst_bytes long; for the
 * shipped 96×96 pack that's 1152 each. Returns true on success.
 *
 * Wrong cell dimensions, missing label, missing mask plane, or a
 * dst_bytes smaller than the pack's plane size all return false —
 * callers fall back to the network path on a miss. */
bool pet_pack_load(const char *expr,
                   uint8_t *dst_ink, uint8_t *dst_mask,
                   size_t dst_bytes,
                   uint16_t *out_w, uint16_t *out_h);

#ifdef __cplusplus
}
#endif
