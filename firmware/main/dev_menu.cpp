#include "dev_menu.h"

#include <stdio.h>
#include <string.h>
#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "epd_ui.h"
#include "font8x8.h"
#include "nvs_creds.h"
#include "model_prefs.h"
#include "ota_channel.h"
#include "ota_update.h"
#include "sprite_cache.h"   /* ui-icons-a cells for stat rows + tile icons (design/30) */
#include "sprite_fetch.h"   /* lazy on-demand fetch of menu/tile icons */
#include "compositor.h"     /* downsample native 80×80 → 48 */

static const char *TAG = "dev_menu";

namespace dev_menu {

/* Inactivity timeout: any non-Live mode snaps back to Live after this
 * many microseconds without a PWR press. Long enough to read a full
 * diagnostics screen + cross-check details with notes; touch or a
 * fresh PWR press still exits / advances earlier. */
static constexpr int64_t INACTIVITY_US = 60LL * 1000 * 1000;

static Mode                 s_mode = Mode::Live;
static int64_t              s_entered_mode_us = 0;
/* Set by request_advance() (called from main.cpp on PWR taps) and
 * cleared by tick() after it advances the wheel. */
static std::atomic<bool>    s_press_pending{false};
static bool                 s_started = false;
/* The e-paper driver handle, captured at init(). dispatch_touch needs
 * it to re-render in place (modal pushes, toggle relabels) and tick()
 * uses it too — both run on the main loop's task, so the panel is
 * only ever touched from one thread (unlike the LVGL era, where a
 * 33 Hz dispatcher task shared the panel behind a mutex). */
static epaper_driver_display *s_epd = nullptr;

/* Most recent menu data — cached so dispatch-time mode pushes
 * (e.g. SwitchWifi → render WifiModal) can re-render without being
 * re-handed the params. tick() refreshes these on every pass. */
static char                 s_pet_name[40] = {};
static char                 s_version[40]  = {};
static char                 s_ip_str[24]   = {};
static char                 s_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};
static int                  s_net_phase    = 0;
static int                  s_batt_pct     = 0;
static char                 s_pet_status[80] = {};
/* Numeric pet stats [0,100] for MenuP1's icon + bar rows; <0 = unknown. */
static int                  s_happy = -1;
static int                  s_full  = -1;
static int                  s_energy = -1;

/* ─── ui-v1 icon cache (design/30) ──────────────────────────────────
 *
 * The home chrome already fetches + caches four ui-v1 cells as 48×48
 * 2-plane (ink+mask) blobs under sprite_cache keys
 * "<key>_icon_48x48_{ink,mask}" (main.cpp CARE_ICON_KEYS). The dev menu
 * reuses those for the stat rows. We lazily load each requested key once
 * into small PSRAM buffers; a miss (offline before the first fetch) just
 * means the row falls back to a one-letter tag. */
static constexpr int ICON_DIM   = 48;                       /* cached size */
static constexpr int ICON_STRIDE = (ICON_DIM + 7) / 8;      /* 6 bytes/row */
static constexpr int ICON_BYTES  = ICON_STRIDE * ICON_DIM;  /* 288 */
/* Native ui cell on the sheet (server delivers 80×80 2-plane); downsampled
 * to ICON_DIM. Must match main.cpp UI_CELL_NATIVE_*. */
static constexpr int UI_NATIVE_DIM   = 80;
static constexpr int UI_NATIVE_BYTES = (UI_NATIVE_DIM / 8) * UI_NATIVE_DIM;  /* 800 */

struct IconSlot {
    char     key[24];
    uint8_t *ink;
    uint8_t *mask;
    bool     ok;
};
/* Enough for the care/stat icons + every distinct menu-tile glyph. */
static constexpr int ICON_SLOTS = 18;
static IconSlot s_icon_cache[ICON_SLOTS] = {};
static int      s_icon_n = 0;

/* Return the cached icon for `key`, loading it from sprite_cache on first
 * use. Returns nullptr (and the caller draws a text fallback) on a cache
 * miss or alloc failure. Idempotent + cheap after the first hit. */
