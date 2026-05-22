/*
 * mochi firmware — M4: first sprite from server.
 *
 * Boot path (M3 → M4 superset):
 *   1. e-paper power on, render boot splash via full refresh
 *   2. NVS load → if no creds, wifi_prov::run() (SoftAP + portal),
 *      persist creds, reboot
 *   3. NVS has creds → wifi_sta::connect(), render online IP
 *   4. Idle screen: "Tap BOOT to fetch sprite"
 *   5. On BOOT press: HTTPS GET https://mochi.val.run/devsprite/test
 *      → memcpy 5000 bytes into the e-paper framebuffer →
 *      EPD_Display() (full refresh). Show round-trip time as a
 *      caption overlay below the sprite.
 *
 * The blink LED stays on throughout as the "firmware is alive"
 * signal. Heartbeat runs on its own task so it survives every
 * blocking call in app_main.
 *
 * Acceptance (design/01-bring-up-plan.md M4): device on home WiFi →
 * BOOT press → fetch /devsprite/test → render fox sprite → round
 * trip <3s.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "board_pins.h"
#include "epaper_driver_bsp.h"
#include "epd_ui.h"
#include "nvs_creds.h"
#include "wifi_prov.h"
#include "wifi_sta.h"
#include "sprite_fetch.h"
#include "touch.h"
#include "rtc.h"
#include "shtc3.h"
#include "pair_creds.h"
#include "openai_key.h"
#include "device_pair.h"
#include "factory_reset.h"
#include "compositor.h"
#include "font8x8.h"
#include "battery.h"
#include "sleep_gesture.h"
#include "voice.h"
extern "C" {
#include "voice/voice_diag.h"
}
#include "sprite_cache.h"
#include "ota_update.h"
#include "key_portal.h"
extern "C" {
#include "pet_state.h"
#include "decay.h"
#include "engagement.h"
#include "mood.h"
#include "thought.h"
#include "event_log.h"
#include "time_sync.h"
#include "scene_pack.h"
#include "pet_pack.h"
#include "imagine.h"
}
#include "pet_sync.h"

/*
 * Endpoints for the on-device compositor. The scene is fetched once
 * at boot and cached in PSRAM; pet expressions are fetched per-tap
 * via the cell endpoint (native 96×96, 8-byte header + 1152 bytes).
 *
 * Touch zone → expression map:
 *   TL  feed     → 'eating'
 *   TR  play     → 'excited'
 *   BL  comfort  → 'comforted'
 *   BR  cheer    → 'cheerful_wave'
 *   centre attention → 'curious'
 * After each tap we render the expression for ~5 s, then re-fetch
 * 'neutral' as the resting pose.
 */
/* Scene area = panel below the status bar. We fetch the scene
 * pre-rendered at this exact size so it doesn't get vertically
 * squashed by the legacy 200×200 fit (the scene templates are 360×336,
 * not square).
 *
 * STATUS_BAR_H rows are reserved at the top, leaving the rest for
 * the scene. The fetch URL uses the server's `?fit=area` form. */
#define MOCHI_SCENE_URL  "https://mochi.val.run/devsprite/scene-v1/day?fit=fill&w=200&h=172"
/* Scene buffer covers the FULL panel now, not just the area below
 * the status bar. The MPK1 cells SPRITE·FORGE authors are 200×200
 * — same as the panel — and authored zones often extend into what
 * the old 200×172 layout was clipping (e.g. food/ball zones in
 * scenes_a sit at y=139..184). Rendering the full cell preserves
 * authored content; the status bar paint at panel rows 0..18 then
 * overwrites the top of the scene with white + glyphs. */
static constexpr size_t SCENE_W = 200;
static constexpr size_t SCENE_H = 200;
static constexpr size_t SCENE_BYTES = (SCENE_W / 8) * SCENE_H;  /* 5000 */
#define MOCHI_PET_CELL_URL_BASE "https://mochi.val.run/devsprite/cell/pet-v1/"

/* OTA — manifest is uploaded as a release asset by the GitHub
 * Actions workflow on every tag push. The `/releases/latest/download/`
 * URL is GitHub's redirector to the latest release's asset, so we
 * never have to bake a version number into firmware. The redirect
 * lands on objects.githubusercontent.com and is followed by the
 * default HTTP client. */
#define MOCHI_OTA_MANIFEST_URL \
    "https://github.com/christopherdebeer/mochi-esp/releases/latest/download/latest.json"

/* Pet cell geometry — pet-v1 template grid is 96×96. The fetcher
 * verifies the wire-format header matches before copying. */
static constexpr size_t PET_CELL_W = 96;
static constexpr size_t PET_CELL_H = 96;
static constexpr size_t PET_CELL_BYTES = (PET_CELL_W / 8) * PET_CELL_H;  /* 1152 */

/* Foot-anchored placement on the 200×200 panel: horizontally
 * centred, vertical position chosen so the pet sits on the scene
 * floor rather than floating mid-screen. With the status bar
 * eating the top 28 px and the scene cropped via fit=fill, the
 * "ground" lands near the bottom of the panel — pet bottom at
 * y≈190 puts feet on the floor. */
static constexpr int PET_DX = (MOCHI_EPD_WIDTH  - (int)PET_CELL_W) / 2;
static constexpr int PET_DY = (MOCHI_EPD_HEIGHT - (int)PET_CELL_H) - 12;

static constexpr int RESTING_AFTER_TAP_MS = 5000;

/* Care icons — 4 ui-v1 cells, one per zone, downsampled from 80×80
 * to 32×32 once at boot and stamped into the panel corners every
 * frame. 32×32 packed = 128 bytes per plane × 2 planes × 4 icons
 * = 1024 bytes total in PSRAM. */
static constexpr size_t UI_CELL_NATIVE_W = 80;
static constexpr size_t UI_CELL_NATIVE_H = 80;
static constexpr size_t UI_CELL_NATIVE_BYTES =
    (UI_CELL_NATIVE_W / 8) * UI_CELL_NATIVE_H;  /* 800 */

static constexpr size_t ICON_W = 48;
static constexpr size_t ICON_H = 48;
static constexpr size_t ICON_BYTES = (ICON_W / 8) * ICON_H;  /* 288 */
static constexpr int ICON_MARGIN = 4;

/*
 * Care-icon assignments and their target-zone screen positions.
 * Order matches the touch-zone enum (TL, TR, BL, BR). After a
 * 2026-05-18 reshuffle the comfort + cheer icons live at the top
 * (closer to the status bar) and the more "active" feed + play
 * sit at the bottom near the pet's feet — feels more natural for
 * thumb-reach on a handheld device.
 *
 *   TL (top-left)     → heart  → 'comforted'
 *   TR (top-right)    → star   → 'cheerful_wave'
 *   BL (bottom-left)  → bowl   → 'eating'
 *   BR (bottom-right) → ball   → 'excited'
 */
static const char *CARE_ICON_KEYS[4] = { "heart", "star", "bowl", "ball" };
#define MOCHI_UI_CELL_URL_BASE "https://mochi.val.run/devsprite/cell/ui-v1/"

/* Status bar — full-width slab at the top of the panel. Time on
 * left, pet name centred, battery on right. The bar is its own
 * band (no icons inside it); icons live below in the scene area.
 *
 * Tight vertical: 8-px font + 5-px top + 5-px bottom + 1-px
 * divider = 19 px. Reduces the wasted space the previous 28-px
 * bar had above and below the text. */
static constexpr int STATUS_BAR_H = 19;
static constexpr int STATUS_TEXT_Y = 5;
static constexpr int SCENE_TOP_Y = STATUS_BAR_H;     /* icons + pet
                                                        live below this */

static const char *TAG = "mochi";

/* ─── M11/M12 substrate dev snapshot ──────────────────────────────
 *
 * Until M13's snapshot pull lands, the device has no real pet to
 * project mood from. We use a hardcoded development pet here so the
 * decay → engagement → mood pipeline has realistic input on
 * hardware. Stats start a touch below "fresh" so all branches of
 * project_mood are reachable with a few minutes of poking:
 *
 *   - happiness=72, fullness=58, energy=63 (above the soft floors,
 *     so an unattended pet projects as content/curious initially)
 *   - born_at = ~3 days before boot (age stage = "young", multiplier
 *     ≈ 3.2× — needy enough to hit soft floors quickly)
 *   - last_interaction_at = boot time, so loneliness only fires
 *     after the age-scaled threshold (young: 24h / 3.2 ≈ 7.5h)
 *
 * stats_at = boot time means decay accumulates from that point.
 * After ~30 min of unattended runtime the device transitions
 * naturally through curious → hungry / tired and we can see
 * project_mood return different values. */
static pet_t s_dev_pet;

static void init_dev_pet(int64_t now_ms) {
    s_dev_pet.born_at             = now_ms - (3LL * 24 * 60 * 60 * 1000);
    s_dev_pet.stats_at            = now_ms;
    s_dev_pet.last_interaction_at = now_ms;
    s_dev_pet.stats.happiness     = 72;
    s_dev_pet.stats.fullness      = 58;
    s_dev_pet.stats.energy        = 63;
    s_dev_pet.asleep              = false;
    s_dev_pet.transient.sprite    = SPRITE_NONE;
    s_dev_pet.transient.until     = 0;
}

/* ─── M11.5a — thought-bubble subsystem ───────────────────────────
 *
 * See design/12-thought-bubble.md. Single bubble at a time for M1;
 * the renderer + touch dispatch read s_active_thought + s_thought_hit
 * directly. Suppression is local to this translation unit — a
 * 30 s window after a bubble tap so the same need can't immediately
 * re-fire before the substrate confirms the action server-side. */
static pet_thought_t      s_active_thought = {};
static thought_hit_rect_t s_thought_hit    = {};
static bool               s_thought_active = false;
static constexpr int64_t  THOUGHT_SUPPRESS_MS = 30LL * 1000;
static int64_t            s_thought_suppress_until_ms = 0;

/* Resolve a thought's action into the immediate transient
 * expression we render during the post-tap 5 s hold. Mirrors the
 * web side's TRANSIENT_FOR_KIND map; "slept" doesn't have a
 * transient sprite there, so we use "sleeping" here to give the kid
 * visible feedback while the substrate round-trip lands. */
static const char *event_kind_to_expr(event_kind_t k) {
    switch (k) {
        case EVENT_SLEPT:     return "sleeping";
        case EVENT_FED:       return "eating";
        case EVENT_PLAYED:    return "excited";
        case EVENT_COMFORTED: return "comforted";
        case EVENT_CHEERED:   return "cheerful_wave";
        case EVENT_HUGGED:    return "blushing";
        case EVENT_WOKE:      return "waking_up";
        case EVENT_TAPPED:    return "curious";
        default:              return nullptr;
    }
}

static const char *thought_action_to_expr(const pet_thought_t *t) {
    if (!t || t->action_kind != THOUGHT_ACTION_CARE_EVENT) return nullptr;
    return event_kind_to_expr(t->action_event);
}

