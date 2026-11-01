/**
 * @file ui_helpers.cpp
 * @brief The widget helpers declared in ui_common.h.
 */

#include "ui_common.h"
#include "spectrum.h"
#include <cstring>

namespace uih {

void bare(lv_obj_t* o) {
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_outline_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t* box(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t* o = lv_obj_create(parent);
    bare(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    return o;
}

lv_obj_t* rect(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
               uint32_t color, int32_t radius) {
    lv_obj_t* o = box(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, ui_col(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

lv_obj_t* card(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, bool on) {
    const UiTheme& t = ui_theme();
    lv_obj_t* o = box(parent, x, y, w, h);
    // Material depth is tonal: a card is a flat, lighter surface with a large radius, and a
    // selected one is a tonal container. No gradient, no border, no shadow — the "rich"
    // mode this helper used to draw is what the redesign replaces.
    lv_obj_set_style_bg_color(o, ui_col(on ? t.panel_on : t.panel), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, m3::shape::L, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    return o;
}

lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* txt,
                int32_t ls) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, ui_col(color), 0);
    if (ls) lv_obj_set_style_text_letter_space(l, ls, 0);
    lv_label_set_text(l, txt);
    return l;
}

lv_obj_t* icon(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* sym) {
    return label(parent, font, color, sym, 0);
}

lv_obj_t* pill(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, const char* txt,
               lv_obj_t** out_lbl) {
    const UiTheme& t = ui_theme();
    // An outlined assist chip: hairline in outline_var, small radius, no letter-spacing.
    lv_obj_t* p = box(parent, x, y, w, 24);
    lv_obj_set_style_radius(p, m3::shape::XS, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, ui_col(t.hair), 0);
    lv_obj_t* l = label(p, t.f_cap, t.dim, txt, 0);
    lv_obj_center(l);
    if (out_lbl) *out_lbl = l;
    return p;
}

lv_obj_t* glow(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius) {
    // Material has no glow. The helper stays for the screens that still place one, and
    // draws nothing; it leaves with them.
    lv_obj_t* o = box(parent, x, y, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    return o;
}

void set_text(lv_obj_t* lbl, char* cache, size_t cache_sz, const char* txt) {
    if (!lbl || !txt) return;
    if (strncmp(cache, txt, cache_sz - 1) == 0) return;
    strncpy(cache, txt, cache_sz - 1);
    cache[cache_sz - 1] = '\0';
    lv_label_set_text(lbl, txt);
}

lv_obj_t* scroller(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t* o = lv_obj_create(parent);
    bare(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_AUTO);
    // No snapping: it would fight a flick through a long folder, and the elastic edge is
    // what tells a finger it has reached the end.
    lv_obj_set_scroll_snap_y(o, LV_SCROLL_SNAP_NONE);
    return o;
}

void show(lv_obj_t* o, bool visible) {
    if (!o) return;
    if (visible) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ── the spectrum row ──────────────────────────────────────────────────────────────────────

namespace {

static_assert(SPEC_BARS == audio_core::SPECTRUM_BANDS,
              "the row draws one bar per analyser band");

/// One row's state. Screens are built once at startup and never destroyed, so these live for
/// the life of the firmware: no allocation, and nothing to free. Two rows exist: the Player's
/// strip and the standby face's.
struct SpecRow {
    int32_t h     = 0;                   ///< the tallest a bar may be
    int32_t pitch = SPEC_PITCH;
    int32_t bar_w = SPEC_BAR_W;
    int16_t bar[SPEC_BARS] = {};         ///< current height of each bar, in pixels
    bool    live = false;                ///< a frame has been published at least once
};
SpecRow s_spec[2];
size_t  s_spec_n = 0;

/// Two pixels, so a silent band still reads as a band rather than vanishing from the row.
constexpr int16_t SPEC_MIN_H = 2;

void spec_draw_cb(lv_event_t* e) {
    SpecRow* row = static_cast<SpecRow*>(lv_event_get_user_data(e));
    lv_obj_t* o  = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!row || !o || !layer) return;

    lv_area_t co;
    lv_obj_get_coords(o, &co);
    const UiTheme& t = ui_theme();

    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_opa = LV_OPA_COVER;

    // The baseline first, and the bars over it — the same order, and therefore the same
    // picture, as when each of these was its own object stacked in creation order.
    dsc.bg_color = ui_col(t.hair);
    lv_area_t a = { co.x1, co.y2, co.x2, co.y2 };
    lv_draw_rect(layer, &dsc, &a);

    // Bars in primary, the loud ones in tertiary — the design's spectrum card, where a peak
    // is a change of tone rather than an alarm.
    dsc.radius = 4;
    const int32_t loud_from = row->h * 82 / 100;
    const uint32_t loud = m3::theme().c.tertiary;
    for (int32_t i = 0; i < SPEC_BARS; i++) {
        const int32_t h = row->bar[i];
        dsc.bg_color = ui_col(!row->live ? t.hair : (h > loud_from ? loud : t.acc));
        a.x1 = co.x1 + i * row->pitch;
        a.x2 = a.x1 + row->bar_w - 1;
        a.y2 = co.y2;
        a.y1 = a.y2 - h + 1;
        lv_draw_rect(layer, &dsc, &a);
    }
}

} // namespace

void spectrum_reset() { s_spec_n = 0; }

lv_obj_t* spectrum(lv_obj_t* parent, int32_t x, int32_t base_y, int32_t h, const SpecStyle& style) {
    if (s_spec_n >= sizeof(s_spec) / sizeof(s_spec[0])) return nullptr;
    SpecRow& row = s_spec[s_spec_n++];
    row = SpecRow{};
    row.h = h;
    row.pitch = style.pitch;
    row.bar_w = style.bar_w;
    for (auto& b : row.bar) b = SPEC_MIN_H;

    // The object covers the whole row INCLUDING the baseline, so the one invalidation it
    // ever asks for is the one rectangle that can change.
    lv_obj_t* o = box(parent, x, base_y - h, SPEC_BARS * style.pitch, h);
    lv_obj_set_user_data(o, &row);
    lv_obj_add_event_cb(o, spec_draw_cb, LV_EVENT_DRAW_MAIN, &row);
    return o;
}

void spectrum_set(lv_obj_t* o, const float* db) {
    if (!o) return;
    SpecRow* row = static_cast<SpecRow*>(lv_obj_get_user_data(o));
    if (!row) return;

    if (!db) {
        // Nothing has been published. The row stays as it is; the draw callback paints it in
        // the hairline colour for as long as `live` is false.
        return;
    }

    bool moved = !row->live;
    row->live = true;
    for (int32_t i = 0; i < SPEC_BARS; i++) {
        // floor..band reference → 0..h. The top of the row is SPECTRUM_BAND_REF_DB, not
        // 0 dBFS: referencing a BAND display to the level of the WHOLE signal is what left
        // loud material pinned against the top.
        constexpr float SPAN = audio_core::SPECTRUM_BAND_REF_DB
                             - audio_core::SPECTRUM_FLOOR_DB;
        float u = (db[i] - audio_core::SPECTRUM_FLOOR_DB) / SPAN;
        if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
        int16_t h = (int16_t)(u * (float)row->h);
        if (h < SPEC_MIN_H) h = SPEC_MIN_H;
        if (h == row->bar[i]) continue;
        row->bar[i] = h;
        moved = true;
    }
    // One invalidation for the whole row, and none at all on a frame where nothing moved —
    // which is every frame of silence, and the reason a paused screen costs nothing.
    if (moved) lv_obj_invalidate(o);
}

} // namespace uih
