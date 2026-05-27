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

/* Visual register of the bubble. Inner monologue (the pet's own
 * feelings, surfaced by the predicate chain or a pet-tap mood
 * readout) draws as a cloud with two trailing dots — the comic
 * convention for "thought". External speech (a talk_seed bubble
 * echoing what mochi would say aloud) keeps the classic rectangular
 * bubble with a triangular tail pointing at the speaker. */
typedef enum {
    THOUGHT_STYLE_THOUGHT = 0,   /* cloud + two dots (default; zero-init) */
    THOUGHT_STYLE_SPOKEN  = 1,   /* triangle tail */
} thought_style_t;

/* Pure-data view of one thought. Strings are owned by the producer
 * — for the M1 predicate chain they're string literals in
 * thought.cpp's generator, so callers can hold the pointer across
 * the render call without copying.
 *
 * `text` is a single string the renderer word-wraps + vertically
 * centres inside the bubble interior. Greedy break on space; honours
 * '\n' as a hard line break so producers can force a split where
 * the wording calls for it ("sleepy...\ntap sleep" stays as two
 * conceptual lines regardless of width). When the wrap overflows
 * the bubble's per-page line budget the renderer trails the last
 * visible line with "..." and surfaces overflow via
 * thought_has_more(); callers route a Zone::Thought tap to bump
 * `page` (passive bubbles only) until the last page is reached, at
 * which point the next tap dismisses. */
typedef struct {
    thought_action_kind_t action_kind;
    event_kind_t          action_event;   /* CARE_EVENT only */
    const char           *text;           /* body — wrapped + centred at render */
    int64_t               expires_at_ms;  /* 0 = lifetime tied to need */
    thought_style_t       style;          /* visual register; default = THOUGHT */
    int                   page;           /* 0-indexed; only meaningful for passive bubbles */
    bool                  persistent;     /* true = caller pinned this bubble; the auto-
                                           * generate path (thought_generate inside the
                                           * resting render) must not overwrite or clear
                                           * it. Used by multi-page talk_seed echoes that
                                           * need to outlive a single tap hold. */
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

/* Pet-tap mood readout. Always fills *out — no thresholds, no
 * cooldowns. Pulls the dominant current mood/need from project_mood
 * and renders it as a short two-line "this is how I feel right now"
 * bubble in the THOUGHT style. action_kind = NONE — the bubble is
 * a passive expression, not a tap target (it auto-clears with the
 * post-tap render_resting cycle).
 *
 * `events` is the recent event slice that drives project_mood
 * (same one the caller already loads for the resting render). */
void thought_for_pet_tap(const pet_t *pet,
                         const pet_event_t *events, size_t event_count,
                         int64_t now_ms,
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

/* True iff the most-recent thought_render() ran out of vertical
 * room — i.e. the rendered `page` exposed fewer lines than the
 * text would wrap to total. The render also painted "..." at the
 * end of the last visible line as the visual cue. Callers (the
 * Zone::Thought tap handler) consult this to decide whether the
 * next tap should bump the page (more to read) or dismiss (last
 * page reached). Only meaningful for passive bubbles
 * (action_kind == THOUGHT_ACTION_NONE) — action bubbles use the
 * tap for the action itself, never pagination. */
bool thought_has_more(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif
