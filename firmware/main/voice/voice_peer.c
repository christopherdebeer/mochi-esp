/*
 * Voice peer connection — esp_peer-direct glue with Opus playback.
 *
 * Single-session model: one global voice_peer_t, started/stopped
 * via voice_peer_start / voice_peer_stop. We don't need multiple
 * concurrent sessions on the device.
 *
 * Threading:
 *   - The signaling impl runs synchronously on whichever task calls
 *     voice_peer_start. Both the mint POST and the SDP POST happen
 *     on that task. That task's stack must be >= 8 KB to handle the
 *     TLS handshakes.
 *   - The peer main loop runs on a dedicated worker task with 12 KB
 *     stack. mbedTLS DTLS handshakes happen post-SDP on this task.
 *   - All state callbacks (pc_on_state, pc_on_data, pc_on_audio_*)
 *     fire from the worker task. The audio path does Opus decode +
 *     esp_codec_dev_write inline; both are blocking but their costs
 *     pace naturally with frame arrival.
 *
 * Playback:
 *   on_audio_info → open Opus decoder + esp_codec_dev playback at the
 *                   negotiated sample_rate / channel
 *   on_audio_data → decode → write PCM16 to esp_codec_dev_write.
 *
 * No mic capture yet (M9.f.2 follow-up). The negotiated audio_dir is
 * still SEND_RECV in the SDP because OpenAI rejects RECV_ONLY for the
 * Realtime endpoint, but we don't actually push audio frames upstream.
 */

#include "voice_peer.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_peer_signaling.h"
#include "openai_signaling.h"

#include "esp_codec_dev.h"
#include "esp_opus_dec.h"
#include "codec_init.h"
#include "voice_diag.h"
#include "voice_tools.h"
#include "voice_mic.h"
#include "voice_aec.h"

#define TAG "voice_peer"

/* Mirror an ESP_LOGI to both serial and the voice diag buffer.
 * voice_diag is a no-op until reset() is called by start, and
 * survives USB disconnect via flush() in stop. */
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

#define WORKER_STACK_BYTES  (12 * 1024)
#define WORKER_PRIO         5
#define MAIN_LOOP_SLEEP_MS  10

/* Forward decl + helper: flips s_peer.phase atomically and logs the
 * transition. Defined after s_peer so it can reach the field. */
static const char *phase_name(voice_phase_t p) {
    switch (p) {
        case VOICE_PHASE_IDLE:       return "IDLE";
        case VOICE_PHASE_CONNECTING: return "CONNECTING";
        case VOICE_PHASE_READY:      return "READY";
        case VOICE_PHASE_SPEAKING:   return "SPEAKING";
    }
    return "?";
}

/* PCM16 output buffer for the Opus decoder. Max Opus frame is 120 ms;
 * at 24 kHz mono = 2880 samples = 5760 bytes. Round up to 8 KB so
 * we don't have to grow the buffer on edge-case frames. */
#define OPUS_PCM_OUT_BYTES  (8 * 1024)

/*
 * Session caps. Match the policy in design/07-voice-architecture.md:
 *   idle  — no audio for N seconds → close. Forgives the user
 *           thinking but doesn't burn tokens on an abandoned session.
 *   hard  — absolute ceiling. Bounds cost if anything else fails to
 *           release the session.
 *
 * 60 s idle is more permissive than the design doc's 30 s during
 * bring-up — easier to validate "the cap fires" without
 * accidentally cutting off a real exchange. The hard cap is 5 min
 * not 10 (the doc's number) for the same reason: testing budget.
 * Both can move once the real conversation lifecycle is dialled in.
 */
#define VOICE_IDLE_CAP_MS     (60   * 1000)
#define VOICE_HARD_CAP_MS     (5    * 60 * 1000)

/* How long to keep the mic muted after the last "loud" audio frame
 * received from the server. Loud = Opus packet > VOICE_DTX_FRAME_BYTES,
 * i.e., real voice rather than 3-byte comfort-noise / DTX. This is
 * the right signal because:
 *   - response.done fires when the model finishes _generating_, but
 *     the server keeps streaming audio into our buffer for a while
 *     after that (and our I²S DMA is still draining). Gating on
 *     phase = SPEAKING under-shoots.
 *   - output_audio_buffer.stopped is the server's _estimate_ of when
 *     we're done playing — it pads heavily (often 10 s late). Gating
 *     on that over-shoots and locks the user out of the conversation.
 *   - Last-loud-frame plus a fixed drain window tracks real playback.
 *
 * 700 ms ≈ I²S DMA drain (~100 ms typical) + speaker decay + a small
 * margin. Tunable. */
#define VOICE_LOUD_DRAIN_MS    700

/* Opus packet size threshold below which we treat a frame as DTX /
 * comfort noise. The Opus DTX packet is 3 bytes; we add a couple of
 * bytes of slack in case the server uses slightly larger silence-frame
 * encodings. Real voice frames at 24 kbps / 20 ms run ~60 B. */
#define VOICE_DTX_FRAME_BYTES  5

/* Caps the first-event log so we don't dump megabytes if OpenAI
 * sends something unexpected. */
#define FIRST_EVENT_LOG_BYTES  256

