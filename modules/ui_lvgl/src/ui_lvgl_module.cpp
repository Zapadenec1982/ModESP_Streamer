/**
 * @file ui_lvgl_module.cpp
 * @brief Lifecycle and the main-loop side: bind backends, start LVGL, mirror state, drain
 *        requests. The screens themselves live in ui_shell.cpp and screen_*.cpp.
 *
 * Threading contract (unchanged):
 *   - on_update() runs in the 100 Hz main loop and never blocks. It is the ONLY writer of
 *     SharedState here; anything the LVGL task wants changed arrives through the request
 *     ring.
 *   - Every LVGL call happens in the esp_lvgl_port task, from the 40 ms refresh timer or an
 *     event callback — except the one-shot build and a rebuild for a new palette or language
 *     (rebuild_look), which take lvgl_port_lock() explicitly.
 */

#include "ui_common.h"
#include "i18n.h"
#include "ui_screenshot.h"
#include "ui_scroll_bench.h"
#include "ui_web_media.h"
#include "ppa_flush.h"
#include "cover.h"
#include "playlist.h"
#include "stations.h"
#include "eq_store.h"
#include "out_caps.h"
#include "modesp/liveness.h"
#include "modesp/utf8.h"
#include "modesp/hal/driver_registry.h"
#include "modesp/hal/driver_manager.h"
#include "modesp/hal/hal.h"
#include "modesp/net/wifi_service.h"
#include "bt_settings.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <strings.h>

static const char* TAG = "UiLvgl";

/// Opaque to the header; holds what the LVGL side needs between calls.
struct UiScreen {
    UiCtx         ctx{};
    lv_display_t* disp  = nullptr;
    lv_indev_t*   indev = nullptr;
    lv_timer_t*   timer = nullptr;
    int32_t       enc_last = 0;     ///< last encoder position this side has seen
    uint32_t      enc_seen = 0;     ///< last click/hold sequence consumed
    bool          dot_on = false;
    lv_obj_t*     dot   = nullptr;
};

