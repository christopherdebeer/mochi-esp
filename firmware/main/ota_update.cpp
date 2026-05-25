#include "ota_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_partition.h"

#include "cJSON.h"

#include "device_diag.h"

static const char *TAG = "ota";

/* Boot-to-first-check delay. WiFi + sprite + pairing all settle in
 * the first ~10s; we don't want OTA's TLS handshake competing for
 * the radio with the boot-time sprite fetches. */
static constexpr int OTA_BOOT_DELAY_MS = 30 * 1000;

/* Interval between subsequent checks. 24 hours matches the user's
 * "auto-check on boot + daily" choice; tightening this is mostly
 * pointless for a hobbyist device. */
static constexpr int OTA_CHECK_INTERVAL_MS = 24 * 60 * 60 * 1000;

/* Manifest payload cap. A well-formed manifest is <512 bytes — 4 KB
 * gives plenty of headroom and protects against a runaway response. */
static constexpr size_t MANIFEST_MAX_BYTES = 4096;

namespace {

const char *g_manifest_url = nullptr;
volatile bool g_reboot_ready = false;
volatile bool g_task_started = false;
/* Set by ota_update::check_now() (Settings → "update now"); cuts the
 * 24 h inter-check sleep short so the next poll runs immediately. */
volatile bool g_check_now = false;

/* Sleep up to `ms`, but return early if a check-now is requested.
 * Polls in short chunks rather than blocking the whole interval so the
 * "update now" button feels instant. */
void ota_wait(int ms) {
    constexpr int CHUNK_MS = 500;
    int waited = 0;
    while (waited < ms) {
        if (g_check_now) {
            g_check_now = false;
            ESP_LOGI(TAG, "check-now → waking early");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(CHUNK_MS));
        waited += CHUNK_MS;
    }
}

struct manifest_ctx {
    char  *buf;
    size_t cap;
    size_t written;
    bool   overflowed;
};

esp_err_t manifest_event(esp_http_client_event_t *evt) {
    auto *ctx = static_cast<manifest_ctx *>(evt->user_data);
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (ctx->overflowed) return ESP_OK;
    size_t want = (size_t)evt->data_len;
    if (ctx->written + want >= ctx->cap) {
        ESP_LOGW(TAG, "manifest body > %u bytes; truncating", (unsigned)ctx->cap);
        ctx->overflowed = true;
        return ESP_OK;
    }
    memcpy(ctx->buf + ctx->written, evt->data, want);
    ctx->written += want;
    return ESP_OK;
}

/* Fetch the manifest JSON and pull out { version, url }. Returns true
 * iff both fields parsed cleanly. Caller owns out_version and out_url
 * buffers; we copy into them. */
bool fetch_manifest(const char *url,
                    char *out_version, size_t version_cap,
                    char *out_url, size_t url_cap) {
    char *body = (char *)calloc(1, MANIFEST_MAX_BYTES);
    if (!body) {
        ESP_LOGE(TAG, "manifest buf alloc failed");
        return false;
    }

    manifest_ctx ctx = { body, MANIFEST_MAX_BYTES, 0, false };
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.event_handler = manifest_event;
    cfg.user_data = &ctx;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = false;
    /* GitHub serves chunky headers (CSP, cookies) on the redirect leg —
     * default 512 B buffer overflows with "HTTP_CLIENT: Out of buffer"
     * before the Location header even parses. 4 KB rx + 1 KB tx is
     * enough for both legs of github.com → objects.githubusercontent.com. */
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 1024;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        ESP_LOGE(TAG, "manifest http init failed");
        free(body);
        return false;
    }
    esp_err_t err = esp_http_client_perform(cli);
    int status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    if (err != ESP_OK || status != 200 || ctx.overflowed) {
        ESP_LOGW(TAG, "manifest fetch failed: err=%d status=%d written=%u overflow=%d",
            err, status, (unsigned)ctx.written, ctx.overflowed);
        free(body);
        return false;
    }
    body[ctx.written] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        ESP_LOGW(TAG, "manifest parse failed");
        return false;
    }
    cJSON *jv = cJSON_GetObjectItemCaseSensitive(root, "version");
    cJSON *ju = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!cJSON_IsString(jv) || !cJSON_IsString(ju)) {
        ESP_LOGW(TAG, "manifest missing version/url");
        cJSON_Delete(root);
        return false;
    }
    snprintf(out_version, version_cap, "%s", jv->valuestring);
    snprintf(out_url,     url_cap,     "%s", ju->valuestring);
    cJSON_Delete(root);
    return true;
}

