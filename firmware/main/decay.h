/*
 * decay — pure stats projection, mirrored from
 * c15r/mochi:shared/decay.ts.
 *
 * Per-hour decay with separate awake/asleep rates, age-multiplier
 * scaling, clamped to [0, 100], capped at 36 hours of elapsed wall
 * time. Pure functions, no I/O, no allocations.
 *
 * Constants and curve match the TS file. Re-tune in lock-step or
 * the device renders a different mood than the server says.
 */

#pragma once

#include "pet_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Smooth age-stage decay multiplier. floor + amp · exp(-days/scale).
 * 0 days → 4.0×, 60 days → 1.0×, 180 days → 0.7× asymptote. */
double age_multiplier(int64_t days);

/* Decay stats over elapsed time since `stats_at`. `age_days` scales
 * awake decay only; asleep rates are intentionally age-flat. */
pet_stats_t decay_stats(pet_stats_t stats, int64_t stats_at,
                        bool asleep, int64_t age_days, int64_t now_ms);

/* Discrete age — days + stage label. Stages: newborn (<1d), young
 * (<7d), adolescent (<30d), adult (<180d), elder (>=180d). */
age_t compute_age(int64_t born_at, int64_t now_ms);

#ifdef __cplusplus
}  /* extern "C" */
#endif
