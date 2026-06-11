/*
 * fetch_worker — the single background owner of main-loop-initiated
 * network fetches (design/35, closing design/25 C1/C2/C6).
 *
 * The main loop is the single owner of touch + panel; anything
 * multi-second it calls inline freezes ALL input for that long
 * (design/25 §C). This worker moves every network round-trip the loop
 * used to make inline onto one background task:
 *
 *   enter_place        POST /api/places/:id/enter (the nav_place tap)
 *   travel pack        cold place-pack GET into the LittleFS cache
 *   post-arrival       ETag confirm of the rendered place pack
 *   neighbour warm     design/29 prefetch ring
 *   pet cell           a pack/cache-miss expression cell
 *   keepsake mirror    best-effort POST /api/keepsake/collect
 *
 * Ownership rule (the reason this is safe): the worker NEVER touches
 * live render state. Packs are fetched into the LittleFS cache only
 * (pack_cache_prefetch_geom_ex); cells are stored into sprite_cache
 * only. The main loop reloads from cache on its own task when a result
 * or dirty flag tells it to, so the active mpk_t / framebuffers stay
 * main-owned exactly as before.
 *
 * Results: travel-affecting requests (enter / travel pack / refresh)
 * post a fetch_result_t onto a small queue the main loop drains each
 * tick. Warm / cell / keepsake requests are fire-and-forget (cells
 * raise the render-dirty flag instead).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FETCH_RESULT_ENTER = 0,    /* enter_place POST finished            */
    FETCH_RESULT_TRAVEL_PACK,  /* cold travel pack fetch finished      */
    FETCH_RESULT_REFRESH_PACK, /* post-arrival refresh found a CHANGE  */
} fetch_result_kind_t;

typedef struct {
    fetch_result_kind_t kind;
    bool ok;
    char place_id[40];   /* which travel this belongs to ("" for refresh) */
    char sheet[64];      /* pack sheet (empty for enter results)          */
} fetch_result_t;

/* One-time bring-up: queue + task. Idempotent. Call after WiFi/lwip
 * are up (the worker only runs network ops handed to it, so starting
 * earlier is harmless but pointless). */
bool fetch_worker_init(void);

/* nav_place tap: POST /enter off-loop. A result (FETCH_RESULT_ENTER)
 * is posted either way — on ok the main loop re-evaluates travel (the
 * POST already updated pet_sync's local location), on failure it
 * floats the travel-fail bubble. Returns false if the queue is full
 * (caller may fall back to its legacy inline path). */
bool fetch_worker_enter_place(const char *place_id);

/* Cold travel pack: warm (sheet, cw, ch) into LittleFS and post a
 * FETCH_RESULT_TRAVEL_PACK with ok/fail. place_id rides along so the
 * failure bubble can name the place. */
bool fetch_worker_fetch_travel_pack(const char *place_id, const char *sheet,
                                    uint16_t cw, uint16_t ch);

/* Post-arrival confirm: ETag-probe the rendered pack; GET+store only
 * on change. Posts FETCH_RESULT_REFRESH_PACK ONLY when fresh bytes
 * landed (unchanged / offline / failure are silent best-effort). */
bool fetch_worker_refresh_pack(const char *sheet, uint16_t cw, uint16_t ch);

/* design/29 neighbour warm. Fire-and-forget. */
bool fetch_worker_warm_pack(const char *sheet, uint16_t cw, uint16_t ch);

/* Pack/cache-miss pet cell (design/25 C2): fetch
 * /devsprite/cell/<sheet>/<expr> into sprite_cache and raise the
 * render-dirty flag so the next resting render picks it up. Dedupes
 * per (sheet, expr) with a cooldown so a missing cell can't be
 * re-enqueued every render. Expected cell dims ride along for
 * validation. */
bool fetch_worker_fetch_cell(const char *sheet, const char *expr,
                             uint16_t cw, uint16_t ch);

/* Best-effort keepsake mirror POST (design/33). Fire-and-forget; the
 * NVS set is the source of truth either way. */
bool fetch_worker_collect_keepsake(const char *keepsake_id);

/* Drain one queued result. Returns false when empty. Main-loop only. */
bool fetch_worker_take_result(fetch_result_t *out);

/* True once after one or more cell fetches landed in sprite_cache —
 * the main loop re-renders the resting pet so the new artwork shows.
 * Clears on read. */
bool fetch_worker_take_cell_dirty(void);

#ifdef __cplusplus
}
#endif
