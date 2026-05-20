/*
 * OpenAI Realtime signaling — esp_peer_signaling_impl_t implementation.
 *
 * Source: esp-webrtc-solution/solutions/openai_demo/main/openai_signaling.c,
 * commit 4e5419c1ec3e0750108009b7191536684ac129b5.
 *
 * Diverges from upstream because OpenAI's Realtime Beta API was
 * retired (returns 400 beta_api_shape_disabled as of 2026-05). The
 * structural deltas are:
 *   - Mint URL  : /v1/realtime/sessions  →  /v1/realtime/client_secrets
 *   - SDP URL   : /v1/realtime?model=…   →  /v1/realtime/calls
 *   - Mint body : {model, modalities, voice} (top-level)
 *                 → {session: {type:"realtime", model, audio:{output:{voice}}}}
 *   - Token     : extracted via cJSON walk; accepts $.value (GA) and
 *                 $.client_secret.value (transitional).
 *   - Default model: gpt-realtime (was gpt-4o-mini-realtime-preview).
 *
 * Cross-checked against /tmp/mochi-val/shared/realtime-mint.ts and
 * frontend/voice-realtime.ts (the canonical mochi.val.run client).
 *
 * Public Domain (or CC0 licensed, at your option) — same as upstream.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "voice_https.h"
#include "openai_signaling.h"
#include "esp_log.h"
#include <cJSON.h>

#define TAG                   "openai_signaling"

/*
 * GA Realtime endpoints. The upstream openai_demo references the Beta
 * shape (`/v1/realtime?model=…` for SDP, `/v1/realtime/sessions` for
 * mint, `{model, modalities, voice}` request body). OpenAI returned
 *   "The Realtime Beta API is no longer supported. Please use
 *    /v1/realtime for the GA API." (HTTP 400, beta_api_shape_disabled)
 * on 2026-05-19 when we tried that shape.
 *
 * GA replacements (cross-checked against `/tmp/mochi-val/shared/
 * realtime-mint.ts` and `frontend/voice-realtime.ts`):
 *   mint URL : POST /v1/realtime/client_secrets
 *   mint body: { session: { type: "realtime", model: "<id>" } }
 *              (full session config — instructions, tools,
 *              audio.input, audio.output, etc — added in the peer-
 *              glue step. Smoke test only needs the type+model
 *              discriminator.)
 *   mint resp: token at $.value (top-level GA) OR
 *              $.client_secret.value (transitional). Accept both.
 *   SDP URL  : POST /v1/realtime/calls (NOT /v1/realtime?model=…;
 *              that path is beta-only and rejects GA `ek_…` tokens
 *              with "API version mismatch").
 *
 * Default model is `gpt-realtime` rather than `gpt-realtime-2` —
 * the latter is "way better but more expensive" (per project
 * direction). User-facing override path will live in NVS later.
 */
#define OPENAI_REALTIME_MODEL    "gpt-realtime"
#define OPENAI_REALTIME_MINT_URL "https://api.openai.com/v1/realtime/client_secrets"
#define OPENAI_REALTIME_SDP_URL  "https://api.openai.com/v1/realtime/calls"

#define SAFE_FREE(p) if (p) {   \
    free(p);                    \
    p = NULL;                   \
}

typedef struct {
    esp_peer_signaling_cfg_t cfg;
    uint8_t                 *remote_sdp;
    int                      remote_sdp_size;
    char                    *ephemeral_token;
} openai_signaling_t;

/*
 * Extract the ephemeral token from the mint response.
 *
 * GA puts the token at $.value at the top level. Some transitional
 * responses still nest it under $.client_secret.value. Walk the JSON
 * with cJSON instead of the upstream's quoted-substring scan — the
 * substring scan would prefer $.client_secret.value over $.value
 * just by virtue of the key name appearing later in the document,
 * which is fragile if the field order changes.
 */
static void session_answer(http_resp_t *resp, void *ctx)
{
    openai_signaling_t *sig = (openai_signaling_t *)ctx;
    cJSON *root = cJSON_Parse((const char *)resp->data);
    if (!root) {
        ESP_LOGE(TAG, "mint response not JSON");
        return;
    }
    const char *token = NULL;
    cJSON *top_value = cJSON_GetObjectItemCaseSensitive(root, "value");
    if (cJSON_IsString(top_value) && top_value->valuestring) {
        token = top_value->valuestring;
    } else {
        cJSON *cs = cJSON_GetObjectItemCaseSensitive(root, "client_secret");
        if (cJSON_IsObject(cs)) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(cs, "value");
            if (cJSON_IsString(v) && v->valuestring) {
                token = v->valuestring;
            }
        }
    }
    if (token) {
        sig->ephemeral_token = strdup(token);
    } else {
        ESP_LOGE(TAG, "mint response: no .value or .client_secret.value");
    }
    cJSON_Delete(root);
}