typedef struct {
    esp_peer_handle_t           pc;
    esp_peer_signaling_handle_t sig;
    TaskHandle_t                worker;
    atomic_bool                 running;
    atomic_bool                 dc_opened;
    atomic_bool                 first_event_seen;
    atomic_int                  phase;  /* voice_phase_t */
    /*
     * Set by the worker (idle / hard cap) or by pc_on_state when the
     * peer transitions to DISCONNECTED / CONNECT_FAILED. Polled by
     * main.cpp's touch loop, which calls voice::stop_session() from
     * its own task context to avoid deadlocking on worker-join.
     */
    atomic_bool                 stop_requested;
    /* Timestamps for the cap check, in esp_timer_get_time() units (us).
     * `start_us` is set in voice_peer_start; `last_audio_us` updates
     * on every audio frame received (so silence between turns counts
     * toward the idle cap, but mid-utterance gaps don't). */
    int64_t                     start_us;
    atomic_llong                last_audio_us;
    /* Lifetime-managed copies of strings handed to the signaling impl.
     * The impl reads through .token / .instructions on the start() call;
     * holding the storage here keeps it valid through the SDP exchange
     * and any reconnect. Both wiped + freed on stop. */
    char                       *key_copy;
    char                       *instructions_copy;

    /* Audio-out plumbing. Opens lazily on pc_on_audio_info. */
    void                       *opus_dec;       /* esp_opus_dec handle */
    uint8_t                    *pcm_out_buf;    /* OPUS_PCM_OUT_BYTES */
    esp_codec_dev_handle_t      play_dev;
    bool                        play_open;

    /* Stream id for the data channel — captured on on_channel_open,
     * needed to send response.create back to OpenAI. */
    uint16_t                    dc_stream_id;
    bool                        dc_stream_known;

    /* Tail-mute deadline: timestamp (us) until which voice_mic should
     * keep muting even after phase has dropped from SPEAKING. The
     * speaker DMA continues draining ~600 ms after we stop feeding
     * decoder output, so the ES8311's single mic still picks up that
     * bleed and triggers server-side VAD → self-interrupt. Setting a
     * future deadline on every audio frame received pins this to "X
     * ms after the LAST audio frame", regardless of how phase moves. */
    atomic_llong                tail_mute_until_us;
} voice_peer_t;

static voice_peer_t s_peer;  /* zero-initialised at .bss */

static void set_phase(voice_phase_t next) {
    voice_phase_t prev = (voice_phase_t)atomic_exchange(&s_peer.phase, next);
    if (prev != next) {
        LOGI_DIAG("phase: %s → %s", phase_name(prev), phase_name(next));
    }
}

/* ─── audio playback helpers ──────────────────────────────────── */

static void open_audio_playback(uint32_t sample_rate, uint8_t channel) {
    if (s_peer.opus_dec) {
        ESP_LOGW(TAG, "opus dec already open; not re-opening");
        return;
    }
    esp_opus_dec_cfg_t cfg = {
        .sample_rate = sample_rate,
        .channel = channel,
        /* INVALID makes the decoder size pcm-out as a 60 ms frame —
         * fine for our ≤120 ms output buffer. */
        .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID,
        .self_delimited = false,
    };
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &s_peer.opus_dec) != ESP_AUDIO_ERR_OK) {
        LOGE_DIAG("esp_opus_dec_open failed");
        return;
    }
    s_peer.pcm_out_buf = malloc(OPUS_PCM_OUT_BYTES);
    if (!s_peer.pcm_out_buf) {
        LOGE_DIAG("pcm_out_buf alloc failed");
        esp_opus_dec_close(s_peer.opus_dec);
        s_peer.opus_dec = NULL;
        return;
    }

    s_peer.play_dev = get_playback_handle();
    if (!s_peer.play_dev) {
        LOGE_DIAG("no playback handle from codec_board");
        return;
    }
    /* Match the negotiated PCM format. ES8311 will run its DAC at
     * this rate via the I²S MCLK config codec_board set up. */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = channel,
        .channel_mask = 0,
        .sample_rate = sample_rate,
    };
    int rc = esp_codec_dev_open(s_peer.play_dev, &fs);
    if (rc != 0) {
        LOGE_DIAG("esp_codec_dev_open rc=%d", rc);
        return;
    }
    /* 85 is loud-but-not-shouty for the Waveshare's small speaker.
     * Above ~90 the ES8311 starts to clip noticeably. */
    esp_codec_dev_set_out_vol(s_peer.play_dev, 85);
    s_peer.play_open = true;
    LOGI_DIAG("playback open: %lu Hz, %d ch",
        (unsigned long)sample_rate, channel);

    /* Stand up the software-reference AEC alongside playback and
     * engage it for the whole session. Once enabled,
     * voice_peer_mic_should_mute() returns false — the canceller
     * is responsible for echo defence and barge-in works. If init
     * or aec_create fails, the half-duplex mute remains active as
     * the fallback. */
    if (voice_aec_init((int)sample_rate, channel)) {
        voice_aec_set_enabled(true);
    } else {
        LOGW_DIAG("voice_aec_init failed; falling back to half-duplex mute");
    }
}

static void close_audio_playback(void) {
    voice_aec_deinit();
    if (s_peer.play_open && s_peer.play_dev) {
        esp_codec_dev_close(s_peer.play_dev);
        s_peer.play_open = false;
    }
    s_peer.play_dev = NULL;
    if (s_peer.opus_dec) {
        esp_opus_dec_close(s_peer.opus_dec);
        s_peer.opus_dec = NULL;
    }
    if (s_peer.pcm_out_buf) {
        free(s_peer.pcm_out_buf);
        s_peer.pcm_out_buf = NULL;
    }
}

/* ─── data-channel send (response.create) ─────────────────────── */

static void send_response_create(void) {
    if (!s_peer.dc_stream_known) {
        ESP_LOGW(TAG, "no dc stream id yet; cannot send response.create");
        return;
    }
    /* Minimal "kick the model into producing something" event. With
     * instructions bound at mint time, this is enough for OpenAI to
     * generate an opening greeting. The session.update + tools shape
     * comes in M9.g+. */
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "response.create");
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    esp_peer_data_frame_t f = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = s_peer.dc_stream_id,
        .data = (uint8_t *)json,
        .size = (int)strlen(json),
    };
    int rc = esp_peer_send_data(s_peer.pc, &f);
    if (rc != ESP_PEER_ERR_NONE) {
        LOGE_DIAG("send_data response.create rc=%d", rc);
    } else {
        LOGI_DIAG("sent response.create on dc stream %u",
            (unsigned)s_peer.dc_stream_id);
    }
    free(json);
}

/* ─── peer-side callbacks ─────────────────────────────────────── */

