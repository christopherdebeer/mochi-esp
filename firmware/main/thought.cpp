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
 * Coordinates are panel-space pixels. The 128×40 central rect sits
 * between the status bar (ends y=18) and the pet sprite top
 * (PET_DY=92), with a 7-px disc halo extending another ~7 px past
 * the rect on every side — total cloud bbox ≈ 142×54 centred at
 * (100, 58). Cloud bottom lands at y=85, tail tip at y=83 (above
 * the start of the pet at y=92).
 *
 * Refinement history:
 *   pre-v0.1.9: 92×36 rect with r=4 discs. The small discs read as a
 *               knobbly staircase border — barely 8 px across, the
 *               1-px lobes between adjacent discs gave a "wave of
 *               1-px hops" texture rather than a smooth curve.
 *   v0.1.9:     128×40 rect with r=7 discs and matching halo bump.
 *               Bigger radius makes each lobe a real arc; wider rect
 *               grows text capacity to ~14 chars × 3 lines. Side
 *               margins shrunk into care-icon territory (lobes at
 *               x≈29..43 vs icons at x∈[4,52] y∈[22,70]); since the
 *               bubble draws AFTER chrome it paper-fills over the
 *               icons in the overlap. Intended — bubble focus +
 *               icons fade out behind it; they reappear when
 *               dismissed. */
static const int BUBBLE_X0    = 36;
static const int BUBBLE_X1    = 164;     /* exclusive — 128 px wide rect */
static const int BUBBLE_Y0    = 38;
static const int BUBBLE_Y1    = 78;      /* exclusive — 40 px tall rect */
static const int BUBBLE_TAIL_W = 6;      /* base of the down-pointing tail */
static const int BUBBLE_TAIL_H = 6;      /* height; tip lands at Y1 + H - 1 */
static const int HIT_PAD       = 6;      /* enlarge tap target on every side */

/* Text layout — fixed-width font8x8, so chars-per-line = pixels/8.
 * The renderer word-wraps `text` greedily on spaces (honouring '\n'
 * as a hard break) and vertically centres the resulting block inside
 * the bubble's interior rect. Lines past the page budget overflow
 * to the next page (caller bumps pet_thought_t.page on tap). */
static const int GLYPH_W   = 8;
static const int GLYPH_H   = 8;
static const int LINE_GAP  = 2;
static const int LINE_H    = GLYPH_H + LINE_GAP;       /* 10 */
static const int TEXT_PAD_X = 2;                       /* keep 2 px clear of the L/R border */
static const int TEXT_PAD_Y = 2;                       /* keep 2 px clear of the T/B border */
static const int MAX_TEXT_LINES = 3;                   /* 3 × 10 = 30 px fits in 40-px box */
static const int MAX_TOTAL_LINES = 16;                 /* upper bound across all pages (~5 pages worth) */
static const int ACTION_ICON_RESERVE_PX = 10;          /* bottom row when action_kind != NONE */

/* Overflow latch — written by blit_text_wrapped, read by callers
 * via thought_has_more(). Module-scope so thought_render can stay
 * `void` (callers don't need a return path; just consult the latch
 * after the call). */
static bool s_has_more = false;

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

/* ─── Cloud-shape geometry (THOUGHT_STYLE_THOUGHT only) ──────────────
 *
 * The cloud is modelled as the UNION of (1) the central bubble rect
 * and (2) a ring of small filled discs around its perimeter. Each
 * disc protrudes ~r pixels outward; adjacent discs overlap slightly
 * so the union outline reads as a continuous scalloped curve. The
 * bottom-centre has a gap (no disc near x=100) so the trailing
 * thought-dots can emerge cleanly from the cloud's underside.
 *
 * Render is brute-force: for every pixel in the cloud bounding box
 * we test "inside cloud?" via inside_cloud(); the interior fills
 * with paper-white and the boundary (interior pixel whose 4-neighbour
 * is exterior) draws black. ~50×50 px × ~20 disc checks per pixel
 * is a few hundred kilo-ops per render — negligible on ESP32, and
 * we only re-render when the bubble actually shows. */

/* Panel coords exceed int8_t (max 127) for the bubble's right + bottom
 * disc rows — use int16_t for cx/cy. r is large enough to need int8_t. */