/*
 * Mint an ephemeral client secret.
 *
 * GA request shape (per /tmp/mochi-val/shared/realtime-mint.ts):
 *   { "session": { "type": "realtime", "model": "<id>",
 *                  "instructions": "<persona>",
 *                  "output_modalities": ["audio"],
 *                  "audio": { "output": {
 *                      "voice": "<voice>",
 *                      "format": { "type": "audio/pcm", "rate": 24000 }
 *                  } } } }
 *
 * Why instructions ride along with the mint and not session.update:
 * the model starts generating with whatever system prompt was bound
 * at mint time. If we leave it blank and try to session.update post-
 * connect, the first response is already shaped by OpenAI's default
 * ("Your knowledge cutoff is 2023…"). M9.e proved this on hardware.
 *
 * The peer-glue step (M9.f+) adds tools, audio.input transcription /
 * turn_detection, and the OpenAI-Safety-Identifier header derived
 * from the pet_id.
 */
static void get_ephemeral_token(openai_signaling_t *sig, char *token, char *voice, char *instructions, char *tools_json)
{
    char content_type[32] = "Content-Type: application/json";
    int len = strlen("Authorization: Bearer ") + strlen(token) + 1;
    char auth[len];
    snprintf(auth, len, "Authorization: Bearer %s", token);
    char *header[] = {
        content_type,
        auth,
        NULL,
    };
    cJSON *root = cJSON_CreateObject();
    cJSON *session = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "session", session);
    cJSON_AddStringToObject(session, "type", "realtime");
    cJSON_AddStringToObject(session, "model", OPENAI_REALTIME_MODEL);
    if (instructions && *instructions) {
        cJSON_AddStringToObject(session, "instructions", instructions);
    }
    cJSON *output_modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(output_modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(session, "output_modalities", output_modalities);

    cJSON *audio = cJSON_CreateObject();
    cJSON_AddItemToObject(session, "audio", audio);

    /* audio.output: voice + PCM16/24kHz format hint. */
    cJSON *audio_out = cJSON_CreateObject();
    cJSON_AddItemToObject(audio, "output", audio_out);
    cJSON_AddStringToObject(audio_out, "voice", voice);
    cJSON *out_fmt = cJSON_CreateObject();
    cJSON_AddItemToObject(audio_out, "format", out_fmt);
    cJSON_AddStringToObject(out_fmt, "type", "audio/pcm");
    cJSON_AddNumberToObject(out_fmt, "rate", 24000);

    /* audio.input: server-side STT + VAD so the model can hear the
     * user. Without this, OpenAI accepts our audio but doesn't emit
     * transcription events on the data channel and won't auto-commit
     * input audio buffers — the model would never see input. Same
     * shape as mochi-val's realtime-mint.ts.
     *
     *   transcription.model = gpt-4o-mini-transcribe (cheapest viable)
     *   turn_detection.type = semantic_vad, eagerness = "low"
     *     "low" pairs with the absent-AEC posture: less eager VAD
     *     means mochi's own audio leaking into the mic is less
     *     likely to trigger a spurious user turn that the model
     *     interrupts itself for. */
    cJSON *audio_in = cJSON_CreateObject();
    cJSON_AddItemToObject(audio, "input", audio_in);
    cJSON *in_fmt = cJSON_CreateObject();
    cJSON_AddItemToObject(audio_in, "format", in_fmt);
    cJSON_AddStringToObject(in_fmt, "type", "audio/pcm");
    cJSON_AddNumberToObject(in_fmt, "rate", 24000);
    cJSON *transcription = cJSON_CreateObject();
    cJSON_AddItemToObject(audio_in, "transcription", transcription);
    cJSON_AddStringToObject(transcription, "model", "gpt-4o-mini-transcribe");
    cJSON *turn_detect = cJSON_CreateObject();
    cJSON_AddItemToObject(audio_in, "turn_detection", turn_detect);
    cJSON_AddStringToObject(turn_detect, "type", "semantic_vad");
    cJSON_AddStringToObject(turn_detect, "eagerness", "low");

    /* Tool specs — fetched per-pet from /api/voice/tools and parsed
     * here as a pre-formed JSON array. We attach the array directly
     * (cJSON_AddItemToObject takes ownership) and emit
     * tool_choice:"auto" so the model can call any of them. The
     * canonical list lives at shared/voice-tools-spec.ts; the device
     * never needs to know what's in it, only how to forward calls
     * via /api/voice/tool (handled in voice_tools.c). */
    if (tools_json && *tools_json) {
        cJSON *tools = cJSON_Parse(tools_json);
        if (tools && cJSON_IsArray(tools)) {
            cJSON_AddItemToObject(session, "tools", tools);
            cJSON_AddStringToObject(session, "tool_choice", "auto");
        } else if (tools) {
            cJSON_Delete(tools);
        }
    }

    char *json_string = cJSON_Print(root);
    if (json_string) {
        https_post(OPENAI_REALTIME_MINT_URL, header, json_string, session_answer, sig);
        free(json_string);
    }
    cJSON_Delete(root);
}

