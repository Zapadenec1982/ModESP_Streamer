/**
 * @file ui_theme.cpp
 * @brief The legacy palette, filled from the Material 3 roles.
 *
 * Every screen that has not been redrawn yet still reads UiTheme. Rather than keep two
 * palettes in step by hand, this maps each old token onto the M3 role it now means
 * (ui_m3.h), so switching the scheme in Settings — or, later, from the cover — recolours the
 * old screens and the new ones from the same table. The mapping is the transition; when the
 * last old screen goes, so does this file.
 *
 * Depth is gone on purpose: glow and card shadows are LV_OPA_TRANSP. Material's depth is
 * tonal — a raised thing is a lighter surface, not a lit one.
 */

#include "ui_theme.h"
#include "ui_m3.h"

namespace {

UiTheme legacy(const m3::Theme& m) {
    UiTheme t{};
    t.name     = m.name;
    t.bg       = m.c.surface;              // both ends of the old gradient: a flat screen
    t.bg2      = m.c.surface;
    t.panel    = m.c.surface_container;
    t.panel_on = m.c.secondary_container;
    t.hair     = m.c.outline_var;
    t.text     = m.c.on_surface;
    t.dim      = m.c.on_surface_var;
    t.faint    = m.c.outline;
    t.acc      = m.c.primary;
    t.acc_lo   = m.c.primary_container;
    t.red      = m.c.error;
    t.glow_opa        = LV_OPA_TRANSP;
    t.card_shadow_opa = LV_OPA_TRANSP;
    t.f_cap     = m.t.label_s;
    t.f_body    = m.t.body_m;
    t.f_body_b  = m.t.label_m;
    t.f_lead    = m.t.title_s;
    t.f_clock   = m.t.title_m;
    t.f_title   = m.t.headline;
    t.f_num     = m.t.display;
    t.f_num_xl  = m.t.clock;
    t.f_icon_s  = m.t.icon_s;
    t.f_icon_m  = m.t.icon_m;
    t.f_icon_l  = m.t.icon_l;
    t.f_icon_xl = m.t.icon_xl;
    return t;
}

UiTheme s_legacy[(int)m3::Scheme::COUNT];
bool    s_built = false;

void build() {
    for (int i = 0; i < (int)m3::Scheme::COUNT; i++) s_legacy[i] = legacy(m3::theme_at((m3::Scheme)i));
    s_built = true;
}

} // namespace

const UiTheme& ui_theme() {
    if (!s_built) build();
    return s_legacy[(int)m3::scheme()];
}

UiThemeId ui_theme_id() { return m3::scheme(); }

const UiTheme& ui_theme_at(UiThemeId id) {
    if (!s_built) build();
    return s_legacy[(int)id < (int)m3::Scheme::COUNT ? (int)id : 0];
}

bool ui_theme_set(UiThemeId id) { return m3::set_scheme(id); }
