/**
 * @file test_spectrum.cpp
 * @brief The spectrum analyser, checked against tones of known frequency and level.
 */

#include "doctest.h"
#include "spectrum.h"
#include "dsp_chain.h"
#include <cmath>
#include <cstring>
#include <vector>

using namespace audio_core;

namespace {

/// What the two tasks do on the panel, in one: the audio task's feed, then the analysis.
bool step(SpectrumAnalyzer& a, const float* st, size_t n) {
    a.feed(st, n);
    return a.analyse();
}

/// Feed `secs` of a stereo sine at `hz`, `amp` peak, and return the last band frame.
std::vector<float> analyse_tone(float hz, float amp, float secs = 0.5f) {
    SpectrumAnalyzer a;
    a.reset();
    constexpr size_t BLK = 240;
    float st[2 * BLK];
    double ph = 0.0;
    const size_t blocks = (size_t)(secs * FS_HZ_DEFAULT / BLK);
    for (size_t b = 0; b < blocks; b++) {
        for (size_t i = 0; i < BLK; i++) {
            const float v = amp * sinf((float)ph);
            st[2 * i] = v; st[2 * i + 1] = v;
            ph += 2.0 * M_PI * (double)hz / (double)FS_HZ_DEFAULT;
        }
        step(a, st, BLK);
    }
    return std::vector<float>(a.bands_db(), a.bands_db() + SPECTRUM_BANDS);
}

int loudest(const std::vector<float>& b) {
    int best = 0;
    for (int i = 1; i < SPECTRUM_BANDS; i++) if (b[i] > b[best]) best = i;
    return best;
}

} // namespace

TEST_CASE("a tone lands in the band that contains it, where resolution allows") {
    // Only above ~200 Hz is a log band wider than one FFT bin; below that, several bands
    // share bins by construction and "which band" stops being a meaningful question.
    for (float hz : {250.0f, 440.0f, 1000.0f, 4000.0f, 10000.0f}) {
        const auto b = analyse_tone(hz, 0.5f);
        const int k = loudest(b);
        // The peak band's nominal centre must be within half a band of the tone.
        const float ratio = spectrum_band_hz(k) / hz;
        const float half_band = powf(SPECTRUM_F_HI / SPECTRUM_F_LO, 0.5f / SPECTRUM_BANDS);
        CHECK(ratio > 1.0f / (half_band * 1.35f));
        CHECK(ratio < half_band * 1.35f);
    }
}

TEST_CASE("the lowest bands share bins — the documented resolution limit, pinned") {
    // Not a defect to be fixed by a test tolerance: at 46.9 Hz per bin a 40..50 Hz band is
    // a fraction of one bin wide. This asserts the limit is where the header says it is, so
    // a future window-size change has to update the claim rather than silently move it.
    const float bin_hz = FS_HZ_DEFAULT / (float)SPECTRUM_FFT_N;
    int shared = 0;
    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        const float r  = SPECTRUM_F_HI / SPECTRUM_F_LO;
        const float f0 = SPECTRUM_F_LO * powf(r, (float)i / (float)SPECTRUM_BANDS);
        const float f1 = SPECTRUM_F_LO * powf(r, (float)(i + 1) / (float)SPECTRUM_BANDS);
        if ((f1 - f0) < bin_hz) shared++;
    }
    CHECK(shared > 0);                       // the limit exists
    CHECK(spectrum_band_hz(shared) > 180.0f); // and it ends around 200 Hz, as documented
    CHECK(spectrum_band_hz(shared) < 260.0f);
}

TEST_CASE("level is reported in dBFS: full scale reads ~0 dB, half reads ~-6") {
    const auto full = analyse_tone(1000.0f, 1.0f);
    const auto half = analyse_tone(1000.0f, 0.5f);
    const int kf = loudest(full), kh = loudest(half);
    // Absolute bounds, not Approx: doctest's epsilon is relative, so Approx(0).epsilon(x)
    // demands exact equality and says nothing useful. The contract a level display owes is
    // "within half a dB of full scale, and never reads above it" — leakage outside the
    // summed bins accounts for the rest.
    CHECK(full[kf] <= 0.0f);
    CHECK(full[kf] > -0.5f);
    CHECK(half[kh] == doctest::Approx(-6.0f).epsilon(0.1));
    // and a 6 dB change in the signal is a 6 dB change on screen
    CHECK((full[kf] - half[kh]) == doctest::Approx(6.0f).epsilon(0.05));
}

