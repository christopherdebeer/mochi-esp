/*
 * scenes_a — metadata for the embedded MPK1 scene pack.
 *
 * 2026-06-06: assets/scenes_a.mpk was re-baked from the substrate
 * `scene-bundle-a` plan (GET /devsprite/pack/scene-bundle-a) — now a
 * **format=1** pack carrying its zones + nav INLINE (the redesigned
 * spine + nav_place portals; see design/32). scene_pack.c gates on
 * `s_pack.format == 1` and hit-tests the inline zones directly, so the
 * SCENES_A_ZONES table below NO LONGER APPLIES to the embedded bundle.
 *
 * The table is retained only as a legacy/factory fallback (a format=0
 * pack would still bind to it) and because scene_pack.c references the
 * SCENES_A_ZONES / SCENES_A_ZONES_COUNT symbols. SCENES_A_COUNT must
 * still match the .mpk cell count (16) — it gates the sync warning in
 * open_into_active. The stale per-sprite zone rects below describe the
 * OLD SPRITE·FORGE export, not the current art; do not trust them for
 * the format=1 bundle.
 *
 * Re-bake recipe when the home plan changes:
 *   curl -s https://mochi.val.run/devsprite/pack/scene-bundle-a \
 *     -o firmware/main/assets/scenes_a.mpk   # then bump version.txt
 *
 * Legacy fingerprint (pre-2026-06-06, SPRITE·FORGE format=0 export):
 *   16 sprites · 200×200 mochi 2-plane (MPK1) · 21 tap zones
 *   sprite 0: 7 zones (food / ball / door / window / heart / shelf / ornament)
 *   sprite 1: 5 zones
 *   sprite 2: 5 zones
 *   sprite 3: 4 zones
 *   sprites 4..15 carry no zones (pure scenery)
 */

#pragma once

#include "mochi_pack.h"

#define SCENES_A_COUNT 16u

/* Per-sprite zone arrays. Coordinates are cell-local (200×200). */
static const mpk_zone_t SCENES_A_ZONES_00[7] = {
    {   6, 139,  52,  43, "food" },
    { 115, 142,  45,  43, "ball" },
    { 163,  53,  30, 109, "door" },
    {  47,  16,  90,  72, "window" },
    {   8,  25,  23,  24, "heart" },
    {   3,  61,  41,  61, "shelf" },
    { 153,  15,  31,  30, "ornament" },
};
static const mpk_zone_t SCENES_A_ZONES_01[5] = {
    {  17, 143,  42,  32, "food" },
    { 135, 149,  35,  33, "ball" },
    { 169,  68,  29,  97, "door" },
    {  52,   9, 101,  88, "window" },
    {   5,  70,  40,  67, "shelf" },
};
static const mpk_zone_t SCENES_A_ZONES_02[5] = {
    { 161,  59,  33, 122, "door" },
    {  97, 112,  63,  49, "plants" },
    {  27,  14,  98,  93, "window" },
    { 148,  11,  33,  40, "light" },
    {   2, 114,  46,  55, "box" },
};
static const mpk_zone_t SCENES_A_ZONES_03[4] = {
    { 144,  99,  49,  54, "stairs" },
    {   9, 124,  57,  54, "plant" },
    {  17,  14, 165,  81, "view" },
    { 155, 156,  41,  33, "container" },
};

static const mpk_zone_set_t SCENES_A_ZONES[4] = {
    { 0, 7, SCENES_A_ZONES_00 },
    { 1, 5, SCENES_A_ZONES_01 },
    { 2, 5, SCENES_A_ZONES_02 },
    { 3, 4, SCENES_A_ZONES_03 },
};
#define SCENES_A_ZONES_COUNT 4u
