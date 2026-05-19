/*
 * Software-reference AEC — implementation. See voice_aec.h for the
 * pipeline diagram, sample-rate posture, and threading rules.
 *
 * Build gate
 * ----------
 * The esp-sr binding (aec_create / aec_process) lives behind
 * VOICE_AEC_USE_ESP_SR. Flip to 1 once espressif/esp-sr is declared
 * in idf_component.yml and you've validated the dep pulls cleanly.
 * With the gate at 0, the module compiles as a "plumbing skeleton":
 * ring buffer + API + lifecycle all wired, process_in_place is a
 * pass-through. That keeps the integration call sites stable while
 * the actual AEC is being brought up.
 *
 * Status (2026-05-19): VOICE_AEC_USE_ESP_SR=0, AEC default OFF.
 * Half-duplex mute in voice_peer_mic_should_mute() remains the
 * active echo defence.
 */

#include "voice_aec.h"
#include "voice_diag.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"

#define TAG "voice_aec"

/* Flip to 1 once espressif/esp-sr is in idf_component.yml and the
 * board is ready for an on-hardware AEC bring-up. */
#ifndef VOICE_AEC_USE_ESP_SR
#define VOICE_AEC_USE_ESP_SR 0
#endif

#if VOICE_AEC_USE_ESP_SR
#include "esp_aec.h"
#endif

/* esp-sr AEC is fixed at 16 kHz. */
#define AEC_INTERNAL_RATE     16000

/* aec_create(sample_rate, filter_length_ms, channel_num, mode):
 *   - filter_length_ms longer = better tail handling, more CPU/RAM.
 *     4 ms is the Espressif "low cost" recommendation; bump to 8 if
 *     the room has long echo tails.
 *   - mode FD_LOW_COST is the full-duplex VoIP-grade setting and
 *     the recommended starting point for single-mic boards. Move
 *     to FD_HIGH_PERF if CPU headroom allows and the low-cost mode
 *     leaves audible bleed. */
#define AEC_FILTER_LENGTH_MS  4
#define AEC_CHANNELS          1

/* Reference ring buffer size — ~1.36 s of 24 kHz mono PCM. 32 Ki
 * samples = 64 KB PSRAM. Power of two so the wrap is a mask, not a
 * modulo; size chosen as the smallest 2^N that holds ≥ 1 s at
 * 24 kHz, which is enough to ride out any reasonable scheduling
 * lag between the audio decode callback and the mic task. */
#define REF_RING_SAMPLES      (1u << 15)
#define REF_RING_MASK         (REF_RING_SAMPLES - 1u)
#define REF_RING_BYTES        ((int)REF_RING_SAMPLES * (int)sizeof(int16_t))

typedef struct {
    int16_t *buf;
    /* head = next write slot; tail = next read slot. SPSC: writer
     * (peer thread) only moves head, reader (mic task) only moves
     * tail. We don't need a separate "count" — count is (head - tail)
     * modulo capacity. Both atomic so the other side sees the update
     * without a memory barrier on each ring access. */
    atomic_uint head;
    atomic_uint tail;
    /* Counts for diag visibility. */
    uint32_t pushed_samples;
    uint32_t pulled_samples;
    uint32_t underrun_samples;
    uint32_t overrun_samples;
} ref_ring_t;

static struct {
    bool         inited;
    int          external_rate;
    int          channels;
    ref_ring_t   ref;
    atomic_bool  enabled;
#if VOICE_AEC_USE_ESP_SR
    aec_handle_t *aec;
    int           aec_chunk_samples;   /* native 16 kHz chunksize */
#endif
    /* Counters for diag output. */
    uint32_t      processed_frames;
    uint32_t      skipped_frames;
    uint32_t      failed_frames;
} s = {0};

/* ─── reference ring buffer ───────────────────────────────────── */

