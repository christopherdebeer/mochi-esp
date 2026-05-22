/*
 * thought — general-purpose progressive-disclosure bubble.
 *
 * Surfaces situational tap targets above the pet when the substrate
 * warrants an action the kid wouldn't otherwise reach (e.g. Sleep,
 * which has no permanent corner-icon slot). Mirrors the web side's
 * `shared/thoughts.ts` model — generation is a pure predicate chain
 * over the pet snapshot, rendering is a thin 1bpp blit into the
 * existing composite framebuffer.
 *
 * See design/12-thought-bubble.md for the full subsystem rationale,
 * layout constants, and the M2/M3 extension plan.
 *
 * M1 (this header) ships SLEEPY only; the action union is structured
 * so M2/M3 (hungry, lonely, talk-seed, navigate) can land without an
 * ABI break.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pet_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What happens when the bubble is tapped. CARE_EVENT routes the
 * `event_kind_t` to substrate.mutate via the existing pet_sync
 * enqueue path; TALK_SEED + NAVIGATE are reserved for later
 * milestones (no payload on the struct yet). */
typedef enum {
    THOUGHT_ACTION_NONE = 0,
    THOUGHT_ACTION_CARE_EVENT,
    THOUGHT_ACTION_TALK_SEED,
    THOUGHT_ACTION_NAVIGATE,
} thought_action_kind_t;

/* Pure-data view of one thought. Strings are owned by the producer
 * — for the M1 predicate chain they're string literals in
 * thought.cpp's generator, so callers can hold the pointer across
 * the render call without copying. */
typedef struct {
    thought_action_kind_t action_kind;
    event_kind_t          action_event;   /* CARE_EVENT only */
    const char           *line1;          /* top label; ≤ 11 scale-1 chars */
    const char           *line2;          /* sub-line hint; ≤ 11 scale-1 chars */
    int64_t               expires_at_ms;  /* 0 = lifetime tied to need */
} pet_thought_t;

/* Tap hit rectangle, panel coordinates. Half-open: [x0, x1) × [y0, y1). */
typedef struct {
    int x0, y0, x1, y1;
} thought_hit_rect_t;

/* Generate a thought from the current pet snapshot. Returns true and
 * fills *out when a thought applies, false otherwise. Pure: no
 * allocation, no I/O, deterministic in (pet, now_ms).
 *
 * M1 chain — SLEEPY only:
 *   - pet.asleep                          → no thought
 *   - pet.stats.energy < SLEEPY_THRESHOLD → SLEEPY (CARE_EVENT: SLEPT)
 *
 * Future milestones append rules; first match wins. See
 * design/12-thought-bubble.md §generation. */
bool thought_generate(const pet_t *pet, int64_t now_ms,
                      pet_thought_t *out);

/* Render a thought bubble into a 200×200 1bpp MSB-first composite
 * framebuffer. The bubble is positioned between the TL and TR corner
 * care icons (centered on x=100) just below the status bar, with a
 * small tail pointing down toward the pet's head.
 *
 * dst_w / dst_h are the framebuffer dimensions (200/200 in practice);
 * stride is derived as (dst_w + 7) / 8 to match the existing chrome
 * blit. The function only writes inside the bubble + tail rect — the
 * rest of the framebuffer is untouched.
 *
 * out_hit receives the touch hit rectangle (enlarged from the visible
 * shape) so the caller can store it for the touch classifier. May be
 * NULL if the caller doesn't need the rect. */
void thought_render(uint8_t *dst, size_t dst_w, size_t dst_h,
                    const pet_thought_t *thought,
                    thought_hit_rect_t *out_hit);

/* Convenience: is (x, y) inside *r? Safe to call with r == NULL
 * (returns false). Used by the touch classify path so a callsite
 * doesn't have to inline the bounds check. */
bool thought_hit_contains(const thought_hit_rect_t *r, int x, int y);

#ifdef __cplusplus
}  /* extern "C" */
#endif
