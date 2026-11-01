/**
 * @file screen_set.cpp
 * @brief «Налаштування» — the few things set once, the state of the links,
 *        and the ways into the equalizer, the screen and the service readout.
 *
 * Four groups, each a card of rows:
 *
 *   Звук      Тембр → its page (the sounds, Бас, Високі, the loudness and the equalizer:
 *             sound_sheet.cpp), what plays on its second line;
 *             Колонки → its page (screen_speakers.cpp: the state and the balance), what has been
 *             done about them on its second line; Кросовер → its page (screen_xover.cpp), the
 *             scheme that plays on its second line — the web's order
 *   Зв'язок   Wi-Fi (network · address) and Bluetooth (phone · codec), each → its page
 *   Пристрій  Екран → its page (brightness, standby, scheme: screen_display.cpp); Мова → a dialog
 *             of the four languages; Сервіс → the diagnostics page, the firmware on its second line
 *   Служби    AirPlay and DLNA: announcement switches; a tap on AirPlay's row chooses its mode,
 *             which is on its second line and applies live
 *
 * Every row is a finger's 64 px, five to a column so the page stays whole: Звук and Зв'язок on
 * the left, Пристрій and Служби on the right, and «Режим AirPlay» folded into AirPlay's row —
 * eleven rows did not fit.
 *
 * Only rows backed by something real are here — a settings page whose switches do nothing is
 * worse than a shorter one. The two service switches existed (airplay.enabled, dlna.enabled) but
 * could only be changed from the web; they are here now. What left: the PEQ switch and the limiter
 * (the equalizer's page is where sound is shaped), the clock (it is in the header), and the C6
 * row, folded into the firmware's line, which is Сервіс's second line.
 *
 * Wi-Fi shows network and address but no RSSI: esp_hosted 3.0.7's host glue omits rssi from
 * sta_get_ap_info, so wifi.rssi is 0 by definition here — printing it would read as "no
 * signal" rather than "not reported". The Wi-Fi page shows the scan's figure instead.
 * */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "esp_app_desc.h"
#include "eq_store.h"
#include <cstdio>
#include <cstring>

