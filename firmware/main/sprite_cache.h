/*
 * Persistent sprite cache backed by LittleFS on the 'storage'
 * partition (6 MB at 0x210000).
 *
 * Each cached blob is keyed by (sheet_id, suffix). The suffix is
 * a short string identifying the variant — e.g. "neutral.cell"
 * for the 2-plane native cell of pet-v1/neutral, or
 * "fill_200x172" for a fit=fill scene render. ETag invalidation
 * is per-sheet: one stored ETag tag per sheet, refreshed by an
 * HTTP HEAD on boot.
 *
 * The cache is best-effort. Lookups that miss return false; the
 * caller should fall through to a network fetch and then call
 * sprite_cache::store() with the result.
 *
 * Files on disk:
 *   /sprites/<sheet>/<suffix>     — raw bytes
 *   /etags/<sheet>.tag            — current sheet ETag
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace sprite_cache {

/* Mount LittleFS on the 'storage' partition. Idempotent.
 * Returns true on success. If the partition has no FS yet the
 * mount call formats it on the first attempt. */
bool init(void);

/* Look up a blob by (sheet, suffix). Returns true and fills
 * *out_size with the actual byte count if the file exists; reads
 * up to max_bytes into out_buf. Returns false if absent or any
 * I/O error. */
bool load(const char *sheet, const char *suffix,
          uint8_t *out_buf, size_t max_bytes, size_t *out_size);

/* Persist a blob. Overwrites if already present. Returns true
 * on success. */
bool store(const char *sheet, const char *suffix,
           const uint8_t *bytes, size_t size);

/* Read/write the per-sheet ETag tag. ETags are short ASCII
 * strings (e.g. quoted "7c11b551ffaeb2dd") — buffer ≥ 32 bytes
 * is plenty. load_etag returns true and a NUL-terminated string
 * on success, false if absent. */
bool load_etag(const char *sheet, char *out, size_t out_cap);
bool store_etag(const char *sheet, const char *etag);

/* Drop everything cached for a given sheet (all suffixes + the
 * ETag). Used after detecting a sheet's source has changed. */
void invalidate_sheet(const char *sheet);

}  /* namespace sprite_cache */
