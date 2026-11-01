/**
 * @file test_dsp_design.cpp
 * @brief HOST tests for the matched band design (components/audio_dsp, Design::MATCHED):
 *        |H| equals the analogue prototype at DC, Nyquist and f0, the customer's EQ is the
 *        same curve at every rate, the presets stay close to their analogue targets, and a
 *        shelf's Q is capped where it would start to overshoot.
 */

#include <algorithm>
#include <cmath>
#include <vector>
#include "doctest.h"
#include "dsp_chain.h"

using namespace audio_core;

namespace {

uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

EqBand band(BandType t, float f, float g, float q) {
    EqBand b; b.type = t; b.freq_hz = f; b.gain_db = g; b.q = q; b.enabled = true;
    return b;
}

std::vector<EqBand> preset(const float g[10]) {
    static const BandType T[10] = {BandType::LOW_SHELF, BandType::PEAK, BandType::PEAK, BandType::PEAK,
                                   BandType::PEAK, BandType::PEAK, BandType::PEAK, BandType::PEAK,
                                   BandType::PEAK, BandType::HIGH_SHELF};
    static const float F[10] = {40, 62.5f, 125, 250, 500, 1000, 2000, 4000, 8000, 12000};
    static const float Q[10] = {0.707f, 1, 1, 1, 1, 1, 1, 1, 1, 0.707f};
    std::vector<EqBand> v(10);
    for (int i = 0; i < 10; i++) v[i] = band(T[i], F[i], g[i], Q[i]);
    return v;
}

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

/// The EQ on the customer's panel (from a backup): "Софт-рок" as the earlier fit made it.
std::vector<EqBand> customer_eq() {
    const float g[10] = {2.6f, 1.8f, 1.7f, 1.5f, 0.2f, -1.7f, -2.1f, -2.1f, -1.4f, 4.2f};
    return preset(g);
}

/// The summed curve, digital and analogue, worst difference over 20 Hz…20 kHz (and Nyquist).
double worst_vs_analog_db(const std::vector<EqBand>& eq, double fs, Design how) {
    std::vector<SvfDesign> d;
    for (const auto& b : eq) { SvfDesign x; REQUIRE(design_svf_params(b, fs, x, how)); d.push_back(x); }
    double worst = 0.0;
    const double hi = std::min(20000.0, 0.49 * fs);
    for (double f = 20.0; f <= hi; f *= 1.01) {
        double dig = 0.0, ana = 0.0;
        for (size_t i = 0; i < eq.size(); i++) {
            dig += svf_magnitude_db(d[i], fs, f);
            ana += 10.0 * std::log10(analog_mag2(eq[i], f));
        }
        worst = std::max(worst, std::fabs(dig - ana));
    }
    return worst;
}

/// The digital curve itself at one frequency.
double curve_db(const std::vector<EqBand>& eq, double fs, double f, Design how) {
    double s = 0.0;
    for (const auto& b : eq) { SvfDesign x; design_svf_params(b, fs, x, how); s += svf_magnitude_db(x, fs, f); }
    return s;
}

}  // namespace

TEST_CASE("matched design: |H| is the analogue prototype's at DC, at Nyquist and at f0") {
    uint32_t seed = 5;
    double worst = 0.0;
    int fallbacks = 0;
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0})
        for (int k = 0; k < 400; k++) {
            const BandType t[] = {BandType::PEAK, BandType::LOW_SHELF, BandType::HIGH_SHELF};
            EqBand b = band(t[lcg(seed) % 3], 20.0f * std::pow(1000.0f, (float)(lcg(seed) % 1000) / 1000.0f),
                            -12.0f + 24.0f * (float)(lcg(seed) % 1000) / 1000.0f,
                            0.3f + 7.7f * (float)(lcg(seed) % 1000) / 1000.0f);
            if (b.freq_hz > 0.49f * (float)fs) continue;
            SvfDesign m, bl;
            REQUIRE(design_svf_params(b, fs, m, Design::MATCHED));
            REQUIRE(design_svf_params(b, fs, bl, Design::BILINEAR));
            if (m.g == bl.g && m.k == bl.k && m.m1 == bl.m1) { fallbacks++; continue; }
            REQUIRE(std::isfinite(m.g)); REQUIRE(std::isfinite(m.k));
            REQUIRE(std::isfinite(m.m0)); REQUIRE(std::isfinite(m.m1)); REQUIRE(std::isfinite(m.m2));
            const double wm = std::min(2 * M_PI * b.freq_hz / fs, 0.9 * M_PI) * fs / (2 * M_PI);
            for (double f : {1e-3, fs / 2.0, wm}) {
                const double want = 10.0 * std::log10(analog_mag2(b, f));
                worst = std::max(worst, std::fabs(svf_magnitude_db(m, fs, f) - want));
            }
        }
    MESSAGE("matched against the analogue prototype at the three points: worst " << worst
            << " dB; bilinear fallbacks: " << fallbacks);
    CHECK(worst < 1e-6);
}

