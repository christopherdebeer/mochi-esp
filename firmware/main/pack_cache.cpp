#include "pack_cache.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "sprite_fetch.h"
#include "sprite_cache.h"
#include "device_diag.h"

static const char *TAG = "pack_cache";

/* Upper bound for a single pack. scene-bundle-a (16×200×200 cells +
 * zones) is ~161 KB today; 320 KB leaves generous room for growth
 * while staying invisible against the 8 MB PSRAM budget. A pack that
 * exceeds this is rejected (we keep the embedded baseline) rather
 * than silently truncated. */
#define PACK_MAX_BYTES (320u * 1024u)

/* Blob suffix under sprite_cache. The cache "sheet" we use is
 * "<sheet>.pack" so the pack ETag never collides with the per-cell
 * ETag block in main.cpp (which keys plain "pet-v1" etc.). */
#define PACK_SUFFIX "mpk"

/* Cheap structural check: enough bytes for the envelope, right magic,
 * known version, known format. Guards against HTML error pages and
 * truncated bodies before we hand the buffer to mpk_open. */
static bool looks_like_mpk1(const uint8_t *p, size_t n) {
    if (!p || n < 16) return false;
    if (memcmp(p, "MPK1", 4) != 0) return false;
    if (p[4] != 1) return false;
    if (p[5] != 0 && p[5] != 1) return false;
    return true;
}

/* Allocate a PSRAM buffer and fill it from the cached blob. Returns
 * the buffer (caller keeps it; shrunk to the stored size) or NULL on
 * miss / validation failure. */
static const uint8_t *load_cached(const char *cache_sheet) {
    uint8_t *buf = (uint8_t *)heap_caps_malloc(PACK_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "PSRAM alloc (%u) failed for cache load",
            (unsigned)PACK_MAX_BYTES);
        return NULL;
    }
    size_t got = 0;
    if (!sprite_cache::load(cache_sheet, PACK_SUFFIX, buf, PACK_MAX_BYTES, &got) ||
        !looks_like_mpk1(buf, got)) {
        heap_caps_free(buf);
        return NULL;
    }
    /* Shrink to fit; on realloc failure the original (oversized but
     * valid) buffer is still good, so ignore a NULL return. */
    uint8_t *shrunk = (uint8_t *)heap_caps_realloc(buf, got, MALLOC_CAP_SPIRAM);
    if (shrunk) buf = shrunk;
    ESP_LOGI(TAG, "'%s' pack from cache (%u bytes)",
        cache_sheet, (unsigned)got);
    return buf;
}

/* Core resolver shared by pack_cache_active (no geometry suffix) and
 * pack_cache_active_geom (cw/ch query string + geometry-tagged cache
 * key). cache_sheet is the sprite_cache key (must encode any
 * disambiguators), url is the server URL to GET; both are caller-
 * supplied so this function stays geometry-agnostic. embedded is the
 * baseline blob to fall back to when offline + no cache; pass NULL
 * when the pack has no build-time baseline (most travel packs). */
