/*
 * mochi_pack — build-time sprite pack reader.
 *
 * A "MPK1" pack bundles many device cells into one blob that the
 * firmware embeds at build time (via CMake EMBED_FILES, the same
 * mechanism splash.bin uses) and indexes by label at runtime. It is
 * the offline / zero-fetch sibling of the /devsprite/cell endpoint.
 *
 * The crucial property: each entry's cell blob is **byte-identical**
 * to a /devsprite/cell HTTP response (8-byte header + ink plane
 * [+ mask plane], same MSB-first packing, same bit-0-is-ink
 * convention — see design/05-sprite-format.md). So a bundled cell
 * and a fetched cell flow through the exact same
 * compositor::blit_two_plane path; nothing downstream needs to know
 * where the bytes came from.
 *
 * Authored by the SPRITE·FORGE web tool (target: mochi-esp), which
 * also emits a companion `<pack>_meta.h` of label indices + tap
 * zones. See design/13-build-time-asset-packs.md for the format
 * rationale and how bundled packs interact with scene contracts.
 *
 * Header-only, no allocations, no I/O. The pack bytes must outlive
 * every mpk_* pointer (embedded blobs and PSRAM are fine; a stack
 * buffer is not).
 *
 * Pack wire format (envelope is little-endian — native to the
 * ESP32-S3 and to the embedded blob; the per-cell header inside each
 * entry stays big-endian to match the fetch format verbatim):
 *
 *   header (16 bytes):
 *     0..3   magic     "MPK1"
 *     4      version   (1)
 *     5      format    (0 = device 2-plane cell entries)
 *     6..7   cell_w    u16 LE
 *     8..9   cell_h    u16 LE
 *     10..11 count     u16 LE
 *     12     label_len u8
 *     13     flags     (bit 0 = entries carry a mask plane)
 *     14..15 reserved  (0)
 *   entries (count), each `stride` bytes:
 *     label      : label_len bytes, NUL-padded UTF-8
 *     cell blob  : 8-byte cell header (w u16 BE, h u16 BE, flags u8,
 *                  3 reserved) + ink plane [+ mask plane]
 *
 *   plane_bytes = ((cell_w + 7) / 8) * cell_h
 *   cell_bytes  = 8 + (has_mask ? 2 : 1) * plane_bytes
 *   stride      = label_len + cell_bytes
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t       cell_w;
    uint16_t       cell_h;
    uint16_t       count;
    uint8_t        label_len;
    uint8_t        has_mask;     /* flags bit 0 */
    uint32_t       plane_bytes;  /* ((cell_w+7)/8) * cell_h */
    uint32_t       cell_bytes;   /* 8 + (has_mask?2:1) * plane_bytes */
    uint32_t       stride;       /* label_len + cell_bytes */
    const uint8_t *base;         /* first entry */
} mpk_t;

/* Validate + parse the 16-byte header. Returns 0 on success, a
 * negative code on a bad magic (-1) or unsupported version (-2).
 * `data` must remain valid for the lifetime of every pointer the
 * other helpers return. */
static inline int mpk_open(const uint8_t *data, mpk_t *p) {
    if (memcmp(data, "MPK1", 4) != 0) return -1;
    if (data[4] != 1)                 return -2;
    p->cell_w    = (uint16_t)data[6]  | ((uint16_t)data[7]  << 8);
    p->cell_h    = (uint16_t)data[8]  | ((uint16_t)data[9]  << 8);
    p->count     = (uint16_t)data[10] | ((uint16_t)data[11] << 8);
    p->label_len = data[12];
    p->has_mask  = (uint8_t)(data[13] & 0x01);
    p->plane_bytes = (uint32_t)((p->cell_w + 7) / 8) * p->cell_h;
    p->cell_bytes  = 8u + (p->has_mask ? 2u : 1u) * p->plane_bytes;
    p->stride      = (uint32_t)p->label_len + p->cell_bytes;
    p->base        = data + 16;
    return 0;
}

/* Label for entry `i`. NUL-padded; copy at most label_len bytes if
 * you need a C string and the label fills the field exactly. */
static inline const char *mpk_label(const mpk_t *p, uint16_t i) {
    return (const char *)(p->base + (size_t)i * p->stride);
}

