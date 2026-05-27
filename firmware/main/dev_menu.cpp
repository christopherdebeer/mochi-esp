#include "dev_menu.h"

#include <stdio.h>
#include <string.h>
#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "board_pins.h"
#include "epd_ui.h"
#include "nvs_creds.h"

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

static const char *mode_name(Mode m) {
    switch (m) {
        case Mode::Live:      return "live";
        case Mode::Info:      return "info";
        case Mode::Actions:   return "actions";
        case Mode::WifiModal: return "wifi_modal";
        default:              return "?";
    }
}

/* ─── Tappable button registry ─────────────────────────────────────
 *
 * Each tappable region renders a 1-px bordered rect with its label
 * centred inside. Hit-tests resolve via dispatch_touch() at the
 * coords stored here. Coordinates are panel pixels.
 *
 * Buttons live only while their owning screen is up; clear_buttons()
 * runs before each render so a stale Info hit-rect can't leak into a
 * touch dispatched against the next screen. */
struct Button {
    int x, y, w, h;
    TouchResult action;
    const char *label;
};
/* Headroom for the Actions screen (7) plus the WiFi modal's stored
 * networks (capped to fit on the panel). */
static constexpr int MAX_BUTTONS = 10;
static Button   s_buttons[MAX_BUTTONS] = {};
static int      s_button_count = 0;
static char     s_button_ssid[MAX_BUTTONS][MOCHI_WIFI_SSID_MAX + 1] = {};
static char     s_picked_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};

static void clear_buttons(void) {
    s_button_count = 0;
    for (int i = 0; i < MAX_BUTTONS; i++) {
        s_buttons[i] = {};
        s_button_ssid[i][0] = '\0';
    }
}

const char *picked_ssid(void) { return s_picked_ssid; }

void init(epaper_driver_display * /*epd*/) {
    s_mode = Mode::Live;
    s_entered_mode_us = 0;
    s_press_pending.store(false, std::memory_order_release);
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
    }
}

/* Advance: Live → Info → Actions → Info (wraps). The WifiModal mode
 * is *not* in the cycle — it's pushed by the SwitchWifi button on
 * Actions, and a PWR-tap from inside the modal advances back to Info
 * (i.e. one tap past Actions). */
static Mode advance(Mode m) {
    switch (m) {
        case Mode::Live:      return Mode::Info;
        case Mode::Info:      return Mode::Actions;
        case Mode::Actions:   return Mode::Info;     /* wrap */
        case Mode::WifiModal: return Mode::Info;     /* exit modal back to top */
        default:              return Mode::Info;
    }
}

/* ─── Mode renderers ─────────────────────────────────────────────── */

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

static void register_button(int x, int y, int w, int h,
                            TouchResult action, const char *label,
                            const char *ssid = nullptr) {
    if (s_button_count >= MAX_BUTTONS) return;
    const int i = s_button_count++;
    s_buttons[i] = { x, y, w, h, action, label };
    snprintf(s_button_ssid[i], sizeof(s_button_ssid[i]), "%s", ssid ? ssid : "");
}

/* Info screen — merges what used to be Splash + Settings. The boot
 * splash already has a polished design (pet face, version banner) so
 * we re-use it for the visual top of the screen, then overlay the
 * compact Settings text block over the lower half. */
static void render_info(epaper_driver_display *epd,
                        const char *pet_name, const char *version,
                        const char *ip_str, const char *ssid,
                        int net_phase, int batt_pct) {
    epd_ui::clear(epd);
    clear_buttons();

    epd_ui::draw_text_centered(epd, 4, 2, "INFO");

    char line[40];
    int y = 26;
    snprintf(line, sizeof(line), "fw   %s", version ? version : "?");
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;
    snprintf(line, sizeof(line), "pet  %s", pet_name ? pet_name : "?");
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;
    snprintf(line, sizeof(line), "net  %s", phase_label(net_phase));
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;
    snprintf(line, sizeof(line), "ip   %s", ip_str && *ip_str ? ip_str : "-");
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;
    snprintf(line, sizeof(line), "ssid %.30s", ssid && *ssid ? ssid : "-");
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;

    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psr = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snprintf(line, sizeof(line), "ram  %uK", (unsigned)(free_int / 1024));
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;
    snprintf(line, sizeof(line), "psr  %uK", (unsigned)(free_psr / 1024));
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;
    snprintf(line, sizeof(line), "batt %d%%", batt_pct);
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;

    epd_ui::draw_text(epd, 4, MOCHI_EPD_HEIGHT - 10, 1,
                      "PWR: Actions  60s exit");

    epd->EPD_Init_Partial();
    epd->EPD_DisplayPart();
}

