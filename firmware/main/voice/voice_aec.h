/*
 * Software-reference acoustic echo cancellation for the single-mic
 * Waveshare board. Sits between the mic I²S read and the Opus encode
 * in voice_mic.c, with a reference tap in voice_peer.c that clones
 * decoded downlink PCM into a ring buffer before the I²S TX write.
 *
 * Pipeline when enabled:
 *
 *   pc_on_audio_data:
 *     opus_dec_decode → voice_aec_push_ref(pcm, samples) → es_codec_dev_write
 *
 *   mic_task:
 *     codec_dev_read → voice_aec_process_in_place(pcm, samples) → opus_enc
 *
 * Pipeline when disabled (default):
 *
 *   push_ref discards. process_in_place is a no-op. The existing
 *   half-duplex mute in voice_peer_mic_should_mute() remains the
 *   echo defence.
 *
 * Sample-rate posture
 * -------------------
 * The audio stack runs at 24 kHz end-to-end (matches OpenAI Realtime's
 * preferred input format). Espressif's esp-sr AEC requires 16 kHz.
 * We resample 24→16 on entry and 16→24 on exit; the caller never sees
 * the rate change. Quality cost is small; latency cost is ≤ one frame
 * (≤ 20 ms) on top of the AEC's own filter delay.
 *
 * Threading
 * ---------
 * push_ref runs on the WebRTC peer's audio callback thread.
 * process_in_place runs on the voice_mic task (core 1).
 * The ref ring buffer is a lock-free SPSC: single writer (peer),
 * single reader (mic). Underrun = zero-fill (which the AEC interprets
 * as "no echo expected" — correct behaviour when no playback is
 * happening).
 *
 * Lifecycle
 * ---------
 * voice_aec_init  — called from voice_peer's open_audio_playback.
 * voice_aec_deinit — called from voice_peer's close paths.
 * Both are idempotent.
 *
 * Enable/disable is runtime so we can flip it per-session for A/B
 * testing on hardware without a re-flash. Defaults OFF until the
 * software-reference loop is validated under load.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialise. Allocates the reference ring buffer in PSRAM but does
 * NOT yet create the esp-sr AEC handle — that happens lazily on the
 * first voice_aec_set_enabled(true), so the cost (≈ tens of KB +
 * ~5 % CPU) is only paid when we actually engage AEC.
 *
 * external_rate is the sample rate of the PCM that callers pass in
 * (currently 24 kHz). channels must be 1; the single-mic Waveshare
 * board is the only target.
 *
 * Returns false on allocation failure.
 */
bool voice_aec_init(int external_rate, int channels);

/* Tear down. Safe to call when not initialised. */
void voice_aec_deinit(void);

/*
 * Engage / disengage the AEC processing at runtime. On the first
 * enable after init, this creates the esp-sr handle. Subsequent
 * disables hold the handle (cheap re-engage) — full handle teardown
 * only happens at voice_aec_deinit().
 */
void voice_aec_set_enabled(bool enabled);
bool voice_aec_is_enabled(void);

/*
 * Push downlink (speaker-bound) PCM into the reference ring buffer.
 * Called by the audio decode callback in voice_peer just before
 * esp_codec_dev_write. PCM must be at the external_rate set at init.
 *
 * No-op when the module is not initialised. Cheap when AEC is
 * disabled but still initialised — the ring buffer keeps filling so
 * a runtime enable doesn't start from a cold reference history.
 */
void voice_aec_push_ref(const int16_t *pcm, size_t samples);

/*
 * Run AEC over one mic frame in place. Called by the mic task after
 * esp_codec_dev_read, before Opus encode. pcm must be at external_rate.
 *
 * When AEC is disabled or not initialised, this is a no-op — the
 * caller's mic frame is unmodified.
 *
 * On internal failure (filter not yet warmed up, ref ring underrun,
 * resampler glitch) the function falls back to leaving the mic frame
 * untouched. Half-duplex mute in voice_mic is the safety net.
 */
void voice_aec_process_in_place(int16_t *pcm, size_t samples);

#ifdef __cplusplus
}
#endif
