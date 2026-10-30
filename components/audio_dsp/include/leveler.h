/**
 * @file leveler.h
 * @brief «Вирівнювання гучності»: a slow automatic gain at the head of the DSP chain that holds
 *        what plays near one loudness, and the schedule that switches it.
 *
 * The case it is for: a film mixed quiet, with room for its explosions, and advertising squeezed
 * to the loudest it can be. Set the volume for the dialogue, and the break comes in far louder,
 * then the film goes back to quiet. The leveler measures the loudness the way ITU-R BS.1770
 * does (K-weighted mean square) and turns the source down when it is louder than LEVEL_LUFS, up
 * when it is quieter — down within about half a second, up only after LEV_HOLD_S of nothing
 * louder and then at LEV_RISE_DB_S, so the dialogue's pauses do not pump. What the TV, AV
 * receivers and soundbars ship as a night mode (Audyssey Dynamic Volume, Sonos Night Sound).
 *
 * It runs before the volume, so the level it holds does not depend on where the volume is: the
 * volume then sets how loud that level plays.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "dsp_kernels.h"

namespace audio_core {

class Leveler {
public:
    /// The loudness it holds, LUFS of the source's full scale (EBU R128 programmes sit at −23,
    /// music services at −14…−16, a film's dialogue at −24…−31).
    static constexpr float LEV_TARGET_LUFS = -20.0f;
    /// The most it lifts something quiet, and the most it takes off something loud.
    static constexpr float LEV_BOOST_DB = 6.0f;
    static constexpr float LEV_CUT_DB = 20.0f;
    /// The loudness over the last ~0.4 s (BS.1770's momentary window is 400 ms) sets the level;
    /// over the last ~50 ms it catches a bang after a quiet scene, LEV_MARGIN_DB above the level.
    static constexpr float LEV_SLOW_S = 0.4f;
    static constexpr float LEV_FAST_S = 0.05f;
    static constexpr float LEV_MARGIN_DB = 8.0f;
    /// Down: a one-pole on the gain in dB.
    static constexpr float LEV_ATTACK_S = 0.1f;
    /// Up: not before this long without a cut, then no faster than LEV_RISE_DB_S.
    static constexpr float LEV_HOLD_S = 1.5f;
    static constexpr float LEV_RISE_DB_S = 3.0f;
    /// A block quieter than this is a pause: it is not measured and the gain stays where it is.
    static constexpr float LEV_GATE_LUFS = -60.0f;

    Leveler() { set_sample_rate(48000.0f); }

    /// The K-weighting at `hz`. The state is cleared.
    void set_sample_rate(float hz);
    /// On, or off. Switched off, the gain goes back to 0 dB at LEV_RISE_DB_S and the stage is then
    /// a wire, bit-exact and free; switched on, it measures from scratch.
    void set_on(bool on);
    bool on() const { return on_; }
    /// A new signal (a source switch, a new clock): the measurement starts again; the gain stays,
    /// so a loud source followed by another starts no louder than the first one was playing.
    void reset();
    /// Interleaved stereo, in place. The gain for a block is the one the block before it decided.
    void process(float* stereo, size_t frames);
    /// The gain now, dB (> 0 lifts, < 0 cuts).
    float gain_db() const { return g_db_; }
    /// False once anything in it stopped being a number.
    bool sane() const;

    /// BS.1770's K-weighting as two trapezoidal SVF sections at `fs`: the pre-filter's high shelf
    /// (+4 dB above ~1.7 kHz, the head's effect) and the RLB high-pass at 38 Hz.
    static void k_weighting(double fs, SvfCoef& shelf, SvfCoef& hp);

private:
    void decide(float ms, float dt);

    bool     on_ = false;
    float    fs_ = 48000.0f;
    SvfCoef  kw_shelf_, kw_hp_;
    float    s_[4] = {};             ///< the two sections' states (one channel: the mono sum)
    float    ps_ = 0.0f, pf_ = 0.0f;  ///< mean square, slow and fast
    float    hold_ = LEV_HOLD_S;     ///< seconds left before the gain may rise
    float    g_db_ = 0.0f;           ///< the gain decided
    float    lin_ = 1.0f;            ///< the gain applied at the end of the last block
    float    lin_to_ = 1.0f;         ///< where this block ramps to
};

/// When the schedule switches the leveler: once when it starts to apply (the clock is known and
/// the schedule is on, or it was moved), and on every crossing of its edges after that. Between
/// them a hand on the switch is left alone.
class LevelerSchedule {
public:
    /// `now_min`: minute of the day, −1 while the clock is not set. `from_min`, `to_min`: the
    /// window [from, to) in minutes of the day; it may run across midnight, and from == to is an
    /// empty window. Returns 1 to switch the leveler on, 0 to switch it off, −1 to leave it.
    int tick(int now_min, bool enabled, int from_min, int to_min);
    static bool inside(int now_min, int from_min, int to_min);

private:
    int last_ = -1;                  ///< inside (1) or not (0) at the last tick that applied
    int from_ = -1, to_ = -1;
};

}  // namespace audio_core
