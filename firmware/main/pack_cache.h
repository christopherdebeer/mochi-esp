/*
 * pack_cache — boot-time MPK1 pack refresh.
 *
 * The firmware embeds baseline packs at build time (pet_a.mpk,
 * scenes_a.mpk via CMake EMBED_FILES). Those are the fail-safe: a
 * device with no network, or a freshly flashed one, always has a
 * complete pack in flash.
 *
 * pack_cache_active() layers a "sync at boot" refresh on top. Given a
 * sheet id and the embedded baseline, it:
 *   1. HEAD-probes  https://mochi.val.run/devsprite/pack/<sheet>
 *      for the server ETag.
 *   2. If the ETag differs from the locally cached one (or no cache
 *      exists), GETs the pack into a persistent PSRAM buffer, validates
 *      the MPK1 envelope, and writes it + the ETag to LittleFS.
 *   3. If the ETag matches, loads the cached pack from LittleFS into
 *      PSRAM (no network body fetch).
 *   4. On any failure (offline, bad response, FS error) falls back to
 *      the most recent good source: cached pack if present, else the
 *      embedded baseline.
 *
 * The returned pointer is owned by pack_cache (a never-freed PSRAM
 * allocation or the embedded .rodata blob) and is valid for the rest
 * of the device's life — exactly what mpk_open() requires.
 *
 * The studio publishes sheet edits to substrate; the server pack
 * endpoint re-derives and re-ETags from substrate, so an authored
 * change shows up on the device at its next boot with no reflash.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Resolve the active pack bytes for `sheet`, preferring the server
 * over the cache over the embedded baseline. `embedded` must point at
 * the build-time blob and is returned unchanged as the fail-safe.
 * Never returns NULL when `embedded` is non-NULL.
 */
const uint8_t *pack_cache_active(const char *sheet, const uint8_t *embedded);

/*
 * Same as pack_cache_active but for travel-sized place packs that the
 * server resolves with a per-cell geometry query
 * (/devsprite/pack/<sheet>?cw=<cw>&ch=<ch>). The cache key includes
 * the geometry so a 96×96 pet pack and a 200×200 place pack at the
 * same sheet id can coexist (they don't share, but the device-side
 * key prevents accidental collision).
 *
 * `embedded` is optional: pass NULL if the pack has no build-time
 * baseline (most place packs). On total failure (no cache, no
 * embedded, no network) returns NULL — caller should keep the
 * previous scene rather than re-render.
 */
const uint8_t *pack_cache_active_geom(const char *sheet,
                                      uint16_t cw, uint16_t ch,
                                      const uint8_t *embedded);

#ifdef __cplusplus
}
#endif
