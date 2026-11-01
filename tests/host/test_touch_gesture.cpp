/**
 * @file test_touch_gesture.cpp
 * @brief A finger that scrolls does not set anything — the glass's rules.
 *
 * modules/ui_lvgl/include/touch_gesture.h, the same rules as the web's lib/gesture.js
 * (tools/tests/test_webui_gesture.py). Without them scrolling catches the sliders and the
 * equalizer. A press changes nothing; a slider is the finger's once it went 8 px along, more
 * along than across, and the page's if it went up or down first; a tap changes nothing; an EQ
 * point moves after a 300 ms hold or 8 px; «Скасувати» says where a setting started. And on the
 * glass only a finger on the knob takes a slider, and the knob follows it from where it stood
 * instead of jumping to it.
 */

#include "doctest.h"
#include "../../modules/ui_lvgl/include/touch_gesture.h"

#include <utility>
#include <vector>

using touch::Intent;
using touch::Point;
using touch::Slide;

namespace {

/// A slider 0…100 across 200 px, at 40 — its knob at x 80 — and a finger's path over it: what it
/// ends as, and the value, held to the slider's limits as its caller holds it.
std::pair<Slide::State, int> finger(const std::vector<std::pair<int, int>>& path) {
    int value = 40;
    Slide s;
    s.down(path[0].first, path[0].second, 80, value);
    for (size_t i = 1; i < path.size(); i++) {
        if (s.move(path[i].first, path[i].second) == Slide::DRAG) {
            const int v = s.follow(path[i].first, 100, 200);
            value = v < 0 ? 0 : v > 100 ? 100 : v;
        }
    }
    return {s.end(), value};
}

}  // namespace

TEST_CASE("touch: the threshold and the direction") {
    CHECK(touch::SLOP == 8);
    CHECK(touch::intent(7, 0) == Intent::PRESS);
    CHECK(touch::intent(-7, 7) == Intent::PRESS);
    CHECK(touch::intent(8, 0) == Intent::DRAG);
    CHECK(touch::intent(-9, 8) == Intent::DRAG);          // more along than across
    CHECK(touch::intent(8, 8) == Intent::SCROLL);         // as much across as along is the page's
    CHECK(touch::intent(3, -9) == Intent::SCROLL);
    CHECK(touch::intent(0, 8) == Intent::SCROLL);
}

TEST_CASE("touch: a press, a tap and a swipe down change nothing; along, the slider follows") {
    CHECK(finger({{80, 100}}) == std::make_pair(Slide::PRESS, 40));                    // a tap
    CHECK(finger({{80, 100}, {84, 103}, {77, 98}}) == std::make_pair(Slide::PRESS, 40));
    // down first: the page's, and along afterwards does not take it back
    CHECK(finger({{80, 100}, {81, 110}, {82, 160}, {120, 170}}) == std::make_pair(Slide::SCROLL, 40));
    CHECK(finger({{80, 100}, {90, 102}}) == std::make_pair(Slide::DRAG, 45));
    // once it is the slider's, across does not give it to the page; the limits hold it
    CHECK(finger({{80, 100}, {70, 101}, {30, 140}, {-10, 300}}) == std::make_pair(Slide::DRAG, 0));

    Slide s;
    s.down(10, 10, 10, 0);
    s.scrolled();                                          // LVGL found a scroll first
    CHECK(s.move(40, 10) == Slide::SCROLL);
    CHECK(s.end() == Slide::SCROLL);
    CHECK(s.state() == Slide::IDLE);
}

TEST_CASE("touch: only the knob takes a finger, and the knob follows it without a jump") {
    // a touch on the track away from the knob must not move the slider
    CHECK(finger({{150, 100}, {190, 100}}) == std::make_pair(Slide::MISS, 40));
    CHECK(finger({{0, 100}, {200, 100}}) == std::make_pair(Slide::MISS, 40));
    CHECK(finger({{80 + touch::GRAB, 100}, {200, 100}}).first == Slide::DRAG);        // the edge of the reach
    CHECK(finger({{80 + touch::GRAB + 1, 100}, {200, 100}}).first == Slide::MISS);
    CHECK(finger({{80 - touch::GRAB - 1, 100}, {0, 100}}).first == Slide::MISS);
    // landed 20 px right of the knob and went 10 more: the knob moves 10 px' worth, not under the finger
    CHECK(finger({{100, 100}, {110, 100}}) == std::make_pair(Slide::DRAG, 45));
    CHECK(finger({{60, 100}, {40, 100}}) == std::make_pair(Slide::DRAG, 30));
    // a miss stays a miss, whatever the finger does after
    Slide m;
    CHECK(m.down(300, 10, 80, 40) == Slide::MISS);
    CHECK(m.move(320, 10) == Slide::MISS);
    CHECK(m.move(300, 60) == Slide::MISS);
    CHECK(m.end() == Slide::MISS);
    // the follow rounds to the nearest step, both ways
    Slide f;
    f.down(80, 0, 80, 0);
    CHECK(f.follow(81, 12, 300) == 0);
    CHECK(f.follow(93, 12, 300) == 1);                     // 25 px a step: past half of one rounds up
    CHECK(f.follow(67, 12, 300) == -1);
    CHECK(f.follow(80, 12, 0) == 0);                       // no span, no move
}

TEST_CASE("touch: an EQ point moves after a hold or a clear move; a tap only picks it") {
    CHECK(touch::HOLD_MS == 300);
    Point p;
    p.down(100, 100, 1000);
    CHECK(p.tick(1299) == Point::PRESS);
    CHECK(p.end() == Point::PRESS);                       // a tap: the band, nothing moved

    p.down(100, 100, 1000);
    CHECK(p.move(103, 104, 1120) == Point::PRESS);        // 5 px
    CHECK(p.tick(1300) == Point::LIFT);                   // held: picked up
    CHECK(p.move(104, 104, 1400) == Point::DRAG);         // picked up, any move drags it
    CHECK(p.end() == Point::DRAG);

    p.down(100, 100, 1000);
    CHECK(p.move(100, 108, 1050) == Point::DRAG);         // 8 px from where it landed

    p.down(100, 100, 1000);
    CHECK(p.move(100, 100, 1350) == Point::LIFT);         // a move event with no movement
    CHECK(p.end() == Point::PRESS);                       // held and let go: picked, not moved

    p.down(100, 100, 0xFFFFFF00u);                         // the tick wraps
    CHECK(p.tick(0x0000002Cu) == Point::LIFT);
}

TEST_CASE("touch: «Скасувати» says where a setting started") {
    CHECK(touch::UNDO_MS == 5000);
    touch::UndoStart<int> u;
    CHECK(u.note(0, 1000) == 0);                           // Бас 0 → +3
    CHECK(u.note(3, 3000) == 0);                           // moved again while it shows: from 0
    CHECK(u.note(5, 7999) == 0);                           // the 5 s run from the last move
    CHECK(u.note(2, 13000) == 2);                          // later: a new start
    u.clear();
    CHECK(u.note(4, 13500) == 4);                          // taken back: a new start
}
