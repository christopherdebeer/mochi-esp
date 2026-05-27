#include "lvgl_port.h"

#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

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

/* Refresh policy. e-paper accumulates ghosting after repeated partial
 * updates, so a periodic FULL refresh (EPD_Init + EPD_Display) clears
 * it. The user perceives the full as a brief panel-flash; doing it
 * on every Nth wheel transition keeps it bearable. */
static constexpr int FULL_REFRESH_EVERY = 6;

/* Tick. LVGL needs lv_tick_inc called periodically (default doc says
 * 5 ms); a FreeRTOS gptimer or esp_timer is the canonical source. */
static constexpr int TICK_PERIOD_MS = 5;

static bool                 s_inited = false;
static epaper_driver_display *s_epd = nullptr;
static lv_display_t         *s_disp = nullptr;
static lv_indev_t           *s_indev = nullptr;
static esp_timer_handle_t    s_tick_timer = nullptr;

/* Two LVGL draw buffers in PSRAM. LVGL renders into one while the
 * other is being flushed (double-buffering). For 1bpp this is two
 * 5 KB allocations — trivial against our 7 MB free PSRAM. */
static uint8_t              *s_buf_a = nullptr;
static uint8_t              *s_buf_b = nullptr;

static int                   s_partial_count = 0;
static volatile bool         s_force_full = false;

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
    /* LVGL 1bpp packing: bit n of byte k corresponds to pixel (k*8+n).
     * Documented: in LV_COLOR_FORMAT_I1 the bits are MSB-first by
     * default (matches our e-paper driver). */
    const int x1 = area->x1, x2 = area->x2;
    const int y1 = area->y1, y2 = area->y2;
    const int w  = x2 - x1 + 1;
    const int row_bytes = (w + 7) / 8;

    if (x1 == 0 && x2 == PANEL_W - 1 && y1 == 0 && y2 == PANEL_H - 1) {
        /* Whole-screen flush — common case with full render mode. */
        memcpy(s_composite, px_map, PANEL_BYTES);
    } else {
        /* Partial — copy row-by-row into the composite at the right
         * offset. Rare with LV_DISPLAY_RENDER_MODE_FULL, but keeps
         * us correct if we ever flip to PARTIAL mode for speed. */
        for (int y = 0; y < y2 - y1 + 1; y++) {
            const uint8_t *src = px_map + y * row_bytes;
            uint8_t *dst = s_composite + (y1 + y) * PANEL_STRIDE + (x1 / 8);
            memcpy(dst, src, row_bytes);
        }
    }

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

    /* Tell LVGL we're done; it'll release the draw buffer for the
     * next render. */
    lv_display_flush_ready(disp);
}

/* ─── Touch indev ──────────────────────────────────────────────────
 * LVGL polls this — return the latest finger-down position
 * non-blocking. We translate one touch::Event per call; LVGL itself
 * handles long-press vs tap detection from the (x, y, pressed)
 * stream we feed it.
 *
 * The FT6336 on the Waveshare panel only reports finger-down (no
 * release event from the queue). To bridge: when wait_event(0)
 * returns nothing, report RELEASED with the last position. The
 * (released → pressed) transition kicks LVGL's button-press logic;
 * the next "no event" tick reports release.
 *
 * This is the simplest correct shape — a more sophisticated bridge
 * would also surface the FT6336's "lift" register if available, but
 * touch::current_point already wraps that and we use it on the
 * release-edge below. */
static void indev_read_cb(lv_indev_t * /*indev*/, lv_indev_data_t *data) {
    static int16_t s_last_x = 0;
    static int16_t s_last_y = 0;

    touch::Event ev;
    if (touch::wait_event(&ev, 0)) {
        s_last_x = ev.x;
        s_last_y = ev.y;
        data->point.x = ev.x;
        data->point.y = ev.y;
        data->state = LV_INDEV_STATE_PRESSED;
        return;
    }

    /* No queued tap — check whether the finger's still down so the
     * "press hold" case stays pressed across LVGL polls. */
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

    s_inited = true;
    ESP_LOGI(TAG, "LVGL %u.%u.%u up — 1bpp %dx%d",
        lv_version_major(), lv_version_minor(), lv_version_patch(),
        PANEL_W, PANEL_H);
}

extern "C" int lvgl_port_tick(void) {
    if (!s_inited) return 1000;
    /* Returns ms-until-next-call. dev_menu's main loop already paces
     * itself, so the value is informational; we still honour it as a
     * lower bound for sleeps inside the wheel. */
    uint32_t next_ms = lv_timer_handler();
    if (next_ms > 1000) next_ms = 1000;
    return (int)next_ms;
}

extern "C" void lvgl_port_force_full_refresh(void) {
    s_force_full = true;
}
