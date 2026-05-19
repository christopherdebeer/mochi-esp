/*
 * SoftAP + captive-portal provisioning.
 *
 * Boots a SoftAP, runs a tiny DNS hijacker (every query resolves to
 * 192.168.4.1), serves a portal page that lists nearby SSIDs and
 * accepts a password, validates the submitted creds by attempting a
 * brief STA join, and on success persists them to NVS and returns.
 *
 * Hand-rolled (not using ESP-IDF's wifi_provisioning) because the
 * UX integration with the e-paper renderer is the main thing we
 * care about, and wifi_provisioning's lifecycle hooks are awkward
 * to bend around our screen transitions. See design/03-provisioning.md.
 */

#pragma once

#include "epaper_driver_bsp.h"
#include "nvs_creds.h"
#include "openai_key.h"

namespace wifi_prov {

/* Block until the user has supplied WiFi credentials and an OpenAI
 * API key (M3.1). The function spins up SoftAP + portal, takes the
 * user through the flow, captures both fields, and returns once the
 * form has been submitted. Persistence is the caller's job — see
 * main.cpp.
 *
 * On every screen change (idle → trying → failed → idle), the
 * passed-in `epd` driver gets a partial refresh with the relevant
 * status. Heavy state lives in module-internal globals because
 * esp_event_handler signatures don't pass user context cleanly.
 *
 * `out_key_buf` is a caller-provided buffer of at least
 * MOCHI_OPENAI_KEY_MAX + 1 bytes. On return, it holds the entered
 * key (NUL-terminated). Empty if the user left the field blank.
 *
 * Returns true once the user has submitted the form. Never returns
 * false in current implementation — the function loops until
 * submission. */
bool run(epaper_driver_display *epd,
         struct mochi_wifi_creds *out_creds,
         char *out_key_buf,
         size_t out_key_buf_len);

/* Construct a "Mochi-XXXX" SSID from the device's base MAC, where
 * XXXX is the last 4 hex digits, uppercase. Output buffer must be
 * at least MOCHI_WIFI_SSID_MAX + 1 bytes. */
void make_softap_ssid(char *out_buf, size_t buf_len);

}  /* namespace wifi_prov */
