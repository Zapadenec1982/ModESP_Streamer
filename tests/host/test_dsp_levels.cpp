/**
 * @file test_dsp_levels.cpp
 * @brief HOST tests for the chain's gain structure (components/audio_dsp): 1.0 inside the chain
 *        is the amplifier's ceiling, the volume tops out at drive − B where B is the EQ's
 *        loudest point, and below that the EQ switch changes the bands and nothing else.
 */

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include "doctest.h"
#include "dsp_chain.h"

using namespace audio_core;

namespace {

uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

/// The ten preset bands (components/audio_dsp/src/eq_presets.cpp) with a preset's gains.
std::vector<EqBand> preset(const float g[10]) {
    static const BandType T[10] = {BandType::LOW_SHELF, BandType::PEAK, BandType::PEAK, BandType::PEAK,
                                   BandType::PEAK, BandType::PEAK, BandType::PEAK, BandType::PEAK,
                                   BandType::PEAK, BandType::HIGH_SHELF};
    static const float F[10] = {40, 62.5f, 125, 250, 500, 1000, 2000, 4000, 8000, 12000};
    static const float Q[10] = {0.707f, 1, 1, 1, 1, 1, 1, 1, 1, 0.707f};
    std::vector<EqBand> v(10);
    for (int i = 0; i < 10; i++) { v[i].type = T[i]; v[i].freq_hz = F[i]; v[i].q = Q[i]; v[i].gain_db = g[i]; v[i].enabled = true; }
    return v;
}

// The presets, as tools/eq/fit_presets.py generated them (eq_presets.cpp).
const float PRESETS[][10] = {
    {3.9f, 3.1f, 2.6f, -0.3f, -0.3f, 0.1f, 0.0f, -0.1f, 0.9f, 2.4f},
    {-5.8f, -4.6f, -3.8f, 0.4f, 0.5f, -0.2f, 0.2f, 0.2f, -2.2f, -3.4f},
    {6.1f, 4.0f, 3.5f, 3.9f, 2.9f, 0.6f, -0.8f, -2.3f, -4.1f, -7.0f},
    {-6.1f, -4.0f, -3.5f, -4.2f, -2.7f, 1.2f, 3.1f, 5.0f, 6.7f, 10.1f},
    {4.5f, 3.1f, 3.0f, 1.3f, -3.0f, -2.6f, -0.4f, 1.8f, 4.4f, 8.0f},
    {2.6f, 1.7f, 4.3f, 3.8f, -1.4f, -1.8f, 0.2f, 0.9f, 2.6f, 9.6f},
    {6.4f, 4.4f, 4.0f, 2.8f, 2.4f, 0.2f, -1.5f, -2.1f, -2.3f, 0.2f},
    {-3.2f, -2.3f, -0.8f, 1.2f, 2.2f, 2.2f, 2.3f, 2.0f, 1.4f, 1.3f},
    {4.4f, 3.1f, 3.3f, 0.8f, -0.7f, -0.5f, 0.1f, -0.1f, -0.6f, 4.6f},
    {0.0f, 0.0f, 0.0f, -0.1f, -0.2f, -0.0f, 0.7f, -1.0f, -3.5f, -5.5f},
    {0.0f, -0.5f, -0.5f, 0.7f, 2.3f, 2.3f, 2.3f, 2.1f, 0.6f, -0.8f},
    {-1.4f, -1.1f, 1.0f, 2.7f, 3.4f, 2.3f, 0.2f, -1.1f, -1.2f, -1.0f},
    {0.0f, -0.1f, 0.3f, 0.2f, -2.6f, -0.8f, 2.1f, 3.4f, 1.6f, -1.5f},
    {4.9f, 3.6f, 3.1f, -0.8f, -4.2f, -1.8f, 0.7f, 2.5f, 4.5f, 7.0f},
    {-1.4f, -0.9f, -1.7f, -2.2f, -0.9f, 1.8f, 2.0f, 2.6f, 4.1f, 6.3f},
    {3.1f, 2.2f, 1.2f, -0.2f, -1.3f, -0.8f, 0.7f, 2.3f, 3.9f, 7.4f},
    {2.6f, 1.8f, 1.7f, 1.5f, 0.2f, -1.7f, -2.1f, -2.1f, -1.5f, 5.0f},   // "Софт-рок"
    {6.1f, 4.2f, 3.3f, 1.4f, -0.1f, 0.1f, -1.3f, -2.9f, -3.7f, 0.3f},
    {4.9f, 3.4f, 3.0f, 1.1f, -2.3f, -2.4f, -1.1f, 1.3f, 4.3f, 6.1f},
    {3.3f, 1.9f, 0.5f, -0.2f, -0.2f, 0.7f, 1.0f, 0.4f, 1.3f, 3.3f},       // "Джаз"
    {5.3f, 3.3f, 1.8f, 0.4f, -0.6f, -0.1f, 0.3f, 0.6f, 1.0f, 2.1f},       // "Хіп-хоп"
};
constexpr int SOFT_ROCK = 16;

/// The true maximum of the summed curve on [20 Hz, min(20 kHz, 0.49 fs)], by brute force.
double dense_max_db(const std::vector<EqBand>& eq, double fs) {
    std::vector<SvfDesign> d;
    for (const auto& b : eq) { SvfDesign x; if (b.enabled && design_svf_params(b, fs, x)) d.push_back(x); }
    const double lo = 20.0, hi = std::min(20000.0, 0.49 * fs);
    double best = -1e9;
    for (int j = 0; j <= 20000; j++) {
        const double f = lo * std::pow(hi / lo, j / 20000.0);
        double s = 0.0;
        for (const auto& x : d) s += svf_magnitude_db(x, fs, f);
        best = std::max(best, s);
    }
    return best;
}

double headroom(const std::vector<EqBand>& eq, double fs) {
    DspChain c;
    c.set_sample_rate((float)fs);
    c.set_bands(eq.data(), (int)eq.size());
    return c.headroom_db();
}

/// Peak of the settled output for a sine at `hz`, amplitude `amp` (both channels).
float settled_peak(DspChain& c, double fs, double hz, float amp, double seconds = 0.6) {
    float blk[DspChain::BLOCK * 2], pk = 0.0f;
    double ph = 0.0;
    const size_t total = (size_t)(seconds * fs), from = total / 2;
    for (size_t done = 0; done < total; done += DspChain::BLOCK) {
        for (size_t i = 0; i < DspChain::BLOCK; i++) { blk[2 * i] = blk[2 * i + 1] = amp * (float)std::sin(ph); ph += 2 * M_PI * hz / fs; }
        c.process(blk, DspChain::BLOCK);
        if (done >= from) for (float v : blk) pk = std::max(pk, std::fabs(v));
    }
    return pk;
}

/// RMS of the settled output for a sine at `hz` — a peak of samples is up to 0.3 dB short at
/// a few kHz, and by a different amount after a filter has moved the phase.
double settled_rms(DspChain& c, double fs, double hz, float amp) {
    float blk[DspChain::BLOCK * 2];
    double ph = 0.0, acc = 0.0;
    size_t cnt = 0;
    const size_t total = (size_t)(1.0 * fs), from = total / 2;
    for (size_t done = 0; done < total; done += DspChain::BLOCK) {
        for (size_t i = 0; i < DspChain::BLOCK; i++) { blk[2 * i] = blk[2 * i + 1] = amp * (float)std::sin(ph); ph += 2 * M_PI * hz / fs; }
        c.process(blk, DspChain::BLOCK);
        if (done >= from) for (size_t i = 0; i < DspChain::BLOCK; i++) { acc += (double)blk[2 * i] * blk[2 * i]; cnt++; }
    }
    return std::sqrt(acc / (double)cnt);
}

double db(double x) { return 20.0 * std::log10(x); }

}  // namespace

