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

#ifdef __cplusplus
}
#endif
