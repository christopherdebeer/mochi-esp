/*
 * NVS-backed WiFi credentials storage. Multi-network: an N-deep MRU
 * list, indexed 0..count-1. Index 0 is most-recently-added.
 *
 * No encryption: the threat model is "anyone with physical device
 * access could read flash anyway, so let's not pretend otherwise"
 * (see design/03-provisioning.md). Credentials live in cleartext.
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Length caps from 802.11 + ESP-IDF wifi config struct. SSIDs are
 * 32 bytes, passphrases up to 64 (WPA2 max is 63 + NUL). */
#define MOCHI_WIFI_SSID_MAX  32
#define MOCHI_WIFI_PASS_MAX  64

/* Cap the stored list. 8 is comfortable for "home, work, café,
 * mum's, friend's"-class use; cheap to bump if it ever bites. */
#define MOCHI_WIFI_CREDS_MAX 8

struct mochi_wifi_creds {
    char ssid[MOCHI_WIFI_SSID_MAX + 1];
    char password[MOCHI_WIFI_PASS_MAX + 1];
};

/* One-time NVS init. Idempotent — safe to call multiple times. */
void nvs_creds_init(void);

/* Number of credential slots currently stored (0..MOCHI_WIFI_CREDS_MAX). */
size_t nvs_creds_count(void);

/* Load the cred at MRU index `i`. i=0 is most-recently-added.
 * Returns false if i >= count or if any read fails. */
bool nvs_creds_load_at(size_t i, struct mochi_wifi_creds *out);

/* Insert `creds` at MRU index 0. If the SSID already exists in the
 * list, that older entry is removed first (so the same SSID never
 * appears twice and the list stays MRU-ordered). When the list is
 * already full, the oldest entry (highest index) is dropped to make
 * room. Returns true on success. */
bool nvs_creds_append(const struct mochi_wifi_creds *creds);

/* Wipe all stored creds. Used by factory reset. */
bool nvs_creds_clear_all(void);

/* Remove every stored entry whose SSID matches `ssid`, keeping the
 * rest in MRU order. Returns true if something was removed, false if
 * the SSID wasn't stored. Used by Settings → "Forget WiFi": after a
 * reboot the device reconnects to the next-strongest known network
 * (design/22). */
bool nvs_creds_forget(const char *ssid);

/* --- Legacy single-cred helpers, retained as thin shims so older
 *     call sites and tests keep compiling. nvs_creds_load returns the
 *     MRU entry; nvs_creds_save inserts at MRU; nvs_creds_clear wipes
 *     everything (matching the historical "one slot" semantics). --- */
bool nvs_creds_load(struct mochi_wifi_creds *out);
bool nvs_creds_save(const struct mochi_wifi_creds *creds);
bool nvs_creds_clear(void);

/* "Enter provisioning on next boot" flag. Set by the connect_any
 * failure path so the device can persist + reboot into a clean
 * boot, rather than do an in-process STA→AP swap (which deadlocks
 * intermittently — see project_eink_wifi_handover memory). The boot
 * branch reads + clears the flag and routes directly to wifi_prov
 * without bringing up the STA stack first. */
bool nvs_creds_set_prov_on_boot(bool flag);
bool nvs_creds_get_prov_on_boot(void);

/* User-selected POSIX timezone string (e.g. "GMT0BST,M3.5.0/1,M10.5.0").
 * Set at provisioning time via the captive portal; read at boot by
 * time_sync. Returns true and writes "" into `out` if no value
 * is stored. */
#define MOCHI_TZ_MAX 48
bool nvs_creds_set_tz(const char *tz);
bool nvs_creds_get_tz(char *out, size_t out_len);

#ifdef __cplusplus
}  /* extern "C" */
#endif
