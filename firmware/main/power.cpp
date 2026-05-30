#include "power.h"

#include "sdkconfig.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "device_diag.h"
#include "board_pins.h"
#include "touch.h"
#include "wifi_sta.h"

#if CONFIG_MOCHI_LIGHT_SLEEP
#include "esp_pm.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#endif

/* ── Kconfig policy (with fallbacks so the file compiles even if the
 * project Kconfig hasn't been regenerated into sdkconfig yet). ─────── */
#ifndef CONFIG_MOCHI_DOZE_TIMEOUT_S
#define CONFIG_MOCHI_DOZE_TIMEOUT_S 60
#endif
#ifndef CONFIG_MOCHI_DEEP_SLEEP_TIMEOUT_S
#define CONFIG_MOCHI_DEEP_SLEEP_TIMEOUT_S 0
#endif
#ifndef CONFIG_MOCHI_PM_CPU_MAX_MHZ
#define CONFIG_MOCHI_PM_CPU_MAX_MHZ 240
#endif
#ifndef CONFIG_MOCHI_PM_CPU_MIN_MHZ
#define CONFIG_MOCHI_PM_CPU_MIN_MHZ 80
#endif

#if CONFIG_MOCHI_LIGHT_SLEEP
#define PWR_SLEEP_EN 1
#else
#define PWR_SLEEP_EN 0
#endif
#if defined(CONFIG_MOCHI_DOZE_WIFI_POWERSAVE)
#define PWR_WIFI_PS 1
#else
#define PWR_WIFI_PS 0
#endif
#if defined(CONFIG_MOCHI_DOZE_WIFI_DROP)
#define PWR_WIFI_DROP 1
#else
#define PWR_WIFI_DROP 0
#endif

static const char *TAG = "power";

/* Substrate re-projection cadence per tier. Live matches the historical
 * 60 s; Doze stretches it so the SoC stays asleep between refreshes. */
static const int64_t LIVE_REFRESH_US = 60LL * 1000 * 1000;
static const int64_t DOZE_REFRESH_US = 300LL * 1000 * 1000;

static const int64_t DOZE_TIMEOUT_US =
    (int64_t)CONFIG_MOCHI_DOZE_TIMEOUT_S * 1000 * 1000;
static const int64_t DEEP_SLEEP_TIMEOUT_US =
    (int64_t)CONFIG_MOCHI_DEEP_SLEEP_TIMEOUT_S * 1000 * 1000;

static power_tier_t s_tier            = POWER_TIER_LIVE;
static int64_t      s_last_activity_us = 0;
static int64_t      s_last_update_us   = 0;
static int64_t      s_tier_enter_us    = 0;
static int64_t      s_live_us          = 0;   /* cumulative */
static int64_t      s_doze_us          = 0;   /* cumulative */
static uint32_t     s_doze_entries     = 0;
static bool         s_net_online       = false;
static bool         s_deep_sleep_req   = false;
static bool         s_inited           = false;
static bool         s_wifi_dropped     = false;  /* link dropped for doze */
/* Actual-sleep telemetry: sampled each snapshot to report the % of
 * wall time core 0 spent idle (≈ light sleep) since the previous
 * snapshot — the ground truth the tier counter can't give. */
static uint32_t     s_prev_idle_ct     = 0;
static int64_t      s_prev_idle_wall   = 0;

#if !PWR_WIFI_DROP
/* Switch WiFi modem-sleep depth. Only meaningful once WiFi is up; the
 * call is a harmless no-op error otherwise, but we gate on s_net_online
 * to avoid log noise. Restoring to MIN_MODEM matches the IDF default.
 * Unused when PWR_WIFI_DROP supersedes it (we drop the link instead). */
static void wifi_powersave(bool deep) {
#if PWR_WIFI_PS
    if (!s_net_online) return;
    esp_err_t err = esp_wifi_set_ps(deep ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "esp_wifi_set_ps(%d): %s", deep, esp_err_to_name(err));
    }
#else
    (void)deep;
#endif
}
#endif