static int pc_on_state(esp_peer_state_t state, void *ctx) {
    (void)ctx;
    LOGI_DIAG("peer state → %d", (int)state);
    if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED) {
        /* SCTP up but DCEP not yet opened. With OpenAI Realtime as
         * the peer, the device has to initiate the channel — OpenAI
         * doesn't push DCEP from its side (and esp_peer's auto-create
         * only fires when its SCTP role is client, which it isn't
         * here). Same shape as upstream openai_demo's webrtc.c:466.
         * Label matches mochi-val's `pc.createDataChannel("oai-events")`. */
        esp_peer_data_channel_cfg_t cfg = { 0 };
        cfg.label = "oai-events";
        int rc = esp_peer_create_data_channel(s_peer.pc, &cfg);
        if (rc != ESP_PEER_ERR_NONE) {
            LOGE_DIAG("esp_peer_create_data_channel failed: %d", rc);
        } else {
            LOGI_DIAG("manual DCEP open: 'oai-events'");
        }
    } else if (state == ESP_PEER_STATE_DATA_CHANNEL_OPENED) {
        atomic_store(&s_peer.dc_opened, true);
        set_phase(VOICE_PHASE_READY);
        LOGI_DIAG("DATA_CHANNEL_OPENED — sending response.create");
        send_response_create();
    } else if (state == ESP_PEER_STATE_CONNECT_FAILED) {
        LOGE_DIAG("peer connect FAILED — requesting stop");
        set_phase(VOICE_PHASE_IDLE);
        atomic_store(&s_peer.stop_requested, true);
    } else if (state == ESP_PEER_STATE_DISCONNECTED) {
        /* Remote-side close OR our own voice_peer_disconnect from
         * within stop. Distinguish: if the worker is already winding
         * down (running=false), this is the local-stop case and the
         * stop_requested flag is irrelevant. If the worker is still
         * running, the remote dropped us and the touch loop needs to
         * know so is_active() doesn't lie indefinitely. */
        LOGW_DIAG("peer DISCONNECTED");
        if (atomic_load(&s_peer.running)) {
            set_phase(VOICE_PHASE_IDLE);
            atomic_store(&s_peer.stop_requested, true);
        }
    }
    return 0;
}

static int pc_on_msg(esp_peer_msg_t *info, void *ctx) {
    (void)ctx;
    LOGI_DIAG("peer → signaling: type=%d size=%d",
        (int)info->type, info->size);
    const esp_peer_signaling_impl_t *impl =
        esp_signaling_get_openai_signaling();
    esp_peer_signaling_msg_t sig_msg = {
        .type = (esp_peer_signaling_msg_type_t)info->type,
        .data = info->data,
        .size = info->size,
    };
    return impl->send_msg(s_peer.sig, &sig_msg);
}

static int pc_on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx) {
    (void)ctx;
    LOGI_DIAG("data channel opened: label='%s' stream_id=%u",
        ch->label ? ch->label : "(null)", (unsigned)ch->stream_id);
    s_peer.dc_stream_id = ch->stream_id;
    s_peer.dc_stream_known = true;
    return 0;
}

/* ─── pending tool-call accumulators ───────────────────────────
 *
 * OpenAI streams a tool call across three event types:
 *   1. response.output_item.added  with item.type === "function_call"
 *      → captures (call_id, name)
 *   2. response.function_call_arguments.delta × N
 *      → text chunks of the JSON arguments string, by call_id
 *   3. response.function_call_arguments.done
 *      → may include the final arguments inline; if not, use accumulator
 *
 * We keep a small fixed-size table so multiple in-flight tool calls
 * across a turn (asleep mode legitimately fires many) don't collide.
 * 4 slots is plenty — observed in mochi-val: typically 1 per turn,
 * occasional 2-3 in dream-capture bursts.
 */
#define MAX_PENDING_TOOLS 4
typedef struct {
    bool   in_use;
    char   call_id[40];
    char   name[40];
    char  *args;       /* growing buffer, malloc'd */
    size_t args_len;
    size_t args_cap;
} pending_tool_t;

static pending_tool_t s_pending_tools[MAX_PENDING_TOOLS];

static pending_tool_t *pending_find(const char *call_id) {
    if (!call_id) return NULL;
    for (int i = 0; i < MAX_PENDING_TOOLS; i++) {
        if (s_pending_tools[i].in_use &&
            strcmp(s_pending_tools[i].call_id, call_id) == 0) {
            return &s_pending_tools[i];
        }
    }
    return NULL;
}

static pending_tool_t *pending_alloc(const char *call_id, const char *name) {
    for (int i = 0; i < MAX_PENDING_TOOLS; i++) {
        if (!s_pending_tools[i].in_use) {
            pending_tool_t *t = &s_pending_tools[i];
            t->in_use = true;
            strncpy(t->call_id, call_id, sizeof(t->call_id) - 1);
            t->call_id[sizeof(t->call_id) - 1] = 0;
            strncpy(t->name, name, sizeof(t->name) - 1);
            t->name[sizeof(t->name) - 1] = 0;
            t->args = NULL;
            t->args_len = 0;
            t->args_cap = 0;
            return t;
        }
    }
    return NULL;
}

static void pending_release(pending_tool_t *t) {
    if (!t) return;
    free(t->args);
    memset(t, 0, sizeof(*t));
}

static void pending_release_all(void) {
    for (int i = 0; i < MAX_PENDING_TOOLS; i++) {
        pending_release(&s_pending_tools[i]);
    }
}

static void pending_append_args(pending_tool_t *t, const char *delta) {
    if (!t || !delta) return;
    size_t dlen = strlen(delta);
    if (t->args_len + dlen + 1 > t->args_cap) {
        size_t new_cap = t->args_cap ? t->args_cap * 2 : 128;
        while (new_cap < t->args_len + dlen + 1) new_cap *= 2;
        char *grown = (char *)realloc(t->args, new_cap);
        if (!grown) return;
        t->args = grown;
        t->args_cap = new_cap;
    }
    memcpy(t->args + t->args_len, delta, dlen);
    t->args_len += dlen;
    t->args[t->args_len] = 0;
}

/* Substring match against frame->data without alloc. Frames are
 * NUL-bounded JSON in practice (the data channel passes them as
 * UTF-8 strings) but we use length-bounded compare to be safe. */
static bool event_type_is(const char *body, int body_len,
                          const char *type_str) {
    if (!body || body_len <= 0) return false;
    const char *p = strstr(body, "\"type\":\"");
    if (!p) return false;
    p += 8;
    int n = (int)strlen(type_str);
    if (p + n > body + body_len) return false;
    if (strncmp(p, type_str, n) != 0) return false;
    /* Make sure the match is followed by a closing quote, not a
     * longer event whose type string starts with our needle. */
    return p[n] == '"';
}

