/*
 * Voice tool-call routing.
 *
 * When OpenAI Realtime emits `response.function_call_arguments.done`,
 * the model is asking the device to invoke a substrate tool — settle
 * the pet to sleep, request care, note something about the human, etc.
 * The device routes the call to mochi.val.run/api/voice/tool, gets a
 * structured `{ok, message?, reason?}` result back, and feeds that
 * to OpenAI as `function_call_output` so the model can fold the
 * outcome into its next breath.
 *
 * This is a thin async dispatch layer. The HTTPS POST to val.run
 * blocks for ~150-300 ms (TLS handshake + request); we keep that
 * off the worker task that runs `esp_peer_main_loop` so DTLS
 * keepalives don't stall. A dedicated worker task pulls calls off
 * a queue, makes the request, sends the result back.
 *
 * Lifecycle:
 *   voice_tools_init()   — start the worker + queue. Idempotent.
 *   voice_tools_set_pet_id(id) — call once when the session starts.
 *                                (val.run /api/voice/tool needs
 *                                 X-Pet-Id, and the pet_id is the
 *                                 same one set as voice's NVS pair.)
 *   voice_tools_dispatch(call_id, name, args_json)
 *     — push a request onto the queue. Returns immediately.
 *       The worker task does the round-trip + sends the result
 *       back over the data channel (via voice_peer_send_*).
 *   voice_tools_shutdown() — stop the worker, drain the queue.
 *
 * The dispatch worker calls back into voice_peer to send the
 * function_call_output + response.create via esp_peer_send_data.
 * Threading: the worker is a single dedicated task, so no
 * lock contention with the peer worker.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time bring-up. Spawns the dispatch task + creates the queue.
 * Idempotent. */
bool voice_tools_init(void);

/* Set the pet_id used as X-Pet-Id when POSTing to val.run. The
 * pointer is copied into a static buffer; safe to wipe after. */
void voice_tools_set_pet_id(const char *pet_id);

/* Push a tool-call onto the queue. Returns true if queued.
 *
 * `call_id` and `name` are short strings (≤64 each); `args_json` is
 * a (possibly empty) JSON-encoded string. All three are strdup'd
 * inside the queue entry; caller can free / wipe after this returns.
 *
 * The worker calls voice_peer_send_data internally with the
 * function_call_output + response.create after the val.run round-
 * trip. There's no return path — failures are logged and an error-
 * shaped function_call_output is sent so the model can react. */
bool voice_tools_dispatch(const char *call_id, const char *name, const char *args_json);

/* Stop the worker, drain the queue. Called on voice_peer_stop. */
void voice_tools_shutdown(void);

#ifdef __cplusplus
}
#endif