/* WiFi policy on a tier edge. Drop (design/26 Option 1) takes precedence
 * over modem-sleep when configured: a dropped link can't also do PS. We
 * only drop when actually online (mirrors the PS guard) and only
 * reconnect what we dropped, so a never-connected device stays untouched
 * and the boot-time net_worker connect is never sabotaged. */
static void wifi_tier(bool entering_doze) {
#if PWR_WIFI_DROP
    if (entering_doze) {
        if (s_net_online) {
            wifi_sta::set_radio_active(false);
            s_wifi_dropped = true;
        }
    } else if (s_wifi_dropped) {
        wifi_sta::set_radio_active(true);
        s_wifi_dropped = false;
    }
#else
    wifi_powersave(entering_doze);
#endif
}

/* Edge work for a tier change. Accounting (time-in-tier) stays in
 * power_update; this only fires side effects + the transition record. */
static void apply_tier(power_tier_t want, int64_t now_us) {
    if (want == s_tier) return;
    const int64_t prev_ms = (now_us - s_tier_enter_us) / 1000;
    uint16_t mv = 0;
    battery_read(&mv, NULL);

    char ctx[96];
    snprintf(ctx, sizeof(ctx),
        "{\"prev_ms\":%lld,\"batt_mv\":%u,\"up_s\":%lld}",
        (long long)prev_ms, (unsigned)mv, (long long)(now_us / 1000000));

    if (want == POWER_TIER_DOZE) {
        s_doze_entries++;
        wifi_tier(true);
        touch::set_low_power(true);   /* stop the 10 Hz poll waking the SoC */
        device_diag_event(DIAG_INFO, "power", "doze", ctx);
        ESP_LOGI(TAG, "→ doze (live %lld ms)", (long long)prev_ms);
    } else {
        wifi_tier(false);
        touch::set_low_power(false);
        s_deep_sleep_req = false;
        device_diag_event(DIAG_INFO, "power", "wake", ctx);
        ESP_LOGI(TAG, "→ live (doze %lld ms)", (long long)prev_ms);
    }
    s_tier = want;
    s_tier_enter_us = now_us;
}