static const IconSlot *get_icon(const char *key) {
    for (int i = 0; i < s_icon_n; i++)
        if (strcmp(s_icon_cache[i].key, key) == 0)
            return s_icon_cache[i].ok ? &s_icon_cache[i] : nullptr;
    if (s_icon_n >= ICON_SLOTS) return nullptr;

    IconSlot &slot = s_icon_cache[s_icon_n++];
    snprintf(slot.key, sizeof(slot.key), "%s", key);
    slot.ok = false;
    slot.ink  = (uint8_t *)heap_caps_malloc(ICON_BYTES, MALLOC_CAP_SPIRAM);
    slot.mask = (uint8_t *)heap_caps_malloc(ICON_BYTES, MALLOC_CAP_SPIRAM);
    if (!slot.ink || !slot.mask) return nullptr;

    char ink_s[40], mask_s[40];
    snprintf(ink_s,  sizeof(ink_s),  "%s_icon_%dx%d_ink",  key, ICON_DIM, ICON_DIM);
    snprintf(mask_s, sizeof(mask_s), "%s_icon_%dx%d_mask", key, ICON_DIM, ICON_DIM);
    size_t gi = 0, gm = 0;
    if (sprite_cache::load(MOCHI_UI_SHEET, ink_s,  slot.ink,  ICON_BYTES, &gi) && gi == ICON_BYTES &&
        sprite_cache::load(MOCHI_UI_SHEET, mask_s, slot.mask, ICON_BYTES, &gm) && gm == ICON_BYTES) {
        slot.ok = true;
        return &slot;
    }
    /* Cache miss → fetch the native 80×80 cell from the sheet, downsample to
     * ICON_DIM, and cache it. Blocking, but only the first time a given icon
     * is needed (when a menu page that uses it is first opened); the care/
     * stat icons are already warmed at boot by main.cpp. Offline → the fetch
     * fails → the caller falls back to a one-letter tag. */
    uint8_t *ni = (uint8_t *)heap_caps_malloc(UI_NATIVE_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *nm = (uint8_t *)heap_caps_malloc(UI_NATIVE_BYTES, MALLOC_CAP_SPIRAM);
    if (ni && nm) {
        char url[128];
        snprintf(url, sizeof(url),
            "https://mochi.val.run/devsprite/cell/" MOCHI_UI_SHEET "/%s", key);
        uint16_t w = 0, h = 0;
        uint32_t ms = 0;
        if (sprite_fetch_cell(url, ni, nm, UI_NATIVE_BYTES, &w, &h, &ms) &&
            w == UI_NATIVE_DIM && h == UI_NATIVE_DIM) {
            memset(slot.ink,  0xFF, ICON_BYTES);
            memset(slot.mask, 0xFF, ICON_BYTES);
            compositor::downsample_plane(slot.ink,  ICON_DIM, ICON_DIM, ni, UI_NATIVE_DIM, UI_NATIVE_DIM);
            compositor::downsample_plane(slot.mask, ICON_DIM, ICON_DIM, nm, UI_NATIVE_DIM, UI_NATIVE_DIM);
            if (sprite_cache::store(MOCHI_UI_SHEET, ink_s,  slot.ink,  ICON_BYTES))
                sprite_cache::store(MOCHI_UI_SHEET, mask_s, slot.mask, ICON_BYTES);
            slot.ok = true;
        }
    }
    heap_caps_free(ni);
    heap_caps_free(nm);
    return slot.ok ? &slot : nullptr;
}

/* Nearest-neighbour blit of a cached 48×48 2-plane icon into an out×out
 * box at (x,y), drawing only its black ink (the tile/panel is white, so
 * paper pixels need no write). Matches compositor.cpp's bit convention:
 * MSB-first within a byte, mask bit 1 = transparent, ink bit 0 = black. */
static void blit_icon(epaper_driver_display *epd, int x, int y, int out,
                      const IconSlot *ic) {
    if (!ic) return;
    for (int oy = 0; oy < out; oy++) {
        const int sy = oy * ICON_DIM / out;
        for (int ox = 0; ox < out; ox++) {
            const int sx = ox * ICON_DIM / out;
            const int off = sy * ICON_STRIDE + (sx >> 3);
            const uint8_t bit = (uint8_t)(1u << (7 - (sx & 7)));
            if (ic->mask[off] & bit) continue;            /* transparent */
            if ((ic->ink[off] & bit) == 0)                /* ink 0 = black */
                epd->EPD_DrawColorPixel(x + ox, y + oy, DRIVER_COLOR_BLACK);
        }
    }
}

/* ─── Tile registry + hit-testing ──────────────────────────────────
 *
 * Hand-rolled (this is the rollback from the LVGL widget tree — see
 * dev_menu.h history). Each render lays the page out as a grid of
 * tiles and records their pixel rects here; dispatch_touch resolves
 * an (x, y) tap to the rect it lands in. Buttons live only while
 * their owning screen is up; clear_buttons() runs before each render
 * so a stale rect can't leak into the next page's dispatch.
 *
 * Touch sizing: the panel is 200 px across ~27.7 mm (≈7.2 px/mm), so
 * the old 24 px-tall full-width rows were only ~3.3 mm — well under a
 * fingertip. We lay actions out as a 2-column grid (cells ≈94×56 px,
 * ~13×8 mm) so targets are ~2.7× the area; long-text pages (Wi-Fi
 * list, model picker) use a single wide column instead. */
struct Button {
    int x, y, w, h;
    TouchResult action;
};
/* Headroom for the riskiest page (5 tiles) plus the Wi-Fi modal's
 * stored networks (capped to what fits on the panel). */
static constexpr int MAX_BUTTONS = 10;
static Button   s_buttons[MAX_BUTTONS] = {};
static int      s_button_count = 0;
/* SSID payload, parallel to s_buttons — only the WifiModal tiles use
 * it. Picked into s_picked_ssid when a WifiSwitch tile is tapped. */
static char     s_button_ssid[MAX_BUTTONS][MOCHI_WIFI_SSID_MAX + 1] = {};
static char     s_picked_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};