static int pc_on_data(esp_peer_data_frame_t *frame, void *ctx) {
    (void)ctx;
    const char *body = (const char *)frame->data;
    int body_len = frame->size;

    /* Extract event type once for both logging + phase transitions. */
    char type_buf[64] = {0};
    {
        const char *p = strstr(body, "\"type\":\"");
        if (p) {
            p += 8;
            const char *q = strchr(p, '"');
            if (q && q > p && (q - p) < (int)sizeof(type_buf)) {
                memcpy(type_buf, p, q - p);
                type_buf[q - p] = 0;
            }
        }
    }

    if (!atomic_load(&s_peer.first_event_seen)) {
        atomic_store(&s_peer.first_event_seen, true);
        int n = body_len < FIRST_EVENT_LOG_BYTES
            ? body_len : FIRST_EVENT_LOG_BYTES;
        ESP_LOGI(TAG, "first data-channel event (size=%d, first %d B):",
            body_len, n);
        ESP_LOGI(TAG, "%.*s", n, body);
        if (type_buf[0]) {
            voice_diag_log("dc event[0] type=%s size=%d", type_buf, body_len);
        }
    } else if (type_buf[0]) {
        LOGI_DIAG("dc event type=%s size=%d", type_buf, body_len);
    }

    /* Phase transitions driven by specific event types. The mic-mute
     * deadline is driven by the audio path itself (last loud Opus frame
     * + drain window — see VOICE_LOUD_DRAIN_MS), not by these events,
     * because event-driven gating either over- or under-shoots the
     * actual speaker drain.
     *
     *   output_audio_buffer.started → SPEAKING (UI hint)
     *   response.done               → READY (so UI can move on)
     *   output_audio_buffer.stopped → READY (server's late "done")
     */
    if (event_type_is(body, body_len, "output_audio_buffer.started")) {
        set_phase(VOICE_PHASE_SPEAKING);
    } else if (event_type_is(body, body_len, "output_audio_buffer.stopped") ||
               event_type_is(body, body_len, "response.done")) {
        voice_phase_t cur = (voice_phase_t)atomic_load(&s_peer.phase);
        if (cur == VOICE_PHASE_SPEAKING) {
            set_phase(VOICE_PHASE_READY);
        }
    }

    /* ── Tool call routing (M9.h) ──
     *
     * Three event types matter:
     *   response.output_item.added            — captures (call_id, name)
     *   response.function_call_arguments.delta — accumulates args text
     *   response.function_call_arguments.done  — dispatches via val.run
     *
     * We cJSON_Parse the body once per matched event. Cheap at the
     * ~250 B per event we see on the wire. Anything that doesn't
     * involve tool calls skips the parse entirely via type_buf check.
     */
    if (strcmp(type_buf, "response.output_item.added") == 0 ||
        strcmp(type_buf, "response.function_call_arguments.delta") == 0 ||
        strcmp(type_buf, "response.function_call_arguments.done") == 0)
    {
        cJSON *root = cJSON_Parse(body);
        if (!root) {
            LOGW_DIAG("tool event JSON parse failed");
            return 0;
        }

        if (strcmp(type_buf, "response.output_item.added") == 0) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "item");
            if (cJSON_IsObject(item)) {
                cJSON *itype = cJSON_GetObjectItemCaseSensitive(item, "type");
                if (cJSON_IsString(itype) && itype->valuestring &&
                    strcmp(itype->valuestring, "function_call") == 0) {
                    cJSON *cid = cJSON_GetObjectItemCaseSensitive(item, "call_id");
                    cJSON *nm = cJSON_GetObjectItemCaseSensitive(item, "name");
                    if (cJSON_IsString(cid) && cJSON_IsString(nm)) {
                        pending_tool_t *t = pending_alloc(
                            cid->valuestring, nm->valuestring);
                        if (t) {
                            LOGI_DIAG("tool begin: %s call_id=%s",
                                t->name, t->call_id);
                        } else {
                            LOGW_DIAG("pending tool table full; dropping %s",
                                nm->valuestring);
                        }
                    }
                }
            }
        } else if (strcmp(type_buf, "response.function_call_arguments.delta") == 0) {
            cJSON *cid = cJSON_GetObjectItemCaseSensitive(root, "call_id");
            cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
            if (cJSON_IsString(cid) && cJSON_IsString(delta)) {
                pending_tool_t *t = pending_find(cid->valuestring);
                if (t) {
                    pending_append_args(t, delta->valuestring);
                }
            }
        } else if (strcmp(type_buf, "response.function_call_arguments.done") == 0) {
            cJSON *cid = cJSON_GetObjectItemCaseSensitive(root, "call_id");
            if (cJSON_IsString(cid)) {
                pending_tool_t *t = pending_find(cid->valuestring);
                /* Final args may come inline on .done OR be the
                 * accumulator from .delta events. Prefer inline. */
                cJSON *inline_args = cJSON_GetObjectItemCaseSensitive(
                    root, "arguments");
                const char *args_str = NULL;
                if (cJSON_IsString(inline_args) && inline_args->valuestring) {
                    args_str = inline_args->valuestring;
                } else if (t && t->args) {
                    args_str = t->args;
                }
                /* Tool name: prefer the table entry; fall back to a
                 * .name field if the model put one on .done directly. */
                const char *name_str = NULL;
                cJSON *inline_name = cJSON_GetObjectItemCaseSensitive(
                    root, "name");
                if (cJSON_IsString(inline_name) && inline_name->valuestring) {
                    name_str = inline_name->valuestring;
                } else if (t) {
                    name_str = t->name;
                }
                if (name_str) {
                    LOGI_DIAG("tool dispatch: %s call_id=%s args=%u B",
                        name_str, cid->valuestring,
                        (unsigned)(args_str ? strlen(args_str) : 0));
                    voice_tools_dispatch(cid->valuestring, name_str, args_str);
                } else {
                    LOGW_DIAG("tool .done with no known name (call_id=%s)",
                        cid->valuestring);
                }
                if (t) pending_release(t);
            }
        }
        cJSON_Delete(root);
    }

    /* Transcript capture: log final user + assistant transcripts into
     * the diag buffer so post-disconnect dumps show "what mochi heard"
     * vs "what mochi said." Truncated to 200 chars to fit within the
     * 4 KB diag cap. Both event types fire ONCE per turn.
     *
     *   conversation.item.done with role=user → user's transcribed
     *     audio. content[].transcript is what STT produced.
     *   response.output_audio_transcript.done → mochi's full reply
     *     transcript at $.transcript.
     */
    if (strcmp(type_buf, "conversation.item.done") == 0 ||
        strcmp(type_buf, "response.output_audio_transcript.done") == 0)
    {
        cJSON *root = cJSON_Parse(body);
        if (root) {
            if (strcmp(type_buf, "response.output_audio_transcript.done") == 0) {
                cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "transcript");
                if (cJSON_IsString(t) && t->valuestring) {
                    voice_diag_log("ASSISTANT: %.200s", t->valuestring);
                }
            } else {
                /* conversation.item.done — only log when role=user.
                 * Assistant items get logged via the response.* path
                 * above; doing both would double-log mochi's reply. */
                cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "item");
                if (cJSON_IsObject(item)) {
                    cJSON *role = cJSON_GetObjectItemCaseSensitive(item, "role");
                    if (cJSON_IsString(role) && role->valuestring &&
                        strcmp(role->valuestring, "user") == 0) {
                        cJSON *content = cJSON_GetObjectItemCaseSensitive(item, "content");
                        if (cJSON_IsArray(content)) {
                            cJSON *part = NULL;
                            cJSON_ArrayForEach(part, content) {
                                /* Audio-input parts carry transcript;
                                 * text-input parts carry text. */
                                cJSON *tr = cJSON_GetObjectItemCaseSensitive(
                                    part, "transcript");
                                if (cJSON_IsString(tr) && tr->valuestring) {
                                    voice_diag_log("USER: %.200s", tr->valuestring);
                                    break;
                                }
                                cJSON *tx = cJSON_GetObjectItemCaseSensitive(
                                    part, "text");
                                if (cJSON_IsString(tx) && tx->valuestring) {
                                    voice_diag_log("USER (text): %.200s",
                                        tx->valuestring);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            cJSON_Delete(root);
        }
    }

    return 0;
}

static int pc_on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx) {
    (void)ctx;
    LOGI_DIAG("negotiated audio: codec=%d rate=%lu ch=%d",
        (int)info->codec, (unsigned long)info->sample_rate, info->channel);
    if (info->codec != ESP_PEER_AUDIO_CODEC_OPUS) {
        LOGE_DIAG("unsupported codec — only Opus wired today");
        return -1;
    }
    open_audio_playback(info->sample_rate, info->channel);
    /* Start mic capture as soon as the codec/I²S is configured. The
     * mic task will stall on voice_peer_is_running()=false until the
     * peer reaches CONNECTED, so encoded frames don't get pushed
     * before the SRTP transport is ready. */
    if (!voice_mic_start()) {
        LOGW_DIAG("voice_mic_start failed — talking back disabled");
    }
    return 0;
}

/* Audio counters — logged periodically into the diag buffer so the
 * persistent log captures "we got N frames, M decoded, K written"
 * without one line per 20-ms RTP packet. */
static uint32_t s_aud_recv_n;
static uint32_t s_aud_dec_ok;
static uint32_t s_aud_dec_err;
static uint32_t s_aud_write_err;
static uint32_t s_aud_pcm_bytes;

static int pc_on_audio_data(esp_peer_audio_frame_t *info, void *ctx) {
    (void)ctx;
    if (!atomic_load(&s_peer.running)) return 0;
    if (!s_peer.opus_dec || !s_peer.play_open || !s_peer.pcm_out_buf) {
        return 0;  /* still bringing up; drop quietly */
    }

    s_aud_recv_n++;
    int64_t now_us = esp_timer_get_time();
    atomic_store(&s_peer.last_audio_us, (long long)now_us);
    /* Keep the mic muted past the real speaker drain. See
     * VOICE_LOUD_DRAIN_MS for rationale. Gating on info->size > DTX
     * threshold means silence frames don't extend the deadline — we
     * unmute as soon as the server stops sending real voice. */
    if (info->size > VOICE_DTX_FRAME_BYTES) {
        atomic_store(&s_peer.tail_mute_until_us,
            (long long)(now_us + (int64_t)VOICE_LOUD_DRAIN_MS * 1000));
    }

    /* Log the TOC byte of the first few frames — Opus's first byte
     * encodes config (bandwidth + frame size) + channel + frame count.
     * Helps identify whether OpenAI is sending stereo, multi-frame,
     * or some non-standard config that doesn't match our decoder. */
    if (s_aud_recv_n <= 3 && info->size > 0) {
        LOGI_DIAG("audio frame[%lu] size=%d TOC=0x%02X bytes=%02X %02X %02X",
            (unsigned long)s_aud_recv_n, info->size,
            info->data[0],
            info->data[0],
            info->size >= 2 ? info->data[1] : 0,
            info->size >= 3 ? info->data[2] : 0);
    }

    esp_audio_dec_in_raw_t raw = {
        .buffer = info->data,
        .len = (uint32_t)info->size,
    };
    esp_audio_dec_out_frame_t out = {
        .buffer = s_peer.pcm_out_buf,
        .len = OPUS_PCM_OUT_BYTES,
    };
    esp_audio_dec_info_t dec_info = {0};
    /* Opus packets from WebRTC come as one packet per RTP — just call
     * decode once. Loop guards against any future case where decode
     * could partially consume. */
    while (raw.len) {
        esp_audio_err_t derr = esp_opus_dec_decode(s_peer.opus_dec, &raw, &out, &dec_info);
        if (derr != ESP_AUDIO_ERR_OK) {
            s_aud_dec_err++;
            if (s_aud_dec_err <= 3 || (s_aud_dec_err & 0x1F) == 0) {
                LOGE_DIAG("opus_dec_decode err=%d (in=%lu, n_err=%lu)",
                    (int)derr, (unsigned long)raw.len,
                    (unsigned long)s_aud_dec_err);
            }
            break;
        }
        s_aud_dec_ok++;
        if (s_aud_dec_ok <= 3) {
            LOGI_DIAG("decoded[%lu]: pcm=%lu B, info{rate=%lu ch=%d bps=%d}",
                (unsigned long)s_aud_dec_ok,
                (unsigned long)out.decoded_size,
                (unsigned long)dec_info.sample_rate,
                dec_info.channel,
                dec_info.bits_per_sample);
        }
        if (out.decoded_size > 0) {
            s_aud_pcm_bytes += out.decoded_size;
            /* Reference tap for software AEC. The exact PCM about to
             * hit the speaker is the cleanest reference signal we can
             * give the canceller — taken here before any codec-side
             * processing or DMA framing. No-op when voice_aec is
             * disabled or not inited. */
            voice_aec_push_ref((const int16_t *)s_peer.pcm_out_buf,
                (size_t)(out.decoded_size / sizeof(int16_t)));
            int wrc = esp_codec_dev_write(s_peer.play_dev,
                s_peer.pcm_out_buf, (int)out.decoded_size);
            if (wrc != 0) {
                s_aud_write_err++;
                if (s_aud_write_err <= 3 || (s_aud_write_err & 0x1F) == 0) {
                    LOGW_DIAG("codec_dev_write rc=%d (n_err=%lu)",
                        wrc, (unsigned long)s_aud_write_err);
                }
            }
        }
        if (raw.consumed == 0) break;  /* defensive: avoid infinite loop */
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }

    /* Periodic snapshot into the diag buffer (every 50 frames ≈ 1 s
     * at Opus 20 ms). Log to ESP-only after the first; the diag buffer
     * gets one line per second of audio. */
    if (s_aud_recv_n == 1 || (s_aud_recv_n % 50) == 0) {
        voice_diag_log("audio: rx=%lu dec_ok=%lu dec_err=%lu wr_err=%lu pcm=%lu B",
            (unsigned long)s_aud_recv_n,
            (unsigned long)s_aud_dec_ok,
            (unsigned long)s_aud_dec_err,
            (unsigned long)s_aud_write_err,
            (unsigned long)s_aud_pcm_bytes);
    }
    return 0;
}

/* ─── signaling-side callbacks ────────────────────────────────── */

static int sig_on_ice_info(esp_peer_signaling_ice_info_t *info, void *ctx) {
    (void)ctx;
    LOGI_DIAG("sig → ice_info (initiator=%d, stun=%s)",
        info->is_initiator,
        info->server_info.stun_url ? info->server_info.stun_url : "(none)");

    esp_peer_cfg_t peer_cfg = {};
    peer_cfg.role = info->is_initiator
        ? ESP_PEER_ROLE_CONTROLLING
        : ESP_PEER_ROLE_CONTROLLED;
    peer_cfg.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL;
    peer_cfg.audio_info.codec = ESP_PEER_AUDIO_CODEC_OPUS;
    peer_cfg.audio_info.sample_rate = 24000;  /* gpt-realtime native */
    peer_cfg.audio_info.channel = 1;
    peer_cfg.audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
    peer_cfg.video_dir = ESP_PEER_MEDIA_DIR_NONE;
    peer_cfg.enable_data_channel = true;
    peer_cfg.manual_ch_create = true;   /* SCTP-server side; auto-create
                                            doesn't fire. We open the
                                            channel manually on
                                            DATA_CHANNEL_CONNECTED. */
    peer_cfg.no_auto_reconnect = true;  /* one-shot for now */
    peer_cfg.on_state = pc_on_state;
    peer_cfg.on_msg = pc_on_msg;
    peer_cfg.on_data = pc_on_data;
    peer_cfg.on_channel_open = pc_on_channel_open;
    peer_cfg.on_audio_info = pc_on_audio_info;
    peer_cfg.on_audio_data = pc_on_audio_data;

    int rc = esp_peer_open(&peer_cfg, esp_peer_get_default_impl(), &s_peer.pc);
    if (rc != ESP_PEER_ERR_NONE) {
        LOGE_DIAG("esp_peer_open failed: %d", rc);
        return rc;
    }
    LOGI_DIAG("peer opened");
    return 0;
}

static int sig_on_connected(void *ctx) {
    (void)ctx;
    if (!s_peer.pc) {
        LOGE_DIAG("sig connected but no peer");
        return -1;
    }
    int rc = esp_peer_new_connection(s_peer.pc);
    if (rc != ESP_PEER_ERR_NONE) {
        LOGE_DIAG("esp_peer_new_connection failed: %d", rc);
        return rc;
    }
    LOGI_DIAG("kicked peer: new_connection");
    return 0;
}

static int sig_on_msg(esp_peer_signaling_msg_t *msg, void *ctx) {
    (void)ctx;
    if (msg->type == ESP_PEER_SIGNALING_MSG_BYE) {
        LOGI_DIAG("sig → BYE");
        if (s_peer.pc) {
            esp_peer_disconnect(s_peer.pc);
        }
        return 0;
    }
    if (!s_peer.pc) {
        LOGE_DIAG("sig msg but no peer");
        return -1;
    }
    esp_peer_msg_t pmsg = {
        .type = (esp_peer_msg_type_t)msg->type,
        .data = msg->data,
        .size = msg->size,
    };
    LOGI_DIAG("sig → peer: type=%d size=%d", (int)msg->type, msg->size);
    return esp_peer_send_msg(s_peer.pc, &pmsg);
}

static int sig_on_close(void *ctx) {
    (void)ctx;
    LOGI_DIAG("sig closed");
    return 0;
}

/* ─── worker task ─────────────────────────────────────────────── */

static void check_caps(void) {
    /* Only check once a session is up. The hard cap is anchored at
     * voice_peer_start; the idle cap is anchored at the most recent
     * audio frame (or start, before any audio arrived). Once the
     * cap fires we set stop_requested and let the touch loop
     * actually call voice::stop_session(). */
    if (atomic_load(&s_peer.stop_requested)) return;
    if (!atomic_load(&s_peer.dc_opened)) return;

    int64_t now_us = esp_timer_get_time();
    int64_t age_ms = (now_us - s_peer.start_us) / 1000;
    int64_t silence_ms = (now_us - (int64_t)atomic_load(&s_peer.last_audio_us)) / 1000;

    if (age_ms >= VOICE_HARD_CAP_MS) {
        LOGI_DIAG("hard cap fired at %lld ms (limit %d ms) — requesting stop",
            (long long)age_ms, VOICE_HARD_CAP_MS);
        atomic_store(&s_peer.stop_requested, true);
        return;
    }
    if (silence_ms >= VOICE_IDLE_CAP_MS) {
        LOGI_DIAG("idle cap fired at %lld ms silence (limit %d ms) — requesting stop",
            (long long)silence_ms, VOICE_IDLE_CAP_MS);
        atomic_store(&s_peer.stop_requested, true);
        return;
    }
}

static void worker_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "worker_task started");
    while (atomic_load(&s_peer.running)) {
        if (s_peer.pc) {
            esp_peer_main_loop(s_peer.pc);
        }
        check_caps();
        vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_SLEEP_MS));
    }
    ESP_LOGI(TAG, "worker_task exiting");
    s_peer.worker = NULL;
    vTaskDelete(NULL);
}

