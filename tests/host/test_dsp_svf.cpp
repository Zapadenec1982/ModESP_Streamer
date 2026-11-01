/**
 * @file test_dsp_svf.cpp
 * @brief HOST tests for the EQ's SVF kernel and its design (components/audio_dsp):
 *        same response as the RBJ cookbook, the stereo loops equal to the one-sample
 *        definition bit for bit, and the noise floor at a low corner and a high rate —
 *        the case the Direct Form II kernel failed.
 */

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <vector>
#include "doctest.h"
#include "dsp_chain.h"
#include "dsp_kernels.h"

using namespace audio_core;

namespace {

// ── RBJ Audio-EQ-Cookbook in double: the reference the SVF design must reproduce ──
struct Bq { double b0, b1, b2, a1, a2; };

bool rbj(const EqBand& b, double fs, Bq& o) {
    const double A = std::pow(10.0, b.gain_db / 40.0);
    const double w0 = 2.0 * M_PI * b.freq_hz / fs;
    const double cw = std::cos(w0), sw = std::sin(w0), alpha = sw / (2.0 * b.q);
    double b0, b1, b2, a0, a1, a2;
    switch (b.type) {
    case BandType::PEAK:
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
        break;
    case BandType::LOW_SHELF: {
        const double s = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + s);  b1 = 2 * A * ((A - 1) - (A + 1) * cw);
        b2 = A * ((A + 1) - (A - 1) * cw - s);  a0 = (A + 1) + (A - 1) * cw + s;
        a1 = -2 * ((A - 1) + (A + 1) * cw);     a2 = (A + 1) + (A - 1) * cw - s;
        break;
    }
    case BandType::HIGH_SHELF: {
        const double s = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + s);  b1 = -2 * A * ((A - 1) + (A + 1) * cw);
        b2 = A * ((A + 1) + (A - 1) * cw - s);  a0 = (A + 1) - (A - 1) * cw + s;
        a1 = 2 * ((A - 1) - (A + 1) * cw);      a2 = (A + 1) - (A - 1) * cw - s;
        break;
    }
    case BandType::LOW_PASS:
        b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = (1 - cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case BandType::HIGH_PASS:
        b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = (1 + cw) / 2; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    case BandType::NOTCH:
        b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + alpha; a1 = -2 * cw; a2 = 1 - alpha;
        break;
    default:
        return false;
    }
    o = {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    return true;
}

double bq_mag_db(const Bq& c, double fs, double f) {
    const std::complex<double> z1 = std::polar(1.0, -2.0 * M_PI * f / fs), z2 = z1 * z1;
    return 20.0 * std::log10(std::abs((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2)));
}

/// The SVF section in double, straight from the design parameters.
struct SvfD {
    double c1, c2, c3, m0, m1, m2, s1 = 0, s2 = 0;
    explicit SvfD(const SvfDesign& d) {
        c1 = 1.0 / (1.0 + d.g * (d.g + d.k)); c2 = d.g * c1; c3 = d.g * c2;
        m0 = d.m0; m1 = d.m1; m2 = d.m2;
    }
    /// The float coefficients a chain runs, held at double precision: the reference for
    /// measuring the rounding of the ARITHMETIC alone.
    explicit SvfD(const SvfCoef& c) : c1(c.c1), c2(c.c2), c3(c.c3), m0(c.m0), m1(c.m1), m2(c.m2) {}
    double tick(double x) {
        const double v3 = x - s2;
        const double v1 = c1 * s1 + c2 * v3;
        const double v2 = s2 + c2 * s1 + c3 * v3;
        s1 = 2 * v1 - s1;
        s2 = 2 * v2 - s2;
        return m0 * x + m1 * v1 + m2 * v2;
    }
};

EqBand band(BandType t, float f, float g, float q) {
    EqBand b; b.type = t; b.freq_hz = f; b.gain_db = g; b.q = q; b.enabled = true;
    return b;
}

/// The customer's EQ as it was on the panel (backup of 26.09.2026).
std::vector<EqBand> customer_eq() {
    return {
        band(BandType::LOW_SHELF, 40, 2.6f, 0.71f),  band(BandType::PEAK, 62, 1.8f, 1.0f),
        band(BandType::PEAK, 125, 1.7f, 1.0f),       band(BandType::PEAK, 250, 1.5f, 1.0f),
        band(BandType::PEAK, 500, 0.2f, 1.0f),       band(BandType::PEAK, 1000, -1.7f, 1.0f),
        band(BandType::PEAK, 2000, -2.1f, 1.0f),     band(BandType::PEAK, 4000, -2.1f, 1.0f),
        band(BandType::PEAK, 8000, -1.4f, 1.0f),     band(BandType::HIGH_SHELF, 12000, 4.2f, 0.71f),
    };
}

uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

/// Residual of the float chain against the same float coefficients run in double, in dB
/// relative to the output: what the kernel's own rounding adds, and nothing else.
double kernel_noise_db(const std::vector<EqBand>& eq, double fs, float bass_amp) {
    std::vector<SvfCoef> c;
    for (const auto& b : eq) { SvfCoef k; REQUIRE(design_svf(b, fs, k)); c.push_back(k); }
    std::vector<SvfState> st(c.size());
    std::vector<SvfD> ref;
    std::vector<SvfD> refr;
    for (const auto& k : c) { ref.emplace_back(k); refr.emplace_back(k); }

    const size_t N = DspChain::BLOCK;
    float blk[N * 2];
    const size_t settle = (size_t)(0.5 * fs), total = settle + (size_t)fs;
    double err = 0.0, sig = 0.0;
    size_t n = 0;
    while (n < total) {
        double in_l[N], in_r[N];
        for (size_t i = 0; i < N; i++) {
            const double t = (double)(n + i) / fs;
            in_l[i] = bass_amp * std::sin(2 * M_PI * 40.0 * t) + 0.1 * std::sin(2 * M_PI * 1000.0 * t);
            in_r[i] = bass_amp * std::sin(2 * M_PI * 40.0 * t + 0.7) + 0.1 * std::sin(2 * M_PI * 997.0 * t);
            blk[2 * i] = (float)in_l[i];
            blk[2 * i + 1] = (float)in_r[i];
        }
        for (size_t k = 0; k < c.size(); k++) svf_stereo(blk, N, c[k], st[k]);
        for (size_t i = 0; i < N; i++) {
            // the reference takes the same float input the kernel did
            double yl = (double)(float)in_l[i], yr = (double)(float)in_r[i];
            for (size_t k = 0; k < c.size(); k++) { yl = ref[k].tick(yl); yr = refr[k].tick(yr); }
            if (n + i >= settle) {
                err += (blk[2 * i] - yl) * (blk[2 * i] - yl) + (blk[2 * i + 1] - yr) * (blk[2 * i + 1] - yr);
                sig += yl * yl + yr * yr;
            }
        }
        n += N;
    }
    return 10.0 * std::log10(err / sig + 1e-300);
}

}  // namespace

