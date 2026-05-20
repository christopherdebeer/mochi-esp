#include "mood.h"
#include "decay.h"
#include "engagement.h"

#include <string.h>

/* Adult-baseline "lonely" threshold, scaled by 1/age_multiplier:
 *   newborn (×4.0): 24h / 4.0 ≈ 6h
 *   adult   (×1.0): 24h
 *   elder   (×0.7): 24h / 0.7 ≈ 34h */
#define LONELY_BASE_MS  (24LL * 60 * 60 * 1000)

#define HARD_HUNGRY  15
#define HARD_TIRED   12
#define SOFT_HUNGRY  30
#define SOFT_TIRED   25

/* Indexed by mood_t. Order MUST match the enum in pet_state.h. */
static const char *MOOD_NAMES[MOOD_COUNT] = {
    [MOOD_SLEEPING]  = "sleeping",
    [MOOD_HUNGRY]    = "hungry",
    [MOOD_TIRED]     = "tired",
    [MOOD_LONELY]    = "lonely",
    [MOOD_PLAYFUL]   = "playful",
    [MOOD_CURIOUS]   = "curious",
    [MOOD_CONTENT]   = "content",
    [MOOD_SURPRISED] = "surprised",
};

static const char *SPRITE_NAMES[SPRITE_COUNT] = {
    [SPRITE_SLEEPING]      = "sleeping",
    [SPRITE_HUNGRY]        = "hungry",
    [SPRITE_TIRED]         = "tired",
    [SPRITE_LONELY]        = "lonely",
    [SPRITE_PLAYFUL]       = "playful",
    [SPRITE_CURIOUS]       = "curious",
    [SPRITE_SURPRISED]     = "surprised",
    [SPRITE_HAPPY]         = "happy",
    [SPRITE_BLUSHING]      = "blushing",
    [SPRITE_EATING]        = "eating",
    [SPRITE_EXCITED]       = "excited",
    [SPRITE_COMFORTED]     = "comforted",
    [SPRITE_CHEERFUL_WAVE] = "cheerful_wave",
    [SPRITE_WAKING_UP]     = "waking_up",
    [SPRITE_THINKING]      = "thinking",
    [SPRITE_NEUTRAL]       = "neutral",
    [SPRITE_SAD]           = "sad",
    [SPRITE_GOODBYE]       = "goodbye",
};

static const char *EVENT_KIND_NAMES[EVENT_COUNT] = {
    [EVENT_FED]       = "fed",
    [EVENT_PLAYED]    = "played",
    [EVENT_COMFORTED] = "comforted",
    [EVENT_CHEERED]   = "cheered",
    [EVENT_HUGGED]    = "hugged",
    [EVENT_SLEPT]     = "slept",
    [EVENT_WOKE]      = "woke",
    [EVENT_TALKED]    = "talked",
    [EVENT_TAPPED]    = "tapped",
    [EVENT_AUTO]      = "auto",
};

static const char *AGE_STAGE_NAMES[5] = {
    [AGE_NEWBORN]    = "newborn",
    [AGE_YOUNG]      = "young",
    [AGE_ADOLESCENT] = "adolescent",
    [AGE_ADULT]      = "adult",
    [AGE_ELDER]      = "elder",
};

/* Three hints per mood, mirrored from shared/mood.ts HINTS table. */
static const char *MOOD_HINTS[MOOD_COUNT][3] = {
    [MOOD_SLEEPING]  = {"zzz…",            "out cold",          "dreaming"},
    [MOOD_HUNGRY]    = {"a bit peckish",   "tummy says hi",     "could eat"},
    [MOOD_TIRED]     = {"heavy eyelids",   "needs a rest",      "low battery"},
    [MOOD_LONELY]    = {"missed you",      "where were you?",   "thinking of you"},
    [MOOD_PLAYFUL]   = {"full of bounce",  "wants to play",     "zoomies imminent"},
    [MOOD_CURIOUS]   = {"pondering",       "noticing things",   "tilting head"},
    [MOOD_CONTENT]   = {"doing okay",      "settled",           "warm and fine"},
    [MOOD_SURPRISED] = {"oh!",             "what was that?",    "huh!"},
};

