/*
 * time_sync — SNTP + timezone for wall-clock substrate timestamps.
 *
 * Why: pet decay/engagement projection uses ms-since-epoch
 * timestamps that must agree with the server. Without NTP the
 * device starts at year 2000 (or whatever the RTC last held); with
 * NTP it picks up real time and projects against the same now()
 * the server uses.
 *
 * Lifecycle: call init() once after WiFi STA is up. It kicks the
 * SNTP client (pool.ntp.org default) and waits up to ~5 s for a
 * sync. If the sync fails the device falls back to whatever the
 * RTC has — substrate projection is approximate but doesn't crash.
 *
 * On success the device's <time.h> functions return real wall time.
 * The PCF85063 RTC is not touched here — set_clock from sntp goes
 * straight to esp_timer's offset state.
 *
 * Timezone: defaults to GMT/BST (UK). Override via menuconfig
 * (`Mochi pet → Timezone string`) or call set_tz() at runtime once
 * we have a server-supplied TZ to feed it.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up SNTP and apply the default TZ. Returns true if a sync
 * was achieved within the timeout, false otherwise. Idempotent. */
bool time_sync_init(void);

/* Replace the active TZ string. Format is the standard POSIX TZ
 * (e.g. "GMT0BST,M3.5.0/1,M10.5.0" for UK). Caller owns the
 * pointer; we strdup. M13d will use this with the server-supplied
 * TZ once available. */
void time_sync_set_tz(const char *tz);

/* Wall-clock ms since epoch. Equivalent to JS Date.now(). Used by
 * substrate projection so device + server agree on `now`. */
int64_t time_sync_now_ms(void);

/* True once a successful SNTP sync has happened this boot. */
bool time_sync_synced(void);

#ifdef __cplusplus
}
#endif
