#include "lvgl_port.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lvgl.h"

#include "touch.h"
#include "board_pins.h"

static const char *TAG = "lvgl_port";

/* Panel geometry. The Waveshare 1.54" V2 is 200×200; epd_ui hardcodes
 * a 25-byte stride (200/8). We mirror those constants here so a future
 * panel swap touches one place — but they MUST match the e-paper
 * driver's expectations or EPD_LoadBuffer's bytes won't line up. */
static constexpr int PANEL_W = MOCHI_EPD_WIDTH;
static constexpr int PANEL_H = MOCHI_EPD_HEIGHT;
static constexpr int PANEL_STRIDE = (PANEL_W + 7) / 8;
static constexpr int PANEL_BYTES = PANEL_STRIDE * PANEL_H;

/* Refresh policy. e-paper accumulates ghosting on repeated partial
 * updates, so we *do* need a periodic full refresh to clear it —
 * but only at human-scale cadence. v0.1.7's first cut counted every
 * partial flush and full-refreshed every 6th; combined with the new
 * 33 Hz dispatcher that fired ~5 fulls/sec and made the menu look
 * like a strobe. Now: explicit force_full_refresh on screen swaps,
 * and an auto-trigger only after a long quiescent run of partials
 * (32 in a row). Tuned so a static menu doesn't randomly re-flash
 * mid-read. */
static constexpr int FULL_REFRESH_EVERY = 32;

/* Tick. LVGL needs lv_tick_inc called periodically (default doc says
 * 5 ms); a FreeRTOS gptimer or esp_timer is the canonical source. */
static constexpr int TICK_PERIOD_MS = 5;

/* Dedicated lv_timer_handler task period. We started by calling
 * lv_timer_handler() once per main-loop iteration (~1 Hz, 10 Hz
 * with voice) — but LVGL's drag-vs-tap discrimination relies on
 * indev_read_cb sampling many positions during a single finger
 * movement. At 1 Hz polling, every drag looks like a stationary
 * press and fires LV_EVENT_CLICKED on release — i.e. scrolling
 * the menu also tapped a button.
 *
 * 30 ms (~33 Hz) is enough to see ~30 movement samples during a
 * 1 s drag. The actual e-paper flush only happens when something's
 * dirty (and is throttled by the panel's own ~300 ms refresh
 * latency anyway), so the high poll rate doesn't cause display
 * thrash — it just feeds LVGL enough samples to discriminate. */
static constexpr int LV_TASK_PERIOD_MS = 30;
static constexpr int LV_TASK_STACK_BYTES = 8192;
static constexpr int LV_TASK_PRIO = 2;

static bool                 s_inited = false;
static epaper_driver_display *s_epd = nullptr;
static lv_display_t         *s_disp = nullptr;
static lv_indev_t           *s_indev = nullptr;
static esp_timer_handle_t    s_tick_timer = nullptr;
static TaskHandle_t          s_lv_task = nullptr;
/* Mutex serialising lv_timer_handler() against widget creation
 * (build_menu, refresh_info, etc.) running on the main task. LVGL's
 * own _lock/_unlock helpers live behind LV_USE_OS — we use a plain
 * SemaphoreHandle_t to keep the dependency surface small. */
static SemaphoreHandle_t     s_lv_mtx = nullptr;

/* Two LVGL draw buffers in PSRAM. LVGL renders into one while the
 * other is being flushed (double-buffering). For 1bpp this is two
 * 5 KB allocations — trivial against our 7 MB free PSRAM. */
static uint8_t              *s_buf_a = nullptr;
static uint8_t              *s_buf_b = nullptr;

static int                   s_partial_count = 0;
static volatile bool         s_force_full = false;

/* Minimum interval between EPD pushes. The Waveshare 1.54" V2 takes
 * ~300 ms for a partial refresh and ~600 ms for a full; firing more
 * often than that does nothing visually but bombards the panel
 * driver. With the 33 Hz LVGL dispatcher, jittery touch position
 * input would otherwise cause back-to-back partials that read on
 * hardware as a continuous flicker. 250 ms ≈ "as fast as the panel
 * can handle"; LVGL sees its flush_cb still fire (so it doesn't
 * back up its dirty list), we just NO-OP the EPD push. */
