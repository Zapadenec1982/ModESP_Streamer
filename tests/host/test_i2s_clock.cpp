/**
 * @file test_i2s_clock.cpp
 * @brief The clock arithmetic that decides whether a format reaches the speakers as music.
 *
 * Every case here was a real symptom first. The 32-bit ones are the day the satellite started
 * sending aptX HD in 32-bit slots and the panel played noise: the host had advertised that it
 * could take them, and the arithmetic below says under exactly which condition that is true.
 */

#include "doctest.h"
#include "i2s_clock.h"

using namespace audio_core;

TEST_CASE("slave MCLK is eight times the bit clock") {
    // IDF clamps a slave's bclk_div up to 8 and will not take less.
    CHECK(i2s_slave_mclk_hz(44100, 16) == 11289600u);
    CHECK(i2s_slave_mclk_hz(48000, 16) == 12288000u);
    CHECK(i2s_slave_mclk_hz(44100, 32) == 22579200u);
    CHECK(i2s_slave_mclk_hz(48000, 32) == 24576000u);
    CHECK(i2s_slave_mclk_hz(96000, 32) == 49152000u);
}

TEST_CASE("the crystal carries 16-bit slots to 78.5 kHz and no further") {
    CHECK(i2s_slave_clock_fits(I2S_XTAL_HZ, 44100, 16));
    CHECK(i2s_slave_clock_fits(I2S_XTAL_HZ, 48000, 16));
    CHECK(i2s_slave_clock_fits(I2S_XTAL_HZ, 78000, 16));
    CHECK_FALSE(i2s_slave_clock_fits(I2S_XTAL_HZ, 79000, 16));
    CHECK_FALSE(i2s_slave_clock_fits(I2S_XTAL_HZ, 96000, 16));
}

TEST_CASE("the crystal cannot do 32-bit slots at any audio rate") {
    // 40 MHz against 1.99 x 22.58 MHz. Not close, and not a question of margins.
    CHECK_FALSE(i2s_slave_clock_fits(I2S_XTAL_HZ, 44100, 32));
    CHECK_FALSE(i2s_slave_clock_fits(I2S_XTAL_HZ, 48000, 32));
    // Far enough down it would fit, but nothing sends 32-bit slots at 32 kHz.
    CHECK(i2s_slave_clock_fits(I2S_XTAL_HZ, 32000, 32));
}

TEST_CASE("the master sets the PLL to twice its own MCLK") {
    CHECK(i2s_master_apll_hz(44100, 256) == 22579200u);
    CHECK(i2s_master_apll_hz(48000, 256) == 24576000u);
    CHECK(i2s_master_apll_hz(88200, 256) == 45158400u);
    CHECK(i2s_master_apll_hz(96000, 256) == 49152000u);
    CHECK(i2s_master_apll_hz(192000, 256) == 98304000u);
    // Above 192 kHz at 256x the PLL runs out of range, and so does the master.
    CHECK(i2s_master_apll_hz(384000, 256) == 0u);
    // Low rates need a bigger multiplier to clear the PLL's floor, and IDF applies one.
    CHECK(i2s_master_apll_hz(8000, 256) == 6144000u);
}

TEST_CASE("the live failure: a 44.1 kHz master cannot clock a 48 kHz 32-bit slave") {
    // This is the panel as it stood: output on 44100, satellite announcing 48000 in 32-bit
    // slots. The PLL was frozen at 22.5792 MHz by the master, the slave needed above 48.9,
    // IDF refused the reconfiguration, and the channel stayed 16-bit against a 32-bit wire —
    // one frame read as two, left and right glued into one word.
    const uint32_t pll = i2s_master_apll_hz(44100, 256);
    CHECK_FALSE(i2s_slave_clock_fits(pll, 48000, 32));
    // And it would not have worked at the announced rate either, which is the part that
    // matters: retuning the output to 48000 first would have changed nothing.
    CHECK_FALSE(i2s_slave_clock_fits(i2s_master_apll_hz(48000, 256), 48000, 32));
}

