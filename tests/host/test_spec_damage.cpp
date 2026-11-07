/**
 * @file test_spec_damage.cpp
 * @brief The rows a spectrum bar repaints when it moves: every row that looks different is in them.
 *
 * modules/ui_lvgl/include/spec_damage.h. A bar is modelled the way LVGL draws a filled rounded
 * rectangle: each row is empty, full, or a corner row whose coverage depends only on its distance
 * from the top or the bottom edge and on the radius LVGL clamps to (lv_draw_sw_fill). Two bars
 * look the same in a row exactly when that row's description is the same; the rows outside the
 * repainted range must all look the same, or the glass keeps a stale piece of the old bar.
 */

#include "doctest.h"
#include "../../modules/ui_lvgl/include/spec_damage.h"

using spec_damage::SPEC_RADIUS;
using spec_damage::drawn_radius;

namespace {

/// What row `y` (1 = the baseline row) of a bar `w` x `h` looks like, as one number.
int32_t row_look(int32_t h, int32_t w, int32_t y, int32_t loud_from) {
    if (y > h) return 0;                                   // above the bar: background
    const int32_t r = drawn_radius(SPEC_RADIUS, w, h);
    const int32_t tone = h > loud_from ? 1 : 2;
    const int32_t from_top = h - y, from_bottom = y - 1;
    int32_t shape;
    if (from_top < r)         shape = 100 + r * 10 + from_top;      // a top corner row
    else if (from_bottom < r) shape = 200 + r * 10 + from_bottom;   // a bottom corner row
    else                      shape = 1;                            // full width
    return tone * 1000 + shape;
}

void check_rows(int32_t w, int32_t row_h) {
    const int32_t loud_from = row_h * 82 / 100;
    int missed = 0;
    for (int32_t a = 2; a <= row_h; a++) {
        for (int32_t b = 2; b <= row_h; b++) {
            int32_t lo = 0, hi = 0;
            const bool moved = spec_damage::rows(a, b, w, SPEC_RADIUS, loud_from, lo, hi);
            if (a == b) { CHECK_FALSE(moved); continue; }
            REQUIRE(moved);
            CHECK(lo >= 1);
            CHECK(hi == (a > b ? a : b));
            for (int32_t y = 1; y <= row_h; y++) {
                if (y >= lo && y <= hi) continue;
                if (row_look(a, w, y, loud_from) != row_look(b, w, y, loud_from)) missed++;
            }
        }
    }
    CHECK(missed == 0);
}

}  // namespace

TEST_CASE("spec damage: every changed row is repainted, the Player's row") {
    check_rows(10, 88);
}

TEST_CASE("spec damage: every changed row is repainted, the standby face's row") {
    check_rows(12, 180);
}

TEST_CASE("spec damage: a move between two tall bars repaints the caps, not the column") {
    int32_t lo = 0, hi = 0;
    REQUIRE(spec_damage::rows(50, 60, 12, SPEC_RADIUS, 200, lo, hi));
    CHECK(lo == 50 - SPEC_RADIUS + 1);
    CHECK(hi == 60);
}

TEST_CASE("spec damage: crossing into the loud tone repaints the whole bar") {
    int32_t lo = 0, hi = 0;
    REQUIRE(spec_damage::rows(140, 150, 12, SPEC_RADIUS, 147, lo, hi));
    CHECK(lo == 1);
    CHECK(hi == 150);
}

TEST_CASE("spec damage: a short bar's corners shrink, so its bottom is repainted too") {
    int32_t lo = 0, hi = 0;
    REQUIRE(spec_damage::rows(3, 9, 12, SPEC_RADIUS, 200, lo, hi));
    CHECK(lo == 1);
    CHECK(hi == 9);
}