static constexpr int64_t     EPD_MIN_INTERVAL_US = 250 * 1000;
static int64_t               s_last_epd_us = 0;

/* Forward declarations — these are used by lvgl_port_init below
 * but defined later in the file (the dispatcher task) or by the
 * LVGL library itself (the malloc hooks). */
static void lv_task(void *arg);

/* ─── Memory hooks ──────────────────────────────────────────────────
 * LV_USE_STDLIB_MALLOC=LV_STDLIB_CUSTOM (set via Kconfig) — LVGL
 * calls these for every internal allocation. Routing to PSRAM keeps
 * internal RAM free for TLS handshakes. heap_caps_realloc handles
 * NULL->malloc and size 0 -> free for us. */
extern "C" void *lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
extern "C" void lv_free_core(void *p) {
    heap_caps_free(p);
}
extern "C" void *lv_realloc_core(void *p, size_t size) {
    return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
extern "C" void lv_mem_init(void) {}     /* no-op — heap_caps owns the pool */
extern "C" void lv_mem_deinit(void) {}
extern "C" void lv_mem_monitor_core(lv_mem_monitor_t * /*mon*/) {}

/* ─── Tick ──────────────────────────────────────────────────────────
 * esp_timer fires our callback in ISR context for short periods, but
 * lv_tick_inc is documented to be ISR-safe so we can stay in the
 * timer callback rather than dispatch to a task. */
static void tick_cb(void * /*arg*/) {
    lv_tick_inc(TICK_PERIOD_MS);
}

/* ─── Flush ─────────────────────────────────────────────────────────
 * LVGL hands us an `area` (clipped dirty rect) and a packed pixel
 * map. With LV_COLOR_DEPTH=1 the map is 1 bit per pixel, MSB-first.
 *
 * The Waveshare driver expects a full 200×200/8 = 5 000 B framebuffer
 * via EPD_LoadBuffer, then triggers a refresh. There's no partial-
 * region API on this driver, so even when LVGL only marked a small
 * dirty area we have to push the whole composite buffer. This is OK
 * because (a) we're already 1bpp, (b) the rendering bottleneck is
 * the panel itself (~300 ms for partial), not the byte copy.
 *
 * We assemble a static internal-RAM composite (zero-initialised to
 * white = 0xFF) on first flush of a screen, copy LVGL's region into
 * it, then push. Since LVGL's full-screen render mode (configured in
 * lvgl_port_init) gives us area = full panel anyway, the "merge into
 * persistent fb" code path is mostly defensive. */
static uint8_t s_composite[PANEL_BYTES];

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    /* LVGL 9 LV_COLOR_FORMAT_I1 packs an 8-byte palette header at the
     * start of px_map (2 colors × 4 bytes). Real pixel data starts at
     * offset 8. Earlier we memcpy'd the whole buffer, which shifted
     * everything right by 8 bytes = 64 pixels = 32 % of a 200 px panel
     * — visually, the menu wrapped around the right edge. Skip the
     * palette and copy from there.
     *
     * Bit packing inside the data is MSB-first (matches our e-paper). */
    static constexpr int I1_PALETTE_BYTES = 8;
    const uint8_t *pixels = px_map + I1_PALETTE_BYTES;
    const int x1 = area->x1, x2 = area->x2;
    const int y1 = area->y1, y2 = area->y2;
    const int w  = x2 - x1 + 1;
    const int row_bytes = (w + 7) / 8;

    if (x1 == 0 && x2 == PANEL_W - 1 && y1 == 0 && y2 == PANEL_H - 1) {
        /* Whole-screen flush — common case with full render mode. */
        memcpy(s_composite, pixels, PANEL_BYTES);
    } else {
        /* Partial — copy row-by-row into the composite at the right
         * offset. Rare with LV_DISPLAY_RENDER_MODE_FULL, but keeps
         * us correct if we ever flip to PARTIAL mode for speed. */
        for (int y = 0; y < y2 - y1 + 1; y++) {
            const uint8_t *src = pixels + y * row_bytes;
            uint8_t *dst = s_composite + (y1 + y) * PANEL_STRIDE + (x1 / 8);
            memcpy(dst, src, row_bytes);
        }
    }

    /* Rate-limit the actual hardware push. LVGL will keep calling
     * flush_cb whenever its dirty regions accumulate; on a 33 Hz
     * dispatcher with a held finger that's many calls per second,
     * which the panel can't handle (each refresh is ~300 ms). We
     * still tell LVGL we flushed (so it advances its draw buffer
     * pipeline), but only actually drive the EPD when enough wall
     * time has passed since the previous push. The visual cost is
     * a small "stale frame" window; the win is no continuous strobe
     * during a touch. */
    const int64_t now = esp_timer_get_time();
    const bool can_push = (now - s_last_epd_us) >= EPD_MIN_INTERVAL_US ||
                          s_force_full;

    if (can_push) {
        s_last_epd_us = now;
        s_epd->EPD_LoadBuffer(s_composite, PANEL_BYTES);

        bool do_full = s_force_full ||
                       (++s_partial_count % FULL_REFRESH_EVERY == 0);
        if (do_full) {
            s_force_full = false;
            s_partial_count = 0;
            ESP_LOGI(TAG, "full refresh (ghost-bust)");
            s_epd->EPD_Init();
            s_epd->EPD_Display();
        } else {
            s_epd->EPD_Init_Partial();
            s_epd->EPD_DisplayPart();
        }
    }

    /* Tell LVGL we're done; it'll release the draw buffer for the
     * next render. (Still tell it 'ready' even when we no-op'd the
     * push, so its pipeline doesn't stall.) */
    lv_display_flush_ready(disp);
}

