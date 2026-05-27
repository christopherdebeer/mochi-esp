/*
 * thought — generation predicate + bubble rendering.
 *
 * Two halves: a pure thought_generate() that mirrors the relevant
 * subset of c15r/mochi:shared/thoughts.ts, and a thought_render()
 * that stamps the bubble shape + text into the 1bpp composite
 * framebuffer the main render loop builds for the SSD1681.
 *
 * Bubble layout lives entirely in this file (constants below). The
 * design doc walks the math; the short version is "fits exactly
 * between the TL and TR corner icons so it never has to overlap
 * them." See design/12-thought-bubble.md §rendering.
 */

#include "thought.h"
#include "font8x8.h"
#include "mood.h"

#include <string.h>

/* ─── M1 predicate constants — mirrored from shared/thoughts.ts ───
 *
 * Energy at or below this floor while awake surfaces a SLEEPY thought.
 * Tuned low so the bubble feels like a real "needs sleep" signal,
 * not a routine state — most of the day mochi quietly gets on with
 * life and the bubble only appears when something is genuinely off.
 * Server-side `c15r/mochi:shared/thoughts.ts` should track the
 * same number; if they drift, device and web will disagree on
 * what counts as "really needs sleep." */
static const uint8_t SLEEPY_ENERGY_FLOOR = 10;
/* Reserved for M2 (hungry need):
 *   static const uint8_t HUNGRY_FULLNESS_THRESHOLD = 35; */

/* ─── Bubble geometry ──────────────────────────────────────────────
 *
 * Coordinates are panel-space pixels. The bubble sits exactly in the
 * 96-px gap between the TL care icon (ends x=52) and the TR care
 * icon (starts x=148); we use 54..146 (92 wide) for a 2-px breathing
 * margin against each neighbour. Top edge clears the status bar
 * (ends y=18) with a 3-px gap; bottom edge clears the pet head
 * (~y=92) with the tail. */
static const int BUBBLE_X0    = 54;
static const int BUBBLE_X1    = 146;     /* exclusive */
static const int BUBBLE_Y0    = 42;
static const int BUBBLE_Y1    = 78;      /* exclusive */
static const int BUBBLE_TAIL_W = 6;      /* base of the down-pointing tail */
static const int BUBBLE_TAIL_H = 6;      /* height; tip lands at Y1 + H - 1 */
static const int HIT_PAD       = 6;      /* enlarge tap target on every side */

/* Two scale-1 text lines, centered horizontally inside the bubble.
 * y values are the top of each glyph (glyphs are 8 rows tall). */
static const int LINE1_TOP_Y = 48;
static const int LINE2_TOP_Y = 60;

/* ─── Framebuffer helpers ──────────────────────────────────────────
 *
 * Match the inline 1bpp math the existing chrome blit in main.cpp
 * already uses: MSB-first within each byte, row-major, stride is
 * (w + 7) / 8 bytes per row. Bit 0 = ink (black); bit 1 = paper
 * (white). The rule is exactly the same as compositor::clear_to_paper
 * / blit_two_plane, just at the per-pixel level. */

static inline void pixel_black(uint8_t *dst, size_t stride, int x, int y) {
    const size_t off = (size_t)y * stride + ((size_t)x >> 3);
    const uint8_t mask = (uint8_t)(1u << (7 - ((size_t)x & 7)));
    dst[off] &= (uint8_t)~mask;
}

static inline void pixel_white(uint8_t *dst, size_t stride, int x, int y) {
    const size_t off = (size_t)y * stride + ((size_t)x >> 3);
    const uint8_t mask = (uint8_t)(1u << (7 - ((size_t)x & 7)));
    dst[off] |= mask;
}

static void fill_rect_white(uint8_t *dst, size_t dst_w, size_t dst_h,
                            int x0, int y0, int x1, int y1) {
    const size_t stride = (dst_w + 7) >> 3;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)dst_w) x1 = (int)dst_w;
    if (y1 > (int)dst_h) y1 = (int)dst_h;
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            pixel_white(dst, stride, x, y);
        }
    }
}

static void draw_hline_black(uint8_t *dst, size_t dst_w, size_t dst_h,
                             int x0, int x1, int y) {
    const size_t stride = (dst_w + 7) >> 3;
    if (y < 0 || y >= (int)dst_h) return;
    if (x0 < 0) x0 = 0;
    if (x1 > (int)dst_w) x1 = (int)dst_w;
    for (int x = x0; x < x1; x++) pixel_black(dst, stride, x, y);
}

static void draw_vline_black(uint8_t *dst, size_t dst_w, size_t dst_h,
                             int x, int y0, int y1) {
    const size_t stride = (dst_w + 7) >> 3;
    if (x < 0 || x >= (int)dst_w) return;
    if (y0 < 0) y0 = 0;
    if (y1 > (int)dst_h) y1 = (int)dst_h;
    for (int y = y0; y < y1; y++) pixel_black(dst, stride, x, y);
}

/* Centered scale-1 glyph blit. Mirrors the inline pattern in
 * main.cpp's render_chrome — bit 0 of each row is the leftmost
 * column, set bits draw black, unset bits leave the framebuffer
 * pixel alone. */
