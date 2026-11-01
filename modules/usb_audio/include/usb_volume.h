/**
 * @file usb_volume.h
 * @brief The computer's volume slider as a position, 0…100: the sender's level that the room's
 *        follows by the rule AirPlay keeps too (audio_dsp's volume_follow.h).
 *
 * The computer reports decibels (−80…0, the feature unit's range in uac_usb.c); position()
 * spreads them evenly over 0…100, which is how the OS draws its slider against this range.
 */

#pragma once

#include <cstdint>

namespace usb_volume {

/// The feature unit's range, 1/256 dB (uac_usb.c: VOL_MIN, VOL_MAX).
constexpr int32_t MIN_256 = -80 * 256;
constexpr int32_t MAX_256 = 0;

/// The slider's position, 0..100, from the host's level in 1/256 dB.
inline int32_t position(int32_t db256) {
    if (db256 <= MIN_256) return 0;
    if (db256 >= MAX_256) return 100;
    return ((db256 - MIN_256) * 100 + (MAX_256 - MIN_256) / 2) / (MAX_256 - MIN_256);
}

}  // namespace usb_volume
