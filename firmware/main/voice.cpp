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
#include "cJSON.h"
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

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
    if (!resp || !resp->data || resp->size <= 0) return;
    size_t n = (size_t)resp->size;
    if (n >= fc->cap) n = fc->cap - 1;
    memcpy(fc->out, resp->data, n);
    fc->out[n] = 0;
    fc->got = true;
}

/* Session-config buffer sizes. Persona ~2-3 KB; tools array ~7-8 KB
 * (12 specs × ~600 B) with headroom for per-pet place/costume enums. */
static constexpr size_t PERSONA_BUF_BYTES = 4096;
static constexpr size_t TOOLS_BUF_BYTES   = 16384;

/* Combined persona+tools cache (PSRAM). Populated by prefetch_config()
 * on the connectivity worker after WiFi is up, reused by start_session
 * so the session-start critical path skips the fetch entirely. Both
 * tasks touch it, so it's mutex-guarded. Slightly-stale persona/tools
 * (a place added since prefetch) is fine — refreshed each prefetch. */
static SemaphoreHandle_t s_cfg_mtx   = nullptr;
static char             *s_cfg_instr = nullptr;
static char             *s_cfg_tools = nullptr;
/* True while prefetch_config is mid-fetch. start_session checks this
 * and waits briefly for the in-flight prefetch to land instead of
 * kicking off its OWN /api/voice/session GET. Without this, a user
 * tapping BOOT while prefetch was still running gave us two
 * concurrent TLS sockets to mochi.val.run and one to api.openai.com,
 * exhausting internal heap (MBEDTLS_ERR_SSL_ALLOC_FAILED). */
static volatile bool     s_prefetch_in_flight = false;

static char *voice_psram_strdup(const char *s) {
    if (!s) return nullptr;
    size_t n = strlen(s) + 1;
    char *p = (char *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) memcpy(p, s, n);
    return p;
}

static void cfg_store(const char *instr, const char *tools) {
    if (!s_cfg_mtx) return;
    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
    free(s_cfg_instr);
    free(s_cfg_tools);
    s_cfg_instr = (instr && *instr) ? voice_psram_strdup(instr) : nullptr;
    s_cfg_tools = (tools && *tools) ? voice_psram_strdup(tools) : nullptr;
    xSemaphoreGive(s_cfg_mtx);
}

/* Fetch the combined {instructions, tools} bootstrap from
 * mochi.val.run in ONE round-trip (vs the old two split fetches —
 * design/23). instr_out gets the persona text; tools_out gets the
 * tools JSON array re-serialised (empty if absent). Returns true when
 * at least the instructions parsed. */
static bool fetch_session_config(const char *pet_id,
                                 char *instr_out, size_t instr_cap,
                                 char *tools_out, size_t tools_cap) {
    if (!pet_id || !*pet_id || !instr_out || instr_cap < 64) return false;
    if (tools_out && tools_cap) tools_out[0] = 0;

    char header[80];
    snprintf(header, sizeof(header), "X-Pet-Id: %s", pet_id);
    char *headers[] = { header, nullptr };

    /* Capture the raw JSON ({instructions, tools}) into a temp buffer:
     * persona + tools together run ~12 KB, so give headroom. */
    const size_t RAW_CAP = 24576;
    char *raw = (char *)heap_caps_calloc(1, RAW_CAP, MALLOC_CAP_SPIRAM);
    if (!raw) return false;

    fetch_ctx fc = { raw, RAW_CAP, false };
    char url[] = "https://mochi.val.run/api/voice/session";
    int rc = https_get(url, headers, persona_body, &fc);

    bool ok = false;
    if (rc == 0 && fc.got) {
        cJSON *root = cJSON_Parse(raw);
        if (root) {
            cJSON *ji = cJSON_GetObjectItemCaseSensitive(root, "instructions");
            cJSON *jt = cJSON_GetObjectItemCaseSensitive(root, "tools");
            if (cJSON_IsString(ji) && ji->valuestring) {
                snprintf(instr_out, instr_cap, "%s", ji->valuestring);
                ok = true;
            }
            if (tools_out && tools_cap && jt && cJSON_IsArray(jt)) {
                char *ts = cJSON_PrintUnformatted(jt);
                if (ts) { snprintf(tools_out, tools_cap, "%s", ts); cJSON_free(ts); }
            }
            cJSON_Delete(root);
        } else {
            ESP_LOGW(TAG, "session config parse failed");
        }
    } else {
        ESP_LOGW(TAG, "session config fetch failed (rc=%d got=%d)", rc, fc.got);
    }
    free(raw);
    if (ok) {
        ESP_LOGI(TAG, "session config: instr=%uB tools=%uB",
            (unsigned)strlen(instr_out),
            (unsigned)(tools_out ? strlen(tools_out) : 0));
    }
    return ok;
}