TEST_CASE("matched design: the customer's EQ is the same curve at every rate") {
    const auto eq = customer_eq();
    double worst = 0.0, worst_bilinear = 0.0;
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        worst = std::max(worst, worst_vs_analog_db(eq, fs, Design::MATCHED));
        worst_bilinear = std::max(worst_bilinear, worst_vs_analog_db(eq, fs, Design::BILINEAR));
    }
    // The spread across rates, frequency by frequency.
    double spread = 0.0;
    for (double f = 20.0; f <= 20000.0; f *= 1.02) {
        double lo = 1e9, hi = -1e9;
        for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
            const double v = curve_db(eq, fs, f, Design::MATCHED);
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
        spread = std::max(spread, hi - lo);
    }
    MESSAGE("the customer's EQ against its analogue curve: matched " << worst << " dB, bilinear " << worst_bilinear
            << " dB; spread over 44.1…192 kHz " << spread << " dB");
    CHECK(worst <= 0.26);
    CHECK(spread <= 0.25);
}

TEST_CASE("matched design: every preset stays close to its analogue target at 44.1 and 48 kHz") {
    double w44 = 0.0, w48 = 0.0;
    for (const auto& g : PRESETS) {
        const auto eq = preset(g);
        w44 = std::max(w44, worst_vs_analog_db(eq, 44100.0, Design::MATCHED));
        w48 = std::max(w48, worst_vs_analog_db(eq, 48000.0, Design::MATCHED));
    }
    MESSAGE("presets against their analogue curves: " << w44 << " dB at 44.1 kHz, " << w48 << " dB at 48 kHz");
    CHECK(w44 <= 0.95);
    CHECK(w48 <= 0.65);
}

TEST_CASE("matched design: a notch keeps its zero exactly at f0") {
    for (double fs : {44100.0, 192000.0})
        for (float f : {50.0f, 1000.0f, 15000.0f}) {
            SvfDesign d;
            REQUIRE(design_svf_params(band(BandType::NOTCH, f, 0, 4), fs, d, Design::MATCHED));
            CAPTURE(fs); CAPTURE(f);
            CHECK(svf_magnitude_db(d, fs, f) < -100.0);
            CHECK(svf_magnitude_db(d, fs, 1e-3) == doctest::Approx(0.0).scale(1.0).epsilon(1e-6));
        }
}

TEST_CASE("shelves: Q is capped at SHELF_Q_MAX, where a shelf stops overshooting its gain") {
    for (BandType t : {BandType::LOW_SHELF, BandType::HIGH_SHELF}) {
        SvfDesign a, b;
        REQUIRE(design_svf_params(band(t, 1000, 12, 8.0f), 48000.0, a));
        REQUIRE(design_svf_params(band(t, 1000, 12, SHELF_Q_MAX), 48000.0, b));
        CHECK(a.g == b.g); CHECK(a.k == b.k); CHECK(a.m0 == b.m0); CHECK(a.m1 == b.m1); CHECK(a.m2 == b.m2);
        // At the cap the overshoot over +12 dB is under a dB.
        double peak = -1e9;
        for (double f = 20; f < 20000; f *= 1.01) peak = std::max(peak, svf_magnitude_db(b, 48000.0, f));
        CHECK(peak < 13.0);
    }
    // A peak's Q is its own business.
    SvfDesign p8, p1;
    design_svf_params(band(BandType::PEAK, 1000, 6, 8.0f), 48000.0, p8);
    design_svf_params(band(BandType::PEAK, 1000, 6, 1.0f), 48000.0, p1);
    CHECK(p8.k != p1.k);
}
