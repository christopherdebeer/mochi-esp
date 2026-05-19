#pragma once

/*
 * ota_update — over-the-air firmware updates from GitHub Releases.
 *
 * The flow:
 *   1. After WiFi connects, main calls mark_valid_if_pending() once.
 *      If we just booted into a freshly-OTA-installed slot, this
 *      cancels the rollback timer; otherwise it's a no-op.
 *   2. main calls start_background_task() with the manifest URL.
 *      A low-priority task wakes on boot (after a short settle
 *      delay), every ~24 hours, and checks the manifest for a
 *      version != esp_app_get_description()->version. On a mismatch
 *      it streams the .bin into the inactive slot via
 *      esp_https_ota, flips otadata, and asks main to reboot.
 *   3. Reboot is gated by reboot_ready() — main polls this from its
 *      idle loop and calls esp_restart() when no voice session is
 *      active and no recent touch.
 *
 * If a freshly-flashed image crashes during boot before
 * mark_valid_if_pending() runs, the bootloader auto-rolls back to
 * the previous slot on the next reset. This requires
 * CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y in sdkconfig.
 */

#include <stdbool.h>

namespace ota_update {

/* Call once after WiFi STA connect succeeds. If the running image is
 * marked ESP_OTA_IMG_PENDING_VERIFY this commits it; otherwise no-op.
 *
 * Returns true if a pending image was promoted to valid, false in
 * the no-op case or on error. The return value is informational
 * (mostly for logging); callers shouldn't gate behaviour on it. */
bool mark_valid_if_pending();

/* Spawn the OTA background task. Safe to call once after WiFi + NVS
 * are up. The task lifetime is the process lifetime; we never join.
 *
 * manifest_url is borrowed (must outlive the process). Use a string
 * literal in main.cpp. The task fetches this JSON document, expects
 *   { "version": "<string>", "url": "<https url to .bin>" }
 * and updates iff version != running version. */
void start_background_task(const char *manifest_url);

/* True after the background task has finished writing a new image
 * into the inactive slot and called esp_ota_set_boot_partition. The
 * device should reboot at the next idle moment to apply.
 *
 * Idempotent — main can poll this every loop tick. Stays true until
 * the device reboots. */
bool reboot_ready();

/* Returns the currently running app version string from
 * esp_app_get_description(). Stable pointer into rodata; do not
 * free. Useful for status UI / logs. */
const char *current_version();

}  // namespace ota_update
