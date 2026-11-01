/**
 * @file ui_shell.cpp
 * @brief The frame every page lives in: the header, the one menu (a modal drawer), the
 *        snackbar, the standby face — and page switching.
 *
 * The structure: the Player is the
 * home and owns the screen; nothing permanent stands beside a 48 px header. There is ONE
 * menu, the drawer — opened by ≡, by a press on the left edge or by holding the encoder —
 * with the three sections: Плеєр · Джерело · Налаштування (DSP is not one of them:
 * the equalizer is a list under Налаштування › Звук, for
 * those who want it, and the daily sound goes to the Player). Nothing else is in it: the rail
 * the design package drew was dropped (88 px off every page for something wanted a few times
 * a day), and so were its "sleep" and "off" actions — standby comes by itself on the idle
 * timer, and switching the output off is not a touch interface's job. Lists (stations, DLNA,
 * the service readout) are reached from a page and their ← goes back to it. Left alone for a
 * minute, any page goes back to the Player by itself (idle_home.h).
 *
 * Pages are built once and shown/hidden, and a page can be redrawn without the shell changing
 * for it.
 *
 * Volume is a moment, not a readout: a turn of the encoder brings up a slider for two
 * seconds, and the Player's loudspeaker button does the same.
 *
 * The knob has one job on every page: a turn is the volume, a press plays and pauses (mutes a
 * station), a hold opens the drawer. It used to walk each page's rows with a ring, and set the
 * volume where a page did not take it — what a turn did depended on the page and on whether a
 * ring was up: a mode, where people err. What is on the glass is chosen by touch.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "idle_home.h"
#include "device_name.h"
#include "bt_settings.h"
#include "eq_store.h"
#include "spectrum.h"
#include "esp_log.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "UiShell";

namespace {

using i18n::S;
using i18n::tr;

// ── the pages ──────────────────────────────────────────────────────────────────────────────

enum class Kind : uint8_t {
    HOME,     ///< the Player: ≡ and no title — the content says what it is
    SECTION,  ///< a drawer destination: ≡ and the title
    LIST,     ///< reached from a page: ← back to that page, and the title
};

struct PageDef {
    const UiScreenDef* def;
    S                  title;   ///< title case; the old defs carry caps for the old top bar
    Kind               kind;
    const char*        sym;     ///< the drawer's icon; only the menu's pages have one
};

constexpr PageDef PAGES[] = {
    { &UI_SCREEN_NOW,     S::nav_player,         Kind::HOME,    UI_SYM_PLAY_CIRCLE },
    { &UI_SCREEN_SRC,     S::nav_source,         Kind::SECTION, UI_SYM_SPEAKER },
    { &UI_SCREEN_SET,     S::nav_settings,       Kind::SECTION, UI_SYM_SETTINGS },
    // the lists: each opened from the Player's list button, for the source that is playing
    { &UI_SCREEN_RADIO,   S::page_stations,      Kind::LIST,    nullptr },
    { &UI_SCREEN_LIBRARY, S::page_library_short, Kind::LIST,    nullptr },
    { &UI_SCREEN_CATALOG, S::page_stations,      Kind::LIST,    nullptr },   // the station list's other tabs
    { &UI_SCREEN_DLNA,    S::src_dlna,           Kind::LIST,    nullptr },
    { &UI_SCREEN_SVC,     S::page_service,       Kind::LIST,    nullptr },
    { &UI_SCREEN_WIFI,    S::page_wifi,          Kind::LIST,    nullptr },
    { &UI_SCREEN_BT,      S::src_bt,             Kind::LIST,    nullptr },
    // Налаштування's own pages: the sound and its equalizer (Звук), the screen (Пристрій)
    { &UI_SCREEN_EQ,      S::page_eq,            Kind::LIST,    nullptr },
    { &UI_SCREEN_DISPLAY, S::sys_screen,         Kind::LIST,    nullptr },
    { &UI_SCREEN_SPEAKERS, S::spk_title,         Kind::LIST,    nullptr },
    { &UI_SCREEN_XOVER,   S::xover_title,        Kind::LIST,    nullptr },
    { &UI_SCREEN_SOUND,   S::sys_timbre,         Kind::LIST,    nullptr },
#if CONFIG_UI_M3_TEST_SCREEN
    { &UI_SCREEN_M3TEST,  S::page_m3test,        Kind::LIST,    nullptr },   // bench only: ?screen=m3test
#endif
};
constexpr int N_PAGES    = sizeof(PAGES) / sizeof(PAGES[0]);
constexpr int PAGE_HOME  = 0;
constexpr int PAGE_RADIO = 3;
constexpr int PAGE_CATALOG = 5;
static_assert(PAGES[PAGE_HOME].kind == Kind::HOME, "the home page comes first");
static_assert(PAGES[PAGE_RADIO].def == &UI_SCREEN_RADIO, "PAGE_RADIO names the radio page");
static_assert(PAGES[PAGE_CATALOG].def == &UI_SCREEN_CATALOG, "PAGE_CATALOG names the catalog");

// ── the menu: the first N_MENU pages, in this order ────────────────────────────────────────

constexpr int N_MENU = 3;
static_assert(PAGES[1].kind == Kind::SECTION && PAGES[2].kind == Kind::SECTION &&
              PAGES[N_MENU].kind == Kind::LIST, "the sections are the menu's rows 1..2, and only they");

// ── geometry ──
constexpr int32_t D_RADIUS = m3::shape::XL;     ///< the drawer's right-hand corners
/// Rows, their text and their icons well above the M3 drawer's (label_m, 24 px icons): those
/// are too small to read from where a stereo is looked at, and 24 px still small. The rows are
/// set in headline (32 px), the title stays at title_m.
constexpr int32_t D_ROW_H  = 72;
constexpr int32_t D_ROW_Y0 = 84;
constexpr int32_t D_PITCH  = D_ROW_H + 4;

struct Shell {
    UiCtx* ctx = nullptr;
    // header
    m3w::Button nav;                     ///< ≡ on the home and the sections, ← on a list
    lv_obj_t*   title   = nullptr;
    lv_obj_t*   clock   = nullptr;
    lv_obj_t*   wifi_ic = nullptr;
    // pages
    lv_obj_t* stack = nullptr;
    lv_obj_t* page[N_PAGES] = {};
    int       cur  = PAGE_HOME;
    int       from = PAGE_HOME;          ///< where the list that is open was opened from
    int       parent = -1;               ///< the list it was opened from, when one list led to it
    // drawer
    lv_obj_t* edge   = nullptr;
    lv_obj_t* scrim  = nullptr;
    lv_obj_t* drawer = nullptr;
    lv_obj_t* row[N_MENU]     = {};
    lv_obj_t* row_ico[N_MENU] = {};
    lv_obj_t* row_lbl[N_MENU] = {};
    bool      drawer_open = false;
    lv_obj_t* web      = nullptr;        ///< the web's address and QR code at the menu's foot
    lv_obj_t* web_qr   = nullptr;
    lv_obj_t* web_addr = nullptr;
    char      web_txt[16] = "";
    // the volume pop-up
    lv_obj_t*   vol = nullptr;
    m3w::Slider vol_sl;
    touch::Slide vol_touch;          ///< a finger on it (touch_gesture.h)
    uint32_t    vol_at = 0;          ///< when it last had a reason to be on screen; 0 = hidden
    bool        vol_drag = false;    ///< a finger is on it: the snapshot does not move it
    int32_t     vol_posted = -1;     ///< the level the glass asked for last: the knob, or a finger on the pop-up
    uint32_t    vol_posted_at = 0;   ///< when
    // «Скасувати» (ui_shell_undo)
    lv_obj_t*   undo_bar = nullptr;
    lv_obj_t*   undo_lbl = nullptr;
    m3w::Button undo_btn;
    int         undo_key = 0;
    uint32_t    undo_at  = 0;        ///< when it went up; 0 = down
    void      (*undo_fn)(int32_t, void*) = nullptr;
    int32_t     undo_val = 0;
    void*       undo_arg = nullptr;
    // standby
    lv_obj_t* sb = nullptr;
    lv_obj_t* sb_clock = nullptr;        ///< the row of three labels: hours, colon, minutes
    lv_obj_t* sb_hh = nullptr;
    lv_obj_t* sb_mm = nullptr;
    lv_obj_t* sb_date = nullptr;
    lv_obj_t* sb_line = nullptr;         ///< what is playing, one line that scrolls when long
    lv_obj_t* sb_spec = nullptr;
    bool      standby = false;
    int8_t    sb_music = -1;             ///< the face's layout: 1 music, 0 the clock alone
    // change detection
    char    clock_txt[12] = "";
    char    hh_txt[4] = "";
    char    mm_txt[4] = "";
    char    date_txt[12] = "";
    char    sb_line_txt[288] = "";       ///< station 64 + title 128, or artist 96 + title 96, + markup
    bool    wifi_last = false;
    bool    wifi_init = false;
    int32_t vol_last = -1;
    bool    mute_last = false;
    // the pairing dialog (link protocol 1.8): over every page, and it wakes the screen
    m3w::Dialog pair;
    lv_obj_t*   pair_code = nullptr;
    uint32_t    pair_seq  = 0;         ///< the request the dialog is about
    uint8_t     pair_kind = 0;
    char        pair_name[34] = "";
    bool        pair_open = false;
    bool        pair_done = false;     ///< showing the result, about to close
    bool        pair_there = false;    ///< answered in the web while the dialog waited
    uint32_t    pair_done_at = 0;
};

Shell g;
/// Outside `g`, which a rebuild starts over (ui_shell_build): a scheme or a language changed from
/// the web must not give an idle period that is spent a second return to the Player.
idle_home::Period s_idle;

void set_drawer(bool open);
void paint_drawer();

// ── pages and header ───────────────────────────────────────────────────────────────────────

/// The destination the current page belongs to: itself, or, for a list, the page it was
/// opened from.
int destination() { return PAGES[g.cur].kind == Kind::LIST ? g.from : g.cur; }

void select_page(int idx, bool back = false) {
    if (idx < 0 || idx >= N_PAGES) return;
    const PageDef& p = PAGES[idx];
    // A list remembers where it was opened from. One list leading to another keeps that first
    // origin and remembers the list too, so ← from Еквалайзер opened on Налаштування › Звук
    // lands on Звук, and from there on Налаштування — as the web goes back.
    // Мої and the catalog's tabs are one page (ui_station_tabs): a tab does not lead to another.
    const bool tabs = (idx == PAGE_RADIO || idx == PAGE_CATALOG) && (g.cur == PAGE_RADIO || g.cur == PAGE_CATALOG);
    if (p.kind == Kind::LIST && PAGES[g.cur].kind != Kind::LIST) { g.from = g.cur; g.parent = -1; }
    else if (p.kind == Kind::LIST && !back && idx != g.cur && !tabs) g.parent = g.cur;
    else if (p.kind != Kind::LIST) g.parent = -1;
    for (int i = 0; i < N_PAGES; i++) uih::show(g.page[i], i == idx);
    g.cur = idx;
    lv_label_set_text(g.title, p.kind == Kind::HOME ? "" : tr(p.title));
    if (g.nav.icon) lv_label_set_text(g.nav.icon, p.kind == Kind::LIST ? UI_SYM_BACK : UI_SYM_MENU);
    if (g.drawer_open) paint_drawer();
    if (p.def->shown && g.ctx) p.def->shown(g.page[idx], *g.ctx);
}

void nav_cb(lv_event_t*) { ui_shell_nav(); }
void edge_cb(lv_event_t*) { set_drawer(true); }

// ── drawer ─────────────────────────────────────────────────────────────────────────────────

/// The web at the menu's foot: the panel's address and a QR code of the web's front page, from
/// any page; nothing while the panel has no address.
void paint_web() {
    if (!g.web || !g.ctx) return;
    const char* ip = g.ctx->s.wifi_ip;
    if (strncmp(ip, g.web_txt, sizeof(g.web_txt)) == 0) return;      // built hidden, with no address
    uih::set_text(g.web_addr, g.web_txt, sizeof(g.web_txt), ip);
#if LV_USE_QRCODE
    if (ip[0]) {
        char url[32];
        const int n = snprintf(url, sizeof(url), "http://%s/", ip);
        lv_qrcode_update(g.web_qr, url, (uint32_t)n);
    }
#endif
    uih::show(g.web, ip[0] != '\0');
}

void paint_drawer() {
    const auto& t = m3::theme();
    const int dest = destination();
    paint_web();
    for (int i = 0; i < N_MENU; i++) {
        const bool on = (i == dest);
        const uint32_t fg = on ? t.c.on_secondary_container : t.c.on_surface_var;
        lv_obj_set_style_bg_opa(g.row[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(g.row_ico[i], m3::col(fg), 0);
        lv_obj_set_style_text_font(g.row_ico[i], on ? t.t.icon_l_fill : t.t.icon_l, 0);
        lv_obj_set_style_text_color(g.row_lbl[i], m3::col(fg), 0);
    }
}

void set_drawer(bool open) {
    if (g.drawer_open == open) return;
    g.drawer_open = open;
    uih::show(g.scrim, open);
    uih::show(g.drawer, open);
    if (open) {
        lv_obj_move_foreground(g.scrim);
        lv_obj_move_foreground(g.drawer);
        paint_drawer();
    }
}

void menu_activate(int i) {
    set_drawer(false);
    select_page(i);
}

void scrim_cb(lv_event_t*)   { set_drawer(false); }
void row_cb(lv_event_t* e)   { menu_activate((int)(intptr_t)lv_event_get_user_data(e)); }

void build_drawer(lv_obj_t* scr) {
    const auto& t = m3::theme();

    g.scrim = m3w::surface(scr, 0, 0, uig::SCR_W, uig::SCR_H, 0x000000, 0);
    lv_obj_set_style_bg_opa(g.scrim, LV_OPA_50, 0);
    lv_obj_add_flag(g.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g.scrim, scrim_cb, LV_EVENT_CLICKED, nullptr);

    // Rounded on the right only: the left corners sit off-screen, one radius to the left.
    g.drawer = m3w::surface(scr, -D_RADIUS, 0, uig::DRAWER_W + D_RADIUS, uig::SCR_H,
                            t.c.surface_low, D_RADIUS);
    lv_obj_add_flag(g.drawer, LV_OBJ_FLAG_CLICKABLE);    // swallow taps meant for the drawer
    const int32_t DX = D_RADIUS;                          // children: screen x = x − DX

    char nm[device_name::CAP];
    device_name::get(nm, sizeof nm);                     // the panel's one name (device_name.h)
    lv_obj_t* name = m3w::text(g.drawer, t.t.title_m, t.c.on_surface_var, nm);
    // level with the close button's centre; the button is a finger's 64 px (ui_m3_widgets.h)
    lv_obj_set_pos(name, DX + 28, 4 + (64 - lv_font_get_line_height(t.t.title_m)) / 2);
    m3w::Button close = m3w::icon_button(g.drawer, DX + uig::DRAWER_W - 8 - 64, 4, 64,
                                         UI_SYM_MENU_OPEN, t.t.icon_m);
    lv_obj_add_event_cb(close.obj, scrim_cb, LV_EVENT_CLICKED, nullptr);

    for (int i = 0; i < N_MENU; i++) {
        const int32_t y = D_ROW_Y0 + i * D_PITCH;
        lv_obj_t* row = m3w::surface(g.drawer, DX + 12, y, uig::DRAWER_W - 24, D_ROW_H,
                                     t.c.secondary_container, m3::shape::FULL);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        g.row[i] = row;
        g.row_ico[i] = m3w::icon(row, t.t.icon_l, t.c.on_surface_var, PAGES[i].sym);
        lv_obj_align(g.row_ico[i], LV_ALIGN_LEFT_MID, 20, 0);
        g.row_lbl[i] = m3w::text(row, t.t.headline, t.c.on_surface_var, tr(PAGES[i].title));
        lv_obj_align(g.row_lbl[i], LV_ALIGN_LEFT_MID, 64, 0);
    }

    // the web, at the foot: the QR code dark on a white square (its margin the quiet zone), and
    // beside it «Вебінтерфейс» over the address (paint_web)
    constexpr int32_t QR = 116;
    static_assert(D_ROW_Y0 + N_MENU * D_PITCH + 16 <= uig::SCR_H - 24 - QR, "the QR code under the rows");
    g.web = m3w::box(g.drawer, DX + 24, uig::SCR_H - 24 - QR, uig::DRAWER_W - 48, QR);
    int32_t tx = 0;
#if LV_USE_QRCODE
    lv_obj_t* sq = m3w::surface(g.web, 0, 0, QR, QR, 0xFFFFFF, m3::shape::XS);
    g.web_qr = lv_qrcode_create(sq);
    lv_qrcode_set_size(g.web_qr, QR - 16);
    lv_qrcode_set_dark_color(g.web_qr, lv_color_black());
    lv_qrcode_set_light_color(g.web_qr, lv_color_white());
    lv_obj_center(g.web_qr);
    tx = QR + 16;
#endif
    const int32_t lh_cap = lv_font_get_line_height(t.t.label_m);
    lv_obj_t* cap = m3w::text(g.web, t.t.label_m, t.c.on_surface_var, tr(S::nav_web));
    lv_obj_set_pos(cap, tx, QR / 2 - lh_cap - 2);
    g.web_addr = m3w::text(g.web, t.t.body_l, t.c.on_surface, "");
    lv_obj_set_pos(g.web_addr, tx, QR / 2 + 2);
    uih::show(g.web, false);

    uih::show(g.scrim, false);
    uih::show(g.drawer, false);
}

// ── the volume pop-up ──────────────────────────────────────────────────────────────────────
//
// A slider that appears when the volume is being changed and leaves two seconds after it
// stops: a permanent one would be a readout of the thing the encoder already is. It follows
// the snapshot, so the encoder and the web page both move it, and a finger on it posts the
// value it is dragged to.

constexpr uint32_t VOL_MS = 2000;
/// A level asked for reaches the snapshot within ~0.1 s (the next request drain, then the 20 Hz
/// mirror, then a refresh); until this long after it, the knob steps from it, not the snapshot.
constexpr uint32_t VOL_HOLD_MS = 300;

void vol_show() {
    g.vol_at = lv_tick_get();
    // Not raised: built before every other overlay, it stays under the menu, the «Звук» sheet, the
    // pick list and the dialogs (docs/UI.md, "Overlays") rather than over their buttons, where it
    // took the taps meant for them for as long as it showed.
    if (lv_obj_has_flag(g.vol, LV_OBJ_FLAG_HIDDEN)) uih::show(g.vol, true);
}

void vol_hide() {
    g.vol_at = 0;
    g.vol_drag = false;
    uih::show(g.vol, false);
}

/// Only a finger on the knob moves the volume, once it went along, and from where the knob stood
/// (m3w::slider_touch); a press, a tap or a finger elsewhere sets nothing — a jump to where a
/// finger lands is the loudest false touch there is.
void vol_touch_cb(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    int32_t pct = 0;
    const m3w::Slid r = m3w::slider_touch(e, g.vol_sl, g.vol_touch, pct);
    g.vol_at = lv_tick_get();
    if (code == LV_EVENT_PRESSED) g.vol_drag = g.vol_touch.state() == touch::Slide::PRESS;
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) g.vol_drag = false;
    if (r != m3w::Slid::MOVE) return;
    m3w::slider_set(g.vol_sl, pct);
    if (pct != g.vol_posted && g.ctx && g.ctx->mod) {
        g.vol_posted = pct;
        g.vol_posted_at = lv_tick_get();
        g.ctx->mod->lvgl_post(UiLvglModule::Req::VOLUME, pct);
    }
}

void build_volume(lv_obj_t* scr) {
    const auto& t = m3::theme();
    constexpr int32_t W = 480, H = m3w::SLIDER_H + 24;
    g.vol = m3w::surface(scr, (uig::SCR_W - W) / 2, uig::SCR_H - H - 24, W, H,
                         t.c.surface_high, m3::shape::XXL);
    lv_obj_add_flag(g.vol, LV_OBJ_FLAG_CLICKABLE);      // a tap beside the slider is not a tap through
    g.vol_sl = m3w::slider(g.vol, 16, (H - m3w::SLIDER_H) / 2, W - 32, UI_SYM_VOLUME);
    lv_obj_add_event_cb(g.vol_sl.obj, vol_touch_cb, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(g.vol_sl.obj, vol_touch_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(g.vol_sl.obj, vol_touch_cb, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(g.vol_sl.obj, vol_touch_cb, LV_EVENT_PRESS_LOST, nullptr);
    uih::show(g.vol, false);
}

void refresh_volume(const UiLvglModule::Snapshot& s) {
    // Shown on a change, and only then. The first pass goes from "no audio core yet" to a
    // real number, which is not a change the user made.
    if (s.volume != g.vol_last || s.mute != g.mute_last) {
        const bool first = (g.vol_last < 0);
        g.vol_last  = s.volume;
        g.mute_last = s.mute;
        if (g.vol_sl.lead) lv_label_set_text(g.vol_sl.lead, s.mute ? UI_SYM_MUTE : UI_SYM_VOLUME);
        if (!first && s.volume >= 0) vol_show();
    }
    if (!g.vol_at) return;
    if (!g.vol_drag && s.volume >= 0) m3w::slider_set(g.vol_sl, s.volume);
    if (!g.vol_drag && lv_tick_elaps(g.vol_at) > VOL_MS) vol_hide();
}

// ── «Скасувати» ────────────────────────────────────────────────────────────────────────────

void undo_hide() {
    g.undo_at = 0;
    g.undo_fn = nullptr;
    uih::show(g.undo_bar, false);
}

void undo_cb(lv_event_t*) {
    void (*fn)(int32_t, void*) = g.undo_fn;
    const int32_t v = g.undo_val;
    void* arg = g.undo_arg;
    undo_hide();
    if (fn) fn(v, arg);
}

/// The snackbar of the mock-ups (V2_Panel_EQ): inverse surface at the bottom, over the pages and
/// the «Звук» sheet. Wide enough for a band's «1,00 kHz +2,0 dB › 1,20 kHz +3,5 dB».
void build_undo(lv_obj_t* scr) {
    const auto& t = m3::theme();
    constexpr int32_t W = 620, H = 64, BTN_W = 150;
    g.undo_bar = m3w::surface(scr, (uig::SCR_W - W) / 2, uig::SCR_H - H - 14, W, H,
                              t.c.inverse_surface, m3::shape::XS);
    lv_obj_add_flag(g.undo_bar, LV_OBJ_FLAG_CLICKABLE);    // a tap beside the button is not a tap through
    lv_obj_set_style_shadow_color(g.undo_bar, m3::col(0x000000), 0);
    lv_obj_set_style_shadow_width(g.undo_bar, 20, 0);
    lv_obj_set_style_shadow_offset_y(g.undo_bar, 6, 0);
    lv_obj_set_style_shadow_opa(g.undo_bar, LV_OPA_40, 0);
    g.undo_lbl = m3w::text(g.undo_bar, t.t.label_m, t.c.inverse_on_surface, "");
    lv_obj_set_width(g.undo_lbl, W - 20 - BTN_W - 8);
    lv_label_set_long_mode(g.undo_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(g.undo_lbl, LV_ALIGN_LEFT_MID, 20, 0);
    g.undo_btn = m3w::button_text(g.undo_bar, W - BTN_W - 4, 0, BTN_W, H, tr(S::undo_action));
    // Material's inverse_primary: the primary of the light scheme, which primary_container is
    if (g.undo_btn.label) lv_obj_set_style_text_color(g.undo_btn.label, m3::col(t.c.primary_container), 0);
    lv_obj_add_event_cb(g.undo_btn.obj, undo_cb, LV_EVENT_CLICKED, nullptr);
    uih::show(g.undo_bar, false);
}

// ── standby ────────────────────────────────────────────────────────────────────────────────

/// A click on the face wakes it. A finger on the glass already has through the touch driver
/// (UiLvglModule::publish_touch), and a second WAKE only re-syncs the shell's standby face with
/// the module; this is for the presses that do
/// not come from the touch controller — the bench's tap=, which could not wake the panel.
void sb_click_cb(lv_event_t*) {
    if (g.ctx && g.ctx->mod) g.ctx->mod->lvgl_post(UiLvglModule::Req::WAKE, 0);
}

// The face has two layouts. With music playing: the clock and the date at the top, one line
// of what is playing under them, and the spectrum at the bottom. With nothing playing: the
// clock and the date alone, centred. The line and the spectrum share one window, 80 % of the
// screen's width; a line wider than that scrolls.
constexpr int32_t SB_WIN_W  = uig::SCR_W * 80 / 100;                 // 640
constexpr int32_t SB_WIN_X  = (uig::SCR_W - SB_WIN_W) / 2;
constexpr int32_t SB_TOP    = 28;        ///< the clock row, music layout
constexpr int32_t SB_DATE_GAP = 6;       ///< under the clock's line box, which has air of its own
constexpr int32_t SB_LINE_GAP = 18;      ///< date → the line
constexpr int32_t SB_SPEC_BASE = uig::SCR_H - 24;
constexpr int32_t SB_SPEC_H    = 180;
/// 23 px a band and 12 of it bar: 28 bands come to 633 px, the nearest the pitch gets to the
/// window without bars of two widths. Otherwise the Player's row exactly — no peak caps, which
/// were tried and dropped so the spectrum looks the same wherever it is.
constexpr uih::SpecStyle SB_SPEC = {23, 12};
constexpr int32_t SB_SPEC_X = (uig::SCR_W - uih::spec_drawn_w(SB_SPEC)) / 2;
static_assert(uih::spec_drawn_w(SB_SPEC) <= SB_WIN_W, "the spectrum fits the window");
/// How fast the line runs, px/s. Set per text rather than left to LVGL, whose default caps a
/// loop at 10 s and so runs a long title several times as fast as a short one.
constexpr uint32_t SB_SCROLL_PX_S = 45;

void sb_layout(bool music) {
    const auto& t = m3::theme();
    const int32_t clock_h = lv_font_get_line_height(t.t.clock);
    const int32_t date_h  = lv_font_get_line_height(t.t.title_m);
    const int32_t y = music ? SB_TOP : (uig::SCR_H - (clock_h + SB_DATE_GAP + date_h)) / 2;
    lv_obj_set_y(g.sb_clock, y);
    lv_obj_align(g.sb_date, LV_ALIGN_TOP_MID, 0, y + clock_h + SB_DATE_GAP);
    uih::show(g.sb_line, music);
    uih::show(g.sb_spec, music);
}

/// The line: `dim` in the quieter colour and `bright` after it — the artist and the title, or
/// the station and what it is playing — or `fallback` when both are empty.
///
/// The two colours are one label with LVGL's recolour markup, because the circular scroll
/// belongs to a label. The markup has no escape for its own '#' (lv_text_is_cmd's escape
/// branch can never be taken), so a name with a '#' in it is shown in one colour instead of
/// being cut up by a command it never meant.
void sb_set_line(const char* dim, const char* bright, const char* fallback) {
    const auto& t = m3::theme();
    char txt[sizeof(g.sb_line_txt)];
    bool recolor = false;
    if (dim[0] && bright[0]) {
        recolor = !strchr(dim, '#') && !strchr(bright, '#');
        if (recolor) snprintf(txt, sizeof(txt), "#%06lX %s · #%s",
                              (unsigned long)(t.c.on_surface_var & 0xFFFFFF), dim, bright);
        else         snprintf(txt, sizeof(txt), "%s · %s", dim, bright);
    } else {
        snprintf(txt, sizeof(txt), "%s", dim[0] ? dim : bright[0] ? bright : fallback);
    }
    if (strcmp(txt, g.sb_line_txt) == 0) return;
    memcpy(g.sb_line_txt, txt, sizeof(txt));

    const lv_font_t* font = t.t.headline;
    lv_point_t sz;
    lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX,
                     recolor ? LV_TEXT_FLAG_RECOLOR : LV_TEXT_FLAG_NONE);
    const int32_t loop = sz.x + lv_font_get_glyph_width(font, ' ', ' ') * LV_LABEL_WAIT_CHAR_COUNT;
    lv_obj_set_style_anim_duration(g.sb_line, (uint32_t)loop * 1000u / SB_SCROLL_PX_S, 0);
    // LVGL keeps a running scroll's place across a new text; a new track starts from its start.
    lv_anim_delete(g.sb_line, nullptr);
    lv_label_set_recolor(g.sb_line, recolor);
    lv_label_set_text(g.sb_line, txt);
}

void build_standby(lv_obj_t* scr) {
    const auto& t = m3::theme();
    g.sb = m3w::surface(scr, 0, 0, uig::SCR_W, uig::SCR_H, t.c.surface_lowest, 0);
    lv_obj_add_flag(g.sb, LV_OBJ_FLAG_CLICKABLE);         // a tap anywhere wakes it
    lv_obj_add_event_cb(g.sb, sb_click_cb, LV_EVENT_CLICKED, nullptr);

    // The clock as the design sets it: hours in primary, the colon in outline, minutes in
    // on_surface. Three labels in a centred row, because a label has one colour.
    const int32_t clock_h = lv_font_get_line_height(t.t.clock);
    g.sb_clock = m3w::box(g.sb, 0, SB_TOP, uig::SCR_W, clock_h);
    lv_obj_set_flex_flow(g.sb_clock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g.sb_clock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(g.sb_clock, 4, 0);
    g.sb_hh = m3w::text(g.sb_clock, t.t.clock, t.c.primary, "--");
    m3w::text(g.sb_clock, t.t.clock, t.c.outline, ":");
    g.sb_mm = m3w::text(g.sb_clock, t.t.clock, t.c.on_surface, "--");

    g.sb_date = m3w::text(g.sb, t.t.title_m, t.c.on_surface_var, "");

    // what is playing: no card around it, centred while it fits, a running line when not
    const int32_t line_y = SB_TOP + clock_h + SB_DATE_GAP + lv_font_get_line_height(t.t.title_m)
                         + SB_LINE_GAP;
    g.sb_line = m3w::text(g.sb, t.t.headline, t.c.on_surface, "");
    lv_obj_set_pos(g.sb_line, SB_WIN_X, line_y);
    lv_obj_set_size(g.sb_line, SB_WIN_W, lv_font_get_line_height(t.t.headline));
    lv_obj_set_style_text_align(g.sb_line, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(g.sb_line, LV_LABEL_LONG_SCROLL_CIRCULAR);

    g.sb_spec = uih::spectrum(g.sb, SB_SPEC_X, SB_SPEC_BASE, SB_SPEC_H, SB_SPEC);

    g.sb_music = 0;
    sb_layout(false);
    uih::show(g.sb, false);
}

void refresh_standby(UiCtx& c) {
    const auto& s = c.s;
    // "HH:MM" or "--:--" → the two halves
    char hh[4] = "--", mm[4] = "--";
    if (strlen(s.clock) >= 5 && s.clock[2] == ':') {
        hh[0] = s.clock[0]; hh[1] = s.clock[1]; hh[2] = '\0';
        mm[0] = s.clock[3]; mm[1] = s.clock[4]; mm[2] = '\0';
    }
    uih::set_text(g.sb_hh, g.hh_txt, sizeof(g.hh_txt), hh);
    uih::set_text(g.sb_mm, g.mm_txt, sizeof(g.mm_txt), mm);
    uih::set_text(g.sb_date, g.date_txt, sizeof(g.date_txt), s.date);

    // The module decides when music is playing, with a few seconds' grace, and the backlight
    // follows the same flag — the face and its brightness change together.
    const int8_t music = s.sb_music ? 1 : 0;
    if (music != g.sb_music) {
        g.sb_music = music;
        sb_layout(music != 0);
    }
    if (!music) return;

    // The play_* fields belong to whichever source has a track; with AUX selected they still
    // hold the card's last one, so AUX says only what it is.
    const PlayerFace face = ui_face(s);
    const char* name = tr(ui_src_def(ui_src(s)).name);     // "" for the output off and the tone
    if (face == PlayerFace::RADIO)      sb_set_line(s.radio_name, s.radio_now, name);
    else if (face == PlayerFace::TRACK) sb_set_line(s.play_artist, s.play_title, name);
    else                                sb_set_line("", "", name);

    float db[audio_core::SPECTRUM_BANDS];
    uih::spectrum_set(g.sb_spec, audio_core::spectrum_bus().read(db) ? db : nullptr);
}

// ── pairing ────────────────────────────────────────────────────────────────────────────────
//
// A phone asks to pair; the satellite asks the panel (link protocol 1.8, host bit 7). The
// question can come while any page is up or while the screen sleeps, so it lives here, on
// top of everything, and wakes the screen. The code is the point: the phone shows the same
// six digits, and matching them is what tells the panel's owner that the phone asking is the
// one in their hand. The satellite gives up after 25 s on its own, and the bt module closes a
// request whose result was lost on the wire (bt_module.cpp), so nothing here times out.

constexpr uint32_t PAIR_RESULT_MS = 2500;
constexpr uint8_t  KIND_NUMERIC = 0, KIND_CONSENT = 1, KIND_PASSKEY = 2;   // sat::PAIR_*

void pair_buttons(bool yes, bool no) {
    m3w::show(g.pair.yes.obj, yes);
    m3w::show(g.pair.no.obj, no);
}

void pair_yes_cb(lv_event_t*) {
    bt_settings::request_pair_reply(g.pair_seq, true);
    lv_label_set_text(g.pair.body, tr(S::pair_confirm_phone));
    pair_buttons(false, false);
}

void pair_no_cb(lv_event_t*) {
    if (g.pair_kind == KIND_PASSKEY) {       // nothing to refuse: the phone decides this one
        m3w::dialog_hide(g.pair);
        g.pair_open = false;
        return;
    }
    bt_settings::request_pair_reply(g.pair_seq, false);
    lv_label_set_text(g.pair.body, tr(S::pair_rejecting));
    pair_buttons(false, false);
}

void build_pairing(lv_obj_t* scr) {
    const auto& t = m3::theme();
    g.pair = m3w::dialog(scr, 560, 300);
    g.pair_code = m3w::text(g.pair.card, t.t.display, t.c.primary, "");
    lv_obj_set_pos(g.pair_code, 0, 104);
    lv_obj_add_event_cb(g.pair.yes.obj, pair_yes_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g.pair.no.obj, pair_no_cb, LV_EVENT_CLICKED, nullptr);
}

void show_pair_request(const bt_settings::Pairing& p) {
    g.pair_seq  = p.seq;
    g.pair_kind = p.kind;
    snprintf(g.pair_name, sizeof(g.pair_name), "%s", p.name[0] ? p.name : tr(S::pair_phone));
    char body[192], code[12];
    snprintf(code, sizeof(code), "%03lu %03lu", (unsigned long)(p.passkey / 1000),
             (unsigned long)(p.passkey % 1000));
    switch (p.kind) {
    case KIND_NUMERIC:
        i18n::fmt(body, sizeof(body), tr(S::pair_numeric), {{"name", g.pair_name}});
        m3w::dialog_show(g.pair, tr(S::pair_title), body, tr(S::pair_connect), tr(S::pair_reject));
        break;
    case KIND_CONSENT:
        i18n::fmt(body, sizeof(body), tr(S::pair_consent), {{"name", g.pair_name}});
        m3w::dialog_show(g.pair, tr(S::pair_title), body, tr(S::pair_connect), tr(S::pair_reject));
        code[0] = '\0';
        break;
    default:
        i18n::fmt(body, sizeof(body), tr(S::pair_passkey), {{"name", g.pair_name}});
        m3w::dialog_show(g.pair, tr(S::pair_title), body, "", tr(S::pair_hide));
        pair_buttons(false, true);
        break;
    }
    lv_label_set_text(g.pair_code, code);
    g.pair_open = true;
    g.pair_done = false;
    g.pair_there = false;
}

void show_pair_result(uint8_t status) {
    char title[64];
    switch (status) {
    case 0:  i18n::fmt(title, sizeof(title), tr(S::pair_ok), {{"name", g.pair_name}}); break;
    case 1:  i18n::fmt(title, sizeof(title), tr(S::pair_rejected), {}); break;
    case 2:  i18n::fmt(title, sizeof(title), tr(S::pair_timeout), {}); break;
    default: i18n::fmt(title, sizeof(title), tr(S::pair_failed), {}); break;
    }
    lv_label_set_text(g.pair.title, title);
    lv_label_set_text(g.pair.body, "");
    lv_label_set_text(g.pair_code, "");
    pair_buttons(false, false);
    g.pair_done = true;
    g.pair_done_at = lv_tick_get();
}

void refresh_pairing(UiCtx& c) {
    if (!g.pair.scrim) return;
    bt_settings::Pairing p;
    bt_settings::pairing(p);
    if (p.active && p.seq != g.pair_seq) {
        show_pair_request(p);
        if (g.standby && c.mod) c.mod->lvgl_post(UiLvglModule::Req::WAKE, 0);
        return;
    }
    // Answered in a web tab while this dialog waited: the panel's own
    // answer would be ignored now (bt_settings, first answer wins), so the buttons go and the
    // dialog says where it was answered, until the result comes.
    if (g.pair_open && !g.pair_done && !g.pair_there && p.active && p.seq == g.pair_seq &&
        p.by == bt_settings::Source::WEB) {
        lv_label_set_text(g.pair.body, p.accepted ? tr(S::pair_accepted_web) : tr(S::pair_rejected_web));
        pair_buttons(false, false);
        g.pair_there = true;
    }
    if (g.pair_open && !g.pair_done && !p.active && p.seq == g.pair_seq && p.result != 0xFF) {
        show_pair_result(p.result);
    }
    if (g.pair_done && lv_tick_elaps(g.pair_done_at) > PAIR_RESULT_MS) {
        m3w::dialog_hide(g.pair);
        g.pair_open = false;
        g.pair_done = false;
    }
}

// ── a minute alone ─────────────────────────────────────────────────────────────────────────

/// Something on the glass waits for an answer: a keyboard, or a dialog other than the pick list —
/// the pairing dialog, «Забути …?». A minute alone throws away neither half a password nor a
/// question, as a rebuild does not (rebuild_look); the pick list it answers «Скасувати».
bool waiting_for_answer() { return m3w::modal_shown(lv_screen_active(), ui_pick_scrim()); }

/// Back to the bare Player after idle_home::AFTER_MS alone (idle_home.h).
void refresh_idle(const UiCtx& c) {
    // The standby's idle time, or less while a press LVGL has seen has not reached it yet: that
    // goes round the main loop first, and the tap that closes a keyboard must not pass for a
    // minute alone. The knob's input comes that way too, but the knob ends nothing that holds a
    // page.
    const uint32_t idle = LV_MIN(c.s.idle_ms, lv_display_get_inactive_time(nullptr));
    const bool over = g.drawer_open || ui_sound_is_open() || ui_pick_shown();
    const bool test = c.s.source == modesp::gen::AUDIO_SOURCE_TONE || c.s.port_meas;
    if (!s_idle.pass(idle, g.standby, PAGES[g.cur].def->id, over,
                     [test] { return test || waiting_for_answer(); }))
        return;
    set_drawer(false);
    ui_pick_hide();                             // as its «Скасувати»
    ui_sound_close();
    select_page(PAGE_HOME);
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════════════════════════

void ui_shell_show_player() { select_page(PAGE_HOME); }
void ui_shell_show_radio()  { select_page(PAGE_RADIO); }

void ui_shell_build(lv_obj_t* scr, UiCtx& c) {
    const auto& t = m3::theme();
    g = Shell{};
    g.ctx = &c;
    // The self-drawing rows keep their state in fixed tables; a rebuild starts them over.
    uih::spectrum_reset();
    m3w::wave_reset();

    lv_obj_set_style_bg_color(scr, m3::col(t.c.surface), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, m3::col(t.c.on_surface), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ── content: every page is a child of this, all the same size ──
    g.stack = m3w::box(scr, uig::CONT_X, uig::CONT_Y, uig::CONT_W, uig::CONT_H);
    c.stack = g.stack;
    for (int i = 0; i < N_PAGES; i++) {
        g.page[i] = PAGES[i].def->build(g.stack, c);
        uih::show(g.page[i], false);
    }

    // ── header: ≡ or ←, the title, Wi-Fi and the clock ──
    // ≡ and ← are a finger's 64 px (ui_m3_widgets.h), centred where the 48-px button was, so
    // the header's box is 8 px taller each way and starts at the edge strip (a press reaches a
    // button only inside its parent's box). The title stays where it was on the screen.
    constexpr int32_t NAV = 64;
    constexpr int32_t HX = uig::EDGE_W, HY = uig::HEAD_Y - (NAV - uig::HEAD_H) / 2;
    lv_obj_t* head = m3w::box(scr, HX, HY, uig::CONT_X + uig::CONT_W - HX, NAV);
    g.nav = m3w::icon_button(head, 0, 0, NAV, UI_SYM_MENU, t.t.icon_m);
    lv_obj_add_event_cb(g.nav.obj, nav_cb, LV_EVENT_CLICKED, nullptr);
    g.title = m3w::text(head, t.t.headline, t.c.on_surface, "");
    lv_obj_set_pos(g.title, uig::CONT_X + 60 - HX, uig::HEAD_Y + 5 - HY);
    // Wi-Fi and the clock share one right-aligned row, so the icon follows the clock as the
    // digits change width; a one-off align_to against "--:--" left them overlapping.
    lv_obj_t* right = m3w::box(head, 0, 0, 240, NAV);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 8, 0);
    g.wifi_ic = m3w::icon(right, t.t.icon_s, t.c.on_surface_var, UI_SYM_WIFI);
    g.clock = m3w::text(right, t.t.body_l, t.c.on_surface_var, "--:--");

    // ── the left edge: a press there opens the drawer, so a swipe from off-screen does ──
    g.edge = m3w::box(scr, 0, 0, uig::EDGE_W, uig::SCR_H);
    lv_obj_add_flag(g.edge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g.edge, edge_cb, LV_EVENT_PRESSED, nullptr);

    // ── overlays, in stacking order: the volume under the drawer, standby over everything ──
    build_volume(scr);
    build_drawer(scr);
    ui_sound_build(scr, c);   // the «Звук» sheet over the Player (sound_sheet.cpp)
    ui_pick_build(scr);       // the list a choice is made from, over the sheet (ui_pick.cpp)
    build_undo(scr);          // raised over the sheet when it shows (ui_shell_undo)
    build_standby(scr);
    build_pairing(scr);     // over standby too: a phone asking wakes the screen to ask

    select_page(PAGE_HOME);
    ESP_LOGI(TAG, "built: %d page(s), scheme %s", N_PAGES, t.name);
}

void ui_shell_refresh(UiCtx& c) {
    if (g.undo_at && lv_tick_elaps(g.undo_at) > touch::UNDO_MS) undo_hide();
    refresh_pairing(c);
    refresh_idle(c);          // first, so the page painted below is the one it leaves on the glass
    if (g.standby) {
        refresh_standby(c);
        return;
    }

    uih::set_text(g.clock, g.clock_txt, sizeof(g.clock_txt), c.s.clock);
    if (!g.wifi_init || c.s.wifi_ok != g.wifi_last) {
        g.wifi_init = true;
        g.wifi_last = c.s.wifi_ok;
        const auto& t = m3::theme();
        lv_obj_set_style_text_color(g.wifi_ic, m3::col(c.s.wifi_ok ? t.c.on_surface_var : t.c.outline_var), 0);
    }

    refresh_volume(c.s);
    ui_sound_refresh(c);

    if (PAGES[g.cur].def->refresh) PAGES[g.cur].def->refresh(g.page[g.cur], c);
}

void ui_shell_undo(int key, const char* text, void (*undo)(int32_t, void*), int32_t value, void* arg,
                   const char* action) {
    if (!g.undo_bar) return;
    if (g.undo_btn.label) lv_label_set_text(g.undo_btn.label, action ? action : tr(S::undo_action));
    g.undo_key = key;
    g.undo_fn  = undo;
    g.undo_val = value;
    g.undo_arg = arg;
    g.undo_at  = lv_tick_get() | 1;
    lv_label_set_text(g.undo_lbl, text);
    uih::show(g.undo_bar, true);
    // Over the «Звук» sheet, which raises itself as it opens. Standby and the pairing dialog
    // raise themselves too, and neither leaves a finger anything to move meanwhile.
    lv_obj_move_foreground(g.undo_bar);
}

void ui_shell_undo_drop(int key) {
    if (g.undo_at && g.undo_key == key) undo_hide();
}

bool ui_shell_go(const char* id) {
    for (int i = 0; i < N_PAGES; i++) {
        if (strcmp(PAGES[i].def->id, id) == 0) { select_page(i); return true; }
    }
    return false;
}

const char* ui_shell_current() { return PAGES[g.cur].def->id; }

const char* ui_shell_from() { return PAGES[g.from].def->id; }

const char* ui_shell_parent() { return g.parent >= 0 ? PAGES[g.parent].def->id : nullptr; }

bool ui_shell_modal_open() { return m3w::modal_shown(lv_screen_active()); }

const char* ui_shell_page_of(const lv_obj_t* o) {
    for (; o; o = lv_obj_get_parent(o)) {
        for (int i = 0; i < N_PAGES; i++) if (g.page[i] == o) return PAGES[i].def->id;
    }
    return "shell";
}

void ui_shell_set_standby(bool on) {
    if (g.standby == on) return;
    g.standby = on;
    if (on) { set_drawer(false); vol_hide(); ui_pick_hide(); ui_sound_close(); }
    uih::show(g.sb, on);
    if (on) lv_obj_move_foreground(g.sb);
}

void ui_shell_nav() {
    const PageDef& p = PAGES[g.cur];
    if (p.kind != Kind::LIST) { set_drawer(true); return; }
    // A page with a hierarchy of its own climbs it first, and is left from its top.
    if (p.def->back && g.ctx && p.def->back(*g.ctx)) return;
    if (g.parent >= 0) {
        const int to = g.parent;
        g.parent = -1;
        select_page(to, true);
        return;
    }
    select_page(g.from);
}

void ui_shell_show_volume() {
    if (g.standby || !g.vol) return;
    if (g.ctx && g.ctx->s.volume >= 0) m3w::slider_set(g.vol_sl, g.ctx->s.volume);
    vol_show();
}

bool ui_shell_peek(const char* what) {
    static bool     peeked_standby = false;
    static bool     peeked_pair = false;
    static uint32_t real_pair_seq = 0;
    if (!what || !*what) {
        set_drawer(false);
        ui_pick_hide();
        ui_sound_close();
        if (peeked_standby) { peeked_standby = false; ui_shell_set_standby(false); }
        if (peeked_pair) {
            peeked_pair = false;
            m3w::dialog_hide(g.pair);
            g.pair_open = false;
            g.pair_seq = real_pair_seq;
        }
        ui_bt_preview(false);
        return true;
    }
    if (strcmp(what, "drawer") == 0)  { set_drawer(true); return true; }
    if (strcmp(what, "sound") == 0)   { ui_sound_open(); return true; }
    if (strcmp(what, "pick") == 0)    { ui_sound_pick_show(audio_core::eq_store().preset(), nullptr); return true; }   // a look only
    if (strcmp(what, "standby") == 0) { peeked_standby = !g.standby; ui_shell_set_standby(true); return true; }
    // The pairing dialog and the 1.8 Bluetooth page, on made-up data, so both can be looked
    // at before a satellite that sends the real thing exists. Nothing is sent anywhere: the
    // dialog is drawn from a local request, and the page's view is a copy the bt module
    // never reads.
    if (strcmp(what, "pair") == 0 && g.pair.scrim) {
        if (!peeked_pair) real_pair_seq = g.pair_seq;
        peeked_pair = true;
        bt_settings::Pairing p;
        p.seq = 0xFFFFFFFFu;
        p.active = true;
        p.kind = KIND_NUMERIC;
        p.passkey = 48213;
        snprintf(p.name, sizeof(p.name), "Pixel 7");
        show_pair_request(p);
        return true;
    }
    if (strcmp(what, "bt18") == 0) { ui_bt_preview(true); return true; }
    return false;
}

void ui_shell_encoder(int32_t delta, uint8_t event, UiCtx& c) {
    if (g.standby) {
        // Asleep, any input is a wake and nothing else — the first turn of the knob must not
        // also change the volume of a system the user cannot see.
        if ((delta || event) && c.mod) c.mod->lvgl_post(UiLvglModule::Req::WAKE, 0);
        return;
    }
    // The same on every page and over every sheet, list and dialog: those are touched.
    if (event == 2) {                       // hold: the drawer, open or closed — not over a dialog
        if (!ui_pick_shown() && !ui_sound_is_open() && !ui_shell_modal_open()) set_drawer(!g.drawer_open);
        return;
    }
    if (event == 1) {                       // press: the Player's play button (a station's mute), anywhere
        ui_now_press();
        return;
    }
    if (!delta || !c.mod || c.s.volume < 0) return;
    // From the level asked for last while the snapshot may not have it yet, or a brisk turn
    // posts one level twice and loses the detent; after that from the snapshot, which is where a
    // level set on the web or by MQTT shows.
    const bool fresh = g.vol_posted >= 0 && lv_tick_elaps(g.vol_posted_at) < VOL_HOLD_MS;
    int32_t v = (fresh ? g.vol_posted : c.s.volume) + delta * 2;
    if (v < 0) v = 0; else if (v > 100) v = 100;
    // A turn is wanting sound at that level: a mute set on the web ends with it, or the knob
    // would turn a silent panel up with nothing to hear.
    if (c.s.mute) c.mod->lvgl_post(UiLvglModule::Req::MUTE, 0);
    g.vol_posted = v;
    g.vol_posted_at = lv_tick_get();
    c.mod->lvgl_post(UiLvglModule::Req::VOLUME, v);
}
