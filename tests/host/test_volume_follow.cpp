/**
 * @file test_volume_follow.cpp
 * @brief HOST tests for how a sender's volume slider moves the room
 *        (components/audio_dsp/include/volume_follow.h), the one rule of the computer on USB and
 *        the phone on AirPlay: the room keeps its level when a stream starts, the slider means
 *        nothing while held or while the sender is not the source, it then maps through the
 *        room's level to the panel's top and to silence, and the knob re-references it.
 */

#include "doctest.h"
#include "volume_follow.h"

using audio_core::VolumeFollow;

// ── the computer (modules/usb_audio): its slider stands wherever it was left ──

TEST_CASE("volume_follow: the room keeps its level when the computer starts at full volume") {
    VolumeFollow f;
    f.session_start(30, 100);
    CHECK(f.on_level(100, 30, true) == -1);   // within the hold: a restated level, not a press
    f.tick(VolumeFollow::HOLD_MS);
    CHECK(f.on_level(100, 30, true) == 30);   // unchanged slider, unchanged room
    CHECK(f.on_level(50, 30, true) == 15);    // half way down is half the room's level
    CHECK(f.on_level(0, 30, true) == 0);      // the bottom is silence
}

TEST_CASE("volume_follow: up from a lowered slider reaches the panel's top at the slider's top") {
    VolumeFollow f;
    f.session_start(40, 60);
    f.tick(VolumeFollow::HOLD_MS);
    CHECK(f.on_level(60, 40, true) == 40);
    CHECK(f.on_level(80, 40, true) == 70);    // half of the span above the reference
    CHECK(f.on_level(100, 40, true) == 100);
}

TEST_CASE("volume_follow: while the sender is not the source the slider only moves the reference") {
    VolumeFollow f;
    f.session_start(30, 100);
    f.tick(VolumeFollow::HOLD_MS);
    CHECK(f.on_level(50, 30, false) == -1);
    CHECK(f.on_level(50, 30, true) == 30);    // the reference moved with it: no jump on return
}

TEST_CASE("volume_follow: the knob moves the room, and the slider counts from there") {
    VolumeFollow f;
    f.session_start(30, 80);
    f.tick(VolumeFollow::HOLD_MS);
    CHECK(f.on_level(80, 30, true) == 30);
    f.room_moved(50);                          // turned up on the panel
    CHECK(f.on_level(80, 50, true) == 50);    // the slider where it was: the knob's level stands
    CHECK(f.on_level(40, 50, true) == 25);
}

// ── the phone (modules/airplay): told the room's level, its slider starts there ──

TEST_CASE("volume_follow: a sender told the room's level follows it one to one") {
    VolumeFollow f;
    f.session_start(37, 37);
    f.tick(VolumeFollow::HOLD_MS);
    CHECK(f.on_level(43, 37, true) == 43);    // every press lands where the slider is
    CHECK(f.on_level(100, 43, true) == 100);
    CHECK(f.on_level(0, 100, true) == 0);
    CHECK(f.on_level(37, 0, true) == 37);
}

TEST_CASE("volume_follow: a sender that restates its own level as it starts maps through the room's") {
    VolumeFollow f;
    f.session_start(37, 37);
    CHECK(f.on_level(80, 37, true) == -1);    // not 80: the room keeps its level
    f.tick(VolumeFollow::HOLD_MS);
    CHECK(f.on_level(90, 37, true) == 69);    // half way up
    CHECK(f.on_level(100, 37, true) == 100);  // the top
    CHECK(f.on_level(40, 37, true) == 19);    // half way down
    CHECK(f.on_level(0, 37, true) == 0);      // the bottom
}

TEST_CASE("volume_follow: playback starting holds again, for a level restated with it") {
    VolumeFollow f;
    f.session_start(37, 37);
    f.tick(5000);                              // the session's hold long over
    f.play_start();
    CHECK(f.on_level(60, 37, true) == -1);    // restated as playback starts: not a press either
    f.tick(VolumeFollow::HOLD_MS - 1);
    CHECK(f.on_level(60, 37, true) == -1);
    f.tick(1);
    CHECK(f.on_level(54, 37, true) == 33);    // a press, counted from (60, 37)
}
