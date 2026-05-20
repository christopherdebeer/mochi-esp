#include "decay.h"

#include <math.h>

#define HOUR_MS         (60LL * 60 * 1000)
#define DAY_MS          (24LL * HOUR_MS)
#define DECAY_CAP_HOURS 36.0

/* Per-hour rates — adult-equivalent baseline. age_multiplier scales
 * the awake set at the call site. Asleep decay is age-flat. */
static const double RATE_AWAKE_HAPPINESS = 3.0;
static const double RATE_AWAKE_FULLNESS  = 4.0;
static const double RATE_AWAKE_ENERGY    = 2.5;
static const double RATE_ASLEEP_HAPPINESS = 1.0;
static const double RATE_ASLEEP_FULLNESS  = 2.0;
static const double RATE_ASLEEP_ENERGY    = -8.0;  /* negative = regen */

static uint8_t clamp_u8(double n) {
    if (n < 0.0)   return 0;
    if (n > 100.0) return 100;
    return (uint8_t)(n + 0.5);
}

double age_multiplier(int64_t days) {
    if (days < 0) days = 0;
    return 0.7 + 3.3 * exp(-(double)days / 25.0);
}

pet_stats_t decay_stats(pet_stats_t stats, int64_t stats_at,
                        bool asleep, int64_t age_days, int64_t now_ms) {
    double elapsed_h = (double)(now_ms - stats_at) / (double)HOUR_MS;
    if (elapsed_h <= 0.0) return stats;
    if (elapsed_h > DECAY_CAP_HOURS) elapsed_h = DECAY_CAP_HOURS;

    const double rh = asleep ? RATE_ASLEEP_HAPPINESS : RATE_AWAKE_HAPPINESS;
    const double rf = asleep ? RATE_ASLEEP_FULLNESS  : RATE_AWAKE_FULLNESS;
    const double re = asleep ? RATE_ASLEEP_ENERGY    : RATE_AWAKE_ENERGY;
    const double mult = asleep ? 1.0 : age_multiplier(age_days);

    pet_stats_t out;
    out.happiness = clamp_u8((double)stats.happiness - rh * mult * elapsed_h);
    out.fullness  = clamp_u8((double)stats.fullness  - rf * mult * elapsed_h);
    out.energy    = clamp_u8((double)stats.energy    - re * mult * elapsed_h);
    return out;
}

age_t compute_age(int64_t born_at, int64_t now_ms) {
    int64_t elapsed_ms = now_ms - born_at;
    if (elapsed_ms < 0) elapsed_ms = 0;
    int64_t days = elapsed_ms / DAY_MS;

    age_stage_t stage;
    if      (days <   1) stage = AGE_NEWBORN;
    else if (days <   7) stage = AGE_YOUNG;
    else if (days <  30) stage = AGE_ADOLESCENT;
    else if (days < 180) stage = AGE_ADULT;
    else                 stage = AGE_ELDER;

    age_t out = { days, stage };
    return out;
}
