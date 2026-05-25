/*
 * Voice mic capture loop — see voice_mic.h for high-level shape.
 *
 * Sample/frame math at 24 kHz mono PCM16:
 *   20 ms frame = 480 samples = 960 bytes PCM in
 *   Opus encoded out: ~80–160 bytes (VBR off, fixed bitrate)
 *
 * The encoder's actual in_size/out_size come from
 * `esp_opus_enc_get_frame_size()` — we trust those at runtime rather
 * than hardcoding, in case the codec component bumps internal frame
 * sizing.
 */

#include "voice_mic.h"
#include "voice_peer.h"
#include "voice_diag.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreatePinnedToCoreWithCaps */

#include "codec_init.h"
#include "esp_codec_dev.h"
#include "esp_opus_enc.h"
#include "esp_peer_types.h"   /* ESP_PEER_ERR_WOULD_BLOCK */
#include "voice_peer.h"       /* voice_peer_phase() — for half-duplex gate */
#include "voice_aec.h"

#define TAG "voice_mic"

#define LOGI_DIAG(fmt, ...) do {              \
    ESP_LOGI(TAG, fmt, ##__VA_ARGS__);        \
    voice_diag_log(fmt, ##__VA_ARGS__);       \
} while (0)
#define LOGW_DIAG(fmt, ...) do {              \
    ESP_LOGW(TAG, fmt, ##__VA_ARGS__);        \
    voice_diag_log("WARN: " fmt, ##__VA_ARGS__); \
} while (0)
#define LOGE_DIAG(fmt, ...) do {              \
    ESP_LOGE(TAG, fmt, ##__VA_ARGS__);        \
    voice_diag_log("ERR: " fmt, ##__VA_ARGS__); \
} while (0)

/* Stack budget — 32 KB in PSRAM.
 *
 * Boundaries we've measured on hardware (internal RAM):
 *   8 KB  → overflowed on first encode (real cause turned out to
 *           be the codec-open bug; with that fixed 8 KB might
 *           have worked, but we never re-tested).
 *   16 KB → overflowed on the FIRST diag-log line. Backtrace shows
 *           voice_diag_logv → vsnprintf at the bottom.
 *   20 KB → still overflowed on entry, _frxt_dispatch caught it on
 *           the first context switch after task start.
 *   32 KB → xTaskCreatePinnedToCore fails: not enough contiguous
 *           internal RAM after wifi + peer + tools take their slices.
 *
 * Way out: allocate the stack from PSRAM via
 * xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM). PSRAM has 7+ MB
 * free at this point so we can afford a generous 32 KB. ISRs still
 * use internal RAM, so the only cost is ~3× slower stack access on
 * function entry/exit — negligible at our 20 ms cadence.
 *
 * sdkconfig requirement: CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
 * (already set). */
#define MIC_TASK_STACK_BYTES   (32 * 1024)
#define MIC_TASK_PRIO          5
/* Pin to APP core (1) so wifi (which runs on core 0) doesn't pile its
 * ISR frames onto our blocked-on-I²S stack. The peer's worker is
 * unpinned so this is the only voice task with a hard core affinity. */
#define MIC_TASK_CORE          1

/* Match the mint's audio.input.format. gpt-realtime wants 24 kHz. */
#define MIC_SAMPLE_RATE        24000
#define MIC_CHANNEL            1
#define MIC_BITS_PER_SAMPLE    16

/* Opus VOIP @ 24 kHz / 20 ms — 24000 Hz × 0.020 s × 1 ch × 2 B = 960 B
 * input per frame. Output bitrate 24 kbps gives ~60 B/frame on the
 * wire — very voice-friendly. Higher complexity = more CPU; 5 is the
 * mochi-val sweet spot. FEC on so packet loss doesn't tear up
 * utterances. */
#define MIC_OPUS_BITRATE       24000
#define MIC_OPUS_COMPLEXITY    5
#define MIC_OPUS_FEC           true
#define MIC_OPUS_DTX           false  /* DTX needs sample_rate ≤ 16 kHz */

static TaskHandle_t           s_task;
static atomic_bool            s_running;
static esp_codec_dev_handle_t s_record_dev;
static bool                   s_record_open;
static void                  *s_opus_enc;       /* esp_opus_enc handle */
static int                    s_in_bytes;       /* PCM bytes per frame */
static int                    s_out_cap;        /* recommended out buffer */
static uint8_t               *s_pcm_buf;        /* in_bytes */
static uint8_t               *s_enc_buf;        /* out_cap */

/* Counters logged periodically into the diag buffer. */
static uint32_t s_pcm_frames;
static uint32_t s_enc_ok;
static uint32_t s_enc_err;
static uint32_t s_send_ok;
static uint32_t s_send_err;
static uint32_t s_send_block;
static uint32_t s_read_err;
static uint32_t s_muted_frames;   /* frames dropped because mochi was speaking */

/* Latest per-frame peak amplitude, in dBFS (negative; -120 ≈ silence).
 * Single int — torn read is fine, the only consumer is a one-shot
 * snapshot on speech_started. */
static atomic_int s_last_peak_dbfs = -120;

/* Cheap log10 via integer math: peak/32768 → dBFS = 20·log10(peak/32768).
 * Implemented as a 16-step lookup over peak ranges so we avoid pulling
 * the float libm. Rounds to nearest int dBFS, clamped to [-120, 0]. */
static int peak_to_dbfs(int16_t *pcm, size_t samples) {
    int peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int v = pcm[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    if (peak <= 0) return -120;
    /* dBFS table for peak ranges. Each step ≈ 6 dB (a halving of
     * amplitude). Tight enough for self-interrupt vs real-speech
     * discrimination without the float math. */
    static const int thresholds[] = {
        32768, 23170, 16384, 11585, 8192, 5793, 4096, 2896,
        2048,  1448,  1024,  724,   512,  362,  256,  181,
        128,   90,    64,    45,    32,   23,   16,   11,
        8,     6,     4,     3,     2,    1
    };
    static const int dbs[] = {
        0,  -3,  -6,  -9,  -12, -15, -18, -21,
        -24, -27, -30, -33, -36, -39, -42, -45,
        -48, -51, -54, -57, -60, -63, -66, -69,
        -72, -75, -78, -81, -84, -90
    };
    for (size_t i = 0; i < sizeof(thresholds)/sizeof(thresholds[0]); i++) {
        if (peak >= thresholds[i]) return dbs[i];
    }
    return -120;
}

int voice_mic_last_peak_dbfs(void) {
    return atomic_load(&s_last_peak_dbfs);
}

static bool open_record(void) {
    s_record_dev = get_record_handle();
    if (!s_record_dev) {
        LOGE_DIAG("no record handle from codec_board");
        return false;
    }
    /* On the Waveshare V2, codec_board configures ES8311 with
     * `reuse_dev=true` — record and playback share the SAME
     * esp_codec_dev_handle_t (IN_OUT type). Calling
     * esp_codec_dev_open on the record handle is calling it on the
     * SAME handle voice_peer already opened for playback, which
     * (a) earns "Adev_Codec: Input already open" log noise and
     * (b) BREAKS playback — every subsequent esp_codec_dev_write
     * returns ESP_ERR_CODEC_DEV_NOT_OPEN (259). Caught on hardware
     * 2026-05-19; took us a full debug cycle to find.
     *
     * Right shape: don't open again. Playback already opened the
     * codec at 24 kHz mono PCM16 — same format we want for read.
     * Just bump the input gain (idempotent) and start reading. */
    esp_codec_dev_set_in_gain(s_record_dev, 30.0f);
    /* s_record_open stays false — close_all() will skip the close
     * that would also break playback. */
    return true;
}

static bool open_encoder(void) {
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate     = MIC_SAMPLE_RATE;
    cfg.channel         = MIC_CHANNEL;
    cfg.bits_per_sample = MIC_BITS_PER_SAMPLE;
    cfg.bitrate         = MIC_OPUS_BITRATE;
    cfg.frame_duration  = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    cfg.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    cfg.complexity      = MIC_OPUS_COMPLEXITY;
    cfg.enable_fec      = MIC_OPUS_FEC;
    cfg.enable_dtx      = MIC_OPUS_DTX;
    cfg.enable_vbr      = false;

    if (esp_opus_enc_open(&cfg, sizeof(cfg), &s_opus_enc) != ESP_AUDIO_ERR_OK) {
        LOGE_DIAG("esp_opus_enc_open failed");
        return false;
    }
    if (esp_opus_enc_get_frame_size(s_opus_enc, &s_in_bytes, &s_out_cap)
            != ESP_AUDIO_ERR_OK) {
        LOGE_DIAG("esp_opus_enc_get_frame_size failed");
        return false;
    }
    /* PCM buffer in internal RAM (small, ~960 B, hot read path).
     * Encoder output also small; internal RAM is fine. */
    s_pcm_buf = (uint8_t *)malloc(s_in_bytes);
    s_enc_buf = (uint8_t *)malloc(s_out_cap);
    if (!s_pcm_buf || !s_enc_buf) {
        LOGE_DIAG("mic alloc failed (in=%d out=%d)", s_in_bytes, s_out_cap);
        return false;
    }
    LOGI_DIAG("mic encoder open: in=%d B, out_cap=%d B (br=%d, cmplx=%d)",
        s_in_bytes, s_out_cap, MIC_OPUS_BITRATE, MIC_OPUS_COMPLEXITY);
    return true;
}

static void close_all(void) {
    if (s_record_open && s_record_dev) {
        esp_codec_dev_close(s_record_dev);
    }
    s_record_open = false;
    s_record_dev = NULL;
    if (s_opus_enc) {
        esp_opus_enc_close(s_opus_enc);
        s_opus_enc = NULL;
    }
    free(s_pcm_buf); s_pcm_buf = NULL;
    free(s_enc_buf); s_enc_buf = NULL;
    s_in_bytes = s_out_cap = 0;
}

static void mic_task(void *arg) {
    (void)arg;
    /* Plain ESP_LOGI on entry (not LOGI_DIAG) — see stack-budget
     * comment above MIC_TASK_STACK_BYTES. */
    ESP_LOGI(TAG, "mic_task started");
    s_pcm_frames = s_enc_ok = s_enc_err = 0;
    s_send_ok = s_send_err = s_send_block = s_read_err = 0;
    s_muted_frames = 0;

    while (atomic_load(&s_running)) {
        /* Bail early if the peer's been torn down — we don't want to
         * busy-loop calling esp_peer_send_audio against a closed peer. */
        if (!voice_peer_is_running()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* Read one frame's worth of PCM. esp_codec_dev_read blocks
         * until the I²S buffer has the requested bytes — natural
         * pacing at the sample rate. */
        int rrc = esp_codec_dev_read(s_record_dev, s_pcm_buf, s_in_bytes);
        if (rrc != 0) {
            s_read_err++;
            if (s_read_err <= 3 || (s_read_err & 0x1F) == 0) {
                LOGW_DIAG("codec_dev_read rc=%d (n=%lu)",
                    rrc, (unsigned long)s_read_err);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        s_pcm_frames++;

        /* Software-reference AEC. When enabled, runs the mic frame
         * against the ring of decoded speaker PCM that voice_peer's
         * pc_on_audio_data has been depositing. Returns the cancelled
         * mic in place; no-op when disabled. */
        voice_aec_process_in_place((int16_t *)s_pcm_buf,
            (size_t)(s_in_bytes / (int)sizeof(int16_t)));

        /* Stamp the post-AEC peak. This is the same energy the server
         * VAD sees over the wire (we ship encoded mic that started as
         * this PCM). On a self-interrupt, the model's audio leaked
         * past AEC and we'd expect a moderate-to-high peak with NO
         * preceding human speech; on a real barge-in, the peak is
         * elevated AND co-occurs with user voice. The /api/device/diag
         * line on speech_started includes this value so we can tell
         * the two apart from the val.run side. */
        atomic_store(&s_last_peak_dbfs,
            peak_to_dbfs((int16_t *)s_pcm_buf,
                (size_t)(s_in_bytes / (int)sizeof(int16_t))));

        /* Half-duplex mute fallback. voice_peer_mic_should_mute()
         * returns false whenever voice_aec is enabled, so this gate
         * only fires when AEC failed to init / enable. In that
         * fallback case it's the same over-muting compromise as
         * before (drop mic frames during SPEAKING + drain tail);
         * we keep draining the I²S read above so DMA doesn't
         * overrun. */
        if (voice_peer_mic_should_mute()) {
            s_muted_frames++;
            goto loop_tail;
        }

        /* Encode. */
        esp_audio_enc_in_frame_t in = {
            .buffer = s_pcm_buf,
            .len = (uint32_t)s_in_bytes,
        };
        esp_audio_enc_out_frame_t out = {
            .buffer = s_enc_buf,
            .len = (uint32_t)s_out_cap,
        };
        esp_audio_err_t eerr = esp_opus_enc_process(s_opus_enc, &in, &out);
        if (eerr != ESP_AUDIO_ERR_OK) {
            s_enc_err++;
            if (s_enc_err <= 3 || (s_enc_err & 0x1F) == 0) {
                LOGE_DIAG("opus_enc_process rc=%d (n=%lu)",
                    (int)eerr, (unsigned long)s_enc_err);
            }
            continue;
        }
        s_enc_ok++;
        if (out.encoded_bytes == 0) continue;

        /* Send. WOULD_BLOCK is an "RTP send queue full" hint; drop
         * the frame and continue rather than retry — at our 20 ms
         * cadence, holding back the loop would just back-pressure
         * into the codec read and stretch frames. */
        int src = voice_peer_send_audio_frame(s_enc_buf, (int)out.encoded_bytes);
        if (src == 0) {
            s_send_ok++;
        } else if (src == ESP_PEER_ERR_WOULD_BLOCK) {
            s_send_block++;
        } else {
            s_send_err++;
            if (s_send_err <= 3 || (s_send_err & 0x1F) == 0) {
                LOGE_DIAG("send_audio rc=%d (n=%lu)",
                    src, (unsigned long)s_send_err);
            }
        }

    loop_tail:
        /* Yield once per encoded frame so IDLE on this core gets to
         * reset the watchdog. opus_encode is CPU-bound (~3-4 ms on a
         * 20 ms frame) and we observed task_wdt: IDLE1 starvation
         * when the encoder + codec_read alternated without an
         * explicit reschedule point. esp_codec_dev_read itself blocks
         * on I²S, but only AFTER the encoder finishes — so a yield
         * BEFORE looping back to read is the right insertion. */
        taskYIELD();
        if (s_pcm_frames == 1 || (s_pcm_frames % 50) == 0) {
            voice_diag_log("mic: read=%lu muted=%lu enc_ok=%lu enc_err=%lu "
                "snd_ok=%lu snd_blk=%lu snd_err=%lu",
                (unsigned long)s_pcm_frames,
                (unsigned long)s_muted_frames,
                (unsigned long)s_enc_ok,
                (unsigned long)s_enc_err,
                (unsigned long)s_send_ok,
                (unsigned long)s_send_block,
                (unsigned long)s_send_err);
        }
    }
    LOGI_DIAG("mic_task exit: read=%lu muted=%lu snd_ok=%lu snd_blk=%lu",
        (unsigned long)s_pcm_frames,
        (unsigned long)s_muted_frames,
        (unsigned long)s_send_ok,
        (unsigned long)s_send_block);
    s_task = NULL;
    /* Created via xTaskCreatePinnedToCoreWithCaps → must use the
     * matching delete so the PSRAM stack actually gets freed. */
    vTaskDeleteWithCaps(NULL);
}

bool voice_mic_start(void) {
    if (s_task) return true;  /* already running */

    if (!open_record() || !open_encoder()) {
        close_all();
        return false;
    }

    atomic_store(&s_running, true);
    /* Stack from PSRAM (MALLOC_CAP_SPIRAM) — see stack-budget comment
     * above MIC_TASK_STACK_BYTES. */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        mic_task, "voice_mic", MIC_TASK_STACK_BYTES,
        NULL, MIC_TASK_PRIO, &s_task, MIC_TASK_CORE,
        MALLOC_CAP_SPIRAM);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreateWithCaps failed");
        atomic_store(&s_running, false);
        close_all();
        return false;
    }
    return true;
}

void voice_mic_stop(void) {
    if (!s_task && !s_record_open && !s_opus_enc) return;
    atomic_store(&s_running, false);
    /* Wait briefly for the task to exit. esp_codec_dev_read may be
     * blocked on I²S — set running=false, the loop will check after
     * the read returns (worst case 20 ms). */
    for (int i = 0; i < 30 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    close_all();
}
