/**
 * @file screen_display.cpp
 * @brief Налаштування → Екран: the screen's brightness, the standby face's brightness while music
 *        plays, auto-standby and the colour scheme.
 *
 * Налаштування's «Екран» group until S1 (14.10.2026), when the
 * first column went to Звук and these four rows became a page of their own, touched as they were.
 * The rows are the full column's width now, so the two sliders are longer than they were.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include <cstdio>

namespace {

using i18n::S;
using i18n::tr;

// ── geometry, relative to the 752x384 content column ──
constexpr int32_t RH      = 64;                               // a row: four of them, and room
constexpr int32_t INSET   = 4;                                // the rows' inset in the card
constexpr int32_t CARD_W  = uig::CONT_W;
constexpr int32_t RW      = CARD_W - 2 * INSET;
constexpr int32_t TITLE_Y = 10;
constexpr int32_t SUB_Y   = 36;
constexpr int32_t SLIDER_W = 320;
static_assert(4 * RH + 2 * INSET <= uig::CONT_H, "the four rows fit the column");

/// auto-standby's steps, in minutes; 0 is never
constexpr int32_t SLEEP_STEPS[] = {0, 5, 10, 15, 20, 30, 45, 60};
constexpr int     N_SLEEP = (int)(sizeof(SLEEP_STEPS) / sizeof(SLEEP_STEPS[0]));
/// After − or +, the row shows what was pressed and the next press steps from it: the snapshot
/// takes up to half a second to follow (mirror_diag, 2 Hz). As long as Тембр holds its times
/// (sound_sheet.cpp, TIME_HOLD_MS).
constexpr uint32_t SLEEP_HOLD_MS = 800;

enum RowId { R_BRIGHT, R_SB_BRIGHT, R_SLEEP, R_SCHEME, R_COUNT };
constexpr int N_SCHEME  = (int)m3::Scheme::COUNT;
/// The schemes by name, in m3::Scheme's order (ui_m3.cpp) — the web's names for them.
constexpr S SCHEME_NAMES[] = {S::sys_scheme_amber, S::sys_scheme_blue, S::sys_scheme_green, S::sys_scheme_red};
static_assert(sizeof(SCHEME_NAMES) / sizeof(SCHEME_NAMES[0]) == N_SCHEME, "a name per scheme");

struct Page {
    UiCtx*      ctx = nullptr;
    lv_obj_t*   row[R_COUNT]  = {};
    lv_obj_t*   sub[R_COUNT]  = {};
    char        sub_txt[R_COUNT][96] = {};
    m3w::Slider bright;
    touch::Slide bright_touch;
    int32_t     bright_posted = -1;
    bool        bright_drag = false;
    m3w::Slider sb_bright;               ///< the standby face's level while music plays
    touch::Slide sb_touch;
    int32_t     sb_posted = -1;
    bool        sb_drag = false;
    lv_obj_t*   sleep_val = nullptr;
    m3w::Button sleep_dec, sleep_inc;
    int32_t     sleep_want = -1;         ///< the minutes − or + asked for last...
    uint32_t    sleep_at = 0;            ///< ...and when
    lv_obj_t*   swatch[N_SCHEME] = {};
    int32_t     backlight = -1;
    int32_t     standby_bl = -1;
    int32_t     standby_min = -1;
    int32_t     theme = -1;
};

Page g;

void post(UiLvglModule::Req kind, int32_t v) {
    if (g.ctx && g.ctx->mod) g.ctx->mod->lvgl_post(kind, v);
}

/// The step after `min` that way: the next one strictly above or below, so minutes between two
/// steps (the API and MQTT take any multiple of 5) go to a neighbour, not past it; at an end, that end.
int32_t sleep_step(int32_t min, int dir) {
    if (dir > 0) {
        for (int i = 0; i < N_SLEEP; i++) if (SLEEP_STEPS[i] > min) return SLEEP_STEPS[i];
        return SLEEP_STEPS[N_SLEEP - 1];
    }
    for (int i = N_SLEEP - 1; i >= 0; i--) if (SLEEP_STEPS[i] < min) return SLEEP_STEPS[i];
    return SLEEP_STEPS[0];
}

/// Auto-standby's minutes in its row, and − and + as far as they go.
void show_sleep(int32_t min) {
    g.standby_min = min;
    char buf[96];
    if (min <= 0) {
        snprintf(buf, sizeof(buf), "%s", tr(S::sys_never));
    } else {
        char n[12];
        snprintf(n, sizeof(n), "%ld", (long)min);
        i18n::fmt(buf, sizeof(buf), tr(S::sys_minutes), {{"n", n}});
    }
    lv_label_set_text(g.sleep_val, buf);
    m3w::button_enable(g.sleep_dec, min > 0);
    m3w::button_enable(g.sleep_inc, min < SLEEP_STEPS[N_SLEEP - 1]);
}

bool sleep_held() { return g.sleep_want >= 0 && lv_tick_elaps(g.sleep_at) < SLEEP_HOLD_MS; }

void set_sleep_step(int dir) {
    if (!g.ctx) return;
    const int32_t cur = sleep_held() ? g.sleep_want : g.ctx->s.standby_min;
    const int32_t to = sleep_step(cur, dir);
    if (to == cur) return;
    g.sleep_want = to;
    g.sleep_at = lv_tick_get();
    show_sleep(to);
    post(UiLvglModule::Req::STANDBY_MIN, to);
}

void set_brightness(int32_t pct) {
    // Never all the way down: a panel at 0 % is indistinguishable from a dead one, and the
    // finger that did it has nothing left to see to undo it.
    if (pct < 5) pct = 5; else if (pct > 100) pct = 100;
    if (pct == g.bright_posted) return;
    g.bright_posted = pct;
    post(UiLvglModule::Req::BACKLIGHT, pct);
}

/// The standby level, floored like the other: the panel shows it for a moment as it is set.
void set_sb_brightness(int32_t pct) {
    if (pct < 5) pct = 5; else if (pct > 100) pct = 100;
    if (pct == g.sb_posted) return;
    g.sb_posted = pct;
    post(UiLvglModule::Req::STANDBY_BL, pct);
}

// ── touch ──

/// A finger on one of the two brightness sliders: only one on the knob takes it, a press or a tap
/// sets nothing, and it follows a finger that went along (m3w::slider_touch). `drag` while a
/// finger holds the knob: the snapshot leaves it.
void slider_touch(lv_event_t* e, m3w::Slider& sl, touch::Slide& gesture, bool& drag, void (*set)(int32_t)) {
    const lv_event_code_t code = lv_event_get_code(e);
    int32_t pct = 0;
    const m3w::Slid r = m3w::slider_touch(e, sl, gesture, pct);
    if (code == LV_EVENT_PRESSED) drag = gesture.state() == touch::Slide::PRESS;
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) drag = false;
    if (r != m3w::Slid::MOVE) return;
    m3w::slider_set(sl, pct < 5 ? 5 : pct);
    set(pct);
}
void bright_cb(lv_event_t* e)    { slider_touch(e, g.bright, g.bright_touch, g.bright_drag, set_brightness); }
void sb_bright_cb(lv_event_t* e) { slider_touch(e, g.sb_bright, g.sb_touch, g.sb_drag, set_sb_brightness); }
void sleep_dec_cb(lv_event_t*) { set_sleep_step(-1); }
void sleep_inc_cb(lv_event_t*) { set_sleep_step(+1); }
void swatch_cb(lv_event_t* e) {
    const int s = (int)(intptr_t)lv_event_get_user_data(e);
    if (g.ctx && s != g.ctx->s.theme) post(UiLvglModule::Req::THEME, s);
}

// ── building ──

/// One row of the card: icon, title, and the line under it; the caller adds the control. A row
/// whose control shows its own value (a slider: the number at its right) has no second line, and
/// its title sits in the middle.
lv_obj_t* row(lv_obj_t* card, int i, RowId id, const char* sym, const char* title, int32_t control_w,
              bool second_line = true) {
    const auto& t = m3::theme();
    lv_obj_t* r = m3w::box(card, INSET, INSET + i * RH, RW, RH);
    lv_obj_set_style_radius(r, m3::shape::M, 0);
    lv_obj_t* ic = m3w::icon(r, t.t.icon_m, t.c.on_surface_var, sym);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t* ti = m3w::text(r, t.t.body_l, t.c.on_surface, title);
    g.row[id] = r;
    if (!second_line) {
        lv_obj_align(ti, LV_ALIGN_LEFT_MID, 54, 0);
        return r;
    }
    lv_obj_set_pos(ti, 54, TITLE_Y);
    lv_obj_t* s = m3w::text(r, t.t.label_s, t.c.on_surface_var, "");
    lv_obj_set_pos(s, 54, SUB_Y);
    // the second line is a value, and leaves the control its room
    lv_obj_set_size(s, RW - 54 - control_w - 16, lv_font_get_line_height(t.t.label_s));
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    g.sub[id] = s;
    return r;
}

/// One of the two brightness sliders, on the right of its row, the percentage at its end.
void slider(lv_obj_t* r, m3w::Slider& sl, lv_event_cb_t cb) {
    sl = m3w::slider(r, RW - SLIDER_W - 10, (RH - m3w::SLIDER_H) / 2, SLIDER_W, nullptr, " %");
    lv_obj_add_event_cb(sl.obj, cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(sl.obj, cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(sl.obj, cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(sl.obj, cb, LV_EVENT_PRESS_LOST, nullptr);
}

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    g = Page{};
    g.ctx = &c;
    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);
    lv_obj_t* card = m3w::surface(root, 0, 0, CARD_W, 4 * RH + 2 * INSET, t.c.surface_low, m3::shape::L);

    lv_obj_t* r = row(card, 0, R_BRIGHT, UI_SYM_BRIGHTNESS, tr(S::sys_brightness), SLIDER_W + 10, false);
    slider(r, g.bright, bright_cb);

    // the standby face's level while music plays; with nothing playing it stays at 5 %
    r = row(card, 1, R_SB_BRIGHT, UI_SYM_GRAPHIC_EQ, tr(S::sys_standby_bl_short), SLIDER_W + 10, false);
    slider(r, g.sb_bright, sb_bright_cb);

    // − and + a finger's 64 px (20.10.2026: 48), the minutes between them
    r = row(card, 2, R_SLEEP, UI_SYM_MOON, tr(S::sys_sleep), 210, false);   // the minutes are its value
    g.sleep_dec = m3w::icon_button(r, RW - 204, 0, 64, UI_SYM_REMOVE, t.t.icon_m);
    g.sleep_inc = m3w::icon_button(r, RW - 70, 0, 64, UI_SYM_ADD, t.t.icon_m);
    lv_obj_add_event_cb(g.sleep_dec.obj, sleep_dec_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g.sleep_inc.obj, sleep_inc_cb, LV_EVENT_CLICKED, nullptr);
    g.sleep_val = m3w::text(r, t.t.label_m, t.c.on_surface, "");
    lv_obj_set_width(g.sleep_val, 70);
    lv_obj_set_style_text_align(g.sleep_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.sleep_val, RW - 140, (RH - lv_font_get_line_height(t.t.label_m)) / 2);

    // the four schemes: 44 px drawn, 20 apart, each a finger's 64 with nothing between their reaches
    constexpr int32_t SW = 44, SW_PITCH = 64;
    r = row(card, 3, R_SCHEME, UI_SYM_PALETTE, tr(S::sys_scheme_short), N_SCHEME * SW_PITCH);
    for (int i = 0; i < N_SCHEME; i++) {
        lv_obj_t* sw = m3w::surface(r, RW - 10 - SW - (N_SCHEME - 1 - i) * SW_PITCH, (RH - SW) / 2, SW, SW,
                                    m3::theme_at((m3::Scheme)i).c.primary, m3::shape::FULL);
        lv_obj_set_style_outline_color(sw, m3::col(t.c.on_surface), 0);
        lv_obj_set_style_outline_pad(sw, 3, 0);
        lv_obj_set_style_outline_width(sw, i == (int)m3::scheme() ? 2 : 0, 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(sw, (SW_PITCH - SW) / 2);
        lv_obj_add_event_cb(sw, swatch_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        g.swatch[i] = sw;
    }
    lv_label_set_text(g.sub[R_SCHEME], tr(SCHEME_NAMES[(int)m3::scheme()]));
    return root;
}

// ── refresh ──

void set_sub(RowId id, const char* txt) {
    uih::set_text(g.sub[id], g.sub_txt[id], sizeof(g.sub_txt[id]), txt);
}

void refresh(lv_obj_t*, UiCtx& c) {
    const auto& s = c.s;
    if (s.backlight != g.backlight) {
        g.backlight = s.backlight;
        if (!g.bright_drag) { g.bright_posted = s.backlight; m3w::slider_set(g.bright, s.backlight); }
    }
    if (s.standby_bl != g.standby_bl) {
        g.standby_bl = s.standby_bl;
        if (!g.sb_drag) { g.sb_posted = s.standby_bl; m3w::slider_set(g.sb_bright, s.standby_bl); }
    }
    if (!sleep_held() && s.standby_min != g.standby_min) show_sleep(s.standby_min);
    if (s.theme != g.theme) {
        g.theme = s.theme;
        // A new scheme rebuilds the whole UI; until then the swatch already says which it is.
        for (int i = 0; i < N_SCHEME; i++) lv_obj_set_style_outline_width(g.swatch[i], i == s.theme ? 2 : 0, 0);
        if (s.theme >= 0 && s.theme < N_SCHEME) set_sub(R_SCHEME, tr(SCHEME_NAMES[s.theme]));
    }
}

} // namespace

const UiScreenDef UI_SCREEN_DISPLAY = {"display", UI_SYM_BRIGHTNESS, build, refresh, nullptr, nullptr};
