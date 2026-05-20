/*
 * pet_state — substrate types mirrored from c15r/mochi:shared/types.ts.
 *
 * This is the device-side projection of mochi's pet substrate. The
 * server is the source of truth for the values; this header just
 * defines the shape so decay / engagement / mood projection can be
 * ported to C and run on-device per design/11-m11-pet-state-in-c.md.
 *
 * Conventions vs the TS:
 *   - Stats are uint8_t in [0, 100] rather than `number`. The TS
 *     code clamps to that range anyway and the wire format would
 *     send integers; the saved space matters when the snapshot
 *     also has to be persisted in NVS later.
 *   - Time is int64_t milliseconds since the epoch — same units as
 *     `Date.now()` the server uses. Wide enough that we don't have
 *     to think about overflow until 2262.
 *   - Strings (mood/sprite names) are returned via small const tables
 *     of `const char *` in mood.c; callers index by enum.
 *   - PetEvent's `data` payload field from TS is intentionally
 *     dropped — none of the three projection modules consume it.
 *     M13's sync layer can carry it on the wire if needed.
 *
 * Pure data only — no functions. decay.h / engagement.h / mood.h
 * provide projections over these types.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t happiness;  /* [0, 100] */
    uint8_t fullness;   /* [0, 100] */
    uint8_t energy;     /* [0, 100] */
} pet_stats_t;

/* Match the TS Mood union, in declaration order so values can be
 * indexed into the parallel name + hint tables in mood.c. Add any
 * new TS variant by appending here AND extending the tables in
 * mood.c — keep them in lock-step. */
typedef enum {
    MOOD_SLEEPING = 0,
    MOOD_HUNGRY,
    MOOD_TIRED,
    MOOD_LONELY,
    MOOD_PLAYFUL,
    MOOD_CURIOUS,
    MOOD_CONTENT,
    MOOD_SURPRISED,
    MOOD_COUNT,
} mood_t;

/* Sprite key — superset of mood_t per the TS definition. mood_to_sprite
 * always returns one of these; resolve_sprite preferentially returns
 * pet.transient.sprite if set. */
typedef enum {
    SPRITE_SLEEPING = 0,
    SPRITE_HUNGRY,
    SPRITE_TIRED,
    SPRITE_LONELY,
    SPRITE_PLAYFUL,
    SPRITE_CURIOUS,
    SPRITE_SURPRISED,
    SPRITE_HAPPY,            /* adult content */
    SPRITE_BLUSHING,         /* young content / hugged */
    SPRITE_EATING,
    SPRITE_EXCITED,          /* played */
    SPRITE_COMFORTED,
    SPRITE_CHEERFUL_WAVE,    /* cheered */
    SPRITE_WAKING_UP,
    SPRITE_THINKING,
    SPRITE_NEUTRAL,
    SPRITE_SAD,
    SPRITE_GOODBYE,
    SPRITE_COUNT,
    SPRITE_NONE = -1,
} sprite_key_t;

typedef enum {
    AGE_NEWBORN = 0,
    AGE_YOUNG,
    AGE_ADOLESCENT,
    AGE_ADULT,
    AGE_ELDER,
} age_stage_t;

typedef struct {
    int64_t     days;
    age_stage_t stage;
} age_t;

/* Event kinds in declaration order; engagement.c indexes the WEIGHTS
 * table by this enum. Lock-step requirement applies. */
typedef enum {
    EVENT_FED = 0,
    EVENT_PLAYED,
    EVENT_COMFORTED,
    EVENT_CHEERED,
    EVENT_HUGGED,
    EVENT_SLEPT,
    EVENT_WOKE,
    EVENT_TALKED,
    EVENT_TAPPED,
    EVENT_AUTO,
    EVENT_COUNT,
    EVENT_NONE = -1,
} event_kind_t;

typedef struct {
    event_kind_t kind;
    int64_t      at;     /* ms since epoch */
} pet_event_t;

/* Transient mood — short-lived sprite override set by a recent action.
 * `until == 0` means "no transient". */
typedef struct {
    sprite_key_t sprite;
    int64_t      until;
} transient_mood_t;

/* Pet snapshot — the subset of TS Pet the projection modules need.
 * Excludes id/name/species/PetKnowledge etc. — those don't flow into
 * mood projection on the device. M13's sync layer carries the wider
 * shape over the wire and reconstructs this struct. */
typedef struct {
    int64_t          born_at;             /* ms since epoch */
    int64_t          stats_at;            /* when `stats` were snapshotted */
    int64_t          last_interaction_at; /* lonely-threshold input */
    pet_stats_t      stats;
    bool             asleep;
    transient_mood_t transient;           /* until==0 → none */
} pet_t;

#ifdef __cplusplus
}  /* extern "C" */
#endif
