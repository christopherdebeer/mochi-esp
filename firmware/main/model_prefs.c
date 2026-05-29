#include "model_prefs.h"

#include <string.h>
#include <stdio.h>

#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "model_prefs";
static const char *NS = "models";
static const char *K_VOICE = "voice";
static const char *K_TEXT  = "text";
static const char *K_VDEBUG = "vdebug";   /* admin debug-voice toggle (design/27) */

/* Curated lists. Index 0 = default = today's hardcoded behaviour.
 * Voice: speech-to-speech realtime models (translate/whisper variants
 * are not conversational, so they're excluded). Text: chat models that
 * support JSON mode — kept in lockstep with CONSOLIDATE_MODELS in
 * c15r/mochi backend/api.ts (the server re-validates ?model=). */
static const char *const VOICE_MODELS[] = { "gpt-realtime", "gpt-realtime-2" };
static const char *const TEXT_MODELS[]  = {
    "gpt-4o-mini", "gpt-5-mini", "gpt-5.4-mini", "gpt-5.4",
};
#define VOICE_N (sizeof(VOICE_MODELS) / sizeof(VOICE_MODELS[0]))
#define TEXT_N  (sizeof(TEXT_MODELS) / sizeof(TEXT_MODELS[0]))

static void load(const char *key, const char *const *list, size_t n,
                 char *out, size_t cap) {
    snprintf(out, cap, "%s", list[0]);   /* default */
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    char buf[48];
    size_t len = sizeof(buf);
    if (nvs_get_str(h, key, buf, &len) == ESP_OK) {
        /* Honour the stored value only if it's still in the curated
         * list — a removed/renamed id falls back to the default rather
         * than sending a stale model the API would reject. */
        for (size_t i = 0; i < n; i++) {
            if (strcmp(buf, list[i]) == 0) {
                snprintf(out, cap, "%s", buf);
                break;
            }
        }
    }
    nvs_close(h);
}

static void cycle(const char *key, const char *const *list, size_t n) {
    char cur[48];
    load(key, list, n, cur, sizeof(cur));
    size_t idx = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(cur, list[i]) == 0) { idx = i; break; }
    }
    const char *next = list[(idx + 1) % n];
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e1 = nvs_set_str(h, key, next);
    esp_err_t ec = nvs_commit(h);
    nvs_close(h);
    if (e1 == ESP_OK && ec == ESP_OK) ESP_LOGI(TAG, "%s → %s", key, next);
    else ESP_LOGW(TAG, "%s save failed: %d %d", key, e1, ec);
}

void model_prefs_voice(char *out, size_t cap) {
    load(K_VOICE, VOICE_MODELS, VOICE_N, out, cap);
}
void model_prefs_text(char *out, size_t cap) {
    load(K_TEXT, TEXT_MODELS, TEXT_N, out, cap);
}
void model_prefs_cycle_voice(void) { cycle(K_VOICE, VOICE_MODELS, VOICE_N); }
void model_prefs_cycle_text(void)  { cycle(K_TEXT, TEXT_MODELS, TEXT_N); }

bool model_prefs_voice_debug(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t v = 0;
    nvs_get_u8(h, K_VDEBUG, &v);   /* absent → stays 0 (off) */
    nvs_close(h);
    return v != 0;
}

void model_prefs_toggle_voice_debug(void) {
    bool next = !model_prefs_voice_debug();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e1 = nvs_set_u8(h, K_VDEBUG, next ? 1 : 0);
    esp_err_t ec = nvs_commit(h);
    nvs_close(h);
    if (e1 == ESP_OK && ec == ESP_OK) ESP_LOGI(TAG, "voice_debug → %d", next);
    else ESP_LOGW(TAG, "voice_debug save failed: %d %d", e1, ec);
}
