#include "dev_menu.h"

#include <stdio.h>
#include <string.h>
#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "board_pins.h"
#include "epd_ui.h"
#include "nvs_creds.h"
#include "model_prefs.h"
#include "ota_channel.h"
#include "ota_update.h"

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

/* ─── Tappable button registry ─────────────────────────────────────
 *
 * Hand-rolled hit-testing (this is the rollback from the LVGL widget
 * tree — see dev_menu.h history). Each render lays out a vertical
 * stack of 1-px bordered rects with centred labels and records their
 * pixel rects here; dispatch_touch resolves an (x, y) tap to the
 * rect it lands in. Buttons live only while their owning screen is
 * up; clear_buttons() runs before each render so a stale rect from a
 * previous page can't leak into the next page's dispatch. */
struct Button {
    int x, y, w, h;
    TouchResult action;
};
/* Headroom for the riskiest page (5) plus the WiFi modal's stored
 * networks (capped to fit on the panel). */
static constexpr int MAX_BUTTONS = 10;
static Button   s_buttons[MAX_BUTTONS] = {};
static int      s_button_count = 0;
/* SSID payload, parallel to s_buttons — only the WifiModal rows use
 * it. Picked into s_picked_ssid when a WifiSwitch row is tapped. */
static char     s_button_ssid[MAX_BUTTONS][MOCHI_WIFI_SSID_MAX + 1] = {};
static char     s_picked_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};

/* Layout constants. 200×200 panel; ASCII 8×8 font (scale 1 = 8 px). */
static constexpr int MARGIN  = 4;
static constexpr int BTN_W   = MOCHI_EPD_WIDTH - 2 * MARGIN;  /* 192 */
static constexpr int BTN_H   = 24;
static constexpr int BTN_GAP = 4;
static constexpr int GLYPH_W = 8;   /* font8x8 cell, scale 1 */
static constexpr int GLYPH_H = 8;

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

/* ─── Draw helpers ─────────────────────────────────────────────────*/

static void draw_button_border(epaper_driver_display *epd, const Button &b) {
    for (int dx = 0; dx < b.w; dx++) {
        epd->EPD_DrawColorPixel(b.x + dx, b.y, DRIVER_COLOR_BLACK);
        epd->EPD_DrawColorPixel(b.x + dx, b.y + b.h - 1, DRIVER_COLOR_BLACK);
    }
    for (int dy = 0; dy < b.h; dy++) {
        epd->EPD_DrawColorPixel(b.x, b.y + dy, DRIVER_COLOR_BLACK);
        epd->EPD_DrawColorPixel(b.x + b.w - 1, b.y + dy, DRIVER_COLOR_BLACK);
    }
}

/* Lay out a full-width row at vertical pixel `y`: bordered rect with a
 * centred (clipped) label, recorded as a hit-rect. Returns the y for
 * the next row. ssid is the optional WifiSwitch payload. */
static int add_button(epaper_driver_display *epd, int y, const char *label,
                      TouchResult action, const char *ssid = nullptr) {
    Button b = { MARGIN, y, BTN_W, BTN_H, action };
    draw_button_border(epd, b);
    const int label_w = (int)strlen(label) * GLYPH_W;
    int tx = b.x + (b.w - label_w) / 2;
    if (tx < b.x + 2) tx = b.x + 2;   /* left-clip long labels */
    const int ty = b.y + (b.h - GLYPH_H) / 2;
    epd_ui::draw_text(epd, tx, ty, 1, label);
    if (s_button_count < MAX_BUTTONS) {
        const int i = s_button_count++;
        s_buttons[i] = b;
        snprintf(s_button_ssid[i], sizeof(s_button_ssid[i]),
                 "%s", ssid ? ssid : "");
    }
    return y + BTN_H + BTN_GAP;
}

/* Draw a possibly-multiline ('\n'-separated) string, each line centred,
 * starting at `y`. Returns the y just below the block. */
static int draw_multiline_centered(epaper_driver_display *epd, int y,
                                    const char *text, int line_h) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", text);
    char *line = buf;
    while (line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        epd_ui::draw_text_centered(epd, y, 1, line);
        y += line_h;
        line = nl ? nl + 1 : nullptr;
    }
    return y;
}