/* ─── public API ──────────────────────────────────────────────── */

int voice_peer_start(const char *openai_key, const char *instructions) {
    if (atomic_load(&s_peer.running)) {
        ESP_LOGW(TAG, "already running");
        return -1;
    }
    if (!openai_key || !*openai_key) {
        ESP_LOGE(TAG, "no openai key");
        return -1;
    }

    /* Fresh diagnostic log for this session. */
    voice_diag_reset();
    LOGI_DIAG("voice_peer_start (key_len=%u, has_instr=%d)",
        (unsigned)strlen(openai_key),
        instructions && *instructions ? 1 : 0);

    /* Spin up the tool-dispatch worker. Caller is responsible for
     * voice_tools_set_pet_id() before this. Idempotent — safe to
     * call from every session start. */
    voice_tools_init();

    s_peer.key_copy = strdup(openai_key);
    if (!s_peer.key_copy) return -1;

    /* instructions is optional; NULL means OpenAI fills in its
     * default (which we don't want — see M9.e finding). The smoke
     * test path passes a one-line greeting. */
    s_peer.instructions_copy = NULL;
    if (instructions && *instructions) {
        s_peer.instructions_copy = strdup(instructions);
        if (!s_peer.instructions_copy) {
            free(s_peer.key_copy);
            s_peer.key_copy = NULL;
            return -1;
        }
    }

    atomic_store(&s_peer.running, true);
    atomic_store(&s_peer.dc_opened, false);
    atomic_store(&s_peer.first_event_seen, false);
    atomic_store(&s_peer.stop_requested, false);
    set_phase(VOICE_PHASE_CONNECTING);
    s_peer.dc_stream_known = false;
    s_peer.dc_stream_id = 0;
    s_peer.start_us = esp_timer_get_time();
    /* Initialise last_audio_us to start_us so the idle cap can't
     * fire during the connect window (~9 s) before any audio
     * arrives. The 'idle' it measures is post-connection silence. */
    atomic_store(&s_peer.last_audio_us, (long long)s_peer.start_us);
    s_aud_recv_n = s_aud_dec_ok = s_aud_dec_err = 0;
    s_aud_write_err = s_aud_pcm_bytes = 0;

    BaseType_t ok = xTaskCreate(
        worker_task, "voice_peer", WORKER_STACK_BYTES,
        NULL, WORKER_PRIO, &s_peer.worker);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        free(s_peer.key_copy); s_peer.key_copy = NULL;
        free(s_peer.instructions_copy); s_peer.instructions_copy = NULL;
        atomic_store(&s_peer.running, false);
        return -1;
    }

    openai_signaling_cfg_t openai_cfg = {};
    openai_cfg.token = s_peer.key_copy;
    openai_cfg.voice = NULL;  /* defaults to alloy */
    openai_cfg.instructions = s_peer.instructions_copy;

    esp_peer_signaling_cfg_t sig_cfg = {};
    sig_cfg.on_ice_info = sig_on_ice_info;
    sig_cfg.on_connected = sig_on_connected;
    sig_cfg.on_msg = sig_on_msg;
    sig_cfg.on_close = sig_on_close;
    sig_cfg.extra_cfg = &openai_cfg;
    sig_cfg.extra_size = sizeof(openai_cfg);

    const esp_peer_signaling_impl_t *impl =
        esp_signaling_get_openai_signaling();
    int rc = impl->start(&sig_cfg, &s_peer.sig);
    if (rc != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "signaling.start rc=%d", rc);
        atomic_store(&s_peer.running, false);
        free(s_peer.key_copy); s_peer.key_copy = NULL;
        free(s_peer.instructions_copy); s_peer.instructions_copy = NULL;
        return -1;
    }

    return 0;
}

