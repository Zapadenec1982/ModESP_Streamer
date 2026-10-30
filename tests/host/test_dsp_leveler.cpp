/**
 * @file test_dsp_leveler.cpp
 * @brief HOST tests for «Вирівнювання гучності» (components/audio_dsp, leveler.h): the
 *        K-weighting is BS.1770's at 48 kHz and reads LUFS at every rate; a loud break is turned
 *        down within a second and the film comes back up after the hold at no more than the rise's
 *        pace; pauses and silence leave the gain alone; a bang after a quiet scene is caught by the
 *        fast detector; off is a wire, and switched off it glides back first; in the chain it runs
 *        ahead of the volume and survives a NaN; the schedule switches at its edges only.
 */

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>
#include "doctest.h"
#include "dsp_chain.h"
#include "leveler.h"

using namespace audio_core;

namespace {

constexpr size_t BLK = DspChain::BLOCK;

double db(double x) { return 20.0 * std::log10(x); }

/// |H| of a biquad b/a at f, dB.
double biquad_db(const double b[3], const double a[3], double f, double fs) {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * M_PI * f / fs), z2 = z1 * z1;
    return db(std::abs((b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2)));
}

/// |H| of an SVF section at f, dB, measured: a sine through it, its RMS against the input's once
/// it settles (a peak would miss the crest a few samples a period never land on).
double svf_db(const SvfCoef& c, double f, double fs) {
    float s1 = 0.0f, s2 = 0.0f;
    const int settle = (int)(fs * 0.5), n = (int)(fs * 0.5);
    double in = 0.0, out = 0.0;
    for (int i = 0; i < settle + n; i++) {
        const double x = std::sin(2.0 * M_PI * f * i / fs);
        const float y = svf_tick((float)x, c, s1, s2);
        if (i >= settle) { in += x * x; out += (double)y * y; }
    }
    return 10.0 * std::log10(out / in);
}

/// A 997 Hz sine, the same in both channels, `level_db` dBFS peak, from sample `t0` on.
void sine(float* x, size_t frames, double level_db, double fs, uint64_t& t) {
    const double a = std::pow(10.0, level_db / 20.0);
    for (size_t i = 0; i < frames; i++, t++) {
        const float v = (float)(a * std::sin(2.0 * M_PI * 997.0 * (double)t / fs));
        x[2 * i] = x[2 * i + 1] = v;
    }
}

/// Feed `seconds` of a sine at `level_db` (or silence for −999) and return the gain after every
/// block.
std::vector<float> run(Leveler& lv, double level_db, double seconds, double fs, uint64_t& t) {
    std::vector<float> g;
    float x[BLK * 2];
    const size_t blocks = (size_t)std::lround(seconds * fs / BLK);
    for (size_t b = 0; b < blocks; b++) {
        if (level_db < -900.0) { std::memset(x, 0, sizeof(x)); t += BLK; }
        else sine(x, BLK, level_db, fs, t);
        lv.process(x, BLK);
        g.push_back(lv.gain_db());
    }
    return g;
}

/// The block index `seconds` into a run.
size_t at(double seconds, double fs) { return (size_t)std::lround(seconds * fs / BLK) - 1; }

}  // namespace

TEST_CASE("leveler: the K-weighting is BS.1770's at 48 kHz") {
    // ITU-R BS.1770-4, Tables 1 and 2: the pre-filter and the RLB filter at 48 kHz.
    const double b1[3] = {1.53512485958697, -2.69169618940638, 1.19839281085285};
    const double a1[3] = {1.0, -1.69065929318241, 0.73248077421585};
    const double b2[3] = {1.0, -2.0, 1.0};
    const double a2[3] = {1.0, -1.99004745483398, 0.99007225036621};
    SvfCoef shelf, hp;
    Leveler::k_weighting(48000.0, shelf, hp);
    for (double f : {20.0, 31.5, 50.0, 100.0, 300.0, 997.0, 1500.0, 2000.0, 4000.0, 8000.0, 16000.0}) {
        CAPTURE(f);
        CHECK(std::fabs(svf_db(shelf, f, 48000.0) - biquad_db(b1, a1, f, 48000.0)) < 0.02);
        CHECK(std::fabs(svf_db(hp, f, 48000.0) - biquad_db(b2, a2, f, 48000.0)) < 0.05);
    }
}