TEST_CASE("silence sits on the floor, not below it and not above") {
    SpectrumAnalyzer a;
    a.reset();
    constexpr size_t BLK = 240;
    float st[2 * BLK] = {};
    for (int i = 0; i < 400; i++) step(a, st, BLK);
    for (int i = 0; i < SPECTRUM_BANDS; i++) {
        CHECK(a.bands_db()[i] == doctest::Approx(SPECTRUM_FLOOR_DB));
    }
}

TEST_CASE("feed only reports a frame when a whole window has been filled") {
    SpectrumAnalyzer a;
    a.reset();
    float st[2 * 100] = {};
    int frames = 0;
    // 1024-sample window fed 100 frames at a time: the 11th call completes it (1100 > 1024).
    for (int i = 0; i < 11; i++) if (step(a, st, 100)) frames++;
    CHECK(frames == 1);
}

TEST_CASE("bands are log-spaced and cover the design's range") {
    CHECK(spectrum_band_hz(0) > SPECTRUM_F_LO);
    CHECK(spectrum_band_hz(SPECTRUM_BANDS - 1) < SPECTRUM_F_HI);
    // every step is the same ratio — that is what "log-spaced" has to mean
    const float r0 = spectrum_band_hz(1) / spectrum_band_hz(0);
    for (int i = 1; i < SPECTRUM_BANDS - 1; i++) {
        CHECK((spectrum_band_hz(i + 1) / spectrum_band_hz(i)) == doctest::Approx(r0).epsilon(1e-4));
    }
}

TEST_CASE("bus: nothing to read before the first publish, exact frame after") {
    SpectrumBus bus;
    float out[SPECTRUM_BANDS];
    CHECK_FALSE(bus.read(out));

    float in[SPECTRUM_BANDS];
    for (int i = 0; i < SPECTRUM_BANDS; i++) in[i] = -1.0f * (float)i;
    bus.publish(in);
    REQUIRE(bus.read(out));
    for (int i = 0; i < SPECTRUM_BANDS; i++) CHECK(out[i] == doctest::Approx(in[i]));

    for (int i = 0; i < SPECTRUM_BANDS; i++) in[i] = -60.0f + (float)i;
    bus.publish(in);
    REQUIRE(bus.read(out));
    for (int i = 0; i < SPECTRUM_BANDS; i++) CHECK(out[i] == doctest::Approx(in[i]));
}

TEST_CASE("decay is gradual, attack is immediate") {
    SpectrumAnalyzer a;
    a.reset();
    constexpr size_t BLK = 240;
    float st[2 * BLK];
    double ph = 0.0;
    // establish a loud 1 kHz tone
    for (int b = 0; b < 100; b++) {
        for (size_t i = 0; i < BLK; i++) {
            const float v = 0.9f * sinf((float)ph);
            st[2 * i] = v; st[2 * i + 1] = v;
            ph += 2.0 * M_PI * 1000.0 / (double)FS_HZ_DEFAULT;
        }
        step(a, st, BLK);
    }
    const int k = loudest(std::vector<float>(a.bands_db(), a.bands_db() + SPECTRUM_BANDS));
    const float loud = a.bands_db()[k];
    CHECK(loud > -3.0f);

    // cut to silence: one window later it must have fallen by the decay step, not to the floor
    float quiet[2 * BLK] = {};
    while (!step(a, quiet, BLK)) { }        // exactly one more window (50 % overlap hop)
    CHECK(a.bands_db()[k] < loud);
    CHECK(a.bands_db()[k] > loud - 6.0f);
}

