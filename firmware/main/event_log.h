/*
 * event_log — append-only ring buffer of pet events on LittleFS.
 *
 * Why this exists: engagement.c and (eventually) M13's sync layer
 * both need access to recent events. NVS is wrong for the volume
 * (1024 entries × tens of bytes), and a flat append-only file would
 * grow without bound until M13 ships the event-push half of sync.
 *
 * Shape:
 *   - Fixed-size records (16 B each: kind + at) so the recent slice
 *     can be found by stat + read-back from a known offset.
 *   - Ring buffer of EVENT_LOG_CAPACITY entries. Oldest entries are
 *     overwritten in place; M13 will need to push BEFORE the buffer
 *     wraps if the device is offline for >1024 events.
 *   - Header block at offset 0 carries (magic, version, head idx,
 *     count) — written atomically with each append via fwrite +
 *     fflush + fsync.
 *
 * Threading: append() is called from the touch loop and from voice
 * callbacks. A single FreeRTOS mutex serialises all access; reads
 * are short-running so contention isn't an issue.
 *
 * Lifetime: init() requires the LittleFS storage partition to be
 * mounted (sprite_cache::init() does this). Call init() after
 * sprite_cache::init() in main.cpp.
 */

#pragma once

#include "pet_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ring capacity. 1024 events × 16 B/record + 16 B header = ~16 KB
 * on disk. LittleFS storage partition has ~4 MB so cost is
 * negligible; sized this way to comfortably out-buffer M13's
 * sync window even if the device is offline for hours. */
#define EVENT_LOG_CAPACITY 1024

/* Maximum slice size callers can request via load_recent. The TS
 * code consumes ≤12 events; this is the upper bound on engagement.c's
 * per-call cost. */
#define EVENT_LOG_SLICE_MAX 32

/* Mount + read header. Returns true on success. Idempotent. Must be
 * called after the LittleFS storage partition is mounted. */
bool event_log_init(void);

/* Append a single event with `at = esp_timer time mapped to ms-since-
 * epoch via the device's RTC`. Caller is responsible for picking
 * `at` — typically rtc::now_ms() or a wall-clock estimate.
 *
 * Returns true if the record was committed to disk, false on I/O
 * error (logged but non-fatal — engagement degrades gracefully). */
bool event_log_append(event_kind_t kind, int64_t at_ms);

/* Load up to `cap` most-recent events into `out`, newest first.
 * Returns the number actually loaded (≤ cap, ≤ current count). */
size_t event_log_load_recent(pet_event_t *out, size_t cap);

/* Total number of events currently in the ring (≤ EVENT_LOG_CAPACITY).
 * Useful for diagnostics. */
size_t event_log_count(void);

/* Wipe the on-disk log. Used by factory_reset; not part of the
 * normal device lifecycle. */
bool event_log_clear(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif
