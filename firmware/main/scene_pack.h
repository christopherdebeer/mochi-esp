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

#include "mochi_pack.h"   /* mpk_zone_t */

#ifdef __cplusplus
extern "C" {
#endif

/* One-shot bring-up. Validates the embedded pack header. Idempotent.
 * Talks to the network through pack_cache_active to refresh the
 * server-authored pack — DO NOT call this before WiFi is up (the
 * lwip mbox isn't initialised yet at boot splash time and lwip
 * asserts inside getaddrinfo). For pre-WiFi callers (warm-boot
 * pet render) use scene_pack_init_embedded() and let net_worker
 * upgrade later via scene_pack_init(). Returns true on success. */
bool scene_pack_init(void);

/* Embedded-only bring-up. Opens the bundled scenes_a.mpk blob with
 * no network probe — safe to call before WiFi is up. A subsequent
 * scene_pack_init() will resync to the server-authored pack.
 * Idempotent. */
bool scene_pack_init_embedded(void);

/* Swap the active scene pack to a caller-provided MPK1 blob — e.g. a
 * freshly imagined place fetched into PSRAM (design/16). The bytes must
 * OUTLIVE the swap: pass a heap/PSRAM buffer that is never freed, not a
 * stack buffer. Validates the envelope; on success replaces the active
 * pack, resets the current index to 0, and returns true. The previously
 * active pack (embedded or cached) is forgotten until the next reboot. */
bool scene_pack_load_bytes(const uint8_t *mpk);

/* Restore the active pack to the "home" bundle resolved at init (the
 * embedded/server-synced scene-bundle-a). Used when travel returns the
 * pet home (pets.location == "home"). Resets the index to 0. Returns
 * false if init hasn't run. See design/17. */
bool scene_pack_load_home(void);

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

/* True if the current scene has any authored zones. Callers use this
 * to gate UI chrome — when the scene's diegetic affordances cover
 * intent, the legacy corner icons + corner-quadrant fallback are
 * suppressed. Unzoned scenes (most of scenes_a today) keep the
 * old behaviour as a safety net. */
bool scene_pack_current_has_zones(void);

/* Iterate the current scene's zone set. Returns the count via
 * *out_count and a pointer to the zones array (borrowed, lives for
 * the program's lifetime). Returns NULL if the current scene has
 * no zones. */
const mpk_zone_t *scene_pack_current_zones(uint8_t *out_count);

/* Forgiving hit-test: returns the zone whose rectangle is nearest
 * (Chebyshev distance in cell-local pixels) to (x, y), provided that
 * distance is ≤ slop_px. Direct hits return distance 0. Returns true
 * when a zone matched within slop, setting *out_name to its name.
 * Used after scene_pack_zone_at fails so a near-miss can still
 * resolve to the intended zone — touch panels are noisy and authored
 * rects don't always cover what reads as tappable. */
bool scene_pack_zone_near(int16_t x, int16_t y, int slop_px,
                          const char **out_name);

/* Typed action resolved from a tap. Mirrors mpk_action_kind_t and
 * works uniformly across format=0 and format=1 packs:
 *   - format=1 reads inline zones via mpk_zone_get verbatim.
 *   - format=0 looks up the matched zone name (door, food, …) in
 *     a static name→action table inside scene_pack.c so callers
 *     don't have to keep the strcmp ladder around.
 *
 * Lifetime: name + seed_text are borrowed pointers into the embedded
 * pack or the meta header — they live for the program's lifetime.
 * seed_text is NOT NUL-terminated; use seed_len. */
typedef struct {
    int               kind;        /* mpk_action_kind_t value           */
    int16_t           data;        /* event_kind_t / scene idx / delta  */
    const char       *seed_text;   /* talk_seed only; may be NULL       */
    uint8_t           seed_len;
    const char       *name;        /* format=0 zone name; NULL on fmt 1 */
} scene_pack_action_t;

/* Resolve a tap (cell-local x,y) to an action. Tries the direct
 * hit-test first; on miss, falls back to scene_pack_zone_near with
 * `slop_px`. Returns true when a zone matched (direct or near).
 * The caller dispatches on out->kind. */
bool scene_pack_action_at(int16_t x, int16_t y, int slop_px,
                          scene_pack_action_t *out);

#ifdef __cplusplus
}
#endif