bool voice_peer_data_channel_opened(void) {
    return atomic_load(&s_peer.dc_opened);
}

bool voice_peer_first_event_seen(void) {
    return atomic_load(&s_peer.first_event_seen);
}

voice_phase_t voice_peer_phase(void) {
    return (voice_phase_t)atomic_load(&s_peer.phase);
}

bool voice_peer_mic_should_mute(void) {
    /* AEC active → mute steps aside. The whole point of M9.f.3 is
     * to get barge-in back, and the half-duplex mute as tuned was
     * over-muting (cutting the user off during the speaker drain
     * tail and any DTX-comfort-noise window). When AEC is engaged
     * the canceller is responsible for echo; mute is only the
     * fallback for when voice_aec_init / aec_create has failed. */
    if (voice_aec_is_enabled()) {
        return false;
    }
    if ((voice_phase_t)atomic_load(&s_peer.phase) == VOICE_PHASE_SPEAKING) {
        return true;
    }
    int64_t deadline = (int64_t)atomic_load(&s_peer.tail_mute_until_us);
    return deadline > esp_timer_get_time();
}

bool voice_peer_stop_requested(void) {
    return atomic_load(&s_peer.stop_requested);
}

int voice_peer_send_dc_json(const char *json) {
    if (!json || !*json) return -1;
    if (!s_peer.dc_stream_known) {
        LOGW_DIAG("send_dc_json: no dc stream");
        return -1;
    }
    if (!s_peer.pc) {
        LOGW_DIAG("send_dc_json: no peer");
        return -1;
    }
    esp_peer_data_frame_t f = {
        .type = ESP_PEER_DATA_CHANNEL_STRING,
        .stream_id = s_peer.dc_stream_id,
        .data = (uint8_t *)json,
        .size = (int)strlen(json),
    };
    return esp_peer_send_data(s_peer.pc, &f);
}