TEST_CASE("headroom: B is the EQ's loudest point, found to within 0.05 dB") {
    double worst_under = 0.0, worst_over = 0.0;
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        for (const auto& g : PRESETS) {
            const auto eq = preset(g);
            const double want = std::max(0.0, dense_max_db(eq, fs)), got = headroom(eq, fs);
            worst_under = std::max(worst_under, want - got);
            worst_over = std::max(worst_over, got - want);
        }
        uint32_t seed = (uint32_t)fs;
        for (int k = 0; k < 50; k++) {                          // 200 random EQs over the four rates
            std::vector<EqBand> eq(1 + lcg(seed) % 10);
            for (auto& b : eq) {
                const BandType t[] = {BandType::PEAK, BandType::LOW_SHELF, BandType::HIGH_SHELF, BandType::NOTCH, BandType::LOW_PASS, BandType::HIGH_PASS};
                b.type = t[lcg(seed) % 6];
                b.freq_hz = 20.0f * std::pow(1000.0f, (float)(lcg(seed) % 1000) / 1000.0f);
                b.gain_db = -12.0f + 24.0f * (float)(lcg(seed) % 1000) / 1000.0f;
                b.q = 0.3f + 7.7f * (float)(lcg(seed) % 1000) / 1000.0f;
                b.enabled = (lcg(seed) % 8) != 0;
            }
            const double want = std::max(0.0, dense_max_db(eq, fs)), got = headroom(eq, fs);
            worst_under = std::max(worst_under, want - got);
            worst_over = std::max(worst_over, got - want);
        }
    }
    MESSAGE("headroom against a 20 001-point search: at worst " << worst_under << " dB short, " << worst_over << " dB over");
    CHECK(worst_under <= 0.05);
    CHECK(worst_over <= 0.01);
    // The customer's EQ: its 12 kHz shelf is +4.2 dB at Nyquist; how much of that has arrived by
    // 20 kHz depends on how close 20 kHz is to Nyquist.
    for (double fs : {48000.0, 96000.0}) {
        const double b = headroom(preset(PRESETS[SOFT_ROCK]), fs);
        MESSAGE("Софт-рок at " << fs << " Hz: B = " << b << " dB");
    }
}