static void ring_push(const int16_t *src, size_t n) {
    if (!s.ref.buf || n == 0) return;
    unsigned head = atomic_load_explicit(&s.ref.head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&s.ref.tail, memory_order_acquire);
    /* If incoming would overrun, advance tail to drop the oldest
     * samples. The reader will see an apparent jump in time — fine
     * for AEC, since stale reference doesn't correlate with the
     * current mic frame anyway. */
    unsigned free = REF_RING_MASK - ((head - tail) & REF_RING_MASK);
    if (n > free) {
        unsigned drop = (unsigned)n - free;
        atomic_store_explicit(&s.ref.tail, tail + drop, memory_order_release);
        s.ref.overrun_samples += drop;
    }
    for (size_t i = 0; i < n; i++) {
        s.ref.buf[(head + i) & REF_RING_MASK] = src[i];
    }
    atomic_store_explicit(&s.ref.head, head + (unsigned)n, memory_order_release);
    s.ref.pushed_samples += n;
}

static void ring_pull(int16_t *dst, size_t n) {
    if (!s.ref.buf || n == 0) return;
    unsigned head = atomic_load_explicit(&s.ref.head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&s.ref.tail, memory_order_relaxed);
    unsigned have = (head - tail) & REF_RING_MASK;
    size_t take = have >= n ? n : have;
    for (size_t i = 0; i < take; i++) {
        dst[i] = s.ref.buf[(tail + i) & REF_RING_MASK];
    }
    /* Zero-fill the rest on underrun. AEC interprets a silent ref as
     * "no echo expected", which is correct when the speaker isn't
     * playing — and harmless to the filter when it is, since the
     * non-silent ref will catch up on the next frame. Advance tail
     * only by what we actually consumed: pushing it past head would
     * wrap unsigned arithmetic into a phantom-full state. */
    if (take < n) {
        memset(&dst[take], 0, (n - take) * sizeof(int16_t));
        s.ref.underrun_samples += (n - take);
    }
    atomic_store_explicit(&s.ref.tail, tail + (unsigned)take, memory_order_release);
    s.ref.pulled_samples += take;
}

/* ─── resampling ──────────────────────────────────────────────── */

/* Linear resample 24 kHz → 16 kHz (factor 2/3) and 16 → 24 (factor
 * 3/2). Crude but bounded-quality; AEC reference + mic tolerance is
 * forgiving compared to the listening path. Upgrade to a polyphase
 * FIR with anti-alias if hardware testing shows the resampler is
 * the bottleneck on echo suppression. */
static void resample_24_to_16(const int16_t *in, size_t in_n,
                              int16_t *out, size_t out_n) {
    if (in_n == 0 || out_n == 0) return;
    /* For each output sample at 16 kHz, sample position in input is
     * i * 24 / 16 = i * 1.5. Linear interp between in[k] and in[k+1]. */
    for (size_t i = 0; i < out_n; i++) {
        uint32_t pos_q16 = (uint32_t)((i * 24u * 65536u) / 16u);
        size_t k = pos_q16 >> 16;
        uint32_t frac = pos_q16 & 0xFFFFu;
        if (k + 1 >= in_n) { out[i] = in[in_n - 1]; continue; }
        int32_t a = in[k];
        int32_t b = in[k + 1];
        out[i] = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
    }
}

static void resample_16_to_24(const int16_t *in, size_t in_n,
                              int16_t *out, size_t out_n) {
    if (in_n == 0 || out_n == 0) return;
    for (size_t i = 0; i < out_n; i++) {
        uint32_t pos_q16 = (uint32_t)((i * 16u * 65536u) / 24u);
        size_t k = pos_q16 >> 16;
        uint32_t frac = pos_q16 & 0xFFFFu;
        if (k + 1 >= in_n) { out[i] = in[in_n - 1]; continue; }
        int32_t a = in[k];
        int32_t b = in[k + 1];
        out[i] = (int16_t)(a + (((b - a) * (int32_t)frac) >> 16));
    }
}

/* ─── public API ──────────────────────────────────────────────── */