/* Wall-clock ms since epoch — what the server uses for its
 * timestamps. Routed through time_sync.c so device + server agree on
 * "now" once SNTP has run. Pre-sync this returns whatever the kernel
 * thinks the time is (typically near-zero). Substrate projection
 * tolerates that — we just won't have meaningful decay until the
 * first sync lands. */
static int64_t now_ms_wall(void) {
    return time_sync_now_ms();
}

/* Returns a decayed snapshot of whatever we currently know about the
 * pet, preferring pet_sync's authoritative server snapshot when
 * available and falling back to the M11 hardcoded dev pet otherwise.
 * `now` runs decay forward from the snapshot's stats_at — same shape
 * as the TS code does it. */
static pet_t current_pet_decayed(int64_t now_ms) {
    pet_t snap;
    if (!pet_sync_get_snapshot(&snap)) {
        snap = s_dev_pet;
    }
    age_t a = compute_age(snap.born_at, now_ms);
    snap.stats = decay_stats(snap.stats, snap.stats_at,
                             snap.asleep, a.days, now_ms);
    return snap;
}

static void log_chip_info(void) {
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_size_bytes) != ESP_OK) {
        flash_size_bytes = 0;
    }

    size_t psram_size = 0;
#if CONFIG_SPIRAM
    psram_size = esp_psram_get_size();
#endif

    ESP_LOGI(TAG, "chip: %s rev %d.%d, %d cores",
        CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100,
        chip.cores);
    ESP_LOGI(TAG, "flash: %lu MB %s",
        (unsigned long)(flash_size_bytes / (1024 * 1024)),
        (chip.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");
    ESP_LOGI(TAG, "psram: %u KB",
        (unsigned)(psram_size / 1024));
}

static void led_init(void) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << MOCHI_LED_GPIO;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(MOCHI_LED_GPIO, 0);
}

