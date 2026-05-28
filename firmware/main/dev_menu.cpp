#include "dev_menu.h"

#include <stdio.h>
#include <string.h>
#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "lvgl_port.h"

#include "board_pins.h"
#include "epd_ui.h"
#include "nvs_creds.h"
#include "model_prefs.h"

static const char *TAG = "dev_menu";

namespace dev_menu {

/* Inactivity timeout: any non-Live mode snaps back to Live after this
 * many microseconds without a PWR press. Long enough to read a full
 * diagnostics screen + cross-check details with notes; touch or a
 * fresh PWR press still exits / advances earlier. */
static constexpr int64_t INACTIVITY_US = 60LL * 1000 * 1000;

static Mode                 s_mode = Mode::Live;
static int64_t              s_entered_mode_us = 0;
/* Set by request_advance() (called from main.cpp on PWR taps) and
 * cleared by tick() after it advances the wheel. */
static std::atomic<bool>    s_press_pending{false};
static bool                 s_started = false;

/* Most recent menu data — cached so dispatch-time mode pushes
 * (e.g. SwitchWifi → render WifiModal) don't need to be re-handed
 * the params. tick() refreshes these on every advance/render. */
static char                 s_pet_name[40] = {};
static char                 s_version[40]  = {};
static char                 s_ip_str[24]   = {};
static char                 s_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};
static int                  s_net_phase    = 0;
static int                  s_batt_pct     = 0;
static char                 s_pet_status[80] = {};

/* Picked SSID payload — populated by dispatch_touch when the user
 * commits a switch in the WifiModal; consumed by main.cpp's
 * picked_ssid() shortly after. */
static char                 s_picked_ssid[MOCHI_WIFI_SSID_MAX + 1] = {};

/* LVGL screens — one per non-Live mode. We keep them as separate
 * lv_obj_t* so swap-on-advance is just lv_screen_load(). Each is
 * lazily created on first entry to avoid burning RAM during boot.
 *
 * Buttons use user_data to carry the TouchResult enum back through
 * the click event; dispatch_touch reads it from `lv_event_get_target`
 * and returns it to main.cpp.
 *
 * v0.1.7 collapsed Info + Actions into a single Menu screen — the
 * info header sits above the action buttons inside the same scroll
 * container, so the user sees device state and tappable actions in
 * one glance instead of two PWR taps. */
static lv_obj_t            *s_menu_p1_scr      = nullptr;
static lv_obj_t            *s_menu_p2_scr      = nullptr;
static lv_obj_t            *s_menu_p3_scr      = nullptr;
static lv_obj_t            *s_pet_status_label = nullptr; /* lives inside s_menu_p1_scr */
static lv_obj_t            *s_info_label       = nullptr; /* lives inside s_menu_p2_scr */
static lv_obj_t            *s_wifi_scr         = nullptr;
static lv_obj_t            *s_models_scr       = nullptr;

/* Pending action latched by the click event handler; consumed by
 * dispatch_touch on the next call. LVGL's click events fire on
 * release inside its own dispatcher (called from lv_timer_handler),
 * which runs on the main task — so a single static is safe.
 *
 * Why this indirection: we want the existing dev_menu.h API where
 * main.cpp calls dispatch_touch(x,y) and gets a TouchResult. With
 * LVGL, the click is already routed by LVGL's hit-testing — the
 * click handler stamps the result and dispatch_touch just hands it
 * back. */
static TouchResult          s_pending_action = TouchResult::None;

/* Modal-push flag — set by on_click when it mutates s_mode to a
 * modal (WifiModal / ModelsModal) and latches s_press_pending to
 * force tick() to render. Distinct from s_press_pending so tick()
 * can disambiguate "user pressed PWR to advance" from "click handler
 * pushed a modal" without resorting to a wall-clock window (which
 * raced — main.cpp's 1 Hz tick easily exceeded any tight threshold,
 * causing the modal to render-and-immediately-advance-out as if PWR
 * had been pressed). Cleared by tick() once consumed. */
static std::atomic<bool>    s_modal_push_pending{false};

static const char *mode_name(Mode m) {
    switch (m) {
        case Mode::Live:      return "live";
        case Mode::MenuP1:    return "menu_p1";
        case Mode::MenuP2:    return "menu_p2";
        case Mode::MenuP3:    return "menu_p3";
        case Mode::WifiModal: return "wifi_modal";
        case Mode::ModelsModal: return "models_modal";
        default:              return "?";
    }
}

