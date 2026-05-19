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
 *   - Echo defence is layered: voice_aec (when enabled, gated by
 *     VOICE_AEC_USE_ESP_SR — see voice_aec.c) runs first, then the
 *     half-duplex mute in voice_peer_mic_should_mute() drops frames
 *     while the speaker is active and during the I²S drain tail.
 *     Server-side semantic_vad with eagerness="low" is the third
 *     layer. With voice_aec disabled (default) the mute does all
 *     the work; barge-in is sacrificed until M9.f.3 lands enabled.
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

#ifdef __cplusplus
}
#endif
