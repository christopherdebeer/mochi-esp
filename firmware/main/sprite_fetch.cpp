#include "sprite_fetch.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"

static const char *TAG = "sprite_fetch";

/*
 * Per-fetch state for the event-driven body assembly. We pre-allocate
 * the output buffer in the caller, so the only thing this needs to
 * track is the write cursor, the cap, and a "saw too many bytes"
 * flag we use to fail fast on a server bug instead of corrupting
 * memory.
 */
struct fetch_ctx {
    uint8_t *out;
    size_t   cap;        /* expected_len from caller */
    size_t   written;
    bool     overflowed;
};

static esp_err_t on_event(esp_http_client_event_t *evt) {
    auto *ctx = static_cast<fetch_ctx *>(evt->user_data);
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA: {
            if (ctx->overflowed) return ESP_OK;
            size_t want = (size_t)evt->data_len;
            if (ctx->written + want > ctx->cap) {
                ESP_LOGE(TAG, "body overflow: have %u + got %u > cap %u",
                    (unsigned)ctx->written, (unsigned)want,
                    (unsigned)ctx->cap);
                ctx->overflowed = true;
                return ESP_OK;
            }
            memcpy(ctx->out + ctx->written, evt->data, want);
            ctx->written += want;
            return ESP_OK;
        }
        default:
            return ESP_OK;
    }
}

bool sprite_fetch(const char *url, uint8_t *out, size_t expected_len,
                  uint32_t *elapsed_ms) {
    if (!url || !out || expected_len == 0) return false;

    fetch_ctx ctx = { out, expected_len, 0, false };

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = on_event;
    cfg.user_data = &ctx;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;

    int64_t t0 = esp_timer_get_time();

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "init failed");
        return false;
    }

    esp_err_t err = esp_http_client_perform(cli);

    int status = esp_http_client_get_status_code(cli);
    int content_len = esp_http_client_get_content_length(cli);

    esp_http_client_cleanup(cli);

    int64_t t1 = esp_timer_get_time();
    if (elapsed_ms) {
        *elapsed_ms = (uint32_t)((t1 - t0) / 1000);
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "perform failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d", status);
        return false;
    }
    if (ctx.overflowed) {
        ESP_LOGE(TAG, "body exceeded expected length");
        return false;
    }
    if (ctx.written != expected_len) {
        ESP_LOGE(TAG, "got %u bytes, expected %u",
            (unsigned)ctx.written, (unsigned)expected_len);
        return false;
    }
    /*
     * content_len is informational only. Some servers send it with
     * Transfer-Encoding: chunked omitted, others don't; we trust
     * the body length over the header. Log a mismatch though —
     * it's a useful debugging signal if rendering ever looks off.
     */
    if (content_len > 0 && (size_t)content_len != expected_len) {
        ESP_LOGW(TAG, "Content-Length=%d but body=%u (using body)",
            content_len, (unsigned)expected_len);
    }

    ESP_LOGI(TAG, "fetched %u bytes in %u ms",
        (unsigned)expected_len, (unsigned)(*elapsed_ms));
    return true;
}

/*
 * /devsprite/cell/ variant. Fetches into a small staging buffer
 * (header + payload), validates header, then memcpys payload into
 * caller's buffer. We use a stack staging buffer sized for the
 * largest cell we expect — pet 96×96 = 1152 bytes, ui 80×80 = 800,
 * scene cells go through plain sprite_fetch instead. 4 KB is
 * generous headroom and keeps us well under the bumped 8 KB main
 * stack.
 */

struct cell_ctx {
    uint8_t *stage;
    size_t   cap;
    size_t   written;
    bool     overflow;
};

static esp_err_t on_cell_event(esp_http_client_event_t *evt) {
    auto *ctx = static_cast<cell_ctx *>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_DATA && !ctx->overflow) {
        size_t want = (size_t)evt->data_len;
        if (ctx->written + want > ctx->cap) {
            ctx->overflow = true;
            return ESP_OK;
        }
        memcpy(ctx->stage + ctx->written, evt->data, want);
        ctx->written += want;
    }
    return ESP_OK;
}