namespace {

using i18n::S;
using i18n::tr;
using modesp::gen::AIRPLAY_MODE_AIRPLAY2;
using modesp::gen::AIRPLAY_MODE_WINDOWS;
using modesp::gen::AUDIO_SOURCE_AIRPLAY;

// ── geometry, all relative to the 752x384 content column ──
constexpr int32_t COL_GAP = 16;
constexpr int32_t COL_W   = (uig::CONT_W - COL_GAP) / 2;      // 368
constexpr int32_t COL2_X  = COL_W + COL_GAP;
// Ten rows in two columns, five to each: a finger's 64 px a row and a 24 px caption keep the page
// whole, without a scroll.
constexpr int32_t CAP_H   = 24;
constexpr int32_t RH      = 64;
constexpr int32_t INSET   = 2;                                // the rows' inset in their card
constexpr int32_t RW      = COL_W - 2 * INSET;
constexpr int32_t GAP     = 8;                                // between one group and the next
// body_l's 25 px line, then label_s's 21, 2 apart, in the middle of the row: their boxes must not
// cross, or GET /api/i18n/overflow names every row "overlap".
constexpr int32_t TITLE_Y = (RH - 25 - 2 - 21) / 2;
constexpr int32_t SUB_Y   = TITLE_Y + 25 + 2;

/// The height a group of `n` rows takes, caption included.
constexpr int32_t group_h(int n) { return CAP_H + n * RH + 2 * INSET; }
constexpr int32_t Y2 = group_h(3) + GAP;                      // the second group of a column
static_assert(Y2 + group_h(2) <= uig::CONT_H, "Sound and Links, Device and Services fit a column");

enum RowId { R_SOUND, R_SPK, R_XOVER, R_DISPLAY, R_LANG, R_SVC, R_WIFI, R_BT, R_AIRPLAY, R_DLNA, R_COUNT };
constexpr int N_SCHEME  = (int)m3::Scheme::COUNT;
/// The schemes by name, in m3::Scheme's order (ui_m3.cpp) — the web's names for them.
constexpr S SCHEME_NAMES[] = {S::sys_scheme_amber, S::sys_scheme_blue, S::sys_scheme_green, S::sys_scheme_red};
static_assert(sizeof(SCHEME_NAMES) / sizeof(SCHEME_NAMES[0]) == N_SCHEME, "a name per scheme");
constexpr int N_LANG    = (int)i18n::Lang::COUNT;

struct Page {
    UiCtx*      ctx = nullptr;
    lv_obj_t*   root = nullptr;
    lv_obj_t*   row[R_COUNT]  = {};
    lv_obj_t*   sub[R_COUNT]  = {};
    char        sub_txt[R_COUNT][96] = {};
    lv_obj_t*   ap_sw = nullptr;
    lv_obj_t*   dlna_sw = nullptr;
    int8_t      ap_on = -1;
    int8_t      dlna_on = -1;
};

Page g;

void post(UiLvglModule::Req kind, int32_t v) {
    if (g.ctx && g.ctx->mod) g.ctx->mod->lvgl_post(kind, v);
}

void ap_cb(lv_event_t*) { post(UiLvglModule::Req::AIRPLAY_ON, lv_obj_has_state(g.ap_sw, LV_STATE_CHECKED) ? 1 : 0); }
/// The mode chosen in its list: switched only when it is another one.
void ap_mode_chosen(int mode) {
    if (g.ctx && mode != g.ctx->s.ap_mode) post(UiLvglModule::Req::AIRPLAY_MODE, mode);
}
/// «Режим AirPlay» as a list of the two, the one on ticked (ui_pick.cpp), from a tap on AirPlay's
/// row — its switch announces it.
void ap_mode_pick() {
    if (!g.ctx || !(g.ctx->s.sources & (1 << AUDIO_SOURCE_AIRPLAY))) return;
    // a row per airplay.mode value, in its order: the row chosen is the mode
    static_assert(AIRPLAY_MODE_AIRPLAY2 == 0 && AIRPLAY_MODE_WINDOWS == 1, "the rows are the modes");
    const char* items[] = {"AirPlay 2", tr(S::sys_airplay_1)};
    const int on = g.ctx->s.ap_mode == AIRPLAY_MODE_WINDOWS ? AIRPLAY_MODE_WINDOWS : AIRPLAY_MODE_AIRPLAY2;
    ui_pick_show(tr(S::sys_airplay_mode), items, 2, on, ap_mode_chosen);
}
void ap_mode_cb(lv_event_t*) {
    ap_mode_pick();
}
void dlna_cb(lv_event_t*) { post(UiLvglModule::Req::DLNA_ON, lv_obj_has_state(g.dlna_sw, LV_STATE_CHECKED) ? 1 : 0); }
void svc_cb(lv_event_t*)     { ui_shell_go("svc"); }
void sound_cb(lv_event_t*)   { ui_shell_go("sound"); }
void spk_cb(lv_event_t*)     { ui_shell_go("speakers"); }
void xover_cb(lv_event_t*)   { ui_shell_go("xover"); }
void display_cb(lv_event_t*) { ui_shell_go("display"); }

// ── the language: ui_pick's list, as the presets' and the filter types' ──

/// Hide first (ui_pick does), then ask: the rebuild waits while a dialog is up.
void lang_chosen(int i) {
    if (i >= 0 && i < N_LANG && i != (int)i18n::lang()) post(UiLvglModule::Req::LANG, i);
}

void lang_open() {
    const char* names[N_LANG];
    for (int i = 0; i < N_LANG; i++) names[i] = i18n::lang_name((i18n::Lang)i);
    ui_pick_show(tr(S::page_language), names, N_LANG, (int)i18n::lang(), lang_chosen);
}

void lang_cb(lv_event_t*) { lang_open(); }
void wifi_cb(lv_event_t*) { ui_shell_go("wifi"); }
void bt_cb(lv_event_t*)   { ui_shell_go("bt"); }

// ── building ──

/// A card of `n` rows at (x, y) under a caption; returns the card.
lv_obj_t* group(lv_obj_t* root, int32_t x, int32_t y, const char* caption, int n) {
    const auto& t = m3::theme();
    lv_obj_t* cap = m3w::text(root, t.t.label_m, t.c.on_surface_var, caption);
    lv_obj_set_pos(cap, x + 12, y);
    return m3w::surface(root, x, y + CAP_H, COL_W, n * RH + 2 * INSET, t.c.surface_low, m3::shape::L);
}

/// One row of a card: icon, title, and the line under it; the caller adds the control.
lv_obj_t* row(lv_obj_t* card, int i, RowId id, const char* sym, const char* title) {
    const auto& t = m3::theme();
    lv_obj_t* r = m3w::box(card, INSET, INSET + i * RH, RW, RH);
    lv_obj_set_style_radius(r, m3::shape::M, 0);
    lv_obj_t* ic = m3w::icon(r, t.t.icon_m, t.c.on_surface_var, sym);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t* ti = m3w::text(r, t.t.body_l, t.c.on_surface, title);
    lv_obj_set_pos(ti, 54, TITLE_Y);
    lv_obj_t* s = m3w::text(r, t.t.label_s, t.c.on_surface_var, "");
    lv_obj_set_pos(s, 54, SUB_Y);
    lv_obj_set_size(s, RW - 54 - 12, lv_font_get_line_height(t.t.label_s));
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    g.row[id] = r;
    g.sub[id] = s;
    return r;
}

/// A row's second line is a value, not a label: narrow it so it leaves the control room.
void sub_width(RowId id, int32_t control_w) {
    const auto& t = m3::theme();
    lv_obj_set_size(g.sub[id], RW - 54 - control_w - 16, lv_font_get_line_height(t.t.label_s));
}

/// A row that opens a page: the whole row is the button, and a chevron says so.
void opens(lv_obj_t* r, RowId id, lv_event_cb_t cb) {
    const auto& t = m3::theme();
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* chev = m3w::icon(r, t.t.icon_m, t.c.on_surface_var, UI_SYM_CHEVRON);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -14, 0);
    sub_width(id, 40);
}

