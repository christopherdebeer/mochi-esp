/*
 * mood — projection from substrate to mood + sprite, mirrored from
 * c15r/mochi:shared/mood.ts.
 *
 * project_mood: short-circuits on asleep, applies hard floors
 * (HARD_HUNGRY=15, HARD_TIRED=12), gates soft floors on engagement,
 * scales the lonely threshold by 1/age_multiplier, falls through to
 * playful / content / curious.
 *
 * resolve_sprite: pet.transient.sprite if `until > now`, else
 * mood_to_sprite(mood, age_stage). The content branch forks on age
 * stage: newborn / young → SPRITE_BLUSHING; older → SPRITE_HAPPY.
 *
 * mood_to_name / sprite_to_name return the canonical strings the
 * server uses on the wire (same identifiers as in shared/types.ts).
 *
 * mood_hint returns one of three flavour strings per mood, indexed
 * by `seed`. Used by the device-side debug log; UI surfaces will
 * eventually consume it.
 */

#pragma once

#include "pet_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Ports the TS projectMood. `events` is the recent slice (newest-or-
 * oldest first either works — projection is order-independent).
 * `count` should be ≤12 in practice; older events contribute
 * negligibly anyway because of the 30-min half-life. */
mood_t project_mood(const pet_t *pet,
                    const pet_event_t *events, size_t count,
                    int64_t now_ms);

/* Transient takes precedence; otherwise mood_to_sprite. */
sprite_key_t resolve_sprite(const pet_t *pet, mood_t mood,
                            age_stage_t age_stage, int64_t now_ms);

/* Mood → sprite mapping. The content branch forks on age stage. */
sprite_key_t mood_to_sprite(mood_t mood, age_stage_t age_stage);

/* Canonical wire-format names. Returns NULL on out-of-range input. */
const char *mood_to_name(mood_t m);
const char *sprite_to_name(sprite_key_t s);
const char *event_kind_to_name(event_kind_t k);
const char *age_stage_to_name(age_stage_t a);

/* Reverse: parse the wire-format name back into the enum. Returns
 * the appropriate _NONE / _COUNT sentinel if no match. */
event_kind_t event_kind_from_name(const char *s);

/* One-line copy hint per mood. `seed` lets callers vary per minute. */
const char *mood_hint(mood_t m, unsigned seed);

#ifdef __cplusplus
}  /* extern "C" */
#endif