/* Layout constants. 200×200 panel; ASCII 8×8 font (scale 1 = 8 px). */
static constexpr int MARGIN     = 4;
static constexpr int GAP        = 4;
static constexpr int FULL_W     = MOCHI_EPD_WIDTH - 2 * MARGIN;   /* 192 */
static constexpr int CELL_W2    = (FULL_W - GAP) / 2;             /* 94  */
static constexpr int MAX_CELL_H = 72;

/* When non-None, the tile whose action matches gets its value pill
 * drawn inverted for one frame — the toggle "flash" acknowledgement.
 * Set by render_mode(); read by draw_tile via layout_tiles. */
static TouchResult s_flash = TouchResult::None;

/* A single tappable cell. Actions render filled/inverted ("press and
 * leave"); toggles render outlined with the current value in a boxed
 * pill ("shows + flips state, stays put"). value/ssid point at buffers
 * owned by the caller and only need to live across the render call. */
struct Tile {
    const char *label;
    TouchResult action;
    bool        toggle;
    const char *value;   /* toggle pill text; nullptr for actions */
    const char *ssid;    /* WifiSwitch payload; nullptr otherwise */
    const char *icon;    /* ui-icons-a key for the tile glyph; nullptr = text-only */
};

static void clear_buttons(void) {
    s_button_count = 0;
    for (int i = 0; i < MAX_BUTTONS; i++) {
        s_buttons[i] = {};
        s_button_ssid[i][0] = '\0';
    }
}

const char *picked_ssid(void) { return s_picked_ssid; }

void init(epaper_driver_display *epd) {
    s_epd = epd;
    s_mode = Mode::Live;
    s_entered_mode_us = 0;
    s_press_pending.store(false, std::memory_order_release);
    clear_buttons();
    s_started = true;
}

void request_advance(void) {
    s_press_pending.store(true, std::memory_order_release);
}

Mode current(void) { return s_mode; }
bool active(void) { return s_mode != Mode::Live; }

void exit_to_live(void) {
    if (s_mode != Mode::Live) {
        ESP_LOGI(TAG, "exit → live");
        s_mode = Mode::Live;
        clear_buttons();
        /* main.cpp's render_resting redraws the pet on the next tick;
         * we don't touch the panel here. */
    }
}

/* Advance (PWR×2 from anywhere): pages deeper into riskier territory.
 *
 *   Live      → MenuP1 (entry — kid-facing main page)
 *   MenuP1    → MenuP2 (settings — Switch WiFi / OpenAI key / models)
 *   MenuP2    → MenuP3 (destructive — Update / Channel / Add / Forget / Re-pair)
 *   MenuP3    → MenuP1 (wrap back to the safe page)
 *   WifiModal → MenuP2 (a modal isn't a "page" — bounce to its parent)
 *   ModelsModal → MenuP2
 *
 * PWR×1 from anywhere non-Live is handled by exit_to_live() up in
 * main.cpp — it never enters this table, so "single tap escapes" is
 * absolute regardless of which page is up. */
static Mode advance(Mode m) {
    switch (m) {
        case Mode::Live:        return Mode::MenuP1;
        case Mode::MenuP1:      return Mode::MenuP2;
        case Mode::MenuP2:      return Mode::MenuP3;
        case Mode::MenuP3:      return Mode::MenuP1;
        case Mode::WifiModal:   return Mode::MenuP2;
        case Mode::ModelsModal: return Mode::MenuP2;
        default:                return Mode::Live;
    }
}

static const char *phase_label(int phase) {
    /* NetPhase enum lives in main.cpp — passed through as int to keep
     * dev_menu free of that header. 0 Connecting, 1 Online, 2 Offline. */
    switch (phase) {
        case 0:  return "connecting";
        case 1:  return "online";
        case 2:  return "offline";
        default: return "?";
    }
}