TEST_CASE("SVF design: bilinear, every band type has the RBJ cookbook's response") {
    const BandType types[] = {BandType::PEAK, BandType::LOW_SHELF, BandType::HIGH_SHELF,
                              BandType::LOW_PASS, BandType::HIGH_PASS, BandType::NOTCH};
    double worst = 0.0;
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0})
        for (BandType t : types)
            for (float f : {20.0f, 40.0f, 120.0f, 1000.0f, 8000.0f, 16000.0f})
                for (float q : {0.3f, 0.71f, 2.0f, 8.0f})
                    for (float g : {-12.0f, -3.0f, 4.2f, 12.0f}) {
                        // Shelves take Q up to SHELF_Q_MAX: compare at the Q the design uses.
                        const bool shelf = t == BandType::LOW_SHELF || t == BandType::HIGH_SHELF;
                        const EqBand b = band(t, f, g, shelf ? std::min(q, SHELF_Q_MAX) : q);
                        Bq r; SvfDesign d;
                        REQUIRE(rbj(b, fs, r));
                        REQUIRE(design_svf_params(b, fs, d, Design::BILINEAR));
                        for (double fq = 10.0; fq < fs * 0.499; fq *= 1.17) {
                            const double want = bq_mag_db(r, fs, fq);
                            if (want < -120.0) continue;          // deep in a notch or a stopband
                            worst = std::max(worst, std::fabs(svf_magnitude_db(d, fs, fq) - want));
                        }
                    }
    CHECK(worst < 1e-6);
}