/* Pointer to entry `i`'s cell blob: 8-byte header + planes, the same
 * layout sprite_fetch_cell() parses from the wire. Hand this to any
 * code that already understands a fetched cell. */
static inline const uint8_t *mpk_cell(const mpk_t *p, uint16_t i) {
    return p->base + (size_t)i * p->stride + p->label_len;
}

/* Ink plane (line work): bit 0 = black, bit 1 = paper-white. */
static inline const uint8_t *mpk_ink(const mpk_t *p, uint16_t i) {
    return mpk_cell(p, i) + 8;
}

/* Mask plane: bit 0 = opaque (write ink), bit 1 = transparent (leave
 * the scene). NULL when the pack carries no mask plane. */
static inline const uint8_t *mpk_mask(const mpk_t *p, uint16_t i) {
    return p->has_mask ? mpk_cell(p, i) + 8 + p->plane_bytes : NULL;
}

/* Linear search by label (packs are small — tens of entries). Returns
 * the entry index, or -1 if not found. */
static inline int mpk_find(const mpk_t *p, const char *name) {
    for (uint16_t i = 0; i < p->count; i++) {
        if (strncmp(mpk_label(p, i), name, p->label_len) == 0) return (int)i;
    }
    return -1;
}

/* ── Tap zones (build-time scene contract) ──────────────────────────
 *
 * A pack's companion `<pack>_meta.h` emits, per sprite that has them,
 * an array of named rectangles in cell-local pixel space, plus a set
 * table keyed by sprite index. This is the device-authored analogue
 * of a server scene contract's semantic regions — same {x,y,w,h,name}
 * shape, same hit-test — so input routing can treat bundled and
 * server-supplied zones uniformly. See design/13. */
typedef struct {
    uint16_t    x, y, w, h;
    const char *name;
} mpk_zone_t;

typedef struct {
    uint16_t          sprite_idx;
    uint8_t           count;
    const mpk_zone_t *items;
} mpk_zone_set_t;

/* Point-in-rect test against the zone sets for `sprite_idx`. Returns
 * the hit index within that sprite's set, or -1 for no hit. On a hit,
 * sets *out_name (when non-NULL) to the matched zone's name.
 * Coordinates are cell-local; translate the touch point into cell
 * space before calling if the cell is composited at an offset. */
static inline int mpk_zone_test(const mpk_zone_set_t *sets, size_t n_sets,
                                uint16_t sprite_idx, int16_t x, int16_t y,
                                const char **out_name) {
    for (size_t s = 0; s < n_sets; s++) {
        if (sets[s].sprite_idx != sprite_idx) continue;
        for (uint8_t z = 0; z < sets[s].count; z++) {
            const mpk_zone_t *r = &sets[s].items[z];
            if (x >= (int16_t)r->x && x < (int16_t)(r->x + r->w) &&
                y >= (int16_t)r->y && y < (int16_t)(r->y + r->h)) {
                if (out_name) *out_name = r->name;
                return (int)z;
            }
        }
        return -1;
    }
    return -1;
}

/* ── Wiring sketch ──────────────────────────────────────────────────
 *
 *   // CMake (firmware/main/CMakeLists.txt):
 *   //   idf_component_register(... EMBED_FILES "assets/pets.mpk.bin" ...)
 *
 *   extern const uint8_t _binary_pets_mpk_bin_start[] asm("_binary_pets_mpk_bin_start");
 *
 *   mpk_t pack;
 *   if (mpk_open(_binary_pets_mpk_bin_start, &pack) == 0) {
 *       int i = mpk_find(&pack, "sleeping");
 *       if (i >= 0) {
 *           compositor::blit_two_plane(
 *               composite, MOCHI_EPD_WIDTH, MOCHI_EPD_HEIGHT,
 *               mpk_ink(&pack, i), mpk_mask(&pack, i),
 *               pack.cell_w, pack.cell_h, PET_DX, PET_DY);
 *       }
 *   }
 *
 *   // Touch dispatch against the bundled contract (pets_meta.h):
 *   //   #include "pets_meta.h"   // PETS_ZONES[], PETS_ZONES_COUNT
 *   const char *zone = NULL;
 *   if (mpk_zone_test(PETS_ZONES, PETS_ZONES_COUNT, i,
 *                     tx - PET_DX, ty - PET_DY, &zone) >= 0) {
 *       handle_zone(zone);
 *   }
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif
