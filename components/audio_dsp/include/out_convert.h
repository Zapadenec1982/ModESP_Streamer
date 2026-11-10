/**
 * @file out_convert.h
 * @brief The last step before the DAC: the chain's float samples as 32-bit words.
 *
 * Here, in the library that is built -O2, rather than in the audio module: it runs for every
 * sample that is played, and it can be tested on the host.
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace audio_core {

/// The word sent in place of a digital zero: 1 LSB of 24 bits. The PCM5102A mutes its output
/// after ~21 ms of exact zeros, and coming out of that mute is a click.
constexpr int32_t DAC_KEEPALIVE = 1 << 8;

/// `n` float samples to int32 at `scale` (truncating), clipped to ±1; a NaN becomes silence, not
/// −1; a zero becomes DAC_KEEPALIVE. Returns how many samples were outside ±1.00001 or NaN —
/// none, after the brickwall.
uint32_t f32_to_i32(const float* in, int32_t* out, size_t n, float scale);

}  // namespace audio_core
