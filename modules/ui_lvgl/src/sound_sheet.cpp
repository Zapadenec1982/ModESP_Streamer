/**
 * @file sound_sheet.cpp
 * @brief «Звук»: the sounds, Бас and Високі, the loudness and the way to the equalizer — as a side
 *        sheet over the Player and as Налаштування › Звук › Тембр
 *        (the row named for what it holds: every row of the group is sound).
 *
 * One set of controls in two hosts, as on the web (pages/sound/SoundControls.svelte): the sheet
 * comes in from the right, 384 px, over everything, and the page is a list page of its own with
 * the same (no «Скинути Бас і Високі» there). The presets are a field that opens their list — a
 * dialog over everything, the one that plays ticked, which the equalizer's page opens too
 * (ui_sound_pick_show, over ui_pick.cpp's list). Everything here is touched; the knob stays the
 * volume over it (ui_shell_encoder).
 *
 * The sounds are the EQ store's (audio_dsp/eq_store.h), so a sound chosen here is the web's at
 * once, and the other way round. Бас, Високі and the loudness are settings: posted to the main
 * loop, read back from the snapshot.
 *
 * The page has one section the sheet has not, as on the web: «Вирівнювання гучності» by the clock —
 * «За розкладом» and, while it is on, the two times with − and + (the switch itself is the Player's
 * moon).
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "eq_store.h"
#include "esp_timer.h"

#include <climits>
#include <cstdio>
#include <cstring>

namespace {

using i18n::S;
using i18n::tr;

// ── geometry ──
constexpr int32_t SHEET_W  = 384;                 ///< the sheet, as the mock-up has it
constexpr int32_t SHEET_R  = m3::shape::XL;       ///< its left corners; the right ones are off-screen
constexpr int32_t SIDE     = 30;                  ///< the sheet's sides
constexpr int32_t PAGE_W   = 560;                 ///< the page's column, centred in the content
constexpr int32_t ROW_H    = 64;                  ///< the loudness's and the equalizer's rows: a finger's
constexpr int32_t FIELD_H  = 64;                  ///< the sounds' field
constexpr int32_t CLOSE    = 64;                  ///< the sheet's ✕
constexpr int32_t HEAD_Y   = 10;                  ///< the sheet's title row
// a tone slider: m3w::slider's rail, knob and row (ui_m3_widgets.h), the zero in the middle
constexpr int32_t SLIDER_H = m3w::SLIDER_H;
constexpr int32_t TRACK_H  = m3w::SLIDER_RAIL;
constexpr int32_t KNOB     = m3w::SLIDER_KNOB;
constexpr int32_t TONE_VAL = 36;                  ///< the number's column: "−6"
constexpr int32_t TONE_GAP = 12;
// the schedule's times: − and + a finger's 64, the time between them, as the screen's Автосон
constexpr int32_t TIME_BTN  = 64;
constexpr int32_t TIME_VAL  = 70;
constexpr int32_t TIME_STEP = 15;                 ///< minutes a press moves a time; the web's are 5
constexpr int32_t DAY_MIN   = 24 * 60;
constexpr int64_t TIME_HOLD_MS = 800;             ///< after a press the rows show what was pressed

struct Controls {
    UiCtx*      ctx  = nullptr;
    int32_t     w    = 0;              ///< the column's width
    lv_obj_t*   body = nullptr;        ///< the column; it scrolls when the notes do not fit
    // the EQ is off: what still works, and the way back
    lv_obj_t*   off_card = nullptr;
    m3w::Button enable;
    // the sounds
    lv_obj_t*   sound_row = nullptr;   ///< the field that opens the sounds' list
    lv_obj_t*   sound_val = nullptr;
    char        sound_val_txt[48] = "";
    // the timbre
    lv_obj_t*   bass_val = nullptr;
    lv_obj_t*   treble_val = nullptr;
    lv_obj_t*   bass_sl = nullptr;
    lv_obj_t*   treble_sl = nullptr;
    lv_obj_t*   loud_row = nullptr;
    lv_obj_t*   loud_sw = nullptr;
    lv_obj_t*   eq_row = nullptr;
    lv_obj_t*   eq_val = nullptr;
    char        eq_val_txt[48] = "";
    // what is shown
    int         preset   = INT_MIN;    ///< the sound the field shows
    int8_t      eq_on    = -1;
    int32_t     bass     = INT32_MIN;
    int32_t     treble   = INT32_MIN;
    int8_t      loud_on  = -1;
    // the page's only: «Вирівнювання гучності» by the clock (sched_build)
    lv_obj_t*   sched_sw = nullptr;
    lv_obj_t*   time_row[2] = {};      ///< «Вмикати о», «Вимикати о»: shown while the schedule is on
    lv_obj_t*   time_val[2] = {};
    int32_t     time_v[2] = {INT32_MIN, INT32_MIN};   ///< what they show, minute of the day
    int64_t     time_pressed = 0;      ///< ms of the last − or +: the snapshot catches up after it
    int8_t      sched_on = -1;
    bool        bass_drag = false, treble_drag = false;
    touch::Slide tone_touch[2];        ///< a finger on Бас or Високі (touch_gesture.h)
    // a finger's move of Бас or Високі, for «Скасувати» (ui_shell_undo)
    int32_t     tone_pressed[2] = {};  ///< the value when the finger came down
    touch::UndoStart<int32_t> tone_undo[2];
};

struct Sheet {
    lv_obj_t* scrim = nullptr;
    lv_obj_t* panel = nullptr;
    bool      open  = false;
    Controls  c;
};

// in PSRAM (uih::in_psram): internal RAM is the Wi-Fi co-processor's and the I2S descriptors'
Sheet&    sheet = uih::in_psram<Sheet>();
Controls& page  = uih::in_psram<Controls>();

// ── actions ──

void post(Controls& c, UiLvglModule::Req kind, int32_t v) {
    if (c.ctx && c.ctx->mod) c.ctx->mod->lvgl_post(kind, v);
}

void choose(int p) {
    audio_core::EqStore& st = audio_core::eq_store();
    if (!st.choose(p, st.revision())) return;
    // As a web edit: the store is written when the choosing has stopped (or the sheet closes),
    // not at every choice — a LittleFS write is heard (modules/audio, own_eq).
    st.web_touched(esp_timer_get_time() / 1000);
}

void set_tone(Controls& c, bool bass, int32_t v) {
    if (v < -6) v = -6; else if (v > 6) v = 6;
    lv_slider_set_value(bass ? c.bass_sl : c.treble_sl, v, LV_ANIM_OFF);
    post(c, bass ? UiLvglModule::Req::BASS : UiLvglModule::Req::TREBLE, v);
}

/// Off is 0; on is the strength it had, which the firmware keeps for the web too (audio.loud_back).
void set_loudness(Controls& c, bool on) { post(c, UiLvglModule::Req::LOUDNESS, on ? -1 : 0); }

void close_sheet();

void open_eq(Controls& c) {
    if (&c == &sheet.c) close_sheet();
    ui_shell_go("eq");
}

// ── touch ──

Controls& of(lv_event_t* e) { return *(Controls*)lv_event_get_user_data(e); }

void pick_sounds() { ui_sound_pick_show(audio_core::eq_store().preset(), choose); }
void sound_cb(lv_event_t*) { pick_sounds(); }

/// Бас or Високі as a snackbar says it: "+3", "0", "−3".
void tone_text(char* out, size_t n, int32_t v) {
    if (v > 0) snprintf(out, n, "+%ld", (long)v);
    else if (v < 0) snprintf(out, n, "\xE2\x88\x92%ld", (long)-v);   // U+2212, the minus
    else snprintf(out, n, "0");
}

void show_value(lv_obj_t* lbl, int32_t v);

void tone_undo(Controls& c, bool bass, int32_t v) {
    c.tone_undo[bass ? 0 : 1].clear();
    set_tone(c, bass, v);
    show_value(bass ? c.bass_val : c.treble_val, v);
}
void bass_undo_cb(int32_t v, void* arg)   { tone_undo(*(Controls*)arg, true, v); }
void treble_undo_cb(int32_t v, void* arg) { tone_undo(*(Controls*)arg, false, v); }

/// A finger moved Бас or Високі from `was`: «Скасувати», from where it started (touch_gesture.h).
void note_tone(Controls& c, bool bass, int32_t was) {
    const int k = bass ? 0 : 1;
    const int key = bass ? UNDO_BASS : UNDO_TREBLE;
    const int32_t now = lv_slider_get_value(bass ? c.bass_sl : c.treble_sl);
    if (now == was) return;
    const int32_t from = c.tone_undo[k].note(was, lv_tick_get());
    if (from == now) {                                   // back where it started
        c.tone_undo[k].clear();
        ui_shell_undo_drop(key);
        return;
    }
    char a[16], b[16], txt[80];
    tone_text(a, sizeof(a), from);
    tone_text(b, sizeof(b), now);
    // "›" for the arrow, as the tech chip's "44.1 › 48 kHz": no Roboto subset has an arrow
    snprintf(txt, sizeof(txt), "%s: %s \xE2\x80\xBA %s", tr(bass ? S::sound_bass : S::sound_treble), a, b);
    ui_shell_undo(key, txt, bass ? bass_undo_cb : treble_undo_cb, from, &c);
}

/// A finger on Бас or Високі, before lv_slider's own handler, which it keeps out (m3w::knob_touch):
/// only a finger on the knob takes it, and the knob follows it from where it stood once it went
/// along; a press, a tap or a finger elsewhere sets nothing. A finger that moved it leaves
/// «Скасувати».
void slider_cb(lv_event_t* e) {
    Controls& c = of(e);
    lv_obj_t* sl = (lv_obj_t*)lv_event_get_current_target(e);
    const bool bass = (sl == c.bass_sl);
    const int k = bass ? 0 : 1;
    const lv_event_code_t code = lv_event_get_code(e);
    int32_t v = 0;
    const m3w::Slid r = m3w::knob_touch(e, c.tone_touch[k], v);
    bool& drag = bass ? c.bass_drag : c.treble_drag;
    if (code == LV_EVENT_PRESSED) {
        drag = c.tone_touch[k].state() == touch::Slide::PRESS;   // off the knob the snapshot keeps it
        c.tone_pressed[k] = lv_slider_get_value(sl);
    }
    if (r == m3w::Slid::MOVE && v != lv_slider_get_value(sl)) {
        lv_slider_set_value(sl, v, LV_ANIM_OFF);
        show_value(bass ? c.bass_val : c.treble_val, v);
        post(c, bass ? UiLvglModule::Req::BASS : UiLvglModule::Req::TREBLE, v);
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        drag = false;
        if (r == m3w::Slid::END) note_tone(c, bass, c.tone_pressed[k]);
    }
}

void loud_cb(lv_event_t* e) {
    Controls& c = of(e);
    set_loudness(c, lv_obj_has_state(c.loud_sw, LV_STATE_CHECKED));
}
void enable_cb(lv_event_t* e) { post(of(e), UiLvglModule::Req::EQ_ENABLED, 1); }
void sched_cb(lv_event_t* e) {
    Controls& c = of(e);
    post(c, UiLvglModule::Req::LEV_SCHED, lv_obj_has_state(c.sched_sw, LV_STATE_CHECKED) ? 1 : 0);
}

void show_time(lv_obj_t* lbl, int32_t m) {
    if (m < 0 || m >= DAY_MIN) m = 0;
    char b[16];
    snprintf(b, sizeof(b), "%02d:%02d", (int)(m / 60), (int)(m % 60));
    lv_label_set_text(lbl, b);
}

/// − or + on a time: to the next quarter hour that way, round the clock. A time the web set between
/// two quarters goes to the nearer one in that direction.
void time_cb(lv_event_t* e) {
    Controls& c = page;
    const int which = (int)(intptr_t)lv_event_get_user_data(e) >> 1;
    const int dir = ((int)(intptr_t)lv_event_get_user_data(e) & 1) ? 1 : -1;
    int32_t m = c.time_v[which];
    if (m < 0 || m >= DAY_MIN) return;
    m = dir > 0 ? (m / TIME_STEP + 1) * TIME_STEP : ((m + TIME_STEP - 1) / TIME_STEP - 1) * TIME_STEP;
    m = (m + DAY_MIN) % DAY_MIN;
    c.time_v[which] = m;
    c.time_pressed = esp_timer_get_time() / 1000;
    show_time(c.time_val[which], m);
    post(c, which ? UiLvglModule::Req::LEV_TO : UiLvglModule::Req::LEV_FROM, m);
}
void eq_cb(lv_event_t* e)     { open_eq(of(e)); }

// ── building ──

/// A line of text that wraps in the column.
lv_obj_t* note(lv_obj_t* parent, int32_t w, const char* txt) {
    const auto& t = m3::theme();
    lv_obj_t* l = m3w::text(parent, t.t.body_m, t.c.on_surface_var, txt);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

lv_obj_t* rule(lv_obj_t* parent, int32_t w) {
    const auto& t = m3::theme();
    return m3w::surface(parent, 0, 0, w, 1, t.c.outline_var, 0);
}

/// The column Бас's and Високі's names take: the wider of the two in this language, so both
/// tracks start at one place.
int32_t tone_name_w() {
    const auto& t = m3::theme();
    lv_point_t a, b;
    lv_text_get_size(&a, tr(S::sound_bass), t.t.body_l, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_text_get_size(&b, tr(S::sound_treble), t.t.body_l, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return LV_MAX(a.x, b.x) + TONE_GAP;
}

/// Бас or Високі in one row, as every slider of the panel: the name, a slider whose zero is in the
/// middle, the number. The name's and the number's columns keep the track off the column's sides:
/// a finger that scrolls the sheet has somewhere to go that sets nothing.
lv_obj_t* tone_slider(Controls& c, const char* name, lv_obj_t*& val, int32_t name_w) {
    const auto& t = m3::theme();
    lv_obj_t* row = m3w::box(c.body, 0, 0, c.w, SLIDER_H);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t* n = m3w::text(row, t.t.body_l, t.c.on_surface, name);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
    val = m3w::text(row, t.t.label_m, t.c.on_surface_var, "0");
    lv_obj_set_width(val, TONE_VAL);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
    // the knob's centre runs from x0 to x1, the rail with it
    const int32_t x0 = name_w + KNOB / 2;
    const int32_t x1 = c.w - TONE_VAL - TONE_GAP - KNOB / 2;
    // the zero, a tick across the track's middle
    m3w::surface(row, (x0 + x1) / 2 - 1, (SLIDER_H - 22) / 2, 2, 22, t.c.outline, 1);
    lv_obj_t* sl = lv_slider_create(row);
    lv_obj_remove_style_all(sl);
    lv_obj_set_size(sl, x1 - x0, TRACK_H);
    lv_obj_set_pos(sl, x0, (SLIDER_H - TRACK_H) / 2);
    lv_slider_set_mode(sl, LV_SLIDER_MODE_SYMMETRICAL);
    lv_slider_set_range(sl, -6, 6);
    lv_slider_set_value(sl, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, m3::col(t.c.surface_highest), LV_PART_MAIN);
    lv_obj_set_style_radius(sl, TRACK_H / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, m3::col(t.c.primary), LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sl, m3::col(t.c.primary), LV_PART_KNOB);
    lv_obj_set_style_radius(sl, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, (KNOB - TRACK_H) / 2, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, (SLIDER_H - TRACK_H) / 2);
    lv_obj_add_flag(sl, LV_OBJ_FLAG_CLICKABLE);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(sl, slider_cb, (lv_event_code_t)(code | LV_EVENT_PREPROCESS), &c);
    return sl;
}

void build_controls(Controls& c, lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, UiCtx& ctx) {
    const auto& t = m3::theme();
    c = Controls{};
    c.ctx = &ctx;
    c.w = w;

    // The column scrolls, and it takes the finger itself: LVGL gives a press to the topmost clickable
    // object, so a finger on a note or a row's name went to the sheet behind the column, which does
    // not scroll — the sheet could not be scrolled to the loudness's switch. It clips what
    // scrolls out of it.
    c.body = m3w::box(parent, x, y, w, h);
    lv_obj_add_flag(c.body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c.body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c.body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(c.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c.body, 8, 0);

    // the EQ is off
    c.off_card = m3w::surface(c.body, 0, 0, w, LV_SIZE_CONTENT, t.c.surface_high, m3::shape::M);
    lv_obj_set_style_pad_all(c.off_card, 14, 0);
    lv_obj_set_flex_flow(c.off_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c.off_card, 10, 0);
    note(c.off_card, w - 28, tr(S::sound_eq_off));
    c.enable = m3w::tonal_button(c.off_card, 0, 0, w - 28, 64, m3::shape::FULL, nullptr, nullptr, tr(S::sound_eq_on));
    lv_obj_add_event_cb(c.enable.obj, enable_cb, LV_EVENT_CLICKED, &c);
    uih::show(c.off_card, false);

    // the sounds: the field that opens their list
    c.sound_row = ui_sound_field(c.body, w, FIELD_H, &c.sound_val);
    lv_obj_add_event_cb(c.sound_row, sound_cb, LV_EVENT_CLICKED, &c);

    // the timbre
    const int32_t name_w = tone_name_w();
    c.bass_sl = tone_slider(c, tr(S::sound_bass), c.bass_val, name_w);
    c.treble_sl = tone_slider(c, tr(S::sound_treble), c.treble_val, name_w);

    // the loudness: its name and the switch, no line under them
    rule(c.body, w);
    c.loud_row = m3w::box(c.body, 0, 0, w, ROW_H);
    lv_obj_t* lt = m3w::text(c.loud_row, t.t.body_l, t.c.on_surface, tr(S::eq_loudness));
    lv_obj_align(lt, LV_ALIGN_LEFT_MID, 0, 0);
    c.loud_sw = m3w::switch_m3(c.loud_row, w - 52, (ROW_H - 32) / 2, false);
    lv_obj_add_event_cb(c.loud_sw, loud_cb, LV_EVENT_VALUE_CHANGED, &c);
    rule(c.body, w);

    // the way to the equalizer, with what plays
    c.eq_row = m3w::box(c.body, 0, 0, w, ROW_H);
    lv_obj_add_flag(c.eq_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c.eq_row, eq_cb, LV_EVENT_CLICKED, &c);
    lv_obj_t* et = m3w::text(c.eq_row, t.t.body_l, t.c.on_surface, tr(S::page_eq));
    lv_obj_align(et, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* chev = m3w::icon(c.eq_row, t.t.icon_m, t.c.on_surface_var, UI_SYM_CHEVRON);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, 0, 0);
    c.eq_val = m3w::text(c.eq_row, t.t.label_m, t.c.on_surface_var, "");
    lv_obj_align(c.eq_val, LV_ALIGN_RIGHT_MID, -32, 0);
}

/// The page's section: a caption, «За розкладом», and the two times under it.
void sched_build(Controls& c) {
    const auto& t = m3::theme();
    const int32_t w = c.w;
    rule(c.body, w);
    lv_obj_t* cap = m3w::box(c.body, 0, 0, w, 40);
    lv_obj_t* ct = m3w::text(cap, t.t.label_m, t.c.on_surface_var, tr(S::sound_leveler));
    lv_obj_align(ct, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_t* r = m3w::box(c.body, 0, 0, w, ROW_H);
    lv_obj_t* st = m3w::text(r, t.t.body_l, t.c.on_surface, tr(S::sound_lev_sched));
    lv_obj_align(st, LV_ALIGN_LEFT_MID, 0, 0);
    c.sched_sw = m3w::switch_m3(r, w - 52, (ROW_H - 32) / 2, false);
    lv_obj_add_event_cb(c.sched_sw, sched_cb, LV_EVENT_VALUE_CHANGED, &c);
    const S names[2] = {S::sound_lev_from, S::sound_lev_to};
    for (int k = 0; k < 2; k++) {
        lv_obj_t* tr_row = m3w::box(c.body, 0, 0, w, ROW_H);
        lv_obj_t* n = m3w::text(tr_row, t.t.body_l, t.c.on_surface, tr(names[k]));
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        m3w::Button dec = m3w::icon_button(tr_row, w - 2 * TIME_BTN - TIME_VAL, 0, TIME_BTN, UI_SYM_REMOVE, t.t.icon_m);
        m3w::Button inc = m3w::icon_button(tr_row, w - TIME_BTN, 0, TIME_BTN, UI_SYM_ADD, t.t.icon_m);
        // a press steps once, a held one keeps stepping
        for (lv_event_code_t code : {LV_EVENT_SHORT_CLICKED, LV_EVENT_LONG_PRESSED, LV_EVENT_LONG_PRESSED_REPEAT}) {
            lv_obj_add_event_cb(dec.obj, time_cb, code, (void*)(intptr_t)(k << 1));
            lv_obj_add_event_cb(inc.obj, time_cb, code, (void*)(intptr_t)((k << 1) | 1));
        }
        c.time_val[k] = m3w::text(tr_row, t.t.label_m, t.c.on_surface, "");
        lv_obj_set_width(c.time_val[k], TIME_VAL);
        lv_obj_set_style_text_align(c.time_val[k], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(c.time_val[k], w - TIME_BTN - TIME_VAL, (ROW_H - lv_font_get_line_height(t.t.label_m)) / 2);
        c.time_row[k] = tr_row;
        uih::show(tr_row, false);
    }
}

// ── following the snapshot and the store ──

void show_value(lv_obj_t* lbl, int32_t v) {
    if (v < -6) v = -6; else if (v > 6) v = 6;
    char b[16];
    if (v > 0) snprintf(b, sizeof(b), "+%ld", (long)v);
    else if (v < 0) snprintf(b, sizeof(b), "\xE2\x88\x92%ld", (long)-v);   // U+2212, the minus
    else snprintf(b, sizeof(b), "0");
    lv_label_set_text(lbl, b);
}

void refresh_controls(Controls& c, UiCtx& ctx) {
    const auto& s = ctx.s;
    c.ctx = &ctx;
    const int preset = audio_core::eq_store().preset();
    if (preset != c.preset) {
        c.preset = preset;
        uih::set_text(c.sound_val, c.sound_val_txt, sizeof(c.sound_val_txt), ui_eq_preset_name(preset));
        c.eq_on = -1;                                 // the EQ row follows
    }
    const int8_t eq_on = s.eq_enabled ? 1 : 0;
    if (eq_on != c.eq_on) {
        c.eq_on = eq_on;
        uih::show(c.off_card, !eq_on);
        lv_obj_set_style_opa(c.sound_row, eq_on ? LV_OPA_COVER : LV_OPA_40, 0);
        uih::set_text(c.eq_val, c.eq_val_txt, sizeof(c.eq_val_txt),
                      eq_on ? ui_eq_preset_name(preset) : tr(S::sys_eq_off));
    }
    if (s.bass_db != c.bass) {
        c.bass = s.bass_db;
        if (!c.bass_drag) lv_slider_set_value(c.bass_sl, s.bass_db, LV_ANIM_OFF);
        show_value(c.bass_val, s.bass_db);
    }
    if (s.treble_db != c.treble) {
        c.treble = s.treble_db;
        if (!c.treble_drag) lv_slider_set_value(c.treble_sl, s.treble_db, LV_ANIM_OFF);
        show_value(c.treble_val, s.treble_db);
    }
    const int8_t loud_on = s.loudness > 0 ? 1 : 0;
    if (loud_on != c.loud_on) {
        c.loud_on = loud_on;
        if (loud_on) lv_obj_add_state(c.loud_sw, LV_STATE_CHECKED);
        else         lv_obj_remove_state(c.loud_sw, LV_STATE_CHECKED);
    }
    if (!c.sched_sw) return;                          // the sheet: no schedule
    const int8_t sched_on = s.lev_sched ? 1 : 0;
    if (sched_on != c.sched_on) {
        c.sched_on = sched_on;
        if (sched_on) lv_obj_add_state(c.sched_sw, LV_STATE_CHECKED);
        else          lv_obj_remove_state(c.sched_sw, LV_STATE_CHECKED);
        for (lv_obj_t* r : c.time_row) uih::show(r, sched_on);
    }
    // The snapshot follows a press within a second (mirror_diag, 2 Hz); until then the rows keep
    // what was pressed, so a held + does not jump back.
    if (esp_timer_get_time() / 1000 - c.time_pressed < TIME_HOLD_MS) return;
    const int32_t snap[2] = {s.lev_from, s.lev_to};
    for (int k = 0; k < 2; k++) {
        if (snap[k] == c.time_v[k]) continue;
        c.time_v[k] = snap[k];
        show_time(c.time_val[k], snap[k]);
    }
}

// ── the sheet ──

void close_sheet() {
    if (!sheet.open) return;
    sheet.open = false;
    uih::show(sheet.scrim, false);
    uih::show(sheet.panel, false);
    audio_core::eq_store().keep();                    // a sound chosen here is written now
}

void scrim_cb(lv_event_t*) { close_sheet(); }

// ── the page ──

lv_obj_t* page_build(lv_obj_t* parent, UiCtx& c) {
    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);
    build_controls(page, root, (uig::CONT_W - PAGE_W) / 2, 0, PAGE_W, uig::CONT_H, c);
    sched_build(page);
    refresh_controls(page, c);
    return root;
}

void page_refresh(lv_obj_t*, UiCtx& c) { refresh_controls(page, c); }

}  // namespace

const UiScreenDef UI_SCREEN_SOUND = {"sound", UI_SYM_EQ, page_build, page_refresh, nullptr, nullptr};

void ui_sound_build(lv_obj_t* scr, UiCtx& ctx) {
    const auto& t = m3::theme();
    sheet.open = false;
    sheet.scrim = m3w::surface(scr, 0, 0, uig::SCR_W, uig::SCR_H, 0x000000, 0);
    lv_obj_set_style_bg_opa(sheet.scrim, LV_OPA_50, 0);
    lv_obj_add_flag(sheet.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sheet.scrim, scrim_cb, LV_EVENT_CLICKED, nullptr);
    // Rounded on the left only: the right corners sit off-screen, one radius to the right.
    sheet.panel = m3w::surface(scr, uig::SCR_W - SHEET_W, 0, SHEET_W + SHEET_R, uig::SCR_H,
                               t.c.surface_container, SHEET_R);
    lv_obj_add_flag(sheet.panel, LV_OBJ_FLAG_CLICKABLE);    // swallow taps meant for the sheet
    lv_obj_t* title = m3w::text(sheet.panel, t.t.headline, t.c.on_surface, tr(S::sys_sound));
    lv_obj_set_pos(title, SIDE, HEAD_Y + (CLOSE - lv_font_get_line_height(t.t.headline)) / 2);
    m3w::Button x = m3w::icon_button(sheet.panel, SHEET_W - SIDE - CLOSE + 16, HEAD_Y, CLOSE, UI_SYM_CLOSE, t.t.icon_m);
    lv_obj_add_event_cb(x.obj, scrim_cb, LV_EVENT_CLICKED, nullptr);
    build_controls(sheet.c, sheet.panel, SIDE, HEAD_Y + CLOSE + 8, SHEET_W - 2 * SIDE,
                   uig::SCR_H - HEAD_Y - CLOSE - 8 - 16, ctx);
    uih::show(sheet.scrim, false);
    uih::show(sheet.panel, false);
}

void ui_sound_open() {
    if (!sheet.panel || sheet.open) return;
    sheet.open = true;
    uih::show(sheet.scrim, true);
    uih::show(sheet.panel, true);
    lv_obj_move_foreground(sheet.scrim);
    lv_obj_move_foreground(sheet.panel);
    sheet.c.preset = INT_MIN;                         // everything is read again
    if (sheet.c.ctx) refresh_controls(sheet.c, *sheet.c.ctx);
}

void ui_sound_close() {
    ui_pick_hide();                                   // a list it opened goes with it
    close_sheet();
}

lv_obj_t* ui_sound_field(lv_obj_t* parent, int32_t w, int32_t h, lv_obj_t** value) {
    const auto& t = m3::theme();
    lv_obj_t* f = m3w::box(parent, 0, 0, w, h);
    lv_obj_set_style_border_width(f, 1, 0);
    lv_obj_set_style_border_color(f, m3::col(t.c.outline), 0);
    lv_obj_set_style_radius(f, m3::shape::XS, 0);
    lv_obj_add_flag(f, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* cap = m3w::text(f, t.t.label_m, t.c.on_surface_var, tr(S::sound_styles));
    lv_obj_align(cap, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_t* drop = m3w::icon(f, t.t.icon_m, t.c.on_surface_var, UI_SYM_DROPDOWN);
    lv_obj_align(drop, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_t* v = m3w::text(f, t.t.body_l, t.c.on_surface, "");
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, -44, 0);
    if (value) *value = v;
    return f;
}

namespace {
/// The presets the list shows, row by row, and who takes the one chosen.
struct Presets {
    int  preset[UI_PICK_MAX] = {};
    void (*chosen)(int) = nullptr;
};
Presets s_presets;
void preset_chosen(int row) {
    if (s_presets.chosen) s_presets.chosen(s_presets.preset[row]);
}
}  // namespace

void ui_sound_pick_show(int now, void (*chosen)(int preset)) {
    const char* names[UI_PICK_MAX];
    int n = audio_core::eq_offered_count(now);
    if (n > UI_PICK_MAX) n = UI_PICK_MAX;
    int current = -1;
    for (int i = 0; i < n; i++) {
        const int p = audio_core::eq_offered(i, now);
        s_presets.preset[i] = p;
        names[i] = ui_eq_preset_name(p);
        if (p == now) current = i;
    }
    s_presets.chosen = chosen;
    ui_pick_show(tr(S::sound_styles), names, n, current, chosen ? preset_chosen : nullptr);
}

bool ui_sound_is_open() { return sheet.open; }

void ui_sound_refresh(UiCtx& ctx) {
    if (sheet.open) refresh_controls(sheet.c, ctx);
}