static void blit_text_centered(uint8_t *dst, size_t dst_w, size_t dst_h,
                               const char *text, int x_center, int y_top) {
    if (!text || !*text) return;
    const size_t stride = (dst_w + 7) >> 3;
    const int len = (int)strlen(text);
    int x = x_center - (len * 8) / 2;
    for (int i = 0; i < len; i++) {
        const uint8_t *g = font8x8_glyph(text[i]);
        const int ox = x + i * 8;
        for (int row = 0; row < 8; row++) {
            const uint8_t bits = g[row];
            for (int col = 0; col < 8; col++) {
                if (!((bits >> col) & 1)) continue;
                const int px = ox + col;
                const int py = y_top + row;
                if (px < 0 || py < 0 ||
                    px >= (int)dst_w || py >= (int)dst_h) continue;
                pixel_black(dst, stride, px, py);
            }
        }
    }
}

/* ─── Public API ──────────────────────────────────────────────────── */

extern "C" bool thought_generate(const pet_t *pet, int64_t /*now_ms*/,
                                 pet_thought_t *out) {
    if (!pet || !out) return false;

    /* WAKE — pet is currently asleep. Tapping = wake mochi up.
     * Symmetric with the web side's care_direct{kind:"woke"} path:
     * substrate sets asleep=false on its next mutate, and the
     * resting sprite flips back to its waking projection. */
    if (pet->asleep) {
        out->action_kind   = THOUGHT_ACTION_CARE_EVENT;
        out->action_event  = EVENT_WOKE;
        out->line1         = "zzz...";
        out->line2         = "tap wake";
        out->expires_at_ms = 0;
        return true;
    }

    /* SLEEPY — energy at or below the floor, awake. Tapping = put
     * mochi to sleep. The visible action on the device is symmetric
     * with the web side's care_direct{kind:"slept"} path: substrate
     * sets asleep=true on its next mutate, and the resting sprite
     * flips to SPRITE_SLEEPING on the following render. */
    if (pet->stats.energy <= SLEEPY_ENERGY_FLOOR) {
        out->action_kind   = THOUGHT_ACTION_CARE_EVENT;
        out->action_event  = EVENT_SLEPT;
        out->line1         = "sleepy...";
        out->line2         = "tap sleep";
        out->expires_at_ms = 0;
        return true;
    }

    /* M2 chain extends here (hungry → fed, lonely → talk_seed). */
    return false;
}

/* ─── Pet-tap mood readout ────────────────────────────────────────
 *
 * Unlike thought_generate (which silences itself unless a real need
 * is firing), this always produces a bubble — the user just tapped
 * the pet and is asking "how are you?". Lines are pre-curated to
 * fit the 11-char-per-line scale-1 budget; one line is a short
 * vocalisation, the second is a phrase. action_kind = NONE so the
 * bubble can't be tapped to dispatch anything — it's a passive
 * register of feelings, not a care affordance. */
extern "C" void thought_for_pet_tap(const pet_t *pet,
                                    const pet_event_t *events,
                                    size_t event_count,
                                    int64_t now_ms,
                                    pet_thought_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->action_kind   = THOUGHT_ACTION_NONE;
    out->style         = THOUGHT_STYLE_THOUGHT;
    out->expires_at_ms = 0;

    const mood_t m = pet ? project_mood(pet, events, event_count, now_ms)
                         : MOOD_CURIOUS;
    switch (m) {
        case MOOD_SLEEPING:
            out->line1 = "zzz...";
            out->line2 = "shh";
            break;
        case MOOD_HUNGRY:
            out->line1 = "hungry...";
            out->line2 = "got snack?";
            break;
        case MOOD_TIRED:
            out->line1 = "yawn...";
            out->line2 = "so sleepy";
            break;
        case MOOD_LONELY:
            out->line1 = "miss u...";
            out->line2 = "where ru?";
            break;
        case MOOD_PLAYFUL:
            out->line1 = "play!";
            out->line2 = "zoomies!";
            break;
        case MOOD_CURIOUS:
            out->line1 = "hmm...";
            out->line2 = "what's up?";
            break;
        case MOOD_CONTENT:
            out->line1 = "hi :)";
            out->line2 = "all good";
            break;
        case MOOD_SURPRISED:
            out->line1 = "oh!";
            out->line2 = "what was?";
            break;
        default:
            out->line1 = "hi :)";
            out->line2 = "";
            break;
    }
}