bool init(void) {
    if (s_inited) return true;

    if (!s_cfg_mtx) s_cfg_mtx = xSemaphoreCreateMutex();

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
    char *openai_key = (char *)heap_caps_calloc(
        1, MOCHI_OPENAI_KEY_MAX + 1, MALLOC_CAP_SPIRAM);
    char *persona_buf = (char *)heap_caps_calloc(
        1, PERSONA_BUF_BYTES, MALLOC_CAP_SPIRAM);
    char *tools_buf = (char *)heap_caps_calloc(
        1, TOOLS_BUF_BYTES, MALLOC_CAP_SPIRAM);
    int rc = -1;
    if (!openai_key || !persona_buf || !tools_buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed for session buffers");
        goto cleanup;
    }

    if (!openai_key_load(openai_key, MOCHI_OPENAI_KEY_MAX + 1)) {
        ESP_LOGE(TAG, "key vanished from NVS between init and start");
        goto cleanup;
    }

    /* Persona + tools: prefer the prefetched cache (filled by
     * prefetch_config on the connectivity worker), so session start
     * pays no fetch. On a cache miss, one combined round-trip to
     * /api/voice/session (design/23). Fall back to the smoke string +
     * no tools if even that fails. pet_id is also handed to the
     * tool-dispatch worker so it knows which X-Pet-Id to post. */
    {
        const char *instructions = FALLBACK_INSTRUCTIONS;
        const char *tools_json = nullptr;
        const char *src = "fallback";
        if (s_have_pair) {
            struct mochi_pair_creds pair = {};
            if (pair_creds_load(&pair)) {
                voice_tools_set_pet_id(pair.pet_id);
                bool filled = false;
                if (s_cfg_mtx) {
                    xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
                    if (s_cfg_instr) {
                        snprintf(persona_buf, PERSONA_BUF_BYTES, "%s", s_cfg_instr);
                        if (s_cfg_tools) {
                            snprintf(tools_buf, TOOLS_BUF_BYTES, "%s", s_cfg_tools);
                        }
                        filled = true;
                        src = "cache";
                    }
                    xSemaphoreGive(s_cfg_mtx);
                }
                /* If a prefetch is already in flight, wait for it
                 * to finish (up to ~3 s) instead of opening a second
                 * TLS connection to the same endpoint. The two-
                 * concurrent-fetch case raced with the OpenAI
                 * signaling POST and exhausted internal heap. */
                if (!filled && s_prefetch_in_flight) {
                    ESP_LOGI(TAG, "session config: prefetch in flight, waiting");
                    int waited = 0;
                    while (s_prefetch_in_flight && waited < 3000) {
                        vTaskDelay(pdMS_TO_TICKS(50));
                        waited += 50;
                    }
                    if (s_cfg_mtx) {
                        xSemaphoreTake(s_cfg_mtx, portMAX_DELAY);
                        if (s_cfg_instr) {
                            snprintf(persona_buf, PERSONA_BUF_BYTES, "%s", s_cfg_instr);
                            if (s_cfg_tools) {
                                snprintf(tools_buf, TOOLS_BUF_BYTES, "%s", s_cfg_tools);
                            }
                            filled = true;
                            src = "cache (post-wait)";
                        }
                        xSemaphoreGive(s_cfg_mtx);
                    }
                }
                if (!filled) {
                    filled = fetch_session_config(pair.pet_id,
                        persona_buf, PERSONA_BUF_BYTES,
                        tools_buf, TOOLS_BUF_BYTES);
                    if (filled) src = "fetch";
                }
                if (filled && persona_buf[0]) instructions = persona_buf;
                if (filled && tools_buf[0])   tools_json = tools_buf;
            }
        }

        const size_t heap_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t heap_int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGI(TAG, "starting voice session (config: %s, tools: %s) "
                      "heap_int=%uB largest=%uB",
            src, tools_json ? "yes" : "none",
            (unsigned)heap_int, (unsigned)heap_int_largest);
        rc = voice_peer_start(openai_key, instructions, tools_json);
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
    if (tools_buf) {
        memset(tools_buf, 0, TOOLS_BUF_BYTES);
        free(tools_buf);
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

void prefetch_config(void) {
    if (!s_inited || !s_have_pair) return;
    struct mochi_pair_creds pair = {};
    if (!pair_creds_load(&pair) || !pair.pet_id[0]) return;

    s_prefetch_in_flight = true;
    char *instr = (char *)heap_caps_calloc(1, PERSONA_BUF_BYTES, MALLOC_CAP_SPIRAM);
    char *tools = (char *)heap_caps_calloc(1, TOOLS_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (instr && tools &&
        fetch_session_config(pair.pet_id, instr, PERSONA_BUF_BYTES,
                             tools, TOOLS_BUF_BYTES)) {
        cfg_store(instr, tools);
        ESP_LOGI(TAG, "voice config prefetched");
    }
    free(instr);
    free(tools);
    s_prefetch_in_flight = false;
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