mood_t project_mood(const pet_t *pet,
                    const pet_event_t *events, size_t count,
                    int64_t now_ms) {
    if (!pet) return MOOD_CURIOUS;
    if (pet->asleep) return MOOD_SLEEPING;

    age_t age = compute_age(pet->born_at, now_ms);
    double eng = recent_engagement(events, count, now_ms);

    /* Hard floors — surface regardless of engagement. */
    if (pet->stats.fullness < HARD_HUNGRY) return MOOD_HUNGRY;
    if (pet->stats.energy   < HARD_TIRED)  return MOOD_TIRED;

    bool engaged = eng > ENGAGEMENT_FLOOR;
    if (!engaged) {
        if (pet->stats.fullness < SOFT_HUNGRY) return MOOD_HUNGRY;
        if (pet->stats.energy   < SOFT_TIRED)  return MOOD_TIRED;
        double mult = age_multiplier(age.days);
        int64_t lonely_ms = mult > 0.0
            ? (int64_t)((double)LONELY_BASE_MS / mult)
            : LONELY_BASE_MS;
        if ((now_ms - pet->last_interaction_at) > lonely_ms) {
            return MOOD_LONELY;
        }
    }

    if (pet->stats.happiness > 70 && pet->stats.energy > 60) return MOOD_PLAYFUL;
    if (pet->stats.happiness >= 50)                          return MOOD_CONTENT;
    return MOOD_CURIOUS;
}

sprite_key_t mood_to_sprite(mood_t mood, age_stage_t age_stage) {
    switch (mood) {
        case MOOD_SLEEPING:  return SPRITE_SLEEPING;
        case MOOD_HUNGRY:    return SPRITE_HUNGRY;
        case MOOD_TIRED:     return SPRITE_TIRED;
        case MOOD_LONELY:    return SPRITE_LONELY;
        case MOOD_PLAYFUL:   return SPRITE_PLAYFUL;
        case MOOD_SURPRISED: return SPRITE_SURPRISED;
        case MOOD_CURIOUS:   return SPRITE_CURIOUS;
        case MOOD_CONTENT:
            return (age_stage == AGE_NEWBORN || age_stage == AGE_YOUNG)
                ? SPRITE_BLUSHING : SPRITE_HAPPY;
        case MOOD_COUNT:     return SPRITE_NEUTRAL;  /* unreachable */
    }
    return SPRITE_NEUTRAL;
}

sprite_key_t resolve_sprite(const pet_t *pet, mood_t mood,
                            age_stage_t age_stage, int64_t now_ms) {
    if (pet && pet->transient.until > now_ms &&
        pet->transient.sprite != SPRITE_NONE) {
        return pet->transient.sprite;
    }
    return mood_to_sprite(mood, age_stage);
}

const char *mood_to_name(mood_t m) {
    if (m < 0 || m >= MOOD_COUNT) return NULL;
    return MOOD_NAMES[m];
}

const char *sprite_to_name(sprite_key_t s) {
    if (s < 0 || s >= SPRITE_COUNT) return NULL;
    return SPRITE_NAMES[s];
}

const char *event_kind_to_name(event_kind_t k) {
    if (k < 0 || k >= EVENT_COUNT) return NULL;
    return EVENT_KIND_NAMES[k];
}

const char *age_stage_to_name(age_stage_t a) {
    if (a < 0 || a > AGE_ELDER) return NULL;
    return AGE_STAGE_NAMES[a];
}

event_kind_t event_kind_from_name(const char *s) {
    if (!s) return EVENT_NONE;
    for (int i = 0; i < EVENT_COUNT; i++) {
        if (strcmp(s, EVENT_KIND_NAMES[i]) == 0) {
            return (event_kind_t)i;
        }
    }
    return EVENT_NONE;
}

const char *mood_hint(mood_t m, unsigned seed) {
    if (m < 0 || m >= MOOD_COUNT) return "";
    return MOOD_HINTS[m][seed % 3];
}
