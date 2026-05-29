#include "voice_ui.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Bubble text cap. The thought renderer word-wraps + paginates, but
 * a spoken line on a 200x200 panel only shows a few words legibly;
 * we keep a sentence and let the renderer trail with "...". */
#define VOICE_UI_TEXT_MAX 160

static SemaphoreHandle_t s_mtx;
static char              s_text[VOICE_UI_TEXT_MAX];
static voice_ui_kind_t   s_kind = VOICE_UI_NONE;
static uint32_t          s_seq = 0;        /* bumped on every change */
static uint32_t          s_taken_seq = 0;  /* last seq the consumer saw */

static void ensure_mtx(void) {
    /* First call is from a single task before any concurrency; lazy
     * init avoids an explicit init entrypoint. */
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
}

void voice_ui_post(voice_ui_kind_t kind, const char *text) {
    ensure_mtx();
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_kind = kind;
    snprintf(s_text, sizeof(s_text), "%s", text ? text : "");
    s_seq++;
    xSemaphoreGive(s_mtx);
}

void voice_ui_reset(void) {
    voice_ui_post(VOICE_UI_NONE, "");
}

void voice_ui_clear(void) {
    voice_ui_post(VOICE_UI_NONE, "");
}

bool voice_ui_take(char *out, size_t cap, voice_ui_kind_t *kind) {
    ensure_mtx();
    if (!s_mtx) return false;
    bool changed = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_seq != s_taken_seq) {
        s_taken_seq = s_seq;
        if (out && cap) snprintf(out, cap, "%s", s_text);
        if (kind) *kind = s_kind;
        changed = true;
    }
    xSemaphoreGive(s_mtx);
    return changed;
}