TEST_CASE("SVF design: the section IS the RBJ filter in the time domain, phase included") {
    // Same magnitude is not enough — a different phase would change what two bands sum to.
    uint32_t seed = 7;
    for (double fs : {48000.0, 192000.0})
        for (const EqBand& b : {band(BandType::PEAK, 62, 6, 1), band(BandType::LOW_SHELF, 40, 12, 0.71f),
                                band(BandType::HIGH_SHELF, 12000, 4.2f, 0.71f), band(BandType::LOW_PASS, 120, 0, 0.7071f),
                                band(BandType::HIGH_PASS, 35, 0, 0.5412f), band(BandType::NOTCH, 50, 0, 8)}) {
            Bq r; SvfDesign d;
            REQUIRE(rbj(b, fs, r));
            REQUIRE(design_svf_params(b, fs, d, Design::BILINEAR));
            SvfD s(d);
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0, worst = 0;
            for (int i = 0; i < 20000; i++) {
                const double x = (double)(int32_t)lcg(seed) / 2147483648.0;
                const double y = r.b0 * x + r.b1 * x1 + r.b2 * x2 - r.a1 * y1 - r.a2 * y2;
                x2 = x1; x1 = x; y2 = y1; y1 = y;
                worst = std::max(worst, std::fabs(s.tick(x) - y));
            }
            CAPTURE(fs); CAPTURE(b.freq_hz);
            CHECK(worst < 1e-8);
        }
}

TEST_CASE("SVF kernel: the stereo loops are svf_tick, bit for bit, across uneven blocks") {
    SvfCoef c;
    REQUIRE(design_svf(band(BandType::LOW_SHELF, 40, 6, 0.71f), 96000.0, c));
    SvfState a, b;
    float l1 = 0, l2 = 0, r1 = 0, r2 = 0;
    uint32_t seed = 12345;
    for (int len : {240, 1, 239, 7, 240, 0, 64, 240}) {
        float x[480], y[480], ref[480];
        for (int i = 0; i < 2 * len; i++) x[i] = (float)(int32_t)lcg(seed) / 2147483648.0f;
        memcpy(y, x, sizeof(float) * 2 * len);
        float z[480];
        svf_stereo(y, (size_t)len, c, a);                 // in place
        svf_stereo_to(x, z, (size_t)len, c, b);           // out of place
        for (int i = 0; i < len; i++) {
            ref[2 * i] = svf_tick(x[2 * i], c, l1, l2);
            ref[2 * i + 1] = svf_tick(x[2 * i + 1], c, r1, r2);
        }
        CHECK(memcmp(y, ref, sizeof(float) * 2 * len) == 0);
        CHECK(memcmp(z, ref, sizeof(float) * 2 * len) == 0);
    }
    CHECK(a.s[0] == l1); CHECK(a.s[1] == l2); CHECK(a.s[2] == r1); CHECK(a.s[3] == r2);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);
}