void power_init(void) {
    if (s_inited) return;
    s_inited = true;
    const int64_t now = esp_timer_get_time();
    s_last_activity_us = now;
    s_last_update_us   = now;
    s_tier_enter_us    = now;
    s_tier             = POWER_TIER_LIVE;
    s_prev_idle_wall   = now;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    s_prev_idle_ct     = (uint32_t)ulTaskGetIdleRunTimeCounter();
#endif

#if CONFIG_MOCHI_LIGHT_SLEEP
    /* Automatic light sleep: the SoC sleeps whenever no task holds a PM
     * lock (the main loop's wait_event block + the touch poll delay).
     * Wake on the touch INT (GPIO21, RTC-capable) and the two buttons so
     * a tap/press brings it back in microseconds with RAM intact. */
    esp_pm_config_t pm = {};
    pm.max_freq_mhz = CONFIG_MOCHI_PM_CPU_MAX_MHZ;
    pm.min_freq_mhz = CONFIG_MOCHI_PM_CPU_MIN_MHZ;
    pm.light_sleep_enable = true;
    esp_err_t perr = esp_pm_configure(&pm);
    if (perr != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure: %s", esp_err_to_name(perr));
    }
    gpio_wakeup_enable((gpio_num_t)MOCHI_TOUCH_INT_GPIO, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)MOCHI_PWR_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
    gpio_wakeup_enable((gpio_num_t)MOCHI_BOOT_BUTTON_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
#endif

    char ctx[192];
    snprintf(ctx, sizeof(ctx),
        "{\"sleep_en\":%d,\"wifi_ps\":%d,\"wifi_drop\":%d,\"doze_s\":%d,"
        "\"deep_s\":%d,\"cpu_max\":%d,\"cpu_min\":%d}",
        PWR_SLEEP_EN, PWR_WIFI_PS, PWR_WIFI_DROP,
        (int)CONFIG_MOCHI_DOZE_TIMEOUT_S, (int)CONFIG_MOCHI_DEEP_SLEEP_TIMEOUT_S,
        (int)CONFIG_MOCHI_PM_CPU_MAX_MHZ, (int)CONFIG_MOCHI_PM_CPU_MIN_MHZ);
    device_diag_event(DIAG_INFO, "power", "init", ctx);
    ESP_LOGI(TAG, "init %s", ctx);
}

void power_note_activity(int64_t now_us) {
    s_last_activity_us = now_us;
    /* Make wake crisp: if a touch lands mid-doze, leave the tier now
     * rather than waiting for the next power_update tick. */
    if (s_tier == POWER_TIER_DOZE) apply_tier(POWER_TIER_LIVE, now_us);
}

void power_update(int64_t now_us, bool inhibited, bool net_online) {
    if (!s_inited) power_init();
    s_net_online = net_online;

    /* Accumulate elapsed time into the current tier. */
    if (s_last_update_us != 0) {
        const int64_t dt = now_us - s_last_update_us;
        if (dt > 0) {
            if (s_tier == POWER_TIER_DOZE) s_doze_us += dt;
            else                            s_live_us += dt;
        }
    }
    s_last_update_us = now_us;

    /* A subsystem owning the device counts as activity and pins Live. */
    if (inhibited) s_last_activity_us = now_us;

    power_tier_t want = POWER_TIER_LIVE;
    if (DOZE_TIMEOUT_US > 0 && !inhibited) {
        const int64_t idle = now_us - s_last_activity_us;
        if (idle >= DOZE_TIMEOUT_US) want = POWER_TIER_DOZE;
    }
    apply_tier(want, now_us);

    /* Secondary timeout: protect the battery if left dozing for hours. */
    if (s_tier == POWER_TIER_DOZE && DEEP_SLEEP_TIMEOUT_US > 0 &&
        (now_us - s_tier_enter_us) >= DEEP_SLEEP_TIMEOUT_US) {
        s_deep_sleep_req = true;
    }
}

power_tier_t power_tier(void) { return s_tier; }

int64_t power_substrate_refresh_us(void) {
    return s_tier == POWER_TIER_DOZE ? DOZE_REFRESH_US : LIVE_REFRESH_US;
}

bool power_should_deep_sleep(void) { return s_deep_sleep_req; }

void power_telemetry_ctx(char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    uint16_t mv = 0;
    battery_read(&mv, NULL);
    const int64_t now = esp_timer_get_time();

    /* Actual core-0 idle (≈ light-sleep) fraction since the last
     * snapshot. The run-time counter is esp_timer microseconds, so the
     * ratio against wall-time us is a real residency %. uint32
     * subtraction is wrap-safe for any interval < ~71 min (snapshots are
     * ~5 min). -1 when run-time stats aren't compiled in. This is the
     * number that tells us whether light sleep is *actually* engaging,
     * vs the tier counter which only reports intent. */
    int sleep_pct = -1;
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    const uint32_t idle_now = (uint32_t)ulTaskGetIdleRunTimeCounter();
    const uint32_t didle    = idle_now - s_prev_idle_ct;   /* wrap-safe */
    const int64_t  dwall    = now - s_prev_idle_wall;
    if (dwall > 0) {
        int p = (int)((100LL * (int64_t)didle) / dwall);
        sleep_pct = p < 0 ? 0 : (p > 100 ? 100 : p);
    }
    s_prev_idle_ct   = idle_now;
    s_prev_idle_wall = now;
#endif

    snprintf(buf, cap,
        "{\"tier\":\"%s\",\"live_ms\":%lld,\"doze_ms\":%lld,\"doze_n\":%u,"
        "\"sleep_pct\":%d,\"sleep_en\":%d,\"wifi_ps\":%d,\"doze_s\":%d,"
        "\"batt_mv\":%u,\"up_s\":%lld}",
        s_tier == POWER_TIER_DOZE ? "doze" : "live",
        (long long)(s_live_us / 1000), (long long)(s_doze_us / 1000),
        (unsigned)s_doze_entries, sleep_pct, PWR_SLEEP_EN, PWR_WIFI_PS,
        (int)CONFIG_MOCHI_DOZE_TIMEOUT_S, (unsigned)mv,
        (long long)(now / 1000000));
}
