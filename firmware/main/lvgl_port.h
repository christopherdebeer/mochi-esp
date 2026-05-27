/*
 * lvgl_port — bridge LVGL ↔ Waveshare 1.54" e-ink + FT6336 touch.
 *
 * v0.1.6 introduced LVGL for the dev_menu wheel screens (Info /
 * Actions / WifiModal). Only those screens use LVGL; the live pet
 * render path, boot splash, and provisioning screens stay on the
 * bare epd_ui draw helpers because they're infrequent and the
 * existing code works.
 *
 * Bridge shape:
 *   - 1bpp draw buffer in PSRAM (200×200/8 = 5 000 B + LVGL alignment)
 *   - flush_cb does an MSB-first bit-pack into the e-paper driver's
 *     5 000 B internal buffer, then calls EPD_DisplayPart (or
 *     periodically EPD_Display for a full-clear ghost-bust).
 *   - indev read_cb pulls non-blocking from touch::wait_event with a
 *     0 ms timeout; LVGL polls this on its own cadence.
 *   - tick is driven by a FreeRTOS timer at 5 ms (lv_tick_inc(5)).
 *
 * Threading:
 *   - lv_init() and lv_display_create() run on whatever core the
 *     caller is on (we call from app_main on CPU 0).
 *   - lv_timer_handler() must NOT be called concurrently with widget
 *     creation; the dev_menu API serialises both into the main loop
 *     (request_advance latches → tick consumes → render mutates the
 *     widget tree → next lv_timer_handler() picks up the dirty
 *     regions).
 *   - The flush_cb runs from inside lv_timer_handler(), so the
 *     e-paper driver is touched only from the main loop's thread.
 */

#pragma once

#include <stdbool.h>
#include "epaper_driver_bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One-time bring-up. Wraps lv_init(), allocates the draw buffer,
 * registers display + indev, starts the tick timer. Idempotent.
 * Pass the e-paper driver instance the rest of the firmware uses;
 * the port will call EPD_LoadBuffer / EPD_DisplayPart on it from
 * inside the flush_cb. */
void lvgl_port_init(epaper_driver_display *epd);

/* Drain the LVGL scheduler synchronously on the calling task. Mostly
 * vestigial — a dedicated lv_task pumps lv_timer_handler at ~33 Hz,
 * which is what makes drag-vs-tap discrimination work. Kept as a
 * hook for dispatch_touch which wants click events drained
 * promptly before reading the latched action. Returns the ms until
 * the next call (informational; the high-cadence task ignores it). */
int lvgl_port_tick(void);

/* Mutex around the widget tree. Required around any lv_obj_create /
 * lv_label_set_text / lv_screen_load — LVGL itself is not thread-
 * safe and the dispatcher task holds this lock while running
 * lv_timer_handler. The lock is recursive-friendly only inasmuch as
 * the caller takes care to balance lock/unlock pairs; nested locks
 * from one task block. */
void lvgl_port_lock(void);
void lvgl_port_unlock(void);

/* Force a full-refresh on the next flush. Called when transitioning
 * between wheel screens to bust e-paper ghosting; ordinary partial
 * refreshes are auto-counted internally and a full fires every Nth.
 * Cleared once consumed. */
void lvgl_port_force_full_refresh(void);

#ifdef __cplusplus
}
#endif
