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
#include <atomic>

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
#include "dev_menu.h"
#include "lvgl_port.h"
#include "nvs_creds.h"
#include "wifi_prov.h"
#include "wifi_sta.h"
#include "sprite_fetch.h"
#include "pack_cache.h"
#include "touch.h"
#include "rtc.h"
#include "shtc3.h"
#include "pair_creds.h"
#include "openai_key.h"
#include "device_pair.h"
#include "factory_reset.h"
#include "compositor.h"
#include "ui_dialog.h"
#include "font8x8.h"
#include "battery.h"
#include "sleep_gesture.h"
#include "voice.h"
#include "voice/voice_peer.h"   /* voice_peer_get_session_stats (design/18 ph3b) */
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
#include "consolidate.h"
#include "model_prefs.h"
}
#include "pet_sync.h"
#include "device_diag.h"

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

/* Scene-navigation e-ink refresh policy (design/17). A scene swap is a
 * whole-screen change, so a partial refresh leaves residue from the
 * previous scene; a full refresh is clean but ~1 s and flashes. We use
 * a hybrid: partial on each navigate (fast), with a clean full refresh
 * every Nth to clear the ghosting partial accumulates. Tune against the
 * panel's ghosting tolerance — 1 = always full (pre-design/17 behaviour). */
#define SCENE_NAV_FULL_EVERY 4
#define MOCHI_PET_CELL_URL_BASE "https://mochi.val.run/devsprite/cell/pet-v1/"

/* Travel (design/17): a place's device pack fetched into PSRAM and held
 * live (scene_pack points into it). One reused buffer — travel is
 * sequential and fetch→swap→blit is atomic on the render thread. */
#define TRAVEL_PACK_BYTES (320u * 1024u)
#define MOCHI_BASE_URL    "https://mochi.val.run"

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

/* Wake-from-deepsleep deferred-mutate latch. Set at boot when we
 * detect ESP_RST_DEEPSLEEP, drained by the main loop's "WiFi just
 * came online" branch. Bypasses the early-boot crash where
 * pet_sync_enqueue → push worker → lwip getaddrinfo asserts because
 * the lwip tcpip task hasn't been initialised yet. */
static int64_t            s_pending_woke_at = 0;

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

/* BOOT-press watcher: 25 ms cadence falling-edge detection. BOOT is
 * the dedicated voice start/stop button now (the dev_menu wheel
 * moved to PWR double-tap). Latches s_boot_press_pending so the
 * (slow) main loop can consume the event on its next tick — without
 * this, presses that fit between two main-loop iterations were lost
 * and read as "BOOT is unresponsive". */
