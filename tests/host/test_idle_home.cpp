/**
 * @file test_idle_home.cpp
 * @brief Back to the Player after a minute alone — when, and how often.
 *
 * modules/ui_lvgl/include/idle_home.h: a minute with nothing touched takes any page back to the
 * Player, and closes the menu, the «Звук» sheet and the pick list over the Player itself. Not
 * from Service or the component test page; not while a keyboard, a dialog, a test tone or a
 * measurement holds the page, and then as soon as it lets go; never in standby; once per idle
 * period.
 */

#include "doctest.h"
#include "../../modules/ui_lvgl/include/idle_home.h"

#include <initializer_list>

using idle_home::AFTER_MS;
using idle_home::Period;

namespace {
constexpr auto FREE = [] { return false; };
constexpr auto HELD = [] { return true; };
}  // namespace

TEST_CASE("idle home: a minute alone on another page goes back, once") {
    Period p;
    CHECK_FALSE(p.pass(AFTER_MS - 1, false, "set", false, FREE));
    CHECK(p.pass(AFTER_MS, false, "set", false, FREE));
    CHECK_FALSE(p.pass(AFTER_MS + 40, false, "now", false, FREE));   // it went: not every pass
    CHECK_FALSE(p.pass(AFTER_MS + 80, false, "eq", false, FREE));    // a page that came without a touch stays
    CHECK_FALSE(p.pass(0, false, "eq", false, FREE));                // touched: a new period
    CHECK_FALSE(p.pass(AFTER_MS - 1, false, "eq", false, FREE));
    CHECK(p.pass(AFTER_MS, false, "eq", false, FREE));
}

TEST_CASE("idle home: from every page but the Player, Service and the test page") {
    for (const char* page : {"src", "set", "radio", "catalog", "library", "dlna", "sound", "eq",
                             "speakers", "xover", "display", "wifi", "bt"}) {
        Period p;
        CHECK_MESSAGE(p.pass(AFTER_MS, false, page, false, FREE), page);
    }
    for (const char* page : {"svc", "m3test"}) {
        Period p;
        CHECK_FALSE(p.pass(AFTER_MS, false, page, false, FREE));
        CHECK_FALSE(p.pass(AFTER_MS, false, page, true, FREE));       // the menu over it stays too
    }
}

TEST_CASE("idle home: the bare Player stays; the menu, the sheet or a list over it closes") {
    Period bare;
    CHECK_FALSE(bare.pass(AFTER_MS, false, "now", false, FREE));
    CHECK_FALSE(bare.pass(AFTER_MS + 40, false, "set", false, FREE));  // its period is spent

    Period over;
    CHECK(over.pass(AFTER_MS, false, "now", true, FREE));
    CHECK_FALSE(over.pass(AFTER_MS + 40, false, "now", true, FREE));
}

TEST_CASE("idle home: a keyboard, a dialog, a tone or a measurement keeps the page; then it goes") {
    Period p;
    CHECK_FALSE(p.pass(AFTER_MS, false, "wifi", false, HELD));
    CHECK_FALSE(p.pass(AFTER_MS + 30000, false, "wifi", false, HELD));   // still alone, still held
    CHECK(p.pass(AFTER_MS + 30040, false, "wifi", false, FREE));         // let go: goes at once
    CHECK_FALSE(p.pass(AFTER_MS + 30080, false, "now", false, FREE));

    Period q;                                    // held, then touched: the wait ends with the period
    CHECK_FALSE(q.pass(AFTER_MS, false, "catalog", false, HELD));
    CHECK_FALSE(q.pass(10, false, "catalog", false, FREE));
    CHECK_FALSE(q.pass(AFTER_MS - 1, false, "catalog", false, FREE));
    CHECK(q.pass(AFTER_MS, false, "catalog", false, FREE));
}

TEST_CASE("idle home: nothing moves in standby") {
    Period p;
    CHECK_FALSE(p.pass(AFTER_MS, true, "set", false, FREE));
    CHECK_FALSE(p.pass(5 * AFTER_MS, true, "set", true, FREE));
}

TEST_CASE("idle home: what holds the page is asked only when the panel would go") {
    int asked = 0;
    auto held = [&asked] { asked++; return true; };
    Period p;
    p.pass(AFTER_MS - 1, false, "set", false, held);    // not alone long enough
    p.pass(AFTER_MS, true, "set", false, held);         // standby
    p.pass(AFTER_MS, false, "svc", false, held);        // watched: spends the period
    p.pass(AFTER_MS, false, "set", false, held);        // spent
    CHECK(asked == 0);
    Period q;
    q.pass(AFTER_MS, false, "now", false, held);        // the bare Player
    CHECK(asked == 0);
    Period r;
    r.pass(AFTER_MS, false, "set", false, held);
    r.pass(AFTER_MS + 40, false, "set", false, held);
    CHECK(asked == 2);                                  // held: asked again on the next pass
}
