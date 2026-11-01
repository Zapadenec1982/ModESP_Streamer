/**
 * @file screen_radio.cpp
 * @brief "Станції" — the six presets and every station there is. A tap tunes and goes back
 *        to the Player.
 *
 * The station that is playing, what it says is playing and the transport are the Player's
 * radio face; this page is the choice of station, opened from the Player's list button. The
 * presets sit at the top and scroll away with the list. They live here and not on the Source
 * page, which chooses a source, not a station.
 *
 * A preset works the way a car radio's does: a tap tunes it, and a long press stores the
 * station that is playing in it. An empty preset says "Зберегти сюди" while the radio plays,
 * and a tap on it stores — the one thing a tap on an empty preset could mean, and the way a
 * first-time user finds out how presets are filled.
 *
 * Tuning is choosing to listen, so it lands on the Player.
 *
 * The page is the Улюблені tab of the stations: the tab row above it leads to the
 * catalog's Топ · Країни · Жанри · Пошук (screen_catalog.cpp). A station from the catalog that is
 * playing goes into an empty preset the same way, through the catalog's asks.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "radio_catalog_ui.h"
#include "stations.h"
#include <cstdio>
#include <cstring>

namespace {

using i18n::S;
using i18n::tr;
using modesp::gen::AUDIO_SOURCE_RADIO;

// ── geometry, all relative to the 752x384 content column ──
constexpr int32_t TABS_H    = 68;                            // the tab row (screen_catalog.cpp), and a gap
constexpr int     N_PRE     = radio_core::PRESETS;          // 6
constexpr int32_t PRE_COLS  = 3;
constexpr int32_t PRE_GAP   = 8;
constexpr int32_t PRE_H     = 64;                            // a finger's
constexpr int32_t PRE_X0    = 4;                             // in line with the list's rows
constexpr int32_t PRE_W     = (uig::CONT_W - 2 * PRE_X0 - (PRE_COLS - 1) * PRE_GAP) / PRE_COLS;
constexpr int32_t PRE_Y0    = 2;
constexpr int32_t CAP_Y     = PRE_Y0 + 2 * PRE_H + PRE_GAP + 18;
constexpr int32_t HEAD_H    = CAP_Y + 30;                    // the presets, then "Усі станції"
static_assert(N_PRE == 2 * PRE_COLS, "the presets are two rows of three");

struct Page {
    UiCtx*     ctx = nullptr;
    m3w::VList list;
    lv_obj_t*  pre[N_PRE]     = {};
    lv_obj_t*  pre_dot[N_PRE] = {};
    lv_obj_t*  pre_num[N_PRE] = {};
    lv_obj_t*  pre_lbl[N_PRE] = {};
    lv_obj_t*  cap   = nullptr;
    lv_obj_t*  empty = nullptr;
    uint32_t   seen_gen = 0xFFFFFFFF;
    int        shown_station = -2;
    int8_t     shown_live = -1;
    char       cap_txt[48] = "";
};

Page g;

void post(UiLvglModule::Req kind, int32_t v) {
    if (g.ctx && g.ctx->mod) g.ctx->mod->lvgl_post(kind, v);
}

/// The radio owns the output and plays a station from the list (not a DLNA push).
bool radio_on(const UiLvglModule::Snapshot& s) { return ui_src(s) == UiSrc::RADIO; }

/// …and is playing it, or on its way to.
bool radio_live(const UiLvglModule::Snapshot& s) {
    return radio_on(s) && (strcmp(s.radio_state, "playing") == 0 ||
                           strcmp(s.radio_state, "connecting") == 0 ||
                           strcmp(s.radio_state, "buffering") == 0 ||
                           strcmp(s.radio_state, "reconnecting") == 0);
}

/// Something that can go into a preset is playing: a station from the list, or one from the
/// catalog (which the radio publishes as station −1, with its name).
bool can_store(const UiLvglModule::Snapshot& s) {
    return radio_on(s) && (s.radio_station >= 0 || s.radio_name[0]);
}

/// The station playing goes into preset `slot`. One from the list is stored here, as always;
/// one from the catalog is kept in the list first, which the radio module does.
void store(int slot) {
    if (!g.ctx || !can_store(g.ctx->s)) return;
    if (g.ctx->s.radio_station >= 0) {
        post(UiLvglModule::Req::RADIO_STORE, slot);
        return;
    }
    radio_catalog::Ask a;
    a.kind = radio_catalog::AskKind::KEEP_PLAYING;
    a.slot = slot;
    radio_catalog::asks().post(a);
}

/// Listen to station `idx`, and go to where listening is shown.
///
/// The source first: the radio module only exists while it owns the output, and drops what
/// is posted to it before then (radio_module.cpp, on_update) — a preset tapped while
/// Bluetooth played did nothing until this line. Then a new station is a tune, and the station
/// already tuned needs a play, since writing the same number again changes nothing. The
/// requests are drained in order, so the radio sees the source before the rest.
void listen(int idx) {
    if (!g.ctx || idx < 0) return;
    const auto& s = g.ctx->s;
    if (s.source != AUDIO_SOURCE_RADIO) post(UiLvglModule::Req::SOURCE, AUDIO_SOURCE_RADIO);
    if (idx != s.radio_station)         post(UiLvglModule::Req::RADIO_TUNE, idx);
    else if (!radio_live(s))            post(UiLvglModule::Req::RADIO_CMD, 1);
    ui_shell_show_player();
}

/// The host of a stream URL: enough to tell two stations of the same name apart, without the
/// path and the query nobody reads.
void host_of(const char* url, char* out, size_t n) {
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t k = 0;
    while (p[k] && p[k] != '/' && p[k] != ':' && p[k] != '?' && k + 1 < n) { out[k] = p[k]; k++; }
    out[k] = '\0';
}

// ── the list ──

/// The row's texts stay static: VList reads them after this returns, and a station with no name
/// is titled by its host.
void fill(int idx, m3w::VItem& it, void*) {
    static radio_core::Station st;
    static char sub[96];
    static char host[64];
    if (!radio_core::stations().at(idx, st)) { it.title = "—"; return; }
    host_of(st.url, host, sizeof(host));
    int slot = -1;
    for (int i = 0; i < N_PRE && slot < 0; i++) if (radio_core::stations().preset(i) == idx) slot = i;
    if (slot >= 0) {
        char n[12], preset[24];      // "Speicherplatz 64", the longest, is 16 bytes
        snprintf(n, sizeof(n), "%d", slot + 1);
        i18n::fmt(preset, sizeof(preset), tr(S::st_preset_n), {{"n", n}});
        snprintf(sub, sizeof(sub), "%s · %s", host, preset);
    }
    else           snprintf(sub, sizeof(sub), "%s", host);
    const bool on = g.ctx && radio_on(g.ctx->s) && idx == g.ctx->s.radio_station;
    it.lead  = UI_SYM_RADIO;
    it.title = st.name[0] ? st.name : host;
    it.sub   = sub;
    it.trail = (on && radio_live(g.ctx->s)) ? UI_SYM_GRAPHIC_EQ : nullptr;
    it.on    = on;
}

void pick(int idx, void*) { listen(idx); }

// ── the presets ──

void preset_tap(int slot) {
    if (!g.ctx) return;
    const int st = radio_core::stations().preset(slot);
    if (st >= 0) { listen(st); return; }
    // An empty preset: store what is playing, when there is something to store.
    store(slot);
}

void preset_cb(lv_event_t* e) {
    const int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) store(slot);
    else                                               preset_tap(slot);
}

void paint_presets() {
    const auto& t = m3::theme();
    auto& list = radio_core::stations();
    radio_core::Station st;
    const bool storable = g.ctx && can_store(g.ctx->s);
    for (int i = 0; i < N_PRE; i++) {
        const int idx = list.preset(i);
        const bool have = idx >= 0 && list.at(idx, st);
        const bool on = have && g.ctx && radio_on(g.ctx->s) && idx == g.ctx->s.radio_station;
        lv_obj_set_style_bg_color(g.pre[i], m3::col(on ? t.c.secondary_container : t.c.surface_container), 0);
        lv_obj_set_style_bg_color(g.pre_dot[i], m3::col(on ? t.c.primary : t.c.surface_highest), 0);
        lv_obj_set_style_text_color(g.pre_num[i], m3::col(on ? t.c.on_primary : t.c.on_surface_var), 0);
        if (have) {
            lv_label_set_text(g.pre_lbl[i], st.name[0] ? st.name : "—");
            lv_obj_set_style_text_color(g.pre_lbl[i], m3::col(on ? t.c.on_secondary_container : t.c.on_surface), 0);
        } else {
            lv_label_set_text(g.pre_lbl[i], tr(storable ? S::st_preset_store : S::st_preset_empty));
            lv_obj_set_style_text_color(g.pre_lbl[i], m3::col(storable ? t.c.primary : t.c.on_surface_var), 0);
        }
    }
}

// ── build ──

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    g = Page{};
    g.ctx = &c;
    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);
    ui_station_tabs(root, 0);

    g.list.build(root, 0, TABS_H, uig::CONT_W, uig::CONT_H - TABS_H, fill, pick, nullptr, nullptr, HEAD_H);
    lv_obj_t* head = g.list.head();

    for (int i = 0; i < N_PRE; i++) {
        const int32_t x = PRE_X0 + (i % PRE_COLS) * (PRE_W + PRE_GAP);
        const int32_t y = PRE_Y0 + (i / PRE_COLS) * (PRE_H + PRE_GAP);
        lv_obj_t* p = m3w::surface(head, x, y, PRE_W, PRE_H, t.c.surface_container, m3::shape::M);
        lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
        // SHORT_CLICKED, not CLICKED: LVGL sends CLICKED after a long press as well, and a
        // store must not also tune.
        lv_obj_add_event_cb(p, preset_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(p, preset_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)i);
        g.pre[i] = p;
        g.pre_dot[i] = m3w::surface(p, 12, (PRE_H - 32) / 2, 32, 32, t.c.surface_highest, m3::shape::FULL);
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        g.pre_num[i] = m3w::text(g.pre_dot[i], t.t.label_m, t.c.on_surface_var, num);
        lv_obj_center(g.pre_num[i]);
        g.pre_lbl[i] = m3w::text(p, t.t.label_m, t.c.on_surface, "");
        const int32_t lh = lv_font_get_line_height(t.t.label_m);
        lv_obj_set_pos(g.pre_lbl[i], 56, (PRE_H - lh) / 2);
        lv_obj_set_size(g.pre_lbl[i], PRE_W - 56 - 12, lh);
        lv_label_set_long_mode(g.pre_lbl[i], LV_LABEL_LONG_DOT);
    }

    g.cap = m3w::text(head, t.t.label_m, t.c.on_surface_var, "");
    lv_obj_set_pos(g.cap, PRE_X0 + 12, CAP_Y);

    g.empty = m3w::text(root, t.t.body_l, t.c.on_surface_var, tr(S::st_none));
    lv_obj_set_pos(g.empty, PRE_X0 + 12, TABS_H + HEAD_H + 8);
    uih::show(g.empty, false);
    return root;
}

// ── refresh ──

void refresh(lv_obj_t*, UiCtx& c) {
    const auto& s = c.s;
    auto& list = radio_core::stations();
    const uint32_t gen = list.generation();
    bool repaint = false;
    if (gen != g.seen_gen && !(gen & 1u)) {
        const bool first = g.seen_gen == 0xFFFFFFFF;
        g.seen_gen = gen;
        const int count = list.count();
        // A preset stored changes the generation too, and must not throw the list back to
        // the top under a finger; only a list of another length is a new list.
        if (first || count != g.list.count()) g.list.set_count(count);
        char cap[48], n[12];
        snprintf(n, sizeof(n), "%d", count);
        i18n::fmt(cap, sizeof(cap), tr(S::st_all), {{"n", n}});
        uih::set_text(g.cap, g.cap_txt, sizeof(g.cap_txt), cap);
        uih::show(g.empty, count == 0);
        repaint = true;
    }
    const int station = radio_on(s) ? s.radio_station : -1;
    const int8_t live = radio_live(s) ? 1 : 0;
    if (repaint || station != g.shown_station || live != g.shown_live) {
        g.shown_station = station;
        g.shown_live = live;
        paint_presets();
        g.list.refill();
    }
}

void shown(lv_obj_t*, UiCtx&) {
    g.list.scroll_top();
    paint_presets();
}

}  // namespace

const UiScreenDef UI_SCREEN_RADIO = {"radio", UI_SYM_RADIO, build, refresh, shown, nullptr};
