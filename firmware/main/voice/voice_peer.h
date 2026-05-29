/*
 * Voice peer connection — esp_peer-direct WebRTC glue.
 *
 * Replaces the part of upstream `esp_webrtc.c` that sequences
 * signaling ↔ peer for the OpenAI Realtime use case. We don't pull
 * in `esp_webrtc` itself (see firmware/components/README §
 * "Path choice"). What lives here:
 *   - `peer_cfg` with `on_state`, `on_msg`, `on_data` callbacks
 *     wired to forward SDP through the signaling impl
 *   - signaling cfg with `on_ice_info`, `on_connected`,
 *     `on_msg`, `on_close` wired to drive the peer state machine
 *   - worker task that calls `esp_peer_main_loop` in a tight
 *     loop with 10 ms sleeps (matching upstream's pc_task)
 *
 * The smoke-test path (M9.e) starts the peer, lets it run until
 * ESP_PEER_STATE_DATA_CHANNEL_OPENED, logs the first data-channel
 * event from OpenAI, then stops. The actual conversation path
 * (M9.f+) will keep the peer alive and pipe audio through.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Conversation phase the peer is in. Driven by peer state +
 * data-channel events. Touch loop polls voice_peer_phase() and
 * renders different pet expressions per phase so the user can
 * see what mochi is doing without staring at the diag log.
 */
typedef enum {
    VOICE_PHASE_IDLE = 0,        /* no session running */
    VOICE_PHASE_CONNECTING,      /* mint + ICE + DTLS + DCEP in flight */
    VOICE_PHASE_READY,           /* data channel up, waiting for / between speech */
    VOICE_PHASE_SPEAKING,        /* model audio is currently playing */
} voice_phase_t;

/*
 * Start the peer connection.
 *
 * `openai_key` and `instructions` live only inside this call;
 * voice_peer_start strdup's the bits it needs. The caller can wipe
 * the originals after this returns.
 *
 * Returns 0 on success (the peer is now running on a worker task).
 * Calling voice_peer_start while a session is already running
 * returns -1; call voice_peer_stop first.
 */
int  voice_peer_start(const char *openai_key, const char *instructions,
                      const char *tools_json);

/* Current conversation phase. Safe to call from any task. */
voice_phase_t voice_peer_phase(void);

/*
 * True if the mic should drop frames right now. This is a superset of
 * `voice_peer_phase() == VOICE_PHASE_SPEAKING` — it also stays true for
 * a tail window after the last audio frame received from the peer, to
 * cover the codec's DMA drain time. Without that tail, the speaker
 * continues playing mochi's last few hundred ms of audio after our
 * phase has dropped to READY, the mic picks it up, and the server's
 * VAD self-interrupts on the bleed.
 *
 * Safe to call from any task; reads atomic state only.
 */
bool voice_peer_mic_should_mute(void);

/*
 * True when the session should be torn down by the next polling
 * caller (typically main.cpp's touch loop). Set by:
 *   - the worker task hitting the idle / hard caps
 *   - the peer state machine reaching DISCONNECTED / CONNECT_FAILED
 *
 * Cleared by voice_peer_stop. Letting the touch loop drive teardown
 * keeps voice_peer_stop's worker-join logic on a non-worker task,
 * which avoids deadlock.
 */
bool voice_peer_stop_requested(void);

/*
 * Send a text message + response.create over the data channel.
 * Equivalent to:
 *   { type: "conversation.item.create",
 *     item: { type: "message", role: "user",
 *             content: [{ type: "input_text", text: "<text>" }] } }
 *   { type: "response.create" }
 *
 * Used for the M9.f.1.5 text-talk-back debug path so we can validate
 * multi-turn lifecycle without committing to mic capture. Returns 0
 * on success; -1 if the data channel isn't open or a send fails. */
int voice_peer_send_text(const char *text);

/*
 * Inject a system-role note into the live session (design/27).
 *
 * Like voice_peer_send_text but with role "system" — a substrate→model
 * context push (care taps "[from your body] …", environment changes
 * "[notice] …") the model folds into its next reply. If the model is
 * mid-utterance it issues a response.cancel first so the note lands
 * promptly. Mirrors the legacy web client's notifyCare/notifyEnvironment.
 * Returns 0 on success; -1 if the data channel isn't open or a send fails.
 */
int voice_peer_inject_note(const char *text);

/*
 * Send a raw JSON string as one event over the data channel.
 *
 * Lower-level than voice_peer_send_text — caller owns the full
 * event shape. Used by voice_tools.c to send
 * `conversation.item.create` with `function_call_output` items
 * after dispatching a tool through val.run. Returns
 * ESP_PEER_ERR_NONE (0) on success.
 */
int voice_peer_send_dc_json(const char *json);

/*
 * Send an Opus-encoded audio frame upstream to the peer.
 *
 * Wraps esp_peer_send_audio so voice_mic doesn't have to touch the
 * peer handle directly. The buffer is owned by the caller and must
 * remain valid until this returns. Returns 0 on success,
 * ESP_PEER_ERR_WOULD_BLOCK if the send queue is full (voice_mic
 * should drop the frame and continue), or other negative values
 * on failure.
 */
int voice_peer_send_audio_frame(const uint8_t *buf, int size);

/*
 * True between successful voice_peer_start and voice_peer_stop.
 * Used by voice_mic's task loop to know when to stop pumping
 * samples (so we don't push frames into a torn-down peer). */
bool voice_peer_is_running(void);

/*
 * True once the peer reached ESP_PEER_STATE_DATA_CHANNEL_OPENED.
 * Cleared by voice_peer_stop.
 */
bool voice_peer_data_channel_opened(void);

/*
 * True once we received our first data-channel event from OpenAI
 * (typically `session.created` shortly after channel-open). Used by
 * the smoke test to confirm the channel is bidirectional and the
 * session config we minted with is being honoured server-side.
 * Cleared by voice_peer_stop.
 */
bool voice_peer_first_event_seen(void);

/*
 * Stop the peer connection. Tears down signaling first (sends BYE),
 * then closes the peer, joins the worker task, frees buffers.
 * Idempotent — calling on a stopped peer is a no-op.
 */
void voice_peer_stop(void);

/* Per-session token + turn totals accumulated from response.done usage
 * (design/18 ph3b). Reset at voice_peer_start; read at session end for
 * the realtime_sessions row. Any out-pointer may be NULL. */
void voice_peer_get_session_stats(int *turns, int *in_tok,
                                  int *out_tok, int *total_tok);

#ifdef __cplusplus
}
#endif