/* Parse "MAJOR.MINOR.PATCH" prefix into three ints. Returns true on
 * success. Trailing characters after the third number are ignored
 * (e.g. "0.0.6" → 0,0,6; "0.0.6-2-gabc" → 0,0,6). */
static bool parse_semver(const char *s, int *maj, int *min, int *pat) {
    if (!s) return false;
    if (*s == 'v') s++;
    return sscanf(s, "%d.%d.%d", maj, min, pat) == 3;
}

/* Compare two version strings via semver triplet. Returns:
 *    < 0  if a is older than b
 *    == 0 if equal
 *    > 0  if a is newer than b
 * Falls back to strcmp on either side failing to parse. */
static int compare_versions(const char *a, const char *b) {
    int amaj = 0, amin = 0, apat = 0;
    int bmaj = 0, bmin = 0, bpat = 0;
    bool aok = parse_semver(a, &amaj, &amin, &apat);
    bool bok = parse_semver(b, &bmaj, &bmin, &bpat);
    if (!aok || !bok) return strcmp(a ? a : "", b ? b : "");
    if (amaj != bmaj) return amaj - bmaj;
    if (amin != bmin) return amin - bmin;
    return apat - bpat;
}

/* esp_https_ota client config callback — invoked by the OTA helper
 * for each underlying HTTP request (manifest follow-redirects from
 * github.com → objects.githubusercontent.com). We need the cert
 * bundle on every leg, so attach it here. */
esp_err_t ota_http_init_cb(esp_http_client_handle_t client) {
    /* Most config is set on cfg below; nothing extra needed per-leg. */
    (void)client;
    return ESP_OK;
}

/* Stream the .bin from `bin_url` into the inactive OTA slot. Returns
 * true on success — on success the inactive slot has been activated
 * (otadata flipped) and a reboot will boot the new image. */