struct cloud_disc { int16_t cx, cy; int8_t r; };

/* Disc positions tuned for the 128×40 central rect, r=7 lobes. Top &
 * bottom rows have 11/10 discs at 12 px spacing; sides have 3 each.
 *
 * Disc spacing math: two r=7 discs whose centres sit 12 px apart
 * overlap by 2 px at their nearest edges (sum-of-radii = 14, gap =
 * −2). The chord between two adjacent disc tops dips ~3.4 px below
 * the disc-top line (sqrt(49 − 36) = 3.6) — small enough to read as
 * a single curving cloud edge rather than discrete bumps. Earlier
 * v0.1.9 used 20 px spacing; at that distance the discs *kissed*
 * but didn't overlap, leaving a 7 px groove between every lobe that
 * read as a knobbly hill chain. 12 px spacing fills the gaps.
 *
 * Bottom row keeps an 18 px gap around x=100 so the trailing
 * thought-dots can emerge cleanly without colliding with a lobe.
 *
 * No vertical stagger now — at this overlap density the rows blend
 * cleanly without it; staggering creates asymmetric lobes that read
 * worse than they help. */
static const struct cloud_disc CLOUD_DISCS[] = {
    /* Top row — 11 bumps along y=37. */
    { 40, 37, 7 }, { 52, 37, 7 }, { 64, 37, 7 }, { 76, 37, 7 },
    { 88, 37, 7 }, {100, 37, 7 }, {112, 37, 7 }, {124, 37, 7 },
    {136, 37, 7 }, {148, 37, 7 }, {160, 37, 7 },
    /* Right side — 3 discs centred on the rect's vertical span. */
    {164, 50, 7 }, {164, 58, 7 }, {164, 66, 7 },
    /* Bottom row — 11 bumps along y=79, no gap. Earlier we left a
     * disc out at x=100 so the trailing thought-dots could emerge,
     * but at 12 px spacing the dots fit cleanly between the lobes
     * and the gap looked like a missing tooth in the otherwise
     * continuous border. */
    {160, 79, 7 }, {148, 79, 7 }, {136, 79, 7 }, {124, 79, 7 },
    {112, 79, 7 }, {100, 79, 7 }, { 88, 79, 7 }, { 76, 79, 7 },
    { 64, 79, 7 }, { 52, 79, 7 }, { 40, 79, 7 },
    /* Left side. */
    { 36, 66, 7 }, { 36, 58, 7 }, { 36, 50, 7 },
};
static const int CLOUD_DISC_COUNT =
    (int)(sizeof(CLOUD_DISCS) / sizeof(CLOUD_DISCS[0]));

/* True iff (x, y) is inside the union (central rect ∪ all discs).
 * Used by both the fill pass and the border-detection pass. */
static inline bool inside_cloud(int x, int y) {
    if (x >= BUBBLE_X0 && x < BUBBLE_X1 &&
        y >= BUBBLE_Y0 && y < BUBBLE_Y1) return true;
    for (int i = 0; i < CLOUD_DISC_COUNT; i++) {
        const int dx = x - CLOUD_DISCS[i].cx;
        const int dy = y - CLOUD_DISCS[i].cy;
        const int r  = CLOUD_DISCS[i].r;
        if (dx * dx + dy * dy <= r * r) return true;
    }
    return false;
}

/* Paint the cloud silhouette: paper-white interior, black scalloped
 * border. Caller is responsible for the trailing tail dots, which
 * sit outside the cloud's bounding box and have their own draw
 * code. */
