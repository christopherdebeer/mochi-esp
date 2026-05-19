/*
 * Voice session diagnostic log — survives USB disconnect.
 *
 * Voice sessions tend to be short (10-min cap) and the user often
 * tests them while disconnected from the host (so audio doesn't
 * blare into a meeting). The serial console is only useful while
 * USB is plugged in. To debug what happened on a session that
 * ran while disconnected, we keep a per-session log in PSRAM
 * during the session, then flush to LittleFS on stop.
 *
 * On the next boot, the dump_last() entry point reads + logs +
 * deletes the file, so re-attaching USB after a disconnected
 * session shows the full event timeline once on the next boot.
 *
 * Mirrors what would otherwise have been ESP_LOGI'd during the
 * session — same call sites in voice_peer.c log to both sinks.
 *
 * Cheap enough to mirror everything: ring buffer, no per-event
 * filesystem write, single flush on session stop.
 */

#pragma once

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reset the in-memory buffer. Called by voice_peer_start before any
 * other diag entries land. Idempotent. */
void voice_diag_reset(void);

/* Append a timestamped formatted line to the buffer. printf-shape.
 * Lines are prefixed with the elapsed-millis-since-reset, mirroring
 * ESP_LOGI's timestamp posture. Caps at ~4 KB; subsequent calls
 * after the cap are dropped (with a single "[truncated]" marker). */
void voice_diag_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void voice_diag_logv(const char *fmt, va_list args);

/* Flush the in-memory buffer to /lfs/voice_last.log. Called by
 * voice_peer_stop. Idempotent. Returns 0 on success, -1 on error
 * (logged via ESP_LOGE; not fatal — caller continues). */
int  voice_diag_flush(void);

/* If /lfs/voice_last.log exists, dump it line-by-line via ESP_LOGI
 * with a clear "─── last voice session ───" header, then delete it.
 * Called once at boot, after sprite_cache::init() has mounted the
 * LittleFS partition. */
void voice_diag_dump_last(void);

#ifdef __cplusplus
}
#endif