TEST_CASE("the master sets the PLL to twice its MCLK at 512x too") {
    CHECK(i2s_master_apll_hz(44100, 512) == 45158400u);
    CHECK(i2s_master_apll_hz(48000, 512) == 49152000u);
    CHECK(i2s_master_apll_hz(96000, 512) == 98304000u);
    // And this is what it costs: at 512x the master itself stops above ~122 kHz.
    CHECK(i2s_master_apll_hz(192000, 512) == 0u);
}

TEST_CASE("32-bit slots are paid for with the fastest PLL the chip makes, not a faster output") {
    // The output stays where the stream is; the PLL goes as high as 125 MHz allows, and the
    // slave spends the headroom on looks per bit. 512x left exactly eight at every rate, and
    // eight lost ~0.15 % of the frames at 96 kHz on the bench.
    struct Want { uint32_t hz; uint16_t mult; uint32_t pll; uint32_t looks; };
    const Want wants[] = {
        {44100u, 1280, 112896000u, 20},
        {48000u, 1280, 122880000u, 20},
        {88200u,  640, 112896000u, 10},
        {96000u,  640, 122880000u, 10},
    };
    for (const Want& w : wants) {
        const SlaveClockPlan p = i2s_plan_for_slave(w.hz, 32, 256, 192000);
        REQUIRE(p.out_rate_hz == w.hz);
        CHECK(p.mclk_multiple == w.mult);
        const uint32_t pll = i2s_master_apll_hz(p.out_rate_hz, p.mclk_multiple);
        CHECK(pll == w.pll);
        CHECK(i2s_slave_clock_fits(pll, w.hz, 32));
        CHECK(i2s_slave_bclk_div(pll, w.hz, 32) == w.looks);
        // And the master still divides its own MCLK to a 32-bit bit clock exactly.
        CHECK(p.mclk_multiple % 64 == 0);
    }
}

TEST_CASE("16-bit slots on the crystal leave the board's multiple alone") {
    // Output == input, no PLL touched, the board's own MCLK multiple kept — and the crystal
    // now gives the slave thirteen or fourteen looks per bit instead of eight.
    CHECK(i2s_plan_for_slave(44100, 16, 256, 192000).mclk_multiple == 256);
    CHECK(i2s_plan_for_slave(48000, 16, 256, 192000).mclk_multiple == 256);
    CHECK(i2s_slave_bclk_div(I2S_XTAL_HZ, 44100, 16) == 14);
    CHECK(i2s_slave_bclk_div(I2S_XTAL_HZ, 48000, 16) == 13);
    // 96 kHz in 16-bit slots is beyond the crystal and takes the fast PLL like 32-bit does.
    const SlaveClockPlan p = i2s_plan_for_slave(96000, 16, 256, 192000);
    CHECK(p.out_rate_hz == 96000u);
    CHECK(p.mclk_multiple == 640);
    CHECK(i2s_slave_bclk_div(i2s_master_apll_hz(96000, 640), 96000, 16) == 20);
}

TEST_CASE("the slave's looks per bit agree with what IDF accepts") {
    // Zero exactly where i2s_slave_clock_fits() says no, so a divider this returns can never
    // be refused by i2s_std_calculate_clock.
    const uint32_t sources[] = {I2S_XTAL_HZ, 24576000u, 49152000u, 98304000u, 112896000u, 122880000u};
    const uint32_t rates[] = {8000u, 16000u, 32000u, 44100u, 48000u, 88200u, 96000u, 192000u};
    const uint8_t widths[] = {16, 32};
    for (uint32_t s : sources)
        for (uint32_t hz : rates)
            for (uint8_t bits : widths)
                CHECK((i2s_slave_bclk_div(s, hz, bits) >= 8) == i2s_slave_clock_fits(s, hz, bits));
    // The old plan's PLL gave exactly the minimum; the new one ten.
    CHECK(i2s_slave_bclk_div(98304000u, 96000, 32) == 8);
    CHECK(i2s_slave_bclk_div(122880000u, 96000, 32) == 10);
    CHECK(i2s_slave_bclk_div(49152000u, 96000, 32) == 0);
}