static void draw_cloud_silhouette(uint8_t *dst, size_t dst_w, size_t dst_h) {
    /* Inflate the rect by max disc radius (7) + 1 so the fill/border
     * pass covers every lobe pixel. The previous +8 was correct for
     * r=4 discs whose centres sat 4 px outside the rect; with r=7
     * centres the lobe edge can reach 14 px past the rect. */
    const int xb0 = BUBBLE_X0 - 14;
    const int xb1 = BUBBLE_X1 + 14;
    const int yb0 = BUBBLE_Y0 - 14;
    const int yb1 = BUBBLE_Y1 + 14;
    const size_t stride = (dst_w + 7) >> 3;

    /* Phase 1: fill the interior with paper. */
    for (int y = yb0; y < yb1; y++) {
        if (y < 0 || y >= (int)dst_h) continue;
        for (int x = xb0; x < xb1; x++) {
            if (x < 0 || x >= (int)dst_w) continue;
            if (inside_cloud(x, y)) pixel_white(dst, stride, x, y);
        }
    }
    /* Phase 2: trace the boundary — interior pixel with at least one
     * 4-neighbour outside the cloud. */
    for (int y = yb0; y < yb1; y++) {
        if (y < 0 || y >= (int)dst_h) continue;
        for (int x = xb0; x < xb1; x++) {
            if (x < 0 || x >= (int)dst_w) continue;
            if (!inside_cloud(x, y)) continue;
            if (!inside_cloud(x - 1, y) || !inside_cloud(x + 1, y) ||
                !inside_cloud(x, y - 1) || !inside_cloud(x, y + 1)) {
                pixel_black(dst, stride, x, y);
            }
        }
    }
}

/* ─── Inline action icons (CARE_EVENT only) ─────────────────────────
 *
 * 8×8 1bpp glyphs, one per event_kind_t the bubble might carry.
 * Each row is a byte; bit 7 = leftmost column, bit set = ink.
 * Rendered at fixed bottom-centre of the bubble interior when
 * action_kind == CARE_EVENT (the only kind today that actually
 * needs a "do X" affordance — passive bubbles get no icon, and
 * TALK_SEED / NAVIGATE land on this path only via future producers).
 * Icons are intentionally chunky at 8×8 — readable at the panel's
 * 1:1 device-pixel scale without antialiasing tricks the 1-bit
 * pipeline can't represent. */
typedef struct { event_kind_t k; const uint8_t rows[8]; } action_icon_t;

static const action_icon_t ACTION_ICONS[] = {
    /* SLEPT — uppercase "Z" (universal "asleep"). */
    { EVENT_SLEPT,    { 0xFC, 0x04, 0x08, 0x10, 0x20, 0x40, 0xFC, 0x00 } },
    /* WOKE — upward arrow (rising / waking). */
    { EVENT_WOKE,     { 0x10, 0x38, 0x54, 0x10, 0x10, 0x10, 0x10, 0x10 } },
    /* FED — bowl with food. */
    { EVENT_FED,      { 0x00, 0xFE, 0x82, 0x82, 0x44, 0x7C, 0x38, 0x00 } },
    /* PLAYED — ball with stitching. */
    { EVENT_PLAYED,   { 0x78, 0x84, 0xB4, 0xB4, 0x84, 0x78, 0x00, 0x00 } },
    /* COMFORTED — heart. */
    { EVENT_COMFORTED,{ 0x6C, 0xFE, 0xFE, 0x7C, 0x38, 0x10, 0x00, 0x00 } },
    /* CHEERED — exclamation. */
    { EVENT_CHEERED,  { 0x10, 0x10, 0x10, 0x10, 0x10, 0x00, 0x10, 0x00 } },
    /* HUGGED — "X" (crossed arms). */
    { EVENT_HUGGED,   { 0x81, 0x42, 0x24, 0x18, 0x18, 0x24, 0x42, 0x81 } },
    /* TALKED — speech wave (mouth). */
    { EVENT_TALKED,   { 0x00, 0x7C, 0x82, 0x82, 0xAA, 0x7C, 0x00, 0x00 } },
    /* TAPPED — finger pointing down (tap). */
    { EVENT_TAPPED,   { 0x10, 0x10, 0x50, 0x54, 0x54, 0x7C, 0x38, 0x38 } },
};
static const int ACTION_ICON_COUNT =
    (int)(sizeof(ACTION_ICONS) / sizeof(ACTION_ICONS[0]));

static const uint8_t *icon_for_event(event_kind_t k) {
    for (int i = 0; i < ACTION_ICON_COUNT; i++) {
        if (ACTION_ICONS[i].k == k) return ACTION_ICONS[i].rows;
    }
    return NULL;
}