const char *picked_ssid(void) { return s_picked_ssid; }

void init(epaper_driver_display * /*epd*/) {
    s_mode = Mode::Live;
    s_entered_mode_us = 0;
    s_press_pending.store(false, std::memory_order_release);
    s_started = true;
}

void request_advance(void) {
    s_press_pending.store(true, std::memory_order_release);
}

Mode current(void) { return s_mode; }
bool active(void) { return s_mode != Mode::Live; }

void exit_to_live(void) {
    if (s_mode != Mode::Live) {
        ESP_LOGI(TAG, "exit → live");
        s_mode = Mode::Live;
        /* Don't delete the LVGL screens — they're cheap to keep
         * around in PSRAM and recreate-on-reentry has noticeable
         * latency. main.cpp's render_resting renders directly to
         * the e-paper, bypassing LVGL while in Live. */
    }
}

/* Advance (PWR×2 from anywhere): pages deeper into riskier territory.
 *
 *   Live      → MenuP1 (entry — kid-facing main page)
 *   MenuP1    → MenuP2 (settings — Switch WiFi / OpenAI key)
 *   MenuP2    → MenuP3 (destructive — Update / Add / Forget / Re-pair)
 *   MenuP3    → MenuP1 (wrap back to the safe page)
 *   WifiModal → MenuP2 (a modal isn't a "page" — bounce back to parent;
 *               in practice the user uses PWR×1 to exit to Live instead)
 *
 * PWR×1 from anywhere non-Live is handled by exit_to_live() up in
 * main.cpp — it never enters the advance() table. That keeps the
 * "single tap escapes" rule absolute regardless of which page is up.
 *
 * Pagination beats scrolling on this hardware — see dev_menu.h. */
static Mode advance(Mode m) {
    switch (m) {
        case Mode::Live:      return Mode::MenuP1;
        case Mode::MenuP1:    return Mode::MenuP2;
        case Mode::MenuP2:    return Mode::MenuP3;
        case Mode::MenuP3:    return Mode::MenuP1;
        case Mode::WifiModal: return Mode::MenuP2;
        case Mode::ModelsModal: return Mode::MenuP2;
        default:              return Mode::Live;
    }
}

/* A modal is pushed by its on_click handler (SwitchWifi → WifiModal,
 * OpenModels → ModelsModal): it mutates s_mode itself and latches
 * s_press_pending to force tick() to render the new screen. tick()
 * must recognise that latched press as a modal-push, NOT a wheel
 * advance — otherwise advance() bounces the modal straight back to its
 * parent page before it ever paints. */
static bool is_modal(Mode m) {
    return m == Mode::WifiModal || m == Mode::ModelsModal;
}

/* ─── Click handler ────────────────────────────────────────────────
 *
 * Bound to every actionable widget. user_data carries the TouchResult
 * (cast to a uintptr_t-sized pointer); for SwitchWifi specifically
 * the SSID is also read out of the button's child label.
 *
 * Clicks land on press-release; LVGL's default click feedback (style
 * change on press) is what the user sees during the ~300 ms partial
 * refresh. */
static void on_click(lv_event_t *ev) {
    TouchResult action = (TouchResult)(uintptr_t)lv_event_get_user_data(ev);
    if (action == TouchResult::WifiSwitch) {
        /* Read SSID from the bound label (set when the row was built). */
        lv_obj_t *target = (lv_obj_t *)lv_event_get_target(ev);
        const char *ssid = (const char *)lv_obj_get_user_data(target);
        if (ssid) {
            snprintf(s_picked_ssid, sizeof(s_picked_ssid), "%s", ssid);
        }
        s_pending_action = action;
        return;
    }
    if (action == TouchResult::SwitchWifi) {
        /* Internal: push the WifiModal screen. main.cpp doesn't see
         * this. We swap s_mode here and use the modal-push flag so
         * tick() renders the new screen on the next pass without
         * treating the latch as a PWR-advance. */
        s_mode = Mode::WifiModal;
        s_entered_mode_us = esp_timer_get_time();
        s_modal_push_pending.store(true, std::memory_order_release);
        s_press_pending.store(true, std::memory_order_release);
        s_pending_action = TouchResult::None;
        return;
    }
    if (action == TouchResult::OpenModels) {
        /* Internal: push the ModelsModal screen, same shape as
         * SwitchWifi. main.cpp doesn't see this. */
        s_mode = Mode::ModelsModal;
        s_entered_mode_us = esp_timer_get_time();
        s_modal_push_pending.store(true, std::memory_order_release);
        s_press_pending.store(true, std::memory_order_release);
        s_pending_action = TouchResult::None;
        return;
    }
    if (action == TouchResult::CycleVoiceModel ||
        action == TouchResult::CycleTextModel) {
        /* Internal: advance the model pref + relabel the row in place.
         * We're inside LVGL's dispatcher (main task), so touching the
         * label here is safe. Reset the inactivity timer so a few taps
         * to land on the right model don't bounce back to Live. */
        char m[48];
        if (action == TouchResult::CycleVoiceModel) {
            model_prefs_cycle_voice();
            model_prefs_voice(m, sizeof(m));
        } else {
            model_prefs_cycle_text();
            model_prefs_text(m, sizeof(m));
        }
        lv_obj_t *btn = (lv_obj_t *)lv_event_get_target(ev);
        lv_obj_t *lab = lv_obj_get_child(btn, 0);
        if (lab) {
            char buf[64];
            if (action == TouchResult::CycleVoiceModel)
                snprintf(buf, sizeof(buf), "Voice: %s", m);
            else
                snprintf(buf, sizeof(buf), "Text: %s", m);
            lv_label_set_text(lab, buf);
        }
        s_entered_mode_us = esp_timer_get_time();
        s_pending_action = TouchResult::None;
        return;
    }
    s_pending_action = action;
}

