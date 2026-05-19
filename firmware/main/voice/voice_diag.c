#include "voice_diag.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "voice_diag";

/* 32 KB in PSRAM. Big enough to capture 4–5 minutes of multi-turn
 * voice traffic at the cadence we log (one entry per dc event +
 * periodic mic/audio counter snapshots). PSRAM has 7+ MB free; cost
 * of going from 4 → 32 KB is negligible. */
#define DIAG_BUF_BYTES   (32 * 1024)
#define DIAG_LOG_PATH    "/lfs/voice_last.log"

/* PSRAM-backed buffer; only allocated on first reset. The mutex
 * protects writes from the audio task (pc_on_audio_data) and the
 * UI task (voice::start_session etc.) from interleaving. */
static char           *s_buf;
static int             s_len;
static bool            s_truncated;
static int64_t         s_t0_us;
static SemaphoreHandle_t s_mtx;

static void ensure_buf(void) {
    if (s_buf) return;
    s_buf = (char *)heap_caps_malloc(DIAG_BUF_BYTES, MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        /* PSRAM exhausted — fall back to internal RAM with a smaller
         * cap. The caller's logs degrade gracefully; the path won't
         * fail outright. */
        ESP_LOGW(TAG, "PSRAM alloc failed; using internal RAM");
        s_buf = (char *)malloc(1024);
        if (!s_buf) {
            return;
        }
        s_buf[0] = 0;
    } else {
        s_buf[0] = 0;
    }
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
    }
}

void voice_diag_reset(void) {
    ensure_buf();
    if (!s_buf) return;
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_buf[0] = 0;
    s_len = 0;
    s_truncated = false;
    s_t0_us = esp_timer_get_time();
    if (s_mtx) xSemaphoreGive(s_mtx);
}

void voice_diag_logv(const char *fmt, va_list args) {
    if (!s_buf || !fmt) return;
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);

    int cap = DIAG_BUF_BYTES;
    if (s_truncated || s_len >= cap - 64) {
        if (!s_truncated) {
            const char *m = "[truncated]\n";
            int n = (int)strlen(m);
            if (s_len + n < cap) {
                memcpy(s_buf + s_len, m, n);
                s_len += n;
                s_buf[s_len] = 0;
            }
            s_truncated = true;
        }
        if (s_mtx) xSemaphoreGive(s_mtx);
        return;
    }

    /* Timestamp prefix in ms since reset. Mirrors ESP_LOGI shape. */
    int64_t elapsed_ms = (esp_timer_get_time() - s_t0_us) / 1000;
    int wrote = snprintf(s_buf + s_len, cap - s_len,
        "[%6lld] ", (long long)elapsed_ms);
    if (wrote < 0 || wrote >= cap - s_len) {
        s_truncated = true;
        if (s_mtx) xSemaphoreGive(s_mtx);
        return;
    }
    s_len += wrote;

    int wrote2 = vsnprintf(s_buf + s_len, cap - s_len, fmt, args);
    if (wrote2 < 0) {
        if (s_mtx) xSemaphoreGive(s_mtx);
        return;
    }
    s_len += wrote2;
    if (s_len >= cap - 1) {
        s_len = cap - 1;
        s_truncated = true;
    }
    /* Always end with a newline so the reader sees discrete lines. */
    if (s_len > 0 && s_buf[s_len - 1] != '\n') {
        if (s_len < cap - 1) {
            s_buf[s_len++] = '\n';
        }
    }
    s_buf[s_len] = 0;
    if (s_mtx) xSemaphoreGive(s_mtx);
}

void voice_diag_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    voice_diag_logv(fmt, args);
    va_end(args);
}

int voice_diag_flush(void) {
    if (!s_buf || s_len == 0) {
        return 0;
    }
    if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY);

    FILE *f = fopen(DIAG_LOG_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "fopen(%s) failed", DIAG_LOG_PATH);
        if (s_mtx) xSemaphoreGive(s_mtx);
        return -1;
    }
    size_t want = (size_t)s_len;
    size_t wrote = fwrite(s_buf, 1, want, f);
    fclose(f);
    int rc = (wrote == want) ? 0 : -1;
    if (rc != 0) {
        ESP_LOGE(TAG, "short write %u/%u", (unsigned)wrote, (unsigned)want);
    } else {
        ESP_LOGI(TAG, "flushed %u bytes to %s", (unsigned)want, DIAG_LOG_PATH);
    }
    if (s_mtx) xSemaphoreGive(s_mtx);
    return rc;
}

void voice_diag_dump_last(void) {
    struct stat st;
    if (stat(DIAG_LOG_PATH, &st) != 0) {
        return;  /* nothing from a previous session */
    }
    FILE *f = fopen(DIAG_LOG_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "stat ok but fopen failed for %s", DIAG_LOG_PATH);
        return;
    }
    /* Cap the read at the buffer size — long sessions still cap at
     * 4 KB on the write side, so the file shouldn't be larger. */
    size_t cap = (size_t)st.st_size;
    if (cap > DIAG_BUF_BYTES) cap = DIAG_BUF_BYTES;
    char *body = (char *)malloc(cap + 1);
    if (!body) {
        fclose(f);
        return;
    }
    size_t got = fread(body, 1, cap, f);
    fclose(f);
    body[got] = 0;

    ESP_LOGI(TAG, "─── last voice session (%u bytes) ───", (unsigned)got);
    /* Print line-by-line so the timestamp prefixes line up nicely. */
    char *line = body;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        ESP_LOGI(TAG, "%s", line);
        if (!nl) break;
        line = nl + 1;
    }
    ESP_LOGI(TAG, "─── end last voice session ───");
    free(body);

    /* Delete so we don't keep dumping the same log on every boot if
     * the user doesn't run a new voice session. */
    if (unlink(DIAG_LOG_PATH) != 0) {
        ESP_LOGW(TAG, "unlink(%s) failed", DIAG_LOG_PATH);
    }
}
