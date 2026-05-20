#include "sleep_gesture.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "board_pins.h"
#include "epd_ui.h"

namespace sleep_gesture {

static const char *TAG = "sleep_gesture";

static constexpr int POLL_MS        = 100;
static constexpr int HOLD_TARGET_MS = 3000;

/* Triple-tap window. Three press-release cycles must fit inside
 * TRIPLE_TAP_WINDOW_MS counted from the first press; any press that
 * stays held longer than TRIPLE_TAP_MAX_HOLD_MS aborts the sequence
 * (so the sleep-hold gesture can never accidentally double as a
 * tap). 400 ms is comfortably below the 3000 ms sleep threshold. */
static constexpr int TRIPLE_TAP_WINDOW_MS   = 1500;
static constexpr int TRIPLE_TAP_MAX_HOLD_MS = 400;

/* Grace window between "main observes requested()" and "watcher
 * fires fallback render". Main polls touch events every 1000 ms,
 * so anything > 1000 ms gives main a full poll cycle to claim the
 * gesture before the fallback fires. 1500 ms is comfortable. */
static constexpr int HANDOFF_GRACE_MS = 1500;

static bool s_started = false;
static volatile bool s_requested = false;
static volatile bool s_handled = false;
static volatile bool s_triple_tap_pending = false;
static epaper_driver_display *s_epd = nullptr;

static bool pwr_held_alone() {
    /* Both PWR and BOOT are active-low (pull-up to 3.3V, pressed
     * pulls them to ground). We want PWR pressed AND BOOT released. */
    return gpio_get_level(MOCHI_PWR_BUTTON_GPIO) == 0
        && gpio_get_level(MOCHI_BOOT_BUTTON_GPIO) == 1;
}

[[noreturn]] void commit_sleep(void) {
    /*
     * Disable any default wake sources, then enable just the two
     * buttons via ext1 (any pin in the mask going low). PWR is
     * what we expect; BOOT is included so the device is also
     * recoverable through the BOOT path if PWR ever fails.
     *
     * Holding the VBAT_PWR rail (GPIO 17) across deep sleep keeps
     * the battery divider in a defined state — without rtc_gpio_
     * hold_en the pin floats and the divider can leak.
     */
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    const uint64_t mask =
        (1ULL << MOCHI_PWR_BUTTON_GPIO) |
        (1ULL << MOCHI_BOOT_BUTTON_GPIO);
    ESP_ERROR_CHECK(
        esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW));
    ESP_ERROR_CHECK(rtc_gpio_hold_en((gpio_num_t)MOCHI_VBAT_SENSE_GPIO));

    ESP_LOGI(TAG, "deep sleep — wake on PWR or BOOT (any low)");
    esp_deep_sleep_start();
    /* Unreachable. esp_deep_sleep_start does not return. */
    while (true) { vTaskDelay(portMAX_DELAY); }
}

/* Fallback "Asleep" screen rendered by the watcher itself when no
 * one claims the gesture within HANDOFF_GRACE_MS. Used during
 * provisioning, pair-wait, and any halt-loop in main — all the
 * paths where the rich pet-sprite render isn't available because
 * main never reaches its polling loop. */
static void render_fallback(void) {
    if (!s_epd) return;
    epd_ui::clear(s_epd);
    epd_ui::draw_text_centered(s_epd,  60, 2, "Asleep");
    epd_ui::draw_text_centered(s_epd, 110, 1, "PWR to wake");
    s_epd->EPD_Init();
    s_epd->EPD_Display();
}