/* ─── Builders ─────────────────────────────────────────────────────
 *
 * Each lazily creates an lv_obj_t* screen tree on first entry. Once
 * built, subsequent visits just refresh the dynamic labels (Info)
 * and re-attach to the active screen. Building is cheap (~10 ms
 * for ~6 widgets) but rebuilding on every entry would burn PSRAM
 * via LVGL's allocator churn, hence the cache. */

/* Single-screen Menu (v0.1.7): the info block sits at the top, then
 * the action buttons below in a vertical flex. Total content is taller
 * than 200 px (info ~70 px + 7 buttons × ~32 px); LVGL gives us
 * touch-scroll for free on the parent screen because the children
 * overflow.
 *
 * The lazily-built screen is reused across menu entries; only the
 * info label is refreshed live (RAM/PSRAM/batt change ticked into
 * the same widget). Action rows don't change, so we don't rebuild
 * them. */

static const char *phase_label(int phase) {
    switch (phase) {
        case 0:  return "connecting";
        case 1:  return "online";
        case 2:  return "offline";
        default: return "?";
    }
}

/* P2 (settings) header — the network / device-state info. */
static void refresh_info(void) {
    if (!s_info_label) return;
    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t free_psr = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    char buf[200];
    /* Compact 3-line header: pet/fw, net/ip, ssid+batt+ram. Keeps the
     * actions visible above the fold; full-width wraps on long ssids. */
    snprintf(buf, sizeof(buf),
        "%s  %s\n"
        "%s  %s\n"
        "%.16s  %d%%  %uK",
        s_pet_name[0] ? s_pet_name : "?",
        s_version[0]  ? s_version  : "?",
        phase_label(s_net_phase),
        s_ip_str[0] ? s_ip_str : "-",
        s_ssid[0]   ? s_ssid   : "-",
        s_batt_pct,
        (unsigned)(free_psr / 1024));
    (void)free_int;
    lv_label_set_text(s_info_label, buf);
}

/* P1 (kid) header — the pet's mood + stats. Caller formats the
 * string; we just blit it. Empty payload renders a tactful fallback
 * so the page isn't blank before the first tick. */
static void refresh_pet_status(void) {
    if (!s_pet_status_label) return;
    lv_label_set_text(s_pet_status_label,
        s_pet_status[0] ? s_pet_status : "Mochi");
}

/* Add a full-width tappable row to a vertical container. 28 px tall
 * — generous for finger taps. Pagination (PWR cycles MenuP1 →
 * MenuP2 → Live) means we don't need to cram many rows onto one
 * page, so each row gets the room it deserves. */
static lv_obj_t *add_row(lv_obj_t *parent, const char *label,
                         TouchResult action, const char *ssid_payload) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 28);
    lv_obj_set_style_pad_all(btn, 2, 0);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, label);
    lv_obj_center(lab);
    lv_obj_set_user_data(btn, (void *)ssid_payload);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED,
        (void *)(uintptr_t)action);
    return btn;
}

/* Common: configure a screen as a non-scrolling vertical flex with
 * tight padding so 4 × 28 px buttons + (optional) info header fit
 * within 200 px. Used by both menu pages and the wifi modal. */
