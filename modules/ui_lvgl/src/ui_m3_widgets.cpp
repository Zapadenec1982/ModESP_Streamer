/**
 * @file ui_m3_widgets.cpp
 * @brief The Material 3 components declared in ui_m3_widgets.h.
 *
 * Plain lv_obj_t containers styled once at construction, the way ui_helpers.cpp did it, and
 * for the same reason: a screen file should read as layout. The only LVGL widget used as a
 * widget is lv_switch, because it owns a state the design does not want to reinvent. The
 * wavy line draws itself in LV_EVENT_DRAW_MAIN — one object, one invalidation, the lesson of
 * the spectrum row.
 */

#include "ui_m3_widgets.h"
#include "i18n.h"
#include "fonts/ui_fonts.h"
#include "ppa_flush.h"
#include "ui_scroll_bench.h"
#include "vlist_window.h"
#include "sdkconfig.h"
#include "esp_timer.h"
#include <cmath>
#include <cstring>

namespace m3w {

namespace {

inline const m3::Theme& T() { return m3::theme(); }

/// Material's disabled content: 38 % of on_surface.
constexpr lv_opa_t OPA_DISABLED = 97;

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

void fill(lv_obj_t* o, uint32_t color, int32_t radius) {
    lv_obj_set_style_bg_color(o, m3::col(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
}

/// A button body with an optional icon and label, laid out like Material's: icon then text,
/// centred as a group; icon alone dead centre.
Button button_body(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                   uint32_t bg, uint32_t fg, const char* sym, const lv_font_t* sym_font,
                   const char* txt) {
    Button b;
    b.obj = box(parent, x, y, w, h);
    fill(b.obj, bg, radius);
    lv_obj_add_flag(b.obj, LV_OBJ_FLAG_CLICKABLE);
    if (sym && txt) {
        lv_obj_set_flex_flow(b.obj, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b.obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(b.obj, 8, 0);
        b.icon  = icon(b.obj, sym_font, fg, sym);
        b.label = text(b.obj, T().t.label_m, fg, txt);
    } else if (sym) {
        b.icon = icon(b.obj, sym_font, fg, sym);
        lv_obj_center(b.icon);
    } else if (txt) {
        b.label = text(b.obj, T().t.label_m, fg, txt);
        lv_obj_center(b.label);
    }
    return b;
}

} // namespace

// ── surfaces ───────────────────────────────────────────────────────────────────────────────

lv_obj_t* box(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t* o = lv_obj_create(parent);
    bare(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    return o;
}

lv_obj_t* surface(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t color, int32_t radius) {
    lv_obj_t* o = box(parent, x, y, w, h);
    fill(o, color, radius);
    return o;
}

lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* txt) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, m3::col(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

lv_obj_t* icon(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* sym) {
    return text(parent, font, color, sym);
}

void set_text(lv_obj_t* lbl, char* cache, size_t cache_sz, const char* txt) {
    if (!lbl || !txt) return;
    if (strncmp(cache, txt, cache_sz - 1) == 0) return;
    strncpy(cache, txt, cache_sz - 1);
    cache[cache_sz - 1] = '\0';
    lv_label_set_text(lbl, txt);
}

void show(lv_obj_t* o, bool visible) {
    if (!o) return;
    if (visible) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ── buttons ────────────────────────────────────────────────────────────────────────────────

Button filled_button(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                     int32_t radius, const char* sym, const lv_font_t* sym_font, const char* txt) {
    const auto& c = T().c;
    return button_body(parent, x, y, w, h, radius, c.primary, c.on_primary, sym, sym_font, txt);
}

Button tonal_button(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                    int32_t radius, const char* sym, const lv_font_t* sym_font, const char* txt) {
    const auto& c = T().c;
    return button_body(parent, x, y, w, h, radius, c.secondary_container,
                       c.on_secondary_container, sym, sym_font, txt);
}

Button button_text(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, const char* txt) {
    const auto& c = T().c;
    Button b = button_body(parent, x, y, w, h, m3::shape::FULL, c.surface, c.primary, nullptr, nullptr, txt);
    lv_obj_set_style_bg_opa(b.obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b.obj, m3::col(c.secondary_container), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b.obj, LV_OPA_COVER, LV_STATE_PRESSED);
    return b;
}

Button icon_button(lv_obj_t* parent, int32_t x, int32_t y, int32_t size,
                   const char* sym, const lv_font_t* sym_font, bool outlined) {
    const auto& c = T().c;
    Button b;
    b.obj = box(parent, x, y, size, size);
    lv_obj_set_style_radius(b.obj, m3::shape::FULL, 0);
    lv_obj_add_flag(b.obj, LV_OBJ_FLAG_CLICKABLE);
    if (outlined) {
        lv_obj_set_style_border_width(b.obj, 1, 0);
        lv_obj_set_style_border_color(b.obj, m3::col(c.outline_var), 0);
    }
    b.icon = icon(b.obj, sym_font, outlined ? c.primary : c.on_surface_var, sym);
    lv_obj_center(b.icon);
    return b;
}

void button_enable(Button& b, bool enabled) {
    if (!b.obj) return;
    const lv_opa_t opa = enabled ? static_cast<lv_opa_t>(LV_OPA_COVER) : OPA_DISABLED;
    if (b.icon)  lv_obj_set_style_text_opa(b.icon, opa, 0);
    if (b.label) lv_obj_set_style_text_opa(b.label, opa, 0);
    if (enabled) lv_obj_add_flag(b.obj, LV_OBJ_FLAG_CLICKABLE);
    else         lv_obj_remove_flag(b.obj, LV_OBJ_FLAG_CLICKABLE);
}

// ── chips ──────────────────────────────────────────────────────────────────────────────────

Chip chip(lv_obj_t* parent, int32_t x, int32_t y, const char* txt, bool selected) {
    const auto& t = T();
    Chip c;
    c.obj = box(parent, x, y, LV_SIZE_CONTENT, CHIP_H);
    lv_obj_set_style_radius(c.obj, m3::shape::XS, 0);
    lv_obj_set_style_border_width(c.obj, 1, 0);
    lv_obj_set_style_pad_hor(c.obj, 12, 0);
    lv_obj_set_style_pad_column(c.obj, 4, 0);
    lv_obj_set_flex_flow(c.obj, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c.obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(c.obj, LV_OBJ_FLAG_CLICKABLE);
    c.check = icon(c.obj, t.t.icon_s, t.c.on_secondary_container, UI_SYM_CHECK);
    c.label = text(c.obj, t.t.label_s, t.c.on_surface_var, txt);
    c.selected = !selected;         // so chip_set below applies the first state fully
    chip_set(c, selected);
    return c;
}

void chip_set(Chip& c, bool selected) {
    if (!c.obj || c.selected == selected) return;
    c.selected = selected;
    const auto& col = T().c;
    if (selected) {
        lv_obj_set_style_bg_color(c.obj, m3::col(col.secondary_container), 0);
        lv_obj_set_style_bg_opa(c.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(c.obj, m3::col(col.secondary_container), 0);
        lv_obj_set_style_text_color(c.label, m3::col(col.on_secondary_container), 0);
    } else {
        lv_obj_set_style_bg_opa(c.obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(c.obj, m3::col(col.outline_var), 0);
        lv_obj_set_style_text_color(c.label, m3::col(col.on_surface_var), 0);
    }
    show(c.check, selected);
}

void chip_lead(Chip& c, const char* sym) {
    if (!c.check) return;
    lv_label_set_text(c.check, sym);
    lv_obj_set_style_text_color(c.check, m3::col(c.selected ? T().c.on_secondary_container
                                                             : T().c.on_surface_var), 0);
    show(c.check, true);
}

int32_t chip_width(const Chip& c) {
    if (!c.obj) return 0;
    lv_obj_update_layout(c.obj);
    return lv_obj_get_width(c.obj);
}

// ── switch ─────────────────────────────────────────────────────────────────────────────────

static void switch_row_cb(lv_event_t* e) {
    auto* sw = static_cast<lv_obj_t*>(lv_event_get_user_data(e));
    if (!sw || lv_obj_has_state(sw, LV_STATE_DISABLED)) return;
    if (lv_obj_has_state(sw, LV_STATE_CHECKED)) lv_obj_remove_state(sw, LV_STATE_CHECKED);
    else                                         lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
}

void switch_row(lv_obj_t* row, lv_obj_t* sw) {
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, switch_row_cb, LV_EVENT_CLICKED, sw);
}

lv_obj_t* switch_m3(lv_obj_t* parent, int32_t x, int32_t y, bool on) {
    const auto& c = T().c;
    // part | state as one selector; spelled out because C++20 deprecates or-ing two enums
    constexpr lv_style_selector_t MAIN_ON = static_cast<lv_style_selector_t>(LV_PART_MAIN) | LV_STATE_CHECKED;
    constexpr lv_style_selector_t IND_ON  = static_cast<lv_style_selector_t>(LV_PART_INDICATOR) | LV_STATE_CHECKED;
    constexpr lv_style_selector_t KNOB_ON = static_cast<lv_style_selector_t>(LV_PART_KNOB) | LV_STATE_CHECKED;
    lv_obj_t* sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 52, 32);
    lv_obj_set_pos(sw, x, y);
    // Track: outlined and empty when off, filled with primary when on.
    lv_obj_set_style_bg_color(sw, m3::col(c.surface_highest), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, m3::col(c.outline), LV_PART_MAIN);
    lv_obj_set_style_radius(sw, m3::shape::FULL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, m3::col(c.primary), IND_ON);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, IND_ON);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(sw, m3::col(c.primary), MAIN_ON);
    // Knob: a small outline-coloured dot when off, a large on_primary one when on. LVGL
    // sizes the knob from the track height minus its padding, so the padding is the size.
    lv_obj_set_style_bg_color(sw, m3::col(c.outline), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, 6, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, m3::col(c.on_primary), KNOB_ON);
    lv_obj_set_style_pad_all(sw, 2, KNOB_ON);
    lv_obj_set_style_radius(sw, m3::shape::FULL, LV_PART_KNOB);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_ext_click_area(sw, 16);            // a 32-px switch takes a 64-px finger
    return sw;
}

// ── slider ─────────────────────────────────────────────────────────────────────────────────

Slider slider(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, const char* lead_sym, const char* unit) {
    const auto& t = T();
    constexpr int32_t GAP = 12;
    Slider s;
    s.unit = unit ? unit : "";
    s.obj = box(parent, x, y, w, SLIDER_H);
    lv_obj_add_flag(s.obj, LV_OBJ_FLAG_CLICKABLE);
    if (lead_sym) {
        s.lead = icon(s.obj, t.t.icon_m, t.c.on_surface_var, lead_sym);
        lv_obj_align(s.lead, LV_ALIGN_LEFT_MID, 0, 0);
        s.r0 = 24 + GAP;
    }
    const int32_t r1 = w - SLIDER_VAL - GAP;
    s.k0 = s.r0 + SLIDER_KNOB / 2;
    s.k1 = r1 - SLIDER_KNOB / 2;
    const int32_t ry = (SLIDER_H - SLIDER_RAIL) / 2;
    s.rail = surface(s.obj, s.r0, ry, r1 - s.r0, SLIDER_RAIL, t.c.surface_highest, SLIDER_RAIL / 2);
    s.fill = surface(s.obj, s.r0, ry, SLIDER_RAIL, SLIDER_RAIL, t.c.primary, SLIDER_RAIL / 2);
    s.knob = surface(s.obj, 0, (SLIDER_H - SLIDER_KNOB) / 2, SLIDER_KNOB, SLIDER_KNOB, t.c.primary,
                     LV_RADIUS_CIRCLE);
    s.value = text(s.obj, t.t.label_m, t.c.on_surface_var, "0");
    lv_obj_set_width(s.value, SLIDER_VAL);
    lv_obj_set_style_text_align(s.value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s.value, LV_ALIGN_RIGHT_MID, 0, 0);
    slider_set(s, 0);
    return s;
}

/// The knob's centre for `pct`, in the row.
static int32_t knob_centre(const Slider& s, int32_t pct) {
    return s.k0 + (s.k1 - s.k0) * pct / 100;
}

void slider_set(Slider& s, int32_t pct) {
    if (!s.obj) return;
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    if (pct == s.pct) return;
    s.pct = pct;
    const int32_t c = knob_centre(s, pct);
    lv_obj_set_width(s.fill, c - s.r0);
    lv_obj_set_x(s.knob, c - SLIDER_KNOB / 2);
    lv_label_set_text_fmt(s.value, "%d%s", (int)pct, s.unit);
}

int32_t slider_value_at(const Slider& s, int32_t local_x) {
    if (s.k1 <= s.k0) return 0;
    int32_t pct = (local_x - s.k0) * 100 / (s.k1 - s.k0);
    if (pct < 0) pct = 0; else if (pct > 100) pct = 100;
    return pct;
}

Slid slide_touch(lv_event_t* e, touch::Slide& gesture, int32_t knob_x, int32_t& value, int32_t range,
                 int32_t span) {
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN_VER);     // the page may scroll from it again
        return gesture.end() == touch::Slide::DRAG ? Slid::END : Slid::NONE;
    }
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return Slid::NONE;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        gesture.down(p.x, p.y, knob_x, value);
        return Slid::NONE;
    }
    if (code != LV_EVENT_PRESSING) return Slid::NONE;
    // LVGL sends PRESSING to the pressed box while its parent scrolls, too
    if (lv_indev_get_scroll_obj(indev)) {
        gesture.scrolled();
        return Slid::NONE;
    }
    const touch::Slide::State was = gesture.state();
    if (gesture.move(p.x, p.y) != touch::Slide::DRAG) return Slid::NONE;
    // The slider's now. Without the chain LVGL finds nothing to scroll from this box, so a drift
    // up or down while it drags stays a drag — what lv_slider does for itself.
    if (was != touch::Slide::DRAG) lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    value = gesture.follow(p.x, range, span);
    return Slid::MOVE;
}

Slid slider_touch(lv_event_t* e, Slider& s, touch::Slide& gesture, int32_t& pct) {
    lv_area_t a;
    lv_obj_get_coords(s.obj, &a);
    pct = s.pct;
    const Slid r = slide_touch(e, gesture, a.x1 + knob_centre(s, s.pct), pct, 100, s.k1 - s.k0);
    if (r == Slid::MOVE) pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return r;
}

Slid knob_touch(lv_event_t* e, touch::Slide& gesture, int32_t& value) {
    auto* sl = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_event_stop_processing(e);
    const int32_t lo = lv_slider_get_min_value(sl), hi = lv_slider_get_max_value(sl);
    lv_area_t a;
    lv_obj_get_coords(sl, &a);
    const int32_t w = lv_area_get_width(&a);
    value = lv_slider_get_value(sl);
    const int32_t knob_x = a.x1 + (hi > lo ? (value - lo) * w / (hi - lo) : 0);
    const Slid r = slide_touch(e, gesture, knob_x, value, hi - lo, w);
    if (r == Slid::MOVE) value = value < lo ? lo : value > hi ? hi : value;
    return r;
}

// ── lists ──────────────────────────────────────────────────────────────────────────────────

Row list_row(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, const char* sym,
             const char* title, const char* sub, const char* value) {
    const auto& t = T();
    Row r;
    r.obj = box(parent, x, y, w, ROW_H);
    lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
    if (sym) {
        r.icon = icon(r.obj, t.t.icon_m, t.c.on_surface_var, sym);
        lv_obj_align(r.icon, LV_ALIGN_LEFT_MID, 16, 0);
    }
    const int32_t tx = sym ? 56 : 16;
    r.title = text(r.obj, t.t.body_l, t.c.on_surface, title);
    lv_obj_set_pos(r.title, tx, sub ? 8 : 20);
    if (sub) {
        r.sub = text(r.obj, t.t.label_s, t.c.on_surface_var, sub);
        lv_obj_set_pos(r.sub, tx, 36);
        // One line, ended with an ellipsis: LVGL only truncates a label that has a fixed
        // height; with the default auto height it wraps instead.
        lv_obj_set_size(r.sub, w - tx - 96, lv_font_get_line_height(t.t.label_s));
        lv_label_set_long_mode(r.sub, LV_LABEL_LONG_DOT);
    }
    if (value) {
        r.value = text(r.obj, t.t.label_m, t.c.on_surface_var, value);
        lv_obj_align(r.value, LV_ALIGN_RIGHT_MID, -16, 0);
    }
    return r;
}

// ── the long list ──────────────────────────────────────────────────────────────────────────

namespace {
constexpr int32_t VROW_GAP   = 4;   ///< between rows
constexpr int32_t VROW_INSET = 4;   ///< left and right

// LVGL applies a local style whether or not it changes anything, and a label answers every
// style change by laying its text out again — measuring it and placing the ellipsis
// (lv_label.c, LV_EVENT_STYLE_CHANGED → lv_label_refr_text). A row refilled with the colours
// it already had paid that on every label; these skip the call when the value is already set.
void text_color_if(lv_obj_t* o, uint32_t rgb) {
    const lv_color_t c = m3::col(rgb);
    lv_style_value_t v;
    if (lv_obj_get_local_style_prop(o, LV_STYLE_TEXT_COLOR, &v, 0) == LV_STYLE_RES_FOUND &&
        lv_color_eq(v.color, c)) return;
    lv_obj_set_style_text_color(o, c, 0);
}

void text_opa_if(lv_obj_t* o, lv_opa_t a) {
    lv_style_value_t v;
    if (lv_obj_get_local_style_prop(o, LV_STYLE_TEXT_OPA, &v, 0) == LV_STYLE_RES_FOUND &&
        v.num == a) return;
    lv_obj_set_style_text_opa(o, a, 0);
}

void bg_opa_if(lv_obj_t* o, lv_opa_t a) {
    lv_style_value_t v;
    if (lv_obj_get_local_style_prop(o, LV_STYLE_BG_OPA, &v, 0) == LV_STYLE_RES_FOUND &&
        v.num == a) return;
    lv_obj_set_style_bg_opa(o, a, 0);
}

/// FNV-1a, never 0: 0 is a row's "nothing set yet".
uint32_t text_hash(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h ? h : 1u;
}

/// The same for the text: set it only when it is not what the label was last given.
void label_text_if(lv_obj_t* l, uint32_t& seen, const char* s) {
    const uint32_t h = text_hash(s);
    if (h == seen) return;
    seen = h;
    lv_label_set_text(l, s);
}
} // namespace

void VList::build(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                  Fill fill, Pick pick, Pick trail_pick, void* user, int32_t head_h,
                  int32_t row_h) {
    const auto& t = T();
    *this = VList{};
    fill_ = fill; pick_ = pick; trail_pick_ = trail_pick; user_ = user;
    w_ = w; h_ = h; head_h_ = head_h > 0 ? head_h : 0;
    row_h_ = row_h > 2 * VROW_GAP ? row_h : ROW_H;
    const int32_t VROW_H = row_h_ - VROW_GAP;

    scroller_ = box(parent, x, y, w, h);
    lv_obj_add_flag(scroller_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroller_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroller_, LV_SCROLLBAR_MODE_ACTIVE);
    // No snapping: it would fight a flick through a long folder, and the elastic edge is what
    // tells a finger it has reached the end.
    lv_obj_set_scroll_snap_y(scroller_, LV_SCROLL_SNAP_NONE);
    lv_obj_set_style_bg_color(scroller_, m3::col(t.c.outline), LV_PART_SCROLLBAR);
    lv_obj_set_style_width(scroller_, 4, LV_PART_SCROLLBAR);
    // LVGL marks a scroller SCROLLED from SCROLL_BEGIN to SCROLL_END, and the default theme
    // styles its scrollbar for that state, so each change of it repaints the whole list — and a
    // programmatic scroll (reveal(), scroll_top()) begins and ends on every step. The bar keeps
    // the scrolled look outright instead: it is only shown while scrolling anyway
    // (LV_SCROLLBAR_MODE_ACTIVE). Without the repaints a scroll can be a shift (ppa_flush.cpp).
    lv_obj_remove_style(scroller_, nullptr, LV_PART_SCROLLBAR | LV_STATE_SCROLLED);
    lv_obj_set_style_bg_opa(scroller_, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(scroller_, scroll_cb, LV_EVENT_SCROLL, this);

    // Invisible and one pixel wide, as tall as the whole list: what LVGL measures the
    // scrollable area against, so the list is as long as its entries however few rows exist.
    spacer_ = box(scroller_, 0, 0, 1, 1);
    if (head_h_) head_ = box(scroller_, 0, 0, w, head_h_);

    const int32_t rw = w - 2 * VROW_INSET;
    for (Row& r : row_) {
        r.obj = surface(scroller_, VROW_INSET, 0, rw, VROW_H, t.c.secondary_container, m3::shape::S);
        lv_obj_set_style_bg_opa(r.obj, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r.obj, row_cb, LV_EVENT_CLICKED, this);
        r.lead = icon(r.obj, t.t.icon_m, t.c.on_surface_var, "");
        lv_obj_align(r.lead, LV_ALIGN_LEFT_MID, 16, 0);
        r.title = text(r.obj, t.t.body_l, t.c.on_surface, "");
        lv_label_set_long_mode(r.title, LV_LABEL_LONG_DOT);
        r.sub = text(r.obj, t.t.label_s, t.c.on_surface_var, "");
        lv_label_set_long_mode(r.sub, LV_LABEL_LONG_DOT);
        if (trail_pick_) {
            Button b = icon_button(r.obj, rw - 64, (VROW_H - 64) / 2, 64, "", t.t.icon_m);
            r.trail = b.obj;
            r.trail_ic = b.icon;
            lv_obj_add_event_cb(r.trail, trail_cb, LV_EVENT_CLICKED, this);
        } else {
            r.trail = r.trail_ic = icon(r.obj, t.t.icon_m, t.c.primary, "");
            lv_obj_align(r.trail, LV_ALIGN_RIGHT_MID, -16, 0);
        }
        show(r.obj, false);
    }
}

void VList::fill_row(Row& r, int idx) {
    if (idx < 0 || idx >= count_ || !fill_) {
        show(r.obj, false);
        r.idx = -1;
        return;
    }
    VItem it;
    fill_(idx, it, user_);
    const auto& t = T();
    const int32_t rw = w_ - 2 * VROW_INSET;
    const int32_t VROW_H = row_h_ - VROW_GAP;
    r.idx = idx;
    lv_obj_set_y(r.obj, head_h_ + idx * row_h_ + VROW_GAP / 2);   // the ring fits above row 0
    lv_obj_set_user_data(r.obj, (void*)(intptr_t)idx);
    if (trail_pick_) lv_obj_set_user_data(r.trail, (void*)(intptr_t)idx);

    const uint32_t fg  = it.on ? t.c.on_secondary_container : t.c.on_surface;
    const uint32_t fg2 = it.on ? t.c.on_secondary_container : t.c.on_surface_var;
    const lv_opa_t opa = it.enabled ? static_cast<lv_opa_t>(LV_OPA_COVER) : OPA_DISABLED;
    bg_opa_if(r.obj, it.on ? LV_OPA_COVER : LV_OPA_TRANSP);

    show(r.lead, it.lead != nullptr);
    if (it.lead) {
        label_text_if(r.lead, r.seen_lead, it.lead);
        text_color_if(r.lead, fg2);
        text_opa_if(r.lead, opa);
    }
    show(r.trail, it.trail != nullptr);
    if (it.trail) label_text_if(r.trail_ic, r.seen_trail, it.trail);

    const int32_t tx  = it.lead ? 56 : 16;
    const int32_t tw  = rw - tx - (it.trail ? (trail_pick_ ? 72 : 52) : 16);
    const int32_t lh1 = lv_font_get_line_height(t.t.body_l);
    const int32_t lh2 = lv_font_get_line_height(t.t.label_s);
    const bool two = it.sub && it.sub[0];
    lv_obj_set_size(r.title, tw, lh1);              // size first: a new one lays the text out too
    label_text_if(r.title, r.seen_title, it.title ? it.title : "");
    text_color_if(r.title, fg);
    text_opa_if(r.title, opa);
    if (two) {
        const int32_t top = (VROW_H - lh1 - lh2 - 2) / 2;
        lv_obj_set_pos(r.title, tx, top);
        lv_obj_set_size(r.sub, tw, lh2);
        label_text_if(r.sub, r.seen_sub, it.sub);
        lv_obj_set_pos(r.sub, tx, top + lh1 + 2);
        text_color_if(r.sub, fg2);
        text_opa_if(r.sub, opa);
    } else {
        lv_obj_set_pos(r.title, tx, (VROW_H - lh1) / 2);
    }
    show(r.sub, two);

    if (it.enabled) lv_obj_add_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
    else            lv_obj_remove_flag(r.obj, LV_OBJ_FLAG_CLICKABLE);
    show(r.obj, true);
}

void VList::place(bool force) {
    if (!scroller_) return;
    if (count_ <= 0) {
        for (Row& r : row_) { show(r.obj, false); r.idx = -1; }
        first_ = -1;
        return;
    }
    int first = (lv_obj_get_scroll_y(scroller_) - head_h_) / row_h_ - 1;   // a row of margin above
    if (first > count_ - POOL) first = count_ - POOL;
    if (first < 0) first = 0;
    if (first == first_ && !force) return;
    first_ = first;
    // A row keeps the entry it shows while that stays in the window; only the rows whose
    // entry left are filled, with the entries that came in — one row per row of scrolling,
    // where every crossing used to refill all ten. A forced pass (the entries themselves
    // changed) refills every row in place, and fill_row skips whatever
    // is already right.
    int shown[POOL], want[POOL];
    for (int k = 0; k < POOL; k++) shown[k] = row_[k].idx;
    vlist::assign(shown, POOL, first, count_, want);
    for (int k = 0; k < POOL; k++) {
        if (force || want[k] != shown[k]) fill_row(row_[k], want[k]);
    }
}

void VList::set_count(int n) {
    if (!scroller_) return;
    ppa_flush_list_reset(scroller_);     // new entries: a shift waits for the frame that draws them
    count_ = n > 0 ? n : 0;
    lv_obj_set_height(spacer_, head_h_ + count_ * row_h_ + VROW_GAP);
    first_ = -1;
    lv_obj_scroll_to_y(scroller_, 0, LV_ANIM_OFF);
    place(true);
}

void VList::refill() { place(true); }

void VList::reveal(int idx) {
    if (!scroller_ || idx < 0 || idx >= count_) return;
    const int32_t top = head_h_ + idx * row_h_;
    const int32_t sy  = lv_obj_get_scroll_y(scroller_);
    if (top >= sy && top + row_h_ <= sy + h_) return;       // already in view
    lv_obj_scroll_to_y(scroller_, top - row_h_, LV_ANIM_OFF);
    place(true);
}

void VList::scroll_top() {
    if (!scroller_) return;
    lv_obj_scroll_to_y(scroller_, 0, LV_ANIM_OFF);
    place(true);
}

void VList::scroll_cb(lv_event_t* e) {
    auto* self = static_cast<VList*>(lv_event_get_user_data(e));
#if CONFIG_UI_SCROLL_BENCH
    const int64_t t0 = esp_timer_get_time();
    self->place(false);
    ui_scroll_bench_note_place((uint32_t)(esp_timer_get_time() - t0));
#else
    self->place(false);
#endif
    // Last, after the rows' own invalidations: what LVGL invalidates next is the scroller whole,
    // and the flush turns that into the strip that came into view (ppa_flush.cpp). The scroll
    // position grows as the content moves up; the flush wants how far it moved down.
    const int32_t sy = lv_obj_get_scroll_y(self->scroller_);
    ppa_flush_list_scrolled(self->scroller_, self->last_sy_ - sy);
    self->last_sy_ = sy;
}

// Which ENTRY, not which pool row: the same widget shows a different entry after a scroll,
// so the answer is written on it when it is filled rather than when it is made.
void VList::row_cb(lv_event_t* e) {
    auto* self = static_cast<VList*>(lv_event_get_user_data(e));
    const int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (self && self->pick_ && idx >= 0 && idx < self->count_) self->pick_(idx, self->user_);
}

void VList::trail_cb(lv_event_t* e) {
    auto* self = static_cast<VList*>(lv_event_get_user_data(e));
    const int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    if (self && self->trail_pick_ && idx >= 0 && idx < self->count_) self->trail_pick_(idx, self->user_);
}

// ── text entry ─────────────────────────────────────────────────────────────────────────────

lv_obj_t* text_field(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                     const char* placeholder, uint32_t max_len) {
    const auto& t = T();
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_pos(ta, x, y);
    lv_obj_set_size(ta, w, h);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_placeholder_text(ta, placeholder ? placeholder : "");
    lv_obj_set_scrollbar_mode(ta, LV_SCROLLBAR_MODE_OFF);
    const int32_t line = lv_font_get_line_height(t.t.title_m);
    const int32_t pad_v = (h - line) / 2 - 2;
    lv_obj_set_style_text_font(ta, t.t.title_m, 0);
    lv_obj_set_style_text_color(ta, m3::col(t.c.on_surface), 0);
    lv_obj_set_style_bg_color(ta, m3::col(t.c.surface_high), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ta, m3::shape::XS, 0);
    lv_obj_set_style_border_width(ta, 2, 0);
    lv_obj_set_style_border_color(ta, m3::col(t.c.primary), 0);
    lv_obj_set_style_outline_width(ta, 0, 0);
    lv_obj_set_style_shadow_width(ta, 0, 0);
    lv_obj_set_style_pad_hor(ta, 16, 0);
    lv_obj_set_style_pad_ver(ta, pad_v > 0 ? pad_v : 0, 0);
    lv_obj_set_style_text_color(ta, m3::col(t.c.on_surface_var), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_border_color(ta, m3::col(t.c.primary), LV_PART_CURSOR);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(ta, 500, LV_PART_CURSOR);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);   // the cursor blinks only on a focused field
    return ta;
}

namespace {

// The keys that do something other than type themselves. The space bar's cap is a word of the
// panel's language: the maps below hold a placeholder for it, and each keyboard keeps its own
// copies with the word put in (KbState) — the copies live in LVGL's heap, in PSRAM.
constexpr const char* KEY_SIGNS   = "#+=";
constexpr const char* KEY_LETTERS = "abc";
constexpr const char* KEY_SPACE   = "\x01";        // placeholder: never a key's own text

enum : uint8_t { KB_LOWER, KB_UPPER, KB_SIGNS };

const char* const MAP_LOWER[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    UI_SYM_SHIFT, "z", "x", "c", "v", "b", "n", "m", UI_SYM_BACKSPACE, "\n",
    KEY_SIGNS, "-", "_", KEY_SPACE, ".", "@", "",
};
const char* const MAP_UPPER[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    UI_SYM_SHIFT, "Z", "X", "C", "V", "B", "N", "M", UI_SYM_BACKSPACE, "\n",
    KEY_SIGNS, "-", "_", KEY_SPACE, ".", "@", "",
};
// Every ASCII sign: ten, ten, nine, and the last three beside the space.
const char* const MAP_SIGNS[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "!", "@", "#", "$", "%", "^", "&", "*", "(", ")", "\n",
    "-", "_", "=", "+", "[", "]", "{", "}", "\\", "|", "\n",
    "~", "`", ";", ":", "'", "\"", ",", ".", "/", UI_SYM_BACKSPACE, "\n",
    KEY_LETTERS, "<", ">", KEY_SPACE, "?", "",
};

// A key types on release (CLICK_TRIG), so a finger sliding across the board types nothing
// it did not lift from; backspace alone repeats while held.
constexpr lv_buttonmatrix_ctrl_t K  = (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG);
constexpr lv_buttonmatrix_ctrl_t BS = (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 3);
constexpr lv_buttonmatrix_ctrl_t W2 = (lv_buttonmatrix_ctrl_t)(K | 2);
constexpr lv_buttonmatrix_ctrl_t W3 = (lv_buttonmatrix_ctrl_t)(K | 3);
constexpr lv_buttonmatrix_ctrl_t W8 = (lv_buttonmatrix_ctrl_t)(K | 8);

const lv_buttonmatrix_ctrl_t CTRL_LETTERS[] = {
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K,
    W3, W2, W2, W2, W2, W2, W2, W2, BS,
    W3, W2, W2, W8, W2, W3,
};
const lv_buttonmatrix_ctrl_t CTRL_SIGNS[] = {
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K,
    W2, W2, W2, W2, W2, W2, W2, W2, W2, BS,
    W3, W2, W2, W8, W2,
};

/// Index of the shift key in the letter maps: the first key of the fourth row.
constexpr uint32_t SHIFT_ID = 10 + 10 + 9;

constexpr size_t N_LOWER = sizeof(MAP_LOWER) / sizeof(MAP_LOWER[0]);
constexpr size_t N_UPPER = sizeof(MAP_UPPER) / sizeof(MAP_UPPER[0]);
constexpr size_t N_SIGNS = sizeof(MAP_SIGNS) / sizeof(MAP_SIGNS[0]);

struct KbState {
    lv_obj_t*   target = nullptr;
    uint8_t     page   = KB_LOWER;
    const char* lower[N_LOWER];          ///< the maps with the space bar's word in
    const char* upper[N_UPPER];
    const char* signs[N_SIGNS];
};

/// `out` = `in`, the placeholder replaced by the space bar's word.
void localise(const char* const* in, const char** out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = in[i] == KEY_SPACE ? i18n::tr(i18n::S::kb_space) : in[i];
}

/// The key caps' face: the text face with the icon face behind it, so the shift and backspace
/// glyphs draw from the same button matrix as the letters. A copy, because the generated
/// faces are const; it points at the same glyph data.
lv_font_t s_kb_font;

void kb_page(lv_obj_t* kb, uint8_t page) {
    auto* st = static_cast<KbState*>(lv_obj_get_user_data(kb));
    if (!st) return;
    st->page = page;
    if (page == KB_SIGNS) {
        lv_buttonmatrix_set_map(kb, st->signs);
        lv_buttonmatrix_set_ctrl_map(kb, CTRL_SIGNS);
    } else {
        lv_buttonmatrix_set_map(kb, page == KB_UPPER ? st->upper : st->lower);
        lv_buttonmatrix_set_ctrl_map(kb, CTRL_LETTERS);
        if (page == KB_UPPER) lv_buttonmatrix_set_button_ctrl(kb, SHIFT_ID, LV_BUTTONMATRIX_CTRL_CHECKED);
    }
}

void kb_cb(lv_event_t* e) {
    lv_obj_t* kb = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* st = static_cast<KbState*>(lv_obj_get_user_data(kb));
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        lv_free(st);
        lv_obj_set_user_data(kb, nullptr);
        return;
    }
    if (!st) return;
    const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char* key = lv_buttonmatrix_get_button_text(kb, id);
    if (!key) return;
    if (strcmp(key, UI_SYM_SHIFT) == 0)   { kb_page(kb, st->page == KB_UPPER ? KB_LOWER : KB_UPPER); return; }
    if (strcmp(key, KEY_SIGNS) == 0)      { kb_page(kb, KB_SIGNS); return; }
    if (strcmp(key, KEY_LETTERS) == 0)    { kb_page(kb, KB_LOWER); return; }
    if (!st->target) return;
    if (strcmp(key, UI_SYM_BACKSPACE) == 0) { lv_textarea_delete_char(st->target); return; }
    lv_textarea_add_text(st->target, strcmp(key, i18n::tr(i18n::S::kb_space)) == 0 ? " " : key);
}

} // namespace

lv_obj_t* keyboard(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, lv_obj_t* target) {
    const auto& t = T();
    s_kb_font = *t.t.title_m;
    s_kb_font.fallback = t.t.icon_m;

    lv_obj_t* kb = lv_buttonmatrix_create(parent);
    lv_obj_set_pos(kb, x, y);
    lv_obj_set_size(kb, w, h);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_outline_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_set_style_pad_row(kb, 6, 0);
    lv_obj_set_style_pad_column(kb, 6, 0);

    lv_obj_set_style_bg_color(kb, m3::col(t.c.surface_high), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, m3::shape::XS, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_outline_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &s_kb_font, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, m3::col(t.c.on_surface), LV_PART_ITEMS);
    // part | state as one selector, spelled out for the same reason as in switch_m3
    constexpr lv_style_selector_t ITEMS = static_cast<lv_style_selector_t>(LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, m3::col(t.c.secondary_container), ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, m3::col(t.c.on_secondary_container), ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, m3::col(t.c.primary), ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, m3::col(t.c.on_primary), ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(kb, 0, ITEMS | LV_STATE_FOCUS_KEY);

    auto* st = static_cast<KbState*>(lv_malloc(sizeof(KbState)));
    if (st) {
        st->target = target;
        st->page   = KB_LOWER;
        localise(MAP_LOWER, st->lower, N_LOWER);
        localise(MAP_UPPER, st->upper, N_UPPER);
        localise(MAP_SIGNS, st->signs, N_SIGNS);
    }
    lv_obj_set_user_data(kb, st);
    kb_page(kb, KB_LOWER);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_DELETE, nullptr);
    mark_modal(kb);
    return kb;
}

void keyboard_target(lv_obj_t* kb, lv_obj_t* target) {
    auto* st = kb ? static_cast<KbState*>(lv_obj_get_user_data(kb)) : nullptr;
    if (st) st->target = target;
}

void keyboard_reset(lv_obj_t* kb) {
    if (kb) kb_page(kb, KB_LOWER);
}

// ── dialog ─────────────────────────────────────────────────────────────────────────────────

Dialog dialog(lv_obj_t* parent, int32_t w, int32_t h) {
    const auto& t = T();
    Dialog d;
    d.scrim = box(parent, 0, 0, lv_obj_get_width(parent), lv_obj_get_height(parent));
    lv_obj_set_size(d.scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(d.scrim, m3::col(0x000000), 0);
    lv_obj_set_style_bg_opa(d.scrim, LV_OPA_50, 0);
    lv_obj_add_flag(d.scrim, LV_OBJ_FLAG_CLICKABLE);          // swallow taps meant for below
    constexpr int32_t BW = 150, BH = 64;
    h += BH - 48;                     // callers size h for 48-px buttons; the text keeps its room
    d.card = surface(d.scrim, 0, 0, w, h, t.c.surface_container, m3::shape::XL);
    lv_obj_center(d.card);
    lv_obj_set_style_pad_all(d.card, 24, 0);
    d.title = text(d.card, t.t.title_m, t.c.on_surface, "");
    lv_obj_set_width(d.title, w - 48);
    lv_label_set_long_mode(d.title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(d.title, 0, 0);
    d.body = text(d.card, t.t.body_m, t.c.on_surface_var, "");
    lv_obj_set_width(d.body, w - 48);
    lv_label_set_long_mode(d.body, LV_LABEL_LONG_WRAP);
    lv_obj_set_pos(d.body, 0, 44);
    d.yes = filled_button(d.card, w - 48 - BW, h - 48 - BH, BW, BH, m3::shape::FULL, nullptr, nullptr, "");
    d.no  = button_text(d.card, w - 48 - 2 * BW - 8, h - 48 - BH, BW, BH, "");
    show(d.scrim, false);
    mark_modal(d.scrim);
    return d;
}

void dialog_show(Dialog& d, const char* title, const char* body, const char* yes, const char* no) {
    if (!d.scrim) return;
    lv_label_set_text(d.title, title ? title : "");
    lv_label_set_text(d.body, body ? body : "");
    if (d.yes.label) lv_label_set_text(d.yes.label, yes ? yes : "");
    if (d.no.label)  lv_label_set_text(d.no.label, no ? no : "");
    // Both buttons, every time: a caller hides them while an answer is on its way (the
    // pairing dialog does), and the next question must not inherit that.
    show(d.yes.obj, yes && *yes);
    show(d.no.obj, no && *no);
    // the only button at the card's end, where a second one would have put it
    const bool both = yes && *yes && no && *no;
    if (d.no.obj && d.yes.obj) {
        const int32_t yes_x = lv_obj_get_style_x(d.yes.obj, 0);   // as dialog() placed them
        lv_obj_set_x(d.no.obj, both ? yes_x - lv_obj_get_style_width(d.no.obj, 0) - 8 : yes_x);
    }
    show(d.scrim, true);
    lv_obj_move_foreground(d.scrim);
}

void dialog_hide(Dialog& d) { show(d.scrim, false); }

// ── modality ───────────────────────────────────────────────────────────────────────────────

namespace {
constexpr lv_obj_flag_t MODAL = LV_OBJ_FLAG_USER_1;    // the only user flag this UI sets
}  // namespace

void mark_modal(lv_obj_t* o) {
    if (o) lv_obj_add_flag(o, MODAL);
}

bool modal_shown(lv_obj_t* root, const lv_obj_t* except) {
    if (!root || root == except || lv_obj_has_flag(root, LV_OBJ_FLAG_HIDDEN)) return false;   // nor anything under it
    if (lv_obj_has_flag(root, MODAL)) return true;
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; i++) {
        if (modal_shown(lv_obj_get_child(root, (int32_t)i), except)) return true;
    }
    return false;
}

// ── feedback ───────────────────────────────────────────────────────────────────────────────

lv_obj_t* toast(lv_obj_t* parent) {
    const auto& t = T();
    lv_obj_t* o = box(parent, 0, 0, LV_SIZE_CONTENT, 48);
    fill(o, t.c.inverse_surface, m3::shape::XS);
    lv_obj_set_style_pad_hor(o, 20, 0);
    lv_obj_set_style_shadow_color(o, m3::col(0x000000), 0);
    lv_obj_set_style_shadow_width(o, 20, 0);
    lv_obj_set_style_shadow_offset_y(o, 6, 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_40, 0);
    lv_obj_t* l = text(o, t.t.label_m, t.c.inverse_on_surface, "");
    lv_obj_center(l);
    lv_obj_set_user_data(o, l);
    show(o, false);
    return o;
}

void toast_show(lv_obj_t* o, const char* txt) {
    if (!o) return;
    auto* l = static_cast<lv_obj_t*>(lv_obj_get_user_data(o));
    if (l) lv_label_set_text(l, txt);
    lv_obj_update_layout(o);
    lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -24);
    show(o, true);
    lv_obj_move_foreground(o);
}

namespace {

struct Wave {
    int32_t w = 0;
    float   fraction = 0.0f;
    float   phase = 0.0f;
    bool    live = false;
};
Wave s_waves[4];
size_t s_wave_n = 0;

/// 4 px per segment: 75 short lines for the design's 300 px, smooth enough for a 4 px stroke.
constexpr int32_t WAVE_STEP = 4;
constexpr float   WAVE_AMP  = 3.2f;

void wave_draw_cb(lv_event_t* e) {
    auto* wv = static_cast<Wave*>(lv_event_get_user_data(e));
    lv_obj_t* o = lv_event_get_target_obj(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    if (!wv || !o || !layer) return;
    lv_area_t co;
    lv_obj_get_coords(o, &co);
    const int32_t mid = (co.y1 + co.y2) / 2;
    const int32_t end = (int32_t)(wv->fraction * (float)(wv->w - 12));   // room for the dot
    const auto& c = T().c;

    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.width = 4;
    d.round_start = 1;
    d.round_end = 1;

    // The played part: a sine while live, a straight line while paused.
    d.color = m3::col(c.primary);
    const float amp = wv->live ? WAVE_AMP : 0.0f;
    float py = (float)mid + amp * sinf(-wv->phase);
    for (int32_t x = WAVE_STEP; x <= end; x += WAVE_STEP) {
        const float y = (float)mid + amp * sinf((float)x / 7.0f - wv->phase);
        d.p1.x = (lv_value_precise_t)(co.x1 + x - WAVE_STEP); d.p1.y = (lv_value_precise_t)py;
        d.p2.x = (lv_value_precise_t)(co.x1 + x);             d.p2.y = (lv_value_precise_t)y;
        lv_draw_line(layer, &d);
        py = y;
    }
    // The rest: flat, in the empty-track tone, ending in a dot of primary.
    d.color = m3::col(c.surface_highest);
    d.p1.x = (lv_value_precise_t)(co.x1 + end + 8); d.p1.y = (lv_value_precise_t)mid;
    d.p2.x = (lv_value_precise_t)(co.x2 - 6);       d.p2.y = (lv_value_precise_t)mid;
    if (d.p2.x > d.p1.x) lv_draw_line(layer, &d);

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = m3::col(c.primary);
    dot.bg_opa = LV_OPA_COVER;
    dot.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { co.x2 - 4, mid - 2, co.x2, mid + 2 };
    lv_draw_rect(layer, &dot, &a);
}

} // namespace

void wave_reset() { s_wave_n = 0; }

lv_obj_t* wave(lv_obj_t* parent, int32_t x, int32_t y, int32_t w) {
    if (s_wave_n >= sizeof(s_waves) / sizeof(s_waves[0])) return nullptr;
    Wave& wv = s_waves[s_wave_n++];
    wv.w = w;
    lv_obj_t* o = box(parent, x, y, w, 24);
    lv_obj_set_user_data(o, &wv);
    lv_obj_add_event_cb(o, wave_draw_cb, LV_EVENT_DRAW_MAIN, &wv);
    return o;
}

void wave_set(lv_obj_t* o, float fraction, float phase, bool live) {
    if (!o) return;
    auto* wv = static_cast<Wave*>(lv_obj_get_user_data(o));
    if (!wv) return;
    if (fraction < 0.0f) fraction = 0.0f; else if (fraction > 1.0f) fraction = 1.0f;
    if (wv->fraction == fraction && wv->phase == phase && wv->live == live) return;
    wv->fraction = fraction;
    wv->phase = phase;
    wv->live = live;
    lv_obj_invalidate(o);
}

} // namespace m3w