TEST_CASE("SVF kernel: two sections in one pass are the two passes, bit for bit") {
    // svf_stereo2 shares the loop, not the arithmetic: every sample must meet exactly the
    // operations svf_stereo(a) and then svf_stereo(b) apply, states included, over blocks of
    // any length — and with the same section given twice, the case a cascade of equal
    // sections (the subsonic's pair) is closest to.
    SvfCoef lo, pk, hp;
    REQUIRE(design_svf(band(BandType::LOW_SHELF, 46, 7.2f, 0.707f), 192000.0, lo));
    REQUIRE(design_svf(band(BandType::PEAK, 62.5f, 3.6f, 1.0f), 192000.0, pk));
    REQUIRE(design_svf(band(BandType::HIGH_PASS, 40, 0, 0.5412f), 192000.0, hp));
    const SvfCoef* pairs[][2] = {{&lo, &pk}, {&pk, &lo}, {&hp, &hp}, {&lo, &lo}};
    for (auto& pr : pairs) {
        SvfState a1, b1, a2, b2;
        uint32_t seed = 4242;
        for (int len : {240, 1, 239, 7, 240, 0, 64, 240, 2, 240}) {
            float x[480], y[480];
            for (int i = 0; i < 2 * len; i++) x[i] = (float)(int32_t)lcg(seed) / 2147483648.0f;
            memcpy(y, x, sizeof(float) * 2 * len);
            svf_stereo2(x, (size_t)len, *pr[0], a1, *pr[1], b1);
            svf_stereo(y, (size_t)len, *pr[0], a2);
            svf_stereo(y, (size_t)len, *pr[1], b2);
            CHECK(memcmp(x, y, sizeof(float) * 2 * len) == 0);
        }
        CHECK(memcmp(&a1, &a2, sizeof(a1)) == 0);
        CHECK(memcmp(&b1, &b2, sizeof(b1)) == 0);
    }
}

TEST_CASE("SVF kernel: the customer's EQ adds no audible noise on bass, at any rate") {
    // Direct Form II measured −51/−49/−36/−23 dB here; the threshold is the one set for the
    // SVF, which measured −130/−127/−125/−114.
    for (double fs : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const double db = kernel_noise_db(customer_eq(), fs, 0.25f);
        MESSAGE("customer EQ, " << fs << " Hz: kernel noise " << db << " dB");
        CAPTURE(fs); CAPTURE(db);
        CHECK(db <= (fs > 100000.0 ? -105.0 : -110.0));
    }
}

TEST_CASE("SVF kernel: the UI's extremes stay far below audibility too") {
    // The corners the UI allows: +12 dB, Q up to 8, a shelf at 20 Hz, at the highest rate.
    const std::vector<EqBand> eq = {band(BandType::LOW_SHELF, 20, 12, 0.71f), band(BandType::PEAK, 30, 12, 8),
                                    band(BandType::PEAK, 45, -12, 4), band(BandType::HIGH_SHELF, 16000, 12, 1)};
    for (double fs : {48000.0, 192000.0}) {
        const double db = kernel_noise_db(eq, fs, 0.05f);
        MESSAGE("UI extremes, " << fs << " Hz: kernel noise " << db << " dB");
        CAPTURE(fs); CAPTURE(db);
        CHECK(db <= -90.0);
    }
}

TEST_CASE("eq_curve_db: designs once and draws exactly what eq_magnitude_db says") {
    const auto eq = customer_eq();
    float f[97], a[97];
    for (int i = 0; i < 97; i++) f[i] = 20.0f * std::pow(1000.0f, (float)i / 96.0f);
    eq_curve_db(eq.data(), (int)eq.size(), f, 97, a);
    // The screen's curve is evaluated in float: equal to the double one to well under a
    // hundredth of a dB, which is all a 480-pixel plot can show.
    for (int i = 0; i < 97; i++)
        CHECK(a[i] == doctest::Approx(eq_magnitude_db(eq.data(), (int)eq.size(), f[i])).scale(1.0).epsilon(1e-3));
    // A disabled band and a band that cannot be designed contribute nothing.
    EqBand off = eq[0]; off.enabled = false;
    EqBand bad = eq[0]; bad.freq_hz = 5.0f;
    eq_curve_db(&off, 1, f, 97, a);
    for (float v : a) CHECK(v == 0.0f);
    eq_curve_db(&bad, 1, f, 97, a);
    for (float v : a) CHECK(v == 0.0f);
}