static std::atomic<bool> s_boot_press_pending{false};
static void boot_watcher_task(void *) {
    int prev_level = gpio_get_level((gpio_num_t)MOCHI_BOOT_BUTTON_GPIO);
    int debounce = 0;
    constexpr int POLL_MS = 25;
    constexpr int DEBOUNCE_TICKS = 2;   /* 50 ms */
    while (true) {
        if (debounce > 0) debounce--;
        const int level = gpio_get_level((gpio_num_t)MOCHI_BOOT_BUTTON_GPIO);
        if (level == 0 && prev_level == 1 && debounce == 0) {
            debounce = DEBOUNCE_TICKS;
            s_boot_press_pending.store(true, std::memory_order_release);
            ESP_LOGI(TAG, "BOOT press latched");
        }
        prev_level = level;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
static bool boot_press_consume(void) {
    return s_boot_press_pending.exchange(false, std::memory_order_acq_rel);
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

/* ─── Non-blocking connectivity (design/21-nonblocking-wifi.md) ────
 *
 * On the paired warm-boot path the pet renders from the embedded packs
 * before WiFi is touched; this worker then brings the network up and
 * runs everything that needs it (SNTP, OTA, ETag cache refresh, state
 * pull, cold-cache care-icon fetch) off the boot critical path. It
 * signals the main loop through flags rather than rendering itself, so
 * the main loop remains the single owner of the panel. */
enum class NetPhase : uint8_t { Connecting, Online, Offline };
static volatile NetPhase s_net_phase = NetPhase::Connecting;

/* Last-known IP + SSID, populated by net_worker on a successful join.
 * Read-only outside the worker; kept module-scope so the dev_menu
 * Diagnostics screen can surface them without threading them through
 * the touch loop. Empty before the first join. */
static char s_net_ip[16] = {};
static char s_net_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};

/* Set true when the worker has produced something worth re-rendering
 * (a fresh state snapshot, invalidated cache, or freshly-fetched care
 * icons). The main loop consumes + clears it. */
static volatile bool s_net_render_dirty = false;

/* What the worker borrows from app_main scope. app_main never returns,
 * so a stack-allocated instance there outlives the worker. */
struct NetCtx {
    uint8_t *icon_ink[4];
    uint8_t *icon_mask[4];
    bool      cache_ok;
    bool      icons_cached;   /* main already loaded all 4 from cache */
};

static void net_worker(void *arg) {
    NetCtx *ctx = (NetCtx *)arg;

    char ip_str[16] = {};
    char joined_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};
    wifi_sta::init_stack();
    if (!wifi_sta::connect_any(ip_str, sizeof(ip_str),
                               joined_ssid, sizeof(joined_ssid))) {
        ESP_LOGW(TAG, "net_worker: no stored network reachable — offline");
        device_diag_event(DIAG_WARN, "wifi", "offline", nullptr);
        s_net_phase = NetPhase::Offline;
        s_net_render_dirty = true;   /* repaint chrome with offline glyph */
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "net_worker: online IP=%s ssid='%s'", ip_str, joined_ssid);
    /* Publish IP + SSID so the dev_menu Diagnostics screen can read
     * them. Single writer (this task), readers are non-mutating. */
    snprintf(s_net_ip,   sizeof(s_net_ip),   "%s", ip_str);
    snprintf(s_net_ssid, sizeof(s_net_ssid), "%s", joined_ssid);
    {
        char c[120];
        snprintf(c, sizeof(c), "{\"ssid\":\"%s\",\"ip\":\"%s\"}",
            joined_ssid, ip_str);
        device_diag_event(DIAG_INFO, "wifi", "joined", c);
    }
    device_diag_flush();
    s_net_phase = NetPhase::Online;
    s_net_render_dirty = true;   /* repaint chrome with online glyph */

    /* OTA: a successful join is the health signal that promotes a
     * freshly-installed pending image and starts the update poller. */
    ota_update::mark_valid_if_pending();
    ESP_LOGI(TAG, "running firmware version: %s", ota_update::current_version());
    ota_update::start_background_task(MOCHI_OTA_MANIFEST_URL);

    /* Wall-clock sync for substrate timestamps. */
    time_sync_init();

    /* Upgrade the scene + pet packs from the server now that lwip is
     * up. Boot path opened the embedded baseline so the first frame
     * could render offline; pack_cache_active here probes the ETag
     * and refreshes only if the server has a newer pack. On change
     * the active mpk_t is replaced and we flag the renderer dirty so
     * the next tick re-blits. */
    if (scene_pack_init()) {
        s_net_render_dirty = true;
    }
    if (pet_pack_init()) {
        s_net_render_dirty = true;
    }

    /* Per-sheet ETag refresh: drop cache where server artwork changed
     * so the next render refetches. */
    if (ctx->cache_ok) {
        struct { const char *sheet; const char *url; } probes[] = {
            { "pet-v1",   "https://mochi.val.run/devsprite/cell/pet-v1/neutral" },
            { "ui-v1",    "https://mochi.val.run/devsprite/cell/ui-v1/heart"    },
            { "scene-v1", "https://mochi.val.run/devsprite/scene-v1/day"        },
        };
        for (auto &p : probes) {
            char remote[40] = {}, local[40] = {};
            if (!sprite_fetch_head_etag(p.url, remote, sizeof(remote))) {
                ESP_LOGW(TAG, "etag probe failed for '%s'", p.sheet);
                continue;
            }
            sprite_cache::load_etag(p.sheet, local, sizeof(local));
            if (strcmp(remote, local) != 0) {
                ESP_LOGI(TAG, "etag '%s' changed — invalidating", p.sheet);
                sprite_cache::invalidate_sheet(p.sheet);
                sprite_cache::store_etag(p.sheet, remote);
                s_net_render_dirty = true;
            }
        }
    }

    /* Cold-cache care icons: fetch native, downsample, cache, and fill
     * the buffers the chrome reads. Skipped when main already loaded
     * all four from cache (the warm case). */
    if (!ctx->icons_cached) {
        uint8_t *ni = (uint8_t *)heap_caps_malloc(UI_CELL_NATIVE_BYTES, MALLOC_CAP_SPIRAM);
        uint8_t *nm = (uint8_t *)heap_caps_malloc(UI_CELL_NATIVE_BYTES, MALLOC_CAP_SPIRAM);
        char suffix[24];
        snprintf(suffix, sizeof(suffix), "_icon_%ux%u",
            (unsigned)ICON_W, (unsigned)ICON_H);
        for (int i = 0; ni && nm && i < 4; i++) {
            char url[160];
            snprintf(url, sizeof(url), "%s%s",
                MOCHI_UI_CELL_URL_BASE, CARE_ICON_KEYS[i]);
            uint16_t w = 0, h = 0; uint32_t ms = 0;
            if (!sprite_fetch_cell(url, ni, nm, UI_CELL_NATIVE_BYTES,
                                   &w, &h, &ms) ||
                w != UI_CELL_NATIVE_W || h != UI_CELL_NATIVE_H) {
                ESP_LOGW(TAG, "icon '%s' fetch failed", CARE_ICON_KEYS[i]);
                continue;
            }
            memset(ctx->icon_ink[i],  0xFF, ICON_BYTES);
            memset(ctx->icon_mask[i], 0xFF, ICON_BYTES);
            compositor::downsample_plane(ctx->icon_ink[i],  ICON_W, ICON_H,
                ni, UI_CELL_NATIVE_W, UI_CELL_NATIVE_H);
            compositor::downsample_plane(ctx->icon_mask[i], ICON_W, ICON_H,
                nm, UI_CELL_NATIVE_W, UI_CELL_NATIVE_H);
            if (ctx->cache_ok) {
                char ink_s[40], mask_s[40];
                snprintf(ink_s,  sizeof(ink_s),  "%s%s_ink",
                    CARE_ICON_KEYS[i], suffix);
                snprintf(mask_s, sizeof(mask_s), "%s%s_mask",
                    CARE_ICON_KEYS[i], suffix);
                sprite_cache::store("ui-v1", ink_s,  ctx->icon_ink[i],  ICON_BYTES);
                sprite_cache::store("ui-v1", mask_s, ctx->icon_mask[i], ICON_BYTES);
            }
            ESP_LOGI(TAG, "icon '%s' fetched + cached", CARE_ICON_KEYS[i]);
            s_net_render_dirty = true;
        }
        free(ni);
        free(nm);
    }

    /* Authoritative state pull. pet_sync caches the snapshot
     * internally, so current_pet_decayed picks it up on the next
     * render once we flag dirty. */
    {
        pet_t snap; pet_event_t evs[12]; size_t n = 0;
        if (pet_sync_pull_now(&snap, evs, sizeof(evs)/sizeof(evs[0]), &n)) {
            s_net_render_dirty = true;
        } else {
            ESP_LOGW(TAG, "state pull failed; keeping dev-pet projection");
        }
    }

    /* Warm the voice persona+tools cache so the first long-press pays
     * no fetch on the session-start path (design/23). Best-effort. */
    voice::prefetch_config();

    vTaskDelete(nullptr);
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "—— mochi M3: boot ——");
    log_chip_info();

    /* Over-the-air diagnostics (design/18): captures the boot record
     * (reset reason / heap) up front; flushed to substrate once WiFi +
     * pairing are up so a field device is debuggable without serial. */
    device_diag_init();

    led_init();
    boot_button_init();
    /* Spawn the BOOT-press watcher on the same core as Wi-Fi (it's a
     * trivial GPIO poll, so noise on either core is negligible). The
     * latch is consumed by the main loop's voice start/stop block. */
    xTaskCreatePinnedToCore(boot_watcher_task, "boot_btn",
        2048, nullptr, 2, nullptr, 1);
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
    /* NVS up early (idempotent) so the splash can show the paired pet's
     * lonely expression when the splash pack carries a pet zone. */
    nvs_creds_init();
    struct mochi_pair_creds boot_pair = {};
    const bool boot_paired = pair_creds_load(&boot_pair);
    epd_ui::render_boot_splash(epd, "Mochi", ota_update::current_version(),
                               boot_paired);
    epd->EPD_Init();
    epd->EPD_Display();
    epd->EPD_DisplayPartBaseImage();

    /* LVGL port — bridges the Waveshare e-paper + FT6336 touch into
     * LVGL widgets, used by the dev_menu wheel screens (Info /
     * Actions / WifiModal). Live pet rendering still uses the bare
     * epd_ui draw helpers; LVGL is opt-in per-screen. */
    lvgl_port_init(epd);

    /* Dev-menu wheel: PWR-double-tap enters Info; subsequent PWR taps
     * cycle Info → Actions → Info; 60 s inactivity returns to live. */
    dev_menu::init(epd);

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

    /*
     * Boot-path split (design/21-nonblocking-wifi.md):
     *
     *   unpaired → WAITING-FOR-PAIRING. There's no pet to show yet, so
     *              WiFi is on the critical path by necessity: connect,
     *              run the pair flow, reboot into the paired path.
     *   paired   → WARM BOOT. Render the pet from the embedded packs
     *              first; net_worker brings WiFi + sync up afterwards.
     *              The rest of app_main below is this path.
     */
    struct mochi_pair_creds pair = {};
    bool have_pair = pair_creds_load(&pair);

    if (!have_pair) {
        ESP_LOGI(TAG, "creds present, no pairing → WAITING-FOR-PAIRING");
        wifi_sta::init_stack();
        char ip_str[16] = {};
        char joined_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};
        if (!wifi_sta::connect_any(ip_str, sizeof(ip_str),
                                   joined_ssid, sizeof(joined_ssid))) {
            /* No stored network reachable. Persist prov_on_boot +
             * reboot into a clean SoftAP provisioning boot rather than
             * an in-process STA→AP swap (ESP-IDF v5.3 hangs on that —
             * see project_eink_wifi_handover). Creds are preserved. */
            ESP_LOGW(TAG, "no network reachable while unpaired; "
                          "prov_on_boot + reboot");
            nvs_creds_set_prov_on_boot(true);
            epd_ui::render_prov_failed(epd);
            epd->EPD_Init_Partial();
            epd->EPD_DisplayPart();
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
        ESP_LOGI(TAG, "online (unpaired); IP=%s ssid='%s'",
            ip_str, joined_ssid);
        ota_update::mark_valid_if_pending();
        time_sync_init();   /* pair-check TLS needs wall time */

        device_pair::InitResult init = {};
        if (!device_pair::request_code(&init)) {
            epd_ui::render_pair_failed(epd);
            epd->EPD_Init_Partial();
            epd->EPD_DisplayPart();
            ESP_LOGE(TAG, "pair-init failed; hold PWR+BOOT 10s to reset "
                          "or power-cycle to retry");
            while (true) vTaskDelay(pdMS_TO_TICKS(60000));
        }
        epd_ui::render_pair_prompt(epd, init.code);
        epd->EPD_Init_Partial();
        epd->EPD_DisplayPart();

        /* Block, polling every 5 s, up to the server's 10-min TTL. */
        if (!device_pair::wait_for_user(&init, &pair, 10 * 60 * 1000)) {
            epd_ui::render_pair_failed(epd);
            epd->EPD_Init_Partial();
            epd->EPD_DisplayPart();
            ESP_LOGW(TAG, "pair-check did not complete; hold PWR+BOOT 10s "
                          "to reset or power-cycle to retry");
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
        device_diag_event(DIAG_INFO, "pair", "paired", nullptr);
        device_diag_flush();   /* push before the post-pair reboot */
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }

    /* === Paired warm boot. The pet renders from the embedded packs
     * before any network is touched; net_worker (spawned after the
     * first frame, below) handles connect + SNTP + OTA + ETag refresh
     * + state pull off the critical path. === */
    ESP_LOGI(TAG, "paired to '%s' (pet_id=%s) — warm boot",
        pair.pet_name, pair.pet_id);

    /*
     * Local (no-network) device init: RTC, environmental sensor, codec,
     * battery sense, LittleFS cache + event log. None of these gate on
     * WiFi, so they stay on the boot critical path before the first
     * frame. Anything that needs the network is in net_worker.
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

    if (!voice::init()) {
        ESP_LOGW(TAG, "voice init failed; continuing boot");
    }

    /* Battery sense (ADC1 ch3 via the 1:2 divider on VBAT_PWR). One
     * read for the boot log; render_chrome() polls per frame. The
     * 10-sample LiPo-presence diagnostic was retired from the boot
     * path — it was 2 s of pure logging on the critical path. */
    if (battery_init()) {
        uint16_t mv = 0; uint8_t pct = 0;
        if (battery_read(&mv, &pct)) {
            ESP_LOGI(TAG, "battery: %u mV (%u%%)", (unsigned)mv, (unsigned)pct);
        }
    }

    bool cache_ok = sprite_cache::init();
    if (cache_ok) {
        /* Dump (and consume) the previous boot's voice session log,
         * then bring up the on-device event log. */
        voice_diag_dump_last();
        event_log_init();
    } else {
        ESP_LOGW(TAG, "sprite cache disabled; falling back to per-fetch");
    }
    /* Per-sheet ETag refresh is a network op — moved to net_worker so
     * it can't gate the first frame. */

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
    /* (Pre-v0.1.7 the device kept a 320 KB travel_pack PSRAM buffer
     * here that the travel block fetched into directly. v0.1.7 routes
     * travel through pack_cache_active_geom() which owns its own
     * PSRAM — saving the always-allocated buffer at the cost of one
     * cache-side malloc per place change.) */
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
        /* Embedded-only — warm boot renders before WiFi is up; the
         * full scene_pack_init() does a network probe through
         * pack_cache_active that asserts inside lwip pre-init.
         * net_worker calls scene_pack_init() once WiFi's online to
         * upgrade to the server-synced pack and flag a re-render. */
        if (scene_pack_init_embedded() &&
            scene_pack_blit_current(scene_fb, SCENE_W, SCENE_H)) {
            ESP_LOGI(TAG, "scene from pack idx=%u",
                (unsigned)scene_pack_current());
        } else {
            ESP_LOGW(TAG, "scene_pack unavailable; blank backdrop");
            compositor::clear_to_paper(scene_fb, SCENE_W, SCENE_H);
        }

        /* Wake-from-deepsleep: try to restore the last non-home place
         * pack from LittleFS cache before the first render_with_expression
         * fires. Without this, the embedded "home" frame paints first
         * and the main loop's travel block swaps to (e.g.) forest a
         * couple seconds later — visually that's home → flash → forest
         * on every wake. The cache load is fast (~70 ms) and offline-
         * safe; if no cache, fall back to home which is the same as
         * embedded behaviour. The post-WiFi travel block in the main
         * loop still runs and refreshes against the server ETag. */
        char loc_nvs[MOCHI_LOC_ID_MAX + 1] = {};
        char sheet_nvs[MOCHI_LOC_SHEET_MAX + 1] = {};
        if (nvs_creds_get_last_loc(loc_nvs, sizeof(loc_nvs),
                                   sheet_nvs, sizeof(sheet_nvs)) &&
            loc_nvs[0] && strcmp(loc_nvs, "home") != 0 && sheet_nvs[0]) {
            /* Cache-only load — pack_cache_active_geom would HEAD-probe
             * the server first, which calls lwip getaddrinfo and panics
             * pre-WiFi (tcpip task isn't running yet at this point in
             * app_main). The post-WiFi travel block in the main loop
             * does the proper ETag-against-server refresh. */
            const uint8_t *bytes = pack_cache_load_geom_only(
                sheet_nvs, SCENE_W, SCENE_H);
            if (bytes && scene_pack_load_bytes(bytes) &&
                scene_pack_blit_current(scene_fb, SCENE_W, SCENE_H)) {
                ESP_LOGI(TAG, "boot scene → %s (from cache)", loc_nvs);
            } else {
                ESP_LOGW(TAG, "boot scene: '%s' cache miss — staying home",
                    loc_nvs);
            }
        }
    }

    /* Bring up the bundled pet pack so render_with_expression can
     * serve cells from flash without touching the network. Embedded-
     * only at boot; net_worker upgrades after WiFi is up. */
    if (!pet_pack_init_embedded()) {
        ESP_LOGW(TAG, "pet_pack unavailable; falling back to network "
                      "+ littlefs cache for every render");
    }

    /* On-device scene generation pipeline. Idle until a voice tool
     * call (`imagine_place`) lands; see design/16-on-device-imagine.md. */
    if (!imagine_init()) {
        ESP_LOGW(TAG, "imagine pipeline unavailable; voice "
                      "imagine_place tool will refuse");
    }

    /* On-device sleep consolidation (design/19). Idle until /api/state
     * advises a pass and the loop below fires it while the pet rests;
     * server-orchestrated, BYO key. */
    if (!consolidate_init()) {
        ESP_LOGW(TAG, "consolidate worker unavailable; advised "
                      "consolidation will be skipped");
    }

    /*
     * Care icons: load the downsampled 32×32 planes from the LittleFS
     * cache only. The native 80×80 fetch + downsample is a network op,
     * so on a cold cache (a device's first boot after pairing) it runs
     * on net_worker instead — the worker fills these same buffers and
     * writes them back to cache. `icons_cached` tells the worker
     * whether it needs to. A miss renders as a blank (invisible)
     * corner until the worker lands the artwork; render_chrome only
     * stamps icons on unzoned scenes anyway.
     */
    bool icons_cached = cache_ok;
    {
        char icon_suffix[24];
        snprintf(icon_suffix, sizeof(icon_suffix), "_icon_%ux%u",
            (unsigned)ICON_W, (unsigned)ICON_H);
        for (int i = 0; i < 4; i++) {
            /* Blank default so a miss is a clean corner, not garbage. */
            memset(icon_ink[i],  0xFF, ICON_BYTES);
            memset(icon_mask[i], 0xFF, ICON_BYTES);

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
                ESP_LOGI(TAG, "icon '%s' loaded from cache", CARE_ICON_KEYS[i]);
            } else {
                memset(icon_ink[i],  0xFF, ICON_BYTES);
                memset(icon_mask[i], 0xFF, ICON_BYTES);
                icons_cached = false;
            }
        }
        if (!icons_cached) {
            ESP_LOGI(TAG, "care icons not fully cached — net_worker will fetch");
        }
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

        /* Left: time. Right: wifi-glyph + battery. Centre: pet name
         * (centred on panel midpoint, not the slot between the
         * neighbours — the centre looks more visually balanced).
         * Net status sat left of the pet name in the first cut, but
         * grouping it with battery on the right reads as "device
         * health" cluster — symmetric with the time/clock cluster on
         * the left. */
        constexpr int STATUS_PAD = 4;
        const bool v_active = voice::is_active();
        const voice::Phase v_phase = voice::phase();

        /* Mic glyph (1× scale, 7×7) at the far left, but ONLY when
         * voice is reachable (net online + key present). It's no longer
         * a tap target — BOOT drives voice now (design/24) — so the
         * scaled-up affordance is gone and the glyph is back to a
         * passive state indicator. Hidden when offline so the bar
         * doesn't lie about whether a session would actually connect. */
        const bool voice_reachable =
            (s_net_phase == NetPhase::Online) && voice::is_ready();
        int time_x = STATUS_PAD;
        if (voice_reachable) {
            static const uint8_t MIC_GLYPH[7] = {
                0b00011100, 0b00011100, 0b00011100, 0b00011100,
                0b00001000, 0b00111110, 0b00001000,
            };
            const int mx = STATUS_PAD;
            const int my = STATUS_TEXT_Y + 1; /* baseline-aligned with text */
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 7; col++) {
                    const bool on = v_active
                        ? (row >= 1 && row <= 5 && col >= 1 && col <= 5)
                        : (((MIC_GLYPH[row] >> col) & 1) != 0);
                    if (!on) continue;
                    const int px = mx + col;
                    const int py = my + row;
                    if (px < 0 || py < 0 ||
                        px >= (int)MOCHI_EPD_WIDTH ||
                        py >= (int)MOCHI_EPD_HEIGHT) continue;
                    const size_t off = (size_t)py * 25 + ((size_t)px >> 3);
                    composite[off] &= (uint8_t)~(1u << (7 - ((size_t)px & 7)));
                }
            }
            time_x = STATUS_PAD + 7 + 3;   /* glyph + small gap */
        }
        blit_status_text(time_str, time_x);

        /* Right cluster: WiFi (right-most) + battery (text immediately
         * left of WiFi). Net status used to sit just left of the battery
         * digits at 7×7 — too tiny to read at arm's length. Now the
         * WiFi is the bar's right-most element at 11×9 (described
         * below) and battery slides left to make room. */
        constexpr int WIFI_W = 11;
        constexpr int WIFI_GAP = 3;          /* between battery digits and bars */
        const int wifi_x = (int)MOCHI_EPD_WIDTH - STATUS_PAD - WIFI_W;
        const int batt_w = (int)strlen(batt_str) * 8;
        const int batt_x = wifi_x - WIFI_GAP - batt_w;
        blit_status_text(batt_str, batt_x);

        /* Centre: pet name normally; during a session, the voice state
         * word (connecting / listening / talking) so state is
         * unambiguous regardless of the pet's face. design/23. */
        const char *center = pair.pet_name;
        if (v_active) {
            center = v_phase == voice::Phase::Connecting ? "connecting"
                   : v_phase == voice::Phase::Speaking   ? "talking"
                   :                                       "listening";
        }
        const int name_w = (int)strlen(center) * 8;
        int name_x = ((int)MOCHI_EPD_WIDTH - name_w) / 2;
        if (name_x < 14) name_x = 14;
        blit_status_text(center, name_x);

        /* WiFi state glyph — 4 ascending bars at the far-right of the
         * status bar (right of the battery digits). 11×9 px overall:
         * four 2-px-wide bars separated by a 1-px gutter, climbing in
         * height from 3→6→7→9 px so the cellular-style "signal" reads
         * are immediate. The previous 7×7 hairline version was hard
         * to parse at arm's length on a 200-px panel.
         *
         * State mapping is deliberately conservative for the e-ink
         * panel — no gradients, just filled vs hollow:
         *   Online      → all 4 bars filled
         *   Connecting  → bars 1+2 filled (low-fidelity 'searching')
         *   Offline     → 1×1 dot at the base of each bar slot, plus
         *                 a diagonal slash through the cluster
         *                 (clearly NOT signal — easy to spot)
         * Drawn directly into the composite framebuffer with the
         * same MSB-first convention render_chrome uses elsewhere. */
        {
            const NetPhase phase = s_net_phase;
            const int gx = wifi_x;
            const int base_y = STATUS_TEXT_Y + 8;  /* bottom of bars */
            constexpr int BAR_W = 2;
            constexpr int BAR_GAP = 1;
            constexpr int BAR_HEIGHTS[4] = { 3, 5, 7, 9 };
            auto plot = [&](int px, int py) {
                if (px < 0 || py < 0 ||
                    px >= (int)MOCHI_EPD_WIDTH ||
                    py >= (int)MOCHI_EPD_HEIGHT) return;
                const size_t off = (size_t)py * 25 + ((size_t)px >> 3);
                composite[off] &= (uint8_t)~(1u << (7 - ((size_t)px & 7)));
            };
            const int filled_bars =
                phase == NetPhase::Online     ? 4 :
                phase == NetPhase::Connecting ? 2 : 0;
            for (int b = 0; b < 4; b++) {
                const int bar_x = gx + b * (BAR_W + BAR_GAP);
                if (b < filled_bars) {
                    /* Solid filled bar from baseline up by its height. */
                    const int h = BAR_HEIGHTS[b];
                    for (int dy = 0; dy < h; dy++) {
                        for (int dx = 0; dx < BAR_W; dx++) {
                            plot(bar_x + dx, base_y - dy);
                        }
                    }
                } else {
                    /* Empty: just a 1-px floor pip so the slot still
                     * registers as part of the same indicator. */
                    for (int dx = 0; dx < BAR_W; dx++) {
                        plot(bar_x + dx, base_y);
                    }
                }
            }
            if (phase == NetPhase::Offline) {
                /* Diagonal slash from top-right to bottom-left over
                 * the empty cluster, so offline reads at a glance. */
                for (int i = 0; i < 9; i++) {
                    plot(gx + (10 - i), STATUS_TEXT_Y + i);
                }
            }
        }

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
        /* Pet sheet selection (design/17): a worn costume swaps the pet's
         * cells to costume-<petId>-<costumeId>-v1 (same 96×96 pet
         * geometry); null = base species. The base path is unchanged
         * (embedded pack → cache → pet-v1 fetch). The costume path skips
         * the embedded pack (it only holds base cells) and keys the cache
         * + fetch on the costume sheet. Dormant while the wardrobe is
         * empty (current_costume_id == ""). */
        char pet_sheet[120] = "pet-v1";
        bool costumed = false;
        {
            char costume[40];
            pet_sync_current_costume(costume, sizeof(costume));
            if (costume[0]) {
                struct mochi_pair_creds pc;
                if (pair_creds_load(&pc) && pc.pet_id[0]) {
                    snprintf(pet_sheet, sizeof(pet_sheet),
                        "costume-%s-%s-v1", pc.pet_id, costume);
                    costumed = true;
                }
            }
        }

        const char *cell_source = nullptr;
        uint32_t ms = 0;
        if (!costumed) {
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
                sprite_cache::load(pet_sheet, ink_suffix,
                    pet_ink, PET_CELL_BYTES, &got_ink) &&
                got_ink == PET_CELL_BYTES &&
                sprite_cache::load(pet_sheet, mask_suffix,
                    pet_mask, PET_CELL_BYTES, &got_mask) &&
                got_mask == PET_CELL_BYTES;
            if (from_cache) cell_source = "cache";
        }

        if (!cell_source) {
            char url[224];
            snprintf(url, sizeof(url),
                "https://mochi.val.run/devsprite/cell/%s/%s", pet_sheet, expr);
            uint16_t w = 0, h = 0;
            if (!sprite_fetch_cell(url, pet_ink, pet_mask, PET_CELL_BYTES,
                                   &w, &h, &ms)) {
                ESP_LOGW(TAG, "pet cell fetch failed for '%s' (sheet %s)",
                    expr, pet_sheet);
                device_diag_eventf(DIAG_WARN, "render", NULL,
                    "cell fetch fail %s/%s", pet_sheet, expr);
                return false;
            }
            if (w != PET_CELL_W || h != PET_CELL_H) {
                ESP_LOGW(TAG, "unexpected cell dims %ux%u (want %ux%u)",
                    w, h, (unsigned)PET_CELL_W, (unsigned)PET_CELL_H);
                return false;
            }
            if (cache_ok) {
                sprite_cache::store(pet_sheet, ink_suffix,  pet_ink,  PET_CELL_BYTES);
                sprite_cache::store(pet_sheet, mask_suffix, pet_mask, PET_CELL_BYTES);
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

        /* M11.5a — compute the active thought for this render. A
         * pinned `persistent` bubble (multi-page talk_seed echo)
         * survives intact; otherwise the predicate chain in
         * thought_generate is the only gate, modulo the post-tap
         * suppression window that keeps a freshly-cleared need
         * from re-firing locally before the substrate confirms. */
        const pet_thought_t *thought_ptr = nullptr;
        if (s_thought_active && s_active_thought.persistent) {
            thought_ptr = &s_active_thought;
        } else if (now_ms >= s_thought_suppress_until_ms &&
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
    auto render_asleep = [&](const char *status_text) {
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

        /* Status bar: centred string, no icons. Reuse the inline
         * glyph blit pattern. Caller supplies the text ("Asleep -
         * PWR to wake" for the PWR-tap sleep path, "Needs charge -
         * plug in" for the low-battery soft-power-down). */
        const char *line = status_text ? status_text : "Asleep";
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

    /* Initial render: pet on scene, neutral. Partial refresh —
     * EPD_DisplayPartBaseImage was called after the boot splash, so
     * the panel's "previous frame" buffer is already seeded; a full
     * here just visibly re-flashes the panel from splash → pet for
     * no benefit. Subsequent partial refreshes still work because
     * the splash is the seeded base. */
    if (!render_with_expression("neutral", false, nullptr)) {
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
    /* Seed the dev-pet projection so the resting face is sane before
     * net_worker's first /api/state pull lands. The worker overwrites
     * the snapshot (pet_sync caches it) and flags a re-render on
     * success; until then current_pet_decayed falls back to this. */
    init_dev_pet(now_ms_wall());

    /* Restore the last persisted snapshot from NVS. Each successful
     * pull/mutate writes pet_t + location/sheet/costume there, so a
     * deepsleep wake renders correct stats/mood/sprite from the very
     * first frame instead of falling back to init_dev_pet defaults
     * for 5-30 s while /api/state is in flight. The next pull will
     * overwrite. No-op (returns false) on first-ever boot. */
    pet_sync_restore_snapshot_from_nvs();

    /* Spin up the push worker + periodic-resync task. Idempotent; its
     * workers retry quietly until net_worker brings WiFi up. */
    pet_sync_start();

    /* Wake-from-deepsleep → record EVENT_WOKE locally now (event_log
     * is on-flash and survives a panic), but DEFER the pet_sync_enqueue
     * until WiFi is up. The push worker dequeues immediately on enqueue
     * and calls https_post → lwip_getaddrinfo, which asserts inside
     * tcpip_send_msg_wait_sem if the lwip tcpip task hasn't booted yet.
     *
     * Cold boot doesn't hit this because the event_log is empty at
     * boot; we only enqueue mutates from touch handlers, which run
     * after WiFi is ready. The deepsleep wake path is the only place
     * we'd enqueue at boot — gate the enqueue on s_net_phase == Online
     * via the s_pending_woke_at flag handled in the main loop below. */
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        const int64_t woke_at = now_ms_wall();
        ESP_LOGI(TAG, "wake from deepsleep → log EVENT_WOKE (push deferred)");
        event_log_append(EVENT_WOKE, woke_at);
        s_pending_woke_at = woke_at;
    }

    /* Kick the non-blocking connectivity worker (design/21). Everything
     * that needs the network — connect, SNTP, OTA, ETag refresh,
     * cold-cache icons, state pull — runs here, off the boot critical
     * path, so the pet is already on screen. net_ctx is static so it
     * outlives the worker without question. */
    static NetCtx net_ctx;
    for (int i = 0; i < 4; i++) {
        net_ctx.icon_ink[i]  = icon_ink[i];
        net_ctx.icon_mask[i] = icon_mask[i];
    }
    net_ctx.cache_ok     = cache_ok;
    net_ctx.icons_cached = icons_cached;
    xTaskCreate(net_worker, "net_worker", 16384, &net_ctx, 5, nullptr);

    touch::init();
    int64_t last_event_us = 0;
    constexpr int64_t DEBOUNCE_US = 200 * 1000;

    /* Travel state (design/17): the place the device is currently
     * rendering. Default is "home" (boot renders the embedded
     * scene-bundle-a). On wake-from-deepsleep we restore the
     * persisted last place from NVS — without that, the device
     * renders home for as long as /api/state takes to come back,
     * which on a flaky network can be tens of seconds or never.
     * The travel block below sees boot != server-canonical and
     * fetches the correct pack on the next tick. */
    char last_location[40] = "home";
    {
        char loc_nvs[MOCHI_LOC_ID_MAX + 1] = {};
        char sheet_nvs[MOCHI_LOC_SHEET_MAX + 1] = {};
        if (nvs_creds_get_last_loc(loc_nvs, sizeof(loc_nvs),
                                   sheet_nvs, sizeof(sheet_nvs)) &&
            loc_nvs[0] && strcmp(loc_nvs, "home") != 0 && sheet_nvs[0]) {
            /* Seed pet_sync's location cache from NVS and align
             * last_location so the in-loop travel block does NOT
             * re-fetch the same pack we already loaded above from
             * the LittleFS cache during boot init. Without aligning,
             * (loc=forest) != (last_location=home) → travel block
             * fires → another pack_cache_active_geom call → another
             * blit + EPD full refresh = visible flicker.
             *
             * If the server has since flipped the pet to a different
             * place (e.g. via voice-driven travel while powered down),
             * pet_sync_pull_now will overwrite the cached location on
             * its first successful pull, last_location will lag, and
             * the travel block fires for the new place — correct.  */
            ESP_LOGI(TAG, "boot: restoring last place '%s' (%s)",
                loc_nvs, sheet_nvs);
            pet_sync_seed_location(loc_nvs, sheet_nvs);
            snprintf(last_location, sizeof(last_location), "%.*s",
                (int)(sizeof(last_location) - 1), loc_nvs);
        }
    }
    /* Travel-failure recovery (design/17/18): a place-pack fetch that
     * fails floats a thought bubble and arms a backoff retry instead of
     * silently wedging on the current scene. travel_retry_loc remembers
     * which place we couldn't reach; travel_retry_at_us is the monotonic
     * deadline after which the travel block forces a re-attempt (also
     * triggered immediately by re-tapping the same nav_place zone). */
    char    travel_retry_loc[40]   = "";
    char    travel_warned_loc[40]  = "";   /* place we've already shown the
                                            * failure bubble for; suppresses a
                                            * re-render (e-paper flash) on each
                                            * 30 s retry. Cleared on success. */
    int64_t travel_retry_at_us     = 0;
    /* Backing store for the failure bubble's text. Static so the
     * file-scope s_active_thought.text pointer stays valid after we pin
     * it as a persistent, pageable passive bubble. Sized for the worst
     * case (the longest template + a full-length loc[40]) so the
     * snprintf can't truncate (-Werror=format-truncation). */
    static char travel_fail_msg[96] = "";
    constexpr int64_t TRAVEL_RETRY_BACKOFF_US = 30LL * 1000 * 1000;  /* 30 s */
    /* Worn-costume state (design/17): re-render the pet when it changes.
     * Empty = base species, which is the boot render. */
    char last_costume[40] = "";
    /* Diagnostic flush cadence (design/18). */
    int64_t last_diag_flush_us = esp_timer_get_time();
    /* Voice session bracket (design/18 ph3): nonzero while a session is
     * live; the start timestamp for the realtime_sessions row on end. */
    int64_t voice_sess_start_us = 0;
    /* Health heartbeat (design/18): first fires shortly after boot. */
    int64_t last_health_us = 0;

    /* Auto-trigger the key portal if NVS has no OpenAI key. The portal
     * shows the device's IP for the user to visit, so it's only useful
     * once WiFi is up — deferred into the loop (gated on net online,
     * one-shot via key_autostart_done) rather than fired here against a
     * not-yet-connected stack on the warm path. */
    bool key_autostart_done = false;

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
            /* Connecting gets its own face ('waking_up') rather than
             * reusing 'curious' (which also means attention/pat) — so
             * the connect wait reads as "mochi is waking up to talk",
             * not generic curiosity. design/23. */
            case voice::Phase::Connecting: return "waking_up";
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

    /* Phase 2 — WiFi-unavailable dialog (design/21). Raised by the loop
     * when net_worker reports Offline; dismissed by a tap outside the
     * action button, or actioned (→ reboot into SoftAP provisioning). */
    bool wifi_dialog_shown = false;
    /* One-tick suppression: when dev_menu consumes a touch we set this
     * so the NEXT touch event the FT6336 reports (often a press → release
     * pair across two ticks on the slow main-loop cadence) doesn't leak
     * into the live touch path and re-fire as a pet-body tap. */
    bool drain_next_touch = false;
    /* Deferred substrate POST for an EVENT_TALKED that fired at voice
     * session start. Held until the session ends so the mutate's TLS
     * handshake doesn't fight the OpenAI signaling handshake for
     * internal heap (MBEDTLS_ERR_SSL_ALLOC_FAILED otherwise). 0 when
     * no enqueue is pending. */
    int64_t s_pending_talked_at = 0;
    bool wifi_dialog_dismissed = false;
    ui_dialog::HitRect wifi_dialog_hit = {};

    while (true) {
        touch::Event ev;
        /* Tick cadence: 1 s normally, 100 ms during a voice session
         * so a BOOT-press to stop is consumed promptly. The previous
         * 1 s tick let the model talk for up to 1.5 s after the user
         * tapped to stop — long enough that the model finished a
         * phrase the user meant to cut. Faster polling tightens the
         * latency without burning idle CPU when voice is offline. */
        const int wait_ms = voice::is_active() ? 100 : 1000;
        bool got_touch = touch::wait_event(&ev, wait_ms);

        /* Tell the sleep_gesture watcher when the wheel or voice owns
         * the screen. The watcher gates single-tap PWR through
         * single_tap_advance_consume() in those modes (not the sleep
         * handoff), eliminating the race where the fallback sleep
         * render landed on top of an active flow. Polled at the top
         * of the loop so the gate tracks state edges promptly. */
        sleep_gesture::set_wheel_active(dev_menu::active());
        sleep_gesture::set_voice_active(voice::is_active());

        /* Critical-battery soft-power-down. LiPo cells damage
         * permanently below ~3.0 V; render a clear "Needs charge"
         * screen and commit deep-sleep before the regulator browns
         * out under load. Three consecutive sub-threshold readings
         * required so a transient ADC glitch (e.g. mid-WiFi-TX
         * voltage sag) can't power the device down on a healthy
         * battery. Skipped while voice is active so a session
         * mid-conversation doesn't get yanked — the brown-out
         * detector still catches that case. */
        static int s_critical_batt_streak = 0;
        constexpr uint16_t CRITICAL_BATT_MV = 3200;
        constexpr int      CRITICAL_BATT_CONSEC = 3;
        if (!voice::is_active()) {
            uint16_t mv = 0;
            if (battery_read(&mv, nullptr) && mv > 0 && mv < CRITICAL_BATT_MV) {
                s_critical_batt_streak++;
                ESP_LOGW(TAG, "battery critical: %u mV (streak %d/%d)",
                    mv, s_critical_batt_streak, CRITICAL_BATT_CONSEC);
                if (s_critical_batt_streak >= CRITICAL_BATT_CONSEC) {
                    device_diag_eventf(DIAG_WARN, "battery", nullptr,
                        "soft power-down: %u mV", mv);
                    device_diag_flush();
                    render_asleep("Needs charge - plug in");
                    sleep_gesture::commit_sleep();
                    /* Unreachable. */
                }
            } else {
                s_critical_batt_streak = 0;
            }
        }

        /* Touch-drain guard. If a previous tick set this, eat the
         * first touch we see and reset. Lets dev_menu / dialog flows
         * confidently mark "this gesture has been consumed" without
         * leaking it into the live tap pipeline on the next iteration. */
        if (got_touch && drain_next_touch) {
            ESP_LOGI(TAG, "drained one touch (%u,%u)",
                (unsigned)ev.x, (unsigned)ev.y);
            drain_next_touch = false;
            got_touch = false;
        }

        /* PWR gesture map (design/22 + design/24 update):
         *
         *   single-tap PWR      → sleep (when in Live)
         *                       → advance the dev_menu wheel (when up)
         *   double-tap PWR      → enter the dev_menu wheel from Live
         *
         * sleep_gesture exposes both: requested() (latched once a
         * single-tap clears its 350 ms double-tap window) and
         * double_tap_consume() (latched immediately on the second tap).
         * We service double-tap first so a fast pair doesn't race the
         * single-tap path into a sleep commit. */
        if (sleep_gesture::double_tap_consume()) {
            if (!voice::is_active() && !key_portal::active()) {
                ESP_LOGI(TAG, "PWR double-tap → enter dev_menu");
                dev_menu::request_advance();
            }
        }

        /* Single-tap PWR while a non-sleep flow owns the screen: the
         * watcher gates these via set_wheel_active / set_voice_active,
         * surfaces them through single_tap_advance_consume, and skips
         * the sleep handoff entirely. main routes by what's currently
         * active. The earlier in-handoff intercept raced with the
         * watcher's fallback timeout firing the sleep render on top of
         * the wheel; this gate-up-front design eliminates that race. */
        if (sleep_gesture::single_tap_advance_consume()) {
            if (voice::is_active()) {
                ESP_LOGI(TAG, "PWR tap (voice active) → stop session");
                voice::stop_session();
                render_with_expression("neutral", false, nullptr);
                last_voice_phase = voice::Phase::Idle;
                if (s_pending_talked_at != 0) {
                    pet_sync_enqueue(EVENT_TALKED, s_pending_talked_at);
                    s_pending_talked_at = 0;
                }
            } else if (dev_menu::active()) {
                /* Single-tap is the escape hatch from any non-Live
                 * mode — straight back to Live, never destructive.
                 * Double-tap (handled above) is what pages deeper.
                 * Mark the screen dirty so the pet redraws this tick. */
                ESP_LOGI(TAG, "PWR tap (wheel up) → exit to live");
                dev_menu::exit_to_live();
                s_net_render_dirty = true;
            }
        }

        if (sleep_gesture::requested()) {
            sleep_gesture::mark_handled();
            /* Mutate substrate state — the server has its own decay,
             * but a deliberate sleep gesture deserves an explicit
             * record (so server-side projection sees engagement
             * weight + the sleep-consolidation pass kicks in
             * promptly). event_log_append is local-first and survives
             * the network being down; pet_sync_enqueue queues the
             * mutate POST. push_event_now() does a best-effort
             * synchronous flush before we power down so the substrate
             * sees the sleep promptly when network is healthy; on a
             * timeout the queued event survives in the push worker's
             * pending buffer and ships on next boot. */
            const int64_t slept_at = now_ms_wall();
            event_log_append(EVENT_SLEPT, slept_at);
            pet_sync_enqueue(EVENT_SLEPT, slept_at);
            pet_sync_push_now();   /* best-effort flush before power-down */
            device_diag_event(DIAG_INFO, "sleep", "PWR tap → sleep", nullptr);
            device_diag_flush();   /* push before we power down */
            render_asleep("Asleep - PWR to wake");
            sleep_gesture::commit_sleep();
        }

        /* BOOT short-press → voice start/stop. The dedicated voice
         * gesture (design/24): BOOT is THE voice button. From Live
         * a press starts a session; while voice is up a press stops.
         * Skipped while the dev_menu wheel owns the screen so a
         * mid-wheel BOOT doesn't collide with whatever PWR-driven
         * action the user is in the middle of. */
        if (boot_press_consume() && !dev_menu::active() &&
            !key_portal::active()) {
            if (voice::is_active()) {
                ESP_LOGI(TAG, "BOOT → stop voice session");
                voice::stop_session();
                render_with_expression("neutral", false, nullptr);
                last_voice_phase = voice::Phase::Idle;
                if (s_pending_talked_at != 0) {
                    pet_sync_enqueue(EVENT_TALKED, s_pending_talked_at);
                    s_pending_talked_at = 0;
                }
            } else if (voice::is_ready()) {
                ESP_LOGI(TAG, "BOOT → start voice session");
                render_with_expression("waking_up", false, nullptr);
                last_voice_phase = voice::Phase::Connecting;
                if (voice::start_session() == 0) {
                    /* Local effects only at start; mutate POST defers
                     * to session end (avoids the OpenAI vs val.run
                     * concurrent-TLS heap collision — see v0.1.3). */
                    const int64_t talked_at = now_ms_wall();
                    event_log_append(EVENT_TALKED, talked_at);
                    pet_sync_touch(talked_at);
                    s_dev_pet.last_interaction_at = talked_at;
                    s_pending_talked_at = talked_at;
                } else {
                    ESP_LOGW(TAG, "voice start failed");
                    render_with_expression("neutral", false, nullptr);
                    last_voice_phase = voice::Phase::Idle;
                }
            } else {
                ESP_LOGW(TAG, "BOOT but voice not ready (no key?)");
            }
        }

        /* Dev-menu wheel: BOOT short-press cycles debug screens. While
         * a debug screen is up, dev_menu owns the panel — we skip the
         * pet render path until inactivity returns us to Live. A touch
         * exits early so the kid isn't stuck on a debug screen.
         *
         * Skipped while voice is active or the key portal owns the
         * screen — those flows render their own state. */
        if (!voice::is_active() && !key_portal::active()) {
            const int batt_pct_now = ([&]() -> int {
                uint16_t mv = 0; uint8_t pct = 0;
                return battery_read(&mv, &pct) ? (int)pct : -1;
            })();
            /* Pet-status line for the kid-facing MenuP1 header. One
             * line: mood label + happiness/fullness/energy. Cheap to
             * compute (project_mood is the same call the resting
             * render already makes); we just project against the same
             * decayed snapshot so the menu matches what the screen
             * showed a moment ago. Empty string while the menu is up
             * AND inside the inactivity-driven idle path is fine —
             * dev_menu's fallback is the pet name. */
            char pet_status_buf[80] = {};
            {
                int64_t now_ms = now_ms_wall();
                pet_event_t slice[12];
                size_t n = event_log_load_recent(slice, 12);
                pet_t decayed = current_pet_decayed(now_ms);
                mood_t m = project_mood(&decayed, slice, n, now_ms);
                const char *mname = mood_to_name(m);
                snprintf(pet_status_buf, sizeof(pet_status_buf),
                    "%s  %s\nhappy %u  full %u  energy %u",
                    pair.pet_name[0] ? pair.pet_name : "mochi",
                    mname ? mname : "?",
                    (unsigned)decayed.stats.happiness,
                    (unsigned)decayed.stats.fullness,
                    (unsigned)decayed.stats.energy);
            }
            const bool mode_changed = dev_menu::tick(
                epd,
                pair.pet_id[0] != '\0',
                pair.pet_name,
                ota_update::current_version(),
                s_net_ip, s_net_ssid,
                (int)s_net_phase, batt_pct_now,
                pet_status_buf);
            /* (LVGL is pumped by its own dispatcher task at ~33 Hz —
             * see lvgl_port.cpp's lv_task. Polling here was too slow
             * for drag-vs-tap discrimination during menu scrolling.) */
            if (dev_menu::active()) {
                if (got_touch) {
                    /* Action buttons are LVGL widgets now — LVGL's
                     * indev hit-tests them itself; dispatch_touch just
                     * returns whatever click event fired (or None on a
                     * miss). Pre-LVGL the menu was a hand-rolled
                     * tap-rect list and any miss exited the wheel —
                     * but with LVGL the user expects taps to stay in
                     * the menu (so they can scroll lists, retry a
                     * misaligned tap, etc.). Only exit when an actual
                     * action fired, or when the user PWR-taps. */
                    const auto act = dev_menu::dispatch_touch(
                        (int)ev.x, (int)ev.y);
                    const bool action_fired =
                        act != dev_menu::TouchResult::None;
                    /* Placeholders (Memories / Places) display an
                     * informational toast on tap but keep the menu up
                     * — there's no commit to confirm and no destructive
                     * effect to escape from. */
                    const bool stays_in_menu =
                        act == dev_menu::TouchResult::Memories ||
                        act == dev_menu::TouchResult::Places;
                    /* Only exit when a real action committed. A None
                     * result means the user tapped a non-actionable
                     * area (background, scroll gesture, etc.) and
                     * the menu should stay up. */
                    if (action_fired && !stays_in_menu) {
                        dev_menu::exit_to_live();
                        s_net_render_dirty = true;
                    }
                    /* Small helper: show a one-line "restarting" toast
                     * before a reboot action so the tap gets visible
                     * feedback. */
                    auto reboot_with_msg = [&](const char *l1, const char *l2) {
                        epd_ui::clear(epd);
                        epd_ui::draw_text_centered(epd, 84, 1, l1);
                        epd_ui::draw_text_centered(epd, 104, 1, l2);
                        epd->EPD_Init_Partial();
                        epd->EPD_DisplayPart();
                        vTaskDelay(pdMS_TO_TICKS(1200));
                        esp_restart();
                    };
                    switch (act) {
                        case dev_menu::TouchResult::OpenKeyPortal:
                            ESP_LOGI(TAG, "dev_menu → key portal");
                            key_portal::start(epd);
                            break;
                        case dev_menu::TouchResult::UpdateNow:
                            ESP_LOGI(TAG, "dev_menu → OTA check now");
                            ota_update::check_now();
                            epd_ui::clear(epd);
                            epd_ui::draw_text_centered(epd, 84, 1,
                                "Checking for");
                            epd_ui::draw_text_centered(epd, 104, 1,
                                "updates...");
                            epd->EPD_Init_Partial();
                            epd->EPD_DisplayPart();
                            break;
                        case dev_menu::TouchResult::ChangeWifi:
                            ESP_LOGI(TAG, "dev_menu → change WiFi (SoftAP)");
                            nvs_creds_set_prov_on_boot(true);
                            reboot_with_msg("Restarting for", "WiFi setup...");
                            break;
                        case dev_menu::TouchResult::ForgetWifi: {
                            char ssid[MOCHI_WIFI_SSID_MAX + 1] = {};
                            if (s_net_ssid[0]) {
                                snprintf(ssid, sizeof(ssid), "%s", s_net_ssid);
                            } else {
                                struct mochi_wifi_creds c = {};
                                if (nvs_creds_load_at(0, &c)) {
                                    snprintf(ssid, sizeof(ssid), "%s", c.ssid);
                                }
                            }
                            ESP_LOGI(TAG, "dev_menu → forget WiFi '%s'", ssid);
                            nvs_creds_forget(ssid);
                            reboot_with_msg("Forgetting WiFi", "Restarting...");
                            break;
                        }
                        case dev_menu::TouchResult::RePair:
                            ESP_LOGI(TAG, "dev_menu → re-pair");
                            pair_creds_clear();
                            reboot_with_msg("Re-pairing", "Restarting...");
                            break;
                        case dev_menu::TouchResult::GoHome:
                            ESP_LOGI(TAG, "dev_menu → go home");
                            /* Network POST; the travel block swaps to the
                             * home bundle next tick on success. No-op +
                             * logged if offline. Clear last_location so
                             * the travel block re-renders even if the
                             * device is already showing the home pack
                             * (e.g. after a deepsleep restore). */
                            pet_sync_enter_place("home");
                            last_location[0] = '\0';
                            break;
                        case dev_menu::TouchResult::Memories:
                        case dev_menu::TouchResult::Places:
                            /* MenuP1 placeholders — surfaced so the
                             * page has shape while the substrate
                             * memory ledger + world places list get
                             * wired up. For now: brief toast straight
                             * to e-paper, then force LVGL to repaint
                             * its (still-loaded) menu screen so the
                             * kid lands back on MenuP1 rather than a
                             * stale toast frame. */
                            ESP_LOGI(TAG, "dev_menu → %s (not yet wired)",
                                act == dev_menu::TouchResult::Memories
                                    ? "memories" : "places");
                            epd_ui::clear(epd);
                            epd_ui::draw_text_centered(epd, 84, 1,
                                act == dev_menu::TouchResult::Memories
                                    ? "Memories" : "Places");
                            epd_ui::draw_text_centered(epd, 104, 1,
                                "coming soon...");
                            epd->EPD_Init_Partial();
                            epd->EPD_DisplayPart();
                            vTaskDelay(pdMS_TO_TICKS(1200));
                            /* Drain any touch events the user
                             * generated while the toast was on screen
                             * (a finger lingering past tap-release
                             * registers as a fresh press during the
                             * 1.2 s sleep). Without this, the LVGL
                             * dispatcher catches the late press →
                             * release transition and stamps a
                             * pending_action on whatever button sits
                             * under the finger — committing the wrong
                             * action when the menu next reads its
                             * latch. */
                            drain_next_touch = true;
                            lvgl_port_force_full_refresh();
                            break;
                        case dev_menu::TouchResult::WifiSwitch: {
                            /* Runtime switch to a stored network — no
                             * reboot (STA→STA). Blocks up to ~15 s on this
                             * thread behind a toast. On failure, fall back
                             * to connect_any so we don't strand the device
                             * offline after dropping the old link. */
                            char ssid[MOCHI_WIFI_SSID_MAX + 1] = {};
                            snprintf(ssid, sizeof(ssid), "%s",
                                     dev_menu::picked_ssid());
                            ESP_LOGI(TAG, "dev_menu → switch WiFi '%s'", ssid);
                            struct mochi_wifi_creds c = {};
                            bool found = false;
                            for (size_t i = 0; i < nvs_creds_count(); i++) {
                                struct mochi_wifi_creds t = {};
                                if (nvs_creds_load_at(i, &t) &&
                                    strncmp(t.ssid, ssid,
                                            MOCHI_WIFI_SSID_MAX) == 0) {
                                    c = t; found = true; break;
                                }
                            }
                            if (!found) { ESP_LOGW(TAG, "ssid not stored"); break; }
                            epd_ui::clear(epd);
                            epd_ui::draw_text_centered(epd, 84, 1, "Switching to");
                            epd_ui::draw_text_centered(epd, 104, 1, ssid);
                            epd->EPD_Init_Partial();
                            epd->EPD_DisplayPart();
                            char ip[16] = {};
                            if (wifi_sta::switch_to(&c, ip, sizeof(ip))) {
                                nvs_creds_append(&c);   /* promote to MRU */
                                snprintf(s_net_ip,   sizeof(s_net_ip),   "%s", ip);
                                snprintf(s_net_ssid, sizeof(s_net_ssid), "%s", c.ssid);
                                s_net_phase = NetPhase::Online;
                                ESP_LOGI(TAG, "switched → %s (%s)", c.ssid, ip);
                            } else {
                                ESP_LOGW(TAG, "switch failed; restoring via connect_any");
                                char rs[MOCHI_WIFI_SSID_MAX + 1] = {};
                                if (wifi_sta::connect_any(ip, sizeof(ip),
                                                          rs, sizeof(rs))) {
                                    snprintf(s_net_ip,   sizeof(s_net_ip),   "%s", ip);
                                    snprintf(s_net_ssid, sizeof(s_net_ssid), "%s", rs);
                                    s_net_phase = NetPhase::Online;
                                } else {
                                    s_net_phase = NetPhase::Offline;
                                }
                            }
                            break;
                        }
                        case dev_menu::TouchResult::None:
                        default:
                            /* Touch missed all buttons (or hit a non-
                             * actionable area). Stay in the menu —
                             * pre-LVGL this was the "exit on miss"
                             * path, but LVGL widgets need touches to
                             * remain so users can re-aim, scroll, and
                             * generally interact normally. */
                            break;
                    }
                    /* Either way, we've consumed this touch event for
                     * the menu — clear got_touch so the live touch
                     * handler (icons / pet-tap delight) doesn't also
                     * see it on a miss. drain_next_touch swallows the
                     * FT6336's follow-up release pulse on action exit. */
                    got_touch = false;
                    if (action_fired) drain_next_touch = true;
                }
                continue;  /* dev_menu owns the screen */
            }
            if (mode_changed) {
                /* Just timed out from a debug screen back to live;
                 * mark dirty so the loop redraws the pet promptly. */
                s_net_render_dirty = true;
            }
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

        /* Drain the deferred wake-from-deepsleep mutate now that the
         * lwip stack is up. Boot-time enqueue would have crashed the
         * push worker on tcpip_send_msg_wait_sem; gating on Online
         * means we only kick the POST when the stack is ready to
         * resolve mochi.val.run. The local event_log entry was
         * already appended at boot, so even if we lose power before
         * this drains, the substrate sees it on the next boot. */
        if (s_pending_woke_at != 0 && s_net_phase == NetPhase::Online) {
            ESP_LOGI(TAG, "wake-deferred: enqueue EVENT_WOKE");
            pet_sync_enqueue(EVENT_WOKE, s_pending_woke_at);
            s_pending_woke_at = 0;
        }

        /* Key-portal autostart, deferred until WiFi is up (it shows the
         * device IP for the user to visit). One-shot. */
        if (!key_autostart_done && s_net_phase == NetPhase::Online &&
            !voice::is_active()) {
            key_autostart_done = true;
            char probe[MOCHI_OPENAI_KEY_MAX + 1] = {};
            if (!openai_key_load(probe, sizeof(probe))) {
                ESP_LOGI(TAG, "no openai key — opening key portal");
                key_portal::start(epd);
            }
            memset(probe, 0, sizeof(probe));
            if (key_portal::active()) continue;  /* portal owns the screen */
        }

        /* WiFi-unavailable dialog (design/21 Phase 2). When the
         * background connect gives up, surface a dismissible card over
         * the pet rather than taking over the screen or rebooting — the
         * pet keeps running offline (embedded packs + local decay). The
         * action reboots into SoftAP provisioning; a tap elsewhere
         * dismisses (latched so it doesn't immediately re-appear). */
        if (s_net_phase == NetPhase::Offline && !wifi_dialog_shown &&
            !wifi_dialog_dismissed && !voice::is_active()) {
            wifi_dialog_shown = true;
            render_with_expression(last_resting_expr, false, nullptr);
            ui_dialog::render(composite, MOCHI_EPD_WIDTH, MOCHI_EPD_HEIGHT,
                "No WiFi", "Mochi is offline.", "Tap below to set up.",
                "Set up WiFi", &wifi_dialog_hit);
            epd->EPD_LoadBuffer(composite, FB_LEN);
            epd->EPD_Init_Partial();
            epd->EPD_DisplayPart();
            ESP_LOGI(TAG, "wifi-unavailable dialog shown");
        }
        if (wifi_dialog_shown) {
            if (got_touch) {
                if (ui_dialog::hit_contains(&wifi_dialog_hit,
                                            (int)ev.x, (int)ev.y)) {
                    ESP_LOGI(TAG, "wifi dialog → reboot into provisioning");
                    nvs_creds_set_prov_on_boot(true);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    esp_restart();
                }
                ESP_LOGI(TAG, "wifi dialog dismissed → pet (offline)");
                wifi_dialog_shown = false;
                wifi_dialog_dismissed = true;
                render_with_expression(last_resting_expr, false, nullptr);
            }
            continue;  /* dialog owns input + screen until handled */
        }

        /* net_worker produced fresh state / cache / icons → re-render
         * the resting pet so the new artwork + snapshot show. */
        if (s_net_render_dirty && !voice::is_active()) {
            s_net_render_dirty = false;
            const char *resting = render_resting();
            snprintf(last_resting_expr, sizeof(last_resting_expr), "%s",
                     resting ? resting : "neutral");
            last_substrate_us = esp_timer_get_time();
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
            device_diag_event(DIAG_INFO, "voice", "session end", nullptr);
            /* Flush any deferred EVENT_TALKED enqueue now that the
             * voice session's TLS connection is gone — mbedtls has
             * its internal heap back. */
            if (s_pending_talked_at != 0) {
                pet_sync_enqueue(EVENT_TALKED, s_pending_talked_at);
                s_pending_talked_at = 0;
            }
            /* Travel responsiveness (design/17): a move_to_location said
             * during the session only changed pets.location server-side.
             * Pull once now so the travel block below renders the new
             * place this tick, instead of waiting for the next tap or the
             * 5-min resync. */
            pet_t ps; pet_event_t pe[4]; size_t pn = 0;
            pet_sync_pull_now(&ps, pe, 4, &pn);
        }

        /* Sleep consolidation (design/19, server-orchestrated). When
         * /api/state advised a pass and the pet is asleep, run a
         * reflection pass with the BYO key — but only at rest and with
         * no voice/imagine in flight (PSRAM budget + "reflect during
         * rest, not mid-conversation"). consolidate_start() self-guards
         * on in-flight + debounce, so calling it each idle tick is a
         * cheap no-op until the next pass is actually due. */
        if (!voice::is_active() && !imagine_in_flight() &&
            !consolidate_in_flight() && pet_sync_consolidation_advised()) {
            pet_t csnap;
            if (pet_sync_get_snapshot(&csnap) && csnap.asleep) {
                consolidate_start();
            }
        }

        /* Travel (design/17): follow pets.location. pet_sync refreshes it
         * on every pull/mutate, so a tap or the periodic resync picks up
         * a voice move_to_location / idle drift / travel-to-an-imagined-
         * place. On change we swap the device scene: "home" restores the
         * embedded bundle, any other place renders its server pack at
         * device geometry. Deferred while voice is live (avoid a mid-call
         * full refresh + a blocking fetch on the render thread); picked up
         * the tick after the session ends. */
        if (!voice::is_active()) {
            /* Backoff retry of a previously-failed travel fetch: when the
             * deadline elapses, force re-evaluation by clearing
             * last_location so the fetch below runs again if the location
             * still differs. Keeps a transient failure (cold server cache,
             * a blip) from being a permanent dead-end without thrashing a
             * 15 s-blocking fetch every tick. */
            if (travel_retry_at_us != 0 &&
                esp_timer_get_time() >= travel_retry_at_us) {
                travel_retry_at_us = 0;
                last_location[0] = '\0';
            }
            char loc[40], lsheet[64];
            pet_sync_current_location(loc, sizeof(loc), lsheet, sizeof(lsheet));
            if (loc[0] && strcmp(loc, last_location) != 0) {
                bool swapped = false;
                bool fetch_attempted = false;
                if (strcmp(loc, "home") == 0) {
                    swapped = scene_pack_load_home();
                } else if (lsheet[0]) {
                    /* Travel place packs go through pack_cache so they
                     * survive reboot/sleep. v0.1.7 fetched directly
                     * into a RAM-only travel_pack buffer; on a flaky
                     * network or after a deepsleep wake the device
                     * couldn't restore the last place at all. With
                     * pack_cache_active_geom, the body is written to
                     * LittleFS keyed by (sheet, cell_w, cell_h) and
                     * served by ETag on subsequent boots. */
                    fetch_attempted = true;
                    const uint8_t *bytes = pack_cache_active_geom(
                        lsheet, SCENE_W, SCENE_H, nullptr);
                    if (bytes) {
                        swapped = scene_pack_load_bytes(bytes);
                    }
                    if (!swapped) {
                        ESP_LOGW(TAG, "travel: pack unavailable for %s",
                            lsheet);
                        device_diag_eventf(DIAG_WARN, "travel", NULL,
                            "pack unavailable %s", lsheet);
                    }
                }
                if (swapped) {
                    /* Day/night for 2-cell place packs: pick the cell by
                     * RTC hour. Meta-link resolution is design/17 phase 4. */
                    if (scene_pack_count() == 2) {
                        mochi_datetime dt = {};
                        bool night = rtc_get(&dt) && (dt.hour < 7 || dt.hour >= 19);
                        scene_pack_set(night ? 1 : 0);
                    }
                    scene_pack_blit_current(scene_fb, SCENE_W, SCENE_H);
                    render_with_expression("neutral", true, nullptr);
                    ESP_LOGI(TAG, "traveled → %s", loc);
                    device_diag_eventf(DIAG_INFO, "travel", NULL,
                        "to %s (%s)", loc,
                        (strcmp(loc, "home") == 0) ? "bundle" : lsheet);
                    /* Persist for wake-from-deepsleep restore. Without
                     * this, every wake renders the embedded "home"
                     * bundle until /api/state lands — and on a flaky
                     * network that pull can take 30 s+ or fail
                     * outright (see ESP_ERR_HTTP_EAGAIN traces). The
                     * sheet is saved alongside so the wake-time fetch
                     * goes straight to /devsprite/pack/<sheet>. */
                    nvs_creds_set_last_loc(loc,
                        (strcmp(loc, "home") == 0) ? "" : lsheet);
                    travel_retry_at_us = 0;   /* success clears any pending retry */
                    travel_warned_loc[0] = '\0';
                    /* Drop a still-pinned failure bubble so it doesn't
                     * reappear over the newly-loaded scene. Pointer match
                     * avoids clobbering an unrelated active bubble. */
                    if (s_thought_active && s_active_thought.text == travel_fail_msg) {
                        s_thought_active = false;
                        memset(&s_active_thought, 0, sizeof(s_active_thought));
                    }
                } else if (fetch_attempted) {
                    /* Couldn't reach the place. Stay on the current scene
                     * but float a thought bubble so the failure isn't a
                     * silent no-op, and arm a backoff retry. A 404 means
                     * the place pack isn't built/ready yet ("...yet"); any
                     * other status (0 = timeout/transport, 5xx, or a 200
                     * that failed to parse) is "something's wrong". Only
                     * (re-)paint the bubble the first time we fail to reach
                     * a given place — the periodic retry shouldn't flash the
                     * panel every 30 s while we're stuck there. */
                    if (strcmp(loc, travel_warned_loc) != 0) {
                        /* pack_cache_active_geom hides HTTP status from
                         * us so we can't differentiate 404-not-ready
                         * from "something's wrong" anymore. The cache
                         * layer logs its own device_diag entries with
                         * structured detail; the bubble keeps a single
                         * generic message. */
                        snprintf(travel_fail_msg, sizeof(travel_fail_msg),
                            "can't get to the %s, try again later", loc);
                        /* Pin as a persistent passive bubble so the kid can
                         * page through a longer message — the touch handler's
                         * Zone::Thought path bumps the page on tap and
                         * dismisses on the last — rather than seeing it
                         * truncated. Cleared on a later successful travel. */
                        memset(&s_active_thought, 0, sizeof(s_active_thought));
                        s_active_thought.action_kind = THOUGHT_ACTION_NONE;
                        s_active_thought.text         = travel_fail_msg;
                        s_active_thought.style        = THOUGHT_STYLE_THOUGHT;
                        s_active_thought.persistent   = true;
                        s_active_thought.page         = 0;
                        s_thought_active = true;
                        render_with_expression("neutral", true, &s_active_thought);
                        snprintf(travel_warned_loc, sizeof(travel_warned_loc), "%s", loc);
                    }
                    snprintf(travel_retry_loc, sizeof(travel_retry_loc), "%s", loc);
                    travel_retry_at_us = esp_timer_get_time() + TRAVEL_RETRY_BACKOFF_US;
                }
                /* Record either way so a failed fetch doesn't thrash the
                 * loop every tick. On failure the backoff above forces a
                 * later retry; re-tapping the nav zone forces one now. */
                snprintf(last_location, sizeof(last_location), "%s", loc);
            }
        }

        /* Periodic diagnostic flush (design/18): push buffered records to
         * substrate every couple of minutes when idle, so a field device
         * is debuggable without serial. Best-effort; no-op if nothing
         * buffered or offline. */
        {
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_diag_flush_us > 120LL * 1000 * 1000) {
                device_diag_flush();
                last_diag_flush_us = now_us;
            }
        }

        /* Health heartbeat (design/18): periodic snapshot of heap / PSRAM /
         * battery / temp so slow degradation + crashes have context when
         * you read device_logs. No %f — newlib-nano printf drops it, so
         * temp is decidegrees C. */
        {
            int64_t now_us = esp_timer_get_time();
            if (now_us - last_health_us > 300LL * 1000 * 1000) {
                last_health_us = now_us;
                uint16_t mv = 0; uint8_t pct = 0;
                battery_read(&mv, &pct);
                float t = 0.0f, rh = 0.0f;
                shtc3_read(&t, &rh);
                char ctx[200];
                snprintf(ctx, sizeof(ctx),
                    "{\"heap\":%u,\"heap_min\":%u,\"psram\":%u,\"batt_mv\":%u,"
                    "\"batt_pct\":%u,\"temp_dc\":%d,\"rh\":%d,\"up_s\":%lld}",
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                    (unsigned)mv, (unsigned)pct,
                    (int)(t * 10.0f), (int)rh,
                    (long long)(now_us / 1000000));
                device_diag_event(DIAG_INFO, "health", "snapshot", ctx);
                if (pct > 0 && pct < 15) {
                    device_diag_eventf(DIAG_WARN, "battery", ctx,
                        "low %u%%", (unsigned)pct);
                }
            }
        }

        /* Voice session telemetry (design/18 ph3): bracket the session
         * from is_active() transitions (covers every stop path) and POST
         * a realtime_sessions row on end. Duration + end_reason only;
         * per-turn tokens are ph3b (needs response.usage parsing). */
        {
            bool v_active = voice::is_active();
            if (v_active && voice_sess_start_us == 0) {
                voice_sess_start_us = esp_timer_get_time();
            } else if (!v_active && voice_sess_start_us != 0) {
                int dur_s = (int)((esp_timer_get_time() - voice_sess_start_us) / 1000000);
                voice_sess_start_us = 0;
                int turns = 0, in_tok = 0, out_tok = 0, total_tok = 0;
                voice_peer_get_session_stats(&turns, &in_tok, &out_tok, &total_tok);
                char vmodel[48];
                model_prefs_voice(vmodel, sizeof(vmodel));
                pet_sync_post_voice_session(dur_s, vmodel, "marin",
                    "ended", turns, in_tok, out_tok, total_tok);
            }
        }

        /* Costume follow (design/17): re-render the pet when the worn
         * costume changes (wear / take-off via voice). render_with_
         * expression picks the costume sheet automatically. Deferred
         * while voice is live, like travel. */
        if (!voice::is_active()) {
            char cur_costume[40];
            pet_sync_current_costume(cur_costume, sizeof(cur_costume));
            if (strcmp(cur_costume, last_costume) != 0) {
                snprintf(last_costume, sizeof(last_costume), "%s", cur_costume);
                render_with_expression("neutral", true, nullptr);
            }
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
                /* A caller-pinned persistent bubble (e.g. the travel-fail
                 * notice) owns the channel until it's tapped through or a
                 * state change clears it — don't let auto-generation
                 * regenerate or wipe it here. Mirrors render_resting. */
                const bool pinned = s_thought_active && s_active_thought.persistent;
                const bool gen_ok = !pinned &&
                    now_ms >= s_thought_suppress_until_ms &&
                    thought_generate(&decayed, now_ms, &candidate);
                const bool thought_changed = !pinned && (
                    gen_ok != s_thought_active ||
                    (gen_ok &&
                     (candidate.action_event != s_active_thought.action_event ||
                      candidate.action_kind  != s_active_thought.action_kind)));

                const bool sprite_changed =
                    name && strcmp(name, last_resting_expr) != 0;

                if (sprite_changed || thought_changed) {
                    if (!pinned) {
                        s_thought_active = gen_ok;
                        if (gen_ok) {
                            s_active_thought = candidate;
                        } else {
                            memset(&s_active_thought, 0, sizeof(s_active_thought));
                        }
                    }
                    const char *render_name = name ? name : last_resting_expr;
                    ESP_LOGI(TAG, "substrate refresh: %s → %s "
                                  "(stats h%u/f%u/e%u eng=%.2f) thought=%s",
                        last_resting_expr, render_name,
                        decayed.stats.happiness, decayed.stats.fullness,
                        decayed.stats.energy,
                        recent_engagement(slice, n, now_ms),
                        s_thought_active ? s_active_thought.text : "(none)");
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

        /* Voice start/stop is BOOT-driven now (design/24, see the
         * boot_press_consume block at the top of the loop). The
         * status-bar mic glyph remains as a passive state indicator
         * but no longer hosts a tap rect; touches in that area fall
         * through to the normal care / scene-zone pipeline. While a
         * session is active a touch anywhere is intentionally NOT
         * tap-to-stop — kids playing with the panel mid-conversation
         * shouldn't cut Mochi off accidentally. */

        Zone z = classify(ev.x, ev.y);
        /* Thought-bubble taps resolve expr + kind dynamically from
         * the active thought's payload — the static zone→expr /
         * zone→event maps can't know which action this particular
         * bubble carries (today: SLEPT; tomorrow: FED, talk_seed,
         * etc. — see design/12-thought-bubble.md). */
        const pet_thought_t *tapped_thought =
            (z == Zone::Thought && s_thought_active) ? &s_active_thought : nullptr;

        /* Passive-bubble pagination short-circuit (multi-page talk_seed
         * echoes, future long mood readouts). When the kid taps a
         * persistent passive bubble that still has pages to show,
         * advance the page and re-render in place — skip the rest of
         * the touch pipeline (no event log, no action expression, no
         * 5s hold). On the LAST page, fall through to the normal
         * Zone::Thought clear path (THOUGHT_SUPPRESS + memset) so the
         * tap dismisses the bubble; control then continues through
         * the !expr early-bail. */
        if (tapped_thought &&
            tapped_thought->action_kind == THOUGHT_ACTION_NONE) {
            if (thought_has_more()) {
                ESP_LOGI(TAG, "thought tap: advance page %d → %d",
                    s_active_thought.page, s_active_thought.page + 1);
                s_active_thought.page++;
                render_with_expression(last_resting_expr, false,
                                       &s_active_thought);
                continue;
            }
            ESP_LOGI(TAG, "thought tap: dismiss passive bubble");
            s_thought_suppress_until_ms = now_ms_wall() + THOUGHT_SUPPRESS_MS;
            s_thought_active = false;
            memset(&s_active_thought, 0, sizeof(s_active_thought));
            /* Repaint the resting pet so the dismissed bubble vanishes
             * immediately; the !expr path below already short-circuits
             * the action-render pipeline for action_kind == NONE. */
            render_with_expression(last_resting_expr, false, nullptr);
            continue;
        }

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
         * Forgiving snap: ZONE_SLOP_PX widens each rect by 16 px so a
         * tap near (but not inside) an authored rect still resolves.
         *
         * Lenient overlap policy: scene zones win even when the tap
         * lands inside the pet sprite's bounding box. The pet
         * frequently sits over food/heart/play/door icons in scenes_a;
         * a kid putting their finger on the food bowl shouldn't be
         * pre-empted by "ah you tapped the pet". The pet is only
         * resolved as the target if NO scene zone (with slop) and NO
         * care icon hit. tapped_pet is still tracked for the pet-tap
         * delight path below — but it's the LAST priority, not the
         * first. */
        constexpr int ZONE_SLOP_PX = 16;
        const bool tapped_pet =
            (int)ev.x >= PET_DX && (int)ev.x <  PET_DX + (int)PET_CELL_W &&
            (int)ev.y >= PET_DY && (int)ev.y <  PET_DY + (int)PET_CELL_H;

        scene_pack_action_t scene_act = {};
        bool scene_hit = false;
        if (!tapped_thought && (int)ev.y >= STATUS_BAR_H) {
            scene_hit = scene_pack_action_at(
                (int16_t)ev.x, (int16_t)ev.y, ZONE_SLOP_PX, &scene_act);
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

        /* Scene-navigation actions short-circuit the rest of the touch
         * pipeline: re-blit the scene cell, refresh, and bail before the
         * per-tap care pipeline runs. Refresh is hybrid partial/full —
         * see SCENE_NAV_FULL_EVERY. */
        if (scene_hit && (scene_act.kind == MPK_ACTION_NAV_RELATIVE ||
                          scene_act.kind == MPK_ACTION_NAV_SCENE)) {
            static uint32_t s_nav_n = 0;
            uint16_t to = (scene_act.kind == MPK_ACTION_NAV_RELATIVE)
                ? scene_pack_advance(scene_act.data)
                : scene_pack_set((uint16_t)scene_act.data);
            bool full = (++s_nav_n % SCENE_NAV_FULL_EVERY) == 0;
            ESP_LOGI(TAG, "scene nav %s → idx=%u (%s)",
                scene_act.kind == MPK_ACTION_NAV_RELATIVE ? "rel" : "abs",
                (unsigned)to, full ? "full" : "partial");
            scene_pack_blit_current(scene_fb, SCENE_W, SCENE_H);
            render_with_expression("neutral", full, nullptr);
            continue;
        }

        /* nav_place zones — cross-place travel (design/17). The zone's
         * seed_text is the target place id. We hand it to the substrate
         * (POST /enter, which also updates our local location); the
         * travel block at the top of the loop renders it on the next
         * tick — "home" restores the bundle, any other place fetches its
         * pack at device geometry. This is the non-voice travel path. */
        if (scene_hit && scene_act.kind == MPK_ACTION_NAV_PLACE &&
            scene_act.seed_text && scene_act.seed_len > 0) {
            char place_id[40] = {0};
            size_t n = scene_act.seed_len < sizeof(place_id) - 1
                ? scene_act.seed_len : sizeof(place_id) - 1;
            memcpy(place_id, scene_act.seed_text, n);
            ESP_LOGI(TAG, "scene nav_place → %s", place_id);
            pet_sync_enter_place(place_id);
            /* Force the travel block to re-render even when the user
             * tapped to go to the place they're already in. Without
             * this, a deepsleep-restored last_location pre-set to (e.g.)
             * "forest" makes a deliberate forest-tap a no-op: enter_place
             * succeeds, location matches, travel block sees no diff,
             * nothing redraws. Clearing last_location lets the travel
             * block re-blit the current scene on the next tick — same
             * behaviour as a genuine cross-place move. */
            last_location[0] = '\0';
            /* If this re-taps the place we just failed to reach, drop the
             * backoff so the travel block retries on the next tick rather
             * than waiting out the timer (re-tapping a failed nav zone
             * would otherwise look like a dead button). Clear the
             * warned-place latch too, so if it fails again the kid gets
             * the bubble again instead of silence. */
            if (travel_retry_at_us != 0 && strcmp(place_id, travel_retry_loc) == 0) {
                travel_retry_at_us = esp_timer_get_time();
                travel_warned_loc[0] = '\0';
            }
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
         *                  the raw seed text. The renderer's wrap
         *                  + vertical-centre helper handles the
         *                  layout (up to 3 × ~11-char lines fit in
         *                  the bubble interior); longer seeds
         *                  silently truncate at the third line. No
         *                  care event lands; the bubble just
         *                  decorates the next render and stays
         *                  through the THOUGHT_SUPPRESS_MS window
         *                  so the kid has time to read it.
         *
         * The seed pointer is borrowed from the embedded pack and
         * not NUL-terminated; copy it into a local buffer first. */
        static char  s_seed_buf[256];
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
                memset(&s_seed_thought, 0, sizeof(s_seed_thought));
                s_seed_thought.action_kind = THOUGHT_ACTION_NONE;
                s_seed_thought.text        = s_seed_buf;
                /* External speech echo — render with the spoken-style
                 * triangle tail so it visually reads as "what mochi
                 * would have said" rather than internal monologue. */
                s_seed_thought.style       = THOUGHT_STYLE_SPOKEN;
                seed_thought_ptr           = &s_seed_thought;
                expr = "thinking";
            }
        }

        /* Pet-tap delight + mood readout. The face rotates through a
         * pool of expressive sprites (so a kid poking at the pet sees
         * a reaction) AND a thought-style bubble surfaces the pet's
         * current dominant mood/need ("hungry...", "miss u...", etc.)
         * for the 5-second tap hold. The bubble is passive — no event
         * dispatch, no tap target — it's an at-a-glance "how is mochi
         * feeling?" answer. EVENT_COMFORTED still lands on the event
         * log below, so a pat actually comforts the pet.
         *
         * Spawn a transient mood thought bubble — but DON'T cycle
         * the pet expression. Earlier we rotated through a delight
         * pool (curious/cheerful_wave/excited/comforted/thinking) on
         * pet-tap, but the random-feel didn't read as personality —
         * just inconsistent. Now the pet keeps its current resting
         * expression and only the bubble surfaces; the bubble carries
         * the "what's mochi feeling?" answer through thought_for_pet_tap. */
        static pet_thought_t s_petmood_thought;
        auto spawn_petmood_bubble = [&]() {
            /* Don't overwrite a talk_seed bubble already on the
             * seed-bubble channel — talk_seed is the louder signal. */
            if (!seed_thought_ptr) {
                pet_event_t slice[12];
                size_t n = event_log_load_recent(slice, 12);
                int64_t pmoment = now_ms_wall();
                pet_t decayed = current_pet_decayed(pmoment);
                thought_for_pet_tap(&decayed, slice, n, pmoment,
                                    &s_petmood_thought);
                seed_thought_ptr = &s_petmood_thought;
            }
        };

        /* When the current scene has authored zones, the corner-
         * quadrant fallback is suppressed: a tap that misses every
         * zone (after the snap fallback above) keeps the resting
         * expression and (on pet-body taps) spawns a mood bubble. */
        const bool zoned = scene_pack_current_has_zones();
        if (zoned && !scene_hit && !tapped_thought) {
            if (tapped_pet) {
                spawn_petmood_bubble();
                expr = last_resting_expr;   /* keep current resting face */
            } else {
                expr = "curious";
            }
        }

        /* Unzoned scene + tap on pet body: same deal — bubble only. */
        if (!zoned && !scene_hit && !tapped_thought &&
            z == Zone::Center && tapped_pet) {
            spawn_petmood_bubble();
            expr = last_resting_expr;   /* keep current resting face */
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

        /* (Voice start/stop is handled up-front via the status-bar mic
         * affordance — see the voice-control block above. design/23.) */

        /* Thought-bubble tap, action-bubble branch only. The passive-
         * bubble branch (action_kind == NONE: pagination advance or
         * last-page dismiss) is handled earlier, right after we
         * classify the zone, so it can short-circuit before the
         * !expr continue further down. By the time control reaches
         * here for a Zone::Thought tap, the bubble must be an
         * action bubble (thought_action_to_expr returned a non-NULL
         * expr); commit the action and clear the bubble. */
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

        /* Pagination promotion — the renderer just rendered page 0
         * of seed_thought_ptr; if its text wrapped past one page,
         * thought_has_more() is latched true. Promote the seed
         * bubble into s_active_thought with persistent=true so it
         * survives the post-hold render_resting (which would
         * otherwise clear it) and the next Zone::Thought tap
         * advances pages via the handler above. Skip the 5s hold +
         * resting transition — the bubble now owns the screen
         * until the kid taps through. */
        if (seed_thought_ptr && thought_has_more()) {
            ESP_LOGI(TAG, "thought overflow: promote seed bubble → persistent");
            memcpy(&s_active_thought, seed_thought_ptr, sizeof(s_active_thought));
            s_active_thought.page       = 0;
            s_active_thought.persistent = true;
            s_thought_active = true;
            continue;
        }

        /* Hold the expression for ~5 s, but in small slices so we can
         * still service a PWR-tap-to-sleep gesture mid-hold. The
         * sleep_gesture watcher only waits HANDOFF_GRACE_MS (1500 ms)
         * before falling back to a bare "Asleep / PWR to wake" text
         * render — if we block here for the full 5 s the rich
         * pet-on-scene asleep image never lands. Polling every
         * 100 ms is well within the grace window and cheap.
         *
         * On a mid-hold sleep request: claim it, render the rich
         * asleep frame, commit. commit_sleep doesn't return. */
        {
            constexpr int CHUNK_MS = 100;
            int remaining = RESTING_AFTER_TAP_MS;
            while (remaining > 0) {
                vTaskDelay(pdMS_TO_TICKS(CHUNK_MS));
                remaining -= CHUNK_MS;
                if (sleep_gesture::requested()) {
                    sleep_gesture::mark_handled();
                    ESP_LOGI(TAG, "sleep requested mid-tap-hold → render asleep");
                    device_diag_event(DIAG_INFO, "sleep",
                        "PWR tap (mid-hold) → sleep", nullptr);
                    device_diag_flush();
                    render_asleep("Asleep - PWR to wake");
                    sleep_gesture::commit_sleep();
                    /* Unreachable. */
                }
            }
        }

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