TEST_CASE("leveler: holds a steady sine at the target, within its limits, at every rate") {
    // A 997 Hz sine at L dBFS in both channels is L LUFS (BS.1770's calibration: −3.01 LUFS for
    // 0 dBFS in one channel). Held at −20: −10 is cut by 10, −23 lifted by 3, −40 only by 6.
    for (double fs : {44100.0, 48000.0, 96000.0}) {
        for (double lvl : {-2.0, -10.0, -17.0, -23.0, -40.0}) {
            CAPTURE(fs);
            CAPTURE(lvl);
            Leveler lv;
            lv.set_sample_rate((float)fs);
            lv.set_on(true);
            uint64_t t = 0;
            const auto g = run(lv, lvl, 12.0, fs, t);
            double want = Leveler::LEV_TARGET_LUFS - lvl;
            want = std::min<double>(Leveler::LEV_BOOST_DB, std::max<double>(-Leveler::LEV_CUT_DB, want));
            CHECK(std::fabs(g.back() - want) < 0.15);
        }
    }
}

TEST_CASE("leveler: a loud break is turned down within a second, the film comes back after the hold") {
    const double fs = 48000.0;
    Leveler lv;
    lv.set_sample_rate((float)fs);
    lv.set_on(true);
    uint64_t t = 0;
    // The film: dialogue at −26 LUFS, lifted the full 6 dB.
    auto film = run(lv, -26.0, 10.0, fs, t);
    REQUIRE(film.back() == doctest::Approx(6.0).epsilon(0.01));
    // The break, at −10: 10 dB over the target, 16 over the film.
    auto ad = run(lv, -10.0, 8.0, fs, t);
    CHECK(ad[at(0.05, fs)] > 2.0);           // the first moments are heard as they come
    CHECK(ad[at(0.3, fs)] < -4.0);           // half of the way within 0.3 s
    CHECK(ad[at(1.0, fs)] < -9.0);           // all but a dB within a second
    CHECK(ad.back() == doctest::Approx(-10.0).epsilon(0.02));
    // Back to the film: nothing for the hold, then up at no more than the rise's pace.
    auto back = run(lv, -26.0, 12.0, fs, t);
    CHECK(back[at(Leveler::LEV_HOLD_S - 0.1, fs)] == doctest::Approx(ad.back()).epsilon(0.01));
    for (size_t i = 1; i < back.size(); i++)
        CHECK(back[i] - back[i - 1] <= Leveler::LEV_RISE_DB_S * BLK / fs + 1e-4);
    const double full = Leveler::LEV_HOLD_S + 16.0 / Leveler::LEV_RISE_DB_S;   // 6.8 s
    CHECK(back[at(full + 0.2, fs)] == doctest::Approx(6.0).epsilon(0.01));
}

TEST_CASE("leveler: pauses and silence leave the gain where it is") {
    const double fs = 48000.0;
    Leveler lv;
    lv.set_sample_rate((float)fs);
    lv.set_on(true);
    uint64_t t = 0;
    const float held = run(lv, -14.0, 8.0, fs, t).back();     // −6 dB
    const auto quiet = run(lv, -999.0, 10.0, fs, t);
    for (float g : quiet) CHECK(g == held);
    const auto under_gate = run(lv, -65.0, 5.0, fs, t);
    for (float g : under_gate) CHECK(g == held);
}

TEST_CASE("leveler: a bang after a quiet scene is caught by the fast detector") {
    const double fs = 48000.0;
    Leveler lv;
    lv.set_sample_rate((float)fs);
    lv.set_on(true);
    uint64_t t = 0;
    REQUIRE(run(lv, -30.0, 10.0, fs, t).back() == doctest::Approx(6.0).epsilon(0.01));
    // −8 dBFS: 22 dB over the scene. The slow level alone would leave it lifted for longer.
    const auto bang = run(lv, -8.0, 0.5, fs, t);
    CHECK(bang[at(0.2, fs)] < 0.0);
    CHECK(bang.back() < -6.0);
}

TEST_CASE("leveler: off is a wire, and switched off it glides back to 0 dB first") {
    const double fs = 48000.0;
    Leveler lv;
    lv.set_sample_rate((float)fs);
    float x[BLK * 2], y[BLK * 2];
    uint64_t t = 0;
    sine(x, BLK, -6.0, fs, t);
    std::memcpy(y, x, sizeof(x));
    lv.process(y, BLK);
    CHECK(std::memcmp(x, y, sizeof(x)) == 0);

    lv.set_on(true);
    REQUIRE(run(lv, -8.0, 8.0, fs, t).back() < -11.0);
    lv.set_on(false);
    const auto off = run(lv, -8.0, 6.0, fs, t);
    for (size_t i = 1; i < off.size(); i++)
        CHECK(off[i] - off[i - 1] <= Leveler::LEV_RISE_DB_S * BLK / fs + 1e-4);
    REQUIRE(off.back() == 0.0f);
    sine(x, BLK, -6.0, fs, t);
    std::memcpy(y, x, sizeof(x));
    lv.process(y, BLK);       // the last ramp's block
    sine(x, BLK, -6.0, fs, t);
    std::memcpy(y, x, sizeof(x));
    lv.process(y, BLK);
    CHECK(std::memcmp(x, y, sizeof(x)) == 0);
}

