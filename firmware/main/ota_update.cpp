#include "ota_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
#include "ota_channel.h"

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

/* Per-channel manifest URLs (borrowed string literals from main.cpp).
 * The poll loop picks one each cycle based on the persisted channel
 * (ota_channel_get()), so a mid-session toggle takes effect at the
 * next check without restarting the task. */
const char *g_stable_url = nullptr;
const char *g_beta_url   = nullptr;
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

/* Pre-release tag of a version string, or nullptr if it's a plain
 * release. That's everything after the first '-' (build metadata after
 * a '+' is dropped — we never emit it). For "0.3.0-beta.7" this is
 * "beta.7"; for "0.3.0" it's nullptr. */
static const char *prerelease_of(const char *s) {
    if (!s) return nullptr;
    if (*s == 'v') s++;
    const char *dash = strchr(s, '-');
    return dash ? dash + 1 : nullptr;
}

/* Compare two pre-release strings per semver §11: dot-separated
 * identifiers compared left to right; all-numeric identifiers compared
 * numerically, otherwise ASCII; numeric ranks below alphanumeric; if
 * one runs out of identifiers first (and all preceding were equal) the
 * shorter set is the lower precedence. Returns -1/0/+1. */
static int compare_prerelease(const char *a, const char *b) {
    while (*a && *b) {
        const char *ae = a; while (*ae && *ae != '.') ae++;
        const char *be = b; while (*be && *be != '.') be++;
        size_t alen = (size_t)(ae - a), blen = (size_t)(be - b);
        bool anum = alen > 0, bnum = blen > 0;
        for (const char *p = a; p < ae; p++) if (!isdigit((unsigned char)*p)) anum = false;
        for (const char *p = b; p < be; p++) if (!isdigit((unsigned char)*p)) bnum = false;
        int cmp;
        if (anum && bnum) {
            long av = strtol(a, nullptr, 10);
            long bv = strtol(b, nullptr, 10);
            cmp = (av > bv) - (av < bv);
        } else if (anum != bnum) {
            cmp = anum ? -1 : 1;   /* numeric < alphanumeric */
        } else {
            size_t n = alen < blen ? alen : blen;
            cmp = strncmp(a, b, n);
            if (cmp == 0) cmp = (int)alen - (int)blen;
        }
        if (cmp != 0) return cmp < 0 ? -1 : 1;
        a = (*ae == '.') ? ae + 1 : ae;
        b = (*be == '.') ? be + 1 : be;
    }
    if (*a) return 1;    /* a has further identifiers → higher precedence */
    if (*b) return -1;
    return 0;
}

/* Compare two version strings by semver precedence. Returns:
 *    < 0  if a is older than b
 *    == 0 if equal
 *    > 0  if a is newer than b
 * Falls back to strcmp if either triplet fails to parse.
 *
 * The triplet (major.minor.patch) dominates; on a tie the pre-release
 * tag breaks it. A plain release outranks any pre-release of the same
 * triplet (semver §11: "1.0.0" > "1.0.0-beta"). This is what lets the
 * beta channel work: a "0.3.0-beta.7" build sorts above the previous
 * "0.2.x" stable (so beta devices roll forward) yet below the eventual
 * "0.3.0" release (so they land on stable when it ships), and successive
 * "-beta.<n>" builds order by their monotonic CI run number.
 *
 * Note the running version now comes from firmware/version.txt
 * (PROJECT_VER → esp_app_desc), so it's a clean "0.3.0" rather than the
 * old `git describe` "0.3.0-2-gSHA". A clean local build therefore
 * outranks the stable manifest of the same triplet and won't downgrade. */
static int compare_versions(const char *a, const char *b) {
    int amaj = 0, amin = 0, apat = 0;
    int bmaj = 0, bmin = 0, bpat = 0;
    bool aok = parse_semver(a, &amaj, &amin, &apat);
    bool bok = parse_semver(b, &bmaj, &bmin, &bpat);
    if (!aok || !bok) return strcmp(a ? a : "", b ? b : "");
    if (amaj != bmaj) return amaj - bmaj;
    if (amin != bmin) return amin - bmin;
    if (apat != bpat) return apat - bpat;
    const char *apr = prerelease_of(a);
    const char *bpr = prerelease_of(b);
    if (!apr && !bpr) return 0;
    if (!apr) return 1;    /* a release, b pre-release → a newer */
    if (!bpr) return -1;
    return compare_prerelease(apr, bpr);
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

        /* Pick the manifest for the channel chosen on-device. Read each
         * cycle so a dev_menu toggle (which also nudges check_now) is
         * honoured without restarting the task. Beta falls back to the
         * stable URL if main never supplied one. */
        const ota_channel_t channel = ota_channel_get();
        const char *manifest_url =
            (channel == OTA_CHANNEL_BETA && g_beta_url) ? g_beta_url : g_stable_url;
        ESP_LOGI(TAG, "checking %s channel: %s",
            ota_channel_name(channel), manifest_url ? manifest_url : "(none)");

        if (!manifest_url ||
            !fetch_manifest(manifest_url,
                            remote_version, sizeof(remote_version),
                            bin_url, sizeof(bin_url))) {
            ESP_LOGI(TAG, "no manifest this cycle; sleeping");
            ota_wait(OTA_CHECK_INTERVAL_MS);
            continue;
        }

        const char *running = ota_update::current_version();
        /* Compare by semver precedence (pre-release aware), not strcmp.
         *
         * Running version is esp_app_get_description()->version, which
         * now comes from firmware/version.txt (PROJECT_VER) — a clean
         * "0.3.0" on stable, "0.3.0-beta.<n>" on a beta build. The
         * manifest version is the same string the CI baked in.
         *
         * Two cases we suppress an upgrade for:
         *   1. running == remote — already on this build.
         *   2. running >  remote — a higher-precedence build than the
         *      manifest (e.g. a clean local build of the next version,
         *      or a beta sitting above the older stable on that channel).
         *      Without this, OTA would downgrade us.
         *
         * Only "running < remote" triggers an upgrade — which is exactly
         * what carries a beta device forward through "-beta.<n>" builds
         * and onto the matching stable when it ships (see compare_versions
         * for the precedence rules). */
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

void start_background_task(const char *stable_url, const char *beta_url) {
    if (g_task_started) {
        ESP_LOGW(TAG, "task already started; ignoring");
        return;
    }
    if (!stable_url || !stable_url[0]) {
        ESP_LOGE(TAG, "stable manifest URL empty; not starting task");
        return;
    }
    g_stable_url = stable_url;
    /* beta_url is optional — if null, the beta channel falls back to
     * stable in the poll loop. */
    g_beta_url = (beta_url && beta_url[0]) ? beta_url : nullptr;
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