TEST_CASE("above 50 kHz the analyser runs on a 48 kHz-equivalent stream: same bands, same rate of frames") {
    const float saved = audio_core::g_fs_hz;
    for (float fs : {96000.0f, 192000.0f}) {
        audio_core::g_fs_hz = fs;
        audio_core::SpectrumAnalyzer a;
        a.reset();
        // One second of a 1 kHz tone at −6 dBFS: frames produced, and the band it lands in.
        const size_t total = (size_t)fs;
        std::vector<float> st(2 * 480);
        int frames = 0;
        double ph = 0.0;
        for (size_t done = 0; done < total; done += 480) {
            for (size_t i = 0; i < 480; i++) {
                st[2 * i] = st[2 * i + 1] = 0.5f * (float)std::sin(ph);
                ph += 2.0 * M_PI * 1000.0 / fs;
            }
            frames += step(a, st.data(), 480) ? 1 : 0;
        }
        CAPTURE(fs);
        CHECK(a.decimation() == (fs > 100000.0f ? 4 : 2));
        CHECK(frames >= 90);                               // ~94 a second, as at 48 kHz
        CHECK(frames <= 96);
        int loudest = 0;
        for (int b = 1; b < audio_core::SPECTRUM_BANDS; b++)
            if (a.bands_db()[b] > a.bands_db()[loudest]) loudest = b;
        const float ratio = audio_core::spectrum_band_hz(loudest) / 1000.0f;
        const float half_band = powf(audio_core::SPECTRUM_F_HI / audio_core::SPECTRUM_F_LO,
                                     0.5f / audio_core::SPECTRUM_BANDS);
        CHECK(ratio > 1.0f / (half_band * 1.35f));
        CHECK(ratio < half_band * 1.35f);
        CHECK(a.bands_db()[loudest] == doctest::Approx(-6.0f).epsilon(0.1));
    }
    audio_core::g_fs_hz = saved;
}

// ── the real-input transform and the two-task split ──

namespace {

/// The band levels of one window, worked out the long way: the window, a direct DFT in double
/// precision, the same bins, the same scale. What the 512-point transform must reproduce.
std::vector<float> direct_bands(const std::vector<float>& x) {
    const int N = SPECTRUM_FFT_N;
    std::vector<double> w(N);
    double sq = 0.0;
    for (int i = 0; i < N; i++) {
        w[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (N - 1)));
        sq += w[i] * w[i];
    }
    const double norm = 2.0 / (std::sqrt(sq / N) * N);
    std::vector<double> pw(N / 2, 0.0);
    for (int k = 1; k < N / 2; k++) {
        double re = 0.0, im = 0.0;
        for (int n = 0; n < N; n++) {
            re += x[n] * w[n] * std::cos(2.0 * M_PI * k * n / N);
            im -= x[n] * w[n] * std::sin(2.0 * M_PI * k * n / N);
        }
        pw[k] = re * re + im * im;
    }
    std::vector<float> out(SPECTRUM_BANDS);
    const float fs = FS_HZ_DEFAULT, r = SPECTRUM_F_HI / SPECTRUM_F_LO, bin_hz = fs / N;
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        const float f0 = SPECTRUM_F_LO * powf(r, (float)b / SPECTRUM_BANDS);
        const float f1 = SPECTRUM_F_LO * powf(r, (float)(b + 1) / SPECTRUM_BANDS);
        int lo = (int)(f0 / bin_hz), hi = (int)(f1 / bin_hz);
        if (lo < 1) lo = 1;
        if (hi <= lo) hi = lo + 1;
        if (hi > N / 2) hi = N / 2;
        if (lo >= hi) lo = hi - 1;
        double p = 0.0;
        for (int k = lo; k < hi; k++) p += pw[k];
        const double mag = std::sqrt(p) * norm;
        double db = mag > 1e-7 ? 20.0 * std::log10(mag) : SPECTRUM_FLOOR_DB;
        if (db < SPECTRUM_FLOOR_DB) db = SPECTRUM_FLOOR_DB;
        if (db > SPECTRUM_BAND_REF_DB) db = SPECTRUM_BAND_REF_DB;
        out[b] = (float)db;
    }
    return out;
}