TEST_CASE("no answer is better than a wrong one") {
    CHECK(i2s_plan_for_slave(0, 32, 256, 192000).out_rate_hz == 0u);
    CHECK(i2s_plan_for_slave(48000, 0, 256, 192000).out_rate_hz == 0u);
    // 192 kHz in 32-bit slots needs a PLL above 196 MHz, and the chip makes 125. Silence with
    // a reason, not noise.
    CHECK(i2s_plan_for_slave(192000, 32, 256, 192000).out_rate_hz == 0u);
    // And a rate the output cannot reach at all is refused before any of this matters.
    CHECK(i2s_plan_for_slave(96000, 16, 256, 48000).out_rate_hz == 0u);
}

TEST_CASE("a parked input port keeps its slot width, and the crystal can still clock it") {
    // Leaving Bluetooth parks the input port on the crystal so it stops holding the audio
    // PLL. What it must NOT do is narrow the slots: IDF reallocates the DMA descriptors
    // only when the buffer size changes, and that size follows the slot width alone — so a
    // port that narrows to 16 bits hands its wide buffers back to a heap that, once Wi-Fi
    // and a stream have had their share, cannot produce them again. The panel logged that as
    // ESP_ERR_NO_MEM, 16875 bytes free, largest block 6144, Bluetooth silent until reboot.
    //
    // So the park rate is chosen for the width the port is already at. This pins that the
    // choice is possible at all, which is the part that is not obvious: at 32 bits the
    // crystal runs out below 40 kHz.

    // A narrow port parks where it always did.
    CHECK(i2s_slave_clock_fits(I2S_XTAL_HZ, 48000, 16));

    // A wide port cannot park there — this is why the rate has to move instead of the width.
    CHECK_FALSE(i2s_slave_clock_fits(I2S_XTAL_HZ, 48000, 32));

    // ...and 32 kHz is where it can, with margin rather than by a hair.
    CHECK(i2s_slave_clock_fits(I2S_XTAL_HZ, 32000, 32));

    // The boundary itself, so a future change of crystal or MCLK ratio shows up here and
    // not as silence on the bench: 1.99 x (rate x 2 x 32 x 8) must stay under 40 MHz.
    CHECK(i2s_slave_clock_fits(I2S_XTAL_HZ, 39000, 32));
    CHECK_FALSE(i2s_slave_clock_fits(I2S_XTAL_HZ, 40000, 32));
}

// ── trimming the output clock in parts per million ────────────────────────────────────────
//
// The satellite is the I2S master of the input port and this panel its slave, so the samples
// arrive at the satellite's rate whatever the P4 believes. Today a resampler eats the
// difference between the two crystals; these are the numbers that let the CLOCK eat it
// instead. Every one of them is a refusal the driver has to make before touching hardware,
// because the failures on the other side of it are a panic and a permanently held mutex.

TEST_CASE("the PLL question asked from an MCLK is the same one asked from a rate") {
    // i2s_master_apll_hz is now i2s_apll_for_mclk with the multiplication done for you. The
    // refactor has to be invisible, and a trim asks the MCLK form because the trimmed MCLK
    // is not any rate times any multiple.
    CHECK(i2s_apll_for_mclk(48000u * 256u) == i2s_master_apll_hz(48000, 256));
    CHECK(i2s_apll_for_mclk(96000u * 512u) == i2s_master_apll_hz(96000, 512));
    CHECK(i2s_apll_for_mclk(192000u * 512u) == 0u);
    CHECK(i2s_apll_for_mclk(0) == 0u);
}

