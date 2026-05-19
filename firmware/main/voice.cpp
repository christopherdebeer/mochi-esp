#include "voice.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "i2c_bus.h"
#include "openai_key.h"
#include "pair_creds.h"
extern "C" {
#include "codec_init.h"
#include "voice/openai_signaling.h"
#include "voice/voice_peer.h"
#include "voice/voice_https.h"
#include "voice/voice_tools.h"
#include "esp_peer_signaling.h"
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace voice {

static const char *TAG = "voice";

/* Lifetime of voice::init's outputs. They're refreshed on every
 * start_session() call so we don't have to keep the BYO key in RAM
 * between sessions. */
static bool s_inited = false;
static bool s_have_key = false;
static bool s_have_pair = false;

/* Active-session bookkeeping. Set by start_session, cleared by
 * stop_session. Used by main.cpp's touch loop to drive the
 * tap-to-start / tap-to-stop UX. */
static bool s_active = false;

/* Fallback when val.run /voice/realtime-instructions can't be
 * reached. Better than letting OpenAI's default "you are an AI…"
 * preamble take over (which we'd get if we passed empty
 * instructions). The val.run-fetched persona is much richer; this
 * is the "device just lost the network" recovery path. */
static constexpr const char *FALLBACK_INSTRUCTIONS =
    "Speak only in English. "
    "You are a friendly e-paper kitten. The user just summoned you. "
    "Greet them warmly with a single short sentence in your own voice. "
    "Do not ask a question; just say hello and stop.";

/* Body of /voice/realtime-instructions. Caller-provided buffer; we
 * cap the copy to keep things bounded. */
struct fetch_ctx {
    char  *out;
    size_t cap;
    bool   got;
};

extern "C" void persona_body(http_resp_t *resp, void *ctx) {
    auto *fc = static_cast<fetch_ctx *>(ctx);
    if (!fc || !fc->out || fc->cap == 0) return;
    size_t n = (size_t)resp->size;
    if (n >= fc->cap) n = fc->cap - 1;
    memcpy(fc->out, resp->data, n);
    fc->out[n] = 0;
    fc->got = true;
}

/* Fetch persona from mochi.val.run. Returns true if the buffer
 * contains a valid persona string; false on any error. The caller
 * is responsible for its own fallback when this returns false. */
static bool fetch_persona(const char *pet_id, char *out, size_t cap) {
    if (!pet_id || !*pet_id || !out || cap < 64) return false;

    char header[80];
    snprintf(header, sizeof(header), "X-Pet-Id: %s", pet_id);
    char *headers[] = { header, nullptr };

    fetch_ctx fc = { out, cap, false };
    char url[] = "https://mochi.val.run/api/voice/realtime-instructions";
    int rc = https_get(url, headers, persona_body, &fc);
    if (rc != 0 || !fc.got) {
        ESP_LOGW(TAG, "persona fetch failed (rc=%d, got=%d)", rc, fc.got);
        return false;
    }
    ESP_LOGI(TAG, "persona fetched: %u bytes", (unsigned)strlen(out));
    return true;
}

bool init(void) {
    if (s_inited) return true;

    if (!i2c_bus::handle()) {
        ESP_LOGE(TAG, "i2c bus not ready; aborting codec init");
        return false;
    }

    /* The Waveshare V2 has a single ES8311 doing both capture and
     * playback over the same I²S port (in_out: in board_cfg.txt).
     * use_tdm=false because ES8311 is plain stereo, not TDM.
     * reuse_dev=true because the same codec handles both directions. */
    codec_init_cfg_t cfg = {};
    cfg.in_mode = CODEC_I2S_MODE_STD;
    cfg.in_use_tdm = false;
    cfg.reuse_dev = true;

    int rc = init_codec(&cfg);
    if (rc != 0) {
        ESP_LOGE(TAG, "init_codec failed: %d", rc);
        return false;
    }
    ESP_LOGI(TAG, "init_codec ok");

    /* Probe NVS for the BYO key + pet_id. We don't keep them in RAM
     * between sessions; start_session reloads on demand. The boot-time
     * probe is just to log whether voice is reachable. */
    char openai_key[MOCHI_OPENAI_KEY_MAX + 1] = {};
    s_have_key = openai_key_load(openai_key, sizeof(openai_key));
    memset(openai_key, 0, sizeof(openai_key));
    if (!s_have_key) {
        ESP_LOGW(TAG, "no openai key in NVS — voice path disabled");
    }

    struct mochi_pair_creds pair = {};
    s_have_pair = pair_creds_load(&pair);
    if (!s_have_pair) {
        ESP_LOGW(TAG, "no pet_id in NVS — voice tool routing will fail");
    } else {
        ESP_LOGI(TAG, "pet_id available: %s", pair.pet_id);
    }

    s_inited = true;
    return true;
}

bool is_ready(void) {
    return s_inited && s_have_key;
}

bool is_active(void) {
    return s_active;
}

int start_session(void) {
    if (!s_inited) {
        ESP_LOGE(TAG, "voice::init() not called");
        return -1;
    }
    if (s_active) {
        ESP_LOGW(TAG, "session already active");
        return -1;
    }
    if (!s_have_key) {
        ESP_LOGE(TAG, "no openai key — refusing to start");
        return -1;
    }

    /*
     * Heap-allocate the BYO key + persona buffers. Both are short-
     * lived (just for the duration of voice_peer_start), but together
     * they're ~4.3 KB and the main task stack is 8 KB total — the
     * earlier on-stack version blew past the limit during the TLS
     * handshake's own frames. PSRAM is fine for both; the cleartext
     * is wiped before free either way.
     *
     * Failure-cleanup pattern: jump to `cleanup` so wipes + frees
     * happen on every exit path.
     */
    constexpr size_t PERSONA_BUF_BYTES = 4096;
    char *openai_key = (char *)heap_caps_calloc(
        1, MOCHI_OPENAI_KEY_MAX + 1, MALLOC_CAP_SPIRAM);
    char *persona_buf = (char *)heap_caps_calloc(
        1, PERSONA_BUF_BYTES, MALLOC_CAP_SPIRAM);
    int rc = -1;
    if (!openai_key || !persona_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed for session buffers");
        goto cleanup;
    }

    if (!openai_key_load(openai_key, MOCHI_OPENAI_KEY_MAX + 1)) {
        ESP_LOGE(TAG, "key vanished from NVS between init and start");
        goto cleanup;
    }

    /* Persona: fetch from val.run if we have a pet_id, fall back to
     * the smoke string otherwise. The fetch costs one HTTPS round-
     * trip (~1 s typical) before the mint round-trip; both happen
     * before the touch loop sees the session as active, so the user
     * just sees the long-press → curious render → connecting time
     * grow by the persona-fetch cost. Fine for v1.
     *
     * pet_id also gets handed to the tool-dispatch module so its
     * worker knows which X-Pet-Id to send when posting tool calls
     * to /api/voice/tool. */
    {
        const char *instructions = FALLBACK_INSTRUCTIONS;
        if (s_have_pair) {
            struct mochi_pair_creds pair = {};
            if (pair_creds_load(&pair)) {
                voice_tools_set_pet_id(pair.pet_id);
                if (fetch_persona(pair.pet_id, persona_buf, PERSONA_BUF_BYTES)) {
                    instructions = persona_buf;
                }
            }
        }

        ESP_LOGI(TAG, "starting voice session (instructions: %s)…",
            instructions == persona_buf ? "fetched" : "fallback");
        rc = voice_peer_start(openai_key, instructions);
        if (rc != 0) {
            ESP_LOGE(TAG, "voice_peer_start rc=%d", rc);
            goto cleanup;
        }
        s_active = true;
    }

cleanup:
    /* voice_peer_start strdup'd what it needs; wipe our copies. */
    if (openai_key) {
        memset(openai_key, 0, MOCHI_OPENAI_KEY_MAX + 1);
        free(openai_key);
    }
    if (persona_buf) {
        memset(persona_buf, 0, PERSONA_BUF_BYTES);
        free(persona_buf);
    }
    return rc;
}

void stop_session(void) {
    if (!s_active) return;
    ESP_LOGI(TAG, "stopping voice session…");
    voice_peer_stop();
    s_active = false;
    ESP_LOGI(TAG, "voice session stopped");
}

Phase phase(void) {
    /* Map peer's enum to ours. Both have the same values today; the
     * cast keeps them decoupled in case voice_peer ever grows extra
     * intermediate states. */
    switch (voice_peer_phase()) {
        case VOICE_PHASE_CONNECTING: return Phase::Connecting;
        case VOICE_PHASE_READY:      return Phase::Ready;
        case VOICE_PHASE_SPEAKING:   return Phase::Speaking;
        case VOICE_PHASE_IDLE:
        default:                     return Phase::Idle;
    }
}

bool send_text(const char *text) {
    if (!s_active) return false;
    return voice_peer_send_text(text) == 0;
}

bool stop_requested(void) {
    return s_active && voice_peer_stop_requested();
}

}  /* namespace voice */
