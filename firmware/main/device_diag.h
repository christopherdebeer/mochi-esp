/*
 * device_diag — over-the-air diagnostic log (design/18).
 *
 * A small RAM ring buffer of structured records the firmware flushes to
 * mochi.val.run/api/device/diag, so a field device can be root-caused
 * from SQL (the `device_logs` table) without a USB serial cable. Every
 * record is also mirrored to ESP_LOGx, so serial still works when
 * attached.
 *
 * Curated, not chatty: emit errors/warnings + lifecycle outcomes (boot
 * reason, wifi, pack_cache, imagine, voice, ota), not every info log.
 *
 * Complements voice/voice_diag.c (a local LittleFS/serial dump of one
 * voice session); device_diag is the always-on, network-backed sink.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Levels mirror esp_log ordering. */
#define DIAG_ERROR 1
#define DIAG_WARN  2
#define DIAG_INFO  3

/* Bring-up: alloc the ring (PSRAM), mint a per-boot id, and queue the
 * boot record (reset reason, fw version, free heap/PSRAM). Call as early
 * as practical so the reset reason is captured. Idempotent; safe before
 * WiFi (flush is deferred until there's a network + pairing). */
bool device_diag_init(void);

/* Append a record. tag = short subsystem id; msg = short line; ctx =
 * optional JSON string (or NULL). Non-blocking ring append, safe from
 * any task; also mirrored to ESP_LOGx. */
void device_diag_event(int level, const char *tag, const char *msg,
                       const char *ctx);

/* printf-style msg convenience. */
void device_diag_eventf(int level, const char *tag, const char *ctx,
                        const char *fmt, ...) __attribute__((format(printf, 4, 5)));

/* Flush buffered records to /api/device/diag (POST, X-Pet-Id). Requires
 * WiFi + pairing. Best-effort: clears flushed records on success, retains
 * them on failure for the next flush. Returns the count sent. Call
 * periodically from the main loop. */
int device_diag_flush(void);

#ifdef __cplusplus
}
#endif
