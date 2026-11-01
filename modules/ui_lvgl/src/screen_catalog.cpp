/**
 * @file screen_catalog.cpp
 * @brief The station catalog: Топ · Країни · Жанри · Пошук beside Улюблені, the station list's
 *        own tab.
 *
 * Stages 20б and 20в. The tabs are one page to
 * the reader: Улюблені is the station list with its presets (screen_radio.cpp), the others are
 * this page, and the same row of tabs heads both. Топ is Ukraine's most played; Країни and Жанри
 * are lists that lead to stations, and Пошук is a line and a keyboard (ui_search_kb.h) that lead
 * to the stations whose name has the words — so the header's ← climbs from the stations back to
 * what led to them before it leaves the page.
 *
 * Stations come a page of up to twelve at a time with ‹ ›, as the DLNA browser's do, sorted
 * and floored by the chips. A tap plays the station and goes to the Player; the + at the end
 * of a row keeps it in the station list, or in a preset.
 *
 * Nothing here talks to the network. The radio module asks Radio Browser and publishes what
 * comes back (radio_catalog_ui.h); this page draws it and leaves asks. A row is named by its
 * place on a page, so an ask carries the page it was read from, and a row tapped as a new
 * page arrives does nothing rather than the wrong thing.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "ui_search_kb.h"
#include "ppa_flush.h"
#include "radio_catalog.h"
#include "radio_catalog_ui.h"
#include "stations.h"
#include "esp_heap_caps.h"
#include <cstdio>
#include <cstring>
#include <new>

namespace rc = radio_catalog;

namespace {

using i18n::P;
using i18n::S;
using i18n::tr;
using modesp::gen::AUDIO_SOURCE_RADIO;

enum Tab : int { TAB_FAV = 0, TAB_TOP = 1, TAB_COUNTRIES = 2, TAB_GENRES = 3, TAB_SEARCH = 4, N_TABS = 5 };
constexpr S TAB_NAMES[N_TABS] = {S::st_tab_fav, S::st_tab_top, S::st_tab_countries, S::st_tab_genres, S::st_tab_search};

// ── geometry, in the 752x384 content column ──
// The tabs, the pager and the chips are a finger's 64 px.
constexpr int32_t TAB_W   = 112;
constexpr int32_t TAB_H   = 64;
constexpr int32_t TABS_W  = N_TABS * TAB_W;
constexpr int32_t CHIP_Y  = TAB_H + 4;
constexpr int32_t LIST_Y  = CHIP_Y + m3w::CHIP_H + 4;
constexpr int32_t BTN     = 64;
constexpr int32_t NEXT_X  = uig::CONT_W - BTN;
constexpr int32_t PNUM_W  = 40;
constexpr int32_t PNUM_X  = NEXT_X - PNUM_W;
constexpr int32_t PREV_X  = PNUM_X - BTN;
constexpr int32_t CAP_W   = 220;
// the search line, and the keyboard to the bottom: five rows of 48 and their gaps
constexpr int32_t FIELD_H = 44;
constexpr int32_t KB_Y    = CHIP_Y + FIELD_H + 8;
static_assert(uig::CONT_H - KB_Y >= 5 * 48 + 4 * 6, "the keyboard's keys stay 48 px tall");
constexpr uint32_t TOAST_MS = 2500;
/// Characters in the search line: Cyrillic is two bytes, and "name:" and the words must fit
/// the 64 bytes of a request (radio_catalog_ui.h).
constexpr uint32_t SEARCH_MAX = 28;
static_assert(TABS_W <= PREV_X, "the tabs end before the pager starts");

/// What the page is showing below the tabs: stations, a list that leads to stations, or the
/// search line with its keyboard.
enum class View : uint8_t { STATIONS, COUNTRIES, GENRES, SEARCH };

constexpr S SORT_NAMES[] = {S::cat_sort_pop, S::cat_sort_votes, S::cat_sort_quality, S::cat_sort_az};
constexpr int N_SORTS = 4;
/// The floor chip steps through these, in this order, from what is set.
/// The floor chip's steps; "320" is a number in every language.
const char* quality_name(int q) {
    switch (q) {
    case 0:  return tr(S::cat_q_all);
    case 2:  return tr(S::cat_q_192_short);
    case 3:  return "320";
    case 4:  return tr(S::cat_q_flac_short);
    default: return tr(S::cat_q_128_short);
    }
}
constexpr int N_QUALITIES = 5;

struct Page {
    UiCtx*      ctx = nullptr;
    lv_obj_t*   root = nullptr;
    lv_obj_t*   tabs = nullptr;
    m3w::VList  list;
    m3w::Chip   sort[N_SORTS];
    m3w::Chip   quality;
    lv_obj_t*   caption = nullptr;
    m3w::Button prev, next;
    lv_obj_t*   pnum  = nullptr;
    lv_obj_t*   empty = nullptr;
    lv_obj_t*   field = nullptr;   ///< the search line
    lv_obj_t*   kb    = nullptr;   ///< and its keyboard
    lv_obj_t*   toast = nullptr;
    uint32_t    toast_at = 0;
    bool        toast_on = false;
    // the keep sheet
    lv_obj_t*   scrim = nullptr;
    lv_obj_t*   sheet_title = nullptr;
    m3w::Button sheet_fav;
    m3w::Button sheet_pre[radio_core::PRESETS];
    m3w::Button sheet_cancel;
    int         sheet_row = -1;
    uint32_t    sheet_serial = 0;
    // what is shown
    int         tab  = TAB_TOP;
    View        view = View::STATIONS;
    char        spec[64]    = "top:UA";   ///< the request the stations view stands for
    char        where[64]   = "";         ///< the caption over it: Ukraine's name at first
    /// The request serial current when this page last asked its question: an answer must
    /// come from a later one, or a repeat of the same question would show the old page.
    uint32_t    asked_serial = 0;
    // change detection
    uint32_t    status_gen  = 0xFFFFFFFF;
    uint32_t    page_gen    = 0xFFFFFFFF;
    uint32_t    countries_gen = 0xFFFFFFFF;
    uint32_t    genres_gen  = 0xFFFFFFFF;
    uint32_t    fav_gen     = 0xFFFFFFFF;
    char        playing[64] = "";
    bool        shown_rows  = false;
    char        cap_txt[96] = "";
    char        pnum_txt[12] = "";
    char        empty_txt[96] = "";
    bool        fav[rc::PAGE_ROWS] = {};
};

Page g;
rc::Status s_status;
// The copies drawn from, in PSRAM: a page of rows is ~7 KB.
rc::Page*      s_page      = nullptr;
rc::Countries* s_countries = nullptr;
rc::Genres*    s_genres    = nullptr;

template <typename T>
T* alloc_psram() {
    void* m = heap_caps_calloc(1, sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return m ? new (m) T() : nullptr;
}

void ask(rc::AskKind kind, int32_t value = 0, int32_t slot = -1, const char* spec = nullptr) {
    rc::Ask a;
    a.kind  = kind;
    a.value = value;
    a.slot  = slot;
    a.serial = s_page ? s_page->serial : 0;
    if (spec) snprintf(a.spec, sizeof(a.spec), "%s", spec);
    rc::asks().post(a);
}

void toast(const char* txt) {
    m3w::toast_show(g.toast, txt);
    g.toast_at = lv_tick_get();
    g.toast_on = true;
}

void toast_hide() {
    g.toast_on = false;
    m3w::show(g.toast, false);
}

// ── names the font can draw ──
//
// Station names come in every script there is; the panel's faces carry Latin-1, Ukrainian
// Cyrillic and some punctuation (tools/fonts/build_fonts.py). A letter with a diacritic the
// face lacks becomes its base letter — "Złote" reads "Zlote" — and anything else is left
// out. A name with nothing readable left is replaced by the stream's host.

/// U+0100..U+017F to their base letters.
constexpr char LATIN_EXT_A[] =
    "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
static_assert(sizeof(LATIN_EXT_A) == 129, "one letter for each of U+0100..U+017F");

bool drawable(uint32_t cp) {
    if (cp >= 0x20 && cp < 0x7F) return true;
    if (cp >= 0xC0 && cp <= 0xFF) return true;
    if (cp >= 0x410 && cp <= 0x44F) return true;
    switch (cp) {
    case 0xB0: case 0xB7: case 0xAB: case 0xBB: case 0x2013: case 0x2014: case 0x2019:
    case 0x201C: case 0x201D: case 0x2022: case 0x203A: case 0x2026: case 0x2212:
    case 0x404: case 0x454: case 0x406: case 0x456: case 0x407: case 0x457: case 0x490: case 0x491:
        return true;
    default:
        return false;
    }
}

/// The nearest code point the face has, or 0 for none.
uint32_t substitute(uint32_t cp) {
    if (drawable(cp)) return cp;
    if (cp >= 0x100 && cp <= 0x17F) return (uint8_t)LATIN_EXT_A[cp - 0x100];
    switch (cp) {
    case 0xA0:   return ' ';
    case 0x2018: case 0xB4: case 0x2032: return 0x2019;
    case 0x401:  return 0x415;   // Ё
    case 0x451:  return 0x435;   // ё
    case 0x40E:  return 0x423;   // Ў
    case 0x45E:  return 0x443;   // ў
    default:     return 0;
    }
}

void put_utf8(uint32_t cp, char* out, size_t n, size_t& k) {
    char b[4];
    size_t len;
    if (cp < 0x80)       { b[0] = (char)cp; len = 1; }
    else if (cp < 0x800) { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); len = 2; }
    else                 { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                           b[2] = (char)(0x80 | (cp & 0x3F)); len = 3; }
    if (k + len >= n) return;
    memcpy(out + k, b, len);
    k += len;
}

/// `in` as the face can draw it; "" when fewer than two letters or digits survive.
void display_name(const char* in, char* out, size_t n) {
    size_t k = 0;
    int readable = 0;
    bool space = true;                      // drops leading spaces and runs of them
    const auto* p = reinterpret_cast<const unsigned char*>(in);
    while (*p) {
        uint32_t cp;
        int len;
        if (p[0] < 0x80)                                  { cp = p[0]; len = 1; }
        else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); len = 2; }
        else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu); len = 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80)                 { cp = 0x10000; len = 4; }   // never drawable
        else                                              { cp = 0; len = 1; }
        p += len;
        const uint32_t d = substitute(cp);
        if (!d) continue;
        if (d == ' ') {
            if (!space) { put_utf8(' ', out, n, k); space = true; }
            continue;
        }
        space = false;
        if ((d >= '0' && d <= '9') || ((d | 0x20) >= 'a' && (d | 0x20) <= 'z') || d >= 0xC0) readable++;
        put_utf8(d, out, n, k);
    }
    while (k && out[k - 1] == ' ') k--;
    out[k] = '\0';
    if (readable < 2) out[0] = '\0';
}

/// The host of a stream URL, for a row whose name says nothing the face can draw.
void host_of(const char* url, char* out, size_t n) {
    const char* p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t k = 0;
    while (p[k] && p[k] != '/' && p[k] != ':' && p[k] != '?' && k + 1 < n) { out[k] = p[k]; k++; }
    out[k] = '\0';
}

/// The Топ tab's caption: Ukraine's name from the country table in the panel's language.
const char* ukraine() {
    const char* name = rc::country_name("UA", (int)i18n::lang());
    return name ? name : "UA";
}

/// 12345 → "12 345": counts in the thousands read better in groups.
void grouped(uint32_t v, char* out, size_t n) {
    char digits[12];
    const int len = snprintf(digits, sizeof(digits), "%u", (unsigned)v);
    size_t k = 0;
    for (int i = 0; i < len && k + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[k++] = ' ';
        out[k++] = digits[i];
    }
    out[k] = '\0';
}

// ── the tabs, shared with the station list ──

void paint_tabs(lv_obj_t* row, int on) {
    if (!row) return;
    const auto& t = m3::theme();
    for (int i = 0; i < N_TABS; i++) {
        lv_obj_t* b = lv_obj_get_child(row, i);
        lv_obj_t* l = b ? lv_obj_get_child(b, 0) : nullptr;
        if (!b || !l) continue;
        const bool sel = (i == on);
        lv_obj_set_style_bg_color(b, m3::col(t.c.secondary_container), 0);
        lv_obj_set_style_bg_opa(b, sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(l, m3::col(sel ? t.c.on_secondary_container : t.c.on_surface_var), 0);
    }
}

void tab_cb(lv_event_t* e) {
    const int tab = (int)(intptr_t)lv_event_get_user_data(e);
    if (tab == TAB_FAV) ui_shell_show_radio();
    else                ui_catalog_open(tab);
}

// ── the rows ──

bool on_air(const rc::Row& r) {
    if (!g.ctx) return false;
    const auto& s = g.ctx->s;
    return ui_src(s) == UiSrc::RADIO && s.radio_station < 0 && r.name[0] && strcmp(s.radio_name, r.name) == 0;
}

void station_sub(const rc::Row& r, char* out, size_t n) {
    char rate[32];
    if (!strcmp(r.codec, "FLAC"))  snprintf(rate, sizeof(rate), "FLAC · %s", tr(S::cat_lossless));
    else if (r.bitrate)            snprintf(rate, sizeof(rate), "%s %u", r.codec, (unsigned)r.bitrate);
    else                           snprintf(rate, sizeof(rate), "%s", r.codec);
    const char* country = rc::country_name(r.country, (int)i18n::lang());
    char metric[48] = "";
    char num[16];
    if (s_status.sort == rc::Sort::VOTES) {
        grouped(r.votes, num, sizeof(num));
        char part[40];
        i18n::fmt(part, sizeof(part), i18n::plural(P::cat_votes, (int32_t)r.votes), {{"n", num}});
        snprintf(metric, sizeof(metric), " · %s", part);
    } else if (s_status.sort == rc::Sort::POPULAR) {
        grouped(r.clicks, num, sizeof(num));
        char part[40];
        i18n::fmt(part, sizeof(part), tr(S::cat_per_day), {{"n", num}});
        snprintf(metric, sizeof(metric), " · %s", part);
    }
    snprintf(out, n, "%s · %s%s", rate, country ? country : r.country, metric);
}

void fill(int idx, m3w::VItem& it, void*) {
    static char title[rc::NAME_LEN + 8];
    static char sub[128];
    it.title = "—";
    switch (g.view) {
    case View::STATIONS: {
        if (!s_page || idx >= s_page->count) return;
        const rc::Row& r = s_page->row[idx];
        display_name(r.name, title, sizeof(title));
        if (!title[0]) host_of(r.url, title, sizeof(title));
        station_sub(r, sub, sizeof(sub));
        it.lead  = UI_SYM_RADIO;
        it.title = title;
        it.sub   = sub;
        it.trail = (idx < rc::PAGE_ROWS && g.fav[idx]) ? UI_SYM_HEART : UI_SYM_ADD;
        it.on    = on_air(r);
        break;
    }
    case View::COUNTRIES: {
        if (!s_countries || idx >= s_countries->count) return;
        const rc::CountryCount& c = s_countries->item[idx];
        const char* name = rc::country_name(c.code, (int)i18n::lang());
        snprintf(title, sizeof(title), "%s", name ? name : c.code);
        char num[16];
        grouped(c.stations, num, sizeof(num));
        i18n::fmt(sub, sizeof(sub), i18n::plural(P::cat_stations, (int32_t)c.stations), {{"n", num}});
        it.lead  = UI_SYM_LANGUAGE;
        it.title = title;
        it.sub   = sub;
        it.trail = UI_SYM_CHEVRON;
        break;
    }
    case View::GENRES: {
        if (!s_genres || idx >= s_genres->count) return;
        it.lead  = UI_SYM_MUSIC;
        it.title = rc::genre_name(s_genres->item[idx], (int)i18n::lang());
        it.trail = UI_SYM_CHEVRON;
        break;
    }
    case View::SEARCH:
        break;                                      // the list is not shown while typing
    }
}

/// The stations of `spec` under the caption `where`; either may be the page's own buffer.
void show_stations(const char* spec, const char* where) {
    g.view = View::STATIONS;
    if (spec != g.spec)   snprintf(g.spec, sizeof(g.spec), "%s", spec);
    if (where != g.where) snprintf(g.where, sizeof(g.where), "%s", where);
    g.asked_serial = s_status.serial;
    g.shown_rows = false;
    g.list.set_count(0);
    g.page_gen = g.status_gen = 0xFFFFFFFF;          // repaint from whatever comes next
    ask(rc::AskKind::QUERY, 0, -1, g.spec);
}

void show_list(View v) {
    g.view = v;
    g.shown_rows = false;
    g.list.set_count(0);
    g.countries_gen = g.genres_gen = g.status_gen = 0xFFFFFFFF;
    if (v == View::COUNTRIES) ask(rc::AskKind::COUNTRIES);
}

/// The search line and its keyboard, with what was typed last time still in it.
void show_search() {
    g.view = View::SEARCH;
    g.shown_rows = false;
    g.list.set_count(0);
    g.status_gen = 0xFFFFFFFF;
}

void paint_bar();

/// "Шукати": the stations anywhere whose name has what was typed, most played first unless
/// the chips say otherwise. Radio Browser matches it as a substring and without case.
void search_enter(void*) {
    char words[SEARCH_MAX * 2 + 3];                 // two bytes a character, at most
    snprintf(words, sizeof(words), "%s", g.field ? lv_textarea_get_text(g.field) : "");
    size_t n = strlen(words);
    while (n && words[n - 1] == ' ') words[--n] = '\0';
    const char* w = words;
    while (*w == ' ') w++;
    if (!*w) {
        toast(tr(S::cat_type_name));
        return;
    }
    char spec[sizeof(words) + 5];
    snprintf(spec, sizeof(spec), "name:%s", w);
    char where[sizeof(words) + 4];
    snprintf(where, sizeof(where), "«%s»", w);
    show_stations(spec, where);
    paint_bar();
}
static_assert(SEARCH_MAX * 2 + 2 + 5 < sizeof(rc::Ask::spec), "a search fits a request");

void open_sheet(int row);

void pick(int idx, void*) {
    switch (g.view) {
    case View::STATIONS:
        if (!s_page || idx >= s_page->count) return;
        ask(rc::AskKind::PLAY, idx);
        ui_shell_show_player();
        break;
    case View::COUNTRIES: {
        if (!s_countries || idx >= s_countries->count) return;
        const rc::CountryCount& c = s_countries->item[idx];
        char spec[16];
        snprintf(spec, sizeof(spec), "country:%s", c.code);
        const char* name = rc::country_name(c.code, (int)i18n::lang());
        show_stations(spec, name ? name : c.code);
        break;
    }
    case View::GENRES: {
        if (!s_genres || idx >= s_genres->count) return;
        char spec[64];
        snprintf(spec, sizeof(spec), "tag:%s", s_genres->item[idx].tag);
        show_stations(spec, rc::genre_name(s_genres->item[idx], (int)i18n::lang()));
        break;
    }
    case View::SEARCH:
        break;
    }
}

void trail_pick(int idx, void*) {
    if (g.view == View::STATIONS) open_sheet(idx);
    else                          pick(idx, nullptr);
}

// ── the keep sheet: into the list, or into a preset ──

bool sheet_open() { return g.scrim && !lv_obj_has_flag(g.scrim, LV_OBJ_FLAG_HIDDEN); }

void close_sheet() {
    m3w::show(g.scrim, false);
    g.sheet_row = -1;
}

void open_sheet(int row) {
    if (!s_page || row < 0 || row >= s_page->count) return;
    char name[rc::NAME_LEN + 8];
    display_name(s_page->row[row].name, name, sizeof(name));
    if (!name[0]) host_of(s_page->row[row].url, name, sizeof(name));
    lv_label_set_text(g.sheet_title, name);
    g.sheet_row = row;
    g.sheet_serial = s_page->serial;
    m3w::show(g.scrim, true);
    lv_obj_move_foreground(g.scrim);
}

/// `slot` −1 is the list alone.
void keep(int slot) {
    const int row = g.sheet_row;
    const uint32_t serial = g.sheet_serial;
    close_sheet();
    if (!s_page || row < 0 || row >= s_page->count || s_page->serial != serial) return;
    auto& store = radio_core::stations();
    const bool have = row < rc::PAGE_ROWS && g.fav[row];
    if (!have && store.count() >= radio_core::MAX_STATIONS) {
        toast(tr(S::st_err_full));
        return;
    }
    ask(rc::AskKind::KEEP, row, slot);
    char msg[64];
    if (slot >= 0) {
        char n[12];
        snprintf(n, sizeof(n), "%d", slot + 1);
        i18n::fmt(msg, sizeof(msg), tr(S::cat_toast_preset), {{"n", n}});
    }
    else if (have)  snprintf(msg, sizeof(msg), "%s", tr(S::cat_toast_already));
    else            snprintf(msg, sizeof(msg), "%s", tr(S::cat_toast_kept));
    toast(msg);
}

void sheet_fav_cb(lv_event_t*) { keep(-1); }
void sheet_pre_cb(lv_event_t* e) { keep((int)(intptr_t)lv_event_get_user_data(e)); }
void sheet_cancel_cb(lv_event_t*) { close_sheet(); }

void build_sheet(lv_obj_t* root) {
    const auto& t = m3::theme();
    constexpr int32_t W = 520, H = 332;      // buttons of 64; «Скасувати» under the presets
    g.scrim = m3w::box(root, 0, 0, uig::CONT_W, uig::CONT_H);
    lv_obj_set_style_bg_color(g.scrim, m3::col(0x000000), 0);
    lv_obj_set_style_bg_opa(g.scrim, LV_OPA_50, 0);
    lv_obj_add_flag(g.scrim, LV_OBJ_FLAG_CLICKABLE);          // swallow taps meant for below
    lv_obj_t* card = m3w::surface(g.scrim, 0, 0, W, H, t.c.surface_container, m3::shape::XL);
    lv_obj_center(card);
    g.sheet_title = m3w::text(card, t.t.title_m, t.c.on_surface, "");
    lv_obj_set_pos(g.sheet_title, 24, 20);
    lv_obj_set_width(g.sheet_title, W - 48);
    lv_label_set_long_mode(g.sheet_title, LV_LABEL_LONG_DOT);
    lv_obj_t* body = m3w::text(card, t.t.body_m, t.c.on_surface_var, tr(S::cat_keep_or_preset));
    lv_obj_set_pos(body, 24, 60);
    g.sheet_fav = m3w::tonal_button(card, 24, 96, 220, 64, m3::shape::FULL, UI_SYM_HEART, t.t.icon_s, tr(S::cat_keep));
    lv_obj_add_event_cb(g.sheet_fav.obj, sheet_fav_cb, LV_EVENT_CLICKED, nullptr);
    for (int i = 0; i < radio_core::PRESETS; i++) {
        char num[4];
        snprintf(num, sizeof(num), "%d", i + 1);
        g.sheet_pre[i] = m3w::tonal_button(card, 24 + i * 72, 168, 64, 64, m3::shape::FULL, nullptr, nullptr, num);
        lv_obj_add_event_cb(g.sheet_pre[i].obj, sheet_pre_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    g.sheet_cancel = m3w::button_text(card, W - 24 - 140, H - 24 - 64, 140, 64, tr(S::common_cancel));
    lv_obj_add_event_cb(g.sheet_cancel.obj, sheet_cancel_cb, LV_EVENT_CLICKED, nullptr);
    m3w::show(g.scrim, false);
}

// ── chips and pages ──

void sort_cb(lv_event_t* e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (g.view == View::STATIONS && (int)s_status.sort != i) ask(rc::AskKind::SORT, i);
}

void quality_cb(lv_event_t*) {
    if (g.view != View::STATIONS) return;
    // From "від 128" up to lossless and round to "Усі": the floors in the order they narrow.
    ask(rc::AskKind::QUALITY, ((int)s_status.quality + 1) % N_QUALITIES);
}

void prev_cb(lv_event_t*) { if (g.view == View::STATIONS) ask(rc::AskKind::PREV); }
void next_cb(lv_event_t*) { if (g.view == View::STATIONS) ask(rc::AskKind::NEXT); }

/// The stations view answers this page's own request, settled.
bool page_ready() {
    return s_status.state == rc::State::READY && !strcmp(s_status.spec, g.spec) && s_page &&
           s_page->serial == s_status.serial && s_status.serial != g.asked_serial;
}

void paint_bar() {
    const bool st = g.view == View::STATIONS;
    const bool typing = g.view == View::SEARCH;
    for (auto& c : g.sort) m3w::show(c.obj, st);
    m3w::show(g.quality.obj, st);
    m3w::show(g.prev.obj, st);
    m3w::show(g.next.obj, st);
    m3w::show(g.pnum, st);
    // Typing: the line and the keyboard in the list's place, so nothing scrolls under them.
    m3w::show(g.field, typing);
    m3w::show(g.kb, typing);
    m3w::show(g.list.obj(), !typing);
    m3w::show(g.caption, !typing);
    if (typing) uih::show(g.empty, false);
    char cap[96];
    if (typing) {
        cap[0] = '\0';
    } else if (st) {
        for (int i = 0; i < N_SORTS; i++) m3w::chip_set(g.sort[i], (int)s_status.sort == i);
        const int q = (int)s_status.quality;
        lv_label_set_text(g.quality.label, quality_name(q >= 0 && q < N_QUALITIES ? q : 1));
        const bool loading = s_status.state == rc::State::LOADING;
        m3w::button_enable(g.prev, !loading && s_status.page > 0);
        m3w::button_enable(g.next, !loading && page_ready() && s_status.more);
        char num[12];
        snprintf(num, sizeof(num), "%d", s_status.page + 1);
        uih::set_text(g.pnum, g.pnum_txt, sizeof(g.pnum_txt), num);
        snprintf(cap, sizeof(cap), "%s", g.where);
    } else if (g.view == View::COUNTRIES) {
        const int n = s_countries ? s_countries->count : 0;
        char num[12];
        snprintf(num, sizeof(num), "%d", n);
        if (n) i18n::fmt(cap, sizeof(cap), i18n::plural(P::cat_countries, n), {{"n", num}});
        else   cap[0] = '\0';
    } else {
        const int n = s_genres ? s_genres->count : 0;
        char num[12];
        snprintf(num, sizeof(num), "%d", n);
        if (n) i18n::fmt(cap, sizeof(cap), i18n::plural(P::cat_genres, n), {{"n", num}});
        else   cap[0] = '\0';
    }
    uih::set_text(g.caption, g.cap_txt, sizeof(g.cap_txt), cap);
}

void set_empty(const char* txt) {
    uih::set_text(g.empty, g.empty_txt, sizeof(g.empty_txt), txt ? txt : "");
    uih::show(g.empty, txt && txt[0]);
}

/// Which stations of the page are in the station list already, by stream URL.
void mark_kept() {
    memset(g.fav, 0, sizeof(g.fav));
    if (!s_page) return;
    auto& store = radio_core::stations();
    static radio_core::Station st;
    const int n = store.count();
    for (int i = 0; i < n; i++) {
        if (!store.at(i, st)) continue;
        for (int r = 0; r < s_page->count && r < rc::PAGE_ROWS; r++) {
            if (!strcmp(st.url, s_page->row[r].url)) g.fav[r] = true;
        }
    }
}

// ── build ──

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    g = Page{};
    g.ctx = &c;
    snprintf(g.where, sizeof(g.where), "%s", ukraine());
    if (!s_page)      s_page      = alloc_psram<rc::Page>();
    if (!s_countries) s_countries = alloc_psram<rc::Countries>();
    if (!s_genres)    s_genres    = alloc_psram<rc::Genres>();

    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);
    g.root = root;
    g.tabs = ui_station_tabs(root, TAB_TOP);

    // pager, at the right of the tabs
    g.prev = m3w::icon_button(root, PREV_X, 0, BTN, UI_SYM_CHEVRON_LEFT, t.t.icon_m);
    lv_obj_add_event_cb(g.prev.obj, prev_cb, LV_EVENT_CLICKED, nullptr);
    g.pnum = m3w::text(root, t.t.label_m, t.c.on_surface_var, "1");
    lv_obj_set_width(g.pnum, PNUM_W);
    lv_obj_set_style_text_align(g.pnum, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.pnum, PNUM_X, (BTN - lv_font_get_line_height(t.t.label_m)) / 2);
    g.next = m3w::icon_button(root, NEXT_X, 0, BTN, UI_SYM_CHEVRON, t.t.icon_m);
    lv_obj_add_event_cb(g.next.obj, next_cb, LV_EVENT_CLICKED, nullptr);

    // chips: the order, then the floor. A row that lays itself out, because a chip grows by
    // its check mark when it is chosen.
    lv_obj_t* chips = m3w::box(root, 4, CHIP_Y, uig::CONT_W - CAP_W - 24, m3w::CHIP_H);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chips, 8, 0);
    for (int i = 0; i < N_SORTS; i++) {
        g.sort[i] = m3w::chip(chips, 0, 0, tr(SORT_NAMES[i]), i == 0);
        lv_obj_add_event_cb(g.sort[i].obj, sort_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    g.quality = m3w::chip(chips, 0, 0, quality_name(1), false);
    lv_obj_set_style_margin_left(g.quality.obj, 8, 0);
    m3w::chip_lead(g.quality, UI_SYM_EQ);
    lv_obj_add_event_cb(g.quality.obj, quality_cb, LV_EVENT_CLICKED, nullptr);

    g.caption = m3w::text(root, t.t.label_m, t.c.on_surface_var, "");
    lv_obj_set_width(g.caption, CAP_W);
    lv_label_set_long_mode(g.caption, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(g.caption, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g.caption, uig::CONT_W - CAP_W - 12, CHIP_Y + (m3w::CHIP_H - lv_font_get_line_height(t.t.label_m)) / 2);

    g.list.build(root, 0, LIST_Y, uig::CONT_W, uig::CONT_H - LIST_Y, fill, pick, trail_pick, nullptr);
    g.empty = m3w::text(root, t.t.body_l, t.c.on_surface_var, "");
    lv_obj_set_pos(g.empty, 16, LIST_Y + 16);
    lv_obj_set_width(g.empty, uig::CONT_W - 32);
    lv_label_set_long_mode(g.empty, LV_LABEL_LONG_WRAP);
    uih::show(g.empty, false);

    // the search line and its keyboard, in the chips' and the list's place while typing
    g.field = m3w::text_field(root, 4, CHIP_Y, uig::CONT_W - 8, FIELD_H, tr(S::cat_search_ph), SEARCH_MAX);
    g.kb = search_kb::create(root, 4, KB_Y, uig::CONT_W - 8, uig::CONT_H - KB_Y, g.field,
                             search_enter, nullptr);
    m3w::show(g.field, false);
    m3w::show(g.kb, false);

    build_sheet(root);
    g.toast = m3w::toast(root);
    // The snackbar sits over the list, and the list may scroll under it: as a floating object
    // the list is still shifted, not drawn whole (ppa_flush.h). Registered for its whole life,
    // hidden too: the frame it hides in must still know where it was, or a scroll in that frame
    // carries its picture along with the list. Deleting it unregisters it.
    ppa_flush_floating(g.toast, true);
    if (!s_page || !s_countries || !s_genres) set_empty(tr(S::common_no_memory));
    return root;
}

// ── refresh ──

void refresh(lv_obj_t*, UiCtx& c) {
    if (!s_page || !s_countries || !s_genres) return;
    if (g.toast_on && lv_tick_elaps(g.toast_at) > TOAST_MS) toast_hide();

    bool repaint = false;
    const uint32_t sg = rc::status().generation();
    if (sg != g.status_gen && !(sg & 1u) && rc::status().get(s_status)) {
        g.status_gen = sg;
        repaint = true;
    }

    switch (g.view) {
    case View::STATIONS: {
        const uint32_t pg = rc::catalog().generation();
        const uint32_t fg = radio_core::stations().generation();
        bool rows = repaint;
        if (pg != g.page_gen && !(pg & 1u) && rc::catalog().copy(*s_page)) {
            g.page_gen = pg;
            rows = true;
        }
        if (fg != g.fav_gen && !(fg & 1u)) {
            g.fav_gen = fg;
            rows = true;
        }
        if (rows) {
            const bool ready = page_ready();
            if (ready) {
                mark_kept();
                if (!g.shown_rows || g.list.count() != s_page->count) {
                    g.list.set_count(s_page->count);
                } else {
                    g.list.refill();
                }
                g.shown_rows = true;
            } else if (g.shown_rows) {
                g.list.set_count(0);
                g.shown_rows = false;
            }
            if (ready)                                       set_empty(s_page->count ? "" : tr(S::cat_nothing));
            else if (s_status.state == rc::State::ERROR &&
                     !strcmp(s_status.spec, g.spec))         set_empty(tr(S::cat_down));
            else                                             set_empty(tr(S::app_loading));
        }
        break;
    }
    case View::COUNTRIES: {
        const uint32_t cg = rc::countries().generation();
        if (cg != g.countries_gen && !(cg & 1u) && rc::countries().get(*s_countries)) {
            g.countries_gen = cg;
            repaint = true;
            const bool ready = s_countries->state == rc::State::READY;
            g.list.set_count(ready ? s_countries->count : 0);
            g.shown_rows = ready;
            set_empty(ready ? (s_countries->count ? "" : tr(S::cat_nothing))
                      : s_countries->state == rc::State::ERROR ? tr(S::cat_down)
                                                               : tr(S::app_loading));
        }
        break;
    }
    case View::GENRES: {
        const uint32_t gg = rc::genres().generation();
        if (gg != g.genres_gen && !(gg & 1u) && rc::genres().get(*s_genres)) {
            g.genres_gen = gg;
            repaint = true;
            g.list.set_count(s_genres->count);
            g.shown_rows = true;
            set_empty(s_genres->count ? "" : tr(S::cat_no_genres_file));
        }
        break;
    }
    case View::SEARCH:
        break;                                      // nothing comes in while typing
    }

    // The station playing: its row is marked, and that changes with a tune or a stop.
    char playing[64];
    snprintf(playing, sizeof(playing), "%s",
             (c.s.source == AUDIO_SOURCE_RADIO && c.s.radio_station < 0) ? c.s.radio_name : "");
    if (strcmp(playing, g.playing) != 0) {
        snprintf(g.playing, sizeof(g.playing), "%s", playing);
        if (g.view == View::STATIONS && g.shown_rows) g.list.refill();
    }

    if (repaint) paint_bar();
}

void shown(lv_obj_t*, UiCtx&) {
    close_sheet();
    if (g.toast_on) toast_hide();     // left over from the last visit

    paint_tabs(g.tabs, g.tab);
    // Asked again on every visit: whoever else asked the catalog something meanwhile (the
    // web page shares it), this page shows its own answer.
    if (g.view == View::STATIONS)    show_stations(g.spec, g.where);
    else if (g.view == View::SEARCH) show_search();
    else                             show_list(g.view);
    paint_bar();
}

/// The header's ←: from a country's or a genre's stations to the list they came from, from a
/// search's stations back to the search line, and out of the page from the top.
bool back(UiCtx&) {
    if (sheet_open()) { close_sheet(); return true; }
    if (g.view == View::STATIONS && (g.tab == TAB_COUNTRIES || g.tab == TAB_GENRES)) {
        show_list(g.tab == TAB_COUNTRIES ? View::COUNTRIES : View::GENRES);
        paint_bar();
        return true;
    }
    if (g.view == View::STATIONS && g.tab == TAB_SEARCH) {
        show_search();
        paint_bar();
        return true;
    }
    return false;
}

}  // namespace

lv_obj_t* ui_station_tabs(lv_obj_t* parent, int on) {
    const auto& t = m3::theme();
    lv_obj_t* row = m3w::box(parent, 0, 0, TABS_W, TAB_H);
    for (int i = 0; i < N_TABS; i++) {
        m3w::Button b = m3w::button_text(row, i * TAB_W + 2, 0, TAB_W - 4, TAB_H, tr(TAB_NAMES[i]));
        if (b.label) lv_obj_set_style_text_font(b.label, t.t.label_m, 0);
        lv_obj_add_event_cb(b.obj, tab_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    paint_tabs(row, on);
    return row;
}

void ui_catalog_open(int tab) {
    if (tab < TAB_TOP || tab >= N_TABS) return;
    // A tab opens at its top: Топ's stations, the list of countries or genres, or the search
    // line with what was typed last time.
    g.tab = tab;
    if (tab == TAB_TOP) {
        g.view = View::STATIONS;
        snprintf(g.spec, sizeof(g.spec), "top:UA");
        snprintf(g.where, sizeof(g.where), "%s", ukraine());
    } else if (tab == TAB_SEARCH) {
        g.view = View::SEARCH;
    } else {
        g.view = tab == TAB_COUNTRIES ? View::COUNTRIES : View::GENRES;
    }
    // Bringing the page up runs shown(), which asks for what the tab shows.
    ui_shell_go("catalog");
}

const UiScreenDef UI_SCREEN_CATALOG = {"catalog", UI_SYM_RADIO, build, refresh, shown, back};
