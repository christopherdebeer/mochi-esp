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
 *   - No AEC. Mochi's own audio leaks back through the speaker into
 *     the mic. Server-side semantic_vad with eagerness="low" + the
 *     mint's transcription config provides defence-in-depth, but
 *     ping-pong loops are still possible in noisy environments.
 *     Software-reference AEC is M9.h+ work.
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
