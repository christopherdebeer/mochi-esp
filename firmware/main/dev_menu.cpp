#include "dev_menu.h"

#include <stdio.h>
#include <string.h>
#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "epd_ui.h"

static const char *TAG = "dev_menu";

namespace dev_menu {

/* Inactivity timeout: any non-Live mode snaps back to Live after this
 * many microseconds without a BOOT press. Long enough to read a
 * full diagnostics screen + cross-check details with notes; touch or
 * a fresh BOOT press still exits / advances earlier. */
static constexpr int64_t INACTIVITY_US = 60LL * 1000 * 1000;

/* Watcher poll cadence — fast enough that a kid's normal short press
 * (~80–150 ms) reliably catches a falling edge. The 1 Hz main-loop
 * tick missed presses entirely when they fit between iterations,
 * which read as "BOOT is unresponsive". */
static constexpr int POLL_MS = 25;
/* Debounce: ignore additional edges within this many ticks of the
 * last accepted press. */
static constexpr int DEBOUNCE_TICKS = 2;   /* 50 ms */

static Mode                 s_mode = Mode::Live;
static int64_t              s_entered_mode_us = 0;
/* Set by the watcher task when a debounced BOOT press is observed.
 * Cleared by tick() in the main loop after it advances the wheel. */
static std::atomic<bool>    s_press_pending{false};
static bool                 s_started = false;

static const char *mode_name(Mode m) {
    switch (m) {
        case Mode::Live:     return "live";
        case Mode::Splash:   return "splash";
        case Mode::Settings: return "settings";
        case Mode::Actions:  return "actions";
        default:             return "?";
    }
}

/* ─── Settings-screen action buttons ───────────────────────────────
 *
 * Each tappable region renders a 1-px bordered rect with its label
 * centred inside. Hit-tests resolve via dispatch_touch() at the
 * coords stored here. Coordinates are panel pixels.
 *
 * Buttons are zero-sized when not laid out (current() != Settings),
 * so dispatch_touch trivially misses on any other screen. */
struct Button {
    int x, y, w, h;
    TouchResult action;
    const char *label;
};
/* Single Settings button today. The list is sized for headroom; new
 * actions just append + bump the count. */
static constexpr int MAX_BUTTONS = 4;
static Button   s_buttons[MAX_BUTTONS] = {};
static int      s_button_count = 0;

static void clear_buttons(void) {
    s_button_count = 0;
    for (int i = 0; i < MAX_BUTTONS; i++) s_buttons[i] = {};
}

/* Background watcher: 25 ms cadence falling-edge detection on BOOT.
 * Latches s_press_pending so the (slow) main loop can consume the
 * event on its next tick. Without this, presses that fit entirely
 * between two main-loop ticks were lost. */
static void watcher_task(void *) {
    int prev_level = gpio_get_level((gpio_num_t)MOCHI_BOOT_BUTTON_GPIO);
    int debounce = 0;
    while (true) {
        if (debounce > 0) debounce--;
        const int level = gpio_get_level((gpio_num_t)MOCHI_BOOT_BUTTON_GPIO);
        /* Falling edge with active-low pull-up = press. */
        if (level == 0 && prev_level == 1 && debounce == 0) {
            debounce = DEBOUNCE_TICKS;
            /* Latch — the main loop will consume on its next pass.
             * Multiple presses within a single main-loop tick still
             * register as one (intent: don't double-advance). */
            s_press_pending.store(true, std::memory_order_release);
            ESP_LOGI(TAG, "BOOT press latched");
        }
        prev_level = level;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void init(epaper_driver_display * /*epd*/) {
    /* The BOOT pin is configured for input + pull-up by
     * boot_button_init() in main.cpp. */
    s_mode = Mode::Live;
    s_entered_mode_us = 0;
    s_press_pending.store(false, std::memory_order_release);
    if (!s_started) {
        s_started = true;
        xTaskCreatePinnedToCore(watcher_task, "dev_menu_btn",
            2048, nullptr, 2, nullptr, 1);
        ESP_LOGI(TAG, "watcher started (BOOT poll %d ms)", POLL_MS);
    }
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

TouchResult dispatch_touch(int x, int y) {
    if (s_mode == Mode::Live) return TouchResult::None;
    for (int i = 0; i < s_button_count; i++) {
        const Button &b = s_buttons[i];
        if (x >= b.x && x < b.x + b.w &&
            y >= b.y && y < b.y + b.h) {
            ESP_LOGI(TAG, "button hit: '%s'", b.label);
            return b.action;
        }
    }
    return TouchResult::None;
}

/* Advance helper: Live → Splash, then cycle through the rest of the
 * non-Live modes. Wraps at the end back to the FIRST debug mode
 * (Splash) — never auto-returns to Live; that path is the inactivity
 * timeout. */
static Mode advance(Mode m) {
    int next = static_cast<int>(m) + 1;
    if (m == Mode::Live) return Mode::Splash;
    if (next >= static_cast<int>(Mode::_Count)) return Mode::Splash;
    return static_cast<Mode>(next);
}

/* ─── Mode renderers ───────────────────────────────────────────────
 *
 * Each mode renders a complete frame and pushes it via the partial
 * refresh path so cycling is fast. The "previous frame" buffer is
 * already seeded by main.cpp's startup full refresh. */

static void render_splash(epaper_driver_display *epd, bool paired,
                          const char *pet_name, const char *version) {
    /* Reuse the boot path verbatim — this is exactly what we want to
     * review. render_boot_splash already places `version` via the
     * pack's STATUS text zone (or the default banner when the pack
     * has no status zone), so we don't add a separate
     * overlay_boot_version on top — that would duplicate the string
     * with a filled background and obscure the artwork. */
    const char *name = pet_name && *pet_name ? pet_name : "Mochi";
    const char *ver  = version && *version  ? version  : "?";
    epd_ui::render_boot_splash(epd, name, ver, paired);
    epd->EPD_Init_Partial();
    epd->EPD_DisplayPart();
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

/* Stamp a 1-px border around a rect using the slow per-pixel API. We
 * only call this on the Settings screen, where pre-render cost is
 * paid once per BOOT-press. */
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
                            TouchResult action, const char *label) {
    if (s_button_count >= MAX_BUTTONS) return;
    s_buttons[s_button_count++] = { x, y, w, h, action, label };
}

static void render_settings(epaper_driver_display *epd,
                            const char *pet_name, const char *version,
                            const char *ip_str, const char *ssid,
                            int net_phase, int batt_pct) {
    epd_ui::clear(epd);
    clear_buttons();

    epd_ui::draw_text_centered(epd, 4, 2, "SETTINGS");

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
    snprintf(line, sizeof(line), "ssid %s", ssid && *ssid ? ssid : "-");
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;

    /* Heap & PSRAM — useful when chasing alloc failures. */
    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psr = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snprintf(line, sizeof(line), "ram  %uK", (unsigned)(free_int / 1024));
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;
    snprintf(line, sizeof(line), "psr  %uK", (unsigned)(free_psr / 1024));
    epd_ui::draw_text(epd, 4, y, 1, line); y += 10;
    snprintf(line, sizeof(line), "batt %d%%", batt_pct);
    epd_ui::draw_text(epd, 4, y, 1, line); y += 14;

    /* Read-only screen — the tappable actions live on the next wheel
     * position. BOOT advances; 60 s inactivity (or a touch) returns
     * to the live pet. */
    epd_ui::draw_text(epd, 4, MOCHI_EPD_HEIGHT - 10, 1,
                      "BOOT: Actions  60s exit");

    epd->EPD_Init_Partial();
    epd->EPD_DisplayPart();
}

/* Actions screen — a vertical stack of tappable buttons. Each is a
 * 1-px bordered full-width rect with a centred label; dispatch_touch
 * resolves a tap to the button's TouchResult, and main.cpp performs
 * the action (most reboot, so they exit the wheel implicitly). */
static void render_actions(epaper_driver_display *epd) {
    epd_ui::clear(epd);
    clear_buttons();

    epd_ui::draw_text_centered(epd, 4, 2, "ACTIONS");

    /* Buttons fill the panel below the title down to the hint line.
     * Five generous (28-px) targets fit on the 200-px panel. */
    static const struct { TouchResult action; const char *label; } items[] = {
        { TouchResult::ChangeWifi,    "Change WiFi"     },
        { TouchResult::ForgetWifi,    "Forget WiFi"     },
        { TouchResult::UpdateNow,     "Update now"      },
        { TouchResult::RePair,        "Re-pair device"  },
        { TouchResult::OpenKeyPortal, "OpenAI key"      },
    };
    constexpr int N = (int)(sizeof(items) / sizeof(items[0]));
    constexpr int BTN_MARGIN = 2;
    constexpr int BTN_W = MOCHI_EPD_WIDTH - 2 * BTN_MARGIN;
    constexpr int BTN_H = 28;
    constexpr int BTN_GAP = 3;
    constexpr int TOP_Y = 24;

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

    epd_ui::draw_text(epd, 4, MOCHI_EPD_HEIGHT - 9, 1, "BOOT next  tap=do");

    epd->EPD_Init_Partial();
    epd->EPD_DisplayPart();
}

static void render_mode(epaper_driver_display *epd, Mode m,
                        bool paired, const char *pet_name,
                        const char *version, const char *ip_str,
                        const char *ssid, int net_phase, int batt_pct) {
    switch (m) {
        case Mode::Splash:
            render_splash(epd, paired, pet_name, version);
            break;
        case Mode::Settings:
            render_settings(epd, pet_name, version, ip_str, ssid,
                            net_phase, batt_pct);
            break;
        case Mode::Actions:
            render_actions(epd);
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
     * wheel exactly once (intentional). */
    const bool pressed = s_press_pending.exchange(false, std::memory_order_acq_rel);

    if (pressed) {
        const Mode next = advance(s_mode);
        ESP_LOGI(TAG, "BOOT press: %s → %s",
            mode_name(s_mode), mode_name(next));
        /* Buttons live only while their owning screen is up. Clear
         * before re-rendering so a stale Settings hit-rect can't
         * leak into a touch dispatched against the next screen. */
        clear_buttons();
        s_mode = next;
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
