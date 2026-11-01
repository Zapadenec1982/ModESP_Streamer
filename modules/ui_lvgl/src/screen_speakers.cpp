/**
 * @file screen_speakers.cpp
 * @brief Налаштування → Колонки: what has been done about the speakers, and the balance
 *        (the web's pages/system/SpeakersPage).
 *
 * The state card says what the web's page says (lib/speakers.js, speakerState): closed speakers,
 * a port measured or set by hand, or not set up yet — with the two speakers drawn as the web draws
 * them, the port shown only when they have one — and that setting them up is the web's: the
 * wizard measures with the panel's microphone and asks before it writes anything. The panel's
 * address under that line; the web's QR code is the menu's (≡, ui_shell.cpp).
 *
 * The balance card is the web's: −6…+6 dB in half-dB steps, zero in the middle, «Л» and «П» at
 * the ends. Only a finger on the knob takes it, nothing on a press or a tap, and «Скасувати»
 * after it moved the balance, as Бас and Високі do (sound_sheet.cpp, touch_gesture.h).
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "touch_gesture.h"

#include <climits>
#include <cstdio>
#include <cstring>

namespace {

using i18n::S;
using i18n::tr;
using modesp::gen::AUDIO_PORT_SRC_MEASURED;
using modesp::gen::AUDIO_SPK_TYPE_CLOSED;
using modesp::gen::AUDIO_SPK_TYPE_PORTED;

// ── geometry, relative to the 752x384 content column ──
constexpr int32_t STATE_H  = 212;
constexpr int32_t GAP      = 16;
constexpr int32_t BAL_Y    = STATE_H + GAP;
constexpr int32_t BAL_H    = uig::CONT_H - BAL_Y;
static_assert(BAL_H >= 140, "the balance card has room for its line and its slider");
constexpr int32_t ART_W    = 220;               ///< the two speakers, the web's 220x112 drawing
constexpr int32_t ART_H    = 112;
constexpr int32_t TEXT_X   = 24 + ART_W + 24;
constexpr int32_t TEXT_W   = uig::CONT_W - TEXT_X - 24;
constexpr int32_t ROW_X    = 24;                ///< the slider's row, in from the card's sides
constexpr int32_t ROW_W    = uig::CONT_W - 2 * ROW_X;
// the panel's one slider (ui_m3_widgets.h), the zero in the middle, «Л» and «П» at its ends
constexpr int32_t SLIDER_H = m3w::SLIDER_H;
constexpr int32_t TRACK_H  = m3w::SLIDER_RAIL;
constexpr int32_t KNOB     = m3w::SLIDER_KNOB;
constexpr int32_t END_W    = 40;                ///< «Л» and «П», each side
constexpr int32_t TRACK_EDGE = 16;              ///< where a finger can land and set nothing
constexpr int32_t STEPS    = 12;                ///< half-dB steps each way: ±6 dB

struct Page {
    UiCtx*    ctx = nullptr;
    // the state
    lv_obj_t* art = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* ok = nullptr;              ///< the tick before a measured port
    lv_obj_t* line = nullptr;
    char      line_txt[96] = "";
    int32_t   type = -1, hz = -1, src = -1;   ///< what the card was painted for
    lv_obj_t* addr = nullptr;            ///< the panel's address, under «Налаштувати — у вебінтерфейсі»
    char      addr_txt[16] = "";
    // the balance
    lv_obj_t* row = nullptr;
    lv_obj_t* value = nullptr;
    char      value_txt[48] = "";
    lv_obj_t* sl = nullptr;
    int32_t   shown = INT32_MIN;         ///< the half-dB step the slider shows
    bool      drag = false;
    int32_t   pressed = 0;               ///< the value when the finger came down
    touch::Slide touch;                  ///< a finger on it (touch_gesture.h)
    touch::UndoStart<int32_t> undo;
};

Page g;

void post(UiLvglModule::Req kind, int32_t v) {
    if (g.ctx && g.ctx->mod) g.ctx->mod->lvgl_post(kind, v);
}

// ── the balance ──

/// A half-dB step as the web says it: «Центр», «Лівіше · 1,5», «Правіше · 2».
void balance_text(char* out, size_t n, int32_t half) {
    if (half == 0) { snprintf(out, n, "%s", tr(S::spk_center)); return; }
    const int32_t a = half < 0 ? -half : half;
    char num[16];
    if (a % 2 == 0) i18n::num(num, sizeof(num), a / 2, 0);
    else            i18n::num(num, sizeof(num), a * 5, 1);
    i18n::fmt(out, n, tr(half < 0 ? S::spk_left : S::spk_right), {{"n", num}});
}

void show_balance(int32_t half) {
    char b[48];
    balance_text(b, sizeof(b), half);
    uih::set_text(g.value, g.value_txt, sizeof(g.value_txt), b);
}

void set_balance(int32_t half) {
    if (half < -STEPS) half = -STEPS; else if (half > STEPS) half = STEPS;
    lv_slider_set_value(g.sl, half, LV_ANIM_OFF);
    show_balance(half);
    post(UiLvglModule::Req::BALANCE, half * 5);
}

void undo_cb(int32_t v, void*) {
    g.undo.clear();
    set_balance(v);
}

/// A finger moved the balance from `was`: «Скасувати», from where it started (touch_gesture.h).
void note_move(int32_t was) {
    const int32_t now = lv_slider_get_value(g.sl);
    if (now == was) return;
    const int32_t from = g.undo.note(was, lv_tick_get());
    if (from == now) {                                   // back where it started
        g.undo.clear();
        ui_shell_undo_drop(UNDO_BALANCE);
        return;
    }
    char a[48], b[48], txt[128];
    balance_text(a, sizeof(a), from);
    balance_text(b, sizeof(b), now);
    // "›" for the arrow, as Бас's note does: no Roboto subset has an arrow
    snprintf(txt, sizeof(txt), "%s: %s \xE2\x80\xBA %s", tr(S::spk_balance), a, b);
    ui_shell_undo(UNDO_BALANCE, txt, undo_cb, from, nullptr);
}

/// A finger on the balance, before lv_slider's own handler, which it keeps out (m3w::knob_touch):
/// only a finger on the knob takes it, and the knob follows it from where it stood once it went
/// along; a press, a tap or a finger elsewhere sets nothing.
void slider_cb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    int32_t v = 0;
    const m3w::Slid r = m3w::knob_touch(e, g.touch, v);
    if (code == LV_EVENT_PRESSED) {
        g.drag = g.touch.state() == touch::Slide::PRESS;     // off the knob the snapshot keeps it
        g.pressed = lv_slider_get_value(g.sl);
    }
    if (r == m3w::Slid::MOVE && v != lv_slider_get_value(g.sl)) {
        lv_slider_set_value(g.sl, v, LV_ANIM_OFF);
        show_balance(v);
        post(UiLvglModule::Req::BALANCE, v * 5);
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        g.drag = false;
        if (r == m3w::Slid::END) note_move(g.pressed);
    }
}

// ── the state ──

/// A circle of the drawing: centre, radius, a fill or none, a line or none.
void circle(lv_obj_t* art, int32_t cx, int32_t cy, int32_t r, const uint32_t* fill, bool line) {
    const auto& t = m3::theme();
    lv_obj_t* o = m3w::surface(art, cx - r, cy - r, 2 * r, 2 * r, fill ? *fill : t.c.surface_high, m3::shape::FULL);
    if (!fill) lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    if (line) {
        lv_obj_set_style_border_width(o, 2, 0);
        lv_obj_set_style_border_color(o, m3::col(t.c.on_surface_var), 0);
    }
}

/// The two speakers as the web draws them (SpeakersPage.svelte): a tweeter, a woofer with its
/// cap, and the port under them when they have one.
void draw(bool closed) {
    const auto& t = m3::theme();
    lv_obj_clean(g.art);
    for (int32_t x : {44, 118}) {
        lv_obj_t* b = m3w::surface(g.art, x, 4, 58, 104, t.c.surface_high, 9);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, m3::col(t.c.on_surface_var), 0);
        const int32_t cx = x + 29;
        circle(g.art, cx, closed ? 26 : 22, 6, nullptr, true);
        circle(g.art, cx, closed ? 64 : 56, 18, nullptr, true);
        circle(g.art, cx, closed ? 64 : 56, 6, &t.c.on_surface_var, false);
        if (!closed) circle(g.art, cx, 92, 6, &t.c.surface_lowest, true);
    }
}

/// The card as the snapshot has it: lib/speakers.js speakerState(), word for word.
void paint_state(const UiLvglModule::Snapshot& s) {
    if (s.spk_type == g.type && s.port_hz == g.hz && s.port_src == g.src) return;
    const bool redraw = (g.type == AUDIO_SPK_TYPE_CLOSED) != (s.spk_type == AUDIO_SPK_TYPE_CLOSED) || g.type < 0;
    g.type = s.spk_type;
    g.hz = s.port_hz;
    g.src = s.port_src;
    const bool closed = s.spk_type == AUDIO_SPK_TYPE_CLOSED;
    const int32_t hz = closed ? 0 : s.port_hz;
    const bool measured = hz > 0 && s.port_src == AUDIO_PORT_SRC_MEASURED;
    if (redraw) draw(closed);
    lv_label_set_text(g.title, tr(closed ? S::spk_closed
                                  : s.spk_type == AUDIO_SPK_TYPE_PORTED ? S::spk_ported : S::spk_unset));
    // the line says the port where there is one to say; closed, or not set up, the title is all
    char b[96];
    const bool port = !closed && (s.spk_type == AUDIO_SPK_TYPE_PORTED || hz > 0);
    uih::set_text(g.line, g.line_txt, sizeof(g.line_txt), port ? ui_spk_line(s, b, sizeof(b)) : "");
    uih::show(g.ok, measured);
}

/// The panel's address under «Налаштувати — у вебінтерфейсі»; nothing while it has none.
void paint_addr(const UiLvglModule::Snapshot& s) {
    if (strncmp(s.wifi_ip, g.addr_txt, sizeof(g.addr_txt)) == 0) return;
    uih::set_text(g.addr, g.addr_txt, sizeof(g.addr_txt), s.wifi_ip);
    uih::show(g.addr, s.wifi_ip[0] != '\0');
}

void refresh(lv_obj_t*, UiCtx& c);

lv_obj_t* text_block(lv_obj_t* card) {
    const auto& t = m3::theme();
    lv_obj_t* col = m3w::box(card, TEXT_X, 0, TEXT_W, STATE_H);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 8, 0);
    g.title = m3w::text(col, t.t.title_m, t.c.on_surface, "");
    lv_obj_set_width(g.title, TEXT_W);
    lv_label_set_long_mode(g.title, LV_LABEL_LONG_WRAP);
    lv_obj_t* row = m3w::box(col, 0, 0, TEXT_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    g.ok = m3w::icon(row, t.t.icon_s, t.c.tertiary, UI_SYM_CHECK_CIRCLE);
    g.line = m3w::text(row, t.t.body_l, t.c.on_surface_var, "");
    lv_obj_set_flex_grow(g.line, 1);
    lv_label_set_long_mode(g.line, LV_LABEL_LONG_WRAP);
    lv_obj_t* web = m3w::text(col, t.t.body_m, t.c.outline, tr(S::spk_setup_web));
    lv_obj_set_width(web, TEXT_W);
    lv_label_set_long_mode(web, LV_LABEL_LONG_WRAP);
    g.addr = m3w::text(col, t.t.body_m, t.c.on_surface_var, "");
    uih::show(g.addr, false);
    return col;
}

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    g = Page{};
    g.ctx = &c;
    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);

    // ── the state ──
    lv_obj_t* card = m3w::surface(root, 0, 0, uig::CONT_W, STATE_H, t.c.surface_low, m3::shape::L);
    g.art = m3w::box(card, 24, (STATE_H - ART_H) / 2, ART_W, ART_H);
    text_block(card);

    // ── the balance ──
    lv_obj_t* bal = m3w::surface(root, 0, BAL_Y, uig::CONT_W, BAL_H, t.c.surface_low, m3::shape::L);
    lv_obj_t* name = m3w::text(bal, t.t.body_l, t.c.on_surface, tr(S::spk_balance));
    lv_obj_set_pos(name, ROW_X, 20);
    g.value = m3w::text(bal, t.t.label_m, t.c.on_surface_var, "");
    lv_obj_align(g.value, LV_ALIGN_TOP_RIGHT, -ROW_X, 22);

    const int32_t row_y = BAL_H - SLIDER_H - 24;
    g.row = m3w::box(bal, ROW_X, row_y, ROW_W, SLIDER_H);
    lv_obj_add_flag(g.row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_radius(g.row, m3::shape::M, 0);
    lv_obj_t* l = m3w::text(g.row, t.t.label_m, t.c.on_surface_var, tr(S::spk_l));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t* r = m3w::text(g.row, t.t.label_m, t.c.on_surface_var, tr(S::spk_r));
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, -12, 0);
    // the zero, a tick across the track's middle
    m3w::surface(g.row, ROW_W / 2 - 1, (SLIDER_H - 22) / 2, 2, 22, t.c.outline, 1);
    const int32_t track_x = END_W + TRACK_EDGE + KNOB / 2;
    g.sl = lv_slider_create(g.row);
    lv_obj_remove_style_all(g.sl);
    lv_obj_set_size(g.sl, ROW_W - 2 * track_x, TRACK_H);
    lv_obj_set_pos(g.sl, track_x, (SLIDER_H - TRACK_H) / 2);
    lv_slider_set_mode(g.sl, LV_SLIDER_MODE_SYMMETRICAL);
    lv_slider_set_range(g.sl, -STEPS, STEPS);
    lv_slider_set_value(g.sl, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(g.sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g.sl, m3::col(t.c.surface_highest), LV_PART_MAIN);
    lv_obj_set_style_radius(g.sl, TRACK_H / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(g.sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g.sl, m3::col(t.c.primary), LV_PART_INDICATOR);
    lv_obj_set_style_radius(g.sl, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(g.sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(g.sl, m3::col(t.c.primary), LV_PART_KNOB);
    lv_obj_set_style_radius(g.sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g.sl, (KNOB - TRACK_H) / 2, LV_PART_KNOB);
    lv_obj_set_ext_click_area(g.sl, (SLIDER_H - TRACK_H) / 2);
    lv_obj_add_flag(g.sl, LV_OBJ_FLAG_CLICKABLE);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(g.sl, slider_cb, (lv_event_code_t)(code | LV_EVENT_PREPROCESS), nullptr);

    refresh(root, c);
    return root;
}

void refresh(lv_obj_t*, UiCtx& c) {
    paint_state(c.s);
    paint_addr(c.s);
    // a finger on it leaves it alone; the snapshot catches up after
    const int32_t half = c.s.balance_db10 / 5;
    if (!g.drag && half != g.shown) {
        g.shown = half;
        lv_slider_set_value(g.sl, half, LV_ANIM_OFF);
        show_balance(half);
    }
}

}  // namespace

const char* ui_spk_line(const UiLvglModule::Snapshot& s, char* out, size_t cap) {
    const bool closed = s.spk_type == AUDIO_SPK_TYPE_CLOSED;
    const int32_t hz = closed ? 0 : s.port_hz;
    S key = S::spk_unset;                                     // as its page's title says it
    if (closed)
        key = S::spk_closed;
    else if (hz > 0)
        key = s.port_src == AUDIO_PORT_SRC_MEASURED ? S::spk_port_measured : S::spk_port_manual;
    else if (s.spk_type == AUDIO_SPK_TYPE_PORTED)
        key = S::spk_port_none;
    char n[12];
    snprintf(n, sizeof(n), "%ld", (long)hz);
    i18n::fmt(out, cap, tr(key), {{"hz", n}});
    return out;
}

const UiScreenDef UI_SCREEN_SPEAKERS = {"speakers", UI_SYM_SPEAKER, build, refresh, nullptr, nullptr};
