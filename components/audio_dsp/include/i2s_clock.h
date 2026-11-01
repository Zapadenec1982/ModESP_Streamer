/**
 * @file i2s_clock.h
 * @brief What this chip's I2S can and cannot be clocked at, as arithmetic.
 *
 * Four facts decide whether a format is playable on this panel. None of them is a compile
 * error when you get it wrong, and only one of them produces a log line: the rest arrive as
 * noise in the speakers, which is why they are here as pure functions with tests rather than
 * as a comment somewhere in a driver.
 *
 *  1. On ESP32-P4 below chip revision 3 — and this panel is v1.3 — I2S_CLK_SRC_PLL_160M is
 *     not available, so I2S_CLK_SRC_DEFAULT is the 40 MHz crystal (hal/esp32p4/i2s_ll.h).
 *     That leaves exactly two sources: the crystal and the audio PLL.
 *  2. A SLAVE port still runs an internal MCLK to sample the clock arriving on the wire, and
 *     IDF fixes it at eight times the bit clock — bclk_div is clamped UP to 8 for a slave and
 *     cannot be set lower (esp_driver_i2s/i2s_std.c, i2s_std_calculate_clock).
 *  3. The source feeding that MCLK must be strictly above 1.99 x MCLK, or the configuration
 *     is refused with "sample rate is too large" (same function).
 *  4. The audio PLL is ONE frequency for the whole chip. The first holder sets it; while two
 *     or more hold it the frequency is frozen and every further request silently gets the
 *     frequency already there (esp_hw_support/clk_ctrl_os.c, periph_rtc_apll_freq_set, and
 *     i2s_common.c, i2s_set_get_apll_freq, which turns that refusal into a warning and
 *     returns the current frequency as if it had been granted).
 *
 * Put together: a master that holds the PLL decides what a slave sharing it can do. That is
 * the whole reason 32-bit slots from the Bluetooth satellite arrived as noise.
 */

#pragma once

#include <cstdint>