bool perform_update(const char *bin_url) {
    ESP_LOGI(TAG, "downloading firmware from %s", bin_url);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url = bin_url;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.timeout_ms = 60000;
    http_cfg.keep_alive_enable = true;
    /* See manifest fetch for why these are bumped. */
    http_cfg.buffer_size = 4096;
    http_cfg.buffer_size_tx = 1024;
    /* GitHub releases redirect to objects.githubusercontent.com; the
     * default 10 redirects is plenty. */

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;
    ota_cfg.http_client_init_cb = ota_http_init_cb;

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota failed: %s", esp_err_to_name(err));
        device_diag_eventf(DIAG_ERROR, "ota", NULL,
            "https_ota failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "OTA image written + activated");
    device_diag_event(DIAG_INFO, "ota", "image staged", NULL);
    return true;
}

void ota_task(void *) {
    ESP_LOGI(TAG, "task started; running version=%s", ota_update::current_version());

    /* Boot settle. Let the sprite cache + scene + pet fetches at boot
     * complete before we eat radio bandwidth on a manifest poll. */
    vTaskDelay(pdMS_TO_TICKS(OTA_BOOT_DELAY_MS));

    while (true) {
        char remote_version[32] = {};
        char bin_url[256] = {};

        if (!fetch_manifest(g_manifest_url,
                            remote_version, sizeof(remote_version),
                            bin_url, sizeof(bin_url))) {
            ESP_LOGI(TAG, "no manifest this cycle; sleeping");
            ota_wait(OTA_CHECK_INTERVAL_MS);
            continue;
        }

        const char *running = ota_update::current_version();
        /* Compare versions semver-numerically rather than via strcmp.
         *
         * Running version comes from esp_app_get_description()->version,
         * which defaults to `git describe --tags --dirty`. On a clean
         * tagged build that's "v0.0.3"; on the commit *after* a tag it's
         * "v0.0.3-2-g3b284ff". The manifest version comes from the CI
         * workflow's ${TAG#v} strip, so it's "0.0.3".
         *
         * Two cases we want to suppress upgrade for:
         *   1. running == remote — already on the latest tag.
         *   2. running > remote  — hand-flashed dev build with a higher
         *      semver than the latest published release. Without this
         *      check, OTA would happily downgrade us. Bites local
         *      testing of unpublished work.
         *
         * Only "running < remote" actually triggers an upgrade. The
         * dev-build trailer ("-N-gSHA") is ignored by parse_semver,
         * so a "v0.0.3-2-gXYZ" build is treated as identical-version
         * to "0.0.3" and stays put — that's correct, the user is
         * testing local work past the tag. */
        int cmp = compare_versions(running, remote_version);
        if (cmp >= 0) {
            ESP_LOGI(TAG, "no upgrade: running=%s remote=%s (cmp=%d); sleeping",
                running, remote_version, cmp);
            device_diag_eventf(DIAG_INFO, "ota", NULL,
                "up to date %s", running);
            ota_wait(OTA_CHECK_INTERVAL_MS);
            continue;
        }
        ESP_LOGI(TAG, "update available: %s → %s", running, remote_version);
        device_diag_eventf(DIAG_INFO, "ota", NULL,
            "update %s -> %s", running, remote_version);

        if (perform_update(bin_url)) {
            ESP_LOGI(TAG, "update staged; signalling main to reboot at idle");
            g_reboot_ready = true;
            /* Done — main will reboot us. Park here forever. */
            while (true) vTaskDelay(pdMS_TO_TICKS(60000));
        }
        /* Update failed; back off a long time before retrying so a
         * persistently broken release doesn't hammer the network. */
        ESP_LOGW(TAG, "update failed; sleeping before retry");
        ota_wait(OTA_CHECK_INTERVAL_MS);
    }
}

}  // namespace

namespace ota_update {

bool mark_valid_if_pending() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return false;
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "pending OTA image promoted to valid");
            return true;
        }
        ESP_LOGW(TAG, "mark_app_valid failed: %s", esp_err_to_name(err));
        return false;
    }
    /* Any other state (VALID, NEW, INVALID, UNDEFINED for factory-style
     * boots) is fine; nothing to do. */
    return false;
}

void start_background_task(const char *manifest_url) {
    if (g_task_started) {
        ESP_LOGW(TAG, "task already started; ignoring");
        return;
    }
    if (!manifest_url || !manifest_url[0]) {
        ESP_LOGE(TAG, "manifest URL empty; not starting task");
        return;
    }
    g_manifest_url = manifest_url;
    /* 6 KB stack — TLS handshakes inside esp_https_ota use a lot of
     * mbedtls scratch space. Lower priority than the touch/render
     * loop (tskIDLE_PRIORITY+1) so OTA work never starves the UI. */
    BaseType_t ok = xTaskCreate(ota_task, "ota", 6 * 1024, nullptr,
                                tskIDLE_PRIORITY + 1, nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(ota) failed");
        return;
    }
    g_task_started = true;
}

bool reboot_ready() {
    return g_reboot_ready;
}

void check_now() {
    /* Settings → "update now": cut the inter-check sleep short. No-op
     * if the task hasn't started yet (device offline) — it'll run its
     * normal first check once net_worker brings WiFi up. */
    g_check_now = true;
    ESP_LOGI(TAG, "check-now requested");
}

const char *current_version() {
    const esp_app_desc_t *desc = esp_app_get_description();
    return desc ? desc->version : "unknown";
}

}  // namespace ota_update