TEST_CASE("leveler: in the chain, ahead of the volume") {
    const double fs = 48000.0;
    auto peak_after = [&](bool on, int vol) {
        auto c = std::make_unique<DspChain>();
        c->set_sample_rate((float)fs);
        c->set_eq_enabled(false);
        c->set_subsonic(0.0f);
        c->set_loudness(0.0f);
        c->set_volume_percent(vol);
        c->set_leveler(on);
        c->reset();
        float x[BLK * 2];
        uint64_t t = 0;
        float pk = 0.0f;
        for (int b = 0; b < (int)(8.0 * fs / BLK); b++) {
            sine(x, BLK, -10.0, fs, t);
            c->process(x, BLK);
            pk = 0.0f;
            for (float v : x) pk = std::max(pk, std::fabs(v));
        }
        CHECK(c->faults() == 0);
        return std::pair<double, float>{db(pk), c->leveler_db()};
    };
    // −10 LUFS held at −20: 10 dB less at any volume; the gain does not depend on the volume.
    const auto off70 = peak_after(false, 70), on70 = peak_after(true, 70), on30 = peak_after(true, 30);
    CHECK(off70.first - on70.first == doctest::Approx(10.0).epsilon(0.02));
    CHECK(on70.second == doctest::Approx(on30.second).epsilon(0.001));
}

TEST_CASE("leveler: a NaN is thrown away with the block and the gain stays a number") {
    const double fs = 48000.0;
    DspChain c;
    c.set_sample_rate((float)fs);
    c.set_volume_percent(60);
    c.set_leveler(true);
    c.reset();
    float x[BLK * 2];
    uint64_t t = 0;
    for (int b = 0; b < 200; b++) { sine(x, BLK, -10.0, fs, t); c.process(x, BLK); }
    sine(x, BLK, -10.0, fs, t);
    x[10] = NAN;
    c.process(x, BLK);
    CHECK(c.faults() == 1);
    for (int b = 0; b < 200; b++) { sine(x, BLK, -10.0, fs, t); c.process(x, BLK); }
    CHECK(c.faults() == 1);
    CHECK(std::isfinite(c.leveler_db()));
    CHECK(c.leveler_db() < -5.0f);
}

TEST_CASE("leveler schedule: the window, across midnight, empty when its ends meet") {
    CHECK(LevelerSchedule::inside(22 * 60, 22 * 60, 7 * 60));
    CHECK(LevelerSchedule::inside(0, 22 * 60, 7 * 60));
    CHECK(LevelerSchedule::inside(7 * 60 - 1, 22 * 60, 7 * 60));
    CHECK_FALSE(LevelerSchedule::inside(7 * 60, 22 * 60, 7 * 60));
    CHECK_FALSE(LevelerSchedule::inside(12 * 60, 22 * 60, 7 * 60));
    CHECK(LevelerSchedule::inside(13 * 60, 13 * 60, 14 * 60));
    CHECK_FALSE(LevelerSchedule::inside(14 * 60, 13 * 60, 14 * 60));
    for (int m : {0, 600, 1439}) CHECK_FALSE(LevelerSchedule::inside(m, 600, 600));
}

TEST_CASE("leveler schedule: switches when it starts to apply and at its edges, not in between") {
    LevelerSchedule s;
    const int F = 22 * 60, T = 7 * 60;
    CHECK(s.tick(-1, true, F, T) == -1);          // no clock yet
    CHECK(s.tick(23 * 60, false, F, T) == -1);    // off
    CHECK(s.tick(23 * 60, true, F, T) == 1);      // applies: inside → on
    CHECK(s.tick(23 * 60 + 1, true, F, T) == -1); // a hand on the switch now is left alone
    CHECK(s.tick(6 * 60 + 59, true, F, T) == -1);
    CHECK(s.tick(7 * 60, true, F, T) == 0);       // the end: off
    CHECK(s.tick(12 * 60, true, F, T) == -1);
    CHECK(s.tick(22 * 60, true, F, T) == 1);      // the start: on
    CHECK(s.tick(22 * 60 + 5, true, F, 23 * 60) == 1);   // moved: says what it says now
    CHECK(s.tick(22 * 60 + 6, false, F, 23 * 60) == -1);
    CHECK(s.tick(12 * 60, true, F, 23 * 60) == 0);       // on again, outside: off
    CHECK(s.tick(12 * 60 + 1, true, F, 23 * 60) == -1);
}