namespace audio_core {

/// The crystal on this panel, and I2S_CLK_SRC_DEFAULT with it while the chip is below rev 3.
constexpr uint32_t I2S_XTAL_HZ = 40000000;

/// The audio PLL's range on P4 (hal/esp32p4/clk_tree_ll.h).
constexpr uint32_t APLL_MIN_HZ = 5303031;
constexpr uint32_t APLL_MAX_HZ = 125000000;

/// The internal MCLK a SLAVE port will run at for this format. Fact 2: eight times the bit
/// clock, and the bit clock is one edge per bit of both slots.
constexpr uint32_t i2s_slave_mclk_hz(uint32_t rate_hz, uint8_t slot_bits) {
    return rate_hz * 2u * (uint32_t)slot_bits * 8u;
}

/// Fact 3, as a predicate: would IDF accept `sclk_hz` as the source for a slave at this
/// format? Integer arithmetic against IDF's float comparison, which agree to within a few Hz
/// at these frequencies — far inside any margin worth acting on.
constexpr bool i2s_slave_clock_fits(uint32_t sclk_hz, uint32_t rate_hz, uint8_t slot_bits) {
    return (uint64_t)sclk_hz * 100u > (uint64_t)i2s_slave_mclk_hz(rate_hz, slot_bits) * 199u;
}

/// How finely a SLAVE port may look at the clock on the wire: the largest `bclk_div` IDF will
/// take on source `sclk_hz`, or 0 when not even its minimum of 8 fits. Fact 2 is a floor, not
/// a fixed value — i2s_std_clk_config_t::bclk_div raises it — and the internal MCLK is
/// bclk × bclk_div, so every step up is one more look per bit at the incoming clock.
///
/// Eight looks per bit was not enough at 96 kHz in 32-bit slots: the P4 received ~0.15 % fewer
/// frames than the satellite sent (95 850 of 96 000 a second, on 27.09 and twice on 30.09,
/// unchanged by grounding the wires), while 48 kHz in the same slots lost nothing measurable.
///
/// Zero for exactly the formats i2s_slave_clock_fits() refuses, so the two never disagree.
constexpr uint32_t i2s_slave_bclk_div(uint32_t sclk_hz, uint32_t rate_hz, uint8_t slot_bits) {
    const uint64_t bclk = (uint64_t)rate_hz * 2u * slot_bits;
    if (!bclk || !sclk_hz) return 0;
    const uint64_t most = ((uint64_t)sclk_hz * 100u - 1u) / (bclk * 199u);
    if (most < 8u) return 0;
    // A whole source-to-MCLK divider of 2, when the numbers allow it, keeps the fractional
    // divider — and its cycle-to-cycle jitter — out of the clock that samples the wire.
    if ((uint64_t)sclk_hz % (2u * bclk) == 0) {
        const uint64_t whole = (uint64_t)sclk_hz / (2u * bclk);
        if (whole >= 8u) return (uint32_t)whole;
    }
    return (uint32_t)most;
}

/// Where the audio PLL ends up when something asks IDF for an MCLK of `mclk_hz`.
///
/// IDF does not let you ask for a PLL frequency. It takes the MCLK wanted and multiplies it
/// by the smallest whole number that clears the PLL's minimum, with 2 as the floor because a
/// divider of 1 is not a thing the hardware does (i2s_common.c, i2s_set_get_apll_freq). For
/// every MCLK this firmware uses that number is 2, so the PLL runs at exactly twice it.
///
/// Returns 0 when the result is above what the PLL can synthesise, which is IDF's answer too
/// — and IDF's answer, on the tuning path, is a channel left with its mutex held (see
/// i2s_trim_fits below), so the caller has to ask this question first rather than find out.
constexpr uint32_t i2s_apll_for_mclk(uint32_t mclk_hz) {
    if (!mclk_hz) return 0;
    uint32_t div = APLL_MIN_HZ / mclk_hz + 1u;
    if (div < 2u) div = 2u;
    const uint64_t hz = (uint64_t)mclk_hz * div;
    return hz > APLL_MAX_HZ ? 0u : (uint32_t)hz;
}

/// The same question asked the way a MASTER asks it: rate times MCLK multiple. Knowing the
/// answer is what lets the caller work out, before touching anything, whether a slave can
/// join the PLL the master is about to set.
constexpr uint32_t i2s_master_apll_hz(uint32_t rate_hz, uint16_t mclk_multiple) {
    return i2s_apll_for_mclk(rate_hz * (uint32_t)mclk_multiple);
}

// ── trimming the output clock, in parts per million ───────────────────────────────────────
//
// The difference between two crystals does not have to be eaten by a resampler; it can be
// eaten by the clock. The satellite is the I2S master of the input port and this panel is
// its slave, so the samples arrive at the satellite's rate whatever the P4 thinks — and the
// P4's own output runs a few parts per million away from it. A resampler can absorb that,
// at 192 float multiply-accumulates per output frame and about a quarter of core 1. Moving
// the OUTPUT clock those few ppm instead absorbs it for nothing, and makes the Bluetooth
// path bit-exact on the way through (docs/AUDIO.md, "Playback").

/// How far the clock may be trimmed, either way.
///
/// The measured drift is 4–22 ppm and the satellite's own APLL sits +40 ppm off nominal, so
/// 100 ppm is between two and twenty-five times the authority the job needs — enough margin
/// to be a servo rather than a limiter, and small enough to be inaudible: 100 ppm is 0.17 of
/// a cent, where the smallest pitch change a trained ear resolves is about five.
///
/// It is also what the DAC and the slave port will tolerate without noticing. The PCM5102A
/// locks its internal PLL to the bit clock and does not care at this scale, and the slave
/// port's source-clock margin is half a percent (see the test), which is fifty times this.
constexpr int32_t I2S_TRIM_MAX_PPM = 100;

/// MCLK offset in whole hertz for a trim of `ppm`, rounded rather than truncated.
constexpr int32_t i2s_mclk_delta_hz(uint32_t mclk_hz, int32_t ppm) {
    const int64_t scaled = (int64_t)mclk_hz * ppm;
    const int64_t half   = scaled >= 0 ? 500000 : -500000;
    return (int32_t)((scaled + half) / 1000000);
}

/// Whether IDF's tuning path will survive being asked for this trim.
///
/// i2s_channel_tune_rate computes the new PLL from the new MCLK, then divides one by the
/// other. If that division comes out below 2 — which is what happens when the PLL could not
/// be set, because the frequency it hands back is then zero — the function returns
/// ESP_ERR_INVALID_ARG **while still holding the channel's mutex** (i2s_common.c:1496).
/// Nothing ever gives that mutex back, so the next disable, enable or reconfigure on that
/// channel blocks for the rest of the uptime: the output stops and cannot be restarted. It
/// is not a failure that can be handled afterwards, so it is refused before.
constexpr bool i2s_trim_fits(uint32_t mclk_hz, int32_t ppm) {
    if (!mclk_hz) return false;
    if (ppm > I2S_TRIM_MAX_PPM || ppm < -I2S_TRIM_MAX_PPM) return false;
    const int64_t tuned = (int64_t)mclk_hz + i2s_mclk_delta_hz(mclk_hz, ppm);
    if (tuned <= 0 || tuned > (int64_t)APLL_MAX_HZ) return false;
    return i2s_apll_for_mclk((uint32_t)tuned) != 0;
}

/// The servo law: how far to trim the output clock for a queue that is `err_frames` away
/// from where it should be.
///
/// Proportional, and deliberately only proportional. The queue level is already the integral
/// of the rate difference, so a proportional term alone closes the loop; what it leaves is a
/// standing offset of drift/gain, which at 22 ppm of drift and this gain is nine
/// milliseconds of queue — latency nobody can hear, in exchange for a loop with nothing in
/// it that can wind up.
///
/// `full_at_ms` is where the servo reaches full authority. Correcting an error at 100 ppm
/// takes err/100ppm of time — forty milliseconds of queue needs four hundred SECONDS — so
/// this loop cannot be a capture loop and is not asked to be: it holds a queue that starts
/// at its set point. Anything that throws the queue further than that is a dropout, and
/// dropouts are what the pre-buffer and the resync jump are for.
constexpr int32_t i2s_trim_ppm(int32_t err_frames, uint32_t rate_hz, uint32_t full_at_ms) {
    if (!rate_hz || !full_at_ms) return 0;
    // Ring too full ⇒ the output is consuming too slowly ⇒ run it faster. The sign is the
    // opposite of the resampler's ratio, which slows DOWN to consume more.
    const int64_t num = (int64_t)err_frames * 1000 * I2S_TRIM_MAX_PPM;
    const int64_t den = (int64_t)rate_hz * full_at_ms;
    int64_t ppm = num / den;
    if (ppm >  I2S_TRIM_MAX_PPM) ppm =  I2S_TRIM_MAX_PPM;
    if (ppm < -I2S_TRIM_MAX_PPM) ppm = -I2S_TRIM_MAX_PPM;
    return (int32_t)ppm;
}

/// What the MASTER has to run at for a slave stream of (rate_hz, slot_bits) to be clockable
/// at all. `out_rate_hz` of 0 means there is no such setting.
struct SlaveClockPlan {
    uint32_t out_rate_hz   = 0;
    uint16_t mclk_multiple = 0;
};

/// Master MCLK multiples tried for a slave stream, highest PLL first. Every one is a multiple
/// of 64, so a master in 32-bit slots still divides its MCLK down to the bit clock exactly.
inline constexpr uint16_t SLAVE_PLAN_MULTIPLES[] = {1280, 1152, 1024, 768, 640, 576, 512};

/// Work out that setting.
///
/// In the order they are preferred:
///
///  1. The crystal carries the format. Nothing changes and nothing touches the PLL — this is
///     16-bit streams at 44.1/48 kHz.
///  2. Otherwise the PLL, as FAST as the chip makes it: the highest multiple whose PLL (twice
///     the master's MCLK) stays under 125 MHz. The slave's margin is the PLL's headroom over
///     1.99 x its own MCLK, and it takes that headroom as extra looks per bit
///     (i2s_slave_bclk_div). 512x — the old answer — left exactly eight at every rate: 96 kHz /
///     32-bit now gets 640x, a 122.88 MHz PLL and ten; 48 kHz gets 1280x and twenty.
///  3. The board's own multiple, when nothing above fits (192 kHz in 16-bit slots).
///
/// A fast MCLK is free on this board only because nothing is listening to it: the DAC takes
/// none (its PLL locks to the bit clock) and the ADC is not fitted — and the port stops
/// driving the pin at all outside AUX (IAudioPort::set_line_input). It also lowers the
/// master's own ceiling, which is why it is asked for per format rather than set once.
///
/// `board_mclk_multiple` is what the board configures for ordinary playback, and `max_out_hz`
/// what the output clock can be moved to; both come from the port rather than from a guess.
constexpr SlaveClockPlan i2s_plan_for_slave(uint32_t rate_hz, uint8_t slot_bits,
                                            uint16_t board_mclk_multiple, uint32_t max_out_hz) {
    if (!rate_hz || !slot_bits || rate_hz > max_out_hz) return {};
    if (i2s_slave_clock_fits(I2S_XTAL_HZ, rate_hz, slot_bits))
        return {rate_hz, board_mclk_multiple};
    for (const uint16_t m : SLAVE_PLAN_MULTIPLES) {
        const uint32_t pll = i2s_master_apll_hz(rate_hz, m);
        if (pll && i2s_slave_clock_fits(pll, rate_hz, slot_bits)) return {rate_hz, m};
    }
    if (i2s_slave_clock_fits(i2s_master_apll_hz(rate_hz, board_mclk_multiple), rate_hz, slot_bits))
        return {rate_hz, board_mclk_multiple};
    return {};
}

}  // namespace audio_core
