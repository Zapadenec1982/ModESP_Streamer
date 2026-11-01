/**
 * @file test_airplay_meta.cpp
 * @brief Which AirPlay metadata records reach the tile (modules/airplay/include/airplay_meta.h).
 *
 * On an Apple Music radio station the cover and the title vanished for a second and came back,
 * every ~15 s. The iPhone sends the station's name with no artist between the track's records,
 * and an empty cover between the track's covers.
 */

#include "doctest.h"
#include "airplay_meta.h"

using airplay_meta::Smoother;

namespace {
constexpr int64_t S = 1000 * 1000;
}

TEST_CASE("airplay meta: a station's name between a track's records never reaches the tile") {
    Smoother m;
    bool drop = false;
    CHECK(m.on_names(true, true, false, 10 * S, drop) == Smoother::Names::HOLD);   // the station
    CHECK_FALSE(drop);
    CHECK(m.on_cover(false, 10 * S + 1, drop) == Smoother::Cover::HOLD);  // its cover waits too
    CHECK_FALSE(drop);
    CHECK(m.on_names(false, true, true, 11 * S + S / 2, drop) == Smoother::Names::APPLY); // back
    CHECK(drop);                                       // the station's cover is thrown away
    const auto due = m.due(30 * S);
    CHECK_FALSE(due.title);
    CHECK_FALSE(due.cover);
}

TEST_CASE("airplay meta: a title-only record nothing overtakes is shown, late, with its cover") {
    Smoother m;
    bool drop = false;
    CHECK(m.on_names(true, true, false, 10 * S, drop) == Smoother::Names::HOLD);
    CHECK(m.on_cover(false, 10 * S + 1, drop) == Smoother::Cover::HOLD);
    auto due = m.due(10 * S + Smoother::WINDOW_US - 1);
    CHECK_FALSE(due.title);
    due = m.due(10 * S + Smoother::WINDOW_US);
    CHECK(due.title);
    CHECK(due.cover);
    CHECK_FALSE(m.holding_title());
    CHECK_FALSE(m.holding_cover());
}

TEST_CASE("airplay meta: an empty cover between a track's covers never clears the tile") {
    Smoother m;
    bool drop = false;
    CHECK(m.on_cover(true, 10 * S, drop) == Smoother::Cover::HOLD);        // "none"
    CHECK_FALSE(drop);
    CHECK(m.on_cover(false, 11 * S + S / 2, drop) == Smoother::Cover::POST); // the track's again
    CHECK(drop);                                       // the held "none" is thrown away
    CHECK_FALSE(m.due(30 * S).cover);
}

TEST_CASE("airplay meta: an empty cover nothing overtakes clears the tile, late") {
    Smoother m;
    bool drop = false;
    CHECK(m.on_cover(true, 10 * S, drop) == Smoother::Cover::HOLD);
    CHECK_FALSE(m.due(10 * S + Smoother::WINDOW_US - 1).cover);
    CHECK(m.due(10 * S + Smoother::WINDOW_US).cover);
}

TEST_CASE("airplay meta: a real track, and a title over a title, are shown at once") {
    Smoother m;
    bool drop = false;
    CHECK(m.on_names(false, true, false, 10 * S, drop) == Smoother::Names::APPLY); // new track
    CHECK(m.on_names(true, false, false, 11 * S, drop) == Smoother::Names::APPLY); // no artist shown
    CHECK(m.on_names(true, true, true, 12 * S, drop) == Smoother::Names::APPLY);   // same title
    CHECK(m.on_cover(false, 13 * S, drop) == Smoother::Cover::POST);
    CHECK_FALSE(m.holding_title());
}

TEST_CASE("airplay meta: a cover after an empty one, during an interlude, waits with it") {
    Smoother m;
    bool drop = false;
    CHECK(m.on_cover(true, 10 * S, drop) == Smoother::Cover::HOLD);         // empty
    CHECK(m.on_names(true, true, false, 10 * S + 1, drop) == Smoother::Names::HOLD);
    CHECK(m.on_cover(false, 10 * S + 2, drop) == Smoother::Cover::HOLD);    // station's cover
    CHECK(drop);                                       // replaces the empty one
    CHECK(m.on_names(false, true, true, 11 * S, drop) == Smoother::Names::APPLY);
    CHECK(drop);
    CHECK_FALSE(m.holding_cover());
}

TEST_CASE("airplay meta: the session ending drops whatever is held") {
    Smoother m;
    bool drop = false;
    m.on_names(true, true, false, 10 * S, drop);
    m.on_cover(true, 10 * S, drop);
    m.reset();
    const auto due = m.due(30 * S);
    CHECK_FALSE(due.title);
    CHECK_FALSE(due.cover);
}