TEST_CASE("headroom: a flat or cutting EQ costs nothing, and 100 % is then a wire") {
    std::vector<EqBand> eq = preset(PRESETS[SOFT_ROCK]);
    for (auto& b : eq) b.gain_db = -std::fabs(b.gain_db);
    CHECK(headroom(eq, 48000.0) == 0.0);
    DspChain c;
    c.set_eq_enabled(false);
    c.set_volume_percent(100);
    CHECK(c.headroom_db() == 0.0f);
    CHECK(settled_peak(c, 48000.0, 1000.0, 0.5f) == 0.5f);     // bit-exact level
}

TEST_CASE("volume ceiling: at 100 % the EQ's loudest band arrives exactly at the ceiling") {
    const auto eq = preset(PRESETS[3]);   // "Більше верхів": +10.6 dB shelf, the steepest boost
    for (double fs : {48000.0, 96000.0}) {
        DspChain c;
        c.set_sample_rate((float)fs);
        c.reset();
        c.set_bands(eq.data(), (int)eq.size());
        c.set_volume_percent(100);
        // The loudest frequency, found the slow way.
        std::vector<SvfDesign> d;
        for (const auto& b : eq) { SvfDesign x; design_svf_params(b, fs, x); d.push_back(x); }
        double best = -1e9, best_f = 0;
        for (double f = 20; f < std::min(20000.0, 0.49 * fs); f *= 1.001) {
            double s = 0; for (const auto& x : d) s += svf_magnitude_db(x, fs, f);
            if (s > best) { best = s; best_f = f; }
        }
        const float pk = settled_peak(c, fs, best_f, 1.0f);      // a full-scale source
        CAPTURE(fs); CAPTURE(best_f);
        CHECK(db(pk) == doctest::Approx(0.0).epsilon(0.0).scale(1.0).epsilon(0.05));
        CHECK(pk <= CEILING * (1.0f + 1e-6f));
        CHECK(c.take_limiter_min() > 0.995f);                    // the limiter had nothing to do
    }
}

TEST_CASE("volume ceiling: below it, switching the EQ off changes the bands and nothing else") {
    const auto eq = preset(PRESETS[SOFT_ROCK]);
    for (double hz : {62.5, 250.0, 1000.0, 4000.0}) {
        DspChain on, off;
        on.set_bands(eq.data(), (int)eq.size());
        off.set_bands(eq.data(), (int)eq.size());
        off.set_eq_enabled(false);
        on.set_volume_percent(70);                                 // −18 dB: under the 3.5 dB cap
        off.set_volume_percent(70);
        const double diff = db(settled_rms(on, 48000.0, hz, 0.5f) / settled_rms(off, 48000.0, hz, 0.5f));
        CAPTURE(hz);
        CHECK(diff == doctest::Approx(eq_magnitude_db(eq.data(), (int)eq.size(), (float)hz)).epsilon(0.0).scale(1.0).epsilon(0.02));
    }
}

