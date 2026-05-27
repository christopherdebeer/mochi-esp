#include "dev_menu.h"

#include <stdio.h>
#include <string.h>
#include <atomic>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

#include "lvgl.h"
#include "lvgl_port.h"

#include "board_pins.h"
#include "epd_ui.h"
#include "nvs_creds.h"

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
static lv_obj_t            *s_menu_scr      = nullptr;
static lv_obj_t            *s_info_label    = nullptr;   /* lives inside s_menu_scr */
static lv_obj_t            *s_wifi_scr      = nullptr;

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

static const char *mode_name(Mode m) {
    switch (m) {
        case Mode::Live:      return "live";
        case Mode::Menu:      return "menu";
        case Mode::WifiModal: return "wifi_modal";
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

/* Advance:
 *   Live      → Menu (entered via PWR double-tap from main.cpp)
 *   Menu      → Live (PWR exits the menu — no slot cycling now)
 *   WifiModal → Menu (PWR closes the modal back to the parent menu)
 *
 * Pre-v0.1.7 there was an Info ↔ Actions cycle; collapsing them
 * into one Menu screen made that loop pointless, so PWR now means
 * "back" once you're past Live. */
static Mode advance(Mode m) {
    switch (m) {
        case Mode::Live:      return Mode::Menu;
        case Mode::Menu:      return Mode::Live;
        case Mode::WifiModal: return Mode::Menu;
        default:              return Mode::Live;
    }
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
         * this. We swap the LVGL active screen here and bump
         * s_entered_mode_us so the inactivity timer resets. */
        s_mode = Mode::WifiModal;
        s_entered_mode_us = esp_timer_get_time();
        s_press_pending.store(true, std::memory_order_release);
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

/* Add a full-width tappable row to a vertical container. */
static lv_obj_t *add_row(lv_obj_t *parent, const char *label,
                         TouchResult action, const char *ssid_payload) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, 28);
    lv_obj_t *lab = lv_label_create(btn);
    lv_label_set_text(lab, label);
    lv_obj_center(lab);
    lv_obj_set_user_data(btn, (void *)ssid_payload);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED,
        (void *)(uintptr_t)action);
    return btn;
}

static void build_menu(void) {
    if (s_menu_scr) return;
    s_menu_scr = lv_obj_create(nullptr);
    lv_obj_set_layout(s_menu_scr, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_menu_scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_menu_scr,
        LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(s_menu_scr, 4, 0);
    lv_obj_set_style_pad_gap(s_menu_scr, 4, 0);

    /* Info header — multi-line label rebuilt by refresh_info() each
     * tick so RAM / PSRAM / batt readings stay fresh while the menu
     * is up. Compact format (3 lines of 2 fields each) so the action
     * list below stays mostly visible without scrolling. */
    s_info_label = lv_label_create(s_menu_scr);
    lv_label_set_long_mode(s_info_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_info_label, lv_pct(98));

    /* Action buttons. Kept full-width so the 200 px panel turns each
     * into a generous tap target. */
    add_row(s_menu_scr, "Switch WiFi",   TouchResult::SwitchWifi,    nullptr);
    add_row(s_menu_scr, "Add WiFi",      TouchResult::ChangeWifi,    nullptr);
    add_row(s_menu_scr, "Forget WiFi",   TouchResult::ForgetWifi,    nullptr);
    add_row(s_menu_scr, "Update now",    TouchResult::UpdateNow,     nullptr);
    add_row(s_menu_scr, "Re-pair",       TouchResult::RePair,        nullptr);
    add_row(s_menu_scr, "OpenAI key",    TouchResult::OpenKeyPortal, nullptr);
    add_row(s_menu_scr, "Go home",       TouchResult::GoHome,        nullptr);
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
    lv_obj_set_style_pad_all(s_wifi_scr, 4, 0);
    lv_obj_set_style_pad_gap(s_wifi_scr, 4, 0);

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
        lv_label_set_text(empty, "no saved networks\nAdd via Actions");
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
    /* Run a tick so any click event the indev has already enqueued
     * gets dispatched to the on_click handler above before we read
     * s_pending_action. main.cpp's tick happens earlier in the loop,
     * but a touch arrives later in the same iteration — without this
     * extra tick, the action wouldn't surface until the next loop. */
    lvgl_port_tick();
    TouchResult r = s_pending_action;
    s_pending_action = TouchResult::None;
    return r;
}

/* ─── Render ───────────────────────────────────────────────────────*/

static void render_mode(Mode m) {
    switch (m) {
        case Mode::Menu:
            build_menu();
            refresh_info();
            lv_screen_load(s_menu_scr);
            break;
        case Mode::WifiModal:
            build_wifi();
            lv_screen_load(s_wifi_scr);
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
}

bool tick(epaper_driver_display * /*epd*/, bool /*paired*/,
          const char *pet_name, const char *version,
          const char *ip_str, const char *ssid,
          int net_phase, int batt_pct) {
    const int64_t now_us = esp_timer_get_time();
    bool changed = false;

    /* Cache the freshest data so the WifiModal push (which doesn't
     * receive params) can still see current values. Snapshot before
     * we decide whether to render so the snapshot reflects the most
     * recent main-loop state. */
    if (pet_name) snprintf(s_pet_name, sizeof(s_pet_name), "%s", pet_name);
    if (version)  snprintf(s_version,  sizeof(s_version),  "%s", version);
    if (ip_str)   snprintf(s_ip_str,   sizeof(s_ip_str),   "%s", ip_str);
    if (ssid)     snprintf(s_ssid,     sizeof(s_ssid),     "%s", ssid);
    s_net_phase = net_phase;
    s_batt_pct  = batt_pct;

    /* Consume any presses the watcher latched since the last tick.
     * exchange returns the prior value and resets the flag — so a
     * burst of fast presses inside one tick still advances the
     * wheel exactly once. The SwitchWifi click handler also sets
     * this flag (already having mutated s_mode) to force an
     * immediate render of the modal on the same tick. */
    const bool pressed = s_press_pending.exchange(false, std::memory_order_acq_rel);

    if (pressed) {
        /* on_click already mutated s_mode for the SwitchWifi case
         * (Mode::WifiModal). For the wheel-advance case, advance
         * here. */
        if (s_mode != Mode::WifiModal ||
            (now_us - s_entered_mode_us) > 50 * 1000) {
            const Mode next = advance(s_mode);
            ESP_LOGI(TAG, "PWR: %s → %s", mode_name(s_mode), mode_name(next));
            s_mode = next;
        } else {
            ESP_LOGI(TAG, "modal push → wifi_modal");
        }
        s_entered_mode_us = now_us;
        render_mode(s_mode);
        changed = true;
    } else if (s_mode == Mode::Menu) {
        /* Refresh the info header without a screen swap so RAM/PSRAM/
         * batt readings stay live while the menu is up. Cheap; LVGL
         * only re-renders the label's dirty area. The action buttons
         * below are static — not rebuilt. */
        refresh_info();
    }

    if (s_mode != Mode::Live &&
        (now_us - s_entered_mode_us) >= INACTIVITY_US && !changed) {
        ESP_LOGI(TAG, "inactivity timeout → live");
        s_mode = Mode::Live;
        changed = true;
    }
    return changed;
}

}  /* namespace dev_menu */