static void task(void *) {
    int held_ms = 0;
    int last_logged = -1;

    /* Triple-tap state. tap_count = falling edges seen so far in the
     * current window; window_ms = ms since the first edge; was_pressed
     * tracks the previous-poll level so we count edges, not levels. */
    int tap_count = 0;
    int window_ms = 0;
    int press_ms = 0;          /* duration of the current press */
    bool was_pressed = false;
    bool window_aborted = false;  /* true if a press ran too long */

    while (true) {
        bool pressed_now = (gpio_get_level(MOCHI_PWR_BUTTON_GPIO) == 0);

        /* --- Triple-tap detector ------------------------------- */
        if (tap_count > 0 || pressed_now) {
            window_ms += POLL_MS;
        }
        if (pressed_now) {
            press_ms += POLL_MS;
            if (!was_pressed) {
                /* Falling edge — count this press. The first edge
                 * starts the window; subsequent edges accumulate. */
                tap_count++;
                press_ms = POLL_MS;
            }
            if (press_ms > TRIPLE_TAP_MAX_HOLD_MS) {
                /* Held too long for a tap; abort the window so the
                 * sleep-hold doesn't also count as a triple-tap. */
                window_aborted = true;
            }
        } else {
            press_ms = 0;
        }
        if (tap_count >= 3 && !window_aborted &&
            window_ms <= TRIPLE_TAP_WINDOW_MS && !pressed_now) {
            ESP_LOGI(TAG, "PWR triple-tap detected");
            s_triple_tap_pending = true;
            tap_count = 0;
            window_ms = 0;
            window_aborted = false;
        } else if (window_ms > TRIPLE_TAP_WINDOW_MS ||
                   (window_aborted && !pressed_now)) {
            tap_count = 0;
            window_ms = 0;
            window_aborted = false;
        }
        was_pressed = pressed_now;

        /* --- Existing sleep-hold detector ---------------------- */
        if (pwr_held_alone()) {
            held_ms += POLL_MS;
            const int s_remaining = (HOLD_TARGET_MS - held_ms + 999) / 1000;

            /* Log progress once per second so the USB monitor sees
             * the gesture being detected. The e-paper isn't
             * updated until commit because mid-hold partial
             * refreshes are wasteful — and the user feedback for
             * "I'm holding correctly" is the LED on PWR + the
             * fact that nothing else has happened. */
            if (s_remaining != last_logged) {
                ESP_LOGI(TAG, "PWR held: %d s remaining", s_remaining);
                last_logged = s_remaining;
            }

            if (held_ms >= HOLD_TARGET_MS && !s_requested) {
                ESP_LOGI(TAG, "PWR hold committed — handing off to main");
                s_requested = true;

                /* Hand-off window. Main's touch loop polls every
                 * ~1000 ms; if it's there it'll see requested(),
                 * call mark_handled(), and run the rich render
                 * before committing. If we don't see mark_handled
                 * within HANDOFF_GRACE_MS, main isn't around
                 * (provisioning, pair-wait, halt loop) — fire the
                 * fallback ourselves so the gesture reaches every
                 * screen, not just the main one. */
                int waited = 0;
                while (waited < HANDOFF_GRACE_MS && !s_handled) {
                    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                    waited += POLL_MS;
                }
                if (s_handled) {
                    /* Main owns the commit. Park forever — the
                     * SoC is about to deep-sleep and this task
                     * dies with it. */
                    while (true) vTaskDelay(portMAX_DELAY);
                }
                ESP_LOGI(TAG, "no handler claimed gesture in %d ms — fallback",
                    HANDOFF_GRACE_MS);
                render_fallback();
                commit_sleep();
            }
        } else {
            if (held_ms > 0 && held_ms >= 500) {
                ESP_LOGI(TAG, "PWR hold cancelled (held %d ms)", held_ms);
            }
            held_ms = 0;
            last_logged = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void mark_handled(void) {
    s_handled = true;
}

void start(epaper_driver_display *epd) {
    if (s_started) return;
    s_started = true;
    s_epd = epd;
    xTaskCreatePinnedToCore(task, "sleep_gesture",
        4096, nullptr, 2, nullptr, 1);
    ESP_LOGI(TAG, "watching for PWR 3 s hold");
}

bool requested(void) {
    return s_requested;
}

bool triple_tap_consume(void) {
    if (!s_triple_tap_pending) return false;
    s_triple_tap_pending = false;
    return true;
}

}  /* namespace sleep_gesture */