/// A row that is a switch; returns the switch. `whole_row`: a tap anywhere on the row toggles it;
/// otherwise the switch takes its own 84×64 and the row is the caller's.
lv_obj_t* toggle(lv_obj_t* r, RowId id, lv_event_cb_t cb, bool whole_row = true) {
    lv_obj_t* sw = m3w::switch_m3(r, RW - 52 - 14, (RH - 32) / 2, false);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    if (whole_row) m3w::switch_row(r, sw);
    sub_width(id, 70);
    return sw;
}

/// Put a service switch where the service is, or grey it out when the build has none.
void paint_switch(lv_obj_t* sw, int8_t& cache, bool built, bool on) {
    const int8_t v = built ? (on ? 1 : 0) : -2;
    if (v == cache) return;
    cache = v;
    if (v == 1) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else        lv_obj_remove_state(sw, LV_STATE_CHECKED);
    if (built) lv_obj_remove_state(sw, LV_STATE_DISABLED);
    else       lv_obj_add_state(sw, LV_STATE_DISABLED);
    m3w::show(sw, built);
}

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    g = Page{};
    g.ctx = &c;
    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);
    g.root = root;

    // ── Звук ──
    lv_obj_t* sound = group(root, 0, 0, tr(S::sys_sound), 3);
    lv_obj_t* r = row(sound, 0, R_SOUND, UI_SYM_EQ, tr(S::sys_timbre));
    opens(r, R_SOUND, sound_cb);
    r = row(sound, 1, R_SPK, UI_SYM_SPEAKER, tr(S::spk_title));
    opens(r, R_SPK, spk_cb);
    r = row(sound, 2, R_XOVER, UI_SYM_SPLIT, tr(S::xover_title));
    opens(r, R_XOVER, xover_cb);
    // the scheme that plays: stereo until the outputs' profile says another (screen_xover.cpp)
    lv_label_set_text(g.sub[R_XOVER], tr(S::xover_stereo));

    // ── Пристрій ──
    lv_obj_t* dev = group(root, COL2_X, 0, tr(S::sys_device), 3);
    r = row(dev, 0, R_DISPLAY, UI_SYM_BRIGHTNESS, tr(S::sys_screen));
    opens(r, R_DISPLAY, display_cb);
    r = row(dev, 1, R_LANG, UI_SYM_LANGUAGE, tr(S::page_language));
    opens(r, R_LANG, lang_cb);
    lv_label_set_text(g.sub[R_LANG], i18n::lang_name(i18n::lang()));
    r = row(dev, 2, R_SVC, UI_SYM_CPU, tr(S::page_service));
    opens(r, R_SVC, svc_cb);
    const esp_app_desc_t* app = esp_app_get_description();
    char buf[96];
    // "0.1.0 · IDF 5.5.5 · C6 3.0.7": the long form ("IDF v5.5.5 · C6 esp_hosted 3.0.7") was
    // cut in every language, Ukrainian too (276 px in a 250 px row).
    const char* idf = app->idf_ver[0] == 'v' ? app->idf_ver + 1 : app->idf_ver;
    snprintf(buf, sizeof(buf), "%s · IDF %s · C6 3.0.7", app->version, idf);
    lv_label_set_text(g.sub[R_SVC], buf);

    // ── Зв'язок ──
    lv_obj_t* link = group(root, 0, Y2, tr(S::sys_links), 2);
    r = row(link, 0, R_WIFI, UI_SYM_WIFI, tr(S::page_wifi));
    opens(r, R_WIFI, wifi_cb);
    r = row(link, 1, R_BT, UI_SYM_BLUETOOTH, tr(S::src_bt));
    opens(r, R_BT, bt_cb);

    // ── Служби ──
    lv_obj_t* svcs = group(root, COL2_X, Y2, tr(S::sys_services), 2);
    r = row(svcs, 0, R_AIRPLAY, UI_SYM_AIRPLAY, tr(S::src_airplay));
    g.ap_sw = toggle(r, R_AIRPLAY, ap_cb, false);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, ap_mode_cb, LV_EVENT_CLICKED, nullptr);
    r = row(svcs, 1, R_DLNA, UI_SYM_CAST, tr(S::src_dlna));
    g.dlna_sw = toggle(r, R_DLNA, dlna_cb);

    return root;
}