/* Blit an 8×8 icon at the given top-left, set bits draw ink. */
static void blit_icon_8x8(uint8_t *dst, size_t dst_w, size_t dst_h,
                          const uint8_t *rows, int x_left, int y_top) {
    const size_t stride = (dst_w + 7) >> 3;
    for (int row = 0; row < 8; row++) {
        const uint8_t bits = rows[row];
        for (int col = 0; col < 8; col++) {
            /* bit 7 = leftmost column (MSB-first within byte). */
            if (!((bits >> (7 - col)) & 1)) continue;
            const int px = x_left + col;
            const int py = y_top + row;
            if (px < 0 || py < 0 ||
                px >= (int)dst_w || py >= (int)dst_h) continue;
            pixel_black(dst, stride, px, py);
        }
    }
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

/* Word-wrap + vertically-centre a paged slice of a single string
 * inside the bubble's interior rect [box_y0, box_y1).
 *
 *   - Greedy break on space; '\n' is a hard break.
 *   - Fixed-width font → max_chars per line = interior_w / GLYPH_W.
 *   - Wraps the FULL string into up to MAX_TOTAL_LINES line spans,
 *     then renders the slice [page * lines_per_page,
 *     (page+1) * lines_per_page).
 *   - `reserved_bottom_px` shrinks the interior height (used by the
 *     action-icon row so text doesn't overlap the icon).
 *   - When the page-end exposes fewer lines than wrapped total, the
 *     last visible line gets a trailing "..." (truncating the line
 *     content if needed to fit) and the module-scope s_has_more
 *     latch is raised — tap handlers consult thought_has_more() to
 *     decide between "advance page" and "dismiss".
 *
 * Returns silently if `text` is empty; the latch is cleared at the
 * top so a previous call's overflow doesn't leak into the next.
 */
static void blit_text_wrapped(uint8_t *dst, size_t dst_w, size_t dst_h,
                              const char *text,
                              int box_x0, int box_y0,
                              int box_x1, int box_y1,
                              int page, int reserved_bottom_px) {
    s_has_more = false;
    if (!text || !*text) return;
    const int interior_w = (box_x1 - TEXT_PAD_X) - (box_x0 + TEXT_PAD_X);
    if (interior_w < GLYPH_W) return;
    const int max_chars_int = interior_w / GLYPH_W;
    /* Clamp to a sane on-stack line buffer so single long words still
     * blit (truncated) rather than overflowing. 24 chars × 8 px = 192
     * px is wider than the panel, well past any bubble interior. */
    enum { LINE_BUF = 24 };
    const int max_chars = max_chars_int < (LINE_BUF - 1)
        ? max_chars_int : (LINE_BUF - 1);

    /* Phase 1: scan the FULL string into line spans (not just the
     * current page's worth), so we can compute total_pages + the
     * has_more latch correctly. */
    struct { int start; int len; } lines[MAX_TOTAL_LINES];
    int total_lines = 0;
    const int total = (int)strlen(text);
    int i = 0;
    while (i < total && total_lines < MAX_TOTAL_LINES) {
        while (i < total && text[i] == ' ') i++;
        if (i >= total) break;
        const int line_start = i;
        int last_space_at = -1;
        int j = i;
        while (j < total && text[j] != '\n' && (j - line_start) < max_chars) {
            if (text[j] == ' ') last_space_at = j;
            j++;
        }
        int line_end;
        if (j >= total || text[j] == '\n') {
            line_end = j;
        } else if (last_space_at > line_start) {
            line_end = last_space_at;
        } else {
            /* Single word longer than the line — hard-break. */
            line_end = j;
        }
        lines[total_lines].start = line_start;
        lines[total_lines].len   = line_end - line_start;
        total_lines++;
        i = line_end;
        if (i < total && (text[i] == ' ' || text[i] == '\n')) i++;
    }
    if (total_lines == 0) return;

    /* Phase 2: compute the page slice. Interior height after the
     * action-icon reserve dictates how many lines fit per page.
     * Clamp to MAX_TEXT_LINES so the bubble never tries to cram
     * more than its hardware limit. */
    const int interior_top = box_y0 + TEXT_PAD_Y;
    const int interior_bot = box_y1 - TEXT_PAD_Y - reserved_bottom_px;
    const int interior_h   = interior_bot - interior_top;
    if (interior_h < GLYPH_H) return;
    int lines_per_page = (interior_h + LINE_GAP) / LINE_H;
    if (lines_per_page < 1) lines_per_page = 1;
    if (lines_per_page > MAX_TEXT_LINES) lines_per_page = MAX_TEXT_LINES;

    /* Clamp page to [0, total_pages-1]. Caller may have bumped it
     * past the end on a stale tap; degrade gracefully to the last
     * page rather than rendering an empty bubble. */
    const int total_pages =
        (total_lines + lines_per_page - 1) / lines_per_page;
    int p = page;
    if (p < 0) p = 0;
    if (p >= total_pages) p = total_pages - 1;

    const int slice_start = p * lines_per_page;
    int slice_end = slice_start + lines_per_page;
    if (slice_end > total_lines) slice_end = total_lines;
    const int n_visible = slice_end - slice_start;
    if (n_visible <= 0) return;

    /* There's more content past this page iff we're not on the last
     * page. The last visible line on a non-last page gets a "..."
     * suffix as the overflow cue. */
    const bool more = (p < total_pages - 1);
    s_has_more = more;

    /* Vertical centring of the visible slice inside the (shrunk)
     * interior — same math as the original, just over n_visible
     * rather than total_lines. */
    const int block_h = n_visible * LINE_H - LINE_GAP;
    int start_y = interior_top + (interior_h - block_h) / 2;
    if (start_y < interior_top) start_y = interior_top;

    /* Phase 3: blit each visible line. For the last line on a
     * not-last page, splice "..." onto the end — truncating the
     * payload chars if needed so the total fits the line budget. */
    static const char ELLIPSIS[] = "...";
    const int ELLIPSIS_LEN = (int)(sizeof(ELLIPSIS) - 1);
    const int cx = (box_x0 + box_x1) / 2;
    for (int li = 0; li < n_visible; li++) {
        char tmp[LINE_BUF];
        const int span_idx = slice_start + li;
        int len = lines[span_idx].len;
        if (len > LINE_BUF - 1) len = LINE_BUF - 1;
        memcpy(tmp, text + lines[span_idx].start, (size_t)len);
        const bool is_last_visible = (li == n_visible - 1);
        if (is_last_visible && more) {
            /* Ensure "..." fits — trim payload + any trailing space. */
            int keep = max_chars - ELLIPSIS_LEN;
            if (keep < 0) keep = 0;
            if (len > keep) len = keep;
            while (len > 0 && tmp[len - 1] == ' ') len--;
            int copy_n = ELLIPSIS_LEN;
            if (copy_n > LINE_BUF - 1 - len) copy_n = LINE_BUF - 1 - len;
            memcpy(tmp + len, ELLIPSIS, (size_t)copy_n);
            len += copy_n;
        }
        tmp[len] = '\0';
        const int y_top = start_y + li * LINE_H;
        blit_text_centered(dst, dst_w, dst_h, tmp, cx, y_top);
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
        out->text          = "zzz...\ntap wake";
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
        out->text          = "sleepy...\ntap sleep";
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
    /* Each entry is short enough (≤20 chars incl. the '\n') that the
     * renderer's word-wrap either keeps the hard break or naturally
     * stacks the phrase on two lines. Single-line moods omit the '\n'
     * so they centre as one row in the bubble's vertical axis. */
    switch (m) {
        case MOOD_SLEEPING:  out->text = "zzz...\nshh";          break;
        case MOOD_HUNGRY:    out->text = "hungry...\ngot snack?"; break;
        case MOOD_TIRED:     out->text = "yawn...\nso sleepy";   break;
        case MOOD_LONELY:    out->text = "miss u...\nwhere ru?"; break;
        case MOOD_PLAYFUL:   out->text = "play!\nzoomies!";      break;
        case MOOD_CURIOUS:   out->text = "hmm...\nwhat's up?";   break;
        case MOOD_CONTENT:   out->text = "hi :)\nall good";      break;
        case MOOD_SURPRISED: out->text = "oh!\nwhat was?";       break;
        default:             out->text = "hi :)";                break;
    }
}

extern "C" void thought_render(uint8_t *dst, size_t dst_w, size_t dst_h,
                               const pet_thought_t *thought,
                               thought_hit_rect_t *out_hit) {
    if (!dst || !thought) return;

    /* Silhouette: speech bubbles get the classic chamfered rectangle;
     * thought bubbles get the scalloped cloud outline (paper-white
     * fill across the union of the central rect + the perimeter
     * discs, then the boundary traced in black). */
    if (thought->style == THOUGHT_STYLE_SPOKEN) {
        fill_rect_white(dst, dst_w, dst_h,
                        BUBBLE_X0, BUBBLE_Y0, BUBBLE_X1, BUBBLE_Y1);
        /* 1-px border with chamfered corners. */
        draw_hline_black(dst, dst_w, dst_h, BUBBLE_X0 + 1, BUBBLE_X1 - 1, BUBBLE_Y0);
        draw_hline_black(dst, dst_w, dst_h, BUBBLE_X0 + 1, BUBBLE_X1 - 1, BUBBLE_Y1 - 1);
        draw_vline_black(dst, dst_w, dst_h, BUBBLE_X0, BUBBLE_Y0 + 1, BUBBLE_Y1 - 1);
        draw_vline_black(dst, dst_w, dst_h, BUBBLE_X1 - 1, BUBBLE_Y0 + 1, BUBBLE_Y1 - 1);
    } else {
        draw_cloud_silhouette(dst, dst_w, dst_h);
    }

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

    /* Action icon (CARE_EVENT only) — sits in a reserved row along
     * the bottom of the interior, centred horizontally. Passive
     * bubbles (action_kind == NONE) skip the icon and the reserve,
     * giving them the full 3-line text budget. */
    const uint8_t *icon = NULL;
    if (thought->action_kind == THOUGHT_ACTION_CARE_EVENT) {
        icon = icon_for_event(thought->action_event);
    }
    const int reserved_bottom = icon ? ACTION_ICON_RESERVE_PX : 0;

    /* Body text — word-wrapped, vertically centred, paginated. The
     * helper raises the module-scope `s_has_more` latch when this
     * page exposes fewer than the wrapped total; the renderer then
     * trails the last visible line with "..." so the kid sees the
     * overflow cue, and main.cpp's tap handler bumps `page` or
     * dismisses based on thought_has_more(). */
    blit_text_wrapped(dst, dst_w, dst_h, thought->text,
                      BUBBLE_X0, BUBBLE_Y0, BUBBLE_X1, BUBBLE_Y1,
                      thought->page, reserved_bottom);

    if (icon) {
        /* Drop the icon centred on the bubble's vertical axis, 1 px
         * above the bottom interior edge so it doesn't kiss the
         * scalloped border. */
        const int icon_x = (BUBBLE_X0 + BUBBLE_X1) / 2 - 4;
        const int icon_y = BUBBLE_Y1 - TEXT_PAD_Y - 8;
        blit_icon_8x8(dst, dst_w, dst_h, icon, icon_x, icon_y);
    }

    /* Touch hit rectangle — enlarge from the visible shape by
     * HIT_PAD on every side, including past the tail tip so a tap
     * just under the bubble still registers. THOUGHT-style adds a
     * couple of extra px because the scalloped silhouette extends
     * past the rect by the disc radius. */
    if (out_hit) {
        /* Scalloped silhouette pokes ~7 px past the rect on every side
         * (one disc radius); add that to the hit pad so a tap on a
         * lobe still registers. SPOKEN bubbles are rect-only so
         * scallop = 0. */
        const int scallop = (thought->style == THOUGHT_STYLE_THOUGHT) ? 7 : 0;
        out_hit->x0 = BUBBLE_X0 - HIT_PAD - scallop;
        out_hit->y0 = BUBBLE_Y0 - HIT_PAD - scallop;
        out_hit->x1 = BUBBLE_X1 + HIT_PAD + scallop;
        out_hit->y1 = BUBBLE_Y1 + BUBBLE_TAIL_H + HIT_PAD + scallop;
    }
}

extern "C" bool thought_hit_contains(const thought_hit_rect_t *r,
                                     int x, int y) {
    if (!r) return false;
    return x >= r->x0 && x < r->x1 && y >= r->y0 && y < r->y1;
}

extern "C" bool thought_has_more(void) {
    return s_has_more;
}
