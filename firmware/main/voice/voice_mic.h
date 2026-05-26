/*
 * Voice mic capture — read I²S mic, Opus-encode, send upstream.
 *
 * Pipeline:
 *   esp_codec_dev_read (24 kHz mono PCM16, 20 ms = 480 samples)
 *     → esp_opus_enc_process (VOIP, 20 ms, complexity 5, FEC on)
 *     → voice_peer_send_audio_frame
 *
 * Threading: a single dedicated FreeRTOS task pumps the loop at ~50 Hz.
 * `esp_codec_dev_read` blocks until the requested bytes are available,
 * so the task naturally paces with the I²S sample clock — no manual
 * vTaskDelay needed in steady state.
 *
 * Lifecycle:
 *   voice_mic_start() — open record handle, open Opus encoder, spawn
 *     mic_task. Idempotent.
 *   voice_mic_stop()  — set stop flag, wait for task to exit, close
 *     handles. Idempotent.
 *
 * voice_peer_start/stop call these directly so the mic comes up + down
 * with the session.
 *
 * Caveats:
 *   - Echo defence: voice_aec (software-reference AEC against
 *     decoded speaker PCM) is the live path. The half-duplex mute
 *     in voice_peer_mic_should_mute() runs only when AEC failed
 *     to init — voice_peer_mic_should_mute() short-circuits to
 *     false whenever voice_aec_is_enabled() is true. Server-side
 *     semantic_vad with eagerness="low" remains the outer guard.
 *   - Mic gain comes from codec_board's default (30 dB ES8311 PGA).
 *     If voice is too quiet or clipping, tune via
 *     esp_codec_dev_set_in_gain.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool voice_mic_start(void);
void voice_mic_stop(void);

/* Latest mic-frame peak amplitude in dBFS (negative; -inf encoded as
 * -120). Updated each 20 ms PCM read. Used by voice_peer to attach
 * a "what was the mic doing?" snapshot to `input_audio_buffer.
 * speech_started` events — lets us tell self-interrupt (model leak
 * past AEC, near-silent mic) apart from real barge-in (loud mic).
 * Reads/writes are atomic-ish (single int store/load); no lock. */
int voice_mic_last_peak_dbfs(void);

#ifdef __cplusplus
}
#endif
