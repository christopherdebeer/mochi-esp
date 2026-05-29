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

/* Synchronously drain the queue on the calling task. Used right
 * before deep sleep / soft power-down so a queued mutate (e.g. the
 * EVENT_SLEPT we enqueued microseconds earlier) gets a best-effort
 * push to substrate before the device powers down — otherwise the
 * background worker is killed mid-handshake by deep sleep. Bounded
 * iteration so we don't block forever if the queue is unexpectedly
 * full; each POST has its own TLS-handshake budget. Returns the
 * number of mutates successfully pushed. */
int pet_sync_push_now(void);

/* Latest snapshot (mutex-protected). Returns true if a snapshot has
 * been pulled at least once. *out_pet is filled with the current
 * substrate state. */
bool pet_sync_get_snapshot(pet_t *out_pet);

/* Update the externally-stored last_interaction_at — called on every
 * tap so loneliness resets immediately even before the push round-
 * trip lands. The next /api/mutate response will overwrite it from
 * the authoritative server value. */
void pet_sync_touch(int64_t at_ms);

/* Current location (place id) + its resolved device sheetId, from the
 * latest /api/state. Both NUL-terminated; empty strings before the
 * first successful pull or if the pet has no location. The device
 * renders pets.location: "home" → the bundle, else the place's pack
 * at device geometry. See design/17. */
void pet_sync_current_location(char *id_out, size_t id_cap,
                               char *sheet_out, size_t sheet_cap);

/* Seed the cached location from outside (e.g. NVS-restored last
 * place at boot). Lets the device render the right scene immediately
 * on wake from deep sleep instead of waiting for the first
 * /api/state pull. The next pull will overwrite from the server
 * regardless. Empty strings clear the seed. */
void pet_sync_seed_location(const char *place_id, const char *sheet);

/* Restore the pet snapshot from NVS (written on each successful
 * /api/state pull or mutate response). Marks the snapshot as
 * "have_snapshot" so engagement projection / sprite resolution see
 * server-canonical stats from the moment of wake instead of the
 * hardcoded init_dev_pet defaults. The next pull / mutate will
 * overwrite from the server. Returns true when a snapshot was
 * loaded; false on no NVS data or read failure. */
bool pet_sync_restore_snapshot_from_nvs(void);

/* Current costume id from the latest /api/state ("" = base species).
 * The device renders the pet from costume-<petId>-<costumeId>-v1 when
 * set, else the embedded/pet-v1 base. See design/17. */
void pet_sync_current_costume(char *id_out, size_t id_cap);

/* True when the latest /api/state advised a sleep-consolidation pass
 * (server-computed: asleep + activity + low engagement + cooldown).
 * main.cpp acts on it — server-orchestrated consolidation (design/19). */
bool pet_sync_consolidation_advised(void);

/* Post a realtime_sessions row on voice session end (design/18 ph3).
 * Session-level: duration + model/voice/end_reason; per-turn tokens are
 * deferred (ph3b). Best-effort; no-op without pairing. */
void pet_sync_post_voice_session(int duration_s, const char *model,
                                 const char *voice, const char *end_reason,
                                 int turns, int in_tok, int out_tok,
                                 int total_tok, const char *transcript_json);

/* Travel to a place by id (design/17): POST /api/places/:id/enter, and on
 * success optimistically set the local location + sheetId so the device's
 * location-follow render swaps to it on the next tick. Returns true on a
 * successful enter. The device→substrate travel write behind a tapped
 * nav_place zone. */
bool pet_sync_enter_place(const char *place_id);

#ifdef __cplusplus
}  /* extern "C" */
#endif