namespace {

UiScreen s_ui;

// Draw-buffer size in pixels of the panel's native width. Partial buffers in PSRAM, copied to
// the DPI frame buffer by DMA2D — 2 × 480 × 100 × 2 B ≈ 192 KB, none of it internal/DMA SRAM
// (+ one more 96 KB PSRAM buffer for the PPA when the UI is rotated). LVGL re-slices this into
// rows itself, so the count stays right after rotation swaps the logical width.
constexpr uint32_t DRAW_BUF_LINES = 100;

void refresh_timer_cb(lv_timer_t* t) {
    auto* self = static_cast<UiLvglModule*>(lv_timer_get_user_data(t));
    if (self) self->lvgl_refresh();
}

/// Proof that the LVGL task is still running its timers.
///
/// A 1 Hz timer rather than a counter inside the refresh callback: the refresh timer can be
/// legitimately blocked on lvgl_port_lock() for a while, and standby stops the screen
/// changing but not the task. This fires whatever the screen is doing, and stops only when
/// lv_timer_handler() stops being called — which is the fault worth a log line.
modesp::liveness::Counter s_lvgl_beat{0};

void liveness_timer_cb(lv_timer_t*) { modesp::liveness::tick(s_lvgl_beat); }

/// The DPI panel's "copy finished" callback, in IRAM.
///
/// CONFIG_LCD_DSI_ISR_CACHE_SAFE (sdkconfig.board) keeps the frame restart running through
/// flash writes, and with it IDF refuses any DPI callback that is not in IRAM
/// (esp_lcd_panel_dpi.c, esp_lcd_dpi_panel_register_event_callbacks). esp_lvgl_port's own
/// callback is not, so its registration fails and LVGL would wait for a flush that never
/// completes. This one replaces it and does the same thing. The pointer has to be in IRAM;
/// the body may call into flash because it is only ever called from the DMA2D copy
/// interrupt, which is not IRAM-safe here (CONFIG_DMA2D_ISR_IRAM_SAFE=n) and so never runs
/// with the cache off, or from the drawing task itself. IDF also refuses a user context
/// outside internal RAM, and the LVGL display object lives in PSRAM, so the display is
/// handed over through this internal static instead (the first image, 04.10.2026, passed
/// it as the context, failed registration and boot-looped on the task watchdog).
lv_display_t* s_flush_disp = nullptr;

IRAM_ATTR bool dpi_flush_done_cb(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
    lv_display_flush_ready(s_flush_disp);
    return false;
}

void log_heap(const char* when) {
    ESP_LOGI(TAG, "heap %s: internal %u | DMA %u (largest %u) | PSRAM %u",
             when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

} // namespace

UiLvglModule::UiLvglModule()
    : BaseModule("ui_lvgl", modesp::ModulePriority::LOW)
{}

// ═══════════════════════════════════════════════════════════════
// Bind: LCD-family backends from the registry, inputs from DriverManager
// ═══════════════════════════════════════════════════════════════

void UiLvglModule::on_bind(modesp::DriverManager& drivers,
                           const modesp::BindingTable& bindings, modesp::HAL& hal) {
    for (const auto& b : bindings.bindings) {
        if (!(b.module_name == "ui_lvgl")) continue;           // only our bindings
        const char* type = b.driver_type.c_str();
        if (!modesp::DriverRegistry::has_lcd(type)) continue;  // sensors are DriverManager's
        auto* port = modesp::DriverRegistry::create_lcd(type, b, hal);
        if (!port) {
            ESP_LOGW(TAG, "backend '%s' [hw=%s, role=%s] create failed",
                     type, b.hardware_id.c_str(), b.role.c_str());
            continue;
        }
        if (auto* p = port->as_panel()) {
            if (panel_) ESP_LOGW(TAG, "second panel '%s' ignored (one display)", b.role.c_str());
            else { panel_ = p; ESP_LOGI(TAG, "panel '%s' [hw=%s]", type, b.hardware_id.c_str()); }
        }
        if (auto* t = port->as_touch()) {
            if (touch_) ESP_LOGW(TAG, "second touch '%s' ignored", b.role.c_str());
            else { touch_ = t; ESP_LOGI(TAG, "touch '%s' [hw=%s]", type, b.hardware_id.c_str()); }
        }
    }

    encoder_    = drivers.find_sensor("encoder_main");
    encoder_sw_ = drivers.find_sensor("encoder_sw");
    ESP_LOGI(TAG, "bound: panel=%s touch=%s encoder=%s button=%s",
             panel_ ? "yes" : "no", touch_ ? "yes" : "no",
             encoder_ ? encoder_->type() : "no", encoder_sw_ ? encoder_sw_->type() : "no");
}

// ═══════════════════════════════════════════════════════════════
// Init: panel → touch → LVGL port → screen
// ═══════════════════════════════════════════════════════════════

bool UiLvglModule::on_init() {
    state_set("ui.screen", "boot");
    state_set("ui.encoder_pos", (int32_t)0);
    state_set("ui.encoder_sw", false);
    state_set("ui.touch_down", false);
    state_set("ui.touch_x", (int32_t)0);
    state_set("ui.touch_y", (int32_t)0);
    state_set("ui.cover", (int32_t)0);
    state_set("ui.cover_rgb", (int32_t)0);
    // GET /api/library and /api/cover (A4, A1): before the screen, which they do not need.
    ui_web_media_init();

    // Palette and language are persisted settings, so they have to be chosen before anything
    // is built — every widget bakes its colours in at construction, every label its text.
    const int32_t th = read_int("ui.theme", 0);
    ui_theme_set((UiThemeId)(th >= 0 && th < (int)UiThemeId::COUNT ? th : 0));
    state_set("ui.theme", (int32_t)ui_theme_id());
    if (!i18n::set_lang_index(read_int("ui.lang", 0))) i18n::set_lang(i18n::Lang::UK);
    state_set("ui.lang", (int32_t)i18n::lang());
    want_theme_ = (int32_t)ui_theme_id();
    want_lang_  = (int32_t)i18n::lang();

    if (!panel_) {
        ESP_LOGW(TAG, "no lcd binding — screen disabled, inputs only");
        return true;
    }
    if (!panel_->init()) {
        ESP_LOGE(TAG, "panel init failed — screen disabled");
        panel_ = nullptr;
        return true;   // the rest of the system does not depend on the screen
    }
    if (touch_ && !touch_->init()) {
        ESP_LOGW(TAG, "touch init failed — continuing without touch");
        touch_ = nullptr;
    }

    log_heap("before lvgl_port_init");
    if (!start_lvgl()) {
        ESP_LOGE(TAG, "LVGL start failed — screen disabled");
        return true;
    }
    log_heap("after LVGL screen");

    published_screen_ = ui_shell_current();
    state_set("ui.screen", published_screen_);
    const auto geo = panel_->geometry();
    ui_screenshot_init(panel_->panel(), geo.h_res, geo.v_res, geo.color_bits, geo.rotation);
    ui_scroll_bench_init();
    ESP_LOGI(TAG, "Initialized: panel %ux%u, UI %ldx%ld (rotation %u), touch %s, theme '%s'",
             (unsigned)geo.h_res, (unsigned)geo.v_res,
             (long)lv_display_get_horizontal_resolution(s_ui.disp),
             (long)lv_display_get_vertical_resolution(s_ui.disp),
             (unsigned)geo.rotation, touch_ ? "yes" : "no", ui_theme().name);
    return true;
}

bool UiLvglModule::start_lvgl() {
    const auto geo = panel_->geometry();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    // Core 0 at the main loop's own priority (1), time-sliced with it. On core 1 the UI drew
    // under the audio task (22), the satellite reader (23) and AirPlay's three, and a
    // scrolling list ran at 1.5-4 frames/s; on core 0 the same list ran 3.5-5.5x faster
    // (/api/ui/scrollbench, 05.10.2026). Priority 3 there was faster still, but above the
    // main loop it held the 100 Hz loop off for up to ~90 ms a frame; at 1 the longest gap
    // was ~45 ms, what it already was with the UI on the other core.
    port_cfg.task_priority    = 1;
    port_cfg.task_stack       = 16 * 1024;
    port_cfg.task_affinity    = 0;
    port_cfg.task_max_sleep_ms = 500;
    port_cfg.task_stack_caps  = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;   // never touches flash ops
    port_cfg.timer_period_ms  = 5;
    esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init: %s", esp_err_to_name(err));
        return false;
    }
    lvgl_running_ = true;

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle     = panel_->io();
    disp_cfg.panel_handle  = panel_->panel();
    disp_cfg.control_handle = nullptr;
    disp_cfg.buffer_size   = (uint32_t)geo.h_res * DRAW_BUF_LINES;
    disp_cfg.double_buffer = true;
    disp_cfg.hres          = geo.h_res;
    disp_cfg.vres          = geo.v_res;
    disp_cfg.monochrome    = false;
    disp_cfg.rotation.swap_xy  = false;             // unused with sw_rotate (the port returns
    disp_cfg.rotation.mirror_x = false;             // early before touching esp_lcd_panel_*)
    disp_cfg.rotation.mirror_y = false;
    disp_cfg.color_format  = (geo.color_bits == 24) ? LV_COLOR_FORMAT_RGB888 : LV_COLOR_FORMAT_RGB565;
    // Rotated, with the PPA at hand: the PPA turns each block straight into the frame buffer
    // while LVGL draws the next (ppa_flush.cpp). LVGL keeps drawing RGB888: XRGB8888, which the
    // PPA would pack to RGB888 on the way, was measured slower here — 80 against 76 ms a frame of
    // a scrolling list (05.10.2026), the extra byte a pixel costing PSRAM more than LVGL's 32-bit
    // paths save.
    const bool direct = geo.rotation != 0 && geo.color_bits == 24 &&
                        ppa_flush_prepare(panel_->panel(), geo.h_res, geo.v_res);
    // esp_lvgl_port refuses DMA-capable draw buffers for anything but RGB565 ("not aligned
    // copy"). On the P4 the buffers are PSRAM either way, and PSRAM is DMA-reachable, so the
    // flag only changes what the port will accept.
    disp_cfg.flags.buff_dma    = (geo.color_bits == 24) ? 0 : 1;
    disp_cfg.flags.buff_spiram = 1;
    // A DPI panel cannot swap its own axes (esp_lcd_panel_swap_xy is unsupported there), so a
    // rotated UI is LVGL's job. With CONFIG_LVGL_PORT_ENABLE_PPA the port hands each flushed
    // block to the P4's PPA instead of rotating it on the CPU. Touch is NOT rotated by the
    // port — the touch controller's own swap/mirror flags (board.json) do that.
    disp_cfg.flags.sw_rotate   = (geo.rotation != 0) ? 1 : 0;
    disp_cfg.flags.swap_bytes  = 0;

    lvgl_port_display_dsi_cfg_t dsi_cfg = {};
    dsi_cfg.flags.avoid_tearing = 0;                // BSP: off during bring-up (no VSYNC blocking)

    // Held from before the display exists until the screen is built. lv_display_create marks
    // the refresh timer ready, and once the port lets go of its own (recursive) lock the LVGL
    // task may render and flush the first block — before the IRAM callback below replaces the
    // port's, which IDF refused (CONFIG_LCD_DSI_ISR_CACHE_SAFE). A copy finished in that gap
    // is a completion nobody hears: LVGL then waits in while(disp->flushing) for ever, holding
    // its lock, and this init blocks behind it before the task watchdog is armed to panic.
    // Pinned to the other core the gap was microseconds; a task free to run on this one made
    // it certain (05.10.2026, the boot hung in wait_for_flushing).
    if (!lvgl_port_lock(0)) return false;
    s_ui.disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!s_ui.disp) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed");
        return false;
    }
    {
        esp_lcd_dpi_panel_event_callbacks_t cbs = {};
        cbs.on_color_trans_done = dpi_flush_done_cb;
        s_flush_disp = s_ui.disp;
        err = esp_lcd_dpi_panel_register_event_callbacks(panel_->panel(), &cbs, nullptr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "DPI flush callback: %s", esp_err_to_name(err));
            // Without it no flush ever completes: take the display away before LVGL draws on
            // it, or the task spins in wait_for_flushing and the watchdog reboots the panel.
            lvgl_port_remove_disp(s_ui.disp);
            s_ui.disp = nullptr;
            s_flush_disp = nullptr;
            lvgl_port_unlock();
            return false;
        }
    }

    if (touch_ && touch_->handle()) {
        lvgl_port_touch_cfg_t touch_cfg = {};
        touch_cfg.disp   = s_ui.disp;
        touch_cfg.handle = touch_->handle();
        s_ui.indev = lvgl_port_add_touch(&touch_cfg);
        if (!s_ui.indev) ESP_LOGW(TAG, "lvgl_port_add_touch failed — no touch input");
    }

    if (geo.rotation != 0) {
        lv_display_rotation_t r = LV_DISPLAY_ROTATION_0;
        switch (geo.rotation) {
        case 90:  r = LV_DISPLAY_ROTATION_90;  break;
        case 180: r = LV_DISPLAY_ROTATION_180; break;
        case 270: r = LV_DISPLAY_ROTATION_270; break;
        default:  ESP_LOGW(TAG, "rotation %u not in {0,90,180,270} — ignored", (unsigned)geo.rotation);
        }
        lv_display_set_rotation(s_ui.disp, r);   // swaps the logical resolution too
        if (direct && !ppa_flush_install(s_ui.disp))
            ESP_LOGE(TAG, "direct PPA flush refused after the display was made for it");
    }
    if (ppa_flush_double()) {
        // Frames are paced by the panel now (a frame's first block waits for the last one to
        // reach the glass, every 16.5 ms at 34 MHz: 576 x 976 clocks), so LVGL's 33 ms timers
        // would only halve that. The touch is read as often, or a finger's scroll moves every
        // other frame.
        lv_timer_set_period(lv_display_get_refr_timer(s_ui.disp), 15);
        if (s_ui.indev) lv_timer_set_period(lv_indev_get_read_timer(s_ui.indev), 15);
    }
    s_ui.ctx.mod = this;
    ui_shell_build(lv_screen_active(), s_ui.ctx);
    build_screen();    // touch ring, on top of everything the shell made
    // 40 ms: fast enough that the meters read as ballistics and the touch ring feels attached
    // to the finger, cheap because every setter in a refresh is change-detected.
    s_ui.timer = lv_timer_create(refresh_timer_cb, 40, this);
    lv_timer_create(liveness_timer_cb, 1000, nullptr);
    lvgl_port_unlock();
    ui_ = &s_ui;
    // 5 s against a 1 Hz beat: four missed ticks before anyone is told.
    modesp::liveness::watch("lvgl", &s_lvgl_beat, 5000, this);
    // There is a screen to ask on now, so the satellite may ask before a phone pairs (link
    // protocol 1.8, host bit 7). Only here, after the shell is drawn: a panel without a
    // screen that said this would leave every pairing to time out.
    bt_settings::set_confirmer(true);
    return true;
}