/* Actions screen — vertical stack of tappable buttons. Each is a
 * 1-px bordered full-width rect with a centred label; dispatch_touch
 * resolves a tap to the button's TouchResult, and main.cpp performs
 * the action (most reboot, so they exit the wheel implicitly).
 *
 * v0.1.6: added "Switch WiFi" — pushes the WiFi modal in-place rather
 * than the modal living as its own wheel slot. The 200×200 panel fits
 * 7 buttons at 22 px tall (was 24, lost 2 to make room). */
static void render_actions(epaper_driver_display *epd) {
    epd_ui::clear(epd);
    clear_buttons();

    epd_ui::draw_text_centered(epd, 4, 2, "ACTIONS");

    static const struct { TouchResult action; const char *label; } items[] = {
        { TouchResult::SwitchWifi,    "Switch WiFi"     },
        { TouchResult::ChangeWifi,    "Add WiFi"        },
        { TouchResult::ForgetWifi,    "Forget WiFi"     },
        { TouchResult::UpdateNow,     "Update now"      },
        { TouchResult::RePair,        "Re-pair device"  },
        { TouchResult::GoHome,        "Go home"         },
        { TouchResult::OpenKeyPortal, "OpenAI key"      },
    };
    constexpr int N = (int)(sizeof(items) / sizeof(items[0]));
    constexpr int BTN_MARGIN = 2;
    constexpr int BTN_W = MOCHI_EPD_WIDTH - 2 * BTN_MARGIN;
    constexpr int BTN_H = 22;
    constexpr int BTN_GAP = 2;
    constexpr int TOP_Y = 18;

    int by = TOP_Y;
    for (int i = 0; i < N; i++) {
        Button b = { BTN_MARGIN, by, BTN_W, BTN_H, items[i].action, items[i].label };
        draw_button_border(epd, b);
        const int label_w = (int)strlen(b.label) * 8;
        epd_ui::draw_text(epd, b.x + (b.w - label_w) / 2,
                          b.y + (b.h - 8) / 2, 1, b.label);
        register_button(b.x, b.y, b.w, b.h, b.action, b.label);
        by += BTN_H + BTN_GAP;
    }

    epd->EPD_Init_Partial();
    epd->EPD_DisplayPart();
}

/* WiFi modal — pushed from the Actions/SwitchWifi button. Lists the
 * stored networks (NVS MRU); tap one to commit the switch. Exits on
 * either a button-tap (handled in main.cpp) or the next PWR-tap
 * (advances back to Info). */
static void render_wifi_modal(epaper_driver_display *epd, const char *cur_ssid) {
    epd_ui::clear(epd);
    clear_buttons();

    epd_ui::draw_text_centered(epd, 4, 2, "SWITCH WIFI");

    const size_t stored = nvs_creds_count();
    constexpr int BTN_MARGIN = 2;
    constexpr int BTN_W = MOCHI_EPD_WIDTH - 2 * BTN_MARGIN;
    constexpr int BTN_H = 24;
    constexpr int BTN_GAP = 2;
    constexpr int TOP_Y = 22;
    const int max_rows = (MOCHI_EPD_HEIGHT - TOP_Y - 12) / (BTN_H + BTN_GAP);

    int rows = (int)stored;
    if (rows > max_rows) rows = max_rows;
    if (rows > MAX_BUTTONS) rows = MAX_BUTTONS;

    int by = TOP_Y;
    for (int i = 0; i < rows; i++) {
        struct mochi_wifi_creds c = {};
        if (!nvs_creds_load_at((size_t)i, &c)) continue;
        const bool is_cur = cur_ssid && cur_ssid[0] &&
                            strncmp(c.ssid, cur_ssid, MOCHI_WIFI_SSID_MAX) == 0;
        char label[40];
        snprintf(label, sizeof(label), "%s%.30s", is_cur ? "*" : " ", c.ssid);
        Button b = { BTN_MARGIN, by, BTN_W, BTN_H, TouchResult::WifiSwitch, "" };
        draw_button_border(epd, b);
        epd_ui::draw_text(epd, b.x + 6, b.y + (b.h - 8) / 2, 1, label);
        register_button(b.x, b.y, b.w, b.h, TouchResult::WifiSwitch, "", c.ssid);
        by += BTN_H + BTN_GAP;
    }

    if (rows == 0) {
        epd_ui::draw_text(epd, 4, TOP_Y + 8, 1, "no saved networks");
    }

    epd_ui::draw_text(epd, 4, MOCHI_EPD_HEIGHT - 9, 1,
                      rows > 0 ? "tap=switch  PWR exit" : "Add WiFi from Actions");

    epd->EPD_Init_Partial();
    epd->EPD_DisplayPart();
}

