/**
 * @file screen_src.cpp
 * @brief "Джерело" — what to listen to: one card per source this build can play.
 *
 * One grid, the most listened to first (customer, 18.10.2026): Bluetooth, AirPlay and the radio
 * on the top row, the card, the media servers and USB under them. Each card only when its bit
 * is in audio.sources — bindings decide what exists, and a card for hardware that is not wired
 * is worse than a shorter page (plan §1.4). Off, the tone generator and AUX have no card: the
 * first two are not something anyone listens to, AUX left the hardware (15.10.2026), and all
 * three stay reachable through the API.
 *
 * Each card says what it would give you right now — the phone, the station, how many tracks,
 * what is playing — so the page answers "what is there" without a tap. A tap is choosing to
 * listen, and lands on the Player. «Медіасервери» opens their browser, where a track is chosen
 * on a server. DLNA has no card of its own: an app starts it and the panel follows by itself
 * (dlna_module.cpp), and while a pushed track plays, the servers' card is the one that shows it.
 * The lists — the stations, the catalog, the card's library — are the Player's, for the source
 * that plays.
 *
 * The presets are not here: a preset chooses a station, not a source — they are on the
 * Stations page (plan decision 7.11).
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "playlist.h"
#include "stations.h"
#include <cstdio>
#include <cstring>

namespace {

using i18n::P;
using i18n::S;
using i18n::tr;
using modesp::gen::AUDIO_SOURCE_RADIO;

enum Card : int { BT, AIRPLAY, RADIO, FILE_, SERVERS, USB, N_CARD };

struct CardDef { UiSrc src; S name; const char* sym; };
/// A card for `src`, named and drawn as the glass names and draws that source (ui_src_def).
constexpr CardDef card_of(UiSrc src) { return { src, ui_src_def(src).name, ui_src_def(src).sym }; }
/// Order is the order on screen. A card exists when audio.sources has the bit of the audio.source
/// value its source plays from (ui_src_value).
/// The names are the cards' own (src.*), short enough for a card's headline in every language:
/// «Serwery multimediów», the DLNA page's Polish, ran out of the card. The media servers' card
/// plays DLNA, which rides the radio's ring — it exists when the radio does — but it is the way
/// into the servers rather than a push, so it keeps their name and their icon where the Player
/// says «DLNA».
/// USB took AUX's place (the customer, 24.10.2026): the line input left the hardware on
/// 15.10.2026 and its card was never shown since, and the grid is two rows of three.
constexpr CardDef CARDS[N_CARD] = {
    card_of(UiSrc::BT),
    card_of(UiSrc::AIRPLAY),
    card_of(UiSrc::RADIO),
    card_of(UiSrc::CARD),
    { UiSrc::DLNA, S::src_servers, UI_SYM_HUB },
    card_of(UiSrc::USB),
};

// ── geometry: a 3x2 grid over the whole content column ──
constexpr int32_t COLS  = 3;
constexpr int32_t GAP   = 12;
constexpr int32_t CW    = (uig::CONT_W - (COLS - 1) * GAP) / COLS;     // 242
constexpr int32_t CH    = (uig::CONT_H - GAP) / 2;                      // 186
static_assert(N_CARD <= 2 * COLS, "every card fits the two rows: the page does not scroll");
/// Inside a card: the icon at the top, the name and two lines under it at the bottom. The
/// text is a step above the design's (title_m / label_m): the customer found it too small to
/// read from the sofa (11.10.2026), so the name is headline and the lines title_m, and the
/// margins give back the few pixels that costs.
constexpr int32_t IN    = 16;           ///< the card's top and bottom margin

struct Page {
    UiCtx*    ctx = nullptr;
    lv_obj_t* card[N_CARD]  = {};
    lv_obj_t* ico[N_CARD]   = {};
    lv_obj_t* mark[N_CARD]  = {};    ///< the check on the card that is playing
    lv_obj_t* name[N_CARD]  = {};
    lv_obj_t* sub[N_CARD]   = {};
    int       order[N_CARD] = {};    ///< visible cards, in screen order
    int       n_shown = 0;
    int32_t   sources = -1;
    int       active = -2;
    char      sub_txt[N_CARD][96] = {};
};

Page g;

void post(UiLvglModule::Req kind, int32_t v) {
    if (g.ctx && g.ctx->mod) g.ctx->mod->lvgl_post(kind, v);
}

int active_card(const UiLvglModule::Snapshot& s) {
    const UiSrc playing = ui_src(s);
    for (int c = 0; c < N_CARD; c++)
        if (CARDS[c].src == playing) return c;
    return -1;                                   // off, the tone, AUX: no card
}

/// Choose card `c`: take the output for it and go where it is heard.
void choose(int c) {
    if (!g.ctx) return;
    const auto& s = g.ctx->s;
    const bool active = active_card(s) == c;
    switch (c) {
    case BT:
    case FILE_:
    case AIRPLAY:
    case USB:
        if (!active) post(UiLvglModule::Req::SOURCE, ui_src_value(CARDS[c].src));
        break;
    case RADIO:
        // The source first — the radio drops what it is sent while it does not own the
        // output — then play the station it is tuned to.
        if (s.source != AUDIO_SOURCE_RADIO) post(UiLvglModule::Req::SOURCE, AUDIO_SOURCE_RADIO);
        if (!active || strcmp(s.radio_state, "idle") == 0) post(UiLvglModule::Req::RADIO_CMD, 1);
        break;
    case SERVERS:
        // Nothing to switch to until a track is chosen on a server: the browser is where that
        // happens. Once a track is pushed — from there or from an app — the card leads to it
        // like the others.
        if (!active) { ui_shell_go("dlna"); return; }
        break;
    default: return;
    }
    ui_shell_show_player();
}

void card_cb(lv_event_t* e) { choose((int)(intptr_t)lv_event_get_user_data(e)); }

/// What the card would give you now — one line, the thing a glance should catch.
void describe(int c, const UiLvglModule::Snapshot& s, char* out, size_t n) {
    const bool active = active_card(s) == c;
    switch (c) {
    case BT:
        if (active && s.play_title[0])  snprintf(out, n, "%s", s.play_title);
        else snprintf(out, n, "%s", tr(s.bt_connected ? S::srcsub_phone_on : S::srcsub_phone_off));
        break;
    case SERVERS:
        // the catalog keeps a no-break space after "у" (and the Polish "w"): the preposition
        // goes to the next line with its word, not alone at the end of this one
        if (active && s.play_title[0]) snprintf(out, n, "%s", s.play_title);
        else snprintf(out, n, "%s", tr(S::srcsub_servers));
        break;
    case FILE_: {
        const int count = player_core::playlist().count();
        if (active && s.play_title[0]) snprintf(out, n, "%s", s.play_title);
        else if (count > 0) {
            char num[12];
            snprintf(num, sizeof(num), "%d", count);
            i18n::fmt(out, n, i18n::plural(P::srcsub_tracks, count), {{"n", num}});
        }
        else snprintf(out, n, "%s", tr(S::srcsub_card_none));
        break;
    }
    case RADIO: {
        if (active && s.radio_name[0]) { snprintf(out, n, "%s", s.radio_name); break; }
        radio_core::Station st;
        if (radio_core::stations().at(s.radio_station, st) && st.name[0]) { snprintf(out, n, "%s", st.name); break; }
        // No list station to name — one from the catalog played last (radio.station is -1 then):
        // how many the list has, as the web says it; «станцій немає» only when it has none.
        const int count = radio_core::stations().count();
        if (count > 0) {
            char num[12];
            snprintf(num, sizeof(num), "%d", count);
            i18n::fmt(out, n, i18n::plural(P::srcsub_stations, count), {{"n", num}});
        } else {
            snprintf(out, n, "%s", tr(S::srcsub_radio_none));
        }
        break;
    }
    case AIRPLAY:
        if (active && s.play_title[0]) snprintf(out, n, "%s", s.play_title);
        else snprintf(out, n, "%s", tr(s.ap_connected ? S::srcsub_phone_on : S::srcsub_airplay_wait));
        break;
    case USB:
        // What the computer sends, once it sends; otherwise whether there is one.
        if (active && s.play_fmt[0]) i18n::badge(out, n, s.play_fmt);
        else snprintf(out, n, "%s", tr(s.usb_connected ? S::srcsub_usb_on : S::srcsub_usb_wait));
        break;
    default: out[0] = '\0';
    }
}

/// Lay out the cards the build has, in CARDS order, three to a row.
void place(int32_t sources) {
    g.n_shown = 0;
    int slot = 0;
    for (int c = 0; c < N_CARD; c++) {
        const bool has = (sources & (1 << ui_src_value(CARDS[c].src))) != 0;
        uih::show(g.card[c], has);
        if (!has) continue;
        g.order[g.n_shown++] = c;
        lv_obj_set_pos(g.card[c], (slot % COLS) * (CW + GAP), (slot / COLS) * (CH + GAP));
        slot++;
    }
}

void paint(int active) {
    const auto& t = m3::theme();
    for (int c = 0; c < N_CARD; c++) {
        const bool on = (c == active);
        lv_obj_set_style_bg_color(g.card[c], m3::col(on ? t.c.primary_container : t.c.surface_container), 0);
        lv_obj_set_style_text_color(g.ico[c], m3::col(on ? t.c.on_primary_container : t.c.on_surface_var), 0);
        lv_obj_set_style_text_color(g.name[c], m3::col(on ? t.c.on_primary_container : t.c.on_surface), 0);
        lv_obj_set_style_text_color(g.sub[c], m3::col(on ? t.c.on_primary_container : t.c.on_surface_var), 0);
        uih::show(g.mark[c], on);
    }
}

// ── build ──

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    g = Page{};
    g.ctx = &c;
    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);
    const int32_t lh_name = lv_font_get_line_height(t.t.headline);
    const int32_t lh_sub  = lv_font_get_line_height(t.t.title_m);
    for (int i = 0; i < N_CARD; i++) {
        lv_obj_t* k = m3w::surface(root, 0, 0, CW, CH, t.c.surface_container, m3::shape::L);
        lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(k, card_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        g.card[i] = k;
        g.ico[i] = m3w::icon(k, t.t.icon_xl, t.c.on_surface_var, CARDS[i].sym);
        lv_obj_set_pos(g.ico[i], 20, IN);
        g.mark[i] = m3w::icon(k, t.t.icon_l_fill, t.c.on_primary_container, UI_SYM_CHECK_CIRCLE);
        lv_obj_align(g.mark[i], LV_ALIGN_TOP_RIGHT, -18, IN + 2);
        // two lines of what it would give you, the name above them
        g.sub[i] = m3w::text(k, t.t.title_m, t.c.on_surface_var, "");
        lv_obj_set_size(g.sub[i], CW - 40, 2 * lh_sub);
        lv_obj_set_pos(g.sub[i], 20, CH - IN - 2 * lh_sub);
        lv_label_set_long_mode(g.sub[i], LV_LABEL_LONG_DOT);
        g.name[i] = m3w::text(k, t.t.headline, t.c.on_surface, tr(CARDS[i].name));
        lv_obj_set_pos(g.name[i], 20, CH - IN - 2 * lh_sub - 4 - lh_name);
        uih::show(g.mark[i], false);
        uih::show(k, false);
    }
    return root;
}

void refresh(lv_obj_t*, UiCtx& c) {
    const auto& s = c.s;
    if (s.sources != g.sources) {
        g.sources = s.sources;
        place(s.sources);
    }
    const int a = active_card(s);
    if (a != g.active) { g.active = a; paint(a); }
    char buf[96];
    for (int i = 0; i < g.n_shown; i++) {
        const int k = g.order[i];
        describe(k, s, buf, sizeof(buf));
        uih::set_text(g.sub[k], g.sub_txt[k], sizeof(g.sub_txt[k]), buf);
    }
}

void shown(lv_obj_t* root, UiCtx& c) {
    // Laid out now rather than on the next refresh: a page that is visible for one frame
    // with no cards on it is a page a quick tap falls straight through.
    refresh(root, c);
}

} // namespace

const UiScreenDef UI_SCREEN_SRC = {"src", UI_SYM_SPEAKER, build, refresh, shown, nullptr};
