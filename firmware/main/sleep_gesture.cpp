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

/* Poll fast enough to catch a short tap's falling + rising edges
 * reliably (a kid's tap is ~80–150 ms). */
static constexpr int POLL_MS = 30;
/* A "tap" is a PWR press↘release no longer than this. Longer presses
 * are ignored — PWR-alone has no hold action now that the key portal
 * lives in Settings. */
static constexpr int TAP_MAX_MS = 1000;
/* Ignore taps for this long after the watcher starts so the PWR press
 * that wakes the device from deep sleep can't immediately re-sleep it. */
static constexpr int STARTUP_GRACE_MS = 1500;

/* Grace window between "main observes requested()" and "watcher
 * fires fallback render". Main polls touch events every 1000 ms,
 * so anything > 1000 ms gives main a full poll cycle to claim the
 * gesture before the fallback fires. 1500 ms is comfortable. */
static constexpr int HANDOFF_GRACE_MS = 1500;

static bool s_started = false;
static volatile bool s_requested = false;
static volatile bool s_handled = false;
static epaper_driver_display *s_epd = nullptr;

/* Both buttons are active-low (internal pull-up; pressed reads 0). */
static bool pwr_pressed()  { return gpio_get_level(MOCHI_PWR_BUTTON_GPIO) == 0; }
static bool boot_pressed() { return gpio_get_level(MOCHI_BOOT_BUTTON_GPIO) == 0; }

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
    /*
     * Single-tap detector. A tap is a PWR press↘release where:
     *   - the press lasted ≤ TAP_MAX_MS (a deliberate tap, not a hold),
     *   - BOOT was never down during the press (so the PWR+BOOT factory-
     *     reset combo is never misread as a sleep tap),
     *   - we're past the startup grace (so the PWR press that woke the
     *     device from deep sleep can't immediately re-sleep it).
     * Edge-triggered: a PWR already held when the watcher starts (a
     * wake-hold) is ignored until released and tapped afresh.
     */
    bool was_pressed = false;
    int  press_ms = 0;
    bool press_clean = false;   /* BOOT stayed up for this whole press */
    int  grace_ms = STARTUP_GRACE_MS;

    while (true) {
        if (grace_ms > 0) grace_ms -= POLL_MS;
        const bool pwr  = pwr_pressed();
        const bool boot = boot_pressed();

        if (pwr) {
            if (!was_pressed) {       /* falling edge — press begins */
                press_ms = 0;
                press_clean = true;
            }
            press_ms += POLL_MS;
            if (boot) press_clean = false;             /* it's a combo */
            if (press_ms > TAP_MAX_MS) press_clean = false;  /* a hold */
        } else {
            if (was_pressed) {        /* rising edge — press released */
                if (press_clean && grace_ms <= 0 && !s_requested) {
                    ESP_LOGI(TAG, "PWR tap — sleep; handing off to main");
                    s_requested = true;

                    /* Hand-off window. Main's touch loop polls every
                     * ~1000 ms; if it's there it'll see requested(),
                     * mark_handled(), and run the rich render before
                     * committing. If no one claims it within
                     * HANDOFF_GRACE_MS (provisioning, pair-wait, a halt
                     * loop), fire the fallback ourselves so the gesture
                     * reaches every screen. */
                    int waited = 0;
                    while (waited < HANDOFF_GRACE_MS && !s_handled) {
                        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
                        waited += POLL_MS;
                    }
                    if (s_handled) {
                        /* Main owns the commit. Park forever — the SoC
                         * is about to deep-sleep and this task dies. */
                        while (true) vTaskDelay(portMAX_DELAY);
                    }
                    ESP_LOGI(TAG, "no handler claimed tap in %d ms — fallback",
                        HANDOFF_GRACE_MS);
                    render_fallback();
                    commit_sleep();
                }
            }
            press_ms = 0;
            press_clean = false;
        }
        was_pressed = pwr;
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
    ESP_LOGI(TAG, "watching for PWR tap → sleep");
}

bool requested(void) {
    return s_requested;
}

}  /* namespace sleep_gesture */
