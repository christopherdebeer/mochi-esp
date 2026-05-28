/*
 * power — idle tiers, optional light sleep, and power telemetry
 * (design/26).
 *
 * The device has two hard states today: Live (SoC fully awake) and
 * deep sleep (full reboot on wake). This module adds a resting tier
 * between them — "Doze" — entered after a configurable idle timeout,
 * plus the telemetry needed to analyse the power profile from SQL
 * (the device_logs table, via device_diag).
 *
 * What Doze does (all reversible, all gated):
 *   - WiFi → WIFI_PS_MAX_MODEM (CONFIG_MOCHI_DOZE_WIFI_POWERSAVE)
 *   - substrate refresh interval stretched (power_substrate_refresh_us)
 *   - SoC enters automatic light sleep between events
 *     (CONFIG_MOCHI_LIGHT_SLEEP — default OFF pending hardware
 *     validation; when off the tier machine still runs so the Live
 *     power baseline and doze-time accounting are still recorded)
 *
 * Telemetry (always on, regardless of the light-sleep flag — this is
 * how we "keep insight for existing power draw"):
 *   - cumulative ms in each tier + doze entry count
 *   - a "power"/"doze" and "power"/"wake" diag event per transition,
 *     carrying time-in-previous-tier + battery mv
 *   - power_telemetry_ctx(): a JSON snapshot (tier times, battery mv,
 *     uptime, config) the main loop emits alongside the health
 *     heartbeat, so discharge-per-tier is recoverable via SQL.
 *
 * Tier transitions are inhibited while voice / imagine / consolidate /
 * dev_menu / key_portal own the device — the caller passes that in.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POWER_TIER_LIVE = 0,
    POWER_TIER_DOZE = 1,
} power_tier_t;

/* One-shot bring-up. Reads the Kconfig policy, seeds the activity
 * clock to now (so the device doesn't doze immediately at boot),
 * configures esp_pm + registers GPIO wake sources when
 * CONFIG_MOCHI_LIGHT_SLEEP is set, and emits a "power"/"init" diag
 * event describing the active config. Idempotent. */
void power_init(void);

/* Mark user activity (a dispatched touch, a button). Resets the idle
 * clock; if currently dozing, requests a transition back to Live on
 * the next power_update(). `now_us` is esp_timer_get_time(). */
void power_note_activity(int64_t now_us);

/* Drive the tier state machine. Call once per main-loop iteration.
 *   inhibited  — true while a subsystem owns the device (voice, etc.);
 *                forces/holds Live and is itself treated as activity.
 *   net_online — true when WiFi is associated, so the doze WiFi
 *                power-save switch is only attempted when it can work.
 * Accumulates time into the current tier and fires transition events
 * on the edges. */
void power_update(int64_t now_us, bool inhibited, bool net_online);

/* Current tier (for chrome / gating decisions). */
power_tier_t power_tier(void);

/* Substrate re-projection interval for the current tier, in
 * microseconds. Live uses the normal cadence; Doze stretches it so
 * the SoC stays asleep longer. The main loop's idle-tick gate uses
 * this in place of a fixed constant. */
int64_t power_substrate_refresh_us(void);

/* True once the device has dozed continuously past
 * CONFIG_MOCHI_DEEP_SLEEP_TIMEOUT_S (0 = never). The main loop polls
 * this and, when true, renders the asleep frame and commits deep
 * sleep through the existing sleep_gesture path. */
bool power_should_deep_sleep(void);

/* Fill `buf` with a JSON object snapshotting the power state for a
 * telemetry record (no trailing newline). Safe if buf is small —
 * always NUL-terminated. */
void power_telemetry_ctx(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
