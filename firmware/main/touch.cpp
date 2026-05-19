#include "touch.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#include "board_pins.h"
#include "i2c_bus.h"
#include "ft6336_bsp.h"

namespace touch {

static const char *TAG = "touch";

static QueueHandle_t s_evt_queue = nullptr;
static I2cFt6336Dev *s_ft = nullptr;
static bool s_inited = false;

/*
 * Diagnostic poll task. Runs every 100 ms and pushes a marker onto
 * the event queue any time the FT6336 reports a finger down — same
 * shape as the ISR path. Lets us tell ISR-broken from
 * device-broken: if poll reports touches but the ISR never fires,
 * GPIO 21 isn't actually connected to FT6336_INT on this board
 * rev. If neither sees anything, the device or its address is
 * wrong. Polling at 10 Hz costs ~0.5 mA, fine for bring-up.
 */
static TaskHandle_t s_poll_task = nullptr;

/*
 * ISR handler: pushes the GPIO number onto the queue. We don't read
 * I²C from the ISR because i2c_master_transmit_receive can block on
 * a mutex; the actual read happens in wait_event() on the caller's
 * task.
 */
static void IRAM_ATTR on_touch_int(void *arg) {
    BaseType_t hp_woken = pdFALSE;
    uint32_t marker = 1;  /* ISR source */
    xQueueSendFromISR(s_evt_queue, &marker, &hp_woken);
    if (hp_woken) portYIELD_FROM_ISR();
}

/*
 * Probe a single I²C address with a 1-byte read. Returns true if a
 * device acknowledged. Independent of any vendor wrapper — talks
 * directly to the bus we just brought up. Used as a one-shot
 * diagnostic at init.
 */
static bool i2c_probe(i2c_master_bus_handle_t bus, uint8_t addr) {
    /*
     * ESP-IDF v5 I²C master API doesn't expose a "ping" primitive,
     * but i2c_master_probe() does exactly what we want: drives an
     * address phase and reports the ACK bit. 50 ms timeout is
     * generous; the FT6336 should ACK in microseconds when present.
     */
    esp_err_t err = i2c_master_probe(bus, addr, 50);
    return err == ESP_OK;
}

static void poll_task(void *) {
    uint16_t last_x = 0xFFFF, last_y = 0xFFFF;
    while (true) {
        uint16_t x = 0, y = 0;
        if (s_ft && s_ft->GetTouchPoint(&x, &y)) {
            /*
             * Edge-trigger: only push a marker when this is a
             * different point from the previous (or the first
             * after release). Otherwise a finger held down would
             * spam the queue at 10 Hz.
             */
            if (x != last_x || y != last_y) {
                uint32_t marker = 2;  /* poll source */
                xQueueSend(s_evt_queue, &marker, 0);
                last_x = x;
                last_y = y;
            }
        } else {
            last_x = 0xFFFF;
            last_y = 0xFFFF;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void init() {
    if (s_inited) return;
    s_inited = true;

    /* I²C bus first — project singleton via codec_board. Future
     * RTC + SHTC3 drivers, plus codec_board's own ES8311 init,
     * all attach to this same handle. See i2c_bus.h. */
    auto bus_handle = i2c_bus::handle();
    assert(bus_handle);

    /*
     * Bus scan: probe each candidate device address before we commit
     * to FT6336 init. If 0x38 doesn't ACK we know touch is broken at
     * the wire level (wrong address / wiring / device variant) and
     * the ISR + poll paths below will fail the same way.
     */
    const uint8_t probes[] = {
        MOCHI_TOUCH_I2C_ADDR,   /* 0x38 — FT6336 expected */
        0x15,                   /* GT911 alternate touch addr */
        0x5D,                   /* GT911 default                */
        0x51,                   /* RTC PCF85063                 */
        0x70,                   /* SHTC3                        */
    };
    for (uint8_t addr : probes) {
        bool ack = i2c_probe(bus_handle, addr);
        ESP_LOGI(TAG, "i2c probe 0x%02X: %s", addr, ack ? "ACK" : "no");
    }

    s_ft = I2cFt6336Dev::requestInstance(
        bus_handle,
        MOCHI_TOUCH_I2C_ADDR,
        MOCHI_EPD_WIDTH, MOCHI_EPD_HEIGHT);
    assert(s_ft);
    s_ft->Ft6336_Reset(MOCHI_TOUCH_RST_GPIO);

    /* Set up the INT pin as a falling-edge interrupt. The ISR
     * service may already be installed by other code; ignore the
     * "already installed" error. */
    gpio_config_t cfg = {};
    cfg.intr_type = GPIO_INTR_NEGEDGE;
    cfg.pin_bit_mask = 1ULL << MOCHI_TOUCH_INT_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));

    s_evt_queue = xQueueCreate(8, sizeof(uint32_t));
    assert(s_evt_queue);

    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    ESP_ERROR_CHECK(gpio_isr_handler_add(
        MOCHI_TOUCH_INT_GPIO, on_touch_int, nullptr));

    /* Poll task as a parallel diagnostic path. Both ISR and poll
     * push markers onto the same queue with different source IDs
     * so wait_event can log which path delivered the event. */
    xTaskCreate(poll_task, "touch_poll", 4096, nullptr, 4, &s_poll_task);

    ESP_LOGI(TAG, "ready (INT=GP%d, RST=GP%d, addr=0x%02X)",
        MOCHI_TOUCH_INT_GPIO, MOCHI_TOUCH_RST_GPIO,
        MOCHI_TOUCH_I2C_ADDR);
}

bool wait_event(Event *out, int timeout_ms) {
    if (!s_inited || !out) return false;

    TickType_t timeout = (timeout_ms < 0)
        ? portMAX_DELAY
        : pdMS_TO_TICKS(timeout_ms);

    uint32_t marker;
    if (xQueueReceive(s_evt_queue, &marker, timeout) != pdTRUE) {
        return false;
    }
    uint32_t first_marker = marker;
    /* Drain any queued markers from either source. */
    while (xQueueReceive(s_evt_queue, &marker, 0) == pdTRUE) { /* drain */ }

    uint16_t x = 0, y = 0;
    if (s_ft->GetTouchPoint(&x, &y) == 0) {
        /* Event fired but FT6336 reports no point — finger-up edge. */
        return false;
    }

    ESP_LOGI(TAG, "event src=%s x=%u y=%u",
        first_marker == 1 ? "ISR" : "poll",
        (unsigned)x, (unsigned)y);
    out->x = x;
    out->y = y;
    return true;
}

bool current_point(Event *out) {
    if (!s_inited || !out || !s_ft) return false;
    uint16_t x = 0, y = 0;
    if (!s_ft->GetTouchPoint(&x, &y)) return false;
    out->x = x;
    out->y = y;
    return true;
}

}  /* namespace touch */