/// The touch ring: bring-up aid and touch feedback, owned here rather than by a screen
/// because it has to sit above the shell's menu and scrim.
void UiLvglModule::build_screen() {
    const UiTheme& t = ui_theme();
    lv_obj_t* d = lv_obj_create(lv_screen_active());
    uih::bare(d);
    lv_obj_set_size(d, 30, 30);
    lv_obj_set_style_radius(d, 15, 0);
    lv_obj_set_style_border_width(d, 2, 0);
    lv_obj_set_style_border_color(d, ui_col(t.acc), 0);
    lv_obj_set_style_border_opa(d, LV_OPA_70, 0);
    lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    s_ui.dot = d;
    // It is over a list whenever a finger scrolls one: let the list shift under it anyway.
    ppa_flush_floating(d, true);
}

// ═══════════════════════════════════════════════════════════════
// LVGL-task side
// ═══════════════════════════════════════════════════════════════

void UiLvglModule::lvgl_refresh() {
    if (!ui_) return;
    portENTER_CRITICAL(&mux_);
    s_ui.ctx.s = snap_;
    portEXIT_CRITICAL(&mux_);
    s_ui.ctx.th = &ui_theme();

    // Encoder first: it may switch screens or open the quick menu, and the refresh below
    // should paint the result of that, not the state before it.
    const int32_t pos = s_ui.ctx.s.encoder_pos;
    const int32_t delta = pos - s_ui.enc_last;
    s_ui.enc_last = pos;
    uint8_t ev = 0;
    if (s_ui.ctx.s.enc_event_seq != s_ui.enc_seen) {
        s_ui.enc_seen = s_ui.ctx.s.enc_event_seq;
        ev = s_ui.ctx.s.enc_event;
    }
    if (delta || ev) ui_shell_encoder(delta, ev, s_ui.ctx);

    ui_shell_refresh(s_ui.ctx);

    // Touch ring. Polled rather than event-driven so it follows the finger everywhere,
    // including over widgets that consume their own press events.
    if (s_ui.indev) {
        const bool down = (lv_indev_get_state(s_ui.indev) == LV_INDEV_STATE_PRESSED);
        lv_point_t p = {0, 0};
        if (down) {
            lv_indev_get_point(s_ui.indev, &p);
            lv_obj_set_pos(s_ui.dot, p.x - 15, p.y - 15);
            if (!s_ui.dot_on) { lv_obj_remove_flag(s_ui.dot, LV_OBJ_FLAG_HIDDEN); s_ui.dot_on = true; }
        } else if (s_ui.dot_on) {
            lv_obj_add_flag(s_ui.dot, LV_OBJ_FLAG_HIDDEN);
            s_ui.dot_on = false;
        }
        lvgl_touch_event((int16_t)p.x, (int16_t)p.y, down);
    }
}

void UiLvglModule::lvgl_touch_event(int16_t x, int16_t y, bool down) {
    portENTER_CRITICAL(&mux_);
    if (down || snap_.touch_down) {          // only bump while pressed and once on release
        snap_.touch_x = x;
        snap_.touch_y = y;
        snap_.touch_down = down;
        snap_.touch_seq++;
    }
    portEXIT_CRITICAL(&mux_);
}

void UiLvglModule::lvgl_post(Req kind, int32_t value) {
    portENTER_CRITICAL(&mux_);
    const uint8_t next = (uint8_t)((req_head_ + 1) % REQ_CAP);
    if (next != req_tail_) {                 // full → drop, see the note in the header
        req_[req_head_] = {kind, value};
        req_head_ = next;
    }
    portEXIT_CRITICAL(&mux_);
}

