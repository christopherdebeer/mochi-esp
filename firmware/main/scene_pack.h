/*
 * scene_pack — runtime accessor for the embedded scenes_a MPK1 pack.
 *
 * Replaces the boot-time HTTPS fetch of /devsprite/scene-v1 with a
 * zero-network read of cells bundled into the firmware image via
 * EMBED_FILES (see firmware/main/CMakeLists.txt). 16 cells at
 * 200×200 mochi 2-plane.
 *
 * Today's render path:
 *   - Boot composites the *current* scene into scene_fb (a 200×172
 *     PSRAM buffer keyed off the existing 4300-byte scene area). The
 *     pack's cells are 200×200; we crop to the scene area by reading
 *     the top 172 rows of the ink plane verbatim.
 *   - Touch dispatch tests against the named zones for the current
 *     scene index via mpk_zone_test on SCENES_A_ZONES.
 *
 * Scene transitions:
 *   - scene_pack_set(n) flips the current index and re-blits.
 *   - Zone-driven navigation (e.g. tapping `door`) is wired in
 *     main.cpp's touch handler — this module just owns the index +
 *     pixel access.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-shot bring-up. Validates the embedded pack header. Idempotent.
 * Returns true if the pack opened cleanly. */
bool scene_pack_init(void);

/* Number of scenes in the pack. Returns 0 before init. */
uint16_t scene_pack_count(void);

/* Current scene index. Defaults to 0; advances on scene_pack_set or
 * scene_pack_advance. */
uint16_t scene_pack_current(void);

/* Switch to a specific scene. Wraps modulo count. Returns the
 * resulting index for caller convenience. */
uint16_t scene_pack_set(uint16_t idx);

/* Step to the next (delta>0) or previous (delta<0) scene with wrap.
 * Common cases: +1 for `door`, -1 for `back`. Returns the new index. */
uint16_t scene_pack_advance(int delta);

/* Composite the current scene's ink plane into a scene_w × scene_h
 * destination buffer (typically the 200×172 scene area). The pack's
 * cells are taller than scene_h; the top scene_h rows are copied
 * verbatim, rows below scene_h are dropped. dst_stride is bytes per
 * row in the destination (scene_w/8 packed). The pack's source
 * stride is computed from the cell width.
 *
 * Returns true on success, false if the pack didn't open at boot
 * or the destination geometry doesn't match cell width. */
bool scene_pack_blit_current(uint8_t *dst, size_t scene_w, size_t scene_h);

/* Hit-test the current scene's zone set at cell-local (x, y). Sets
 * *out_name to the matched zone's name on hit (the pointer is into
 * the pack/meta — borrowed, lives forever). Returns true on hit. */
bool scene_pack_zone_at(int16_t x, int16_t y, const char **out_name);

#ifdef __cplusplus
}
#endif
