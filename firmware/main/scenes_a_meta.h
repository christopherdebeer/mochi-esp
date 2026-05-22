/*
 * scenes_a — metadata for the embedded MPK1 scene pack.
 *
 * Hand-authored from the SPRITE·FORGE export (assets/scenes_a.mpk),
 * with cleaner identifiers than the auto-generated header. 16 scenes
 * at 200×200 1bpp 2-plane; 4 of them carry named tap zones today.
 *
 * The content of SCENES_A_ZONES_* came from the SPRITE·FORGE
 * `_meta.h`; the only edit is renaming the prefix. Re-author this
 * file when the pack changes — the binary is the source of truth
 * for pixels, this header is the source of truth for zone semantics.
 *
 * Pack fingerprint:
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
