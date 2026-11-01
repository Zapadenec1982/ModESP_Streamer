/**
 * @file test_usb_volume.cpp
 * @brief HOST tests for the computer's volume slider as a position
 *        (modules/usb_audio/include/usb_volume.h): the feature unit's −80…0 dB spread over 0…100.
 *        How that position moves the room: test_volume_follow.cpp.
 */

#include "doctest.h"
#include "../../modules/usb_audio/include/usb_volume.h"

using namespace usb_volume;

TEST_CASE("usb_volume: the slider's position spreads -80..0 dB over 0..100") {
    CHECK(position(MIN_256) == 0);
    CHECK(position(MIN_256 - 256) == 0);
    CHECK(position(MAX_256) == 100);
    CHECK(position(-40 * 256) == 50);
    CHECK(position(-20 * 256) == 75);
    CHECK(position(-79 * 256) == 1);
}
