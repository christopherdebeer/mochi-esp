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
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t       cell_w;
    uint16_t       cell_h;
    uint16_t       count;
    uint8_t        label_len;
    uint8_t        has_mask;      /* flags bit 0 */
    uint8_t        format;        /* 0 = cells only; 1 = cells + inline zones */
    uint32_t       plane_bytes;   /* ((cell_w+7)/8) * cell_h */
    uint32_t       cell_bytes;    /* 8 + (has_mask?2:1) * plane_bytes */
    uint32_t       stride;        /* fmt 0: label_len + cell_bytes (0 in fmt 1) */
    const uint8_t *data;          /* pack start */
    const uint8_t *base;          /* first entry (fmt 0: data+16) */
    const uint8_t *directory;     /* fmt 1: count x u32 LE entry offsets (else NULL) */
    const uint8_t *label_table;   /* fmt 1: talk_seed string table (else NULL) */
} mpk_t;

/* Little-endian readers (envelope + directory + zone fields are LE;
 * read byte-wise so unaligned access on the embedded blob is safe). */
static inline uint16_t mpk__u16(const uint8_t *d) {
    return (uint16_t)d[0] | ((uint16_t)d[1] << 8);
}
static inline uint32_t mpk__u32(const uint8_t *d) {
    return (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
           ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

/* Validate + parse the 16-byte header. Returns 0 on success; -1 bad
 * magic, -2 unsupported version, -3 unsupported format. `data` must
 * remain valid for the lifetime of every pointer the helpers return. */
static inline int mpk_open(const uint8_t *data, mpk_t *p) {
    if (memcmp(data, "MPK1", 4) != 0) return -1;
    if (data[4] != 1)                 return -2;
    const uint8_t fmt = data[5];
    if (fmt != 0 && fmt != 1)         return -3;   /* format guard */
    p->data      = data;
    p->format    = fmt;
    p->cell_w    = mpk__u16(data + 6);
    p->cell_h    = mpk__u16(data + 8);
    p->count     = mpk__u16(data + 10);
    p->label_len = data[12];
    p->has_mask  = (uint8_t)(data[13] & 0x01);
    p->plane_bytes = (uint32_t)((p->cell_w + 7) / 8) * p->cell_h;
    p->cell_bytes  = 8u + (p->has_mask ? 2u : 1u) * p->plane_bytes;
    if (fmt == 0) {
        p->stride      = (uint32_t)p->label_len + p->cell_bytes;
        p->base        = data + 16;
        p->directory   = NULL;
        p->label_table = NULL;
    } else {
        /* format 1: a u32 LE entry-offset directory replaces the fixed
         * stride; the talk_seed label table sits just past the last
         * entry (computed from the last entry's zone count). */
        p->stride      = 0;
        p->directory   = data + 16;
        p->base        = data + 16 + (size_t)p->count * 4;
        if (p->count > 0) {
            const uint8_t *last = data + mpk__u32(p->directory + (size_t)(p->count - 1) * 4);
            const uint8_t  zc   = last[p->label_len + p->cell_bytes];
            p->label_table = last + p->label_len + p->cell_bytes + 1 + (size_t)zc * 24;
        } else {
            p->label_table = data + 16;
        }
    }
    return 0;
}

/* Start of entry `i` (its label field), format-aware. */
static inline const uint8_t *mpk__entry(const mpk_t *p, uint16_t i) {
    return (p->format == 1)
        ? p->data + mpk__u32(p->directory + (size_t)i * 4)
        : p->base + (size_t)i * p->stride;
}

/* Label for entry `i`. NUL-padded; copy at most label_len bytes if
 * you need a C string and the label fills the field exactly. */
static inline const char *mpk_label(const mpk_t *p, uint16_t i) {
    return (const char *)mpk__entry(p, i);
}

/* Pointer to entry `i`'s cell blob: 8-byte header + planes, the same
 * layout sprite_fetch_cell() parses from the wire. Hand this to any
 * code that already understands a fetched cell. */
static inline const uint8_t *mpk_cell(const mpk_t *p, uint16_t i) {
    return mpk__entry(p, i) + p->label_len;
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

/* ── format=1 zones: typed, inline actions (design/14) ──────────────
 *
 * format=1 packs carry per-entry zones with an action payload plus a
 * pack-global talk_seed string table. These accessors return 0 / false
 * on format=0 packs (whose zones, if any, live in a companion
 * `<pack>_meta.h` read via mpk_zone_test below). */
typedef enum {
    MPK_ACTION_NONE         = 0,
    MPK_ACTION_EVENT        = 1,  /* data = event_kind_t value           */
    MPK_ACTION_NAV_SCENE    = 2,  /* data = absolute scene index         */
    MPK_ACTION_NAV_RELATIVE = 3,  /* data = signed scene delta (i8)      */
    MPK_ACTION_TALK_SEED    = 4,  /* seed_text/seed_len set; data unused */
    MPK_ACTION_NAV_PLACE    = 5,  /* seed_text/seed_len = target place id (design/17) */
    /* design/20 (splash): location-only chrome zones — the payload (text
     * string / pet sprite) is device-authored, not carried in the pack.
     * Both are non-interactive (skipped by tap routing). */
    MPK_ACTION_TEXT         = 6,  /* data = type|colour bitfield: bits0-3 type (0 title, 1 status), bit4 light-glyphs */
    MPK_ACTION_PET          = 7,  /* data = expression index (see studio PET_EXPRESSIONS; 13 = lonely); rect sizes + places the pet */
} mpk_action_kind_t;

/* MPK_ACTION_TEXT data accessors. */
#define MPK_TEXT_TYPE(data)   ((uint8_t)((data) & 0x0F))   /* 0 = title, 1 = status */
#define MPK_TEXT_LIGHT(data)  (((data) & 0x10) != 0)       /* true = light glyphs (dark bg) */
#define MPK_TEXT_TITLE   0
#define MPK_TEXT_STATUS  1

typedef struct {
    uint16_t          x, y, w, h;
    mpk_action_kind_t kind;
    int16_t           data;       /* kind-dependent; nav_relative sign-extended */
    const char       *seed_text;  /* into the label table; NOT NUL-terminated   */
    uint8_t           seed_len;   /* length of seed_text (0 when none)          */
} mpk_zone_v1_t;

/* Number of inline zones on entry `i` (0 unless format==1). */
static inline uint8_t mpk_zone_count(const mpk_t *p, uint16_t i) {
    if (p->format != 1) return 0;
    return mpk__entry(p, i)[p->label_len + p->cell_bytes];
}

/* Resolve talk_seed label `idx`. Returns a pointer into the label
 * table (NOT NUL-terminated; use *out_len) or NULL. */
static inline const char *mpk__seed(const mpk_t *p, uint16_t idx, uint8_t *out_len) {
    if (p->format != 1 || idx == 0xFFFF) { if (out_len) *out_len = 0; return NULL; }
    const uint8_t *t = p->label_table;
    const uint16_t n = mpk__u16(t);
    t += 2;
    for (uint16_t k = 0; k < n; k++) {
        const uint8_t len = *t++;
        if (k == idx) { if (out_len) *out_len = len; return (const char *)t; }
        t += len;
    }
    if (out_len) *out_len = 0;
    return NULL;
}

/* Fill *out with zone `z` of entry `i`. Returns false if format!=1 or
 * the zone index is out of range. */
static inline bool mpk_zone_get(const mpk_t *p, uint16_t i, uint8_t z,
                                mpk_zone_v1_t *out) {
    if (p->format != 1 || z >= mpk_zone_count(p, i)) return false;
    const uint8_t *zp = mpk__entry(p, i) + p->label_len + p->cell_bytes + 1 + (size_t)z * 24;
    out->x = mpk__u16(zp + 0);
    out->y = mpk__u16(zp + 2);
    out->w = mpk__u16(zp + 4);
    out->h = mpk__u16(zp + 6);
    const uint8_t  kind      = zp[8];
    const uint8_t  data      = zp[9];
    const uint16_t label_idx = mpk__u16(zp + 10);
    out->kind = (mpk_action_kind_t)kind;
    out->data = (kind == MPK_ACTION_NAV_RELATIVE) ? (int16_t)(int8_t)data : (int16_t)data;
    /* talk_seed and nav_place both carry a variable-length string in the
     * pack-global label table: the seed text and the target place id
     * respectively. Resolve it for both. */
    if (kind == MPK_ACTION_TALK_SEED || kind == MPK_ACTION_NAV_PLACE) {
        out->seed_text = mpk__seed(p, label_idx, &out->seed_len);
    } else {
        out->seed_text = NULL;
        out->seed_len  = 0;
    }
    return true;
}

/* Point-in-rect hit-test over entry `i`'s inline zones (format=1).
 * Returns the zone index and fills *out (when non-NULL), or -1. */
static inline int mpk_zone_hit(const mpk_t *p, uint16_t i, int16_t x, int16_t y,
                               mpk_zone_v1_t *out) {
    const uint8_t zc = mpk_zone_count(p, i);
    for (uint8_t z = 0; z < zc; z++) {
        mpk_zone_v1_t zone;
        mpk_zone_get(p, i, z, &zone);
        if (x >= (int16_t)zone.x && x < (int16_t)(zone.x + zone.w) &&
            y >= (int16_t)zone.y && y < (int16_t)(zone.y + zone.h)) {
            if (out) *out = zone;
            return (int)z;
        }
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