/* ─── Touch indev ──────────────────────────────────────────────────
 * LVGL polls this — return the current finger-down position via the
 * non-consuming touch::current_point. We deliberately *don't* drain
 * touch::wait_event here: main.cpp's loop owns the queue (so it can
 * still see "a touch happened" → call dispatch_touch). LVGL doesn't
 * need the queue — it works fine off polled live state, since the
 * dispatcher runs at ~33 Hz and gets ~30 samples per finger-stay.
 *
 * The state-machine LVGL infers from the (PRESSED, x, y) → (RELEASED,
 * last_x, last_y) transitions is enough to fire LV_EVENT_CLICKED on
 * a stable tap, LV_EVENT_SCROLL on a drag, etc. */
static void indev_read_cb(lv_indev_t * /*indev*/, lv_indev_data_t *data) {
    static int16_t s_last_x = 0;
    static int16_t s_last_y = 0;

    touch::Event cur;
    if (touch::current_point(&cur)) {
        s_last_x = cur.x;
        s_last_y = cur.y;
        data->point.x = cur.x;
        data->point.y = cur.y;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }

    /* Released. Hold the last position so LVGL can still resolve the
     * release event onto whatever widget the press started on. */
    data->point.x = s_last_x;
    data->point.y = s_last_y;
    data->state = LV_INDEV_STATE_RELEASED;
}

/* ─── Public API ───────────────────────────────────────────────────*/