int voice_peer_send_audio_frame(const uint8_t *buf, int size) {
    if (!buf || size <= 0) return -1;
    if (!s_peer.pc) return -1;
    /* PTS is informational; esp_peer fills the RTP timestamp from
     * its own clock. Pass 0; encoder also produces a PTS in pts ms
     * but we don't have a use for it on the wire. */
    esp_peer_audio_frame_t f = {
        .pts = 0,
        .data = (uint8_t *)buf,
        .size = size,
    };
    return esp_peer_send_audio(s_peer.pc, &f);
}

bool voice_peer_is_running(void) {
    return atomic_load(&s_peer.running);
}

int voice_peer_send_text(const char *text) {
    if (!text || !*text) return -1;
    if (!s_peer.dc_stream_known) {
        LOGW_DIAG("send_text: no dc stream");
        return -1;
    }
    /* Build:
     *   { type: "conversation.item.create",
     *     item: { type: "message", role: "user",
     *             content: [{ type: "input_text", text: "<text>" }] } }
     * then a separate
     *   { type: "response.create" }
     * Both go on the same dc stream. cJSON keeps the JSON well-formed
     * so OpenAI doesn't reject on shape mismatches. */
    cJSON *item_root = cJSON_CreateObject();
    cJSON_AddStringToObject(item_root, "type", "conversation.item.create");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddItemToObject(item_root, "item", item);
    cJSON_AddStringToObject(item, "type", "message");
    cJSON_AddStringToObject(item, "role", "user");
    cJSON *content = cJSON_CreateArray();
    cJSON_AddItemToObject(item, "content", content);
    cJSON *part = cJSON_CreateObject();
    cJSON_AddItemToArray(content, part);
    cJSON_AddStringToObject(part, "type", "input_text");
    cJSON_AddStringToObject(part, "text", text);
    char *item_json = cJSON_PrintUnformatted(item_root);
    cJSON_Delete(item_root);

    cJSON *resp_root = cJSON_CreateObject();
    cJSON_AddStringToObject(resp_root, "type", "response.create");
    char *resp_json = cJSON_PrintUnformatted(resp_root);
    cJSON_Delete(resp_root);

    int rc = -1;
    if (item_json && resp_json) {
        esp_peer_data_frame_t f1 = {
            .type = ESP_PEER_DATA_CHANNEL_STRING,
            .stream_id = s_peer.dc_stream_id,
            .data = (uint8_t *)item_json,
            .size = (int)strlen(item_json),
        };
        esp_peer_data_frame_t f2 = {
            .type = ESP_PEER_DATA_CHANNEL_STRING,
            .stream_id = s_peer.dc_stream_id,
            .data = (uint8_t *)resp_json,
            .size = (int)strlen(resp_json),
        };
        int r1 = esp_peer_send_data(s_peer.pc, &f1);
        int r2 = esp_peer_send_data(s_peer.pc, &f2);
        rc = (r1 == ESP_PEER_ERR_NONE && r2 == ESP_PEER_ERR_NONE) ? 0 : -1;
        LOGI_DIAG("send_text: %d/%d ('%s')", r1, r2, text);
    }
    free(item_json);
    free(resp_json);
    return rc;
}

