/**
 * @file screen_m3test.cpp
 * @brief "M3" — every component of ui_m3_widgets.h on one page, for the bench.
 *
 * What it is for: a screenshot of this page, held against the design package component by
 * component. It is a development page — CONFIG_UI_M3_TEST_SCREEN puts it in the menu in place
 * of the service screen, and a shipping build never has it. The rail and the header around it
 * are the shell's own.
 *
 * Nothing here is wired to state. The slider moves when dragged and the chips toggle when
 * tapped, because a component that cannot be touched cannot be judged as a touch target.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include <cstdio>

#if CONFIG_UI_M3_TEST_SCREEN

namespace {

struct Page {
    m3w::Slider slider;
    m3w::Chip   chips[3];
    lv_obj_t*   toast = nullptr;
    lv_obj_t*   wave  = nullptr;
    float       phase = 0.0f;
    uint32_t    toast_at = 0;
};
Page g;

void slider_cb(lv_event_t*) {
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(g.slider.obj, &a);
    m3w::slider_set(g.slider, m3w::slider_value_at(g.slider, p.x - a.x1));
}

void chip_cb(lv_event_t* e) {
    lv_obj_t* tgt = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (auto& c : g.chips) if (c.obj == tgt) m3w::chip_set(c, !c.selected);
}

void toast_cb(lv_event_t*) {
    m3w::toast_show(g.toast, "Джерело: Радіо");
    g.toast_at = lv_tick_get();
}

lv_obj_t* build(lv_obj_t* parent, UiCtx&) {
    const auto& t = m3::theme();
    g = Page{};
    lv_obj_t* root = m3w::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);

    // ── left: type, chips, surfaces, the wave and the slider ──
    struct Sample { const lv_font_t* f; uint32_t col; const char* s; int32_t y; };
    const Sample SAMPLES[] = {
        { t.t.display,  t.c.on_surface,     "Maria También",                       0 },
        { t.t.title_m,  t.c.on_surface_var, "Khruangbin — Con Todo El Mundo",     60 },
        { t.t.headline, t.c.on_surface,     "Заголовок 32",                       94 },
        { t.t.body_l,   t.c.on_surface,     "Основний текст 20 — кирилиця",      136 },
        { t.t.label_m,  t.c.on_surface_var, "Підпис 18 · 500",                   164 },
        { t.t.label_s,  t.c.outline,        "Найменший підпис 16",               190 },
    };
    for (const auto& s : SAMPLES) {
        lv_obj_t* l = m3w::text(root, s.f, s.col, s.s);
        lv_obj_set_pos(l, 0, s.y);
    }

    const char* const CHIP[3] = { "FLAC", "44.1 › 48 kHz", "PEQ" };
    int32_t cx = 0;
    for (int i = 0; i < 3; i++) {
        g.chips[i] = m3w::chip(root, cx, 218, CHIP[i], i == 2);
        lv_obj_add_event_cb(g.chips[i].obj, chip_cb, LV_EVENT_CLICKED, nullptr);
        cx += m3w::chip_width(g.chips[i]) + 8;
    }

    const uint32_t LEVELS[5] = { t.c.surface_lowest, t.c.surface_low, t.c.surface_container,
                                 t.c.surface_high, t.c.surface_highest };
    for (int i = 0; i < 5; i++) m3w::surface(root, i * 40, 260, 32, 32, LEVELS[i], m3::shape::S);
    m3w::surface(root, 5 * 40 + 8, 260, 32, 32, t.c.primary_container, m3::shape::S);
    m3w::surface(root, 6 * 40 + 8, 260, 32, 32, t.c.secondary_container, m3::shape::S);
    m3w::surface(root, 7 * 40 + 8, 260, 32, 32, t.c.tertiary_container, m3::shape::S);

    g.wave = m3w::wave(root, 0, 300, 300);
    m3w::wave_set(g.wave, 0.62f, 0.0f, true);
    g.slider = m3w::slider(root, 0, 328, 340, UI_SYM_VOLUME);
    m3w::slider_set(g.slider, 62);
    lv_obj_add_event_cb(g.slider.obj, slider_cb, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(g.slider.obj, slider_cb, LV_EVENT_CLICKED, nullptr);

    // ── right: buttons, switches, rows — 312 px, ending at the content's edge ──
    const int32_t c3 = uig::CONT_W - 312;
    m3w::tonal_button(root, c3, 0, 64, 64, m3::shape::M, UI_SYM_PREV, t.t.icon_l_fill);
    m3w::filled_button(root, c3 + 72, 0, 104, 80, 40, UI_SYM_PAUSE, t.t.icon_xl_fill);
    m3w::tonal_button(root, c3 + 184, 0, 64, 64, m3::shape::M, UI_SYM_NEXT, t.t.icon_l_fill);
    m3w::Button fav = m3w::icon_button(root, c3 + 256, 4, 56, UI_SYM_HEART, t.t.icon_l, true);
    lv_obj_add_event_cb(fav.obj, toast_cb, LV_EVENT_CLICKED, nullptr);

    m3w::Button off = m3w::tonal_button(root, c3, 92, 148, 44, m3::shape::XS, UI_SYM_POWER, t.t.icon_s, "Вимк");
    m3w::button_enable(off, false);
    m3w::filled_button(root, c3 + 156, 92, 156, 44, m3::shape::XS, UI_SYM_CHECK, t.t.icon_s, "Зберегти");

    m3w::switch_m3(root, c3, 148, true);
    m3w::switch_m3(root, c3 + 64, 148, false);

    lv_obj_t* col = m3w::surface(root, c3, 192, 312, 2 * m3w::ROW_H + 12, t.c.surface_low, m3::shape::L);
    m3w::list_row(col, 0, 6, 312, UI_SYM_BRIGHTNESS, "Яскравість", "підсвітка ST7701", "80 %");
    m3w::Row r2 = m3w::list_row(col, 0, 6 + m3w::ROW_H, 312, UI_SYM_PALETTE, "Колір з обкладинки", "динамічна схема", nullptr);
    m3w::switch_m3(r2.obj, 312 - 16 - 52, (m3w::ROW_H - 32) / 2, true);

    g.toast = m3w::toast(parent);
    return root;
}

void refresh(lv_obj_t*, UiCtx&) {
    // The wave moves at the refresh rate; a real screen moves it only while the radio plays.
    g.phase += 0.35f;
    m3w::wave_set(g.wave, 0.62f, g.phase, true);
    if (g.toast_at && lv_tick_elaps(g.toast_at) > 1600) {
        g.toast_at = 0;
        m3w::show(g.toast, false);
    }
}

} // namespace

const UiScreenDef UI_SCREEN_M3TEST = {"m3test", UI_SYM_PALETTE, build, refresh, nullptr, nullptr};

#endif // CONFIG_UI_M3_TEST_SCREEN
