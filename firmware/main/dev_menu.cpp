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
        case Mode::Live:        return "live";
        case Mode::Splash:      return "splash";
        case Mode::Diagnostics: return "diag";
        default:                return "?";
    }
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
    }
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

static void render_diagnostics(epaper_driver_display *epd,
                               const char *pet_name, const char *version,
                               const char *ip_str, const char *ssid,
                               int net_phase, int batt_pct) {
    epd_ui::clear(epd);
    epd_ui::draw_text_centered(epd, 6, 2, "DIAGNOSTICS");

    char line[40];
    int y = 32;
    snprintf(line, sizeof(line), "fw  %s", version ? version : "?");
    epd_ui::draw_text(epd, 4, y, 1, line); y += 12;
    snprintf(line, sizeof(line), "pet %s", pet_name ? pet_name : "?");
    epd_ui::draw_text(epd, 4, y, 1, line); y += 12;
    snprintf(line, sizeof(line), "net %s", phase_label(net_phase));
    epd_ui::draw_text(epd, 4, y, 1, line); y += 12;
    snprintf(line, sizeof(line), "ip  %s", ip_str && *ip_str ? ip_str : "-");
    epd_ui::draw_text(epd, 4, y, 1, line); y += 12;
    snprintf(line, sizeof(line), "ssid %s", ssid && *ssid ? ssid : "-");
    epd_ui::draw_text(epd, 4, y, 1, line); y += 12;

    /* Heap & PSRAM — useful when chasing alloc failures. free / total
     * in KB to fit the line within ~25 chars. */
    const size_t free_int  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psr  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    snprintf(line, sizeof(line), "ram %uK free", (unsigned)(free_int / 1024));
    epd_ui::draw_text(epd, 4, y, 1, line); y += 12;
    snprintf(line, sizeof(line), "psr %uK free", (unsigned)(free_psr / 1024));
    epd_ui::draw_text(epd, 4, y, 1, line); y += 12;

    snprintf(line, sizeof(line), "batt %d%%", batt_pct);
    epd_ui::draw_text(epd, 4, y, 1, line); y += 14;

    /* Wheel position chip at the bottom: reduces "did the press
     * register?" anxiety on the partial-refresh cadence. */
    epd_ui::draw_text(epd, 4, MOCHI_EPD_HEIGHT - 12, 1, "BOOT next  5s exit");

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
        case Mode::Diagnostics:
            render_diagnostics(epd, pet_name, version, ip_str, ssid,
                               net_phase, batt_pct);
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
        s_mode = next;
        s_entered_mode_us = now_us;
        render_mode(epd, s_mode, paired, pet_name, version,
                    ip_str, ssid, net_phase, batt_pct);
        changed = true;
    } else if (s_mode != Mode::Live &&
               (now_us - s_entered_mode_us) >= INACTIVITY_US) {
        ESP_LOGI(TAG, "inactivity timeout → live");
        s_mode = Mode::Live;
        changed = true;
    }
    return changed;
}

}  /* namespace dev_menu */