bool voice_aec_init(int external_rate, int channels) {
    if (s.inited) return true;
    if (channels != 1) {
        ESP_LOGE(TAG, "only mono supported (got %d ch)", channels);
        return false;
    }
    if (external_rate != 24000) {
        /* Other rates need a different resampler. Keep the assumption
         * explicit until someone wires a second rate intentionally. */
        ESP_LOGE(TAG, "only 24 kHz external supported (got %d)", external_rate);
        return false;
    }
    s.ref.buf = (int16_t *)heap_caps_calloc(
        1, REF_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.ref.buf) {
        ESP_LOGE(TAG, "ref ring alloc failed (%d B)", REF_RING_BYTES);
        return false;
    }
    atomic_store(&s.ref.head, 0u);
    atomic_store(&s.ref.tail, 0u);
    s.ref.pushed_samples = 0;
    s.ref.pulled_samples = 0;
    s.ref.underrun_samples = 0;
    s.ref.overrun_samples = 0;
    s.external_rate = external_rate;
    s.channels = channels;
    s.processed_frames = s.skipped_frames = s.failed_frames = 0;
    atomic_store(&s.enabled, false);
    s.inited = true;
    voice_diag_log("aec: init ext_rate=%d ch=%d ring=%d B (esp_sr=%d)",
        external_rate, channels, REF_RING_BYTES, VOICE_AEC_USE_ESP_SR);
    return true;
}

void voice_aec_deinit(void) {
    if (!s.inited) return;
    atomic_store(&s.enabled, false);
#if VOICE_AEC_USE_ESP_SR
    if (s.aec) {
        aec_destroy(s.aec);
        s.aec = NULL;
        s.aec_chunk_samples = 0;
    }
#endif
    if (s.ref.buf) {
        free(s.ref.buf);
        s.ref.buf = NULL;
    }
    voice_diag_log("aec: deinit (pushed=%lu pulled=%lu under=%lu over=%lu "
        "proc=%lu skip=%lu fail=%lu)",
        (unsigned long)s.ref.pushed_samples,
        (unsigned long)s.ref.pulled_samples,
        (unsigned long)s.ref.underrun_samples,
        (unsigned long)s.ref.overrun_samples,
        (unsigned long)s.processed_frames,
        (unsigned long)s.skipped_frames,
        (unsigned long)s.failed_frames);
    s.inited = false;
}

void voice_aec_set_enabled(bool enabled) {
    if (!s.inited) {
        if (enabled) ESP_LOGW(TAG, "set_enabled(true) before init — ignored");
        return;
    }
#if VOICE_AEC_USE_ESP_SR
    if (enabled && !s.aec) {
        s.aec = aec_create(AEC_INTERNAL_RATE, AEC_FILTER_LENGTH_MS,
                           AEC_CHANNELS, AEC_MODE_FD_LOW_COST);
        if (!s.aec) {
            ESP_LOGE(TAG, "aec_create failed; staying disabled");
            return;
        }
        s.aec_chunk_samples = aec_get_chunksize(s.aec);
        voice_diag_log("aec: created mode=FD_LOW_COST flt=%d ms "
            "chunk=%d samples (16 kHz)",
            AEC_FILTER_LENGTH_MS, s.aec_chunk_samples);
    }
#endif
    atomic_store(&s.enabled, enabled);
    voice_diag_log("aec: %s", enabled ? "ENABLED" : "disabled");
}

bool voice_aec_is_enabled(void) {
    return s.inited && atomic_load(&s.enabled);
}

void voice_aec_push_ref(const int16_t *pcm, size_t samples) {
    if (!s.inited || !pcm || samples == 0) return;
    ring_push(pcm, samples);
}

