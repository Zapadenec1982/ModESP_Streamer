/**
 * @file spec_damage.h
 * @brief Which rows of a spectrum bar change when its height does. No LVGL.
 *
 * The spectrum row (ui_helpers.cpp) draws each bar as a rectangle standing on the baseline,
 * its corners rounded with radius SPEC_RADIUS. Repainting the whole row whenever one bar moves
 * by a pixel sent 115,920 pixels a frame through the draw buffer on the standby face (49,280
 * on the Player), 25 times a second, and a sweep that size pushes the DSP's code out of the
 * shared L2 cache. Only the rows that differ between the old bar and the new one need drawing.
 *
 * Rows are counted from the baseline: row 1 is the baseline row, a bar of height h covers rows
 * 1..h. LVGL clamps a rectangle's radius to half its shorter side, so a bar's corners are
 * r' = min(r, h / 2, w / 2). Two bars of heights a and b drawn the same way differ:
 *   - above min(a, b) - r': the rounded cap of the lower one, and the rest of the taller one;
 *   - below that only if r' differs between them (a short bar's corners shrink) or the colour
 *     does — the row tells a loud bar by a change of tone, which repaints the whole bar.
 * Pinned on the host by tests/host/test_spec_damage.cpp, against a model of the bar's shape.
 */

#pragma once

#include <cstdint>

namespace spec_damage {

/// The corner radius every bar is drawn with.
constexpr int32_t SPEC_RADIUS = 4;

/// The radius LVGL actually draws a `w` x `h` rectangle with.
constexpr int32_t drawn_radius(int32_t r, int32_t w, int32_t h) {
    const int32_t s = (w < h ? w : h) / 2;
    return r < s ? r : s;
}

/// Rows lo..hi (from the baseline, inclusive) where a bar `w` wide changes from height `a`
/// to `b`; false when nothing changes. `loud_from`: a bar taller than this is drawn in the
/// other tone, so crossing it repaints the whole bar.
inline bool rows(int32_t a, int32_t b, int32_t w, int32_t r, int32_t loud_from, int32_t& lo, int32_t& hi) {
    if (a == b) return false;
    const int32_t low  = a < b ? a : b;
    const int32_t high = a < b ? b : a;
    hi = high;
    const bool recolour = (a > loud_from) != (b > loud_from);
    const bool corners  = drawn_radius(r, w, a) != drawn_radius(r, w, b);
    if (recolour || corners) {
        lo = 1;
    } else {
        lo = low - drawn_radius(r, w, low) + 1;
        if (lo < 1) lo = 1;
    }
    return true;
}

}  // namespace spec_damage