static void config_menu_screen(lv_obj_t *scr) {
    lv_obj_set_layout(scr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(scr, 2, 0);
    lv_obj_set_style_pad_gap(scr, 2, 0);
    /* No scroll: pagination replaces it. The FT6336 can't surface
     * the intra-press position movement LVGL needs to disambiguate
     * a drag from a held tap, so any "scroll" attempt fires a
     * click on whichever button the finger happens to be over. */
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

/* Page 1: kid-facing main page. Pet-status header at the top, then
 * the safe exploratory actions (Memories, Places, Go home). No
 * configuration knobs here — those live one PWR×2 deeper on P2.
 * 1 status line + 3 × 28 px buttons + gaps ≈ 100 px. */
static void build_menu_p1(void) {
    if (s_menu_p1_scr) return;
    s_menu_p1_scr = lv_obj_create(nullptr);
    config_menu_screen(s_menu_p1_scr);

    /* Mood + stats — populated once on entry by refresh_pet_status().
     * Caller passes the formatted string; we just render it. */
    s_pet_status_label = lv_label_create(s_menu_p1_scr);
    lv_label_set_long_mode(s_pet_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_pet_status_label, lv_pct(98));
    lv_obj_set_style_text_align(s_pet_status_label, LV_TEXT_ALIGN_CENTER, 0);

    /* Memories + Places are placeholders: main.cpp surfaces a "not
     * yet" toast on tap so the page has shape while the underlying
     * data (memory ledger / world places list) is wired up. */
    add_row(s_menu_p1_scr, "Memories", TouchResult::Memories, nullptr);
    add_row(s_menu_p1_scr, "Places",   TouchResult::Places,   nullptr);
    add_row(s_menu_p1_scr, "Go home",  TouchResult::GoHome,   nullptr);
}

/* Page 2: settings — network info header + the configuration actions
 * (Switch WiFi → modal, OpenAI key). One PWR×2 into the menu from
 * Live, so the kid won't land here by accident. ~52 px header + 2 ×
 * 28 px + gaps ≈ 115 px. */
static void build_menu_p2(void) {
    if (s_menu_p2_scr) return;
    s_menu_p2_scr = lv_obj_create(nullptr);
    config_menu_screen(s_menu_p2_scr);

    lv_obj_t *title = lv_label_create(s_menu_p2_scr);
    lv_label_set_text(title, "SETTINGS  (PWR×1 exits)");

    /* Network/version/IP header — populated once on entry by refresh_info(). */
    s_info_label = lv_label_create(s_menu_p2_scr);
    lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_info_label, lv_pct(98));

    add_row(s_menu_p2_scr, "Switch WiFi", TouchResult::SwitchWifi,    nullptr);
    add_row(s_menu_p2_scr, "OpenAI key",  TouchResult::OpenKeyPortal, nullptr);
    add_row(s_menu_p2_scr, "AI models",   TouchResult::OpenModels,    nullptr);
}

/* AI models modal — cycle the voice + text model selections. Rebuilt
 * on each entry so the rows show the current model_prefs values; tapping
 * a row cycles it (handled in on_click, which relabels in place). */
static void build_models(void) {
    if (s_models_scr) {
        lv_obj_del(s_models_scr);
        s_models_scr = nullptr;
    }
    s_models_scr = lv_obj_create(nullptr);
    config_menu_screen(s_models_scr);

    lv_obj_t *title = lv_label_create(s_models_scr);
    lv_label_set_text(title, "AI MODELS  (tap to cycle)");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    char vm[48], tm[48];
    model_prefs_voice(vm, sizeof(vm));
    model_prefs_text(tm, sizeof(tm));
    char vrow[64], trow[64];
    snprintf(vrow, sizeof(vrow), "Voice: %s", vm);
    snprintf(trow, sizeof(trow), "Text: %s", tm);
    add_row(s_models_scr, vrow, TouchResult::CycleVoiceModel, nullptr);
    add_row(s_models_scr, trow, TouchResult::CycleTextModel, nullptr);

    lv_obj_t *hint = lv_label_create(s_models_scr);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(98));
    lv_label_set_text(hint, "voice = realtime · text = consolidation");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

/* Page 3: destructive territory. Two PWR×2 into the menu from Live;
 * single-tap escapes back. A "RISK" header makes the page register
 * different from P1/P2 at a glance. 1 header + 4 × 28 px + gaps ≈
 * 130 px. */
static void build_menu_p3(void) {
    if (s_menu_p3_scr) return;
    s_menu_p3_scr = lv_obj_create(nullptr);
    config_menu_screen(s_menu_p3_scr);

    lv_obj_t *title = lv_label_create(s_menu_p3_scr);
    lv_label_set_text(title, "RISK  (PWR×1 exits)");
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    add_row(s_menu_p3_scr, "Update now",  TouchResult::UpdateNow,  nullptr);
    add_row(s_menu_p3_scr, "Add WiFi",    TouchResult::ChangeWifi, nullptr);
    add_row(s_menu_p3_scr, "Forget WiFi", TouchResult::ForgetWifi, nullptr);
    add_row(s_menu_p3_scr, "Re-pair",     TouchResult::RePair,     nullptr);
}

/* WiFi modal — built fresh on each entry because the stored-network
 * list is dynamic (NVS may have grown/shrunk since last entry).
 * Earlier dev_menu state cleared button rects on re-render; LVGL's
 * version replaces the whole screen tree. */
static void build_wifi(void) {
    if (s_wifi_scr) {
        lv_obj_del(s_wifi_scr);
        s_wifi_scr = nullptr;
    }
    s_wifi_scr = lv_obj_create(nullptr);
    lv_obj_set_layout(s_wifi_scr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_wifi_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wifi_scr,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_wifi_scr, 2, 0);
    lv_obj_set_style_pad_gap(s_wifi_scr, 1, 0);
    lv_obj_set_scrollbar_mode(s_wifi_scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_wifi_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_wifi_scr);
    lv_label_set_text(title, "SWITCH WIFI");

    const size_t stored = nvs_creds_count();
    int rendered = 0;
    /* Borrow a static buffer per row for the SSID payload (lifetime
     * = until the next build_wifi). 8 networks max; nvs_creds_count
     * is implicitly bounded by the NVS-creds module's own cap. */
    static char s_row_ssids[8][MOCHI_WIFI_SSID_MAX + 1];
    static char s_row_labels[8][MOCHI_WIFI_SSID_MAX + 4];
    for (size_t i = 0; i < stored && i < 8; i++) {
        struct mochi_wifi_creds c = {};
        if (!nvs_creds_load_at(i, &c)) continue;
        const bool is_cur = s_ssid[0] &&
            strncmp(c.ssid, s_ssid, MOCHI_WIFI_SSID_MAX) == 0;
        snprintf(s_row_ssids[rendered], sizeof(s_row_ssids[rendered]),
            "%s", c.ssid);
        snprintf(s_row_labels[rendered], sizeof(s_row_labels[rendered]),
            "%s%s", is_cur ? "* " : "  ", c.ssid);
        add_row(s_wifi_scr, s_row_labels[rendered],
            TouchResult::WifiSwitch, s_row_ssids[rendered]);
        rendered++;
    }
    if (rendered == 0) {
        lv_obj_t *empty = lv_label_create(s_wifi_scr);
        lv_label_set_text(empty, "no saved networks\nAdd WiFi on the next page");
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    }
}

/* ─── dispatch ─────────────────────────────────────────────────────
 *
 * With LVGL hit-testing in charge, dispatch_touch is now a thin
 * adapter: it just exchanges the s_pending_action latch. The caller
 * (main.cpp) feeds in (x, y) but they're unused here — LVGL's indev
 * already has the position from its own poll. */
TouchResult dispatch_touch(int /*x*/, int /*y*/) {
    if (s_mode == Mode::Live) return TouchResult::None;
    /* The dedicated lv_task runs lv_timer_handler at ~33 Hz, so any
     * click LVGL was about to fire has likely already landed in the
     * on_click handler by the time main.cpp's touch loop calls us.
     * Briefly yield to let the dispatcher drain anything it had
     * queued at exactly this instant (touch::wait_event woke us up
     * before lv_task got its slice), then read the latch. */
    vTaskDelay(pdMS_TO_TICKS(40));
    TouchResult r = s_pending_action;
    s_pending_action = TouchResult::None;
    return r;
}

/* ─── Render ───────────────────────────────────────────────────────*/

static void render_mode(Mode m) {
    /* Lock against the dispatcher task (which holds this lock while
     * running lv_timer_handler). Without it, build_menu's lv_obj
     * creation can race with the renderer mid-frame and crash inside
     * lv_obj_pos.c. */
    lvgl_port_lock();
    /* Clear any stale pending_action stamped on the previous screen.
     * Without this, a click queued while a slow handler was running
     * (e.g. the Memories/Places toast's 1.2 s vTaskDelay during
     * which the LVGL dispatcher kept firing) can leak into the next
     * screen's dispatch_touch and execute the wrong action. Observed
     * on hardware: a tap on MenuP3's "Update" or P2's "AI models"
     * was committing GoHome (stamped earlier on MenuP1). */
    s_pending_action = TouchResult::None;
    switch (m) {
        case Mode::MenuP1:
            build_menu_p1();
            refresh_pet_status();
            lv_screen_load(s_menu_p1_scr);
            break;
        case Mode::MenuP2:
            build_menu_p2();
            refresh_info();
            lv_screen_load(s_menu_p2_scr);
            break;
        case Mode::MenuP3:
            build_menu_p3();
            lv_screen_load(s_menu_p3_scr);
            break;
        case Mode::WifiModal:
            build_wifi();
            lv_screen_load(s_wifi_scr);
            break;
        case Mode::ModelsModal:
            build_models();
            lv_screen_load(s_models_scr);
            break;
        case Mode::Live:
        default:
            /* Live is rendered by main.cpp's render_resting on the
             * next tick after we exit; we don't load any LVGL
             * screen. */
            break;
    }
    /* Force a full e-paper refresh on the first frame of each new
     * screen so accumulated ghost from the previous mode clears. */
    lvgl_port_force_full_refresh();
    lvgl_port_unlock();
}

bool tick(epaper_driver_display * /*epd*/, bool /*paired*/,
          const char *pet_name, const char *version,
          const char *ip_str, const char *ssid,
          int net_phase, int batt_pct,
          const char *pet_status) {
    const int64_t now_us = esp_timer_get_time();
    bool changed = false;

    /* Cache the freshest data so the WifiModal push (which doesn't
     * receive params) can still see current values. Snapshot before
     * we decide whether to render so the snapshot reflects the most
     * recent main-loop state. */
    if (pet_name)   snprintf(s_pet_name,   sizeof(s_pet_name),   "%s", pet_name);
    if (version)    snprintf(s_version,    sizeof(s_version),    "%s", version);
    if (ip_str)     snprintf(s_ip_str,     sizeof(s_ip_str),     "%s", ip_str);
    if (ssid)       snprintf(s_ssid,       sizeof(s_ssid),       "%s", ssid);
    if (pet_status) snprintf(s_pet_status, sizeof(s_pet_status), "%s", pet_status);
    s_net_phase = net_phase;
    s_batt_pct  = batt_pct;

    /* Consume any presses the watcher latched since the last tick.
     * exchange returns the prior value and resets the flag — so a
     * burst of fast presses inside one tick still advances the
     * wheel exactly once. */
    const bool pressed = s_press_pending.exchange(false, std::memory_order_acq_rel);
    const bool modal_push =
        s_modal_push_pending.exchange(false, std::memory_order_acq_rel);

    if (pressed) {
        /* Disambiguate: a click handler that pushed a modal sets
         * s_modal_push_pending (alongside the press latch); tick()
         * then renders the new s_mode as-is. A genuine PWR press
         * arrives without the modal flag and advances the wheel.
         *
         * Earlier we tried gating advance() on a wall-clock window
         * since on_click set s_entered_mode_us, but main.cpp's 1 Hz
         * touch-poll cadence frequently pushed elapsed time past any
         * reasonable threshold — the modal would render then
         * immediately advance back to its parent the next tick,
         * never showing. The explicit flag has no time dependency. */
        if (!modal_push) {
            const Mode next = advance(s_mode);
            ESP_LOGI(TAG, "PWR: %s → %s", mode_name(s_mode), mode_name(next));
            s_mode = next;
        } else {
            ESP_LOGI(TAG, "modal push → %s", mode_name(s_mode));
        }
        s_entered_mode_us = now_us;
        render_mode(s_mode);
        changed = true;
    }
    /* Menu headers are a snapshot taken at entry (render_mode) — we
     * deliberately do NOT re-refresh pet stats / network info on every
     * tick, so the figures the user reads stay stable while a page is
     * up rather than flickering with each partial refresh. */

    if (s_mode != Mode::Live &&
        (now_us - s_entered_mode_us) >= INACTIVITY_US && !changed) {
        ESP_LOGI(TAG, "inactivity timeout → live");
        s_mode = Mode::Live;
        changed = true;
    }
    return changed;
}

}  /* namespace dev_menu */