// ── refresh ──

void set_sub(RowId id, const char* txt) {
    uih::set_text(g.sub[id], g.sub_txt[id], sizeof(g.sub_txt[id]), txt);
}

void refresh(lv_obj_t*, UiCtx& c) {
    const auto& s = c.s;
    char buf[96];

    // what plays, or that the equalizer is off (the loudness and the timbre still are not)
    set_sub(R_SOUND, s.eq_enabled ? ui_eq_preset_name(audio_core::eq_store().preset()) : tr(S::sys_eq_off));
    // what has been done about the speakers, in their page's words (screen_speakers.cpp)
    set_sub(R_SPK, ui_spk_line(s, buf, sizeof(buf)));
    // the screen at a glance: its brightness and the scheme, which the page sets
    snprintf(buf, sizeof(buf), "%ld %% · %s", (long)s.backlight,
             s.theme >= 0 && s.theme < N_SCHEME ? tr(SCHEME_NAMES[s.theme]) : "");
    set_sub(R_DISPLAY, buf);

    if (strcmp(s.wifi_try, "connecting") == 0) i18n::fmt(buf, sizeof(buf), tr(S::wifi_joining), {{"name", s.wifi_try_ssid}});
    else if (s.wifi_ok) snprintf(buf, sizeof(buf), "%s · %s", s.wifi_ssid, s.wifi_ip[0] ? s.wifi_ip : "—");
    else if (s.wifi_ap) i18n::fmt(buf, sizeof(buf), tr(S::sys_wifi_own_ap), {{"name", s.wifi_ssid}});
    else                snprintf(buf, sizeof(buf), "%s", tr(S::sys_wifi_off));
    set_sub(R_WIFI, buf);

    // Bluetooth: the phone if there is one; if not, whether the satellite is there to take one.
    if (s.bt_connected)  snprintf(buf, sizeof(buf), "%s%s%s", s.bt_peer[0] ? s.bt_peer : tr(S::pair_phone),
                                  s.bt_codec[0] ? " · " : "", s.bt_codec);
    else if (s.bt_link)  snprintf(buf, sizeof(buf), "%s", tr(S::sys_bt_ready));
    else if (s.bt_seen)  snprintf(buf, sizeof(buf), "%s", tr(S::bt_err_no_satellite));
    else                 snprintf(buf, sizeof(buf), "%s", tr(S::sys_bt_none));
    set_sub(R_BT, buf);

    // AirPlay, only when this build has the receiver: the mode, what is happening to it while
    // something is (the receiver applies a new one live — its progress or failure, not the bare
    // selection), else the name it is announced by, or that it is not.
    const bool ap_built = (s.sources & (1 << AUDIO_SOURCE_AIRPLAY)) != 0;
    if (!ap_built) {
        snprintf(buf, sizeof(buf), "%s", tr(S::sys_not_built));
    } else {
        const char* mode = s.ap_mode == AIRPLAY_MODE_WINDOWS ? tr(S::sys_airplay_1) : "AirPlay 2";
        if (s.ap_switching)       snprintf(buf, sizeof(buf), "%s · %s", mode, tr(S::sys_ap_switching));
        else if (s.ap_mode_error) snprintf(buf, sizeof(buf), "%s · %s", mode, tr(S::sys_ap_switch_failed));
        else if (s.ap_enabled)    snprintf(buf, sizeof(buf), "%s · %s", mode, s.ap_name[0] ? s.ap_name : "…");
        else                      snprintf(buf, sizeof(buf), "%s · %s", mode, tr(S::sys_not_announced));
    }
    set_sub(R_AIRPLAY, buf);
    paint_switch(g.ap_sw, g.ap_on, ap_built, s.ap_enabled);

    if (!s.dlna_built)        snprintf(buf, sizeof(buf), "%s", tr(S::sys_not_built));
    else if (s.dlna_enabled)  i18n::fmt(buf, sizeof(buf), tr(S::manage_visible_as), {{"name", s.dlna_name[0] ? s.dlna_name : "…"}});
    else                      snprintf(buf, sizeof(buf), "%s", tr(S::sys_not_announced));
    set_sub(R_DLNA, buf);
    paint_switch(g.dlna_sw, g.dlna_on, s.dlna_built, s.dlna_enabled);
}

} // namespace

const UiScreenDef UI_SCREEN_SET = {"set", UI_SYM_SETTINGS, build, refresh, nullptr, nullptr};