TEST_CASE("volume: 1 % to 100 % is −80 dB to drive, and the cap takes the top of the scale") {
    DspChain c;
    c.set_eq_enabled(false);
    c.set_volume_percent(100);
    CHECK(db(settled_peak(c, 48000.0, 1000.0, 0.25f) / 0.25) == doctest::Approx(0.0).scale(1.0).epsilon(0.01));
    c.set_volume_percent(50);
    CHECK(db(settled_peak(c, 48000.0, 1000.0, 0.25f) / 0.25) == doctest::Approx(-30.0).scale(1.0).epsilon(0.01));
    c.set_drive_db(3.0f);
    c.set_volume_percent(100);
    CHECK(db(settled_peak(c, 48000.0, 1000.0, 0.25f) / 0.25) == doctest::Approx(3.0).scale(1.0).epsilon(0.01));
    c.set_drive_db(9.0f);                                          // clamped to 6
    CHECK(db(settled_peak(c, 48000.0, 1000.0, 0.25f) / 0.25) == doctest::Approx(6.0).scale(1.0).epsilon(0.01));
    // A +6 dB band: the top 6 dB of the scale (10 %) is flat.
    c.set_drive_db(0.0f);
    EqBand b; b.type = BandType::PEAK; b.freq_hz = 100; b.gain_db = 6; b.q = 1; b.enabled = true;
    c.set_bands(&b, 1);
    c.set_eq_enabled(true);
    CHECK(c.headroom_db() == doctest::Approx(6.0f).epsilon(0.01));
    c.set_volume_percent(100);
    const float at100 = settled_peak(c, 48000.0, 1000.0, 0.25f);
    c.set_volume_percent(90);
    const float at90 = settled_peak(c, 48000.0, 1000.0, 0.25f);
    c.set_volume_percent(85);
    const float at85 = settled_peak(c, 48000.0, 1000.0, 0.25f);
    CHECK(at100 == doctest::Approx(at90).epsilon(1e-4));
    CHECK(db(at85 / at90) == doctest::Approx(-3.0).scale(1.0).epsilon(0.02));
}

TEST_CASE("volume: 0.6 dB a step down to 50 %, steps that grow under it, 1 % at −80 dB") {
    // The top half is the law it always was; the bottom reaches 20.6 dB further than −59.4.
    CHECK(DspChain::volume_db(100.0f) == 0.0f);
    CHECK(DspChain::volume_db(50.0f) == doctest::Approx(-30.0f));
    CHECK(DspChain::volume_db(70.0f) == doctest::Approx(-18.0f));
    CHECK(DspChain::volume_db(1.0f) == doctest::Approx(-80.0f));
    CHECK(DspChain::volume_db(17.0f) == doctest::Approx(-59.14f).epsilon(1e-3));   // the old 1 %
    // Monotonic, no step under 0.6 dB or over 1.5 dB, and nothing jumps at the knee.
    for (int p = 2; p <= 100; p++) {
        const float step = DspChain::volume_db((float)p) - DspChain::volume_db((float)(p - 1));
        CAPTURE(p);
        CHECK(step >= 0.599f);
        CHECK(step <= 1.5f);
    }
    for (float p : {49.9f, 49.99f})
        CHECK(DspChain::volume_db(p) == doctest::Approx(-60.0f * (1.0f - p / 100.0f)).epsilon(1e-4));
    // A fractional level under «Найгучніше» goes a little further down, still in order.
    CHECK(DspChain::volume_db(0.5f) < DspChain::volume_db(1.0f));

    // And it is what the chain plays: 1 % and 25 % on a 1 kHz sine.
    DspChain c;
    c.set_eq_enabled(false);
    c.set_volume_percent(1);
    CHECK(db(settled_peak(c, 48000.0, 1000.0, 0.25f) / 0.25) == doctest::Approx(-80.0).scale(1.0).epsilon(0.01));
    c.set_volume_percent(25);
    CHECK(db(settled_peak(c, 48000.0, 1000.0, 0.25f) / 0.25) ==
          doctest::Approx(DspChain::volume_db(25.0f)).scale(1.0).epsilon(0.01));
}

TEST_CASE("trim and width count in the headroom; trim is heard ahead of the EQ") {
    DspChain c;
    c.set_eq_enabled(false);
    c.set_pre_gain_db(3.0f);
    CHECK(c.headroom_db() == doctest::Approx(3.0f));
    c.set_volume_percent(100);                                     // trim +3, capped −3: a wire
    CHECK(settled_peak(c, 48000.0, 1000.0, 0.25f) == 0.25f);
    c.set_volume_percent(50);
    CHECK(db(settled_peak(c, 48000.0, 1000.0, 0.25f) / 0.25) == doctest::Approx(-27.0).scale(1.0).epsilon(0.01));
    c.set_pre_gain_db(0.0f);
    c.set_width(1.5f);
    CHECK(c.headroom_db() == doctest::Approx(20.0f * std::log10(1.5f)).epsilon(1e-4));
}

