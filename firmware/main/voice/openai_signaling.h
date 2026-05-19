/*
 * OpenAI Realtime signaling — esp_peer_signaling_impl_t implementation.
 *
 * Ported from `esp-webrtc-solution/solutions/openai_demo/main/
 * openai_signaling.c` + the matching declarations in `common.h`.
 * Definitions live in this header instead of upstream's `common.h`,
 * which we don't pull in (it drags `esp_webrtc.h` / `network.h` /
 * `sys_state.h` we don't have).
 *
 * We use this implementation by passing it (and an
 * `openai_signaling_cfg_t`) into `esp_peer_signaling_start` once the
 * peer-direct glue lands (task #76). The "M9 step today" path
 * (task #71) just calls `start` directly with a no-op cfg to verify
 * that the ephemeral-token mint succeeds.
 */

#pragma once

#include "esp_peer_signaling.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OpenAI signaling configuration. Goes in
 * `esp_peer_signaling_cfg_t::extra_cfg`.
 *
 * `token`: the BYO OpenAI API key (loaded from NVS by voice::init).
 * `voice`: one of alloy, ash, ballad, cedar, coral, echo, marin,
 *          sage, shimmer, verse (GA voice list per
 *          /tmp/mochi-val/shared/realtime-mint.ts). NULL → default,
 *          which is "marin" (matches mochi-val's
 *          REALTIME_DEFAULT_VOICE in shared/realtime-mint.ts).
 * `instructions`: persona / system prompt for the model. NULL or
 *          empty → OpenAI fills in its default ("Your knowledge
 *          cutoff is 2023…"), which we don't want — see the M9.e
 *          finding in design/01-bring-up-plan.md. Production path
 *          fills this from mochi.val.run/voice/realtime-instructions
 *          for the current pet state. Smoke-test path just sends a
 *          one-line greeting prompt.
 *
 * @note  Details:
 *   https://platform.openai.com/docs/api-reference/realtime-sessions/create
 */
typedef struct {
    char *token;
    char *voice;
    char *instructions;
} openai_signaling_cfg_t;

/*
 * Get the singleton implementation. Lifetime of the returned pointer
 * is the program's lifetime; safe to pass into `esp_peer_signaling_start`.
 */
const esp_peer_signaling_impl_t *esp_signaling_get_openai_signaling(void);

#ifdef __cplusplus
}
#endif
