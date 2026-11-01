/**
 * @file screen_bt.cpp
 * @brief "Bluetooth" — the phone, the satellite's settings, and the phones it remembers.
 *
 * Opened from Система → Bluetooth, so ← goes back there. The satellite keeps every setting
 * and every bond (link protocol 1.8); this page shows what it last said and posts requests
 * through bt_settings, and a switch shows the satellite's answer, not the finger's wish.
 *
 *   left     Телефон — who is connected, with a button to drop them
 *            Налаштування — Bluetooth on/off, visible to new phones, reconnect the last phone
 *            at start, the name phones see
 *   right    Кодеки — what is offered to a phone, from its next connection (SBC always is)
 *            Спарені — the phones it remembers, newest first, each with a way to forget it
 *
 * A satellite older than 1.8 has none of that to give: the page then shows the phone and the
 * disconnect button, which CMD_RADIO has always carried, and says why the rest is missing.
 *
 * Pairing itself is not here. The request can come while any page is up, or in standby, so
 * the dialog that shows the code belongs to the shell (ui_shell.cpp).
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "bt_settings.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <new>

namespace {

using i18n::S;
using i18n::tr;

// ── geometry, in the 752x384 content column ──
constexpr int32_t COL_GAP = 16;
constexpr int32_t COL_W   = (uig::CONT_W - COL_GAP) / 2;      // 368
constexpr int32_t COL2_X  = COL_W + COL_GAP;
// Rows, chips and buttons a finger's 64 px.
constexpr int32_t CAP_H   = 24;
constexpr int32_t RH      = 64;
constexpr int32_t INSET   = 2;
constexpr int32_t RW      = COL_W - 2 * INSET;
constexpr int32_t GAP     = 8;
constexpr int32_t CHIP_ROW = m3w::CHIP_H + 8;                 // the codecs, two by two
constexpr int32_t CHIPS_H = 8 + 2 * CHIP_ROW;
constexpr int32_t BONDS_Y = CAP_H + CHIPS_H + GAP;            // the phones' caption, «Забути всі» beside it
constexpr int32_t BONDS_H = 64;
constexpr int32_t LIST_TOP = BONDS_Y + BONDS_H;
constexpr int32_t LIST_HT  = uig::CONT_H - LIST_TOP;
// the rename sheet, as on the Wi-Fi page
constexpr int32_t FIELD_Y = 40;
constexpr int32_t FIELD_H = 64;
constexpr int32_t GO_W    = 176;
constexpr int32_t KB_Y    = FIELD_Y + FIELD_H + 12;

constexpr int32_t group_h(int n) { return CAP_H + n * RH + 2 * INSET; }
static_assert(group_h(1) + GAP + group_h(4) <= uig::CONT_H, "the left column fits");

constexpr uint32_t TOAST_MS = 4000;

enum RowId { R_PHONE, R_RADIO, R_VISIBLE, R_AUTO, R_NAME, R_COUNT };

struct CodecDef { uint8_t bit; const char* label; };
constexpr CodecDef CODECS[] = {
    {1u << 1, "AAC"}, {1u << 2, "aptX"}, {1u << 3, "aptX HD"}, {1u << 4, "LHDC"},
};
constexpr int N_CODECS = (int)(sizeof(CODECS) / sizeof(CODECS[0]));

struct Page {
    UiCtx*      ctx = nullptr;
    lv_obj_t*   row[R_COUNT] = {};
    lv_obj_t*   title[R_COUNT] = {};
    lv_obj_t*   sub[R_COUNT] = {};
    char        title_txt[R_COUNT][48] = {};
    char        sub_txt[R_COUNT][96] = {};
    m3w::Button drop;                    ///< disconnect the phone
    lv_obj_t*   sw_radio = nullptr;
    lv_obj_t*   sw_visible = nullptr;
    lv_obj_t*   sw_auto = nullptr;
    lv_obj_t*   settings_grp = nullptr;  ///< the Налаштування card
    lv_obj_t*   settings_cap = nullptr;  ///< ... and its caption
    lv_obj_t*   right = nullptr;         ///< everything in the right column
    lv_obj_t*   note = nullptr;          ///< why the settings are missing
    char        note_txt[160] = {};
    lv_obj_t*   chips_box = nullptr;
    m3w::Chip   chip[N_CODECS];
    m3w::Button forget_all;
    m3w::VList  list;
    lv_obj_t*   empty = nullptr;
    // the rename sheet
    lv_obj_t*   sheet = nullptr;
    lv_obj_t*   field = nullptr;
    lv_obj_t*   kb = nullptr;
    m3w::Button save;
    // the rest
    m3w::Dialog dlg;
    int         dlg_bond = -2;           ///< what "Забути" forgets: a row, -1 = all
    uint8_t     dlg_addr[6] = {};
    lv_obj_t*   toast = nullptr;
    uint32_t    toast_at = 0;
    bool        toast_on = false;
    uint32_t    rev = UINT32_MAX;
    int8_t      settings_shown = -1;
    bool        preview = false;         ///< ui_bt_preview: the view is made up, keep it
};

Page g;
/// PSRAM, kept across rebuilds: the satellite's view, as last copied.
bt_settings::View* s_view = nullptr;

void toast(const char* txt) {
    m3w::toast_show(g.toast, txt);
    g.toast_at = lv_tick_get();
    g.toast_on = true;
}

bool settings() { return s_view && s_view->linked && s_view->settings; }

// ── building blocks, the System page's ──

lv_obj_t* group(lv_obj_t* root, int32_t x, int32_t y, const char* caption, int n, lv_obj_t** cap_out = nullptr) {
    const auto& t = m3::theme();
    lv_obj_t* cap = m3w::text(root, t.t.label_m, t.c.on_surface_var, caption);
    lv_obj_set_pos(cap, x + 12, y + 2);
    if (cap_out) *cap_out = cap;
    return m3w::surface(root, x, y + CAP_H, COL_W, n * RH + 2 * INSET, t.c.surface_low, m3::shape::L);
}

lv_obj_t* row(lv_obj_t* card, int i, RowId id, const char* sym, const char* title, int32_t control_w) {
    const auto& t = m3::theme();
    lv_obj_t* r = m3w::box(card, INSET, INSET + i * RH, RW, RH);
    lv_obj_set_style_radius(r, m3::shape::M, 0);
    lv_obj_t* ic = m3w::icon(r, t.t.icon_m, t.c.on_surface_var, sym);
    lv_obj_align(ic, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t* ti = m3w::text(r, t.t.body_l, t.c.on_surface, title);
    lv_obj_set_pos(ti, 54, (RH - 25 - 2 - 21) / 2);    // body_l's line, 2 px, label_s's
    lv_obj_set_size(ti, RW - 54 - control_w - 16, lv_font_get_line_height(t.t.body_l));
    lv_label_set_long_mode(ti, LV_LABEL_LONG_DOT);
    lv_obj_t* s = m3w::text(r, t.t.label_s, t.c.on_surface_var, "");
    lv_obj_set_pos(s, 54, (RH - 25 - 2 - 21) / 2 + 25 + 2);
    lv_obj_set_size(s, RW - 54 - control_w - 16, lv_font_get_line_height(t.t.label_s));
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    g.row[id] = r;
    g.title[id] = ti;
    g.sub[id] = s;
    return r;
}

lv_obj_t* toggle(lv_obj_t* r, lv_event_cb_t cb) {
    lv_obj_t* sw = m3w::switch_m3(r, RW - 52 - 14, (RH - 32) / 2, false);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, nullptr);
    return sw;
}

void set_title(RowId id, const char* txt) { m3w::set_text(g.title[id], g.title_txt[id], sizeof(g.title_txt[id]), txt); }
void set_sub(RowId id, const char* txt)   { m3w::set_text(g.sub[id], g.sub_txt[id], sizeof(g.sub_txt[id]), txt); }

void addr_text(const uint8_t* a, char* out, size_t n) {
    snprintf(out, n, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
}

// ── requests ──

/// Everything as the switches and chips now say, for BT_CONFIG_SET's whole-state rule.
void post_config() {
    if (!settings()) return;
    uint8_t codecs = 1;                                     // SBC, always
    for (int i = 0; i < N_CODECS; i++) if (g.chip[i].selected) codecs |= CODECS[i].bit;
    bt_settings::request_config(lv_obj_has_state(g.sw_radio, LV_STATE_CHECKED),
                                lv_obj_has_state(g.sw_visible, LV_STATE_CHECKED),
                                lv_obj_has_state(g.sw_auto, LV_STATE_CHECKED), codecs);
}

void switch_cb(lv_event_t*) { post_config(); }

void chip_cb(lv_event_t* e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= N_CODECS || !settings()) return;
    m3w::chip_set(g.chip[i], !g.chip[i].selected);
    post_config();
    toast(tr(S::bt_codecs_later));
}

void drop_cb(lv_event_t*) {
    bt_settings::request_disconnect();
    toast(tr(S::bt_dropping));
}

// ── the rename sheet ──

bool sheet_open() { return g.sheet && !lv_obj_has_flag(g.sheet, LV_OBJ_FLAG_HIDDEN); }

void open_sheet() {
    if (!settings()) return;
    lv_textarea_set_text(g.field, s_view->name);
    m3w::keyboard_reset(g.kb);
    m3w::show(g.sheet, true);
    lv_obj_move_foreground(g.sheet);
}

void close_sheet() { m3w::show(g.sheet, false); }

void name_cb(lv_event_t*) { open_sheet(); }

void save_cb(lv_event_t*) {
    const char* txt = lv_textarea_get_text(g.field);
    if (!txt || !txt[0]) { toast(tr(S::bt_name_empty)); return; }
    if (!bt_settings::request_name(txt)) { toast(tr(S::bt_name_long)); return; }
    close_sheet();
    toast(tr(S::bt_name_lag));
}

// ── the bonds ──

void fill(int idx, m3w::VItem& it, void*) {
    if (!s_view || idx >= s_view->count) return;
    const auto& b = s_view->bonds[idx];
    static char addr[20];
    it.lead  = UI_SYM_PHONE;
    if (b.name[0]) it.title = b.name;
    else { addr_text(b.addr, addr, sizeof(addr)); it.title = addr; }
    const bool now  = b.flags & 1u;                         // sat::BOND_CONNECTED
    const bool last = b.flags & 2u;                         // sat::BOND_LAST
    it.sub   = now ? tr(S::bt_bond_now) : last ? tr(S::bt_bond_last) : nullptr;
    it.trail = UI_SYM_DELETE;
    it.on    = now;
}

void ask_forget(int idx) {
    char title[80];
    g.dlg_bond = idx;
    if (idx < 0) {
        m3w::dialog_show(g.dlg, tr(S::bt_forget_all_q), tr(S::bt_forget_all_body),
                         tr(S::bt_forget_all), tr(S::common_cancel));
        return;
    }
    const auto& b = s_view->bonds[idx];
    memcpy(g.dlg_addr, b.addr, 6);
    char addr[20];
    addr_text(b.addr, addr, sizeof(addr));
    i18n::fmt(title, sizeof(title), tr(S::bt_forget_q), {{"name", b.name[0] ? b.name : addr}});
    m3w::dialog_show(g.dlg, title, tr(S::bt_forget_body), tr(S::bt_forget), tr(S::common_cancel));
}

void trail_pick(int idx, void*) {
    if (!s_view || idx >= s_view->count || !settings()) return;
    ask_forget(idx);
}

void forget_all_cb(lv_event_t*) { if (settings()) ask_forget(-1); }

void dlg_yes_cb(lv_event_t*) {
    m3w::dialog_hide(g.dlg);
    bt_settings::request_forget(g.dlg_bond < 0 ? nullptr : g.dlg_addr);
    g.dlg_bond = -2;
}
void dlg_no_cb(lv_event_t*) { m3w::dialog_hide(g.dlg); g.dlg_bond = -2; }

// ── build ──

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    g = Page{};
    g.ctx = &c;
    if (!s_view) {
        void* m = heap_caps_calloc(1, sizeof(bt_settings::View), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_view = m ? new (m) bt_settings::View{} : nullptr;
    }
    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);
    lv_obj_add_flag(root, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // ── left: the phone ──
    lv_obj_t* phone = group(root, 0, 0, tr(S::pair_phone), 1);
    lv_obj_t* r = row(phone, 0, R_PHONE, UI_SYM_PHONE, "", 60);
    g.drop = m3w::icon_button(r, RW - 64, 0, 64, UI_SYM_LINK_OFF, t.t.icon_m);
    lv_obj_add_event_cb(g.drop.obj, drop_cb, LV_EVENT_CLICKED, nullptr);

    // ── left: the settings ──
    lv_obj_t* set = group(root, 0, group_h(1) + GAP, tr(S::bt_settings), 4, &g.settings_cap);
    g.settings_grp = set;
    r = row(set, 0, R_RADIO, UI_SYM_BLUETOOTH, tr(S::src_bt), 70);
    g.sw_radio = toggle(r, switch_cb);
    r = row(set, 1, R_VISIBLE, UI_SYM_BT_SEARCH, tr(S::bt_visible), 70);
    g.sw_visible = toggle(r, switch_cb);
    r = row(set, 2, R_AUTO, UI_SYM_LINK, tr(S::bt_auto), 70);
    g.sw_auto = toggle(r, switch_cb);
    r = row(set, 3, R_NAME, UI_SYM_EDIT, tr(S::bt_name), 40);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, name_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* chev = m3w::icon(r, t.t.icon_m, t.c.on_surface_var, UI_SYM_CHEVRON);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -14, 0);

    // ── right: codecs and the phones it remembers ──
    g.right = m3w::box(root, COL2_X, 0, COL_W, uig::CONT_H);
    lv_obj_add_flag(g.right, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t* ccap = m3w::text(g.right, t.t.label_m, t.c.on_surface_var, tr(S::bt_codecs));
    lv_obj_set_pos(ccap, 12, 2);
    lv_obj_t* ccard = m3w::surface(g.right, 0, CAP_H, COL_W, CHIPS_H, t.c.surface_low, m3::shape::L);
    g.chips_box = ccard;
    // Two by two, each chip a fixed half: a chip grows by its check mark when chosen, and in
    // one flowing row that pushed the last one onto a line of its own and back again.
    constexpr int32_t CHIP_W = (COL_W - 12 * 2 - 8) / 2;
    for (int i = 0; i < N_CODECS; i++) {
        g.chip[i] = m3w::chip(ccard, 12 + (i % 2) * (CHIP_W + 8), 8 + (i / 2) * CHIP_ROW, CODECS[i].label, false);
        lv_obj_set_width(g.chip[i].obj, CHIP_W);
        lv_obj_add_event_cb(g.chip[i].obj, chip_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    lv_obj_t* bcap = m3w::text(g.right, t.t.label_m, t.c.on_surface_var, tr(S::bt_paired));
    lv_obj_set_pos(bcap, 12, BONDS_Y + (BONDS_H - lv_font_get_line_height(t.t.label_m)) / 2);
    // As wide as its text, growing to the left: "Zapomnij wszystkie" is half as long again
    // as the "Забути всі" the 132 px were set for.
    const char* fa = tr(S::bt_forget_all);
    const int32_t fa_w = LV_MAX(132, lv_text_get_width(fa, (uint32_t)strlen(fa), t.t.label_m, 0) + 2 * 16);
    g.forget_all = m3w::button_text(g.right, COL_W - fa_w, BONDS_Y, fa_w, BONDS_H, fa);
    lv_obj_add_event_cb(g.forget_all.obj, forget_all_cb, LV_EVENT_CLICKED, nullptr);
    g.list.build(g.right, 0, LIST_TOP, COL_W, LIST_HT, fill, nullptr, trail_pick, nullptr);
    g.empty = m3w::text(g.right, t.t.body_m, t.c.on_surface_var, tr(S::bt_no_phones));
    lv_obj_set_pos(g.empty, 12, LIST_TOP + 12);
    m3w::show(g.empty, false);

    // ── why there are no settings ──
    g.note = m3w::text(root, t.t.body_m, t.c.on_surface_var, "");
    lv_obj_set_pos(g.note, 12, group_h(1) + GAP + 8);
    lv_obj_set_width(g.note, uig::CONT_W - 24);
    lv_label_set_long_mode(g.note, LV_LABEL_LONG_WRAP);

    // ── the rename sheet ──
    g.sheet = m3w::surface(root, 0, 0, uig::CONT_W, uig::CONT_H, t.c.surface, 0);
    lv_obj_t* st = m3w::text(g.sheet, t.t.title_m, t.c.on_surface, tr(S::bt_name_title));
    lv_obj_set_pos(st, 4, 2);
    g.field = m3w::text_field(g.sheet, 0, FIELD_Y, uig::CONT_W - GO_W - 12, FIELD_H, tr(S::bt_name), 32);
    g.save = m3w::filled_button(g.sheet, uig::CONT_W - GO_W, FIELD_Y, GO_W, FIELD_H, m3::shape::FULL,
                                nullptr, nullptr, tr(S::bt_save));
    lv_obj_add_event_cb(g.save.obj, save_cb, LV_EVENT_CLICKED, nullptr);
    g.kb = m3w::keyboard(g.sheet, 0, KB_Y, uig::CONT_W, uig::CONT_H - KB_Y, g.field);
    m3w::show(g.sheet, false);

    // on the screen, not the page: its shade covers the header and its ← too
    g.dlg = m3w::dialog(lv_obj_get_screen(root), 520, 230);
    lv_obj_add_event_cb(g.dlg.yes.obj, dlg_yes_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g.dlg.no.obj, dlg_no_cb, LV_EVENT_CLICKED, nullptr);
    g.toast = m3w::toast(root);
    return root;
}

// ── refresh ──

void set_switch(lv_obj_t* sw, bool on) {
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    else    lv_obj_remove_state(sw, LV_STATE_CHECKED);
}

/// The satellite's answer onto the switches, chips and list — only when it changed, so a
/// finger that has just moved a switch is not overruled by the old state every tick.
void paint_settings() {
    const auto& v = *s_view;
    set_switch(g.sw_radio, v.radio);
    set_switch(g.sw_visible, v.discoverable);
    set_switch(g.sw_auto, v.auto_reconnect);
    set_sub(R_RADIO, !v.known ? "…" : tr(v.radio ? S::bt_on : S::bt_off));
    set_sub(R_VISIBLE, tr(v.discoverable ? S::bt_visible_on : S::bt_visible_off));
    set_sub(R_AUTO, !v.known ? "…" : tr(v.auto_reconnect ? S::bt_on : S::bt_off));   // a state, as its neighbours
    set_sub(R_NAME, v.name[0] ? v.name : "…");
    for (int i = 0; i < N_CODECS; i++) {
        const bool has = v.codecs_supported & CODECS[i].bit;
        m3w::show(g.chip[i].obj, has);
        m3w::chip_set(g.chip[i], has && (v.codecs_enabled & CODECS[i].bit));
    }
    g.list.set_count(v.count);
    m3w::show(g.empty, v.count == 0);
    m3w::show(g.forget_all.obj, v.count > 0);
}

void paint_phone(const UiLvglModule::Snapshot& s) {
    char buf[96];
    if (!s.bt_link) {
        set_title(R_PHONE, tr(s.bt_seen ? S::bt_err_no_satellite : S::sys_bt_none));
        set_sub(R_PHONE, "");
    } else if (s.bt_connected) {
        set_title(R_PHONE, s.bt_peer[0] ? s.bt_peer : tr(S::pair_phone));
        set_sub(R_PHONE, s.bt_codec[0] ? s.bt_codec : tr(S::bt_connected));
    } else {
        set_title(R_PHONE, tr(S::bt_phone_none));
        if (settings() && s_view->known) {
            if (!s_view->radio)             snprintf(buf, sizeof(buf), "%s", tr(S::bt_radio_off));
            else if (s_view->discoverable)  i18n::fmt(buf, sizeof(buf), tr(S::manage_visible_as), {{"name", s_view->name}});
            else                            snprintf(buf, sizeof(buf), "%s", tr(S::bt_hidden_wait));
        } else {
            snprintf(buf, sizeof(buf), "%s", tr(S::bt_wait_phone));
        }
        set_sub(R_PHONE, buf);
    }
    m3w::show(g.drop.obj, s.bt_link && s.bt_connected);
}

void refresh(lv_obj_t*, UiCtx& c) {
    if (!s_view) return;
    const uint32_t rev = bt_settings::revision();
    if (rev != g.rev && !g.preview) {
        g.rev = rev;
        bt_settings::snapshot(*s_view);
        if (settings()) paint_settings();
    }
    const int8_t shown = settings() ? 1 : 0;
    if (shown != g.settings_shown) {
        g.settings_shown = shown;
        m3w::show(g.settings_grp, shown);
        m3w::show(g.settings_cap, shown);
        m3w::show(g.right, shown);
        m3w::show(g.note, !shown);
        if (!shown) { close_sheet(); m3w::dialog_hide(g.dlg); }
    }
    if (!shown) {
        const char* why = tr(!c.s.bt_link ? S::bt_settings_wait : S::bt_settings_old);
        m3w::set_text(g.note, g.note_txt, sizeof(g.note_txt), why);
    }
    paint_phone(c.s);

    if (g.toast_on && lv_tick_elaps(g.toast_at) > TOAST_MS) {
        g.toast_on = false;
        m3w::show(g.toast, false);
    }
}

void shown(lv_obj_t*, UiCtx&) {
    close_sheet();
    m3w::dialog_hide(g.dlg);
    g.rev = UINT32_MAX;                      // repaint from the view on the next pass
    if (settings()) bt_settings::request_refresh();
}

bool back(UiCtx&) {
    if (sheet_open()) { close_sheet(); return true; }
    if (g.dlg.scrim && !lv_obj_has_flag(g.dlg.scrim, LV_OBJ_FLAG_HIDDEN)) { dlg_no_cb(nullptr); return true; }
    return false;
}

}  // namespace

void ui_bt_preview(bool on) {
    if (!s_view || on == g.preview) return;
    g.preview = on;
    g.rev = UINT32_MAX;                      // off: the next refresh copies the real view
    if (!on) { g.settings_shown = -1; return; }
    auto& v = *s_view;
    v = bt_settings::View{};
    v.linked = v.settings = v.known = true;
    v.radio = v.discoverable = true;
    v.codecs_supported = 0x1F;
    v.codecs_enabled = 0x0B;                 // SBC, AAC, aptX HD
    snprintf(v.name, sizeof(v.name), "P4 Player");
    const char* names[] = {"Pixel 7", "iPad", ""};
    v.total = v.count = 3;
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 6; k++) v.bonds[i].addr[k] = (uint8_t)(0x10 * (i + 1) + k);
        v.bonds[i].flags = i == 0 ? 3 : 0;
        snprintf(v.bonds[i].name, sizeof(v.bonds[i].name), "%s", names[i]);
    }
    g.settings_shown = -1;
    paint_settings();
}

const UiScreenDef UI_SCREEN_BT ={"bt", UI_SYM_BLUETOOTH, build, refresh, shown, back};