extern "C" void thought_render(uint8_t *dst, size_t dst_w, size_t dst_h,
                               const pet_thought_t *thought,
                               thought_hit_rect_t *out_hit) {
    if (!dst || !thought) return;

    /* Interior fill — paper-white inside the bubble. Anything the
     * scene or pet had drawn here gets covered; the bubble owns
     * this rect. */
    fill_rect_white(dst, dst_w, dst_h,
                    BUBBLE_X0, BUBBLE_Y0, BUBBLE_X1, BUBBLE_Y1);

    /* 1-px border with chamfered corners. Top + bottom horizontals
     * are 1 pixel shorter on each side; left + right verticals are
     * 1 pixel shorter on each end. No per-corner pixels are drawn,
     * which reads as a soft rounded shape at this scale. */
    draw_hline_black(dst, dst_w, dst_h, BUBBLE_X0 + 1, BUBBLE_X1 - 1, BUBBLE_Y0);
    draw_hline_black(dst, dst_w, dst_h, BUBBLE_X0 + 1, BUBBLE_X1 - 1, BUBBLE_Y1 - 1);
    draw_vline_black(dst, dst_w, dst_h, BUBBLE_X0, BUBBLE_Y0 + 1, BUBBLE_Y1 - 1);
    draw_vline_black(dst, dst_w, dst_h, BUBBLE_X1 - 1, BUBBLE_Y0 + 1, BUBBLE_Y1 - 1);

    /* Tail — two visual registers:
     *
     *   SPOKEN: filled triangle pointing down at the pet's head —
     *           comic convention for "what the speaker said".
     *   THOUGHT: two small filled blobs trailing down — comic
     *            convention for "what the speaker is thinking".
     *
     * Both fit inside BUBBLE_TAIL_H so the hit-rect math below is
     * the same shape regardless of style. */
    const int tail_cx = (BUBBLE_X0 + BUBBLE_X1) / 2;
    if (thought->style == THOUGHT_STYLE_SPOKEN) {
        for (int row = 0; row < BUBBLE_TAIL_H; row++) {
            const int half_w = (BUBBLE_TAIL_W - 2 * row) / 2;
            if (half_w <= 0) break;
            const int y = BUBBLE_Y1 + row;
            if (y >= (int)dst_h) break;
            draw_hline_black(dst, dst_w, dst_h,
                             tail_cx - half_w, tail_cx + half_w + 1, y);
        }
        /* Hollow the very top row of the tail back to paper so the
         * bubble's bottom border + the tail's top read as continuous
         * outline rather than a black bar. */
        {
            const size_t stride = (dst_w + 7) >> 3;
            const int y = BUBBLE_Y1;
            if (y >= 0 && y < (int)dst_h) {
                for (int x = tail_cx - 1; x <= tail_cx + 1; x++) {
                    if (x >= 0 && x < (int)dst_w) {
                        pixel_white(dst, stride, x, y);
                    }
                }
            }
        }
    } else {
        /* THOUGHT — two trailing blobs. Upper blob is a 3×3 with the
         * top-left/top-right corners off (reads as a 3-px rounded
         * dot at this scale). Lower blob is a 2×2 sitting one px
         * below. Both centered on tail_cx, fitting inside BUBBLE_TAIL_H. */
        const size_t stride = (dst_w + 7) >> 3;
        /* Upper blob — y rows BUBBLE_Y1+1 .. BUBBLE_Y1+3. */
        for (int dy = 1; dy <= 3; dy++) {
            const int y = BUBBLE_Y1 + dy;
            if (y < 0 || y >= (int)dst_h) continue;
            for (int dx = -1; dx <= 1; dx++) {
                /* Soft-corner the top row of the 3×3 so it reads as a dot. */
                if (dy == 1 && (dx == -1 || dx == 1)) continue;
                const int x = tail_cx + dx;
                if (x < 0 || x >= (int)dst_w) continue;
                pixel_black(dst, stride, x, y);
            }
        }
        /* Lower blob — 2×2 at y rows BUBBLE_Y1+4 .. BUBBLE_Y1+5,
         * offset half a pixel left of centre so the pair reads as
         * trailing diagonally toward the pet. */
        for (int dy = 4; dy <= 5; dy++) {
            const int y = BUBBLE_Y1 + dy;
            if (y < 0 || y >= (int)dst_h) continue;
            for (int dx = -1; dx <= 0; dx++) {
                const int x = tail_cx + dx;
                if (x < 0 || x >= (int)dst_w) continue;
                pixel_black(dst, stride, x, y);
            }
        }
    }

    /* Two scale-1 text lines, centered on the bubble's vertical
     * midline. Glyphs that exceed the bubble are clipped by the
     * generic glyph blit's bounds check (safe; the caller is
     * responsible for keeping text short). */
    blit_text_centered(dst, dst_w, dst_h, thought->line1, tail_cx, LINE1_TOP_Y);
    blit_text_centered(dst, dst_w, dst_h, thought->line2, tail_cx, LINE2_TOP_Y);

    /* Touch hit rectangle — enlarge from the visible shape by
     * HIT_PAD on every side, including past the tail tip so a tap
     * just under the bubble still registers. */
    if (out_hit) {
        out_hit->x0 = BUBBLE_X0 - HIT_PAD;
        out_hit->y0 = BUBBLE_Y0 - HIT_PAD;
        out_hit->x1 = BUBBLE_X1 + HIT_PAD;
        out_hit->y1 = BUBBLE_Y1 + BUBBLE_TAIL_H + HIT_PAD;
    }
}

extern "C" bool thought_hit_contains(const thought_hit_rect_t *r,
                                     int x, int y) {
    if (!r) return false;
    return x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1;
}
