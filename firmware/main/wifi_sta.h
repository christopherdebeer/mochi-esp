/*
 * STA-mode connection for the already-provisioned path. Assumes
 * netif/event loop/wifi driver have been initialised by the caller
 * (typically wifi_prov::run() did it, or main does it on the
 * already-provisioned branch via wifi_sta::init_stack()).
 */

#pragma once

#include "nvs_creds.h"

namespace wifi_sta {

/* One-time stack init (netif + event loop + wifi_init). Idempotent
 * via internal guard. Used on the already-provisioned branch where
 * we skip wifi_prov::run() entirely. */
void init_stack(void);

/* Block until STA connects with the given creds. On success writes
 * the dotted-quad IP into ip_str (caller-allocated, ≥16 bytes) and
 * returns true. On failure returns false and ip_str[0]=0. */
bool connect(const struct mochi_wifi_creds *creds,
             char *ip_str, size_t ip_len);

/* Runtime switch to a specific stored network without a reboot — a
 * plain STA→STA reconnect (set_config + connect), NOT the AP↔STA
 * hand-off that hangs on v5.3. Assumes the STA stack is already up
 * (it is, post-boot). Uses a shorter 15 s timeout than connect() so a
 * Settings tap doesn't stall the UI for half a minute. On success
 * writes the IP and returns true; on failure returns false (caller
 * should fall back to connect_any to restore some link). design/22. */
bool switch_to(const struct mochi_wifi_creds *creds,
               char *ip_str, size_t ip_len);

/* Scan for visible APs, intersect with the stored MRU cred list, and
 * try matching credentials in scan-strength order until one connects
 * or all fail. Used on the already-provisioned branch so a device
 * that's been carried between known networks comes up on whichever
 * one is visible.
 *
 * On success writes the IP (≥16 bytes), the SSID joined
 * (≥ MOCHI_WIFI_SSID_MAX + 1 bytes; pass nullptr to skip), and
 * returns true. On failure returns false. */
bool connect_any(char *ip_str, size_t ip_len,
                 char *out_ssid, size_t out_ssid_len);

}  /* namespace wifi_sta */