extern "C" void lvgl_port_init(epaper_driver_display *epd) {
    if (s_inited) return;

    if (!epd) {
        ESP_LOGE(TAG, "init: epd null");
        return;
    }
    s_epd = epd;

    lv_init();

    /* 1bpp draw buffers in PSRAM. PANEL_BYTES alone isn't quite enough
     * — LVGL's I1 format includes a small palette header inside the
     * buffer (LV_DRAW_BUF_ALIGN-byte chunk at the start). Round up to
     * the next 64 B and add 16 B of LVGL header headroom; PSRAM is
     * cheap. */
    const size_t buf_len = PANEL_BYTES + 16;
    s_buf_a = (uint8_t *)heap_caps_malloc(buf_len,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_buf_b = (uint8_t *)heap_caps_malloc(buf_len,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf_a || !s_buf_b) {
        ESP_LOGE(TAG, "draw buffer alloc failed");
        return;
    }

    s_disp = lv_display_create(PANEL_W, PANEL_H);
    if (!s_disp) {
        ESP_LOGE(TAG, "lv_display_create failed");
        return;
    }
    /* I1 = 1bpp indexed (palette set by LVGL: 0 = black, 1 = white).
     * Our composite is "1 = white" matching the e-paper driver, so
     * LVGL's default 1bpp out is the right polarity for us. */
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_I1);
    lv_display_set_buffers(s_disp, s_buf_a, s_buf_b, buf_len,
        LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    /* Touch indev */
    s_indev = lv_indev_create();
    if (!s_indev) {
        ESP_LOGE(TAG, "lv_indev_create failed");
        return;
    }
    lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_indev, indev_read_cb);
    lv_indev_set_display(s_indev, s_disp);

    /* Tick timer. esp_timer is one-shot or periodic; periodic is the
     * right shape since LVGL never asks us to stop. The callback runs
     * in the esp_timer task (priority 22) — high enough that LVGL
     * doesn't drift under load, low enough that it doesn't preempt
     * our voice path. */
    const esp_timer_create_args_t targs = {
        .callback = tick_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lv_tick",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&targs, &s_tick_timer) != ESP_OK ||
        esp_timer_start_periodic(s_tick_timer, TICK_PERIOD_MS * 1000) != ESP_OK) {
        ESP_LOGE(TAG, "tick timer failed");
        return;
    }

    /* Mutex + dispatcher task. Created last so any earlier failure
     * leaves nothing for the task to access. */
    s_lv_mtx = xSemaphoreCreateMutex();
    if (!s_lv_mtx) {
        ESP_LOGE(TAG, "lv mutex alloc failed");
        return;
    }
    BaseType_t ok = xTaskCreatePinnedToCore(lv_task, "lv_task",
        LV_TASK_STACK_BYTES, nullptr, LV_TASK_PRIO, &s_lv_task, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "lv task create failed");
        return;
    }

    s_inited = true;
    ESP_LOGI(TAG, "LVGL %u.%u.%u up — 1bpp %dx%d",
        lv_version_major(), lv_version_minor(), lv_version_patch(),
        PANEL_W, PANEL_H);
}

extern "C" int lvgl_port_tick(void) {
    if (!s_inited) return 1000;
    /* Pump once on demand. Mostly a no-op now that the dedicated
     * lv_task drives lv_timer_handler at ~33 Hz; kept as a hook for
     * dev_menu::dispatch_touch which wants to drain pending click
     * events from the indev queue before reading s_pending_action. */
    if (xSemaphoreTake(s_lv_mtx, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 30;
    }
    uint32_t next_ms = lv_timer_handler();
    xSemaphoreGive(s_lv_mtx);
    if (next_ms > 1000) next_ms = 1000;
    return (int)next_ms;
}

extern "C" void lvgl_port_lock(void) {
    if (s_lv_mtx) xSemaphoreTake(s_lv_mtx, portMAX_DELAY);
}

extern "C" void lvgl_port_unlock(void) {
    if (s_lv_mtx) xSemaphoreGive(s_lv_mtx);
}

static void lv_task(void * /*arg*/) {
    /* High-cadence LVGL dispatcher. Runs lv_timer_handler frequently
     * enough that the indev's stream of (x, y) samples during a
     * touch-drag lets LVGL distinguish scroll from tap. Without this,
     * a finger moving over a button still fired LV_EVENT_CLICKED on
     * release because LVGL never saw movement.
     *
     * The mutex serialises against widget mutations on the main task
     * (build_menu, refresh_info, screen swaps). LVGL itself is not
     * thread-safe; whoever holds the mutex owns the widget tree. */
    while (true) {
        if (xSemaphoreTake(s_lv_mtx, portMAX_DELAY) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(s_lv_mtx);
        }
        vTaskDelay(pdMS_TO_TICKS(LV_TASK_PERIOD_MS));
    }
}

extern "C" void lvgl_port_force_full_refresh(void) {
    s_force_full = true;
}
