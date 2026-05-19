/**
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2025 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*
 * Vendored from esp-webrtc-solution/components/esp_webrtc/include/
 *   esp_peer_signaling.h, commit 4e5419c1ec3e0750108009b7191536684ac129b5.
 *
 * The `espressif/esp_peer` registry component ships only the
 * peer-connection types (`esp_peer.h`, `esp_peer_types.h`,
 * `esp_peer_default.h`); the signaling interface lives in the
 * higher-level `esp_webrtc` component, which is NOT in the registry
 * and is heavy enough that we don't want to vendor it whole. This
 * header is just type definitions and function-pointer declarations
 * — small, stable, and decoupled from the rest of esp_webrtc.
 *
 * If we later vendor full esp_webrtc (task #76 may not need this if
 * we keep talking to esp_peer directly), delete this file and
 * `#include "esp_peer_signaling.h"` from the vendored component.
 */

#pragma once

#include "esp_peer_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_peer_ice_server_cfg_t server_info;
    bool                      is_initiator;
} esp_peer_signaling_ice_info_t;

typedef void *esp_peer_signaling_handle_t;

typedef enum {
    ESP_PEER_SIGNALING_MSG_NONE,
    ESP_PEER_SIGNALING_MSG_SDP,
    ESP_PEER_SIGNALING_MSG_CANDIDATE,
    ESP_PEER_SIGNALING_MSG_BYE,
    ESP_PEER_SIGNALING_MSG_CUSTOMIZED,
} esp_peer_signaling_msg_type_t;

typedef struct {
    esp_peer_signaling_msg_type_t type;
    uint8_t                      *data;
    int                           size;
} esp_peer_signaling_msg_t;

typedef struct {
    int (*on_ice_info)(esp_peer_signaling_ice_info_t *info, void *ctx);
    int (*on_connected)(void *ctx);
    int (*on_msg)(esp_peer_signaling_msg_t *msg, void *ctx);
    int (*on_close)(void *ctx);
    char *signal_url;
    void *extra_cfg;
    int   extra_size;
    void *ctx;
} esp_peer_signaling_cfg_t;

typedef struct {
    int (*start)(esp_peer_signaling_cfg_t *cfg, esp_peer_signaling_handle_t *sig);
    int (*send_msg)(esp_peer_signaling_handle_t sig, esp_peer_signaling_msg_t *msg);
    int (*stop)(esp_peer_signaling_handle_t sig);
} esp_peer_signaling_impl_t;

#ifdef __cplusplus
}
#endif