void voice_peer_stop(void) {
    if (!atomic_load(&s_peer.running)) {
        return;
    }
    LOGI_DIAG("voice_peer_stop: rx=%lu dec_ok=%lu dec_err=%lu wr_err=%lu pcm=%lu B",
        (unsigned long)s_aud_recv_n,
        (unsigned long)s_aud_dec_ok,
        (unsigned long)s_aud_dec_err,
        (unsigned long)s_aud_write_err,
        (unsigned long)s_aud_pcm_bytes);

    atomic_store(&s_peer.running, false);

    /* Stop mic FIRST. voice_mic's loop checks voice_peer_is_running()
     * (which now reads false), drops out of esp_codec_dev_read on
     * its next iteration, and exits cleanly. Doing this before peer
     * teardown means the mic task never sees s_peer.pc==NULL and
     * the error counters stay clean. */
    voice_mic_stop();

    if (s_peer.sig) {
        const esp_peer_signaling_impl_t *impl =
            esp_signaling_get_openai_signaling();
        impl->stop(s_peer.sig);
        s_peer.sig = NULL;
    }

    if (s_peer.pc) {
        esp_peer_disconnect(s_peer.pc);
        esp_peer_close(s_peer.pc);
        s_peer.pc = NULL;
    }

    /* Drain pending audio + close I²S/Opus AFTER the peer's worker
     * task has actually stopped delivering audio frames. */
    for (int i = 0; i < 20 && s_peer.worker; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    close_audio_playback();

    if (s_peer.key_copy) {
        memset(s_peer.key_copy, 0, strlen(s_peer.key_copy));
        free(s_peer.key_copy);
        s_peer.key_copy = NULL;
    }
    if (s_peer.instructions_copy) {
        free(s_peer.instructions_copy);
        s_peer.instructions_copy = NULL;
    }

    s_peer.dc_stream_known = false;
    set_phase(VOICE_PHASE_IDLE);
    atomic_store(&s_peer.stop_requested, false);

    /* Drop any in-flight tool-call accumulators + tear down the
     * dispatch worker. New session will re-init lazily. */
    pending_release_all();
    voice_tools_shutdown();

    LOGI_DIAG("stopped");

    /* Persist the session log so it survives USB disconnect. The
     * companion call lives in main.cpp's boot sequence:
     * voice_diag_dump_last() prints + clears the file once. */
    voice_diag_flush();
}
