/*
 * engagement — recency signal over the event log, mirrored from
 * c15r/mochi:shared/engagement.ts.
 *
 * `recent_engagement` returns a value in [0, 1] from a slice of recent
 * events (typically ≤12). Each event contributes its kind weight,
 * decayed exponentially with a 30-min half-life from `at` to `now_ms`.
 * The signal sum is squashed through 1 - exp(-signal/saturation).
 *
 * Above ENGAGEMENT_FLOOR (0.45) mood projection skips the soft-floor
 * checks (lonely / mild hunger / mild tired) — the same rule the
 * server applies.
 */

#pragma once

#include "pet_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The mood-projection floor: above this, soft needs are silenced.
 * Identical to the TS export of the same name. */
extern const double ENGAGEMENT_FLOOR;

/* Compute the engagement signal over `events[0..count)` evaluated at
 * `now_ms`. Order-independent. Negative-age (clock drift) entries are
 * ignored. Pure function. */
double recent_engagement(const pet_event_t *events, size_t count,
                         int64_t now_ms);

#ifdef __cplusplus
}  /* extern "C" */
#endif
