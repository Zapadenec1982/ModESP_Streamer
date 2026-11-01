/**
 * @file ui_m3.h
 * @brief Material 3 design tokens — colour roles, shape, type — for the panel UI.
 *
 * The values are the design package's (artifact «Медіаплеєр P4», round 3, «P4 Player M3»),
 * copied role for role. Three dark schemes; the light one the mock also carries is not used.
 *
 * A screen never spells a colour or a font: it names a ROLE here. That is what makes the
 * scheme switch in Settings a table lookup rather than a hunt through the screens. ui_theme.h still exists for the screens that have
 * not been redrawn yet; it is filled FROM these roles (ui_theme.cpp), so the whole panel
 * takes the new colours at once and the old struct disappears with the last old screen.
 */

#pragma once

#include "lvgl.h"
#include <cstdint>

namespace m3 {

/// Which scheme is active. Persisted as `ui.theme`; the numbering carries over from the
/// previous three palettes so a stored value keeps meaning "the first / second / third".
enum class Scheme : uint8_t { AMBER = 0, BLUE = 1, GREEN = 2, RED = 3, COUNT = 4 };

/// Colour roles, 0xRRGGBB. Names follow Material's, shortened where the long form would
/// make every line of layout code wrap.
struct Colors {
    // surfaces, from the deepest to the most raised
    uint32_t surface;             ///< the screen itself
    uint32_t surface_lowest;      ///< standby, and anything that must sit below the screen
    uint32_t surface_low;         ///< rail, drawer, large cards (cover, spectrum, EQ plot)
    uint32_t surface_container;   ///< default card
    uint32_t surface_high;        ///< card that is being touched, snackbar body
    uint32_t surface_highest;     ///< inactive segment, slider track, inactive preset number
    // text and lines
    uint32_t on_surface;          ///< primary text
    uint32_t on_surface_var;      ///< secondary text, inactive icons
    uint32_t outline;             ///< the reference line in plots, disabled text
    uint32_t outline_var;         ///< hairlines, chip borders, grid
    // accent
    uint32_t primary;             ///< the play button, the active slider, the chosen thing
    uint32_t on_primary;
    uint32_t primary_container;   ///< the active source card, the band panel
    uint32_t on_primary_container;
    uint32_t secondary_container; ///< the selected rail pill, prev/next, selected segment
    uint32_t on_secondary_container;
    uint32_t tertiary;            ///< the loud end of the spectrum
    uint32_t tertiary_container;  ///< the FAB
    uint32_t on_tertiary_container;
    // the rest
    uint32_t inverse_surface;     ///< snackbar, the dragged value bubble
    uint32_t inverse_on_surface;
    uint32_t error;               ///< clipping, mute, a fault
};

/// Corner radii the design uses, in px. Everything rounded picks one of these.
namespace shape {
constexpr int32_t XS  = 8;    ///< chips, the snackbar, the volume track ends
constexpr int32_t S   = 16;   ///< FAB, rail pill, standby art tile
constexpr int32_t M   = 20;   ///< transport buttons, preset cards, band panel
constexpr int32_t L   = 24;   ///< source cards, settings columns, spectrum card
constexpr int32_t XL  = 28;   ///< EQ plot card, drawer edge
constexpr int32_t XXL = 32;   ///< the cover
constexpr int32_t FULL = LV_RADIUS_CIRCLE;
} // namespace shape

/// Type roles. One face per role; the faces themselves come from tools/fonts/build_fonts.py.
/// The scale is one step above the mock's: 12 or 14 px cannot be read on the glass from where
/// a stereo is looked at, so nothing is smaller than 16.
struct Type {
    const lv_font_t* label_s;    ///< 16 medium — captions, chip text, axis labels
    const lv_font_t* label_m;    ///< 18 medium — rail labels, list values, buttons
    const lv_font_t* body_m;     ///< 18 — settings subtitles, secondary lines
    const lv_font_t* body_l;     ///< 20 — list titles, preset names, drawer rows
    const lv_font_t* title_s;    ///< 20 — band values, standby date
    const lv_font_t* title_m;    ///< 24 — the subtitle line under the track title
    const lv_font_t* headline;   ///< 32 — page title, volume bubble
    const lv_font_t* display;    ///< 48 — the track title
    const lv_font_t* clock;      ///< digits only — the standby clock
    // Material Symbols Rounded. `_fill` is the filled variant the design uses for the
    // active rail item, the pressed favourite and the transport glyphs.
    const lv_font_t* icon_s;     ///< 20
    const lv_font_t* icon_m;     ///< 24
    const lv_font_t* icon_l;     ///< 28
    const lv_font_t* icon_xl;    ///< 40 — the play button
    const lv_font_t* icon_m_fill;
    const lv_font_t* icon_l_fill;
    const lv_font_t* icon_xl_fill;
};

struct Theme {
    const char* name;            ///< for the log: amber / blue / green / red. The screens name a
                                 ///< scheme from the catalog (sys.scheme_*), in the panel's language
    Colors      c;
    Type        t;
};

/// The active theme. Never null; AMBER until ui_theme_set() runs.
const Theme& theme();
Scheme       scheme();
/// Returns true when the scheme actually changed, so the caller can restyle.
bool         set_scheme(Scheme s);
const Theme& theme_at(Scheme s);

/// lv_color_t straight from a role value.
static inline lv_color_t col(uint32_t rgb) { return lv_color_hex(rgb); }

} // namespace m3