/* ─── Low-level draw helpers ────────────────────────────────────────
 *
 * These write straight into the e-paper driver's internal buffer via
 * EPD_DrawColorPixel (the slow per-pixel path — fine for a static menu
 * frame, not for animation). epd_ui::draw_text is opaque (paints a
 * white background under glyphs), so for white-on-black tile labels we
 * roll our own transparent glyph blit that only sets the ink pixels. */

static void fill_rect(epaper_driver_display *epd, int x, int y, int w, int h,
                      uint8_t color) {
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            epd->EPD_DrawColorPixel(x + dx, y + dy, color);
}

static void draw_rect_border(epaper_driver_display *epd, int x, int y,
                             int w, int h) {
    for (int dx = 0; dx < w; dx++) {
        epd->EPD_DrawColorPixel(x + dx, y, DRIVER_COLOR_BLACK);
        epd->EPD_DrawColorPixel(x + dx, y + h - 1, DRIVER_COLOR_BLACK);
    }
    for (int dy = 0; dy < h; dy++) {
        epd->EPD_DrawColorPixel(x, y + dy, DRIVER_COLOR_BLACK);
        epd->EPD_DrawColorPixel(x + w - 1, y + dy, DRIVER_COLOR_BLACK);
    }
}

/* Transparent glyph blit: only the set (ink) bits are drawn, in
 * `ink`; cleared bits are left as-is (so the tile fill shows through).
 * Clips at the right panel edge. */
static void draw_glyphs(epaper_driver_display *epd, int x, int y, int scale,
                        const char *text, uint8_t ink) {
    int cur = x;
    for (const char *p = text; *p; p++) {
        if (cur + 8 * scale > MOCHI_EPD_WIDTH) break;
        const uint8_t *g = font8x8_glyph(*p);
        for (int row = 0; row < 8; row++) {
            const uint8_t bits = g[row];
            for (int col = 0; col < 8; col++) {
                if (!((bits >> col) & 1)) continue;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        epd->EPD_DrawColorPixel(cur + col * scale + dx,
                                                y + row * scale + dy, ink);
            }
        }
        cur += 8 * scale;
    }
}

/* Centre `text` horizontally within [x, x+w) at vertical pixel y. */
static void draw_glyphs_centered_in(epaper_driver_display *epd, int x, int w,
                                    int y, int scale, const char *text,
                                    uint8_t ink) {
    const int tw = (int)strlen(text) * 8 * scale;
    int tx = x + (w - tw) / 2;
    if (tx < x + 1) tx = x + 1;
    draw_glyphs(epd, tx, y, scale, text, ink);
}

/* ─── Tiles ─────────────────────────────────────────────────────────*/

static void draw_tile(epaper_driver_display *epd, int x, int y, int w, int h,
                      const Tile &t, bool flash) {
    /* Icon action (design/30): a ui-icons-a glyph centred in the upper area
     * with the label centred below — flat (no fill/border) so the line-art
     * icon reads. Falls through to the legacy filled style when no icon is
     * set or it isn't available yet (offline first paint). Toggles keep
     * their pill (handled below). */
    const IconSlot *ic = (t.icon && !t.toggle) ? get_icon(t.icon) : nullptr;
    if (ic) {
        int isz = h - 16;
        if (isz > 44) isz = 44;
        if (isz < 16) isz = 16;
        blit_icon(epd, x + (w - isz) / 2, y + 3, isz, ic);
        draw_glyphs_centered_in(epd, x, w, y + h - 11, 1, t.label, DRIVER_COLOR_BLACK);
        return;
    }
    if (!t.toggle) {
        /* Action without an (available) icon: filled black with a white
         * centred label — reads as "press to do". */
        fill_rect(epd, x, y, w, h, DRIVER_COLOR_BLACK);
        draw_glyphs_centered_in(epd, x, w, y + (h - 8) / 2, 1,
                                t.label, DRIVER_COLOR_WHITE);
        return;
    }
    /* Toggle: outlined cell, black label up top, the current value in
     * a boxed "pill" below. The pill flips to inverted for one frame
     * on a tap (the flash ack) before settling back outlined. */
    draw_rect_border(epd, x, y, w, h);
    draw_glyphs_centered_in(epd, x, w, y + 5, 1, t.label, DRIVER_COLOR_BLACK);

    const char *v = t.value ? t.value : "";
    int vw = (int)strlen(v) * 8 + 8;          /* text + 4 px padding each side */
    if (vw > w - 6) vw = w - 6;
    if (vw < 12) vw = 12;
    const int pill_h = 14;
    const int px = x + (w - vw) / 2;
    const int py = y + h - pill_h - 4;
    if (flash) {
        fill_rect(epd, px, py, vw, pill_h, DRIVER_COLOR_BLACK);
        draw_glyphs_centered_in(epd, px, vw, py + 3, 1, v, DRIVER_COLOR_WHITE);
    } else {
        draw_rect_border(epd, px, py, vw, pill_h);
        draw_glyphs_centered_in(epd, px, vw, py + 3, 1, v, DRIVER_COLOR_BLACK);
    }
}

