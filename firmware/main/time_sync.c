#include "time_sync.h"

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_creds.h"

static const char *TAG = "time_sync";

/* UK default. POSIX TZ format:
 *   GMT0           — std name + offset
 *   BST            — daylight name (no offset → +1)
 *   M3.5.0/1       — DST start: 3rd month, 5th week, day 0 (Sun), 01:00
 *   M10.5.0        — DST end:   10th month, 5th week, day 0 (Sun)
 * Override via runtime time_sync_set_tz() once we read it from
 * the server (or NVS). */
#define DEFAULT_TZ "GMT0BST,M3.5.0/1,M10.5.0"

#define SNTP_SYNC_TIMEOUT_MS 5000

static bool s_synced;
static char *s_tz;

static void apply_tz(const char *tz) {
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "TZ=%s", tz);
}

static void on_sync(struct timeval *tv) {
    s_synced = true;
    ESP_LOGI(TAG, "SNTP sync: %lld.%06ld", (long long)tv->tv_sec,
             (long)tv->tv_usec);
}

bool time_sync_init(void) {
    if (s_synced) return true;

    /* Pick the TZ in priority order:
     *   1. runtime override (time_sync_set_tz before init)
     *   2. NVS-stored value from the captive portal
     *   3. DEFAULT_TZ as a last resort.
     * Once we read NVS we keep that string in s_tz so subsequent
     * apply_tz calls hit the same value. */
    if (!s_tz) {
        char nvs_tz[MOCHI_TZ_MAX] = {0};
        if (nvs_creds_get_tz(nvs_tz, sizeof(nvs_tz)) && nvs_tz[0]) {
            s_tz = strdup(nvs_tz);
        }
    }
    apply_tz(s_tz ? s_tz : DEFAULT_TZ);

    /* esp_sntp from ESP-IDF — polls pool.ntp.org by default. */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(on_sync);
    esp_sntp_init();

    /* Block briefly waiting for the first sync. Anything beyond
     * ~5 s is a routing/DNS problem, not normal latency. */
    int waited_ms = 0;
    const int step_ms = 100;
    while (!s_synced && waited_ms < SNTP_SYNC_TIMEOUT_MS) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited_ms += step_ms;
    }
    if (!s_synced) {
        ESP_LOGW(TAG, "SNTP sync did not complete within %d ms",
                 SNTP_SYNC_TIMEOUT_MS);
        return false;
    }

    /* Verify we picked up a sane year (>= 2024). Otherwise the
     * NTP packet was bogus or the system clock isn't being applied. */
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    if (tm_now.tm_year + 1900 < 2024) {
        ESP_LOGW(TAG, "SNTP returned implausible year %d",
                 tm_now.tm_year + 1900);
        s_synced = false;
        return false;
    }
    ESP_LOGI(TAG, "local now: %04d-%02d-%02d %02d:%02d:%02d",
        tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
        tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    return true;
}

void time_sync_set_tz(const char *tz) {
    if (!tz || !*tz) return;
    char *copy = strdup(tz);
    if (!copy) return;
    free(s_tz);
    s_tz = copy;
    apply_tz(s_tz);
    /* Persist so subsequent boots use the same value without
     * needing the user to re-pick. Failure here is non-fatal —
     * the runtime change still takes effect for this session. */
    (void)nvs_creds_set_tz(s_tz);
}

int64_t time_sync_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
}

bool time_sync_synced(void) {
    return s_synced;
}
