/**
 * @file fade_gate.h
 * @brief The curve every edge of the output follows: smootherstep, opening in 20 ms and closing in
 *        100 ms.
 *
 * Two of these shape what the audio task plays. One sits in front of the DSP chain, as the
 * envelope always did. The other sits after it, in front of the DAC, and that one is what makes a
 * source change, a pause or a reset silent.
 * A fade at the chain's input does not silence its output. The subsonic, the loudness shelf, a
 * resonant EQ band and the bass limiter's crossover keep ringing after the input has gone quiet,
 * for longer than the chain's look-ahead. Clearing them while they ring was the click. Behind the
 * output gate, a reset happens in silence, whatever the filters still hold.
 */

#pragma once

#include <cstddef>

namespace audio_core {

/// 6p^5 - 15p^4 + 10p^3: it leaves 0 and arrives at 1 with zero slope and zero curvature.
///
/// The curvature is what gets heard. A fade that is continuous in value but turns a corner, in
/// slope (a straight line) or in curvature (smoothstep, p^2(3 - 2p)), spreads that corner across the
/// spectrum. Measured with an 8th-order 500 Hz high-pass, as tests/host/test_out_gate.cpp
/// measures the gate: on a -12 dBFS 62 Hz tone faded over 10 ms, smoothstep leaves -73 dBFS of
/// splatter and this curve -83. Over 20 ms the two leave -85 and -101.
/// Never above 1: in float the polynomial can land an ulp over just short of p = 1, and a gate
/// after the brickwall must not undo its ceiling, even by that much.
inline float smootherstep(float p) {
    const float s = p * p * p * (p * (p * 6.0f - 15.0f) + 10.0f);
    return s < 1.0f ? s : 1.0f;
}

/// A gain that travels between shut (0) and open (1) along smootherstep, at any rate.
///
/// Closing is the slower half. Splatter is gone at 20 ms, but on loud bass a 20 ms stop still
/// sounds like the music being cut off, at every source change. 100 ms is a fade; it is also the
/// time a producer waits for its flush to be acknowledged (FLUSH_WAIT_NAPS in pcm_ring.h).
/// Opening stays at 20 ms, so a track still starts on its first note.
class FadeGate {
public:
    static constexpr float OPEN_S  = 0.020f;
    static constexpr float CLOSE_S = 0.100f;

    /// Per-frame steps at `hz`: times, not counts of frames.
    static float open_step(float hz)  { return hz >= 1000.0f ? 1.0f / (OPEN_S * hz) : 1.0f / 960.0f; }
    static float close_step(float hz) { return hz >= 1000.0f ? 1.0f / (CLOSE_S * hz) : 1.0f / 4800.0f; }

    void set_rate(float hz) { up_ = open_step(hz); down_ = close_step(hz); }

    /// Travel one block toward open or shut, multiplying the block along the way. Resting open, the
    /// gate is a wire. Resting shut, it zeroes the block.
    void run(float* stereo, size_t frames, bool open) {
        const float want = open ? 1.0f : 0.0f;
        if (p_ == want) {
            if (!open) for (size_t i = 0; i < frames * 2; i++) stereo[i] = 0.0f;
            return;
        }
        const float step = open ? up_ : -down_;
        for (size_t i = 0; i < frames; i++) {
            p_ += step;
            if (p_ > 1.0f) p_ = 1.0f;
            else if (p_ < 0.0f) p_ = 0.0f;
            const float g = smootherstep(p_);
            stereo[2 * i] *= g;
            stereo[2 * i + 1] *= g;
        }
    }

    bool  shut() const      { return p_ <= 0.0f; }
    bool  wide_open() const { return p_ >= 1.0f; }
    float position() const  { return p_; }
    /// Rest shut. Only correct when nothing is audible anyway.
    void  shut_now()        { p_ = 0.0f; }

private:
    float p_ = 0.0f;
    float up_ = 1.0f / 960.0f;
    float down_ = 1.0f / 4800.0f;
};

} // namespace audio_core