bool UiLvglModule::lvgl_wifi_connect(const char* ssid, const char* password) {
    if (!ssid || !ssid[0] || wifi_req_ready_.load(std::memory_order_acquire)) return false;
    strncpy(wifi_req_ssid_, ssid, sizeof(wifi_req_ssid_) - 1);
    wifi_req_ssid_[sizeof(wifi_req_ssid_) - 1] = '\0';
    strncpy(wifi_req_pass_, password ? password : "", sizeof(wifi_req_pass_) - 1);
    wifi_req_pass_[sizeof(wifi_req_pass_) - 1] = '\0';
    wifi_req_ready_.store(true, std::memory_order_release);
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Main-loop side (100 Hz, non-blocking)
// ═══════════════════════════════════════════════════════════════

void UiLvglModule::on_update(uint32_t dt_ms) {
    ui_scroll_bench_note_main_turn();   // bench builds only: the main loop's pace during a run
    poll_inputs(dt_ms);
    mirror_ms_ += dt_ms;
    if (mirror_ms_ >= 50) {    // 20 Hz — the meters are only as smooth as this mirror
        mirror_ms_ = 0;
        mirror_state();
    }
    diag_ms_ += dt_ms;
    if (diag_ms_ >= 500) {     // 2 Hz — see mirror_diag()
        diag_ms_ = 0;
        mirror_diag();
        // ui.theme and ui.lang set from off the glass — the web, MQTT. They were read only at
        // boot once, so the web turned at once and the panel at the next restart.
        const int32_t th = read_int("ui.theme", want_theme_);
        if (th != want_theme_) want_theme(th);
        const int32_t lg = read_int("ui.lang", want_lang_);
        if (lg != want_lang_) want_lang(lg);
        rebuild_look();      // also the retry of one that had to wait
    }
    publish_touch();
    apply_requests();
    // The shell's half of a standby change that could not get the lock (set_standby). Whatever
    // standby_ is now is what it gets; never lock(0), that is portMAX_DELAY.
    if (shell_sb_dirty_ && ui_ && lvgl_running_ && lvgl_port_lock(5)) {
        ui_shell_set_standby(standby_);
        lvgl_port_unlock();
        shell_sb_dirty_ = false;
    }
    persist_eq();
    apply_backlight(dt_ms);
}

void UiLvglModule::poll_inputs(uint32_t dt_ms) {
    bool changed = false;
    int32_t pos = last_encoder_pos_;
    bool sw = last_encoder_sw_;

    float v = 0.0f;
    if (encoder_ && encoder_->read(v)) pos = (int32_t)v;
    if (encoder_sw_ && encoder_sw_->read(v)) sw = (v > 0.5f);

    bool input = false;
    if (pos != last_encoder_pos_) {
        const int32_t delta = pos - last_encoder_pos_;
        last_encoder_pos_ = pos;
        state_set("ui.encoder_pos", pos);
        changed = true;
        input = true;
        // With no screen there is no one to interpret the encoder, so keep the plain
        // behaviour: 2 % of volume per detent.
        if (!ui_ && state_get("audio.volume").has_value()) {
            int32_t vol = read_int("audio.volume", 0) + delta * 2;
            if (vol < 0) vol = 0; else if (vol > 100) vol = 100;
            state_set("audio.volume", vol);
        }
    }

    // Button: distinguish a click from a hold. The hold fires while still pressed, which is
    // what makes a press-and-hold menu feel right — waiting for the release would mean the
    // menu appears only after letting go.
    if (sw) {
        sw_held_ms_ += dt_ms;
        if (!sw_hold_fired_ && sw_held_ms_ >= 650) {
            sw_hold_fired_ = true;
            input = true;
            portENTER_CRITICAL(&mux_);
            snap_.enc_event = 2;
            snap_.enc_event_seq++;
            portEXIT_CRITICAL(&mux_);
        }
    }
    if (sw != last_encoder_sw_) {
        last_encoder_sw_ = sw;
        state_set("ui.encoder_sw", sw);
        changed = true;
        input = true;
        if (!sw) {                                  // released
            if (!sw_hold_fired_) {
                if (ui_) {
                    portENTER_CRITICAL(&mux_);
                    snap_.enc_event = 1;
                    snap_.enc_event_seq++;
                    portEXIT_CRITICAL(&mux_);
                } else if (state_get("audio.mute").has_value()) {
                    state_set("audio.mute", !read_bool("audio.mute", false));
                }
            }
            sw_held_ms_ = 0;
            sw_hold_fired_ = false;
        }
    }

    // Auto-standby. Any encoder or touch activity restarts the timer; the touch side feeds
    // it through publish_touch(), which is the only place that knows a finger arrived.
    if (input) { idle_ms_ = 0; if (standby_) set_standby(false); }
    else       { idle_ms_ += dt_ms; }
    const int32_t sleep_min = read_int("ui.standby_min", 5);
    if (!standby_ && sleep_min > 0 && idle_ms_ >= (uint32_t)sleep_min * 60000u) {
        set_standby(true);
    }

    clock_ms_ += dt_ms;
    char clock_txt[12] = "";
    char date_txt[12] = "";
    bool clock_changed = false;
    if (clock_ms_ >= 1000) {
        clock_ms_ = 0;
        time_t now = time(nullptr);
        struct tm t = {};
        localtime_r(&now, &t);
        if (t.tm_year + 1900 >= 2020) {   // SNTP synced (or manual time set)
            strftime(clock_txt, sizeof(clock_txt), "%H:%M", &t);
            strftime(date_txt, sizeof(date_txt), "%d.%m.%Y", &t);
        } else {
            strcpy(clock_txt, "--:--");
            strcpy(date_txt, "--.--.----");
        }
        clock_changed = true;
    }

    if (changed || clock_changed) {
        portENTER_CRITICAL(&mux_);
        snap_.encoder_pos = pos;
        snap_.encoder_sw = sw;
        if (clock_changed) {
            strncpy(snap_.clock, clock_txt, sizeof(snap_.clock) - 1);
            strncpy(snap_.date, date_txt, sizeof(snap_.date) - 1);
        }
        portEXIT_CRITICAL(&mux_);
    }
}

// Audio + Wi-Fi state → snapshot (read-only mirror of SharedState, 20 Hz).
void UiLvglModule::mirror_state() {
    // `ui.screen` is the UI's own record of what it is showing, and until now it was written
    // once at init and again on a standby transition — never when the user actually picked a
    // screen. Two things read it and both were therefore wrong: the Web UI showed "now" for
    // ever, and the audio core gates its 1024-point FFT on it (audio_module.cpp:365-367), so
    // the analyser it believes it switches off on screens without a spectrum row has in fact
    // been running since boot on every one of them. That is CPU spent on nothing, and it is
    // spent on core 1 next to the audio task — it would show up in the very `audio.cpu_pct`
    // the service screen exists to display.
    //
    // Published from the main loop like every other key. ui_shell_current() returns a pointer
    // into the shell's own static table, so the comparison is a pointer test and costs
    // nothing at 20 Hz; reading the shell's current index from this task is what set_standby()
    // below has always done. Standby publishes "standby" and owns the key while it lasts.
    if (ui_ && !standby_) {
        const char* screen = ui_shell_current();
        if (screen != published_screen_) {
            published_screen_ = screen;
            state_set("ui.screen", screen);
        }
    }

    const bool has_audio = state_get("audio.source").has_value();
    const int32_t source = has_audio ? read_int("audio.source", 0) : -1;
    const int32_t sources = has_audio ? read_int("audio.sources", 0) : 0;
    const int32_t volume = has_audio ? read_int("audio.volume", 0) : -1;
    const bool mute = has_audio && read_bool("audio.mute", false);
    const bool leveler = has_audio && read_bool("audio.leveler", false);
    const float level_l = has_audio ? read_float("audio.level_l", -120.0f) : -120.0f;
    const float level_r = has_audio ? read_float("audio.level_r", -120.0f) : -120.0f;
    const bool eq_on = has_audio && read_bool("audio.eq_enabled", false);
    const int32_t bass_db = has_audio ? read_int("audio.bass_db", 0) : 0;
    const int32_t treble_db = has_audio ? read_int("audio.treble_db", 0) : 0;
    const int32_t loudness = has_audio ? read_int("audio.loudness", 0) : 0;
    const int32_t spk_type = has_audio ? read_int("audio.spk_type", 0) : 0;
    const int32_t port_hz = has_audio ? read_int("audio.port_hz", 0) : 0;
    const int32_t port_src = has_audio ? read_int("audio.port_src", 0) : 0;
    const int32_t balance_db10 = has_audio ? read_int("audio.balance_db10", 0) : 0;
    const int32_t underruns = has_audio ? read_int("audio.underruns", 0) : 0;
    // read_number, not read_int: the audio module publishes both of these as floats.
    const int32_t cpu_pct = has_audio ? read_number("audio.cpu_pct", 0) : 0;
    const int32_t in_rate = has_audio ? read_number("audio.in_rate", 0) : 0;
    const int32_t tone_hz = has_audio ? read_int("audio.tone_hz", 0) : 0;
    char audio_state[16] = "";
    if (has_audio) read_string("audio.state", audio_state, sizeof(audio_state));

    char play_title[96] = "", play_fmt[32] = "", play_state[12] = "";
    char play_artist[96] = "", play_album[96] = "";
    char radio_name[64] = "", radio_now[128] = "", radio_fmt[32] = "", radio_state[16] = "";
    read_string("radio.name", radio_name, sizeof(radio_name));
    read_string("radio.now_playing", radio_now, sizeof(radio_now));
    read_string("radio.format", radio_fmt, sizeof(radio_fmt));
    read_string("radio.state", radio_state, sizeof(radio_state));
    read_string("player.title", play_title, sizeof(play_title));
    read_string("player.artist", play_artist, sizeof(play_artist));
    read_string("player.album", play_album, sizeof(play_album));
    read_string("player.format", play_fmt, sizeof(play_fmt));
    read_string("player.state", play_state, sizeof(play_state));
    char radio_mode[12] = "";
    read_string("radio.mode", radio_mode, sizeof(radio_mode));
    const bool network_push = source == 5 && strcmp(radio_mode, "push") == 0;

    // The Player's picture, for the web: the tile screen_now.cpp draws, by the same rule. ui.cover
    // goes out over the WebSocket when it changes, and GET /api/cover sends that tile. The
    // background made from it (ui.cover_rgb, as the glass draws it) changes with the picture.
    {
        player_core::CoverSource tile;
        const auto cv = player_core::cover_now(source, network_push, tile);
        ui_web_media_set_cover(cv.px ? (int)tile : -1);
        const int32_t id = player_core::cover_id(cv, tile);
        if (id != cover_id_) {
            cover_id_ = id;
            state_set("ui.cover", id);
            state_set("ui.cover_rgb", (int32_t)(cv.px ? player_core::cover_backdrop(cv.rgb) : 0u));
        }
    }

    // Bluetooth fills the same play_* fields the file player and DLNA do, rather than
    // getting a set of its own in the snapshot. The snapshot is copied whole under a
    // spinlock at 20 Hz, so three more strings of 96 bytes would be paid for on every tick
    // of every source; and the screen then needs no second branch to draw the same six
    // things. Only one source is selected at a time, which is what makes the field reuse
    // safe — the same reason the network_push branch below already does it.
    char bt_state[16] = "";
    read_string("bt.state", bt_state, sizeof(bt_state));
    const bool bt_connected =
        strcmp(bt_state, "connected") == 0 || strcmp(bt_state, "streaming") == 0;
    if (source == 3) {
        read_string("bt.title", play_title, sizeof(play_title));
        read_string("bt.artist", play_artist, sizeof(play_artist));
        read_string("bt.album", play_album, sizeof(play_album));
        read_string("bt.play_state", play_state, sizeof(play_state));
        // What the stream IS, not what the converter measured: the satellite announces the
        // rate before the first sample of it, so this line is right from the first frame
        // rather than after the rate detector has settled.
        char codec[16] = "";
        read_string("bt.codec", codec, sizeof(codec));
        const int32_t bt_rate = read_int("bt.rate_hz", 0);
        if (bt_rate > 0)
            snprintf(play_fmt, sizeof(play_fmt), "%s%s%.1f kHz",
                     codec, codec[0] ? " " : "", (double)bt_rate / 1000.0);
        else
            snprintf(play_fmt, sizeof(play_fmt), "%s", codec);
    }
    // AirPlay likewise, from airplay.*, and for the same reason: one source at a time.
    char ap_state[12] = "";
    read_string("airplay.state", ap_state, sizeof(ap_state));
    const bool ap_playing = strcmp(ap_state, "playing") == 0;
    const bool ap_connected =
        ap_playing || strcmp(ap_state, "paused") == 0 || strcmp(ap_state, "connected") == 0;
    const bool ap_remote = read_bool("airplay.remote", false);
    if (source == 6) {
        read_string("airplay.title", play_title, sizeof(play_title));
        read_string("airplay.artist", play_artist, sizeof(play_artist));
        read_string("airplay.album", play_album, sizeof(play_album));
        snprintf(play_state, sizeof(play_state), "%s", ap_playing ? "playing" : "paused");
        snprintf(play_fmt, sizeof(play_fmt), "%s", ap_connected ? "AirPlay" : "");
    }
    // USB likewise, from usb_audio.*: no track to name, only whether a computer is there, whether
    // it sends sound, and at what rate and width — the computer chooses both.
    char usb_state[12] = "";
    read_string("usb_audio.state", usb_state, sizeof(usb_state));
    const bool usb_streaming = strcmp(usb_state, "playing") == 0;
    const bool usb_connected = usb_streaming || strcmp(usb_state, "ready") == 0;
    if (source == 7) {
        play_title[0] = play_artist[0] = play_album[0] = '\0';
        snprintf(play_state, sizeof(play_state), "%s", usb_streaming ? "playing" : "idle");
        const int32_t usb_rate = read_int("usb_audio.rate", 0);
        const int32_t usb_bits = read_int("usb_audio.bits", 0);
        if (usb_streaming && usb_rate > 0)
            snprintf(play_fmt, sizeof(play_fmt), "%.1f kHz %ld bit", (double)usb_rate / 1000.0,
                     (long)usb_bits);
        else
            play_fmt[0] = '\0';
    }
    if (network_push) {
        // File-player tags belong to the SD track, not to the network engine.
        play_title[0] = play_artist[0] = play_album[0] = '\0';
        char radio_url[MODESP_MAX_LONG_STRING_LENGTH + 1] = "";
        char dlna_uri[MODESP_MAX_LONG_STRING_LENGTH + 1] = "";
        read_string("radio.url", radio_url, sizeof(radio_url));
        read_string("dlna.uri", dlna_uri, sizeof(dlna_uri));
        if (radio_url[0] && strcmp(radio_url, dlna_uri) == 0) {
            read_string("dlna.title", play_title, sizeof(play_title));
            read_string("dlna.artist", play_artist, sizeof(play_artist));
            read_string("dlna.album", play_album, sizeof(play_album));
        }
        // radio_now is wider than play_title (127 bytes into 95): copy whole letters, or an
        // ICY title in Cyrillic ends on the screen in half of one.
        const char* fallback = radio_now[0] ? radio_now : i18n::tr(i18n::S::now_dlna_track);
        if (!play_title[0]) modesp::utf8_copy(play_title, sizeof(play_title), fallback, strlen(fallback));
        snprintf(play_fmt, sizeof(play_fmt), "%s", radio_fmt);
        char transport[24]{}; read_string("dlna.state",transport,sizeof(transport));
        snprintf(play_state, sizeof(play_state), "%.*s", (int)sizeof(play_state)-1,
                 !strcmp(transport,"PAUSED_PLAYBACK")?"paused":radio_state);
    }
    const char* pos_key = source == 3 ? "bt.pos_s" : source == 6 ? "airplay.pos_s"
                        : network_push ? "radio.pos_s" : "player.pos_s";
    const char* dur_key = source == 3 ? "bt.dur_s" : source == 6 ? "airplay.dur_s"
                        : network_push ? "radio.dur_s" : "player.dur_s";
    const int32_t play_pos   = read_int(pos_key, 0);
    const int32_t play_dur   = read_int(dur_key, 0);
    const int32_t play_index = read_int("player.index", -1);
    const int32_t play_count = read_int("player.count", 0);
    const int32_t repeat     = read_int("player.repeat", 0);
    const bool    shuffle    = read_bool("player.shuffle", false);

    const bool wifi_ok = read_bool("wifi.connected", false);
    char ssid[sizeof(snap_.wifi_ssid)] = "", ip[16] = "";   // the whole SSID: the Wi-Fi page matches it
    read_string("wifi.ssid", ssid, sizeof(ssid));
    read_string("wifi.ip", ip, sizeof(ip));
    const int32_t rssi = read_int("wifi.rssi", 0);

    // The last four values the snapshot needs. They are read HERE, with the rest, and not
    // between the portENTER_CRITICAL and portEXIT_CRITICAL below — which is where three of
    // them used to sit.
    //
    // read_int/read_float go to SharedState::get, and that takes a FreeRTOS mutex with a
    // 100 ms timeout (shared_state.cpp). portENTER_CRITICAL disables interrupts on this
    // core; blocking on a semaphore with the scheduler shut out is not slow, it is a
    // crash — the take either asserts inside the kernel or, if the mutex happens to be
    // free, silently leaves us holding a spinlock across a context-switch point. It
    // survived because the mutex is almost never contended: the fault needs another task
    // to be inside SharedState at that instant, which is exactly what a busy link or a
    // finger on the screen produces.
    const int32_t radio_station = read_int("radio.station", 0);
    const int32_t radio_buffer  = read_int("radio.buffer", 0);
    const int32_t backlight     = read_int("ui.backlight", 80);
    const int32_t standby_bl    = read_int("ui.standby_bl", 25);
    const float   lim_gr_db     = has_audio ? read_float("audio.lim_gr", 0.0f) : 0.0f;
    const int32_t theme         = (int32_t)ui_theme_id();
    const bool    network_seekable = network_push && read_bool("radio.seekable", false);

    // What the standby face needs to know: is there music to show. A station has its own
    // state; every other source with a track (card, DLNA, Bluetooth, AirPlay) reports through
    // play_state above. AUX has no state at all — it is an input, and while it is selected the
    // spectrum is the only thing there is to show of it. On at once, off after four seconds
    // of quiet: a track change or a station rebuffering is a gap, not the end of the music.
    const bool radio_src = source == 5 && !network_push;
    const bool playing = source == 2
        || (radio_src ? strcmp(radio_state, "playing") == 0
                      : source >= 3 && strcmp(play_state, "playing") == 0);
    if (playing) { sb_music_ = true; sb_quiet_ms_ = 0; }
    else if (sb_music_ && (sb_quiet_ms_ += 50) >= 4000) sb_music_ = false;

    portENTER_CRITICAL(&mux_);
    snap_.source = source;
    snap_.network_push = network_push;
    snap_.network_seekable = network_seekable;
    snap_.bt_connected = bt_connected;
    snap_.ap_connected = ap_connected;
    snap_.ap_remote = ap_remote;
    snap_.usb_connected = usb_connected;
    snap_.sources = sources;
    snap_.volume = volume;
    snap_.mute = mute;
    snap_.leveler = leveler;
    snap_.level_l = level_l;
    snap_.level_r = level_r;
    snap_.eq_enabled = eq_on;
    snap_.bass_db = bass_db;
    snap_.treble_db = treble_db;
    snap_.loudness = loudness;
    snap_.spk_type = spk_type;
    snap_.port_hz = port_hz;
    snap_.port_src = port_src;
    snap_.balance_db10 = balance_db10;
    snap_.underruns = underruns;
    snap_.cpu_pct = cpu_pct;
    snap_.in_rate = in_rate;
    snap_.tone_hz = tone_hz;
    strncpy(snap_.play_title, play_title, sizeof(snap_.play_title) - 1);
    strncpy(snap_.radio_name, radio_name, sizeof(snap_.radio_name) - 1);
    strncpy(snap_.radio_now, radio_now, sizeof(snap_.radio_now) - 1);
    strncpy(snap_.radio_fmt, radio_fmt, sizeof(snap_.radio_fmt) - 1);
    strncpy(snap_.radio_state, radio_state, sizeof(snap_.radio_state) - 1);
    snap_.radio_station = radio_station;
    snap_.radio_buffer  = radio_buffer;
    strncpy(snap_.play_artist, play_artist, sizeof(snap_.play_artist) - 1);
    strncpy(snap_.play_album, play_album, sizeof(snap_.play_album) - 1);
    strncpy(snap_.play_fmt, play_fmt, sizeof(snap_.play_fmt) - 1);
    strncpy(snap_.play_state, play_state, sizeof(snap_.play_state) - 1);
    snap_.play_pos_s = play_pos;
    snap_.play_dur_s = play_dur;
    snap_.play_index = play_index;
    snap_.play_count = play_count;
    snap_.repeat = repeat;
    snap_.shuffle = shuffle;
    strncpy(snap_.audio_state, audio_state, sizeof(snap_.audio_state) - 1);
    snap_.wifi_ok = wifi_ok;
    strncpy(snap_.wifi_ssid, ssid, sizeof(snap_.wifi_ssid) - 1);
    strncpy(snap_.wifi_ip, ip, sizeof(snap_.wifi_ip) - 1);
    snap_.rssi = rssi;
    snap_.theme = theme;
    snap_.backlight = backlight;
    snap_.standby_bl = standby_bl;
    snap_.sb_music = sb_music_;
    snap_.lim_gr_db = lim_gr_db;
    snap_.standby = standby_;
    portEXIT_CRITICAL(&mux_);
}

/// Diagnostics → snapshot, for the service screen. 2 Hz, and the slow rate is the point.
///
/// Every value here is republished by somebody else at 0.2 Hz (SystemMonitor's 5 s report,
/// system_monitor.h:39) or 1 Hz (`system.uptime`, main.cpp), so reading them twenty times a
/// second alongside mirror_state() would be sixteen extra SharedState mutex round trips per
/// tick — on the main loop, contending with the HTTP and MQTT tasks — for numbers that
/// cannot have changed. Twice a second is still four times faster than the fastest of them
/// moves, and the panel updates visibly the moment anything does.
///
/// Every read happens BEFORE portENTER_CRITICAL, for the reason spelled out at the end of
/// mirror_state(): read_int/read_float take a FreeRTOS mutex, and taking one with interrupts
/// disabled is a crash rather than a delay (fixed in 44d4cbc; do not undo it here).
void UiLvglModule::mirror_diag() {
    // read_number and not read_int, for every number below.
    //
    // No manifest declares a `system.*` key — they are published straight from the framework
    // (system_monitor.cpp, main.cpp, watchdog_service.cpp), so nothing checks their type:
    // not the generator, not a host test, not the compiler. read_int is
    // etl::get_if<int32_t>, which on a variant holding a float hands back the default
    // silently and for ever. That is exactly how the CPU figure came to read zero on every
    // build that ever shipped (plan item 12.4), and the keys that mismatch there at least had
    // a manifest saying `float`. These have nothing. read_number takes either.
    const int32_t loop_max_ms   = read_number("system.main_loop_max_ms", 0);
    const int32_t stack_min     = read_number("system.main_stack_min", 0);
    const int32_t heap_free     = read_number("system.heap_free", 0);
    const int32_t heap_min      = read_number("system.heap_min", 0);
    const int32_t heap_internal = read_number("system.heap_internal", 0);
    const int32_t heap_dma      = read_number("system.heap_dma", 0);
    const int32_t heap_largest  = read_number("system.heap_largest", 0);
    const int32_t heap_psram    = read_number("system.heap_psram", 0);
    const int32_t uptime_s      = read_number("system.uptime", 0);
    const bool    safe_mode     = read_bool("system.safe_mode", false);
    char boot_reason[12] = "", stalled[16] = "";
    read_string("system.boot_reason", boot_reason, sizeof(boot_reason));
    read_string("system.stalled", stalled, sizeof(stalled));

    // The satellite link, as opposed to the phone on the far side of it: bt.link is the
    // HELLO exchange with the C3, which is up or down regardless of whether anything is
    // paired. Without a satellite fitted the bt module still publishes these, so the screen
    // shows an honest "немає" rather than an empty column.
    const bool    bt_link     = read_bool("bt.link", false);
    // `bt.sat` is written when the satellite says HELLO and names its build, so a non-empty
    // one means a satellite exists on this board — which is what turns "the link is down"
    // from a fault into "there is nothing plugged in".
    char sat_build[24] = "";
    read_string("bt.sat", sat_build, sizeof(sat_build));
    const bool    bt_seen     = sat_build[0] != '\0';
    const int32_t bt_rtt_ms   = read_number("bt.rtt_ms", 0);
    const int32_t bt_uart_ovf = read_number("bt.uart_ovf", 0);
    const int32_t bt_rx_drops = read_number("bt.rx_drops", 0);
    const int32_t bt_seq_gaps = read_number("bt.seq_gaps", 0);
    // The System page: who is on the other end of Bluetooth and AirPlay, and the idle timer.
    char bt_peer[32] = "", bt_codec[16] = "", ap_name[32] = "";
    read_string("bt.peer", bt_peer, sizeof(bt_peer));
    read_string("bt.codec", bt_codec, sizeof(bt_codec));
    read_string("airplay.name", ap_name, sizeof(ap_name));
    const bool    ap_enabled  = read_bool("airplay.enabled", false);
    const int32_t ap_mode     = read_int("airplay.mode", 0);
    const bool    ap_switching = read_bool("airplay.switching", false);
    char ap_mode_error[64] = "";
    read_string("airplay.mode_error", ap_mode_error, sizeof(ap_mode_error));
    const int32_t standby_min = read_int("ui.standby_min", 5);
    // set once at boot (out_caps.h); a build without the audio module plays nothing, and the
    // crossover page then says stereo, which is what the board is
    const int32_t out_caps    = read_number("audio.out_caps", audio_core::OUT_STEREO);
    const bool    dlna_built   = state_get("dlna.enabled").has_value();
    const bool    dlna_enabled = read_bool("dlna.enabled", false);
    char dlna_name[40] = "";
    read_string("dlna.name", dlna_name, sizeof(dlna_name));
    // The Wi-Fi page: which mode, whether a network is stored, and how the last try went.
    char wifi_mode[8] = "", wifi_try[12] = "", wifi_try_ssid[33] = "";
    read_string("wifi.mode", wifi_mode, sizeof(wifi_mode));
    read_string("wifi.try", wifi_try, sizeof(wifi_try));
    read_string("wifi.try_ssid", wifi_try_ssid, sizeof(wifi_try_ssid));
    const int32_t wifi_reason = read_int("wifi.reason", 0);
    const modesp::WiFiService* wifi = modesp::WiFiService::instance();
    const bool    wifi_saved  = wifi && wifi->has_credentials();

    portENTER_CRITICAL(&mux_);
    snap_.loop_max_ms   = loop_max_ms;
    snap_.stack_min     = stack_min;
    snap_.heap_free     = heap_free;
    snap_.heap_min      = heap_min;
    snap_.heap_internal = heap_internal;
    snap_.heap_dma      = heap_dma;
    snap_.heap_largest  = heap_largest;
    snap_.heap_psram    = heap_psram;
    snap_.uptime_s      = uptime_s;
    snap_.safe_mode     = safe_mode;
    strncpy(snap_.boot_reason, boot_reason, sizeof(snap_.boot_reason) - 1);
    strncpy(snap_.stalled, stalled, sizeof(snap_.stalled) - 1);
    snap_.bt_link     = bt_link;
    snap_.bt_seen     = bt_seen;
    snap_.bt_rtt_ms   = bt_rtt_ms;
    snap_.bt_uart_ovf = bt_uart_ovf;
    snap_.bt_rx_drops = bt_rx_drops;
    snap_.bt_seq_gaps = bt_seq_gaps;
    strncpy(snap_.bt_peer, bt_peer, sizeof(snap_.bt_peer) - 1);
    strncpy(snap_.bt_codec, bt_codec, sizeof(snap_.bt_codec) - 1);
    strncpy(snap_.ap_name, ap_name, sizeof(snap_.ap_name) - 1);
    snap_.ap_enabled  = ap_enabled;
    snap_.ap_mode     = ap_mode;
    snap_.ap_switching = ap_switching;
    snap_.ap_mode_error = ap_mode_error[0] != '\0';
    snap_.standby_min = standby_min;
    snap_.out_caps    = out_caps;
    snap_.dlna_built   = dlna_built;
    snap_.dlna_enabled = dlna_enabled;
    strncpy(snap_.dlna_name, dlna_name, sizeof(snap_.dlna_name) - 1);
    snap_.wifi_ap     = strcmp(wifi_mode, "ap") == 0;
    snap_.wifi_saved  = wifi_saved;
    strncpy(snap_.wifi_try, wifi_try, sizeof(snap_.wifi_try) - 1);
    strncpy(snap_.wifi_try_ssid, wifi_try_ssid, sizeof(snap_.wifi_try_ssid) - 1);
    snap_.wifi_reason = wifi_reason;
    portEXIT_CRITICAL(&mux_);
}

void UiLvglModule::publish_touch() {
    uint32_t seq;
    int16_t x, y;
    bool down;
    portENTER_CRITICAL(&mux_);
    seq = snap_.touch_seq;
    x = snap_.touch_x;
    y = snap_.touch_y;
    down = snap_.touch_down;
    portEXIT_CRITICAL(&mux_);
    if (seq == last_touch_seq_) return;
    last_touch_seq_ = seq;
    idle_ms_ = 0;
    if (standby_) set_standby(false);

    state_set("ui.touch_x", (int32_t)x);
    state_set("ui.touch_y", (int32_t)y);
    if (down != last_touch_down_) {
        last_touch_down_ = down;
        state_set("ui.touch_down", down);
        // Bring-up aid: tapping the four corners and reading this line is how the touch
        // orientation was settled (the GT911 here reports landscape natively, so the
        // board.json transform is identity) — cheap to keep, and it re-proves the mapping
        // after any change to rotation or to the touch flags.
        if (down) ESP_LOGI(TAG, "touch %ld,%ld", (long)x, (long)y);
    }
}

/// As long as the SharedState value that carries a path — the side table's width, since
/// player.track is one of the keys the manifest marks `long`. See the root CMakeLists.
static constexpr size_t PATH_LEN = MODESP_MAX_LONG_STRING_LENGTH;

/// Drain what the LVGL task asked for. SharedState is written here and nowhere else.
void UiLvglModule::apply_requests() {
    // A network to try (lvgl_wifi_connect). Polled rather than posted, so a full request ring
    // cannot strand the pair and lock the page out of connecting for good. WiFiService copies
    // it into its own buffers; the password is wiped before the buffers are handed back.
    if (wifi_req_ready_.load(std::memory_order_acquire)) {
        if (auto* w = modesp::WiFiService::instance()) {
            if (!w->try_connect(wifi_req_ssid_, wifi_req_pass_))
                ESP_LOGW(TAG, "wifi: a try is already running");
        }
        memset(wifi_req_pass_, 0, sizeof(wifi_req_pass_));
        wifi_req_ready_.store(false, std::memory_order_release);
    }
    for (;;) {
        // Zero-initialised: the empty branch leaves it untouched and returns, which the
        // compiler cannot always follow, and a request item is two words.
        ReqItem it{};
        portENTER_CRITICAL(&mux_);
        const bool empty = (req_tail_ == req_head_);
        if (!empty) {
            it = req_[req_tail_];
            req_tail_ = (uint8_t)((req_tail_ + 1) % REQ_CAP);
        }
        portEXIT_CRITICAL(&mux_);
        if (empty) return;

        switch (it.kind) {
        case Req::SOURCE:
            if (it.value >= 0 && state_get("audio.source").has_value()) {
                state_set("audio.source", it.value);
                ESP_LOGI(TAG, "source -> %ld", (long)it.value);
            }
            break;
        case Req::VOLUME:
            if (state_get("audio.volume").has_value() && it.value != read_int("audio.volume", -1)) {
                state_set("audio.volume", it.value);
            }
            break;
        case Req::MUTE:
            if (state_get("audio.mute").has_value()) {
                const bool cur = read_bool("audio.mute", false);
                state_set("audio.mute", it.value < 0 ? !cur : (it.value != 0));
            }
            break;
        case Req::LEVELER:
            if (state_get("audio.leveler").has_value()) {
                const bool cur = read_bool("audio.leveler", false);
                state_set("audio.leveler", it.value < 0 ? !cur : (it.value != 0));
            }
            break;
        case Req::EQ_ENABLED:
            if (state_get("audio.eq_enabled").has_value()) {
                state_set("audio.eq_enabled", it.value != 0);
            }
            break;
        case Req::BASS:
            if (it.value >= -6 && it.value <= 6 && state_get("audio.bass_db").has_value())
                state_set("audio.bass_db", it.value);
            break;
        case Req::TREBLE:
            if (it.value >= -6 && it.value <= 6 && state_get("audio.treble_db").has_value())
                state_set("audio.treble_db", it.value);
            break;
        case Req::LOUDNESS:
            if (it.value >= 0 && it.value <= 100 && state_get("audio.loudness").has_value())
                state_set("audio.loudness", it.value);
            break;
        case Req::BALANCE:
            // the manifest's range and step: half a dB, as the web's slider moves it
            if (it.value >= -60 && it.value <= 60 && it.value % 5 == 0 &&
                state_get("audio.balance_db10").has_value())
                state_set("audio.balance_db10", it.value);
            break;
        case Req::THEME:
            want_theme(it.value);
            rebuild_look();
            break;
        case Req::LANG:
            want_lang(it.value);
            rebuild_look();
            break;
        case Req::EQ_APPLY:
            // Just note it. persist_eq() puts the page's document into the store on the next
            // tick; the audio module hears it from there and writes it when it is not in the way.
            eq_pending_rev_ = (uint32_t)it.value;
            break;
        case Req::STANDBY_MIN:
            if (it.value >= 0 && it.value <= 60) state_set("ui.standby_min", it.value);
            break;
        case Req::AIRPLAY_ON:
            if (state_get("airplay.enabled").has_value()) state_set("airplay.enabled", it.value != 0);
            break;
        case Req::AIRPLAY_MODE:
            if ((it.value == 0 || it.value == 1) && state_get("airplay.mode").has_value())
                state_set("airplay.mode", it.value);
            break;
        case Req::DLNA_ON:
            if (state_get("dlna.enabled").has_value()) state_set("dlna.enabled", it.value != 0);
            break;
        case Req::WIFI_FORGET:
            if (auto* w = modesp::WiFiService::instance()) w->forget_credentials();
            break;
        case Req::BACKLIGHT:
            state_set("ui.backlight", it.value);
            break;
        case Req::STANDBY_BL:
            if (it.value >= 5 && it.value <= 100) {
                state_set("ui.standby_bl", it.value);
                if (!standby_) sb_preview_ms_ = 1500;
            }
            break;
        case Req::STANDBY:
            set_standby(it.value != 0);
            break;
        case Req::WAKE:
            idle_ms_ = 0;
            // The shell posts it only while its face is up. Reaching an awake module, it says the
            // two disagree (a set_standby or a bench un-peek that lost the lock): move the shell.
            if (standby_) set_standby(false);
            else          shell_sb_dirty_ = true;
            break;
        case Req::TRANSPORT:
            if (read_int("audio.source",0)==5) {
                int cmd=it.value==2?3:it.value==3?2:it.value==4?4:it.value==5?5:0;
                if(it.value==6) { char state[24]{}; read_string("dlna.state",state,sizeof(state)); cmd=!strcmp(state,"PLAYING")?3:1; }
                if(cmd) post_command_to("dlna", "dlna.cmd", (int32_t)cmd);
            } else if (read_int("audio.source",0)==3) {
                // Bluetooth takes the codes unchanged: bt.cmd is spelled in player.cmd's
                // vocabulary for exactly this reason, and the toggle is resolved by the bt
                // module, which is the side that knows what the phone is doing.
                if (it.value >= 1 && it.value <= 6)
                    post_command_to("bt", "bt.cmd", (int32_t)it.value);
            } else if (read_int("audio.source",0)==6) {
                // AirPlay takes the codes unchanged as well, and sends what it can over DACP
                // to a sender that gave it a remote. One that gave none — a phone in
                // AirPlay 2 mode, whose remote commands ride MRP (UPSTREAM.md) — gets nothing,
                // and the press must not fall through either: the card player would take
                // it, start the card and take the output away from the phone.
                if (it.value >= 1 && it.value <= 6)
                    post_command_to("airplay", "airplay.cmd", (int32_t)it.value);
            } else if (it.value >= 2 && it.value <= 6)
                post_command_to("player", "player.cmd", (int32_t)it.value);
            break;
        case Req::REPEAT: {
            int32_t r = (it.value < 0) ? (read_int("player.repeat", 0) + 1) % 3 : it.value;
            if (r < 0 || r > 2) r = 0;
            state_set("player.repeat", r);
            break;
        }
        case Req::SHUFFLE:
            state_set("player.shuffle",
                      it.value < 0 ? !read_bool("player.shuffle", false) : (it.value != 0));
            break;
        case Req::DLNA_BROWSE:
            post_command_to("dlna", "dlna.browse_cmd", it.value); break;
        case Req::RADIO_CMD:
            if (it.value >= 1 && it.value <= 4) post_command_to("radio", "radio.cmd", it.value);
            break;
        case Req::RADIO_TUNE:
            // Writing the station IS the tune: the radio module watches this key and
            // retunes when it moves, which is also how MQTT changes station. Sending the
            // play command as well asked for the same station twice, and the second request
            // arrived while the first was still connecting.
            if (it.value >= 0) state_set("radio.station", it.value);
            break;
        case Req::RADIO_STORE: {
            // Long press on a preset: whatever is playing goes into that slot, and the list
            // is written back so it survives the power going off.
            const int32_t cur = read_int("radio.station", -1);
            if (it.value >= 0 && it.value < radio_core::PRESETS && cur >= 0) {
                radio_core::stations().set_preset((int)it.value, (int)cur);
                state_set("radio.save", (int32_t)1);
                ESP_LOGI(TAG, "preset %ld = station %ld", (long)it.value, (long)cur);
            }
            break;
        }
        case Req::SEEK: {
            // The player owns what this means: it turns a second into a file position and
            // lets the decoder find the next frame from there.
            //
            // Named sources rather than "5 or else". Bluetooth now draws the same progress
            // bar, and an unlisted source falling through to `player.seek_s` would have the
            // file decoder jump to a position it was told about by a phone.
            const int32_t src = read_int("audio.source", 0);
            if (it.value < 0) break;
            if (src == 5)      state_set("radio.seek_s", it.value);
            else if (src == 4) state_set("player.seek_s", it.value);
            break;
        }
        case Req::PLAY_QUEUE: {
            // Straight from the card's index, which is the list the screen draws.
            char full[PATH_LEN];
            if (it.value >= 0 && player_core::playlist().path_at(it.value, full, sizeof(full))) {
                state_set("player.track", full);
                post_command_to("player", "player.cmd", (int32_t)1);
                ESP_LOGI(TAG, "play %ld: %s", (long)it.value, full);
            }
            break;
        }
        }
    }
}

/// A palette or a language to switch to. The setting moves at once — the web sees it, and it is
/// what gets persisted; the screen follows in rebuild_look(). Something that is not a palette
/// (or a language) is answered with the one that is on.
void UiLvglModule::want_theme(int32_t id) {
    if (id >= 0 && id < (int32_t)UiThemeId::COUNT) want_theme_ = id;
    state_set("ui.theme", want_theme_);
}

void UiLvglModule::want_lang(int32_t index) {
    if (index >= 0 && index < (int32_t)i18n::Lang::COUNT) want_lang_ = index;
    state_set("ui.lang", want_lang_);
}

/// The UI in the palette and language asked for. Widgets bake both in at construction —
/// colours into styles, text into labels — so a change rebuilds the whole tree: re-styling and
/// re-labelling every widget in place would be slower and easy to get incomplete. Taking the
/// LVGL lock keeps the refresh timer out while the old tree is destroyed.
///
/// It waits while a dialog or a keyboard is up, unless the screen is asleep: a question, or
/// half a password, is not thrown away because someone changed the scheme in the web. The
/// 2 Hz tick in on_update() tries again. Afterwards the same page is open, and the standby
/// face is back if it was showing.
void UiLvglModule::rebuild_look() {
    if (want_theme_ == (int32_t)ui_theme_id() && want_lang_ == (int32_t)i18n::lang()) return;
    if (!ui_ || !lvgl_running_) {             // nothing built: the build will take them
        ui_theme_set((UiThemeId)want_theme_);
        i18n::set_lang_index(want_lang_);
        return;
    }
    if (!lvgl_port_lock(50)) return;          // busy drawing — the 2 Hz tick tries again
    if (!standby_ && ui_shell_modal_open()) {
        lvgl_port_unlock();
        return;
    }
    const int64_t t0 = esp_timer_get_time();
    // the page and what ← climbs to from it: a colour or a language changed on Екран must not
    // leave ← going to the Player
    char page[16], from[16], parent[16];
    snprintf(page, sizeof(page), "%s", ui_shell_current());
    snprintf(from, sizeof(from), "%s", ui_shell_from());
    snprintf(parent, sizeof(parent), "%s", ui_shell_parent() ? ui_shell_parent() : "");
    ui_theme_set((UiThemeId)want_theme_);
    i18n::set_lang_index(want_lang_);
    lv_obj_clean(lv_screen_active());
    ui_shell_build(lv_screen_active(), s_ui.ctx);
    build_screen();
    ui_shell_go(from);
    if (parent[0]) ui_shell_go(parent);
    ui_shell_go(page);
    if (standby_) ui_shell_set_standby(true);
    lvgl_port_unlock();
    shell_sb_dirty_ = false;                  // the new shell is at standby_ already
    // The main task held the lock for all of it: a slow one is worth a line at the shipping level.
    const long ms = (long)((esp_timer_get_time() - t0) / 1000);
    ESP_LOG_LEVEL_LOCAL(ms > 100 ? ESP_LOG_WARN : ESP_LOG_INFO, TAG,
                        "UI rebuilt in %ld ms: scheme '%s', language %ld", ms, ui_theme().name, (long)want_lang_);
}

/// An edit on the DSP page, into the EQ document both editors share (audio_dsp/eq_store.h).
///
/// Hearing it and keeping it are no longer this module's: the audio module, the owner, hands
/// the bands to the audio core as soon as the store moves and writes eq.json at a moment that
/// is not in the way — not while this page is in front (ui.screen says so), because every
/// LittleFS write stops both cores' caches and the speakers click (customer, 04.10.2026). The
/// web edits the same document, so the page takes the store's version back when it moved
/// (screen_eq.cpp, pull_from_store).
///
/// The document is copied out under the LVGL lock into static room: it is over a kilobyte,
/// and this runs on the main task.
void UiLvglModule::persist_eq() {
    if (eq_pending_rev_ == eq_live_rev_) return;
    static audio_core::EqDoc doc;
    if (!lvgl_port_lock(20)) return;          // busy drawing — try again next tick
    ui_eq_copy_doc(doc);
    const uint32_t rev = ui_eq_revision();
    const uint32_t store_rev = audio_core::eq_store().put(doc);
    ui_eq_pushed(rev, store_rev);
    lvgl_port_unlock();
    eq_live_rev_ = eq_pending_rev_ = rev;     // the screen is the authority on its revision
}

/// Sleep dims the panel rather than switching it off: the backlight driver is PWM and a
/// zero duty is indistinguishable from a dead panel when the customer looks at it. The
/// user's brightness setting is left alone — apply_backlight() picks the level either way.
void UiLvglModule::set_standby(bool on) {
    if (standby_ == on) return;
    standby_ = on;
    idle_ms_ = 0;
    if (!ui_ || !lvgl_running_) return;
    // A frame can hold the lock past 50 ms. The flag is already moved, and every way off the
    // face asks it, so a shell left behind would keep the face up for good: on_update() moves
    // it on a later tick instead.
    if (lvgl_port_lock(50)) {
        ui_shell_set_standby(on);
        lvgl_port_unlock();
        shell_sb_dirty_ = false;
    } else {
        shell_sb_dirty_ = true;
    }
    // Either way the level is apply_backlight()'s, on this same tick: forget the last one so
    // it is written even when the number happens to be the same.
    last_backlight_ = -1;
    sb_preview_ms_ = 0;
    // Going to sleep takes the key over; waking gives it back. Clearing the cache on the way
    // in is what makes mirror_state() republish the real screen after a wake even if it is
    // the same one that was showing when the panel went dark.
    published_screen_ = on ? nullptr : ui_shell_current();
    state_set("ui.screen", on ? "standby" : ui_shell_current());
    ESP_LOGI(TAG, "standby %s", on ? "on" : "off");
}

/// Read one directory. Runs in the main loop because it blocks on the card; the LVGL lock is
/// held only for the copy, so the UI never sees a half-filled listing. Folders sort first,
/// then names — readdir order is whatever the filesystem feels like, which is not an order.

/// The level the panel should be at, from three settings: the user's brightness while awake,
/// and asleep either the standby face's own level (music is playing and the spectrum is on
/// it) or STANDBY_DIM (just the clock). The standby level is also what the panel shows for a
/// moment after it is changed from the System page.
void UiLvglModule::apply_backlight(uint32_t dt_ms) {
    if (!panel_) return;
    constexpr int32_t STANDBY_DIM = 5;
    if (sb_preview_ms_) sb_preview_ms_ = sb_preview_ms_ > dt_ms ? sb_preview_ms_ - dt_ms : 0;
    int32_t bl;
    if (standby_)            bl = sb_music_ ? read_int("ui.standby_bl", 25) : STANDBY_DIM;
    else if (sb_preview_ms_) bl = read_int("ui.standby_bl", 25);
    else                     bl = read_int("ui.backlight", 80);
    if (bl < 0) bl = 0;
    if (bl > 100) bl = 100;
    if (bl == last_backlight_) return;
    last_backlight_ = bl;
    panel_->set_backlight((uint8_t)bl);
    ESP_LOGI(TAG, "backlight %ld%%", (long)bl);
}

void UiLvglModule::on_stop() {
    modesp::liveness::unwatch("lvgl");
    if (panel_) panel_->set_backlight(0);
    if (lvgl_running_) lvgl_port_stop();
}
