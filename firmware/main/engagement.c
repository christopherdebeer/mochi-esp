#include "engagement.h"

#include <math.h>

const double ENGAGEMENT_FLOOR = 0.45;

#define HALFLIFE_MS  (30LL * 60 * 1000)
#define SATURATION   2.0

/* Indexed by event_kind_t. Order MUST match the enum in pet_state.h.
 * Mirrors the WEIGHTS table in shared/engagement.ts:
 *   talked       1.0    strongest signal — voice turn
 *   played       0.7    active care
 *   fed/comforted/hugged 0.6   passive care, still warm
 *   cheered      0.5    lighter touch
 *   woke         0.3    tepid until something follows
 *   tapped       0.2    a glance, not a moment
 *   slept        0.0    initiates absence; the absence accrues
 *   auto         0.0    system-driven; carries no engagement */
static const double WEIGHTS[EVENT_COUNT] = {
    [EVENT_FED]       = 0.6,
    [EVENT_PLAYED]    = 0.7,
    [EVENT_COMFORTED] = 0.6,
    [EVENT_CHEERED]   = 0.5,
    [EVENT_HUGGED]    = 0.6,
    [EVENT_SLEPT]     = 0.0,
    [EVENT_WOKE]      = 0.3,
    [EVENT_TALKED]    = 1.0,
    [EVENT_TAPPED]    = 0.2,
    [EVENT_AUTO]      = 0.0,
};

double recent_engagement(const pet_event_t *events, size_t count,
                         int64_t now_ms) {
    if (!events || count == 0) return 0.0;

    double signal = 0.0;
    for (size_t i = 0; i < count; i++) {
        event_kind_t k = events[i].kind;
        if (k < 0 || k >= EVENT_COUNT) continue;
        double w = WEIGHTS[k];
        if (w == 0.0) continue;
        int64_t age_ms = now_ms - events[i].at;
        if (age_ms < 0) continue;
        signal += w * pow(0.5, (double)age_ms / (double)HALFLIFE_MS);
    }
    return 1.0 - exp(-signal / SATURATION);
}