/// One window of three tones and a little noise, fed as stereo, analysed once.
std::vector<float> analysed_window(std::vector<float>& mono) {
    const int N = SPECTRUM_FFT_N;
    mono.assign(N, 0.0f);
    uint32_t seed = 12345;
    for (int n = 0; n < N; n++) {
        seed = seed * 1664525u + 1013904223u;
        const float noise = ((float)(seed >> 8) / 16777216.0f - 0.5f) * 0.002f;
        mono[n] = 0.4f * sinf(2.0f * (float)M_PI * 97.0f * n / FS_HZ_DEFAULT)
                + 0.2f * sinf(2.0f * (float)M_PI * 1234.5f * n / FS_HZ_DEFAULT)
                + 0.05f * sinf(2.0f * (float)M_PI * 9876.0f * n / FS_HZ_DEFAULT) + noise;
    }
    std::vector<float> st(2 * N);
    for (int n = 0; n < N; n++) st[2 * n] = st[2 * n + 1] = mono[n];
    SpectrumAnalyzer a;
    a.reset();
    a.feed(st.data(), N);
    REQUIRE(a.analyse());
    return std::vector<float>(a.bands_db(), a.bands_db() + SPECTRUM_BANDS);
}

} // namespace

TEST_CASE("the 512-point transform of the real window gives the bands a direct DFT gives") {
    std::vector<float> mono;
    const auto got = analysed_window(mono);
    const auto want = direct_bands(mono);
    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        CAPTURE(b);
        // Single precision against double: hundredths of a dB where there is signal; on the
        // floor both say the floor.
        CHECK(std::fabs(got[b] - want[b]) < 0.02f);
    }
}

TEST_CASE("analysis: nothing before a whole window, and only once per hop") {
    SpectrumAnalyzer a;
    a.reset();
    std::vector<float> st(2 * SPECTRUM_HOP, 0.1f);
    a.feed(st.data(), SPECTRUM_HOP);
    CHECK_FALSE(a.analyse());                // half a window
    a.feed(st.data(), SPECTRUM_HOP);
    CHECK(a.analyse());                      // a whole one
    CHECK_FALSE(a.analyse());                // the same one again: nothing new
    a.feed(st.data(), SPECTRUM_HOP - 1);
    CHECK_FALSE(a.analyse());                // a hop not yet complete
    a.feed(st.data(), 1);
    CHECK(a.analyse());
}

TEST_CASE("analysis that falls behind still decays by the hops it missed") {
    SpectrumAnalyzer a;
    a.reset();
    constexpr size_t BLK = 240;
    float st[2 * BLK];
    double ph = 0.0;
    for (int b = 0; b < 40; b++) {
        for (size_t i = 0; i < BLK; i++) {
            st[2 * i] = st[2 * i + 1] = 0.9f * sinf((float)ph);
            ph += 2.0 * M_PI * 1000.0 / (double)FS_HZ_DEFAULT;
        }
        step(a, st, BLK);
    }
    const int k = loudest(std::vector<float>(a.bands_db(), a.bands_db() + SPECTRUM_BANDS));
    const float loud = a.bands_db()[k];
    // Three hops of silence fed while the analysis is busy elsewhere, then one look.
    std::vector<float> quiet(2 * 3 * SPECTRUM_HOP, 0.0f);
    a.feed(quiet.data(), 3 * SPECTRUM_HOP);
    REQUIRE(a.analyse());
    CHECK(a.bands_db()[k] == doctest::Approx(loud - 3 * 1.2f).epsilon(0.001));
}

TEST_CASE("a restart drops the window and puts the bands on the floor") {
    SpectrumAnalyzer a;
    a.reset();
    std::vector<float> st(2 * SPECTRUM_FFT_N);
    for (int n = 0; n < SPECTRUM_FFT_N; n++) st[2 * n] = st[2 * n + 1] = 0.5f * sinf(0.3f * n);
    a.feed(st.data(), SPECTRUM_FFT_N);
    REQUIRE(a.analyse());
    a.restart();
    a.feed(st.data(), SPECTRUM_HOP);
    CHECK_FALSE(a.analyse());                // half a window since the restart
    for (int b = 0; b < SPECTRUM_BANDS; b++) CHECK(a.bands_db()[b] == doctest::Approx(SPECTRUM_FLOOR_DB));
}
