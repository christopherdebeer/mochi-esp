/*
 * `esp_peer` prebuilt blob shim.
 *
 * `libpeer_default.a` (the prebuilt half of the espressif/esp_peer
 * registry component) references two symbols that aren't in the
 * standard ESP-IDF logging API:
 *   - `esp_log` (printf-like; level, tag, fmt, ...)
 *   - `esp_log_timestamp` (already provided by IDF's log component,
 *     but only if `log` is in REQUIRES)
 *
 * Upstream `esp-webrtc-solution` resolves both via
 * `tempotian/media_lib_utils` + `media_lib_sal`, neither of which
 * we pull (see firmware/components/README "Path choice"). Provide
 * the bridge ourselves; one tiny file.
 *
 * The signature was inferred from the relocation pattern in
 * libpeer_default.a (alongside esp_log_timestamp + memcpy in
 * formatted-log call sites). The first int arg matches the
 * Espressif convention of an `esp_log_level_t`-shaped enum.
 */

#include <stdarg.h>
#include <stddef.h>
#include "esp_log.h"

/* Make sure the linker drags in IDF's log component (where
 * esp_log_writev and esp_log_timestamp live). The PRIV_REQUIRES
 * link in main/CMakeLists.txt does the structural work; this just
 * keeps the dependency obvious to a future reader. */

void esp_log(int level, const char *tag, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    /* Map the int level to esp_log_level_t. The values line up
     * (NONE=0, ERROR=1, WARN=2, INFO=3, DEBUG=4, VERBOSE=5) with
     * IDF's enum, so a direct cast is fine for any level the
     * blob actually emits. */
    esp_log_writev((esp_log_level_t)level, tag, fmt, args);
    va_end(args);
}