bool sprite_fetch_cell(const char *url,
                       uint8_t *out_ink, uint8_t *out_mask,
                       size_t plane_cap,
                       uint16_t *out_w, uint16_t *out_h,
                       uint32_t *elapsed_ms) {
    if (!url || !out_ink || !out_mask || !out_w || !out_h) return false;

    /*
     * Stage buffer is PSRAM-allocated lazily on the first call and
     * reused. A previous version put it on the stack, which combined
     * with the ~4-5 KB TLS handshake bignum frames overflowed the
     * 8 KB main task stack on the second cell fetch in a row.
     * PSRAM is 8 MB; one 4 KB long-lived buffer is invisible.
     *
     * sprite_fetch_cell is only ever called from app_main (boot
     * sequence) and the touch handler — both serialized — so a
     * static buffer with no mutex is safe.
     */
    constexpr size_t STAGE_CAP = 4096;
    static uint8_t *stage = nullptr;
    if (!stage) {
        stage = (uint8_t *)heap_caps_malloc(STAGE_CAP, MALLOC_CAP_SPIRAM);
        if (!stage) {
            ESP_LOGE(TAG, "stage PSRAM alloc failed");
            return false;
        }
    }
    cell_ctx ctx = { stage, STAGE_CAP, 0, false };

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = on_cell_event;
    cfg.user_data = &ctx;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    int64_t t0 = esp_timer_get_time();
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) { ESP_LOGE(TAG, "cell init failed"); return false; }
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    int64_t t1 = esp_timer_get_time();
    if (elapsed_ms) *elapsed_ms = (uint32_t)((t1 - t0) / 1000);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cell perform: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "cell HTTP %d", status);
        return false;
    }
    if (ctx.overflow) {
        ESP_LOGE(TAG, "cell exceeded stage buffer (%u)", (unsigned)STAGE_CAP);
        return false;
    }
    if (ctx.written < 8) {
        ESP_LOGE(TAG, "cell too short (%u bytes, need >= 8 for header)",
            (unsigned)ctx.written);
        return false;
    }

    /* Parse header: width u16BE, height u16BE, flags u8, 3 reserved. */
    uint16_t w = (uint16_t)((stage[0] << 8) | stage[1]);
    uint16_t h = (uint16_t)((stage[2] << 8) | stage[3]);
    uint8_t flags = stage[4];
    bool has_mask = (flags & 0x01) != 0;
    if (!has_mask) {
        ESP_LOGE(TAG, "cell response missing mask plane (flags=0x%02x)", flags);
        return false;
    }
    size_t plane_bytes = ((w + 7) / 8) * h;
    size_t payload_len = ctx.written - 8;
    size_t expected = plane_bytes * 2;
    if (payload_len != expected) {
        ESP_LOGE(TAG, "cell payload %u != expected %u for %ux%u (2 planes)",
            (unsigned)payload_len, (unsigned)expected, w, h);
        return false;
    }
    if (plane_bytes > plane_cap) {
        ESP_LOGE(TAG, "cell plane %u > plane_cap %u",
            (unsigned)plane_bytes, (unsigned)plane_cap);
        return false;
    }

    memcpy(out_ink,  stage + 8,                plane_bytes);
    memcpy(out_mask, stage + 8 + plane_bytes,  plane_bytes);
    *out_w = w;
    *out_h = h;
    ESP_LOGI(TAG, "cell %ux%u (ink+mask, %u bytes) in %u ms",
        w, h, (unsigned)payload_len,
        elapsed_ms ? (unsigned)*elapsed_ms : 0u);
    return true;
}

/*
 * HEAD request that captures the ETag response header. Runs the
 * full TLS handshake (so it costs roughly the same as a small GET
 * — ~1 s); the *body* is what we save. Used at boot to decide
 * whether we can re-use locally-cached sprite bytes.
 *
 * We capture the ETag inside the HTTP_EVENT_ON_HEADER callback,
 * not via esp_http_client_get_header() after perform(). The
 * post-perform getter returns nullptr in ESP-IDF v5.3 with HEAD
 * requests on chunked or HTTP/2-fronted backends — caught
 * empirically when devsprite HEAD probes were silently returning
 * empty even on a 200 response.
 */

struct head_ctx {
    char *buf;
    size_t cap;
    bool   captured;
};

static esp_err_t on_head_event(esp_http_client_event_t *evt) {
    auto *ctx = static_cast<head_ctx *>(evt->user_data);
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        evt->header_key && evt->header_value) {
        /* Header keys are typically lowercased by the underlying
         * HTTP stack but be defensive. ETag is the only header
         * we want; ignore everything else. */
        if (strcasecmp(evt->header_key, "ETag") == 0) {
            strncpy(ctx->buf, evt->header_value, ctx->cap - 1);
            ctx->buf[ctx->cap - 1] = 0;
            ctx->captured = true;
        }
    }
    return ESP_OK;
}

bool sprite_fetch_head_etag(const char *url, char *out, size_t out_cap) {
    if (!url || !out || out_cap == 0) return false;
    out[0] = 0;
    head_ctx ctx = { out, out_cap, false };

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_HEAD;
    cfg.event_handler = on_head_event;
    cfg.user_data = &ctx;
    cfg.timeout_ms = 8000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return false;

    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    bool ok = (err == ESP_OK) && (status == 200) && ctx.captured && (out[0] != 0);
    if (!ok) {
        ESP_LOGW(TAG, "head_etag: failed (err=%s status=%d captured=%d)",
            esp_err_to_name(err), status, ctx.captured ? 1 : 0);
    }
    return ok;
}