/* Last-known SSID for the WifiModal render; the modal is pushed in
 * dispatch_touch (no main.cpp params there), so we cache the value
 * during the most recent tick() / Actions render. Initialised empty;
 * a dispatch_touch happening before the first tick() falls back to
 * "no current network" which is fine. */
static char s_last_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};

TouchResult dispatch_touch(int x, int y) {
    if (s_mode == Mode::Live) return TouchResult::None;
    for (int i = 0; i < s_button_count; i++) {
        const Button &b = s_buttons[i];
        if (x >= b.x && x < b.x + b.w &&
            y >= b.y && y < b.y + b.h) {
            ESP_LOGI(TAG, "button hit: '%s'", b.label);
            if (b.action == TouchResult::SwitchWifi) {
                /* Internal mode push — main.cpp doesn't see this. The
                 * EPD render happens inline so the user sees the modal
                 * the same tick they tapped. */
                /* Need an epd to render. This one is held in tick()'s
                 * call; we don't have it here — but we don't need to
                 * re-render right now because tick() will run on the
                 * next main loop iteration and pick up the new mode.
                 * Touch in dispatch is dispatched from main's touch
                 * handler which then re-runs tick() / render. As a
                 * shortcut we mark the mode and the next tick handles
                 * render: keep the user's touch frame fresh by
                 * forcing s_press_pending so tick re-renders. */
                s_mode = Mode::WifiModal;
                s_entered_mode_us = esp_timer_get_time();
                s_press_pending.store(true, std::memory_order_release);
                return TouchResult::None;   /* swallowed by dev_menu */
            }
            if (b.action == TouchResult::WifiSwitch) {
                snprintf(s_picked_ssid, sizeof(s_picked_ssid),
                         "%s", s_button_ssid[i]);
            }
            return b.action;
        }
    }
    return TouchResult::None;
}

static void render_mode(epaper_driver_display *epd, Mode m,
                        bool /*paired*/, const char *pet_name,
                        const char *version, const char *ip_str,
                        const char *ssid, int net_phase, int batt_pct) {
    if (ssid) {
        snprintf(s_last_ssid, sizeof(s_last_ssid), "%s", ssid);
    }
    switch (m) {
        case Mode::Info:
            render_info(epd, pet_name, version, ip_str, ssid,
                        net_phase, batt_pct);
            break;
        case Mode::Actions:
            render_actions(epd);
            break;
        case Mode::WifiModal:
            render_wifi_modal(epd, s_last_ssid);
            break;
        default:
            /* Live is rendered by main.cpp's render_resting on the
             * next tick after we exit; we just clear the mode. */
            break;
    }
}

bool tick(epaper_driver_display *epd, bool paired,
          const char *pet_name, const char *version,
          const char *ip_str, const char *ssid,
          int net_phase, int batt_pct) {
    const int64_t now_us = esp_timer_get_time();
    bool changed = false;

    /* Consume any presses the watcher latched since the last tick.
     * exchange returns the prior value and resets the flag — so a
     * burst of fast presses inside one tick still advances the
     * wheel exactly once (intentional). The SwitchWifi dispatch path
     * also sets this flag — to force a re-render on the same tick
     * the modal was pushed without doing the actual mode advance.
     * We disambiguate by checking whether dispatch already changed
     * s_mode to WifiModal: if it did, render-only; else, advance. */
    const bool pressed = s_press_pending.exchange(false, std::memory_order_acq_rel);

    if (pressed) {
        if (s_mode != Mode::WifiModal) {
            const Mode next = advance(s_mode);
            ESP_LOGI(TAG, "PWR: %s → %s", mode_name(s_mode), mode_name(next));
            clear_buttons();
            s_mode = next;
        } else {
            ESP_LOGI(TAG, "modal push → wifi_modal");
            /* Mode already set by dispatch_touch; just re-render.
             * Buttons cleared inside render_wifi_modal. */
        }
        s_entered_mode_us = now_us;
        render_mode(epd, s_mode, paired, pet_name, version,
                    ip_str, ssid, net_phase, batt_pct);
        changed = true;
    } else if (s_mode != Mode::Live &&
               (now_us - s_entered_mode_us) >= INACTIVITY_US) {
        ESP_LOGI(TAG, "inactivity timeout → live");
        s_mode = Mode::Live;
        clear_buttons();
        changed = true;
    }
    return changed;
}

}  /* namespace dev_menu */