/* Lay `n` tiles into `cols` columns starting at `top_y`, filling down
 * to the bottom margin, and register each as a hit-rect. Cell height
 * is the available band split across the row count (capped so a
 * 1- or 2-tile page doesn't get absurd slabs). */
static void layout_tiles(epaper_driver_display *epd, const Tile *tiles, int n,
                         int top_y, int cols) {
    if (n <= 0) return;
    const int rows = (n + cols - 1) / cols;
    const int avail_h = MOCHI_EPD_HEIGHT - top_y - MARGIN;
    int cell_h = (avail_h - (rows - 1) * GAP) / rows;
    if (cell_h > MAX_CELL_H) cell_h = MAX_CELL_H;
    if (cell_h < 16) cell_h = 16;
    const int cell_w = (cols == 1) ? FULL_W : CELL_W2;
    for (int i = 0; i < n; i++) {
        const int r = i / cols, c = i % cols;
        const int x = MARGIN + c * (cell_w + GAP);
        const int y = top_y + r * (cell_h + GAP);
        const bool flash = s_flash != TouchResult::None &&
                           tiles[i].action == s_flash;
        draw_tile(epd, x, y, cell_w, cell_h, tiles[i], flash);
        if (s_button_count < MAX_BUTTONS) {
            const int bi = s_button_count++;
            s_buttons[bi] = { x, y, cell_w, cell_h, tiles[i].action };
            snprintf(s_button_ssid[bi], sizeof(s_button_ssid[bi]),
                     "%s", tiles[i].ssid ? tiles[i].ssid : "");
        }
    }
}

/* ─── Per-screen renderers ─────────────────────────────────────────
 *
 * Each draws a full frame into the e-paper buffer (epd_ui::clear wiped
 * it to white in render_mode) and registers its tap-rects. The panel
 * refresh is driven by render_mode after these return. */

/* One stat row (design/30): a 16 px ui-v1 icon on the left, a filled
 * progress bar to `value`/100 across the middle, and the numeric value on
 * the right. Falls back to a one-letter tag when the icon cell isn't
 * cached yet (offline before the first ui-v1 fetch). Skipped if value<0. */
static void draw_stat_row(epaper_driver_display *epd, int y, const char *key,
                          char fallback, int value) {
    if (value < 0) return;
    if (value > 100) value = 100;
    const int icon = 16;
    const IconSlot *ic = get_icon(key);
    if (ic) {
        blit_icon(epd, MARGIN, y, icon, ic);
    } else {
        char fb[2] = { fallback, 0 };
        draw_glyphs(epd, MARGIN + 4, y + (icon - 8) / 2, 1, fb, DRIVER_COLOR_BLACK);
    }
    char vbuf[8];
    snprintf(vbuf, sizeof(vbuf), "%d", value);
    const int val_w = 3 * 8;                                   /* room for "100" */
    const int bar_x = MARGIN + icon + 6;
    const int bar_w = (MOCHI_EPD_WIDTH - MARGIN - val_w - 4) - bar_x;
    const int bar_h = 12;
    const int bar_y = y + (icon - bar_h) / 2;
    draw_rect_border(epd, bar_x, bar_y, bar_w, bar_h);
    const int fill_w = ((bar_w - 4) * value) / 100;
    if (fill_w > 0)
        fill_rect(epd, bar_x + 2, bar_y + 2, fill_w, bar_h - 4, DRIVER_COLOR_BLACK);
    draw_glyphs(epd, MOCHI_EPD_WIDTH - MARGIN - (int)strlen(vbuf) * 8,
                y + (icon - 8) / 2, 1, vbuf, DRIVER_COLOR_BLACK);
}

/* Page 1: kid-facing. Title (pet name + mood) + three icon/progress-bar
 * stat rows (design/30) + a grid of big, safe exploratory actions. */
