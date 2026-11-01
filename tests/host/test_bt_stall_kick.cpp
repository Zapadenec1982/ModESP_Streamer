/**
 * @file test_bt_stall_kick.cpp
 * @brief When the panel sends PLAY for a phone that plays while no stream comes
 *        (modules/bt/include/bt_stall_kick.h).
 *
 * After the panel was restarted twice, Bluetooth was selected, the phone said it played, and
 * nothing was heard until the satellite was restarted by hand.
 */

#include "doctest.h"
#include "bt_stall_kick.h"

#include <cstdint>

using bt_stall::Kick;

TEST_CASE("bt stall: Bluetooth selected, the phone playing and no stream: one PLAY after the hold") {
    Kick k;
    CHECK_FALSE(k.tick(Kick::HOLD_MS - 1, true, true, false));
    CHECK(k.tick(1, true, true, false));
    // Once per stall: a PLAY that did not help is not repeated every pass.
    CHECK_FALSE(k.tick(Kick::HOLD_MS, true, true, false));
    CHECK_FALSE(k.tick(Kick::HOLD_MS, true, true, false));
}

TEST_CASE("bt stall: a stream that starts within the hold gets no PLAY") {
    Kick k;
    CHECK_FALSE(k.tick(Kick::HOLD_MS - 1, true, true, false));   // connected, not yet streaming
    CHECK_FALSE(k.tick(1, true, true, true));
    CHECK_FALSE(k.tick(10 * Kick::HOLD_MS, true, true, true));
}

TEST_CASE("bt stall: nothing while another source is selected") {
    Kick k;
    CHECK_FALSE(k.tick(10 * Kick::HOLD_MS, false, true, false));
    // and the hold starts only when Bluetooth is picked
    CHECK_FALSE(k.tick(Kick::HOLD_MS - 1, true, true, false));
    CHECK(k.tick(1, true, true, false));
}

TEST_CASE("bt stall: a phone that does not say it plays gets nothing") {
    Kick k;
    CHECK_FALSE(k.tick(10 * Kick::HOLD_MS, true, false, false));   // paused, idle, or no TRACK_POS
}

TEST_CASE("bt stall: a pause somebody asked for is not undone, even by a phone that ignored it") {
    Kick k;
    k.on_user_pause();
    // The phone ignores the PAUSE and keeps streaming; the satellite suspends the stream; the
    // phone goes on saying it plays.
    CHECK_FALSE(k.tick(Kick::HOLD_MS, true, true, true));
    CHECK_FALSE(k.tick(10 * Kick::HOLD_MS, true, true, false));
    // PLAY from a button ends it.
    k.on_play();
    CHECK_FALSE(k.tick(Kick::HOLD_MS - 1, true, true, false));
    CHECK(k.tick(1, true, true, false));
}

TEST_CASE("bt stall: the phone leaving ends a pause somebody asked for") {
    Kick k;
    k.on_user_pause();
    k.on_phone_gone();
    CHECK(k.tick(Kick::HOLD_MS, true, true, false));
}

TEST_CASE("bt stall: a stall that breaks starts the hold again") {
    Kick k;
    CHECK_FALSE(k.tick(Kick::HOLD_MS - 1, true, true, false));
    CHECK_FALSE(k.tick(1, true, false, false));                   // the phone paused
    CHECK_FALSE(k.tick(Kick::HOLD_MS - 1, true, true, false));
    CHECK(k.tick(1, true, true, false));
}

TEST_CASE("bt stall: a stream that ran re-arms the PLAY for the next stall") {
    Kick k;
    CHECK(k.tick(Kick::HOLD_MS, true, true, false));
    CHECK_FALSE(k.tick(1000, true, true, true));
    CHECK_FALSE(k.tick(Kick::HOLD_MS - 1, true, true, false));
    CHECK(k.tick(1, true, true, false));
}

TEST_CASE("bt stall: a long pass does not wrap the hold") {
    Kick k;
    CHECK_FALSE(k.tick(Kick::HOLD_MS - 1, true, true, false));
    CHECK(k.tick(UINT32_MAX, true, true, false));
}