void voice_aec_process_in_place(int16_t *pcm, size_t samples) {
    if (!s.inited || !pcm || samples == 0) return;
    if (!atomic_load(&s.enabled)) {
        s.skipped_frames++;
        return;
    }
#if !VOICE_AEC_USE_ESP_SR
    /* Plumbing-only build — pass through. The ring buffer still
     * fills via push_ref so the rate of underrun/overrun is
     * observable in diag logs. */
    s.skipped_frames++;
    (void)pcm; (void)samples;
    return;
#else
    if (!s.aec || s.aec_chunk_samples <= 0) {
        s.failed_frames++;
        return;
    }
    /* External (24 kHz) frame → internal (16 kHz). For our 20 ms /
     * 480 sample mic frame that's 320 samples at 16 kHz. The AEC
     * processes in chunksize-sample units (typically 256 at 16 kHz);
     * we accumulate until we have at least one full chunk, then
     * batch-process. Anything left over is held until next call.
     *
     * For the first cut we expect to be called with samples=480
     * every 20 ms, producing exactly 320 internal samples. If the
     * AEC chunksize is 256, that's 1 chunk + 64 carry; if it's 320
     * it's exactly 1 chunk. We handle both with a fixed scratch and
     * a per-call carry buffer.
     *
     * Carry buffer sized for one full external frame of slop. */
    enum { MAX_INT_FRAME = 480 * 2 / 3 + 4 };       /* up to ~322 int samples */
    static int16_t carry_mic[MAX_INT_FRAME * 2];
    static int16_t carry_ref[MAX_INT_FRAME * 2];
    static int     carry_n = 0;

    int16_t mic_int[MAX_INT_FRAME];
    int16_t ref_ext[480];                            /* full external frame */
    int16_t ref_int[MAX_INT_FRAME];

    /* Internal-rate sample count for this external frame. */
    int int_n = (int)((samples * AEC_INTERNAL_RATE) / s.external_rate);
    if (int_n <= 0 || int_n > MAX_INT_FRAME) {
        s.failed_frames++;
        return;
    }

    /* Down-sample mic + ref to 16 kHz. The ref ring stores 24 kHz
     * samples, so pull `samples` of ref and resample alongside. */
    if (samples > sizeof(ref_ext) / sizeof(ref_ext[0])) {
        s.failed_frames++;
        return;
    }
    ring_pull(ref_ext, samples);
    resample_24_to_16(pcm, samples, mic_int, int_n);
    resample_24_to_16(ref_ext, samples, ref_int, int_n);

    /* Append into carry buffer. */
    if (carry_n + int_n > (int)(sizeof(carry_mic) / sizeof(carry_mic[0]))) {
        /* Overflow — reset carry to avoid stalling. Loses one frame
         * of conditioning but recovers cleanly. */
        carry_n = 0;
        s.failed_frames++;
        return;
    }
    memcpy(&carry_mic[carry_n], mic_int, int_n * sizeof(int16_t));
    memcpy(&carry_ref[carry_n], ref_int, int_n * sizeof(int16_t));
    carry_n += int_n;

    /* Process as many full chunks as we have. AEC output replaces
     * the mic samples in place; we accumulate the output for the
     * upsample step. */
    int16_t aec_out_int[MAX_INT_FRAME * 2];
    int     out_int_n = 0;
    while (carry_n >= s.aec_chunk_samples) {
        aec_process(s.aec,
            &carry_mic[0],
            &carry_ref[0],
            &aec_out_int[out_int_n]);
        out_int_n += s.aec_chunk_samples;
        /* Shift remainder down. */
        int remain = carry_n - s.aec_chunk_samples;
        memmove(&carry_mic[0], &carry_mic[s.aec_chunk_samples],
                remain * sizeof(int16_t));
        memmove(&carry_ref[0], &carry_ref[s.aec_chunk_samples],
                remain * sizeof(int16_t));
        carry_n = remain;
    }

    if (out_int_n == 0) {
        /* Not enough samples yet to run a full chunk — leave mic
         * frame untouched and try again next call. The mute fallback
         * will be doing its job until the filter has warmed. */
        s.skipped_frames++;
        return;
    }

    /* Upsample AEC output 16→24 back into caller's buffer.
     * out_int_n internal samples → out_int_n * 3 / 2 external. If
     * that differs from `samples` (rare, when chunksize and our
     * frame don't align), we map what we have and leave any tail
     * of the caller's buffer as-is. */
    size_t out_ext_n = (size_t)out_int_n * s.external_rate / AEC_INTERNAL_RATE;
    if (out_ext_n > samples) out_ext_n = samples;
    resample_16_to_24(aec_out_int, out_int_n, pcm, out_ext_n);

    s.processed_frames++;
#endif /* VOICE_AEC_USE_ESP_SR */
}