static void render_menu_p1(epaper_driver_display *epd) {
    /* Title = first line of the status string (name + mood); the numeric
     * second line is now the icon rows below. */
    char title[sizeof(s_pet_status)];
    snprintf(title, sizeof(title), "%s", s_pet_status[0] ? s_pet_status : "Mochi");
    char *nl = strchr(title, '\n');
    if (nl) *nl = '\0';
    epd_ui::draw_text_centered(epd, 6, 1, title);

    int y = 22;
    draw_stat_row(epd, y, "heart", 'H', s_happy);  y += 20;
    draw_stat_row(epd, y, "bowl",  'F', s_full);   y += 20;
    draw_stat_row(epd, y, "star",  'E', s_energy); y += 20;

    const Tile tiles[] = {
        { "Memories", TouchResult::Memories, false, nullptr, nullptr, "memories" },
        { "Places",   TouchResult::Places,   false, nullptr, nullptr, "places" },
        { "Go home",  TouchResult::GoHome,   false, nullptr, nullptr, "home" },
    };
    layout_tiles(epd, tiles, 3, y + 4, 2);
}

/* Page 2: settings — network/device info header + a 2-col grid of the
 * configuration actions (Switch WiFi → modal, OpenAI key, AI models →
 * modal). One PWR×2 in, so the kid won't land here by accident. */
static void render_menu_p2(epaper_driver_display *epd) {
    epd_ui::draw_text_centered(epd, 4, 1, "SETTINGS  (PWR exits)");

    const size_t free_psr = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    /* Sized for the worst case the format-truncation analysis assumes
     * (pet_name + version are each up to 39 chars). */
    char l0[96], l1[64], l2[64];
    snprintf(l0, sizeof(l0), "%s  %s",
        s_pet_name[0] ? s_pet_name : "?",
        s_version[0]  ? s_version  : "?");
    snprintf(l1, sizeof(l1), "%s  %s",
        phase_label(s_net_phase),
        s_ip_str[0] ? s_ip_str : "-");
    snprintf(l2, sizeof(l2), "%.16s  %d%%  %uK",
        s_ssid[0] ? s_ssid : "-", s_batt_pct,
        (unsigned)(free_psr / 1024));
    epd_ui::draw_text(epd, MARGIN, 18, 1, l0);
    epd_ui::draw_text(epd, MARGIN, 30, 1, l1);
    epd_ui::draw_text(epd, MARGIN, 42, 1, l2);

    const Tile tiles[] = {
        { "Switch WiFi", TouchResult::SwitchWifi,    false, nullptr, nullptr, "wifi" },
        { "OpenAI key",  TouchResult::OpenKeyPortal, false, nullptr, nullptr, "secret_key" },
        { "AI models",   TouchResult::OpenModels,    false, nullptr, nullptr, "sparkle_stars" },
    };
    layout_tiles(epd, tiles, 3, 56, 2);
}

/* Page 3: destructive territory. Two PWR×2 in; single-tap escapes. A
 * "RISK" header sets it apart. The Channel toggle sits among the
 * actions to show the two tile styles side by side. */
static void render_menu_p3(epaper_driver_display *epd) {
    epd_ui::draw_text_centered(epd, 4, 1, "RISK  (PWR exits)");

    /* Channel pill value — buffer must outlive layout_tiles (same scope). */
    const char *chan = ota_channel_name(ota_channel_get());
    const Tile tiles[] = {
        { "Update now",  TouchResult::UpdateNow,     false, nullptr, nullptr, "update" },
        { "Channel",     TouchResult::ToggleChannel, true,  chan,    nullptr, nullptr },
        { "Add WiFi",    TouchResult::ChangeWifi,    false, nullptr, nullptr, "wifi" },
        { "Forget WiFi", TouchResult::ForgetWifi,    false, nullptr, nullptr, "wifi_off" },
        { "Re-pair",     TouchResult::RePair,        false, nullptr, nullptr, "repair" },
        { "Consolidate", TouchResult::ConsolidateNow, false, nullptr, nullptr, "dream" },
    };
    layout_tiles(epd, tiles, 6, 20, 2);
}

/* WiFi modal — pushed from "Switch WiFi" on P2. A single wide column
 * of stored SSIDs (long names need the full width); "* " marks the
 * currently-joined one. Built fresh each render (NVS may have changed).
 * Capped to what fits the panel with a usable tap height. */