/* ─── Per-screen renderers ─────────────────────────────────────────
 *
 * Each draws a full frame into the e-paper driver's internal buffer
 * (epd_ui::clear already wiped it to white in render_mode) and
 * registers its tap-rects. The actual panel refresh is driven by
 * render_mode after the screen-specific draw returns. */

/* Page 1: kid-facing main page. Pet-status header at the top, then the
 * safe exploratory actions (Memories, Places, Go home). No config
 * knobs here — those live one PWR×2 deeper on P2. */
static void render_menu_p1(epaper_driver_display *epd) {
    int y = 6;
    y = draw_multiline_centered(epd,
        y, s_pet_status[0] ? s_pet_status : "Mochi", 11);
    if (y < 34) y = 34;
    y = add_button(epd, y, "Memories", TouchResult::Memories);
    y = add_button(epd, y, "Places",   TouchResult::Places);
    y = add_button(epd, y, "Go home",  TouchResult::GoHome);
}

/* Page 2: settings — network/device info header + the configuration
 * actions (Switch WiFi → modal, OpenAI key, AI models → modal). One
 * PWR×2 into the menu, so the kid won't land here by accident. */
static void render_menu_p2(epaper_driver_display *epd) {
    epd_ui::draw_text_centered(epd, 4, 1, "SETTINGS  (PWR exits)");

    const size_t free_psr = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    char l0[40], l1[40], l2[40];
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

    int y = 58;
    y = add_button(epd, y, "Switch WiFi", TouchResult::SwitchWifi);
    y = add_button(epd, y, "OpenAI key",  TouchResult::OpenKeyPortal);
    y = add_button(epd, y, "AI models",   TouchResult::OpenModels);
}

/* Page 3: destructive territory. Two PWR×2 into the menu from Live;
 * single-tap escapes back. A "RISK" header makes the page register
 * different from P1/P2 at a glance. */
static void render_menu_p3(epaper_driver_display *epd) {
    epd_ui::draw_text_centered(epd, 4, 1, "RISK  (PWR exits)");

    int y = 20;
    y = add_button(epd, y, "Update now", TouchResult::UpdateNow);
    /* OTA channel toggle. Lives on RISK because opting into beta pulls
     * pre-release builds. Label reflects the persisted channel; a tap
     * flips it (handled in dispatch_touch, which re-renders this page). */
    char chan_row[32];
    snprintf(chan_row, sizeof(chan_row), "Channel: %s",
        ota_channel_name(ota_channel_get()));
    y = add_button(epd, y, chan_row,     TouchResult::ToggleChannel);
    y = add_button(epd, y, "Add WiFi",    TouchResult::ChangeWifi);
    y = add_button(epd, y, "Forget WiFi", TouchResult::ForgetWifi);
    y = add_button(epd, y, "Re-pair",     TouchResult::RePair);
}

/* WiFi modal — pushed from "Switch WiFi" on P2. Lists the stored
 * networks (NVS may have grown/shrunk since last entry, so it's built
 * fresh each render). A "* " prefix marks the currently-joined SSID. */
static void render_wifi(epaper_driver_display *epd) {
    epd_ui::draw_text_centered(epd, 4, 1, "SWITCH WIFI");

    const size_t stored = nvs_creds_count();
    int y = 20;
    int rendered = 0;
    /* Cap at MAX_BUTTONS and at what fits on the panel (~6 rows). */
    for (size_t i = 0; i < stored && rendered < MAX_BUTTONS && y + BTN_H < MOCHI_EPD_HEIGHT; i++) {
        struct mochi_wifi_creds c = {};
        if (!nvs_creds_load_at(i, &c)) continue;
        const bool is_cur = s_ssid[0] &&
            strncmp(c.ssid, s_ssid, MOCHI_WIFI_SSID_MAX) == 0;
        char label[MOCHI_WIFI_SSID_MAX + 4];
        snprintf(label, sizeof(label), "%s%s", is_cur ? "* " : "  ", c.ssid);
        y = add_button(epd, y, label, TouchResult::WifiSwitch, c.ssid);
        rendered++;
    }
    if (rendered == 0) {
        epd_ui::draw_text_centered(epd, 90,  1, "no saved networks");
        epd_ui::draw_text_centered(epd, 104, 1, "Add WiFi on next page");
    }
}

