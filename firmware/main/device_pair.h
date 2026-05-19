/*
 * Device-side of the device-to-pet pairing protocol.
 *
 * Two HTTPS calls to mochi.val.run; both reuse the same TLS plumbing
 * (esp_http_client + Mozilla cert bundle) that sprite_fetch already
 * brought up. See design/04-pairing.md.
 *
 *   pair_request_code(out) — POST /api/device/pair-init
 *     hits the server with the device's hw_id, fills out->code with
 *     a freshly-allocated MOCHI-XXXX. On retry from the same hw_id
 *     within the 10-minute TTL the server returns the same code, so
 *     the user only ever sees one code per device.
 *
 *   pair_wait_for_user(creds, code, timeout_ms) — GET /api/device/
 *     pair-check loop. Polls every 5 s up to timeout_ms (or forever
 *     if -1) until the server reports paired:true, then fills creds
 *     with the pet_id + pet_name we should persist to NVS.
 *
 * Neither function persists to NVS — caller is responsible for
 * pair_creds_save() once we have a successful result.
 */

#pragma once

#include <stdint.h>
#include "pair_creds.h"

namespace device_pair {

/* MOCHI-XXXX code: 6 chars + dash + 4 chars + NUL = 12. Round up. */
struct InitResult {
    char code[16];
};

bool request_code(InitResult *out);

/* Block, polling pair-check, until the server reports paired or
 * timeout_ms elapses. Returns true on pair, false on timeout or
 * unrecoverable error. timeout_ms < 0 means "no timeout, poll
 * forever." On the way the function may log a couple of "still
 * waiting" lines so the USB monitor isn't completely silent. */
bool wait_for_user(const InitResult *init,
                   struct mochi_pair_creds *out,
                   int timeout_ms);

}  /* namespace device_pair */