static void render_wifi(epaper_driver_display *epd) {
    epd_ui::draw_text_centered(epd, 4, 1, "SWITCH WIFI");

    static constexpr int WIFI_MAX_ROWS = 5;
    char labels[WIFI_MAX_ROWS][MOCHI_WIFI_SSID_MAX + 4];
    char ssids[WIFI_MAX_ROWS][MOCHI_WIFI_SSID_MAX + 1];
    Tile tiles[WIFI_MAX_ROWS];
    const size_t stored = nvs_creds_count();
    int n = 0;
    for (size_t i = 0; i < stored && n < WIFI_MAX_ROWS; i++) {
        struct mochi_wifi_creds c = {};
        if (!nvs_creds_load_at(i, &c)) continue;
        const bool is_cur = s_ssid[0] &&
            strncmp(c.ssid, s_ssid, MOCHI_WIFI_SSID_MAX) == 0;
        snprintf(labels[n], sizeof(labels[n]), "%s%s",
                 is_cur ? "* " : "", c.ssid);
        snprintf(ssids[n], sizeof(ssids[n]), "%s", c.ssid);
        tiles[n] = { labels[n], TouchResult::WifiSwitch, false, nullptr, ssids[n], nullptr };
        n++;
    }
    if (n == 0) {
        epd_ui::draw_text_centered(epd, 90,  1, "no saved networks");
        epd_ui::draw_text_centered(epd, 104, 1, "Add WiFi on next page");
        return;
    }
    layout_tiles(epd, tiles, n, 20, 1);
}

/* AI-models modal — pushed from "AI models" on P2. Two toggle tiles
 * (Voice / Text) in a single wide column so the model name pills have
 * room. Tapping a tile cycles the pref + flashes the pill. */
static void render_models(epaper_driver_display *epd) {
    epd_ui::draw_text_centered(epd, 4, 1, "AI MODELS  tap=cycle");

    char vm[48], tm[48];
    model_prefs_voice(vm, sizeof(vm));
    model_prefs_text(tm, sizeof(tm));
    /* design/27: admin debug-voice persona toggle. Off = the normal
     * in-character persona; On = the server's tool-test diagnostic
     * persona (voice.cpp appends ?mode=debug). */
    const char *dbg = model_prefs_voice_debug() ? "on" : "off";
    const Tile tiles[] = {
        { "Voice", TouchResult::CycleVoiceModel,  true, vm,  nullptr, nullptr },
        { "Text",  TouchResult::CycleTextModel,   true, tm,  nullptr, nullptr },
        { "Debug", TouchResult::ToggleVoiceDebug, true, dbg, nullptr, nullptr },
    };
    layout_tiles(epd, tiles, 3, 22, 1);
}

/* ─── render_mode ──────────────────────────────────────────────────
 *
 * Clear the panel buffer, draw the screen for `m`, then push it. A
 * `full` refresh (init+display) busts e-paper ghosting on screen
 * changes; an in-place update (toggle flash/settle) uses a faster
 * partial refresh against the base image the last full render set.
 * `flash` marks one toggle tile's pill inverted for the ack frame. */
static void render_mode(Mode m, bool full, TouchResult flash) {
    if (!s_epd || m == Mode::Live) return;
    s_flash = flash;
    clear_buttons();
    s_epd->EPD_Clear();
    switch (m) {
        case Mode::MenuP1:      render_menu_p1(s_epd); break;
        case Mode::MenuP2:      render_menu_p2(s_epd); break;
        case Mode::MenuP3:      render_menu_p3(s_epd); break;
        case Mode::WifiModal:   render_wifi(s_epd);    break;
        case Mode::ModelsModal: render_models(s_epd);  break;
        default: s_flash = TouchResult::None; return;
    }
    s_flash = TouchResult::None;
    if (full) {
        s_epd->EPD_Init();
        s_epd->EPD_Display();
        s_epd->EPD_DisplayPartBaseImage();
    } else {
        s_epd->EPD_Init_Partial();
        s_epd->EPD_DisplayPart();
    }
}

void repaint(void) {
    if (s_mode != Mode::Live) render_mode(s_mode, /*full=*/true, TouchResult::None);
}

/* Toggle a stateful tile: mutate the pref, flash the pill inverted as
 * an acknowledgement, then settle back. Both frames are partial
 * refreshes against the page's base image. Resets the inactivity
 * timer so a few taps to land on the right value don't bounce to Live. */
static void toggle_flash(Mode page, TouchResult action) {
    s_entered_mode_us = esp_timer_get_time();
    render_mode(page, /*full=*/false, action);          /* pill inverted */
    vTaskDelay(pdMS_TO_TICKS(220));
    render_mode(page, /*full=*/false, TouchResult::None); /* settle */
}

/* ─── dispatch ─────────────────────────────────────────────────────
 *
 * Hand-rolled hit-test against the rects the current screen registered.
 * On a miss, return None and leave the menu up (main.cpp keeps the
 * page; PWR×1 is the escape). Internal actions (modal pushes, toggles)
 * are handled here and reported as None; everything else is returned
 * for main.cpp to perform. */