static void boot_button_init(void) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << MOCHI_BOOT_BUTTON_GPIO;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void epd_power_on(void) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << MOCHI_EPD_PWR_GPIO;
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(MOCHI_EPD_PWR_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/*
 * The Waveshare board has three independent power rails (EPD,
 * audio, VBAT-sense) controlled by GPIOs 6/42/17. The vendor RTC +
 * SHTC3 examples enable ALL THREE before bringing up I²C — even
 * though those devices have nothing to do with audio or battery.
 * Empirically, leaving Audio_PWR off makes the I²C bus dead at the
 * wire level (every probe times out), so the touch controller, RTC
 * and SHTC3 must sit downstream of the same regulator the codec
 * uses. Diagnosed 2026-05-18 when the touch bring-up returned
 * universal probe timeouts.
 *
 * All rails are active-low (drive 0 to enable). VBAT_PWR is
 * different — its sense path is active-HIGH per the vendor
 * board_power_bsp::VBAT_POWER_ON. We mirror their convention
 * exactly here.
 */
static void peripheral_rails_on(void) {
    gpio_config_t cfg = {};
    cfg.pin_bit_mask =
        (1ULL << MOCHI_AUDIO_PWR_GPIO) |
        (1ULL << MOCHI_VBAT_SENSE_GPIO);
    cfg.mode = GPIO_MODE_OUTPUT;
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(MOCHI_AUDIO_PWR_GPIO, 0);   /* active-low: enable */
    gpio_set_level(MOCHI_VBAT_SENSE_GPIO, 1);  /* active-high per vendor */
    vTaskDelay(pdMS_TO_TICKS(50));
}

/*
 * The 1 Hz LED heartbeat lived here through M1–M6; it served as the
 * "firmware is alive" signal during bring-up. Removed 2026-05-18
 * once we trusted the panel + USB log enough that a blinking LED
 * was just noise. The LED pin (GP3) is still configured by
 * led_init() and held low; future milestones can repurpose it
 * (e.g. notification flash on incoming voice).
 */

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "—— mochi M3: boot ——");
    log_chip_info();

    led_init();
    boot_button_init();
    epd_power_on();
    peripheral_rails_on();

    /* Bring up the e-paper driver. Same pin map as M2. */
    custom_lcd_spi_t lcd_cfg = {};
    lcd_cfg.cs       = MOCHI_EPD_CS_GPIO;
    lcd_cfg.dc       = MOCHI_EPD_DC_GPIO;
    lcd_cfg.rst      = MOCHI_EPD_RST_GPIO;
    lcd_cfg.busy     = MOCHI_EPD_BUSY_GPIO;
    lcd_cfg.mosi     = MOCHI_EPD_MOSI_GPIO;
    lcd_cfg.scl      = MOCHI_EPD_SCK_GPIO;
    lcd_cfg.spi_host = SPI2_HOST;
    lcd_cfg.buffer_len = MOCHI_EPD_BUFFER_LEN;
    auto *epd = new epaper_driver_display(
        MOCHI_EPD_WIDTH, MOCHI_EPD_HEIGHT, lcd_cfg);

    /* Boot splash — full refresh so the partial-refresh "previous
     * image" buffer is seeded before any later partial calls. The
     * version overlay stamps small white text 30% from the top so
     * the running firmware version is visible at every cold boot
     * without USB. */
    epd_ui::render_boot_splash(epd);
    epd_ui::overlay_boot_version(epd, ota_update::current_version());
    epd->EPD_Init();
    epd->EPD_Display();
    epd->EPD_DisplayPartBaseImage();

    /* Factory-reset watchdog. Runs in parallel with everything from
     * here on, so the gesture works in any state — including stuck
     * provisioning or a failed STA join where the touch loop never
     * starts. PWR + BOOT held 10 seconds wipes NVS and reboots. */
    factory_reset::start(epd);

    /* Sleep watchdog. Like factory_reset but only fires on PWR
     * alone (BOOT must not be held). Two paths:
     *  - Rich: main's touch loop polls requested(), claims via
     *    mark_handled(), then renders the pet's `sleeping` cell
     *    over the current scene before commit_sleep().
     *  - Fallback: if main is blocked (provisioning, pair-wait,
     *    a halt loop), the watcher renders a generic "Asleep"
     *    screen itself and commits — so the gesture is reachable
     *    from every screen. We pass the epd here for that path. */
    sleep_gesture::start(epd);

    /* NVS first; cred lookup decides which branch we take. */
    nvs_creds_init();

    struct mochi_wifi_creds creds = {};
    bool have_creds = nvs_creds_count() > 0;

    /* "Enter provisioning on next boot" flag — set when a previous
     * boot's connect_any failed (e.g., user moved networks). We DON'T
     * try to do an in-process STA→AP swap from the connect_any error
     * path; ESP-IDF v5.3 has a known-rough mode-swap path that hangs
     * intermittently (see project_eink_wifi_handover memory).
     * Instead: persist the flag, reboot, and the next-boot's pre-wifi
     * branch lands directly in the no-creds provisioning flow without
     * touching the wifi STA stack first. */
    bool force_prov = nvs_creds_get_prov_on_boot();
    if (force_prov) {
        ESP_LOGI(TAG, "prov_on_boot flag set → force provisioning branch");
        nvs_creds_set_prov_on_boot(false);  /* one-shot */
        have_creds = false;                  /* fall through to prov */
    }

    if (!have_creds) {
        ESP_LOGI(TAG, "no NVS creds → entering provisioning");
        char openai_key[MOCHI_OPENAI_KEY_MAX + 1] = {};
        if (!wifi_prov::run(epd, &creds, openai_key, sizeof(openai_key))) {
            /* run() doesn't return false in current impl; if it ever
             * does, the only sensible response is to retry. */
            ESP_LOGE(TAG, "provisioning returned false; rebooting");
            esp_restart();
        }
        if (!nvs_creds_append(&creds)) {
            ESP_LOGE(TAG, "NVS append failed; rebooting");
            esp_restart();
        }
        /* Persist the key independently so factory_reset can wipe
         * either bucket on its own. Empty-key is allowed at submit
         * time; failures surface at the first voice session. */
        if (strlen(openai_key) > 0) {
            if (!openai_key_save(openai_key)) {
                ESP_LOGW(TAG, "openai_key_save failed; voice will be"
                              " unavailable until re-provisioned");
            }
        } else {
            ESP_LOGI(TAG, "no openai key supplied at provisioning");
        }
        /* Wipe the local copy now that NVS has it (or doesn't). */
        memset(openai_key, 0, sizeof(openai_key));
        ESP_LOGI(TAG, "provisioning complete; rebooting to clean STA boot");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    /* --- Already-provisioned branch. --- */

    size_t cred_count = nvs_creds_count();
    ESP_LOGI(TAG, "have %u stored network(s) → scan + connect_any",
        (unsigned)cred_count);
    wifi_sta::init_stack();

    char ip_str[16] = {};
    char joined_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};
    bool ok = wifi_sta::connect_any(
        ip_str, sizeof(ip_str),
        joined_ssid, sizeof(joined_ssid));
    if (!ok) {
        /*
         * No stored network was reachable. Could be we moved house,
         * stored APs are off, password rotated. We do *not* wipe
         * stored creds — when we eventually return to a known
         * network the cred is still there.
         *
         * We persist a "prov_on_boot" flag and reboot rather than
         * doing an in-process STA→AP swap. ESP-IDF v5.3 has a
         * known-rough mode-swap path that hangs (and `wifi_prov::run`
         * also calls `esp_event_loop_create_default` with
         * ESP_ERROR_CHECK — that aborts when the STA path already
         * created the loop, which is what bit us). The reboot path
         * lands in a clean boot, then the prov_on_boot flag forces
         * the no-creds branch above. See project_eink_wifi_handover.
         */
        ESP_LOGW(TAG, "no stored network reachable; persisting prov_on_boot"
                      " flag and rebooting (existing creds preserved)");
        nvs_creds_set_prov_on_boot(true);
        epd_ui::render_prov_failed(epd);
        epd->EPD_Init_Partial();
        epd->EPD_DisplayPart();
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    ESP_LOGI(TAG, "online; IP=%s, joined='%s'", ip_str, joined_ssid);

    /*
     * OTA bookkeeping. If this boot is the first one after an OTA
     * install, the running slot is marked PENDING_VERIFY and would
     * roll back on next reset unless we promote it now. WiFi up is
     * the earliest signal we have that the new image is healthy:
     * provisioning, NVS, STA stack, radio + DHCP all worked.
     *
     * Then spawn the background OTA task. It sleeps 30s before its
     * first manifest poll so it never competes with the boot-time
     * sprite fetches for radio bandwidth.
     */
    ota_update::mark_valid_if_pending();
    ESP_LOGI(TAG, "running firmware version: %s", ota_update::current_version());
    ota_update::start_background_task(MOCHI_OTA_MANIFEST_URL);

    /*
     * Wall-clock sync. Substrate decay/engagement/mood projection
     * uses ms-since-epoch timestamps that must agree with the
     * server's `Date.now()`. Without this, ageDays/lastInteractionAt
     * comparisons drift by whatever offset the kernel happens to
     * have. Block briefly waiting for the first SNTP sync; if it
     * doesn't land in 5s the pet still works but its sense of time
     * is approximate until a later resync. */
    time_sync_init();

    /*
     * One-shot environmental + RTC readout before the touch loop
     * takes over. Bus and rails are already up (they came up for
     * touch); these drivers just attach as additional devices on
     * the same I2cMasterBus singleton.
     *
     * If the RTC's oscillator-stop flag is set we plant a sentinel
     * time (2026-05-18 12:00:00) so that subsequent reads return
     * something visibly non-zero. M11 (decay clock) will replace
     * this with a real "set from network time" path.
     */
    if (rtc_init()) {
        if (rtc_lost_power()) {
            mochi_datetime t = { 2026, 5, 18, 12, 0, 0, 0 };
            if (rtc_set(&t)) {
                ESP_LOGI(TAG, "rtc: planted sentinel 2026-05-18 12:00:00");
            }
        }
        mochi_datetime now = {};
        if (rtc_get(&now)) {
            ESP_LOGI(TAG, "rtc: %04u-%02u-%02u %02u:%02u:%02u (wd=%u)",
                now.year, now.month, now.day,
                now.hour, now.minute, now.second, now.weekday);
        }
    }

    if (shtc3_init()) {
        float temp_c = 0, rh = 0;
        if (shtc3_read(&temp_c, &rh)) {
            ESP_LOGI(TAG, "shtc3: %.2f°C, %.1f%%RH", temp_c, rh);
        }
    }

    /*
     * M9 codec init smoke test. After RTC + SHTC3 we know the I²C
     * bus + Audio_PWR rail are healthy. voice::init() probes the
     * ES8311 over the same bus and configures the I²S pins for
     * future audio work. Failure is logged-and-continued — codec
     * init must not break the existing M8.5 pet UI path. If this
     * collides with our existing I²C bus singleton (the vendor
     * Waveshare i2c_bsp), the codec_board module's own bus init
     * will trip; we'll see it in the log and decide how to refactor.
     */
    if (!voice::init()) {
        ESP_LOGW(TAG, "voice init failed; continuing boot");
    }

    /* Battery sense — ADC1 ch3 via the 1:2 divider on VBAT_PWR
     * (GPIO 17 already enabled by peripheral_rails_on()). One read
     * at boot for the log; render_chrome() polls again per frame.
     *
     * Diagnostic: take 10 samples over 2 s and report min/mean/max.
     * With a real LiPo present the readings are tight (<50 mV
     * spread) and sit at 4.05–4.20 V on USB charge. Without a
     * battery the readings drift, saturate, or report something
     * implausible — that tells us the "with lithium battery"
     * SKU shipped without one (or the cell is in over-discharge
     * cutoff and needs more USB time). */
    if (battery_init()) {
        uint16_t mv_min = 0xFFFF, mv_max = 0;
        uint32_t mv_sum = 0;
        int samples = 0;
        for (int i = 0; i < 10; i++) {
            uint16_t mv = 0; uint8_t pct = 0;
            if (battery_read(&mv, &pct)) {
                if (mv < mv_min) mv_min = mv;
                if (mv > mv_max) mv_max = mv;
                mv_sum += mv;
                samples++;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (samples > 0) {
            uint16_t mean = (uint16_t)(mv_sum / samples);
            int spread = (int)mv_max - (int)mv_min;
            ESP_LOGI(TAG,
                "battery diag: min=%u mean=%u max=%u spread=%d mV (n=%d)",
                (unsigned)mv_min, (unsigned)mean,
                (unsigned)mv_max, spread, samples);
            /* Heuristic interpretation. Calibrated against typical
             * single-cell LiPo behaviour on USB. */
            if (mean >= 4000 && mean <= 4250 && spread < 50) {
                ESP_LOGI(TAG, "battery diag → looks like a real LiPo on USB charge");
            } else if (mean >= 3300 && mean <= 4000 && spread < 50) {
                ESP_LOGI(TAG, "battery diag → looks like a LiPo discharging or just plugged in");
            } else if (spread >= 50) {
                ESP_LOGW(TAG, "battery diag → readings drift; likely NO LiPo connected");
            } else {
                ESP_LOGW(TAG, "battery diag → out-of-range mean (%u mV); LiPo presence unclear",
                    (unsigned)mean);
            }
        }
    }

    /*
     * Persistent sprite cache — LittleFS on the 'storage' partition.
     * Per-sheet ETag check at boot: HEAD each sheet we use, compare
     * against the locally-stored tag, and invalidate that sheet's
     * cached blobs if they don't match. Subsequent fetches go
     * through fetch_or_load_* helpers below — they read from cache
     * when present and only hit the network on cache miss.
     *
     * First boot has no cache yet, so everything misses and we pay
     * the full ~22s of fetches once. Subsequent boots — and any
     * post-pair reboot — skip the network entirely if the server
     * artwork hasn't changed.
     */
    /* sprite_cache was already init'd in the early-dump block above;
     * re-init is idempotent and returns the same result. */
    bool cache_ok = sprite_cache::init();

    /* Once LittleFS is mounted, dump (and consume) any voice session
     * log left behind by the previous boot. This is what makes
     * disconnected-USB voice testing recoverable: the device flushes
     * the session log on stop_session, the next boot prints it.
     * Idempotent — no-op if no session ran. */
    if (cache_ok) {
        voice_diag_dump_last();
        /* M12 — bring up the on-device event log. Same partition,
         * separate file (/lfs/events.bin). engagement.c reads the
         * recent slice on every render via event_log_load_recent. */
        event_log_init();
    }

    if (cache_ok) {
        struct { const char *sheet; const char *probe_url; } probes[] = {
            { "pet-v1",   "https://mochi.val.run/devsprite/cell/pet-v1/neutral" },
            { "ui-v1",    "https://mochi.val.run/devsprite/cell/ui-v1/heart"    },
            { "scene-v1", "https://mochi.val.run/devsprite/scene-v1/day"        },
        };
        for (auto &p : probes) {
            char remote[40] = {};
            char local[40]  = {};
            if (!sprite_fetch_head_etag(p.probe_url, remote, sizeof(remote))) {
                ESP_LOGW(TAG, "etag probe failed for '%s'; cache unchanged",
                    p.sheet);
                continue;
            }
            sprite_cache::load_etag(p.sheet, local, sizeof(local));
            if (strcmp(remote, local) != 0) {
                ESP_LOGI(TAG, "etag '%s': remote=%s local=%s — invalidating",
                    p.sheet, remote, local[0] ? local : "(none)");
                sprite_cache::invalidate_sheet(p.sheet);
                sprite_cache::store_etag(p.sheet, remote);
            } else {
                ESP_LOGI(TAG, "etag '%s' unchanged (%s) — using cache",
                    p.sheet, remote);
            }
        }
    } else {
        ESP_LOGW(TAG, "sprite cache disabled; falling back to per-fetch");
    }

    /*
     * M5 — device pairing. If NVS has no pet binding yet, run the
     * pair-init / pair-check protocol against mochi.val.run, persist
     * the result, and reboot. The reboot is deliberate: it re-enters
     * this same code path with creds present, which keeps the
     * already-paired branch the canonical "boot path", and avoids
     * having to special-case "we just paired" anywhere downstream.
     *
     * If we already have a pairing, just log it. We don't (yet) verify
     * with the server on each boot — the substrate is durable and the
     * pet_id is what every future call uses as bearer. A future
     * health-check on /api/pets/<id> can reject revoked devices.
     */
    struct mochi_pair_creds pair = {};
    bool have_pair = pair_creds_load(&pair);
    if (!have_pair) {
        ESP_LOGI(TAG, "no pairing → entering pair flow");
        device_pair::InitResult init = {};
        if (!device_pair::request_code(&init)) {
            epd_ui::render_pair_failed(epd);
            epd->EPD_Init_Partial();
            epd->EPD_DisplayPart();
            ESP_LOGE(TAG, "pair-init failed; halting (touch tap to retry)");
            /* Sit on the failed screen — touch handler below isn't
             * running yet, so this halts. M5 acceptance: user
             * power-cycles to retry. M5+ will retry automatically. */
            while (true) vTaskDelay(pdMS_TO_TICKS(60000));
        }

        epd_ui::render_pair_prompt(epd, init.code);
        epd->EPD_Init_Partial();
        epd->EPD_DisplayPart();

        /*
         * Block here, polling every 5 s, for up to the server's
         * 10-min code TTL. If we hit timeout (or 410 expired) the
         * function returns false and we render the failed screen.
         */
        if (!device_pair::wait_for_user(&init, &pair, 10 * 60 * 1000)) {
            epd_ui::render_pair_failed(epd);
            epd->EPD_Init_Partial();
            epd->EPD_DisplayPart();
            ESP_LOGW(TAG, "pair-check did not complete; halting");
            while (true) vTaskDelay(pdMS_TO_TICKS(60000));
        }

        if (!pair_creds_save(&pair)) {
            ESP_LOGE(TAG, "pair save failed; rebooting to retry");
            esp_restart();
        }

        epd_ui::render_pair_success(epd, pair.pet_name);
        epd->EPD_Init_Partial();
        epd->EPD_DisplayPart();

        ESP_LOGI(TAG, "pairing complete; rebooting to clean post-pair boot");
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    ESP_LOGI(TAG, "paired to '%s' (pet_id=%s)",
        pair.pet_name, pair.pet_id);

    /*
     * Pet-on-scene compositor pipeline.
     *
     * Three buffers live in PSRAM:
     *   scene_fb (5000 B)    — fetched once at boot, never written
     *                          again. The home backdrop.
     *   pet_cell (1152 B)    — the current pet expression, fetched
     *                          per-tap from /devsprite/cell.
     *   composite (5000 B)   — what we push to the panel. Built each
     *                          render: copy(scene) → blit(pet at
     *                          PET_DX, PET_DY) → push.
     *
     * The compositor's blit_mask treats source bit `1` as "leave the
     * scene visible" (transparent), so cell PNGs with cream
     * backgrounds composite cleanly without any keying step on the
     * device side.
     */
    constexpr size_t FB_LEN = MOCHI_EPD_BUFFER_LEN;  /* 5000 */
    /* Scene buffer is sized for the area BELOW the status bar
     * (200×172 = 4300 bytes) — not the whole panel. The composite
     * remains full-panel size. */
    uint8_t *scene_fb  = (uint8_t *)heap_caps_malloc(SCENE_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *composite = (uint8_t *)heap_caps_malloc(FB_LEN, MALLOC_CAP_SPIRAM);
    /* Pet cell uses the two-plane format: ink (line work) + mask
     * (silhouette). Both same size; allocated separately so we can
     * pass them to blit_two_plane without packing. */
    uint8_t *pet_ink   = (uint8_t *)heap_caps_malloc(PET_CELL_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *pet_mask  = (uint8_t *)heap_caps_malloc(PET_CELL_BYTES, MALLOC_CAP_SPIRAM);

    /* Care icon storage: 4 icons × 2 planes (ink + mask), each at
     * the downsampled 32×32 = 128-byte size. We also need a single
     * 800-byte staging buffer at the native 80×80 size to fetch
     * into before downsampling. The staging buffer is used once
     * per icon at boot and then thrown away by going out of scope. */
    uint8_t *icon_ink[4]  = {};
    uint8_t *icon_mask[4] = {};
    for (int i = 0; i < 4; i++) {
        icon_ink[i]  = (uint8_t *)heap_caps_malloc(ICON_BYTES, MALLOC_CAP_SPIRAM);
        icon_mask[i] = (uint8_t *)heap_caps_malloc(ICON_BYTES, MALLOC_CAP_SPIRAM);
    }

    if (!scene_fb || !composite || !pet_ink || !pet_mask) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        while (true) vTaskDelay(pdMS_TO_TICKS(60000));
    }
    for (int i = 0; i < 4; i++) {
        if (!icon_ink[i] || !icon_mask[i]) {
            ESP_LOGE(TAG, "PSRAM alloc failed for icon %d", i);
            while (true) vTaskDelay(pdMS_TO_TICKS(60000));
        }
    }

    /*
     * Fetch the home scene at the scene-area size (200×172).
     * Cache-first: if the LittleFS cache has fill_200x172 for
     * scene-v1, load from disk (typical ~5-10 ms vs ~1500 ms for
     * a network fetch). Miss → network → store.
     *
     * If both fail (no cache + no network) we fall back to a
     * paper-white background so the pet is still visible.
     */
    {
        /* MPK1 — bundled at build time via EMBED_FILES, see
         * design/13-build-time-asset-packs.md. Replaces the previous
         * /devsprite/scene-v1 HTTPS fetch entirely: zero-network,
         * zero-latency, fixed working set, and the scene comes with
         * named tap zones authored by SPRITE·FORGE rather than
         * coarse corner quadrants. The runtime fetch path is kept
         * compiled (sprite_fetch / sprite_cache::load both still
         * exist) only for the unused MOCHI_SCENE_URL define and as
         * a future fallback for non-bundled scenes. */
        if (scene_pack_init() &&
            scene_pack_blit_current(scene_fb, SCENE_W, SCENE_H)) {
            ESP_LOGI(TAG, "scene from pack idx=%u",
                (unsigned)scene_pack_current());
        } else {
            ESP_LOGW(TAG, "scene_pack unavailable; blank backdrop");
            compositor::clear_to_paper(scene_fb, SCENE_W, SCENE_H);
        }
    }

    /* Bring up the bundled pet pack so render_with_expression can
     * serve cells from flash without touching the network. Network
     * stays as the fallback for expressions absent from the pack
     * (and for the pack-unavailable case). */
    if (!pet_pack_init()) {
        ESP_LOGW(TAG, "pet_pack unavailable; falling back to network "
                      "+ littlefs cache for every render");
    }

    /* On-device scene generation pipeline. Idle until a voice tool
     * call (`imagine_place`) lands; see design/16-on-device-imagine.md. */
    if (!imagine_init()) {
        ESP_LOGW(TAG, "imagine pipeline unavailable; voice "
                      "imagine_place tool will refuse");
    }

    /*
     * Fetch each care icon at native 80×80, downsample to 32×32
     * once, cache the downsampled planes for the rest of the
     * device's life.
     *
     * The native staging buffers are intentionally PSRAM-allocated
     * (rather than on the main task stack) because sprite_fetch_cell
     * already uses ~4 KB of stack for its own staging during TLS,
     * and adding 1.6 KB of stack-resident staging here was enough
     * to overflow the 8 KB main task stack on the first icon fetch.
     */
    uint8_t *native_ink  = (uint8_t *)heap_caps_malloc(UI_CELL_NATIVE_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *native_mask = (uint8_t *)heap_caps_malloc(UI_CELL_NATIVE_BYTES, MALLOC_CAP_SPIRAM);
    if (!native_ink || !native_mask) {
        ESP_LOGE(TAG, "PSRAM alloc failed for icon staging");
        while (true) vTaskDelay(pdMS_TO_TICKS(60000));
    }
    {
        /* Suffix for cached icons baked into the layout. If we ever
         * change ICON_W or ICON_H, bump this string so old cached
         * blobs get ignored. */
        char icon_suffix[24];
        snprintf(icon_suffix, sizeof(icon_suffix), "_icon_%ux%u",
            (unsigned)ICON_W, (unsigned)ICON_H);

        /*
         * Two cached blobs per icon: one for ink, one for mask.
         * Suffix is "<icon>_icon_48x48_ink" / "<icon>_icon_48x48_mask".
         * Loaded together; if either misses we re-fetch + re-downsample.
         */
        for (int i = 0; i < 4; i++) {
            char ink_suffix[40], mask_suffix[40];
            snprintf(ink_suffix,  sizeof(ink_suffix),  "%s%s_ink",
                CARE_ICON_KEYS[i], icon_suffix);
            snprintf(mask_suffix, sizeof(mask_suffix), "%s%s_mask",
                CARE_ICON_KEYS[i], icon_suffix);

            size_t got_ink = 0, got_mask = 0;
            bool from_cache =
                cache_ok &&
                sprite_cache::load("ui-v1", ink_suffix,
                    icon_ink[i], ICON_BYTES, &got_ink) &&
                got_ink == ICON_BYTES &&
                sprite_cache::load("ui-v1", mask_suffix,
                    icon_mask[i], ICON_BYTES, &got_mask) &&
                got_mask == ICON_BYTES;
            if (from_cache) {
                ESP_LOGI(TAG, "icon '%s' loaded from cache",
                    CARE_ICON_KEYS[i]);
                continue;
            }

            /* Cache miss → network fetch native size, downsample,
             * store the downsampled planes. */
            char url[160];
            snprintf(url, sizeof(url), "%s%s",
                MOCHI_UI_CELL_URL_BASE, CARE_ICON_KEYS[i]);
            uint16_t w = 0, h = 0; uint32_t ms = 0;
            bool ok = sprite_fetch_cell(url, native_ink, native_mask,
                UI_CELL_NATIVE_BYTES, &w, &h, &ms);
            if (!ok || w != UI_CELL_NATIVE_W || h != UI_CELL_NATIVE_H) {
                ESP_LOGW(TAG, "icon '%s' fetch failed (%ux%u); blanking",
                    CARE_ICON_KEYS[i], w, h);
                memset(icon_ink[i],  0xFF, ICON_BYTES);
                memset(icon_mask[i], 0xFF, ICON_BYTES);
                continue;
            }
            memset(icon_ink[i],  0xFF, ICON_BYTES);
            memset(icon_mask[i], 0xFF, ICON_BYTES);
            compositor::downsample_plane(icon_ink[i],  ICON_W, ICON_H,
                native_ink,  UI_CELL_NATIVE_W, UI_CELL_NATIVE_H);
            compositor::downsample_plane(icon_mask[i], ICON_W, ICON_H,
                native_mask, UI_CELL_NATIVE_W, UI_CELL_NATIVE_H);
            if (cache_ok) {
                sprite_cache::store("ui-v1", ink_suffix,  icon_ink[i],  ICON_BYTES);
                sprite_cache::store("ui-v1", mask_suffix, icon_mask[i], ICON_BYTES);
            }
            ESP_LOGI(TAG, "icon '%s' fetched + cached (%lu ms)",
                CARE_ICON_KEYS[i], (unsigned long)ms);
        }
        /* Free the native staging — only used during boot. */
        free(native_ink);
        free(native_mask);
        native_ink = nullptr;
        native_mask = nullptr;
    }

    /*
     * Per-corner placement for icons. Order matches CARE_ICON_KEYS:
     * TL, TR, BL, BR. Top row sits flush against the bottom of the
     * status bar; bottom row flush with the bottom of the panel,
     * both with a small horizontal margin.
     */
    const int icon_pos_x[4] = {
        ICON_MARGIN,
        (int)MOCHI_EPD_WIDTH - (int)ICON_W - ICON_MARGIN,
        ICON_MARGIN,
        (int)MOCHI_EPD_WIDTH - (int)ICON_W - ICON_MARGIN,
    };
    const int icon_pos_y[4] = {
        SCENE_TOP_Y + ICON_MARGIN,
        SCENE_TOP_Y + ICON_MARGIN,
        (int)MOCHI_EPD_HEIGHT - (int)ICON_H - ICON_MARGIN,
        (int)MOCHI_EPD_HEIGHT - (int)ICON_H - ICON_MARGIN,
    };

    /* Helper: fetch <expression> cell, composite scene + pet, push
     * to panel. Returns true on render success. Used for every tap
     * and for the post-tap return-to-neutral. */
    /* Stamp the status bar + 4 corner icons into composite. Called
     * after scene + pet have been laid down. The bar lives at the
     * very top of the panel (rows 0..27) and is a clean
     * paper-white slab so the time + name are legible regardless of
     * what the scene drew there. The icons sit at the four corners
     * over scene/bar; their two-plane format means they only draw
     * where opaque. */
    auto render_chrome = [&]() {
        /* Paper-white bar at top, full-width, STATUS_BAR_H rows
         * tall. The bar is its own band: scene + pet + icons all
         * live BELOW it, not behind it. Stride is 25 bytes/row for
         * a 200-pixel wide framebuffer. */
        memset(composite, 0xFF, 25 * STATUS_BAR_H);

        /*
         * Status bar layout, all scale-1 (8 px tall, 8 px wide per
         * glyph):
         *
         *   HH:MM      Mochi      87%
         *   ↑ left    ↑ centre    ↑ right
         *   x=4       x=centred   x=W-4-text_w
         *
         * Time, name, and battery are independent strings — the
         * pet name centres on the bar regardless of how long the
         * other two pieces are.
         */
        char time_str[8] = "--:--";
        if (time_sync_synced()) {
            /* Local time from <time.h> — applies the TZ string set
             * by time_sync_init/set_tz. The PCF85063 RTC is still
             * around (read at boot for cold-start signage) but for
             * the status bar we trust SNTP-synced wall time once
             * it's available. */
            time_t now_t = time(NULL);
            struct tm tm_now;
            localtime_r(&now_t, &tm_now);
            snprintf(time_str, sizeof(time_str), "%02d:%02d",
                tm_now.tm_hour, tm_now.tm_min);
        } else {
            mochi_datetime now = {};
            if (rtc_get(&now)) {
                snprintf(time_str, sizeof(time_str), "%02u:%02u",
                    now.hour, now.minute);
            }
        }

        char batt_str[8] = "--%";
        uint16_t batt_mv = 0; uint8_t batt_pct = 0;
        if (battery_read(&batt_mv, &batt_pct)) {
            snprintf(batt_str, sizeof(batt_str), "%u%%",
                (unsigned)batt_pct);
        }

        /* One pass blits a glyph string at a chosen x. Reused for
         * each of the three segments. */
        auto blit_status_text = [&](const char *s, int x_origin) {
            for (size_t i = 0; s[i]; i++) {
                const uint8_t *g = font8x8_glyph(s[i]);
                const int ox = x_origin + (int)i * 8;
                for (int row = 0; row < 8; row++) {
                    const uint8_t bits = g[row];
                    for (int col = 0; col < 8; col++) {
                        if (!((bits >> col) & 1)) continue;
                        const int px = ox + col;
                        const int py = STATUS_TEXT_Y + row;
                        if (px < 0 || py < 0 ||
                            px >= (int)MOCHI_EPD_WIDTH ||
                            py >= (int)MOCHI_EPD_HEIGHT) continue;
                        const size_t off = (size_t)py * 25 + ((size_t)px >> 3);
                        composite[off] &= (uint8_t)~(1u << (7 - ((size_t)px & 7)));
                    }
                }
            }
        };

        /* Left: time. Right: battery. Centre: pet name (centred on
         * panel midpoint, not the slot between the two — the centre
         * looks more visually balanced). */
        constexpr int STATUS_PAD = 4;
        blit_status_text(time_str, STATUS_PAD);

        const int batt_w = (int)strlen(batt_str) * 8;
        blit_status_text(batt_str,
            (int)MOCHI_EPD_WIDTH - STATUS_PAD - batt_w);

        const int name_w = (int)strlen(pair.pet_name) * 8;
        int name_x = ((int)MOCHI_EPD_WIDTH - name_w) / 2;
        if (name_x < STATUS_PAD) name_x = STATUS_PAD;
        blit_status_text(pair.pet_name, name_x);

        /* 1-pixel black divider at the bottom of the status bar
         * (y = STATUS_BAR_H - 1). Sits between bar text and the
         * scene; reinforces the bar as its own band. */
        {
            const int y = STATUS_BAR_H - 1;
            const size_t row_off = (size_t)y * 25;
            memset(composite + row_off, 0x00, 25);
        }

        /* Stamp the 4 care icons — but ONLY when the current scene
         * doesn't have authored zones. A scenes_a cell with food /
         * heart / ball / door drawn into the world doesn't need
         * the corner-icon overlay too; it's redundant chrome on
         * top of diegetic affordances. Unzoned scenes (most of
         * scenes_a today) keep the corner-icon UX.
         *
         * Two-plane blit means transparent pixels in the icon don't
         * trash the underlying scene. */
        if (!scene_pack_current_has_zones()) {
            for (int i = 0; i < 4; i++) {
                compositor::blit_two_plane(composite,
                    MOCHI_EPD_WIDTH, MOCHI_EPD_HEIGHT,
                    icon_ink[i], icon_mask[i],
                    ICON_W, ICON_H,
                    icon_pos_x[i], icon_pos_y[i]);
            }
        }
    };

    auto render_with_expression = [&](const char *expr,
                                      bool full_refresh,
                                      const pet_thought_t *thought) -> bool {
        /*
         * Pet cell load priority:
         *   1. Bundled MPK1 pack (flash, zero-network).
         *   2. littlefs cache from a previous fetch.
         *   3. Server fetch over HTTPS.
         *
         * The bundle lets the device run the pet UI offline once
         * paired — boot doesn't have to wait for WiFi to render.
         * The cache + network paths stay as fallbacks for
         * expressions the pack doesn't carry, and for the
         * pet_pack-unavailable case.
         */
        const char *cell_source = nullptr;
        uint32_t ms = 0;
        {
            uint16_t pw = 0, ph = 0;
            if (pet_pack_load(expr, pet_ink, pet_mask,
                              PET_CELL_BYTES, &pw, &ph)) {
                if (pw != PET_CELL_W || ph != PET_CELL_H) {
                    ESP_LOGW(TAG, "pack cell '%s' dims %ux%u != %ux%u",
                        expr, pw, ph,
                        (unsigned)PET_CELL_W, (unsigned)PET_CELL_H);
                } else {
                    cell_source = "pack";
                }
            }
        }

        char ink_suffix[40], mask_suffix[40];
        snprintf(ink_suffix,  sizeof(ink_suffix),  "%s_cell_ink",  expr);
        snprintf(mask_suffix, sizeof(mask_suffix), "%s_cell_mask", expr);

        if (!cell_source) {
            size_t got_ink = 0, got_mask = 0;
            bool from_cache =
                cache_ok &&
                sprite_cache::load("pet-v1", ink_suffix,
                    pet_ink, PET_CELL_BYTES, &got_ink) &&
                got_ink == PET_CELL_BYTES &&
                sprite_cache::load("pet-v1", mask_suffix,
                    pet_mask, PET_CELL_BYTES, &got_mask) &&
                got_mask == PET_CELL_BYTES;
            if (from_cache) cell_source = "cache";
        }

        if (!cell_source) {
            char url[160];
            snprintf(url, sizeof(url), "%s%s", MOCHI_PET_CELL_URL_BASE, expr);
            uint16_t w = 0, h = 0;
            if (!sprite_fetch_cell(url, pet_ink, pet_mask, PET_CELL_BYTES,
                                   &w, &h, &ms)) {
                ESP_LOGW(TAG, "pet cell fetch failed for '%s'", expr);
                return false;
            }
            if (w != PET_CELL_W || h != PET_CELL_H) {
                ESP_LOGW(TAG, "unexpected cell dims %ux%u (want %ux%u)",
                    w, h, (unsigned)PET_CELL_W, (unsigned)PET_CELL_H);
                return false;
            }
            if (cache_ok) {
                sprite_cache::store("pet-v1", ink_suffix,  pet_ink,  PET_CELL_BYTES);
                sprite_cache::store("pet-v1", mask_suffix, pet_mask, PET_CELL_BYTES);
            }
            cell_source = "fetch";
        }

        /* Scene fills the full 200×200 panel — same stride as
         * composite (25 bytes/row) so a single memcpy is the
         * natural blit. render_chrome() then white-fills the top
         * STATUS_BAR_H rows and paints status text + icons over
         * the top, hiding whatever the cell drew up there. */
        memcpy(composite, scene_fb, SCENE_BYTES);

        compositor::blit_two_plane(composite,
            MOCHI_EPD_WIDTH, MOCHI_EPD_HEIGHT,
            pet_ink, pet_mask,
            PET_CELL_W, PET_CELL_H, PET_DX, PET_DY);
        render_chrome();

        /* M11.5a — thought bubble on top of chrome. Centered above
         * the pet, between the TL/TR care icons. The hit rect is
         * cached in module-scope state so the touch classify path
         * can read it on the next tap. NULL thought = no bubble (the
         * usual case for action-tap renders and voice-phase
         * transitions). */
        if (thought) {
            thought_render(composite, MOCHI_EPD_WIDTH, MOCHI_EPD_HEIGHT,
                           thought, &s_thought_hit);
        }

        epd->EPD_LoadBuffer(composite, FB_LEN);
        if (full_refresh) {
            epd->EPD_Init();
            epd->EPD_Display();
            epd->EPD_DisplayPartBaseImage();
        } else {
            epd->EPD_Init_Partial();
            epd->EPD_DisplayPart();
        }
        ESP_LOGI(TAG, "rendered '%s' (%s %lu ms, %s refresh)",
            expr,
            cell_source ? cell_source : "?",
            (unsigned long)ms,
            full_refresh ? "full" : "partial");
        return true;
    };

    /* M11.4 — substrate-driven resting expression. Resolves the
     * pet's current sprite from project_mood + resolve_sprite over
     * the decayed dev pet, then renders that. Falls back to
     * "neutral" if the resolved sprite name doesn't have a cell on
     * the server (render_with_expression returns false on fetch
     * fail). The full M11.5 will replace this with a server-supplied
     * scene contract; for now this is enough to make the substrate
     * user-visible. */
    auto render_resting = [&]() -> const char * {
        int64_t now_ms = now_ms_wall();
        pet_event_t slice[12];
        size_t n = event_log_load_recent(slice, 12);
        pet_t decayed = current_pet_decayed(now_ms);
        mood_t m = project_mood(&decayed, slice, n, now_ms);
        age_t a = compute_age(decayed.born_at, now_ms);
        sprite_key_t sk = resolve_sprite(&decayed, m, a.stage, now_ms);

        /* M11.5a — compute the active thought for this render. The
         * suppression window keeps the same need from re-firing
         * locally before the substrate confirms; outside the
         * window, the predicate chain in thought_generate is the
         * only gate. */
        const pet_thought_t *thought_ptr = nullptr;
        if (now_ms >= s_thought_suppress_until_ms &&
            thought_generate(&decayed, now_ms, &s_active_thought)) {
            s_thought_active = true;
            thought_ptr = &s_active_thought;
        } else {
            s_thought_active = false;
            memset(&s_active_thought, 0, sizeof(s_active_thought));
        }

        const char *name = sprite_to_name(sk);
        if (!name || !render_with_expression(name, false, thought_ptr)) {
            render_with_expression("neutral", false, thought_ptr);
            return "neutral";
        }
        return name;
    };

    /*
     * Asleep render — used when the PWR-long-press gesture fires.
     * Same composition pipeline as the wake render, except:
     *   - pet expression is 'sleeping'
     *   - the status bar shows a single centred "Asleep — PWR to wake"
     *     line (no time, no battery, no pet name) so the persistent
     *     pixels are honest about the device being off
     *   - icons are NOT drawn (they're mute when asleep)
     *
     * This is captured as a lambda so it can call render_with_expression
     * (the network fetch path) and then re-paint the bar locally.
     * Full refresh because the screen will sit untouched for hours.
     */
    auto render_asleep = [&]() {
        /* Pack first, then cache, then network — same priority as
         * render_with_expression. Bundled 'sleeping' makes the
         * PWR-long-press gesture work offline. */
        uint16_t pw = 0, ph = 0;
        bool got_pet = pet_pack_load("sleeping", pet_ink, pet_mask,
                                     PET_CELL_BYTES, &pw, &ph) &&
                       pw == PET_CELL_W && ph == PET_CELL_H;
        size_t got_ink = 0, got_mask = 0;
        if (!got_pet) {
            got_pet =
                cache_ok &&
                sprite_cache::load("pet-v1", "sleeping_cell_ink",
                    pet_ink,  PET_CELL_BYTES, &got_ink) &&
                got_ink == PET_CELL_BYTES &&
                sprite_cache::load("pet-v1", "sleeping_cell_mask",
                    pet_mask, PET_CELL_BYTES, &got_mask) &&
                got_mask == PET_CELL_BYTES;
        }
        if (!got_pet) {
            char url[160];
            snprintf(url, sizeof(url), "%s%s", MOCHI_PET_CELL_URL_BASE, "sleeping");
            uint16_t w = 0, h = 0; uint32_t ms = 0;
            if (sprite_fetch_cell(url, pet_ink, pet_mask, PET_CELL_BYTES,
                    &w, &h, &ms) && w == PET_CELL_W && h == PET_CELL_H) {
                got_pet = true;
                if (cache_ok) {
                    sprite_cache::store("pet-v1", "sleeping_cell_ink",
                        pet_ink,  PET_CELL_BYTES);
                    sprite_cache::store("pet-v1", "sleeping_cell_mask",
                        pet_mask, PET_CELL_BYTES);
                }
            }
        }

        /* Scene + (sleeping) pet base. If the pet fetch failed (no
         * network, etc) we still render the asleep status — pet
         * just doesn't update from whatever was last shown. White-
         * fill the top STATUS_BAR_H rows AFTER the cell blit so the
         * asleep banner reads against paper, not whatever the cell
         * drew up there. */
        memcpy(composite, scene_fb, SCENE_BYTES);
        memset(composite, 0xFF, 25 * STATUS_BAR_H);
        if (got_pet) {
            compositor::blit_two_plane(composite,
                MOCHI_EPD_WIDTH, MOCHI_EPD_HEIGHT,
                pet_ink, pet_mask,
                PET_CELL_W, PET_CELL_H, PET_DX, PET_DY);
        }

        /* Asleep status bar: centred string, no icons. Reuse the
         * inline glyph blit pattern. */
        const char *line = "Asleep - PWR to wake";
        const int text_w = (int)strlen(line) * 8;
        int text_x = ((int)MOCHI_EPD_WIDTH - text_w) / 2;
        if (text_x < 0) text_x = 0;
        for (size_t i = 0; line[i]; i++) {
            const uint8_t *g = font8x8_glyph(line[i]);
            const int ox = text_x + (int)i * 8;
            for (int row = 0; row < 8; row++) {
                const uint8_t bits = g[row];
                for (int col = 0; col < 8; col++) {
                    if (!((bits >> col) & 1)) continue;
                    const int px = ox + col;
                    const int py = STATUS_TEXT_Y + row;
                    if (px < 0 || py < 0 ||
                        px >= (int)MOCHI_EPD_WIDTH ||
                        py >= (int)MOCHI_EPD_HEIGHT) continue;
                    const size_t off = (size_t)py * 25 + ((size_t)px >> 3);
                    composite[off] &= (uint8_t)~(1u << (7 - ((size_t)px & 7)));
                }
            }
        }
        /* 1-pixel divider, same as the awake bar. */
        const size_t row_off = (size_t)(STATUS_BAR_H - 1) * 25;
        memset(composite + row_off, 0x00, 25);

        epd->EPD_LoadBuffer(composite, FB_LEN);
        epd->EPD_Init();
        epd->EPD_Display();
        ESP_LOGI(TAG, "asleep render committed");
    };

    /* Initial render: pet on scene, neutral. Full refresh seeds the
     * partial-refresh "previous frame" so subsequent taps are
     * flicker-free. */
    if (!render_with_expression("neutral", true, nullptr)) {
        ESP_LOGE(TAG, "initial neutral render failed; halting");
        while (true) vTaskDelay(pdMS_TO_TICKS(60000));
    }

    /*
     * Touch zones — five care actions mapped onto the 200×200 panel:
     *
     *   ┌──────────┬───────────┬──────────┐
     *   │  feed    │           │   play   │
     *   │  (TL)    │           │   (TR)   │
     *   ├──────────┤  attention├──────────┤
     *   │          │  (centre) │          │
     *   │          │           │          │
     *   ├──────────┤           ├──────────┤
     *   │ comfort  │           │  cheer   │
     *   │  (BL)    │           │   (BR)   │
     *   └──────────┴───────────┴──────────┘
     *
     * Each zone fetches the matching expression cell, composites,
     * renders for ~5 s, then re-fetches 'neutral' for a resting pose.
     */
    /*
     * Touch zones map to the four icon rectangles (with a small
     * padding around each so off-by-a-few-pixels finger taps still
     * register) plus a centre zone for "attention". The status bar
     * itself is non-interactive; its y range is excluded.
     */
    /* Thought zone is checked first so a tap inside the bubble
     * always wins over any overlapping corner/center geometry.
     * Today the bubble sits between TL/TR icons with no overlap,
     * but future layouts (or a larger hit pad) might cross the
     * gap — fail closed: bubble first. */
    enum class Zone : uint8_t {
        Thought, CornerTL, CornerTR, CornerBL, CornerBR, Center, None
    };
    auto classify = [&](uint16_t x, uint16_t y) -> Zone {
        if (s_thought_active &&
            thought_hit_contains(&s_thought_hit, (int)x, (int)y)) {
            return Zone::Thought;
        }
        constexpr int PAD = 6;  /* enlarge each icon hit-rect by 6px on every side */
        for (int i = 0; i < 4; i++) {
            const int x0 = icon_pos_x[i] - PAD;
            const int y0 = icon_pos_y[i] - PAD;
            const int x1 = icon_pos_x[i] + (int)ICON_W + PAD;
            const int y1 = icon_pos_y[i] + (int)ICON_H + PAD;
            if ((int)x >= x0 && (int)x < x1 && (int)y >= y0 && (int)y < y1) {
                /* Icon index 0..3 maps to CornerTL..CornerBR; offset
                 * by +1 since Zone::Thought slid them down a slot. */
                return (Zone)(i + 1);
            }
        }
        /* Centre: a square in the middle of the panel, away from icons.
         * 100×100 centred at (100, 114) — slightly low to favour the
         * pet's body rather than the status bar. */
        if (x >= 50 && x < 150 && y >= 64 && y < 164) return Zone::Center;
        return Zone::None;
    };
    auto zone_to_expr = [](Zone z) -> const char * {
        switch (z) {
            case Zone::CornerTL: return "comforted";       /* heart icon */
            case Zone::CornerTR: return "cheerful_wave";   /* star icon */
            case Zone::CornerBL: return "eating";          /* bowl icon */
            case Zone::CornerBR: return "excited";         /* ball icon */
            case Zone::Center:   return "curious";         /* attention */
            /* Thought: resolved per-tap from s_active_thought; the
             * static zone→expr map can't know which action this
             * bubble carries. The dispatch path below uses
             * thought_action_to_expr instead. */
            case Zone::Thought:
            default:             return nullptr;
        }
    };

    /* Same map as zone_to_expr, but in event-kind terms. Touch zones
     * are care actions; the centre is just attention so it's a
     * generic "tapped". Used to feed M12's event log on each tap so
     * engagement.c has real input to project mood from.
     *
     * Zone::Thought returns EVENT_NONE here for the same reason as
     * zone_to_expr — the dispatch path reads the kind directly from
     * s_active_thought.action_event. */
    auto zone_to_event = [](Zone z) -> event_kind_t {
        switch (z) {
            case Zone::CornerTL: return EVENT_COMFORTED;
            case Zone::CornerTR: return EVENT_CHEERED;
            case Zone::CornerBL: return EVENT_FED;
            case Zone::CornerBR: return EVENT_PLAYED;
            case Zone::Center:   return EVENT_TAPPED;
            case Zone::Thought:
            default:             return EVENT_NONE;
        }
    };

    /* M13a — pull the real pet snapshot from the server. If it
     * succeeds, pet_sync internally caches the result and
     * pet_sync_get_snapshot below will return it. If it fails (no
     * pairing, network down, server 5xx) we fall back to the M11
     * hardcoded dev pet so the substrate keeps demoing.
     *
     * The pull must come AFTER time_sync_init so X-Pet-Id auth +
     * TLS cert validation work; both need real wall time. */
    {
        pet_t snapshot;
        pet_event_t evs[12];
        size_t n = 0;
        if (!pet_sync_pull_now(&snapshot, evs,
                               sizeof(evs)/sizeof(evs[0]), &n)) {
            ESP_LOGW(TAG, "state pull failed; using hardcoded dev pet");
            init_dev_pet(now_ms_wall());
        }
    }
    /* Spin up the push worker + periodic-resync task. Idempotent. */
    pet_sync_start();

    touch::init();
    int64_t last_event_us = 0;
    constexpr int64_t DEBOUNCE_US = 200 * 1000;

    /* Auto-trigger key portal if NVS has no OpenAI key. The user
     * landed here either by skipping the optional key field during
     * provisioning, or by an OTA from a build where it was set on a
     * different NVS layout. Either way, we'd rather drop them
     * straight into the recovery UX than make them discover the
     * triple-tap gesture by reading source. */
    {
        char probe[MOCHI_OPENAI_KEY_MAX + 1] = {};
        if (!openai_key_load(probe, sizeof(probe))) {
            ESP_LOGI(TAG, "no openai key on boot — opening key portal");
            key_portal::start(epd);
        }
        memset(probe, 0, sizeof(probe));
    }

    /*
     * Phase-driven expression for active voice sessions. The user
     * sees the pet's current expression and infers session state
     * from that — no chrome required. Mapping:
     *
     *   CONNECTING → "curious"        — "what's happening?"
     *   READY      → "comforted"      — calm, listening, between turns
     *   SPEAKING   → "cheerful_wave"  — animated, audio playing
     *   IDLE       → not rendered here; tap-to-stop branch already
     *                snaps to "neutral".
     *
     * Re-rendered only on transition (not every poll) so we don't
     * burn partial-refresh flicker on the e-paper.
     */
    auto phase_expr = [](voice::Phase p) -> const char * {
        switch (p) {
            case voice::Phase::Connecting: return "curious";
            case voice::Phase::Ready:      return "comforted";
            case voice::Phase::Speaking:   return "cheerful_wave";
            case voice::Phase::Idle:
            default:                       return nullptr;
        }
    };
    voice::Phase last_voice_phase = voice::Phase::Idle;

    /* M11.4 periodic refresh state. Re-resolve the resting sprite
     * once a minute when the device is otherwise quiet; only push a
     * new render to the panel if the resolved name actually changed
     * since the last frame, so we don't burn e-paper cycles on
     * unchanged content. */
    constexpr int64_t SUBSTRATE_REFRESH_US = 60LL * 1000 * 1000;
    int64_t last_substrate_us = esp_timer_get_time();
    char last_resting_expr[32] = "neutral";

    while (true) {
        touch::Event ev;
        bool got_touch = touch::wait_event(&ev, 1000);

        /* Sleep gesture takes priority over touch. The wait_event
         * 1-second timeout means we check this at least once per
         * second even with no taps. When the long-press fires we
         * claim it (so the watcher's fallback render doesn't race
         * us), render the rich asleep frame, then commit_sleep()
         * — never returns. */
        if (sleep_gesture::requested()) {
            sleep_gesture::mark_handled();
            render_asleep();
            sleep_gesture::commit_sleep();
        }

        /* Manual key-portal trigger: triple-tap PWR. Distinct from
         * the long-hold sleep gesture and the 10s factory-reset
         * gesture. Used to replace an already-set key. */
        if (sleep_gesture::triple_tap_consume() && !voice::is_active()) {
            ESP_LOGI(TAG, "triple-tap → opening key portal");
            key_portal::start(epd);
        }

        /* Drive the portal's idle / post-submit auto-stop. */
        key_portal::tick();

        /* While the portal is active any touch dismisses it and
         * forces a redraw of the pet via the normal flow on the
         * next iteration. Long-press for voice is suppressed so the
         * QR + key-entry surface stays intact. */
        if (key_portal::active()) {
            if (got_touch) {
                ESP_LOGI(TAG, "touch while portal active → dismissing");
                key_portal::stop();
                /* Fall through and let the standard render path
                 * draw the pet on the next tick. */
                render_with_expression("neutral", false, nullptr);
            }
            continue;
        }

        /*
         * OTA reboot gate. The background task signals reboot_ready()
         * after streaming a new image into the inactive slot. We hold
         * off rebooting until the device is genuinely idle: no active
         * voice session, no touch in the last 60s. That avoids
         * yanking the screen mid-tap or mid-conversation.
         *
         * Once we reboot, the bootloader picks the freshly-flipped
         * otadata slot and mark_valid_if_pending() above promotes it
         * on the next successful WiFi join. */
        if (ota_update::reboot_ready() && !voice::is_active()) {
            int64_t now_us = esp_timer_get_time();
            constexpr int64_t OTA_IDLE_GATE_US = 60LL * 1000 * 1000;
            if (now_us - last_event_us > OTA_IDLE_GATE_US) {
                ESP_LOGI(TAG, "OTA staged + device idle → rebooting to apply");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_restart();
            }
        }

        /* Voice auto-stop: the worker task can't safely call
         * voice::stop_session itself (would deadlock joining its own
         * task). Caps + remote disconnect set a flag we poll here. */
        if (voice::stop_requested()) {
            ESP_LOGI(TAG, "voice auto-stop requested → stopping");
            voice::stop_session();
            render_with_expression("neutral", false, nullptr);
            last_voice_phase = voice::Phase::Idle;
        }

        /* Phase-driven expression update. Runs on every loop tick
         * (1 Hz minimum from wait_event's timeout) so the pet's
         * expression follows the voice session lifecycle without
         * needing the data-channel callbacks to call back into the
         * main task. Only re-renders on transition. */
        {
            voice::Phase cur = voice::phase();
            if (cur != last_voice_phase) {
                const char *e = phase_expr(cur);
                if (e) {
                    ESP_LOGI(TAG, "voice phase → %d, render %s", (int)cur, e);
                    render_with_expression(e, false, nullptr);
                }
                last_voice_phase = cur;
            }
        }

        if (!got_touch) {
            /* Idle tick. Once per SUBSTRATE_REFRESH_US, re-project the
             * resting expression and re-evaluate the active thought.
             * Re-render only if either changed. Skip during voice
             * sessions (the phase machine owns the pet face there)
             * and while the key portal is up (its own screen is
             * current). */
            int64_t now_us = esp_timer_get_time();
            if (!voice::is_active() && !key_portal::active() &&
                (now_us - last_substrate_us) >= SUBSTRATE_REFRESH_US) {
                last_substrate_us = now_us;
                int64_t now_ms = now_ms_wall();
                pet_event_t slice[12];
                size_t n = event_log_load_recent(slice, 12);
                pet_t decayed = current_pet_decayed(now_ms);
                mood_t m = project_mood(&decayed, slice, n, now_ms);
                age_t a = compute_age(decayed.born_at, now_ms);
                sprite_key_t sk = resolve_sprite(&decayed, m, a.stage, now_ms);
                const char *name = sprite_to_name(sk);

                /* Re-evaluate the thought. A change of state (bubble
                 * appeared, disappeared, or its action_event swapped)
                 * triggers a re-render even if the resting sprite is
                 * unchanged — the kid shouldn't have to wait for the
                 * next sprite transition to see a fresh need. */
                pet_thought_t candidate = {};
                const bool gen_ok =
                    now_ms >= s_thought_suppress_until_ms &&
                    thought_generate(&decayed, now_ms, &candidate);
                const bool thought_changed =
                    gen_ok != s_thought_active ||
                    (gen_ok &&
                     (candidate.action_event != s_active_thought.action_event ||
                      candidate.action_kind  != s_active_thought.action_kind));

                const bool sprite_changed =
                    name && strcmp(name, last_resting_expr) != 0;

                if (sprite_changed || thought_changed) {
                    s_thought_active = gen_ok;
                    if (gen_ok) {
                        s_active_thought = candidate;
                    } else {
                        memset(&s_active_thought, 0, sizeof(s_active_thought));
                    }
                    const char *render_name = name ? name : last_resting_expr;
                    ESP_LOGI(TAG, "substrate refresh: %s → %s "
                                  "(stats h%u/f%u/e%u eng=%.2f) thought=%s",
                        last_resting_expr, render_name,
                        decayed.stats.happiness, decayed.stats.fullness,
                        decayed.stats.energy,
                        recent_engagement(slice, n, now_ms),
                        s_thought_active ? s_active_thought.line1 : "(none)");
                    if (render_with_expression(render_name, false,
                            s_thought_active ? &s_active_thought : nullptr)) {
                        snprintf(last_resting_expr, sizeof(last_resting_expr),
                                 "%s", render_name);
                    }
                }
            }
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_event_us < DEBOUNCE_US) continue;
        last_event_us = now_us;

        Zone z = classify(ev.x, ev.y);
        /* Thought-bubble taps resolve expr + kind dynamically from
         * the active thought's payload — the static zone→expr /
         * zone→event maps can't know which action this particular
         * bubble carries (today: SLEPT; tomorrow: FED, talk_seed,
         * etc. — see design/12-thought-bubble.md). */
        const pet_thought_t *tapped_thought =
            (z == Zone::Thought && s_thought_active) ? &s_active_thought : nullptr;

        /* Scene-pack zone hit-test (MPK1 build-time scene contract,
         * design/13/14). The pack itself decides what each tap means
         * via a typed action payload (event / nav_scene / nav_relative
         * / talk_seed) — the firmware no longer keeps a name→action
         * strcmp ladder here. Format=0 packs synthesise the same
         * payload from the static name table inside scene_pack.c,
         * so this code path is uniform across formats.
         *
         * Skipped while a thought bubble is up (its tap target owns
         * the screen) or while the tap landed in the status-bar
         * stripe (no zones up there).
         *
         * Forgiving snap: ZONE_SLOP_PX widens each rect by 16 px
         * before we give up and fall through to corner-quadrant
         * dispatch. The pet-body case is checked first so a pat near
         * a zone doesn't get hijacked. */
        constexpr int ZONE_SLOP_PX = 16;
        const bool tapped_pet =
            (int)ev.x >= PET_DX && (int)ev.x <  PET_DX + (int)PET_CELL_W &&
            (int)ev.y >= PET_DY && (int)ev.y <  PET_DY + (int)PET_CELL_H;

        scene_pack_action_t scene_act = {};
        bool scene_hit = false;
        if (!tapped_thought && (int)ev.y >= STATUS_BAR_H) {
            const int slop = tapped_pet ? 0 : ZONE_SLOP_PX;
            scene_hit = scene_pack_action_at(
                (int16_t)ev.x, (int16_t)ev.y, slop, &scene_act);
        }

        const char *expr = tapped_thought
            ? thought_action_to_expr(tapped_thought)
            : zone_to_expr(z);
        ESP_LOGI(TAG, "touch (%u,%u) zone=%u scene_act=%s expr=%s",
            (unsigned)ev.x, (unsigned)ev.y, (unsigned)z,
            scene_hit
                ? (scene_act.kind == MPK_ACTION_NAV_SCENE    ? "nav_scene"
                 : scene_act.kind == MPK_ACTION_NAV_RELATIVE ? "nav_relative"
                 : scene_act.kind == MPK_ACTION_TALK_SEED    ? "talk_seed"
                 : scene_act.kind == MPK_ACTION_EVENT        ? "event"
                 :                                             "none")
                : "(none)",
            expr ? expr : "(gutter)");

        /* Scene-navigation actions short-circuit the rest of the
         * touch pipeline: re-blit the scene cell, force a full
         * refresh (partial would leave residue from the previous
         * scene), and bail before the per-tap care pipeline runs. */
        if (scene_hit && scene_act.kind == MPK_ACTION_NAV_RELATIVE) {
            int delta = scene_act.data;
            uint16_t to = scene_pack_advance(delta);
            ESP_LOGI(TAG, "scene nav rel %+d → idx=%u", delta, (unsigned)to);
            scene_pack_blit_current(scene_fb, SCENE_W, SCENE_H);
            render_with_expression("neutral", true, nullptr);
            continue;
        }
        if (scene_hit && scene_act.kind == MPK_ACTION_NAV_SCENE) {
            uint16_t to = scene_pack_set((uint16_t)scene_act.data);
            ESP_LOGI(TAG, "scene nav abs → idx=%u", (unsigned)to);
            scene_pack_blit_current(scene_fb, SCENE_W, SCENE_H);
            render_with_expression("neutral", true, nullptr);
            continue;
        }

        /* talk_seed zones — two paths depending on whether the
         * voice session is up:
         *
         *   voice active:  inject the seed as a user-text message
         *                  on the data channel. Mochi reads it and
         *                  speaks back. The intent-router work
         *                  becomes the model's job.
         *
         *   voice idle:    pop a transient thought bubble carrying
         *                  the seed text (truncated to fit the
         *                  bubble's ~22-character budget). No care
         *                  event lands; the bubble just decorates
         *                  the next render and stays through the
         *                  THOUGHT_SUPPRESS_MS window so the kid
         *                  has time to read it.
         *
         * The seed pointer is borrowed from the embedded pack and
         * not NUL-terminated; copy it into a local buffer first. */
        static char  s_seed_buf[256];
        static char  s_seed_line1[24];
        static char  s_seed_line2[24];
        static pet_thought_t s_seed_thought;
        const pet_thought_t *seed_thought_ptr = nullptr;
        if (scene_hit && scene_act.kind == MPK_ACTION_TALK_SEED) {
            const size_t n = scene_act.seed_len < sizeof(s_seed_buf) - 1
                ? scene_act.seed_len
                : sizeof(s_seed_buf) - 1;
            if (scene_act.seed_text && n > 0) {
                memcpy(s_seed_buf, scene_act.seed_text, n);
            }
            s_seed_buf[n] = '\0';
            ESP_LOGI(TAG, "talk_seed: '%s' (voice %s)",
                s_seed_buf, voice::is_active() ? "active" : "idle");

            if (voice::is_active()) {
                /* Inject as a user message; the model will respond
                 * via its usual audio + delta path. */
                voice::send_text(s_seed_buf);
                expr = "thinking";
            } else {
                /* Word-wrap the seed across the two-line bubble.
                 * Bubble interior is ~92 px = ~11 scale-1 glyphs per
                 * line. Try to break on a space near col 11; spill
                 * to line 2; ellipsise overflow. This is intentionally
                 * naive — talk_seed strings are short evocations
                 * authored to fit. */
                const int kPerLine = 11;
                const int total = (int)strlen(s_seed_buf);
                int br = (total > kPerLine) ? kPerLine : total;
                if (total > kPerLine) {
                    for (int i = kPerLine; i > kPerLine - 5 && i > 0; i--) {
                        if (s_seed_buf[i] == ' ') { br = i; break; }
                    }
                }
                snprintf(s_seed_line1, sizeof(s_seed_line1),
                         "%.*s", br, s_seed_buf);
                int after = br + (s_seed_buf[br] == ' ' ? 1 : 0);
                int rem = total - after;
                if (rem <= 0) {
                    s_seed_line2[0] = '\0';
                } else if (rem <= kPerLine) {
                    snprintf(s_seed_line2, sizeof(s_seed_line2),
                             "%s", s_seed_buf + after);
                } else {
                    /* Truncate with a trailing ellipsis (single dot
                     * to save a column). */
                    snprintf(s_seed_line2, sizeof(s_seed_line2),
                             "%.*s.", kPerLine - 1, s_seed_buf + after);
                }
                memset(&s_seed_thought, 0, sizeof(s_seed_thought));
                s_seed_thought.action_kind = THOUGHT_ACTION_NONE;
                s_seed_thought.line1       = s_seed_line1;
                s_seed_thought.line2       = s_seed_line2;
                seed_thought_ptr           = &s_seed_thought;
                expr = "thinking";
            }
        }

        /* When the current scene has authored zones, the corner-
         * quadrant fallback is suppressed: a tap that misses every
         * zone (after the snap fallback above) resolves to either
         * "comforted" (tap landed on the pet body — a pat) or
         * "curious" (empty scene background). */
        const bool zoned = scene_pack_current_has_zones();
        if (zoned && !scene_hit && !tapped_thought) {
            expr = tapped_pet ? "comforted" : "curious";
        }

        /* event-kind zones: derive expr from the pack-supplied
         * event_kind_t. Falls back to "curious" if the kind doesn't
         * have a registered expression. */
        if (scene_hit && scene_act.kind == MPK_ACTION_EVENT) {
            const char *e = event_kind_to_expr((event_kind_t)scene_act.data);
            expr = e ? e : "curious";
        }
        if (!expr) continue;

        /* M11/M12: persist the touch as an event. Kind resolution
         * priority:
         *   1. Thought-bubble taps carry their own action_event.
         *   2. Scene-zone EVENT actions read the kind from the pack.
         *   3. Else fall through to the corner-quadrant zone map. */
        event_kind_t kind;
        if (tapped_thought) {
            kind = tapped_thought->action_event;
        } else if (scene_hit && scene_act.kind == MPK_ACTION_EVENT) {
            kind = (event_kind_t)scene_act.data;
        } else if (scene_hit && scene_act.kind == MPK_ACTION_TALK_SEED) {
            /* talk_seed zones still log a TAPPED event so the M12
             * engagement projection picks up the interaction. */
            kind = EVENT_TAPPED;
        } else if (zoned) {
            /* Zoned scene + tap missed all zones. Pet body =
             * comfort (a pat); empty scene = curious tap. */
            kind = tapped_pet ? EVENT_COMFORTED : EVENT_TAPPED;
        } else {
            kind = zone_to_event(z);
        }
        int64_t now_ms = now_ms_wall();
        if (kind != EVENT_NONE) {
            /* M12 local log — kept as the source for engagement
             * projection until the next /api/state pull lands.
             * After M13 the projection draws from the union of the
             * server slice + local-not-yet-pushed events; for now
             * the local log is the simpler input. */
            event_log_append(kind, now_ms);
            /* M13b — enqueue for /api/mutate POST. The push worker
             * drains this asynchronously so touch handling stays
             * responsive even on a slow TLS handshake. The mutate
             * response refreshes pet_sync's snapshot, so the next
             * projection sees server-canonical stats. */
            pet_sync_enqueue(kind, now_ms);
            /* Bump the in-memory snapshot's last_interaction_at
             * immediately so loneliness resets without waiting for
             * the round-trip. The mutate response will overwrite
             * with the authoritative value shortly. */
            pet_sync_touch(now_ms);
            /* Keep the dev-pet fallback in sync too, so projection
             * is consistent if the server sync drops out mid-session. */
            s_dev_pet.last_interaction_at = now_ms;
        }
        {
            pet_event_t slice[12];
            size_t n = event_log_load_recent(slice, 12);
            pet_t decayed = current_pet_decayed(now_ms);
            mood_t m = project_mood(&decayed, slice, n, now_ms);
            age_t a = compute_age(decayed.born_at, now_ms);
            sprite_key_t sk = resolve_sprite(&decayed, m, a.stage, now_ms);
            ESP_LOGI(TAG,
                "pet_state: mood=%s sprite=%s age=%s(%lld) "
                "stats=h%u/f%u/e%u eng=%.2f n_events=%u",
                mood_to_name(m), sprite_to_name(sk),
                age_stage_to_name(a.stage), (long long)a.days,
                decayed.stats.happiness, decayed.stats.fullness,
                decayed.stats.energy,
                recent_engagement(slice, n, now_ms),
                (unsigned)n);
        }

        /* Voice trigger: long-press of the centre-attention zone
         * starts a session; tap-anywhere while a session is running
         * stops it. Same gesture as design/07-voice-architecture.md
         * specifies. The hold check polls the FT6336 at 50 ms cadence
         * for up to 800 ms — if the finger stays in the centre zone
         * the whole time, treat as long-press; otherwise fall through
         * to the existing tap-to-cycle-expression behaviour. */
        if (voice::is_active()) {
            /* Active-session tap rules:
             *   centre tap → text talk-back (debug path: send a fixed
             *                user message + response.create over the
             *                data channel). Validates multi-turn
             *                lifecycle without committing to mic
             *                capture (M9.f.2). The pet's expression
             *                will hop SPEAKING → READY automatically
             *                via the data-channel event handler.
             *   anywhere else → stop session.
             */
            if (z == Zone::Center) {
                ESP_LOGI(TAG, "voice active — centre tap → text talk-back");
                static const char *DEBUG_TEXT =
                    "Are you still there? Say a quick goodbye and stop.";
                if (!voice::send_text(DEBUG_TEXT)) {
                    ESP_LOGW(TAG, "send_text failed; falling through");
                }
                continue;
            }
            ESP_LOGI(TAG, "voice active — non-centre tap → stop session");
            voice::stop_session();
            /* Render neutral immediately so the user sees the
             * "session over" state. */
            render_with_expression("neutral", false, nullptr);
            last_voice_phase = voice::Phase::Idle;
            continue;
        } else if (z == Zone::Center && voice::is_ready()) {
            constexpr int HOLD_POLL_MS = 50;
            constexpr int HOLD_TARGET_MS = 800;
            int held_ms = 0;
            bool released = false;
            for (; held_ms < HOLD_TARGET_MS; held_ms += HOLD_POLL_MS) {
                touch::Event hold_ev;
                if (!touch::current_point(&hold_ev)) {
                    released = true;
                    break;
                }
                if (classify(hold_ev.x, hold_ev.y) != Zone::Center) {
                    /* finger drifted off-zone — treat as a tap */
                    released = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(HOLD_POLL_MS));
            }
            if (!released) {
                ESP_LOGI(TAG, "long-press → start voice session");
                /* Visual affordance: render 'curious' immediately so the
                 * user knows their long-press registered. Without this,
                 * the user sees nothing for the ~2.4 s mint blocking
                 * call and tends to keep holding longer to be certain.
                 * Once the session enters CONNECTING, phase_expr maps
                 * it to 'curious' too, so the next tick of the touch
                 * loop won't re-render. */
                render_with_expression("curious", false, nullptr);
                last_voice_phase = voice::Phase::Connecting;
                if (voice::start_session() == 0) {
                    /* Log the voice turn as a "talked" event — the
                     * strongest engagement signal in shared/engagement.ts.
                     * Single-event-per-session for now; M13's bidi
                     * sync may want finer-grained per-turn entries. */
                    int64_t talked_at = now_ms_wall();
                    event_log_append(EVENT_TALKED, talked_at);
                    pet_sync_enqueue(EVENT_TALKED, talked_at);
                    pet_sync_touch(talked_at);
                    s_dev_pet.last_interaction_at = talked_at;
                    /* Drain pending touch events that piled up while
                     * start_session was blocked on the mint round-trip
                     * (~2.4 s). Without this, the still-held finger
                     * generates a "stale" touch on the next wait_event,
                     * which trips the is_active() branch above and
                     * stops the session 1 ms after starting it. */
                    touch::Event drain;
                    while (touch::wait_event(&drain, 0)) { /* drain */ }
                    /* Then wait for finger-up so any in-progress hold
                     * doesn't trigger an immediate stop on lift-off. */
                    constexpr int LIFT_TIMEOUT_MS = 5000;
                    constexpr int LIFT_POLL_MS = 50;
                    int lifted_ms = 0;
                    while (lifted_ms < LIFT_TIMEOUT_MS) {
                        touch::Event probe;
                        if (!touch::current_point(&probe)) break;
                        vTaskDelay(pdMS_TO_TICKS(LIFT_POLL_MS));
                        lifted_ms += LIFT_POLL_MS;
                    }
                    /* Drain again — the lift-off itself can leave
                     * one final ISR marker on the queue. */
                    while (touch::wait_event(&drain, 0)) { /* drain */ }
                    continue;
                }
                ESP_LOGW(TAG, "voice start failed; falling through to tap UX");
            }
        }

        /* Thought-bubble tap: suppress further bubble renders for
         * THOUGHT_SUPPRESS_MS so the same need can't re-surface
         * locally before the substrate confirms. Clear the active
         * thought so the subsequent post-hold render_resting() sees
         * a clean slate (it will re-evaluate the predicate against
         * the updated substrate snapshot). */
        if (z == Zone::Thought) {
            s_thought_suppress_until_ms = now_ms + THOUGHT_SUPPRESS_MS;
            s_thought_active = false;
            memset(&s_active_thought, 0, sizeof(s_active_thought));
        }

        /* Render the action expression via partial refresh — fast
         * cadence, no flicker. The bubble is deliberately NOT drawn
         * on action-tap renders — the kid just acted on it, so the
         * 5 s response hold should be uncluttered. The next
         * render_resting() (after the hold) will recompute the
         * thought from fresh state.
         *
         * Exception: a talk_seed tap with voice idle hands a
         * transient bubble (seed_thought_ptr) so the kid can read
         * what mochi would have heard. Suppression for the next
         * substrate refresh keeps the SLEEPY bubble from re-firing
         * over it. */
        if (!render_with_expression(expr, false, seed_thought_ptr)) continue;
        if (seed_thought_ptr) {
            s_thought_suppress_until_ms = now_ms_wall() + THOUGHT_SUPPRESS_MS;
        }

        /* Hold the expression for ~5 s. We block here rather than
         * scheduling a timer so the next touch event sees a quiet
         * pet, not a half-finished transition. */
        vTaskDelay(pdMS_TO_TICKS(RESTING_AFTER_TAP_MS));

        /* Settle back to the substrate's current resting expression
         * — driven by project_mood over the dev pet's decayed stats
         * + recent events. Falls back to "neutral" inside
         * render_resting if the projected sprite isn't fetchable.
         * Cache the chosen name so the periodic-refresh path knows
         * what's currently on the panel. */
        const char *resting = render_resting();
        snprintf(last_resting_expr, sizeof(last_resting_expr),
                 "%s", resting ? resting : "neutral");
        last_substrate_us = esp_timer_get_time();
    }
}