static int openai_signaling_start(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *h)
{
    openai_signaling_t *sig = (openai_signaling_t *)calloc(1, sizeof(openai_signaling_t));
    if (sig == NULL) {
        return ESP_PEER_ERR_NO_MEM;
    }
    openai_signaling_cfg_t *openai_cfg = (openai_signaling_cfg_t *)cfg->extra_cfg;
    sig->cfg = *cfg;
    /* GA voices: alloy, ash, ballad, cedar, coral, echo, marin, sage,
     * shimmer, verse. See voice list in mochi-val realtime-mint.ts.
     * Default is "marin" — matches REALTIME_DEFAULT_VOICE in
     * mochi-val so the device-side voice matches the browser-side. */
    get_ephemeral_token(
        sig,
        openai_cfg->token,
        openai_cfg->voice ? openai_cfg->voice : "marin",
        openai_cfg->instructions,
        openai_cfg->tools_json);
    if (sig->ephemeral_token == NULL) {
        ESP_LOGE(TAG, "ephemeral token not minted");
        free(sig);
        return ESP_PEER_ERR_NOT_SUPPORT;
    }
    *h = sig;
    esp_peer_signaling_ice_info_t ice_info = {
        .is_initiator = true,
    };
    if (sig->cfg.on_ice_info) {
        sig->cfg.on_ice_info(&ice_info, sig->cfg.ctx);
    }
    if (sig->cfg.on_connected) {
        sig->cfg.on_connected(sig->cfg.ctx);
    }
    return ESP_PEER_ERR_NONE;
}

static void openai_sdp_answer(http_resp_t *resp, void *ctx)
{
    openai_signaling_t *sig = (openai_signaling_t *)ctx;
    SAFE_FREE(sig->remote_sdp);
    sig->remote_sdp = (uint8_t *)malloc(resp->size);
    if (sig->remote_sdp == NULL) {
        ESP_LOGE(TAG, "no memory for remote sdp (%d bytes)", resp->size);
        return;
    }
    memcpy(sig->remote_sdp, resp->data, resp->size);
    sig->remote_sdp_size = resp->size;
}

static int openai_signaling_send_msg(esp_peer_signaling_handle_t h, esp_peer_signaling_msg_t *msg)
{
    openai_signaling_t *sig = (openai_signaling_t *)h;
    if (msg->type == ESP_PEER_SIGNALING_MSG_BYE) {
        /* nothing to do — server tears down on TCP close */
    } else if (msg->type == ESP_PEER_SIGNALING_MSG_SDP) {
        char content_type[32] = "Content-Type: application/sdp";
        char *token = sig->ephemeral_token;
        int len = strlen("Authorization: Bearer ") + strlen(token) + 1;
        char auth[len];
        snprintf(auth, len, "Authorization: Bearer %s", token);
        char *header[] = {
            content_type,
            auth,
            NULL,
        };
        int ret = https_post(OPENAI_REALTIME_SDP_URL, header, (char *)msg->data, openai_sdp_answer, h);
        if (ret != 0 || sig->remote_sdp == NULL) {
            ESP_LOGE(TAG, "Fail to post data to %s", OPENAI_REALTIME_SDP_URL);
            return -1;
        }
        esp_peer_signaling_msg_t sdp_msg = {
            .type = ESP_PEER_SIGNALING_MSG_SDP,
            .data = sig->remote_sdp,
            .size = sig->remote_sdp_size,
        };
        if (sig->cfg.on_msg) {
            sig->cfg.on_msg(&sdp_msg, sig->cfg.ctx);
        }
    }
    return 0;
}

static int openai_signaling_stop(esp_peer_signaling_handle_t h)
{
    openai_signaling_t *sig = (openai_signaling_t *)h;
    if (sig->cfg.on_close) {
        sig->cfg.on_close(sig->cfg.ctx);
    }
    SAFE_FREE(sig->remote_sdp);
    SAFE_FREE(sig->ephemeral_token);
    SAFE_FREE(sig);
    return 0;
}

const esp_peer_signaling_impl_t *esp_signaling_get_openai_signaling(void)
{
    static const esp_peer_signaling_impl_t impl = {
        .start = openai_signaling_start,
        .send_msg = openai_signaling_send_msg,
        .stop = openai_signaling_stop,
    };
    return &impl;
}
