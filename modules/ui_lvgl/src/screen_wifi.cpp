/**
 * @file screen_wifi.cpp
 * @brief "Wi-Fi" — the network the panel is on, the networks it can hear, and joining one.
 *
 * Opened from Система → Wi-Fi, so ← goes back there. Three parts, top to bottom:
 *
 *   the card     the network it is on and its address — or the access point it fell back to,
 *                or the one it is trying right now; "Забути" when a network is stored
 *   the list     the networks nearby, strongest first, one row per name, looked for each time
 *                the page opens (a page that waits to be told to look looks broken)
 *   the sheet    a protected network's password: a field and the keyboard over the list
 *
 * Joining never costs the network that works. WiFiService::try_connect() keeps the old pair
 * until the new one has an address and puts it back if not, so a mistyped password is a
 * message on this page and not a panel stranded in its own access point.
 *
 * The scan runs on a task of its own (net_scan) because it takes seconds over the C6's RPC.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "net_scan.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include <cstdio>
#include <cstring>

namespace {

using i18n::S;
using i18n::tr;

// ── geometry, in the 752x384 content column ──
constexpr int32_t PAGE_W       = uig::CONT_W;
// «Забути», the rescan, the eye and «Підключити» a finger's 64 px
constexpr int32_t CARD_H  = 72;
constexpr int32_t BAR_Y   = CARD_H + 8;
constexpr int32_t BAR_H   = 64;
constexpr int32_t BTN     = 64;
constexpr int32_t LIST_TOP  = BAR_Y + BAR_H + 4;
constexpr int32_t LIST_HT  = uig::CONT_H - LIST_TOP;
constexpr int32_t FORGET_W = 140;
// the sheet
constexpr int32_t FIELD_Y = 40;
constexpr int32_t FIELD_H = 64;
constexpr int32_t GO_W    = 176;
constexpr int32_t EYE_X   = PAGE_W - GO_W - 8 - BTN - 8;
constexpr int32_t FIELD_W = EYE_X - 8;
constexpr int32_t KB_Y    = FIELD_Y + FIELD_H + 12;

constexpr uint32_t TOAST_MS = 5000;

struct Page {
    UiCtx*      ctx = nullptr;
    lv_obj_t*   root = nullptr;
    lv_obj_t*   card_ic = nullptr;
    lv_obj_t*   card_title = nullptr;
    lv_obj_t*   card_sub = nullptr;
    char        card_title_txt[80] = {};
    char        card_sub_txt[96] = {};
    char        card_ic_txt[8] = {};
    m3w::Button forget;
    m3w::Button rescan;
    lv_obj_t*   scan_txt = nullptr;
    char        scan_txt_buf[32] = {};
    lv_obj_t*   empty = nullptr;
    char        empty_txt[40] = {};
    m3w::VList  list;
    // the sheet
    lv_obj_t*   sheet = nullptr;
    lv_obj_t*   sheet_title = nullptr;
    lv_obj_t*   field = nullptr;
    lv_obj_t*   kb = nullptr;
    m3w::Button eye, go;
    bool        hidden_pw = true;
    char        pick_ssid[33] = {};
    // the rest
    m3w::Dialog dlg;
    lv_obj_t*   toast = nullptr;
    uint32_t    toast_at = 0;
    bool        toast_on = false;
    int         n = 0;
    uint32_t    scan_seq = UINT32_MAX;
    char        last_try[12] = {};
    bool        try_known = false;      ///< last_try holds a value seen on this page
    char        on_ssid[33] = {};       ///< the network marked in the list
    bool        trying = false;
};

Page g;
/// PSRAM, kept across rebuilds of the object tree like the DLNA page's views.
net_scan::Net* s_nets = nullptr;

// ── text ──

bool supported(uint8_t auth) {
    switch (auth) {
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return true;
    default:
        return false;
    }
}

/// A row's second line. The unsupported kinds are whole strings of the catalog, not the web's
/// "{auth} — …" filled in here: a buffer for that would be internal RAM for a list line.
const char* auth_text(uint8_t auth) {
    switch (auth) {
    case WIFI_AUTH_OPEN:          return tr(S::wifi_open);
    case WIFI_AUTH_WPA2_PSK:      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    case WIFI_AUTH_WEP:           return tr(S::wifi_no_wep);
    case WIFI_AUTH_WPA_PSK:       return tr(S::wifi_no_wpa);
    case WIFI_AUTH_OWE:           return tr(S::wifi_no_owe);
    default:                      return tr(S::wifi_no_enterprise);
    }
}

/// What the last refusal means to someone holding the panel.
const char* reason_text(int32_t r) {
    switch (r) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return tr(S::wifi_why_pass);
    case WIFI_REASON_NO_AP_FOUND:
        return tr(S::wifi_why_none);
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return tr(S::wifi_why_security);
    case 0:
        return tr(S::wifi_why_silent);
    default:
        return tr(S::wifi_why_refused);
    }
}

const char* signal_icon(int8_t rssi) {
    return rssi >= -55 ? UI_SYM_WIFI_4 : rssi >= -67 ? UI_SYM_WIFI_3
         : rssi >= -75 ? UI_SYM_WIFI_2 : UI_SYM_WIFI_1;
}

void toast(const char* txt) {
    m3w::toast_show(g.toast, txt);
    g.toast_at = lv_tick_get();
    g.toast_on = true;
}

// ── the list ──

bool is_current(const net_scan::Net& n) {
    return g.ctx && g.ctx->s.wifi_ok && strcmp(n.ssid, g.ctx->s.wifi_ssid) == 0;
}

void fill(int idx, m3w::VItem& it, void*) {
    if (!s_nets || idx >= g.n) return;
    const auto& n = s_nets[idx];
    const bool cur = is_current(n);
    it.lead    = signal_icon(n.rssi);
    it.title   = n.ssid;
    it.sub     = cur ? tr(S::wifi_connected) : auth_text(n.auth);
    it.trail   = n.auth != WIFI_AUTH_OPEN ? UI_SYM_LOCK : nullptr;
    it.on      = cur;
    it.enabled = supported(n.auth) && !g.trying;
}

void open_sheet(const char* ssid) {
    strncpy(g.pick_ssid, ssid, sizeof(g.pick_ssid) - 1);
    g.pick_ssid[sizeof(g.pick_ssid) - 1] = '\0';
    char buf[80];
    i18n::fmt(buf, sizeof(buf), tr(S::wifi_pass_q), {{"name", ssid}});
    lv_label_set_text(g.sheet_title, buf);
    lv_textarea_set_text(g.field, "");
    g.hidden_pw = true;
    lv_textarea_set_password_mode(g.field, true);
    if (g.eye.icon) lv_label_set_text(g.eye.icon, UI_SYM_VISIBLE);
    m3w::keyboard_reset(g.kb);
    m3w::show(g.sheet, true);
    lv_obj_move_foreground(g.sheet);
}

void close_sheet() {
    if (g.field) lv_textarea_set_text(g.field, "");   // the password does not outlive the sheet
    m3w::show(g.sheet, false);
}

void connect(const char* ssid, const char* pass) {
    if (!g.ctx || !g.ctx->mod) return;
    if (!g.ctx->mod->lvgl_wifi_connect(ssid, pass)) { toast(tr(S::wifi_busy)); return; }
    char buf[80];
    i18n::fmt(buf, sizeof(buf), tr(S::wifi_joining), {{"name", ssid}});
    toast(buf);
}

void pick(int idx, void*) {
    if (!s_nets || idx >= g.n || g.trying) return;
    const auto& n = s_nets[idx];
    if (!supported(n.auth) || is_current(n)) return;
    if (n.auth == WIFI_AUTH_OPEN) connect(n.ssid, "");
    else                          open_sheet(n.ssid);
}

// ── touch ──

void rescan_cb(lv_event_t*) {
    if (!net_scan::start()) toast(tr(g.trying ? S::wifi_wait_join : S::wifi_scan_busy));
}

void go_cb(lv_event_t*) {
    const char* pw = lv_textarea_get_text(g.field);
    // WPA2 wants 8..63 characters; saying so here beats a refusal twenty seconds later.
    if (!pw || strlen(pw) < 8) { toast(tr(S::wifi_pass_short)); return; }
    connect(g.pick_ssid, pw);
    close_sheet();
}

void eye_cb(lv_event_t*) {
    g.hidden_pw = !g.hidden_pw;
    lv_textarea_set_password_mode(g.field, g.hidden_pw);
    if (g.eye.icon) lv_label_set_text(g.eye.icon, g.hidden_pw ? UI_SYM_VISIBLE : UI_SYM_HIDDEN);
}

void forget_cb(lv_event_t*) {
    if (!g.ctx) return;
    char title[64];
    i18n::fmt(title, sizeof(title), tr(S::wifi_forget_q),
              {{"name", g.ctx->s.wifi_ssid[0] ? g.ctx->s.wifi_ssid : tr(S::wifi_this_network)}});
    m3w::dialog_show(g.dlg, title, tr(S::wifi_forget_panel), tr(S::wifi_forget), tr(S::common_cancel));
}
void dlg_yes_cb(lv_event_t*) {
    m3w::dialog_hide(g.dlg);
    if (g.ctx && g.ctx->mod) g.ctx->mod->lvgl_post(UiLvglModule::Req::WIFI_FORGET, 0);
}
void dlg_no_cb(lv_event_t*) { m3w::dialog_hide(g.dlg); }

// ── build ──

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    g = Page{};
    g.ctx = &c;
    if (!s_nets) {
        s_nets = static_cast<net_scan::Net*>(heap_caps_calloc(net_scan::MAX, sizeof(net_scan::Net),
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    lv_obj_t* root = uih::box(parent, 0, 0, PAGE_W, uig::CONT_H);
    g.root = root;

    // the card
    lv_obj_t* card = m3w::surface(root, 0, 0, PAGE_W, CARD_H, t.c.surface_low, m3::shape::L);
    g.card_ic = m3w::icon(card, t.t.icon_m, t.c.on_surface_var, UI_SYM_WIFI);
    lv_obj_align(g.card_ic, LV_ALIGN_LEFT_MID, 20, 0);
    g.card_title = m3w::text(card, t.t.body_l, t.c.on_surface, "");
    lv_obj_set_pos(g.card_title, 60, 12);
    lv_obj_set_size(g.card_title, PAGE_W - 60 - FORGET_W - 28, lv_font_get_line_height(t.t.body_l));
    lv_label_set_long_mode(g.card_title, LV_LABEL_LONG_DOT);
    g.card_sub = m3w::text(card, t.t.label_s, t.c.on_surface_var, "");
    lv_obj_set_pos(g.card_sub, 60, 40);
    lv_obj_set_size(g.card_sub, PAGE_W - 60 - FORGET_W - 28, lv_font_get_line_height(t.t.label_s));
    lv_label_set_long_mode(g.card_sub, LV_LABEL_LONG_DOT);
    g.forget = m3w::tonal_button(card, PAGE_W - FORGET_W - 12, (CARD_H - BTN) / 2, FORGET_W, BTN,
                                 m3::shape::FULL, nullptr, nullptr, tr(S::wifi_forget));
    lv_obj_add_event_cb(g.forget.obj, forget_cb, LV_EVENT_CLICKED, nullptr);

    // the bar over the list
    lv_obj_t* cap = m3w::text(root, t.t.label_m, t.c.on_surface_var, tr(S::wifi_nearby));
    lv_obj_set_pos(cap, 12, BAR_Y + (BAR_H - lv_font_get_line_height(t.t.label_m)) / 2);
    g.scan_txt = m3w::text(root, t.t.label_s, t.c.on_surface_var, "");
    lv_obj_set_width(g.scan_txt, 260);
    lv_obj_set_style_text_align(g.scan_txt, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g.scan_txt, PAGE_W - BTN - 8 - 260, BAR_Y + (BAR_H - lv_font_get_line_height(t.t.label_s)) / 2);
    g.rescan = m3w::icon_button(root, PAGE_W - BTN, BAR_Y, BTN, UI_SYM_REFRESH, t.t.icon_m);
    lv_obj_add_event_cb(g.rescan.obj, rescan_cb, LV_EVENT_CLICKED, nullptr);

    g.list.build(root, 0, LIST_TOP, PAGE_W, LIST_HT, fill, pick, nullptr, nullptr);
    g.empty = m3w::text(root, t.t.body_l, t.c.on_surface_var, "");
    lv_obj_set_pos(g.empty, 16, LIST_TOP + 16);
    m3w::show(g.empty, false);

    // the sheet: opaque, over everything above
    g.sheet = m3w::surface(root, 0, 0, PAGE_W, uig::CONT_H, t.c.surface, 0);
    g.sheet_title = m3w::text(g.sheet, t.t.title_m, t.c.on_surface, "");
    lv_obj_set_pos(g.sheet_title, 4, 2);
    lv_obj_set_width(g.sheet_title, PAGE_W - 8);
    lv_label_set_long_mode(g.sheet_title, LV_LABEL_LONG_DOT);
    g.field = m3w::text_field(g.sheet, 0, FIELD_Y, FIELD_W, FIELD_H, tr(S::wifi_password), 63);
    g.eye = m3w::icon_button(g.sheet, EYE_X, FIELD_Y + (FIELD_H - BTN) / 2, BTN, UI_SYM_VISIBLE, t.t.icon_m);
    lv_obj_add_event_cb(g.eye.obj, eye_cb, LV_EVENT_CLICKED, nullptr);
    g.go = m3w::filled_button(g.sheet, PAGE_W - GO_W, FIELD_Y, GO_W, FIELD_H, m3::shape::FULL,
                              nullptr, nullptr, tr(S::wifi_join));
    lv_obj_add_event_cb(g.go.obj, go_cb, LV_EVENT_CLICKED, nullptr);
    g.kb = m3w::keyboard(g.sheet, 0, KB_Y, PAGE_W, uig::CONT_H - KB_Y, g.field);
    m3w::show(g.sheet, false);

    // on the screen, not the page: its shade covers the header and its ← too
    g.dlg = m3w::dialog(lv_obj_get_screen(root), 520, 230);
    lv_obj_add_event_cb(g.dlg.yes.obj, dlg_yes_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g.dlg.no.obj, dlg_no_cb, LV_EVENT_CLICKED, nullptr);
    g.toast = m3w::toast(root);
    if (!s_nets) lv_label_set_text(g.empty, tr(S::common_no_memory));
    return root;
}

// ── refresh ──

void paint_card(const UiLvglModule::Snapshot& s) {
    char title[80], sub[96];
    const char* ic = UI_SYM_WIFI;
    if (g.trying) {
        snprintf(title, sizeof(title), "%s", s.wifi_try_ssid);
        snprintf(sub, sizeof(sub), "%s", tr(S::wifi_connecting));
    } else if (s.wifi_ok) {
        snprintf(title, sizeof(title), "%s", s.wifi_ssid);
        snprintf(sub, sizeof(sub), "%s · %s", tr(S::wifi_connected), s.wifi_ip[0] ? s.wifi_ip : tr(S::wifi_no_ip));
    } else if (s.wifi_ap) {
        ic = UI_SYM_WIFI_OFF;
        i18n::fmt(title, sizeof(title), tr(S::wifi_ap_title), {{"name", s.wifi_ssid}});
        snprintf(sub, sizeof(sub), "%s · %s", s.wifi_ip[0] ? s.wifi_ip : "192.168.4.1",
                 tr(s.wifi_saved ? S::wifi_saved_silent : S::wifi_none_chosen));
    } else {
        ic = UI_SYM_WIFI_OFF;
        snprintf(title, sizeof(title), "%s", s.wifi_ssid[0] ? s.wifi_ssid : tr(S::wifi_not_connected));
        snprintf(sub, sizeof(sub), "%s", tr(S::wifi_retrying));
    }
    m3w::set_text(g.card_title, g.card_title_txt, sizeof(g.card_title_txt), title);
    m3w::set_text(g.card_sub, g.card_sub_txt, sizeof(g.card_sub_txt), sub);
    m3w::set_text(g.card_ic, g.card_ic_txt, sizeof(g.card_ic_txt), ic);
    m3w::show(g.forget.obj, s.wifi_saved && !g.trying);
}

void refresh(lv_obj_t*, UiCtx& c) {
    const auto& s = c.s;
    const bool trying = strcmp(s.wifi_try, "connecting") == 0;

    // A try that ended while this page was up says how it went. The value that was already
    // there when the page first looked is history, not news.
    if (strcmp(s.wifi_try, g.last_try) != 0) {
        if (g.try_known) {
            char buf[112];
            if (strcmp(s.wifi_try, "ok") == 0) {
                i18n::fmt(buf, sizeof(buf), tr(S::wifi_ok), {{"name", s.wifi_try_ssid}});
                toast(buf);
            } else if (strcmp(s.wifi_try, "fail") == 0) {
                i18n::fmt(buf, sizeof(buf), tr(S::wifi_fail), {{"name", s.wifi_try_ssid}, {"why", reason_text(s.wifi_reason)}});
                toast(buf);
            }
        }
        strncpy(g.last_try, s.wifi_try, sizeof(g.last_try) - 1);
    }
    g.try_known = true;

    bool refill = false;
    if (trying != g.trying) { g.trying = trying; refill = true; }
    const char* on = s.wifi_ok ? s.wifi_ssid : "";
    if (strcmp(on, g.on_ssid) != 0) {
        strncpy(g.on_ssid, on, sizeof(g.on_ssid) - 1);
        refill = true;
    }
    paint_card(s);

    // the scan
    const net_scan::State st = net_scan::state();
    const uint32_t seq = net_scan::seq();
    if (seq != g.scan_seq && st != net_scan::State::BUSY && s_nets) {
        g.scan_seq = seq;
        g.n = net_scan::copy(s_nets, net_scan::MAX);
        g.list.set_count(g.n);
        refill = false;
    }
    if (refill) g.list.refill();
    const char* stxt = st == net_scan::State::BUSY ? tr(S::wifi_scanning)
                     : st == net_scan::State::FAILED ? tr(S::wifi_scan_failed) : "";
    m3w::set_text(g.scan_txt, g.scan_txt_buf, sizeof(g.scan_txt_buf), stxt);
    m3w::button_enable(g.rescan, st != net_scan::State::BUSY && !trying);
    const bool none = g.n == 0 && st != net_scan::State::BUSY;
    if (s_nets) m3w::set_text(g.empty, g.empty_txt, sizeof(g.empty_txt),
                              st == net_scan::State::IDLE ? "" : tr(S::wifi_none));
    m3w::show(g.empty, none);

    if (g.toast_on && lv_tick_elaps(g.toast_at) > TOAST_MS) {
        g.toast_on = false;
        m3w::show(g.toast, false);
    }
}

bool sheet_open() { return g.sheet && !lv_obj_has_flag(g.sheet, LV_OBJ_FLAG_HIDDEN); }

void shown(lv_obj_t*, UiCtx&) {
    close_sheet();
    m3w::dialog_hide(g.dlg);
    net_scan::start();
}

/// ← closes the sheet or the question first, and leaves the page from the list.
bool back(UiCtx&) {
    if (sheet_open()) { close_sheet(); return true; }
    if (g.dlg.scrim && !lv_obj_has_flag(g.dlg.scrim, LV_OBJ_FLAG_HIDDEN)) { m3w::dialog_hide(g.dlg); return true; }
    return false;
}

}  // namespace

const UiScreenDef UI_SCREEN_WIFI = {"wifi", UI_SYM_WIFI, build, refresh, shown, back};