TouchResult dispatch_touch(int x, int y) {
    if (s_mode == Mode::Live) return TouchResult::None;

    int hit = -1;
    for (int i = 0; i < s_button_count; i++) {
        const Button &b = s_buttons[i];
        if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) {
            hit = i;
            break;
        }
    }
    if (hit < 0) return TouchResult::None;

    const TouchResult action = s_buttons[hit].action;
    const int64_t now = esp_timer_get_time();

    switch (action) {
        case TouchResult::SwitchWifi:
            /* Push the WiFi modal in place; main.cpp never sees this. */
            s_mode = Mode::WifiModal;
            s_entered_mode_us = now;
            render_mode(s_mode, /*full=*/true, TouchResult::None);
            return TouchResult::None;
        case TouchResult::OpenModels:
            s_mode = Mode::ModelsModal;
            s_entered_mode_us = now;
            render_mode(s_mode, /*full=*/true, TouchResult::None);
            return TouchResult::None;
        case TouchResult::CycleVoiceModel:
            model_prefs_cycle_voice();
            toggle_flash(Mode::ModelsModal, action);
            return TouchResult::None;
        case TouchResult::CycleTextModel:
            model_prefs_cycle_text();
            toggle_flash(Mode::ModelsModal, action);
            return TouchResult::None;
        case TouchResult::ToggleVoiceDebug:
            model_prefs_toggle_voice_debug();
            toggle_flash(Mode::ModelsModal, action);
            return TouchResult::None;
        case TouchResult::ToggleChannel:
            /* Flip the persisted OTA channel + nudge an immediate check
             * so opting into beta (or back) applies promptly. */
            ota_channel_toggle();
            ota_update::check_now();
            toggle_flash(Mode::MenuP3, action);
            return TouchResult::None;
        case TouchResult::WifiSwitch:
            snprintf(s_picked_ssid, sizeof(s_picked_ssid), "%s",
                     s_button_ssid[hit]);
            return TouchResult::WifiSwitch;
        default:
            /* OpenKeyPortal / UpdateNow / ChangeWifi / ForgetWifi /
             * RePair / GoHome / Memories / Places — main.cpp performs. */
            return action;
    }
}

/* ─── tick ─────────────────────────────────────────────────────────*/

bool tick(epaper_driver_display * /*epd*/, bool /*paired*/,
          const char *pet_name, const char *version,
          const char *ip_str, const char *ssid,
          int net_phase, int batt_pct,
          const char *pet_status,
          int happy, int full, int energy) {
    const int64_t now_us = esp_timer_get_time();
    bool changed = false;

    /* Cache the freshest data so a modal push (which doesn't receive
     * params) can still see current values, and so the header renders
     * reflect the most recent main-loop state. */
    if (pet_name)   snprintf(s_pet_name,   sizeof(s_pet_name),   "%s", pet_name);
    if (version)    snprintf(s_version,    sizeof(s_version),    "%s", version);
    if (ip_str)     snprintf(s_ip_str,     sizeof(s_ip_str),     "%s", ip_str);
    if (ssid)       snprintf(s_ssid,       sizeof(s_ssid),       "%s", ssid);
    if (pet_status) snprintf(s_pet_status, sizeof(s_pet_status), "%s", pet_status);
    s_net_phase = net_phase;
    s_batt_pct  = batt_pct;
    s_happy = happy; s_full = full; s_energy = energy;

    /* Consume any PWR presses the watcher latched since the last tick.
     * exchange returns the prior value and resets the flag — so a burst
     * of fast presses inside one tick still advances the wheel once. */
    const bool pressed = s_press_pending.exchange(false, std::memory_order_acq_rel);

    if (pressed) {
        const Mode next = advance(s_mode);
        ESP_LOGI(TAG, "PWR: advance → %d", (int)next);
        s_mode = next;
        s_entered_mode_us = now_us;
        /* Full refresh on a page change: we may be coming from the busy
         * live pet image or another page, both of which ghost under a
         * partial. Menu headers are a snapshot taken here at entry; we
         * deliberately don't re-refresh them every tick so the figures
         * stay stable while a page is up. */
        render_mode(s_mode, /*full=*/true, TouchResult::None);
        changed = true;
    }

    if (s_mode != Mode::Live &&
        (now_us - s_entered_mode_us) >= INACTIVITY_US && !changed) {
        ESP_LOGI(TAG, "inactivity timeout → live");
        s_mode = Mode::Live;
        clear_buttons();
        changed = true;
    }
    return changed;
}

}  /* namespace dev_menu */