TEST_CASE("a trim in ppm becomes whole hertz of MCLK, rounded both ways") {
    // 12.288 MHz is 48 kHz at 256x — the ordinary output MCLK on this board.
    CHECK(i2s_mclk_delta_hz(12288000, 100) == 1229);     // 1228.8 rounds up
    CHECK(i2s_mclk_delta_hz(12288000, -100) == -1229);   // and away from zero the other way
    CHECK(i2s_mclk_delta_hz(12288000, 0) == 0);
    CHECK(i2s_mclk_delta_hz(12288000, 1) == 12);
    // The widest MCLK this firmware ever runs: 192 kHz at 256x, and 96 kHz at 512x with it.
    CHECK(i2s_mclk_delta_hz(49152000, 100) == 4915);
}

TEST_CASE("the full authority is available at every clock this board runs") {
    // Both ends, because a servo that can only move one way is not a servo. The rates are
    // the ones this board actually clocks: the two families and the two MCLK multiples.
    struct { uint32_t rate; uint16_t mult; } cases[] = {
        {44100, 256}, {48000, 256}, {88200, 256}, {96000, 256}, {192000, 256},
        {44100, 512}, {48000, 512}, {96000, 512},
    };
    for (auto c : cases) {
        const uint32_t mclk = c.rate * (uint32_t)c.mult;
        REQUIRE(i2s_master_apll_hz(c.rate, c.mult) != 0u);
        CHECK(i2s_trim_fits(mclk,  I2S_TRIM_MAX_PPM));
        CHECK(i2s_trim_fits(mclk, -I2S_TRIM_MAX_PPM));
        CHECK(i2s_trim_fits(mclk, 0));
    }
}

TEST_CASE("a trim beyond the authority, or beyond the PLL, is refused rather than clamped") {
    CHECK_FALSE(i2s_trim_fits(12288000, I2S_TRIM_MAX_PPM + 1));
    CHECK_FALSE(i2s_trim_fits(12288000, -(I2S_TRIM_MAX_PPM + 1)));
    CHECK_FALSE(i2s_trim_fits(0, 0));
    // An MCLK whose PLL is already out of range. i2s_set_get_apll_freq answers 0 for it, the
    // division that follows comes out below two, and i2s_channel_tune_rate then returns
    // ESP_ERR_INVALID_ARG **holding the channel mutex** (i2s_common.c:1496) — after which
    // nothing can stop, start or retune that channel again. Refusing here is the only cure.
    REQUIRE(i2s_apll_for_mclk(70000000) == 0u);
    CHECK_FALSE(i2s_trim_fits(70000000, 10));
}

TEST_CASE("the slave's source clock survives the trim at 96 kHz in 32-bit slots") {
    // The tightest case on this board. The output runs at 512x so the PLL is 1024 times the
    // rate, and a 32-bit slave needs 1018.88 times it — half a percent of margin, which is
    // taken to cover 100 ppm. It does, by a factor of fifty, and this checks it rather than
    // assuming it.
    const uint32_t pll = i2s_master_apll_hz(96000, 512);
    REQUIRE(pll == 98304000u);
    REQUIRE(i2s_slave_clock_fits(pll, 96000, 32));

    // Trimmed to both limits, and it is the DOWNWARD one that could bite.
    const uint32_t low  = pll - (uint32_t)((uint64_t)pll * I2S_TRIM_MAX_PPM / 1000000u);
    const uint32_t high = pll + (uint32_t)((uint64_t)pll * I2S_TRIM_MAX_PPM / 1000000u);
    CHECK(i2s_slave_clock_fits(low,  96000, 32));
    CHECK(i2s_slave_clock_fits(high, 96000, 32));

    // And the margin itself, so a future change of multiple or crystal shows up here rather
    // than as a slave port that will not take the format: the PLL may fall 0.5 % before the
    // format stops fitting, and the trim only ever moves it a fiftieth of that.
    const uint32_t need = (uint32_t)((uint64_t)i2s_slave_mclk_hz(96000, 32) * 199u / 100u);
    CHECK(pll > need);
    CHECK((pll - need) * 1000u / pll >= 5u);          // ≥ 0.5 %
    CHECK((uint64_t)I2S_TRIM_MAX_PPM * 10u < (uint64_t)(pll - need) * 1000000u / pll);

    // The same at 48 kHz in 32-bit slots, which is what the satellite sends today.
    const uint32_t pll48 = i2s_master_apll_hz(48000, 512);
    REQUIRE(i2s_slave_clock_fits(pll48, 48000, 32));
    CHECK(i2s_slave_clock_fits(pll48 - pll48 / 10000u, 48000, 32));
}

