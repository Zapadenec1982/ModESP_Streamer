/**
 * @file screen_xover.cpp
 * @brief Налаштування → Кросовер: the ways the outputs can be used, and which of them play
 *        (the mock-up V2_Panel_Xover).
 *
 * The schemes as a list on the left — Стерео, Стерео + сабвуфер, Бі-ампінг, 4 колонки — and what
 * the one that plays means on the right. A scheme is offered only once the firmware says it plays
 * it (audio.out_caps, out_caps.h); the others say «ще не підтримується» and take no finger. This
 * board has one stereo pair, so the page says that no crossover is needed: the schemes' own
 * controls come with the four-channel DAC plan. Nothing here is set, so the knob is the volume's.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "out_caps.h"

namespace {

using i18n::S;
using i18n::tr;

// ── geometry, relative to the 752x384 content column ──
constexpr int32_t LIST_W  = 220;
constexpr int32_t CARD_H  = 88;
constexpr int32_t CARD_GAP = 8;
constexpr int32_t RIGHT_X = LIST_W + 16;
constexpr int32_t RIGHT_W = uig::CONT_W - RIGHT_X;
constexpr int     N_SCHEMES = 4;
static_assert(N_SCHEMES * CARD_H + (N_SCHEMES - 1) * CARD_GAP <= uig::CONT_H, "the four fit the column");

struct Scheme { int32_t bit; S name; };
constexpr Scheme SCHEMES[N_SCHEMES] = {
    {audio_core::OUT_STEREO, S::xover_stereo},
    {audio_core::OUT_SUB,    S::xover_sub},
    {audio_core::OUT_BIAMP,  S::xover_biamp},
    {audio_core::OUT_FOUR,   S::xover_four},
};

struct Page {
    lv_obj_t* card[N_SCHEMES] = {};
    lv_obj_t* name[N_SCHEMES] = {};
    lv_obj_t* sub[N_SCHEMES]  = {};
    int32_t   caps = -1;                 ///< what the cards were painted for
};
Page g;

/// The cards as the firmware's word has them: the one that plays ringed, those that do not play
/// dimmed and saying so. Stereo plays until the outputs' profile says another (lib/xover.js).
void paint(int32_t caps) {
    const auto& t = m3::theme();
    if (caps <= 0) caps = audio_core::OUT_STEREO;
    if (caps == g.caps) return;
    g.caps = caps;
    for (int i = 0; i < N_SCHEMES; i++) {
        const bool plays = SCHEMES[i].bit == audio_core::OUT_STEREO;
        const bool supported = (caps & SCHEMES[i].bit) != 0;
        lv_obj_set_style_border_width(g.card[i], plays ? 2 : 0, 0);
        lv_obj_set_style_bg_color(g.card[i], m3::col(plays ? t.c.surface_container : t.c.surface_low), 0);
        // dimmed whole, as the web dims the card: the line under the name was the brighter of the two
        lv_obj_set_style_text_color(g.name[i], m3::col(supported ? t.c.on_surface : t.c.outline), 0);
        lv_obj_set_style_text_color(g.sub[i], m3::col(supported ? t.c.on_surface_var : t.c.outline), 0);
        // a scheme that plays needs no line under it; one that does not says so
        lv_label_set_text(g.sub[i], supported ? "" : tr(S::xover_unsupported));
    }
}

lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    g = Page{};
    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);

    for (int i = 0; i < N_SCHEMES; i++) {
        lv_obj_t* card = m3w::surface(root, 0, i * (CARD_H + CARD_GAP), LIST_W, CARD_H, t.c.surface_low, m3::shape::L);
        lv_obj_set_style_border_color(card, m3::col(t.c.primary), 0);
        lv_obj_t* n = m3w::text(card, t.t.label_m, t.c.on_surface, tr(SCHEMES[i].name));
        lv_obj_set_width(n, LIST_W - 32);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(n, 16, 18);
        lv_obj_t* s = m3w::text(card, t.t.label_s, t.c.on_surface_var, "");
        lv_obj_set_width(s, LIST_W - 32);
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(s, 16, 48);
        g.card[i] = card;
        g.name[i] = n;
        g.sub[i] = s;
    }

    // what the scheme that plays means — for stereo, that it needs no crossover: a state, not a
    // lesson
    lv_obj_t* note = m3w::surface(root, RIGHT_X, 0, RIGHT_W, LV_SIZE_CONTENT, t.c.surface_low, m3::shape::L);
    lv_obj_set_style_pad_all(note, 24, 0);
    m3w::text(note, t.t.title_m, t.c.on_surface, tr(S::xover_stereo_title));

    paint(c.s.out_caps);
    return root;
}

void refresh(lv_obj_t*, UiCtx& c) { paint(c.s.out_caps); }

}  // namespace

const UiScreenDef UI_SCREEN_XOVER = {"xover", UI_SYM_SPLIT, build, refresh, nullptr, nullptr};
