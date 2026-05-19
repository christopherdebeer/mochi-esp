#include "factory_reset.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "board_pins.h"
#include "epd_ui.h"
#include "nvs_creds.h"
#include "pair_creds.h"
#include "openai_key.h"

namespace factory_reset {

static const char *TAG = "factory_reset";

static constexpr int POLL_MS         = 100;
static constexpr int HOLD_TARGET_MS  = 10000;
static constexpr int RENDER_EVERY_MS = 2000;
static bool s_started = false;
static epaper_driver_display *s_epd = nullptr;

/* The PWR button is wired with internal pull-up like BOOT — both
 * read 0 when pressed. We don't reconfigure them here; touch.cpp
 * already configured BOOT as INPUT + PULLUP, and our boot path
 * does the same for the BOOT button via boot_button_init() in
 * main.cpp. PWR needs configuring once (and only once). */
static void pwr_button_init() {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << MOCHI_PWR_BUTTON_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static bool both_pressed() {
    return gpio_get_level(MOCHI_BOOT_BUTTON_GPIO) == 0
        && gpio_get_level(MOCHI_PWR_BUTTON_GPIO)  == 0;
}

static void render_countdown(int seconds_remaining) {
    if (!s_epd) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "Hold %d more s", seconds_remaining);
    /* Reuse epd_ui primitives. We compose a screen by hand because
     * the existing epd_ui screens don't fit this exact shape. */
    epd_ui::clear(s_epd);
    epd_ui::draw_text_centered(s_epd, 30, 2, "Factory");
    epd_ui::draw_text_centered(s_epd, 60, 2, "reset");
    epd_ui::draw_text_centered(s_epd, 110, 1, buf);
    epd_ui::draw_text_centered(s_epd, 140, 1, "Release to cancel");
    s_epd->EPD_Init_Partial();
    s_epd->EPD_DisplayPart();
}

static void render_committing() {
    if (!s_epd) return;
    epd_ui::clear(s_epd);
    epd_ui::draw_text_centered(s_epd, 60, 2, "Wiping...");
    epd_ui::draw_text_centered(s_epd, 100, 1, "Mochi will reboot");
    epd_ui::draw_text_centered(s_epd, 116, 1, "for setup.");
    s_epd->EPD_Init_Partial();
    s_epd->EPD_DisplayPart();
}

static void task(void *) {
    pwr_button_init();
    int held_ms = 0;
    int last_rendered_remaining = -1;

    while (true) {
        if (both_pressed()) {
            held_ms += POLL_MS;
            int remaining = (HOLD_TARGET_MS - held_ms + 999) / 1000;
            if (remaining < 0) remaining = 0;

            /* Update the screen at most once per RENDER_EVERY_MS.
             * E-paper partial refresh is ~300 ms; rendering more
             * often would just queue up SPI traffic the user can't
             * see fast enough anyway. */
            if (held_ms >= RENDER_EVERY_MS &&
                remaining != last_rendered_remaining &&
                (held_ms % RENDER_EVERY_MS) < POLL_MS) {
                ESP_LOGI(TAG, "factory-reset hold: %d ms (%d s remaining)",
                    held_ms, remaining);
                render_countdown(remaining);
                last_rendered_remaining = remaining;
            }

            if (held_ms >= HOLD_TARGET_MS) {
                ESP_LOGW(TAG, "factory reset committed — wiping NVS");
                render_committing();
                pair_creds_clear();
                openai_key_clear();
                nvs_creds_clear_all();
                /* Brief delay so the panel finishes its partial
                 * refresh before we reboot — otherwise the user
                 * sees the device reboot mid-render. */
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
            }
        } else {
            if (held_ms > 0) {
                if (held_ms >= RENDER_EVERY_MS) {
                    /* Partial-cancel: only worth logging if the
                     * hold was long enough that a screen update
                     * happened. Random brushes against both
                     * buttons shouldn't spam the log. */
                    ESP_LOGI(TAG, "factory-reset cancelled (held %d ms)",
                        held_ms);
                }
                last_rendered_remaining = -1;
            }
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void start(epaper_driver_display *epd) {
    if (s_started) return;
    s_started = true;
    s_epd = epd;
    xTaskCreatePinnedToCore(task, "factory_reset",
        4096, nullptr, 2, nullptr, 1);
    ESP_LOGI(TAG, "watching for PWR+BOOT 10s hold");
}

}  /* namespace factory_reset */