/* AI models modal — pushed from "AI models" on P2. Shows the current
 * voice + text model selections; tapping a row cycles it (handled in
 * dispatch_touch, which re-renders this screen with the new value). */
static void render_models(epaper_driver_display *epd) {
    epd_ui::draw_text_centered(epd, 4, 1, "AI MODELS  tap=cycle");

    char vm[48], tm[48];
    model_prefs_voice(vm, sizeof(vm));
    model_prefs_text(tm, sizeof(tm));
    char vrow[64], trow[64];
    snprintf(vrow, sizeof(vrow), "Voice: %s", vm);
    snprintf(trow, sizeof(trow), "Text: %s", tm);

    int y = 22;
    y = add_button(epd, y, vrow, TouchResult::CycleVoiceModel);
    y = add_button(epd, y, trow, TouchResult::CycleTextModel);

    epd_ui::draw_text_centered(epd, y + 8,  1, "voice = realtime");
    epd_ui::draw_text_centered(epd, y + 20, 1, "text = consolidation");
}

/* ─── render_mode ──────────────────────────────────────────────────
 *
 * Clear the panel buffer, draw the screen for `m`, then push it. A
 * `full` refresh (init+display) busts e-paper ghosting on screen
 * changes (entering from the busy pet image, or page→page); an
 * in-place relabel (model/channel toggle) uses a faster partial
 * refresh against the base image the previous full render set. */
static void render_mode(Mode m, bool full) {
    if (!s_epd || m == Mode::Live) return;
    clear_buttons();
    s_epd->EPD_Clear();
    switch (m) {
        case Mode::MenuP1:      render_menu_p1(s_epd); break;
        case Mode::MenuP2:      render_menu_p2(s_epd); break;
        case Mode::MenuP3:      render_menu_p3(s_epd); break;
        case Mode::WifiModal:   render_wifi(s_epd);    break;
        case Mode::ModelsModal: render_models(s_epd);  break;
        default: return;
    }
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
    if (s_mode != Mode::Live) render_mode(s_mode, /*full=*/true);
}

/* ─── dispatch ─────────────────────────────────────────────────────
 *
 * Hand-rolled hit-test against the rects the current screen registered.
 * On a miss, return None and leave the menu up (main.cpp keeps the
 * page; PWR×1 is the escape). Internal actions (modal pushes, in-place
 * toggles) are handled here and reported as None so main.cpp stays out
 * of it; everything else is returned for main.cpp to perform. */
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
            render_mode(s_mode, /*full=*/true);
            return TouchResult::None;
        case TouchResult::OpenModels:
            s_mode = Mode::ModelsModal;
            s_entered_mode_us = now;
            render_mode(s_mode, /*full=*/true);
            return TouchResult::None;
        case TouchResult::CycleVoiceModel:
            model_prefs_cycle_voice();
            s_entered_mode_us = now;
            render_mode(Mode::ModelsModal, /*full=*/false);
            return TouchResult::None;
        case TouchResult::CycleTextModel:
            model_prefs_cycle_text();
            s_entered_mode_us = now;
            render_mode(Mode::ModelsModal, /*full=*/false);
            return TouchResult::None;
        case TouchResult::ToggleChannel: {
            /* Flip the persisted OTA channel + nudge an immediate check
             * so opting into beta (or back) applies promptly rather than
             * waiting for the next 24 h poll. Re-render P3 in place so
             * the label reflects the new channel. */
            ota_channel_toggle();
            ota_update::check_now();
            s_entered_mode_us = now;
            render_mode(Mode::MenuP3, /*full=*/false);
            return TouchResult::None;
        }
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
          const char *pet_status) {
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
        render_mode(s_mode, /*full=*/true);
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