static const uint8_t *resolve_active(const char *sheet,
                                     const char *cache_sheet,
                                     const char *url,
                                     const uint8_t *embedded) {

    /* Probe the server ETag. Offline → fall back to the last cached
     * pack, then to the embedded baseline. */
    char remote[40] = {};
    if (!sprite_fetch_head_etag(url, remote, sizeof(remote))) {
        ESP_LOGW(TAG, "'%s' pack ETag probe failed (offline?)", sheet);
        const uint8_t *cached = load_cached(cache_sheet);
        device_diag_eventf(DIAG_WARN, "pack_cache",
            cached ? "{\"src\":\"cache\",\"why\":\"offline\"}"
                   : "{\"src\":\"embedded\",\"why\":\"offline\"}",
            "%s", sheet);
        if (cached) return cached;
        ESP_LOGW(TAG, "'%s' no cache → embedded baseline", sheet);
        return embedded;
    }

    /* ETag unchanged → serve the cached body without a network fetch. */
    char local[40] = {};
    sprite_cache::load_etag(cache_sheet, local, sizeof(local));
    if (strcmp(remote, local) == 0) {
        const uint8_t *cached = load_cached(cache_sheet);
        if (cached) {
            ESP_LOGI(TAG, "'%s' pack ETag unchanged (%s)", sheet, remote);
            device_diag_eventf(DIAG_INFO, "pack_cache",
                "{\"src\":\"cache\"}", "%s", sheet);
            return cached;
        }
        ESP_LOGW(TAG, "'%s' ETag matched but cache load failed — refetching",
            sheet);
    }

    /* Changed (or cache miss) → GET the new pack into PSRAM. */
    uint8_t *buf = (uint8_t *)heap_caps_malloc(PACK_MAX_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "PSRAM alloc (%u) failed for fetch",
            (unsigned)PACK_MAX_BYTES);
        const uint8_t *cached = load_cached(cache_sheet);
        return cached ? cached : embedded;
    }

    size_t got = 0;
    uint32_t ms = 0;
    bool ok = sprite_fetch_blob(url, buf, PACK_MAX_BYTES, &got, &ms);
    if (!ok || !looks_like_mpk1(buf, got)) {
        ESP_LOGW(TAG, "'%s' pack fetch invalid (ok=%d, %u bytes)",
            sheet, (int)ok, (unsigned)got);
        heap_caps_free(buf);
        const uint8_t *cached = load_cached(cache_sheet);
        device_diag_eventf(DIAG_WARN, "pack_cache",
            cached ? "{\"src\":\"cache\",\"why\":\"fetch_invalid\"}"
                   : "{\"src\":\"embedded\",\"why\":\"fetch_invalid\"}",
            "%s", sheet);
        return cached ? cached : embedded;
    }

    /* Persist for next boot. Cache write is best-effort: a failure
     * here just means we'll refetch next time, not that this boot
     * fails. */
    if (sprite_cache::store(cache_sheet, PACK_SUFFIX, buf, got)) {
        sprite_cache::store_etag(cache_sheet, remote);
    } else {
        ESP_LOGW(TAG, "'%s' pack cache store failed (using fetched bytes)",
            sheet);
    }

    uint8_t *shrunk = (uint8_t *)heap_caps_realloc(buf, got, MALLOC_CAP_SPIRAM);
    if (shrunk) buf = shrunk;
    ESP_LOGI(TAG, "'%s' pack fetched %u bytes in %u ms (ETag %s)",
        sheet, (unsigned)got, (unsigned)ms, remote);
    device_diag_eventf(DIAG_INFO, "pack_cache",
        "{\"src\":\"server\"}", "%s fetched", sheet);
    return buf;
}

const uint8_t *pack_cache_active(const char *sheet, const uint8_t *embedded) {
    if (!sheet) return embedded;
    char cache_sheet[48];
    snprintf(cache_sheet, sizeof(cache_sheet), "%s.pack", sheet);
    char url[96];
    snprintf(url, sizeof(url),
        "https://mochi.val.run/devsprite/pack/%s", sheet);
    return resolve_active(sheet, cache_sheet, url, embedded);
}

const uint8_t *pack_cache_active_geom(const char *sheet,
                                      uint16_t cw, uint16_t ch,
                                      const uint8_t *embedded) {
    if (!sheet) return embedded;
    /* Cache key encodes geometry so packs at different cell sizes
     * don't collide. Server URL appends ?cw=&ch= so the resolver
     * returns the right pixel count. */
    char cache_sheet[64];
    snprintf(cache_sheet, sizeof(cache_sheet), "%s.%ux%u.pack",
        sheet, (unsigned)cw, (unsigned)ch);
    char url[160];
    snprintf(url, sizeof(url),
        "https://mochi.val.run/devsprite/pack/%s?cw=%u&ch=%u",
        sheet, (unsigned)cw, (unsigned)ch);
    return resolve_active(sheet, cache_sheet, url, embedded);
}