TEST_CASE("subsonic: a 4th-order Butterworth at its corner, ahead of the EQ, switched without a click") {
    // |H|² = 1/(1 + (fc/f)^8): at 40 Hz that is −24.1 at 20, −10.4 at 30, −3.0 at 40, −0.67 at 50,
    // −0.06 at 68 (the speaker's port, reportedly) — the bilinear warp is negligible down here.
    for (double fs : {48000.0, 192000.0}) {
        DspChain c;
        c.set_sample_rate((float)fs);
        c.reset();
        c.set_eq_enabled(false);
        c.set_volume_percent(100);
        c.set_subsonic(40.0f);
        const float ref = (float)settled_rms(c, fs, 1000.0, 0.5f);
        for (double f : {20.0, 30.0, 40.0, 50.0, 68.0}) {
            const double want = -10.0 * std::log10(1.0 + std::pow(40.0 / f, 8.0));
            const double got = db(settled_rms(c, fs, f, 0.5f) / ref);
            CAPTURE(fs); CAPTURE(f);
            CHECK(got == doctest::Approx(want).scale(1.0).epsilon(0.05));
        }
    }
    // Out of range is off, and off is a wire.
    DspChain off;
    off.set_eq_enabled(false);
    off.set_volume_percent(100);
    off.set_subsonic(10.0f);
    CHECK(settled_peak(off, 48000.0, 20.0, 0.5f, 1.0) == 0.5f);
}

TEST_CASE("subsonic: it never adds headroom, and a bass boost under it asks for less") {
    EqBand b; b.type = BandType::LOW_SHELF; b.freq_hz = 40; b.gain_db = 6; b.q = 0.707f; b.enabled = true;
    DspChain c;
    c.set_bands(&b, 1);
    const float without = c.headroom_db();
    c.set_subsonic(40.0f);
    const float with = c.headroom_db();
    // The shelf reaches its +6 dB only at DC; the search starts at 20 Hz, where it is +5.6.
    CHECK(without == doctest::Approx(5.62f).epsilon(0.01));
    CHECK(with < without - 1.0f);                             // the plateau is under the corner
    DspChain flat;
    flat.set_eq_enabled(false);
    flat.set_subsonic(40.0f);
    CHECK(flat.headroom_db() == 0.0f);
}

TEST_CASE("headroom with the bass limiter: what it can absorb is free, what it cannot still counts") {
    std::vector<float> line(DspChain::BASS_LINE_FRAMES * 2);
    auto chain = [&](const EqBand& b) {
        auto c = std::make_unique<DspChain>();
        c->set_bass_line(line.data(), DspChain::BASS_LINE_FRAMES);
        c->reset();
        c->set_bass_xo(120.0f);
        c->set_bands(&b, 1);
        return c;
    };
    // A +9 dB peak at 150 Hz, Q 2: above the crossover, mostly out of the bass stage's reach.
    EqBand p150; p150.type = BandType::PEAK; p150.freq_hz = 150; p150.gain_db = 9; p150.q = 2; p150.enabled = true;
    auto c = chain(p150);
    // 71 % of a 150 Hz sine is LR4's high band: past the stage's reach, the boost counts in full.
    MESSAGE("+9 dB at 150 Hz: B = " << c->headroom_db() << " dB");
    CHECK(c->headroom_db() == doctest::Approx(9.0).epsilon(0.01));
    // A full-scale 150 Hz sine at 100 %: the bass stage takes its share, the brickwall nothing.
    c->set_eq_enabled(true);
    c->set_volume_percent(100);
    settled_peak(*c, 48000.0, 150.0, 1.0f, 1.0);
    CHECK(c->take_limiter_min() > 0.98f);
    // A +6 dB shelf at 40 Hz is the bass stage's entirely.
    EqBand ls; ls.type = BandType::LOW_SHELF; ls.freq_hz = 40; ls.gain_db = 6; ls.q = 0.707f; ls.enabled = true;
    CHECK(chain(ls)->headroom_db() <= 0.1f);             // all but the plateau's tail near xo
    // And a boost right at the crossover is shared: B between the two.
    EqBand p100; p100.type = BandType::PEAK; p100.freq_hz = 100; p100.gain_db = 6; p100.q = 2; p100.enabled = true;
    const float b100 = chain(p100)->headroom_db();
    MESSAGE("+6 dB at 100 Hz, Q 2: B = " << b100 << " dB");
    CHECK(b100 > 0.5f);
    CHECK(b100 < 6.0f);
}
