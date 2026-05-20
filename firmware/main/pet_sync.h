/*
 * pet_sync — bidi sync of substrate state with mochi.val.run.
 *
 * Three responsibilities (M13a/b/c):
 *   pull_now()     — GET /api/state, populate pet_t + recent slice.
 *                    Called at boot and on demand. Synchronous; uses
 *                    https_get from voice_https.
 *   push_event()   — POST /api/mutate, attach the response back into
 *                    pet_t. Async via a worker queue so touch
 *                    handling never blocks on TLS.
 *   start_periodic() — kicks a background task that re-pulls every
 *                      N minutes when the device is otherwise idle.
 *
 * The local event_log on disk is a push-buffer + projection input
 * source: appended to on every touch (already wired in main.cpp),
 * drained by the push worker once acknowledged. engagement.c reads
 * the union of the server-supplied recent slice + locally-buffered
 * not-yet-pushed events.
 *
 * Auth: X-Pet-Id header, same as voice tool routing. The pet id
 * comes from pair_creds (NVS).
 *
 * Lifecycle:
 *   boot:
 *     time_sync_init()       (M13.0)
 *     pet_sync_pull_now()    (M13a — populate pet_t)
 *     pet_sync_start()       (M13b/c — workers)
 *
 * If the initial pull fails, falls back to whatever the cached
 * snapshot in pet_state_*() returns (today: hardcoded; future:
 * NVS-cached snapshot).
 */

#pragma once

#include "pet_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous /api/state pull. Populates *out_pet, fills out_events
 * up to slice_cap (returns count via *out_count). Returns true on
 * success. Caller owns the buffers. */
bool pet_sync_pull_now(pet_t *out_pet,
                       pet_event_t *out_events, size_t slice_cap,
                       size_t *out_count);

/* Spawn the push worker + periodic-resync task. Idempotent. The push
 * worker pulls from the queue, POSTs to /api/mutate, and updates the
 * shared pet_t snapshot from the response. */
void pet_sync_start(void);

/* Enqueue an event for push. Non-blocking. Returns true if queued.
 * The local event_log is the source of truth; this is just the
 * pending-push buffer so the worker knows what to send. */
bool pet_sync_enqueue(event_kind_t kind, int64_t at_ms);

/* Latest snapshot (mutex-protected). Returns true if a snapshot has
 * been pulled at least once. *out_pet is filled with the current
 * substrate state. */
bool pet_sync_get_snapshot(pet_t *out_pet);

/* Update the externally-stored last_interaction_at — called on every
 * tap so loneliness resets immediately even before the push round-
 * trip lands. The next /api/mutate response will overwrite it from
 * the authoritative server value. */
void pet_sync_touch(int64_t at_ms);

#ifdef __cplusplus
}  /* extern "C" */
#endif
