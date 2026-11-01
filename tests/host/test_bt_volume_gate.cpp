/**
 * @file test_bt_volume_gate.cpp
 * @brief Which levels a phone sets become the room's volume (modules/bt/include/bt_volume_gate.h).
 *
 * The radio was playing, a phone connected over Bluetooth and the room jumped to the phone's
 * level. These pin the rule that replaced "every level the phone sets is taken".
 */

#include "doctest.h"
#include "bt_volume_gate.h"

using bt_volume::Gate;

TEST_CASE("bt volume: a phone connecting while the radio plays does not move the room") {
    Gate g;
    CHECK(g.on_state(true));          // fresh: the panel pushes its own level
    CHECK_FALSE(g.take(false));       // the level it brings along
    g.tick(Gate::HOLD_MS);
    CHECK_FALSE(g.take(false));       // nor anything later, while Bluetooth is not the source
}

TEST_CASE("bt volume: with Bluetooth selected, the level a phone restores as it connects is not taken") {
    Gate g;
    CHECK(g.on_state(true));
    CHECK_FALSE(g.take(true));
    g.tick(Gate::HOLD_MS - 1);
    CHECK_FALSE(g.take(true));
    CHECK_FALSE(g.settled());
    g.tick(1);
    CHECK(g.settled());
    CHECK(g.take(true));              // from here a slider move is somebody listening
}

TEST_CASE("bt volume: streaming after connected is the same connection, not a new hold") {
    Gate g;
    CHECK(g.on_state(true));          // BT_CONNECTED
    g.tick(Gate::HOLD_MS);
    CHECK_FALSE(g.on_state(true));    // BT_STREAMING, or a repeated BT_STATE
    CHECK(g.take(true));
}

TEST_CASE("bt volume: a phone that leaves and comes back is held again") {
    Gate g;
    g.on_state(true);
    g.tick(Gate::HOLD_MS);
    CHECK(g.take(true));
    CHECK_FALSE(g.on_state(false));   // disconnected
    CHECK_FALSE(g.take(true));
    CHECK(g.on_state(true));          // reconnected: fresh again
    CHECK_FALSE(g.take(true));
}

TEST_CASE("bt volume: a lost link makes the next BT_STATE a connection") {
    Gate g;
    g.on_state(true);
    g.tick(Gate::HOLD_MS);
    g.link_lost();
    CHECK_FALSE(g.take(true));
    CHECK(g.on_state(true));          // the satellite says connected after its HELLO
    CHECK_FALSE(g.take(true));
}

TEST_CASE("bt volume: a level before BT_STATE says connected is part of the connection") {
    Gate g;                           // AVRCP can come up before A2DP reports
    CHECK_FALSE(g.settled());
    CHECK_FALSE(g.take(true));
}

TEST_CASE("bt volume: only the levels a connection brings are answered with the room's") {
    Gate g;
    CHECK(g.answer());                // before BT_STATE says connected
    g.on_state(true);
    CHECK(g.answer());                // while it connects
    g.tick(Gate::HOLD_MS);
    CHECK_FALSE(g.answer());          // a press while another source plays is left alone
}

TEST_CASE("bt volume: picking Bluetooth tells the phone the room's level, once") {
    // A Pixel's presses while AirPlay played were thrown back every time, which read as volume
    // that lagged; the other source keeps its volume.
    Gate g;
    g.on_state(true);
    g.tick(Gate::HOLD_MS);
    CHECK_FALSE(g.on_selected(false));
    CHECK_FALSE(g.take(false));
    CHECK(g.on_selected(true));       // picked: push the room's level
    CHECK_FALSE(g.on_selected(true)); // still picked: nothing more
    CHECK(g.take(true));
    CHECK_FALSE(g.on_selected(false));
    CHECK(g.on_selected(true));       // picked again
}

TEST_CASE("bt volume: the hold saturates instead of wrapping") {
    Gate g;
    g.on_state(true);
    g.tick(0xFFFFFFFFu);
    CHECK(g.settled());
    g.tick(0xFFFFFFFFu);
    CHECK(g.settled());
}