TEST_CASE("the servo runs the clock the opposite way to the resampler's ratio") {
    // Ring fuller than the set point means the output is consuming too slowly, so the clock
    // speeds UP. The resampler's ratio goes the other way — it slows down to consume more —
    // and mixing the two signs up is a loop that runs to its limit instead of to its target.
    constexpr uint32_t FULL_MS = 40;
    CHECK(i2s_trim_ppm(+480, 48000, FULL_MS) > 0);    // +10 ms of queue
    CHECK(i2s_trim_ppm(-480, 48000, FULL_MS) < 0);
    CHECK(i2s_trim_ppm(0, 48000, FULL_MS) == 0);
}

TEST_CASE("the servo reaches full authority exactly where the tracking band ends") {
    // 40 ms of error is where the resampler's own authority used to stop being enough, and
    // it is where the clock's stops too: the same band, so the two describe one loop.
    constexpr uint32_t FULL_MS = 40;
    CHECK(i2s_trim_ppm(48000 * 40 / 1000, 48000, FULL_MS) == I2S_TRIM_MAX_PPM);
    CHECK(i2s_trim_ppm(-(48000 * 40 / 1000), 48000, FULL_MS) == -I2S_TRIM_MAX_PPM);
    // Half way there, half the authority.
    CHECK(i2s_trim_ppm(48000 * 20 / 1000, 48000, FULL_MS) == I2S_TRIM_MAX_PPM / 2);
    // Past it the answer saturates rather than growing; a whole ring of error is still 100.
    CHECK(i2s_trim_ppm(48000, 48000, FULL_MS) == I2S_TRIM_MAX_PPM);
    CHECK(i2s_trim_ppm(-48000, 48000, FULL_MS) == -I2S_TRIM_MAX_PPM);
}

TEST_CASE("the servo is measured in milliseconds of queue, not in frames") {
    // The same error in TIME has to give the same correction at every rate, or a 96 kHz
    // stream would be steered twice as hard as a 48 kHz one for the same drift.
    constexpr uint32_t FULL_MS = 40;
    const int32_t at_48  = i2s_trim_ppm((int32_t)(48000u * 10 / 1000), 48000, FULL_MS);
    const int32_t at_96  = i2s_trim_ppm((int32_t)(96000u * 10 / 1000), 96000, FULL_MS);
    const int32_t at_441 = i2s_trim_ppm((int32_t)(44100u * 10 / 1000), 44100, FULL_MS);
    CHECK(at_48 == at_96);
    CHECK(at_48 == at_441);
    CHECK(at_48 == I2S_TRIM_MAX_PPM / 4);            // 10 ms of 40
}

TEST_CASE("the servo answers nothing rather than dividing by zero") {
    CHECK(i2s_trim_ppm(1000, 0, 40) == 0);
    CHECK(i2s_trim_ppm(1000, 48000, 0) == 0);
}

TEST_CASE("the drift the servo exists to absorb is well inside its authority") {
    // The satellite measured its own APLL at +40 ppm and the bench saw 4–22 ppm of relative
    // drift. This is the sanity check that the authority was chosen against the measurement
    // rather than against a round number: the worst measured case must not saturate it.
    constexpr int32_t WORST_MEASURED_PPM = 40;
    CHECK(WORST_MEASURED_PPM * 2 < I2S_TRIM_MAX_PPM);
}
