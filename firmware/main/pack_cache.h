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

/* Cache-only load for the boot path, before WiFi/lwip is up. The
 * normal pack_cache_active* helpers always start with an ETag HEAD
 * probe to /devsprite/pack/<sheet>; calling them pre-WiFi panics
 * inside lwip_getaddrinfo (tcpip task not running yet). This sibling
 * skips the network completely and only reads from LittleFS, so
 * boot-time scene restore works without inviting the assert.
 *
 * Returns the cached blob (caller-owned PSRAM, valid for life of the
 * device) on hit, NULL on miss. Use pack_cache_active_geom from the
 * main loop once WiFi is up to ETag-refresh against the server. */
const uint8_t *pack_cache_load_geom_only(const char *sheet,
                                         uint16_t cw, uint16_t ch);

/* Cache-only load for the boot path of a NON-geometry pack (the home
 * bundle "scene-bundle-a", whose cache key is plain "<sheet>.pack" — no
 * cw/ch suffix). Same pre-WiFi rationale as pack_cache_load_geom_only:
 * reads LittleFS only, never touches the network, so the cold-boot home
 * render can prefer the last server-synced bundle over the embedded
 * baseline without inviting the lwip getaddrinfo assert. Returns the
 * cached blob (caller-owned PSRAM, valid for life) on hit, NULL on miss.
 * The post-WiFi scene_pack_init() still ETag-refreshes against the
 * server. design/29. */
const uint8_t *pack_cache_load_only(const char *sheet);

/* Warm the LittleFS cache for a travel pack WITHOUT retaining it in
 * PSRAM — used to eagerly prefetch places reachable from the loaded
 * scene so onward travel hits the warm path (design/29). HEAD-probes
 * the server ETag; GETs + stores the body only when it changed or the
 * cache is empty; then frees the working buffer (unlike
 * pack_cache_active_geom, which keeps it as the live pack). When the
 * ETag already matches it returns immediately (cheap HEAD, no alloc,
 * no GET). Returns true when the cache is warm afterwards (ETag hit or
 * a fresh store), false on offline / fetch / store failure. Strictly
 * best-effort: a false here just means the eventual
 * pack_cache_active_geom does the cold fetch as before. */
bool pack_cache_prefetch_geom(const char *sheet, uint16_t cw, uint16_t ch);

/*
 * Travel refresh (design/29): validate the cached place pack against the
 * server WITHOUT blocking a cache-first render. Returns freshly-fetched +
 * persisted bytes ONLY when the server pack changed since we cached it (or
 * there was no cache yet). Returns NULL — with no PSRAM allocation — when
 * the link is down, the ETag is unchanged, or the probe/fetch fails, in
 * which case the caller keeps showing whatever it already rendered from
 * cache. Returned bytes are owned by pack_cache (never freed), like the
 * other resolvers. Geometry-keyed identically to pack_cache_active_geom.
 */
const uint8_t *pack_cache_refresh_geom(const char *sheet,
                                       uint16_t cw, uint16_t ch);

#ifdef __cplusplus
}
#endif
