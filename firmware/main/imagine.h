/*
 * imagine — on-device scene generation pipeline.
 *
 * The end-state experience: the kid asks Mochi to imagine a place,
 * the OpenAI image-edit endpoint paints the sheet using the user's
 * BYO key (kept on-device), val.run extracts the cells, the device
 * downloads the resulting .mpk pack and swaps it in as the live
 * scene source. See design/15-on-device-imagine.md for the full
 * walk-through.
 *
 * v0 scope: scaffolding + phase tracking. The OpenAI multipart POST
 * + b64 PNG decode are stubbed; calling imagine_start() runs the
 * pipeline through QUEUEING and stops at FETCHING_GUIDE so the
 * voice-tool path can be exercised before the network heavy lifting
 * lands. Each subsequent commit fills in one phase — see the design
 * doc's "v0 scope" for ordering.
 *
 * Concurrency: one imagine task at a time (bounded PSRAM). Refused
 * if voice is currently active (memory budget assumes the two paths
 * don't overlap).
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    IMAGINE_IDLE = 0,
    IMAGINE_QUEUEING,
    IMAGINE_FETCHING_GUIDE,
    IMAGINE_FETCHING_EXEMPLAR,
    IMAGINE_GENERATING,        /* api.openai.com/v1/images/edits */
    IMAGINE_UPLOADING,         /* mochi.val.run/sheets/:id/png */
    IMAGINE_FINALISING,        /* /api/places/:id/ready */
    IMAGINE_FETCHING_PACK,     /* /api/places/:id/pack.mpk */
    IMAGINE_SWAPPING,          /* persist + scene_pack swap */
    IMAGINE_DONE,
    IMAGINE_FAILED,
} imagine_phase_t;

typedef struct {
    char place_id[40];     /* val.run's place id, set during QUEUEING */
    char seed_name[80];
    char seed_vibe[400];
    char from_place_id[40]; /* "" = let server pick */
} imagine_req_t;

/* Bring-up. Allocates the worker queue + persistent state. Idempotent.
 * Returns false if essential resources couldn't be acquired. */
bool imagine_init(void);

/* Kick off a new imagine. Copies `req` into the worker's owned
 * storage. Returns false if:
 *   - imagine_init has not run
 *   - another imagine is already in flight
 *   - voice session is currently active
 *   - the per-device debounce window (60 s) hasn't elapsed
 *
 * The voice-tool dispatch layer is the expected caller; on `false`
 * the model gets a structured `{ ok: false, reason: "..." }` reply. */
bool imagine_start(const imagine_req_t *req);

/* True while a request is queued or running. Cleared on DONE/FAILED. */
bool imagine_in_flight(void);

/* Coarse phase indicator. UI reads this each tick to decide what
 * thought-bubble text to surface ("painting…" / "almost there"…). */
imagine_phase_t imagine_phase(void);

/* Place id of the currently-resolving (or last-resolved) request.
 * Empty string before the first imagine_start. Borrowed pointer —
 * lifetime is the program. */
const char *imagine_place_id(void);

/* On DONE, the relative path of the persisted pack on littlefs.
 * Empty otherwise. Used by the scene-swap step + by tests. */
const char *imagine_last_pack_path(void);

/* Short reason string when phase is FAILED. Empty otherwise. */
const char *imagine_last_reason(void);

#ifdef __cplusplus
}
#endif
