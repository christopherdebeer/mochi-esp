/*
 * Tiny HTTPS-POST helper used by the OpenAI signaling port.
 *
 * Upstream `esp-webrtc-solution`'s openai_demo uses an `https_post`
 * symbol that lives inside its kvs_signaling impl (built around the
 * AWS Signature V4 logic). We're not pulling that whole component
 * in; we just need a function with the same shape so the ported
 * `openai_signaling.c` is a clean drop-in.
 *
 * The signature mirrors the upstream exactly:
 *   - `headers` is a NULL-terminated array of "Name: Value" strings.
 *   - On success the body callback fires once with the full response.
 *   - The data buffer passed to the callback is owned by this helper
 *     and freed when the callback returns; copy out anything you need.
 *
 * Implementation details (TLS, root certs, timeouts) follow the same
 * pattern as `firmware/main/sprite_fetch.cpp`: `esp_http_client` +
 * `esp_crt_bundle_attach` + 10 s perform timeout. No keep-alive
 * across calls; a session-mint plus an SDP exchange is two POSTs
 * and the second isn't on the hot path.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

typedef struct {
    char *data;
    int   size;
} http_resp_t;

typedef void (*http_body_t)(http_resp_t *resp, void *ctx);

int https_post(char *url, char **headers, char *data,
               http_body_t body, void *ctx);

/* GET helper for plain-text responses (e.g. the /api/voice/realtime-
 * instructions endpoint). The body callback fires once with the full
 * response if status is 2xx; on non-2xx (or transport error) it does
 * not fire and the function returns -1. Same caller-owned-buffer
 * convention as https_post.
 *
 * `headers` is a NULL-terminated array of "Name: Value" strings —
 * pass NULL for "no extra headers." */
int https_get(char *url, char **headers,
              http_body_t body, void *ctx);

#ifdef __cplusplus
}
#endif
