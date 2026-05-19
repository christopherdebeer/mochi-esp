#include "sleep_gesture.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "board_pins.h"

namespace sleep_gesture {

static const char *TAG = "sleep_gesture";

static constexpr int POLL_MS        = 100;
static constexpr int HOLD_TARGET_MS = 3000;

static bool s_started = false;
static volatile bool s_requested = false;

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

static void task(void *) {
    int held_ms = 0;
    int last_logged = -1;
    while (true) {
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
                ESP_LOGI(TAG, "PWR hold committed — main task will sleep");
                s_requested = true;
                /* Don't sleep here; main does that after rendering.
                 * Keep polling so we can re-fire if main misses
                 * (it shouldn't — sleep is one-shot per boot). */
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

void start(void) {
    if (s_started) return;
    s_started = true;
    xTaskCreatePinnedToCore(task, "sleep_gesture",
        4096, nullptr, 2, nullptr, 1);
    ESP_LOGI(TAG, "watching for PWR 3 s hold");
}

bool requested(void) {
    return s_requested;
}

}  /* namespace sleep_gesture */
