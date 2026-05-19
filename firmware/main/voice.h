/*
 * Voice — M9 voice session lifecycle.
 *
 * voice::init() runs once at boot, after wifi + RTC + SHTC3 are up.
 * It brings up the I²S codec (ES8311 via codec_board), loads BYO key
 * + pet_id from NVS, and logs whether voice is reachable. It does NOT
 * open a peer connection at boot — voice sessions cost real OpenAI
 * tokens and connect to the user's account, so the path is opt-in.
 *
 * voice::start_session() / voice::stop_session() are the user-tap
 * triggers. main.cpp's touch loop calls them when the user
 * long-presses the centre-attention zone (and again to stop). Both
 * are idempotent — calling start while a session is running, or
 * stop on no-session, is a no-op.
 *
 * Failure of any inner step logs-and-continues: a missing NVS key
 * disables voice but doesn't break boot. The status of the voice
 * subsystem is queryable via voice::is_ready() (key + pet_id loaded)
 * and voice::is_active() (a session is currently running).
 */

#pragma once

namespace voice {

/* One-time bring-up at boot. Brings up the codec, loads NVS creds.
 * Returns true on success; non-fatal failures still return true so
 * boot continues. */
bool init(void);

/* True if NVS has a BYO key + pet_id and we can reasonably attempt a
 * session. Caller should still treat start_session() failure as
 * recoverable. */
bool is_ready(void);

/* True between a successful start_session() and the corresponding
 * stop (either user-initiated or remote-side close). */
bool is_active(void);

/* Start a voice session. Mints an ephemeral token, opens a peer
 * connection, requests OpenAI to greet the user. Asynchronous — the
 * peer worker task keeps running after this returns. Returns 0 on
 * success, -1 on failure to even kick off the mint. */
int start_session(void);

/* Stop the currently-active voice session. Tears down the peer,
 * stops the worker task, clears in-memory state. Idempotent. */
void stop_session(void);

/* Coarse session phase, mirroring voice_peer_t. Used by the touch
 * loop to render a different pet expression per phase. */
enum class Phase {
    Idle = 0,         /* no session */
    Connecting,       /* mint + ICE + DTLS + DCEP in flight */
    Ready,            /* data channel up, between speech */
    Speaking,         /* model audio currently playing */
};
Phase phase(void);

/* Send a text message + response.create over the data channel.
 * Used for the M9.f.1.5 text-talk-back path. Caller is responsible
 * for ensuring is_active() is true; returns false otherwise. */
bool send_text(const char *text);

/* True when the worker requested an auto-stop (idle cap, hard cap,
 * or remote-side disconnect). The touch loop polls this and calls
 * stop_session() from its own task context; calling stop from the
 * worker task would deadlock on the join-self. */
bool stop_requested(void);

}  /* namespace voice */
