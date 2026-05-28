/*
 * ota_channel — NVS-backed selection of the OTA update channel.
 *
 * Two channels, mirroring the GitHub Releases layout the CI publishes:
 *   stable — newest published, non-prerelease release. The device
 *            fetches it via /releases/latest/download/latest.json
 *            (GitHub's redirector skips prereleases).
 *   beta   — the rolling "beta" prerelease that PR builds refresh,
 *            fetched via /releases/download/beta/latest.json.
 *
 * Default is stable, so an unprovisioned device behaves exactly as it
 * did before channels existed. The dev_menu RISK page exposes a toggle;
 * the choice is persisted here and read by ota_update's poll loop on
 * every cycle, so switching takes effect at the next check (the toggle
 * also nudges an immediate check via ota_update::check_now()).
 *
 * Beta vs stable version precedence is resolved by ota_update's
 * semver-aware comparator: beta builds carry a "-beta.<n>" pre-release
 * suffix, which sorts below the matching release but above the previous
 * release — so a beta device updates forward through betas and lands on
 * the final stable when it ships.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_CHANNEL_STABLE = 0,  /* default */
    OTA_CHANNEL_BETA   = 1,
} ota_channel_t;

/* Current channel (NVS value if set, else stable). */
ota_channel_t ota_channel_get(void);

/* Persist `ch`. No-op-safe if NVS is unavailable. */
void ota_channel_set(ota_channel_t ch);

/* Flip stable<->beta, persist, and return the new value. */
ota_channel_t ota_channel_toggle(void);

/* Human label ("stable" / "beta"). Stable pointer into rodata. */
const char *ota_channel_name(ota_channel_t ch);

#ifdef __cplusplus
}
#endif
