/**
 * @file test_dsp_chain.cpp
 * @brief HOST tests for the audio core DSP chain (components/audio_dsp/src/dsp_chain.cpp):
 *        band design (magnitude at the design frequency), volume taper + ramp, peak
 *        limiter ceiling, tone generator level. Pure float math, no IDF. The SVF kernel
 *        itself is held to the RBJ cookbook in test_dsp_svf.cpp.
 */

#include <cmath>
#include <complex>
#include <cstring>
#include "doctest.h"
#include "dsp_chain.h"

using namespace audio_core;

namespace {

/// The designed band's magnitude at frequency f (Hz), at the default rate.
struct Designed {
    SvfDesign d;
    double operator()(double f) const { return svf_magnitude_db(d, FS_HZ_DEFAULT, f); }
};
bool design(const EqBand& b, Designed& out) { return design_svf_params(b, FS_HZ_DEFAULT, out.d); }

float peak_abs(const float* stereo, size_t frames) {
    float p = 0.0f;
    for (size_t i = 0; i < frames * 2; i++) p = std::max(p, std::fabs(stereo[i]));
    return p;
}

void fill_sine(float* stereo, size_t frames, float amp, float hz, float& phase) {
    for (size_t i = 0; i < frames; i++) {
        const float s = amp * std::sin(2.0f * (float)M_PI * phase);
        stereo[2 * i] = stereo[2 * i + 1] = s;
        phase += hz / FS_HZ_DEFAULT;
        if (phase >= 1.0f) phase -= 1.0f;
    }
}

} // namespace

TEST_CASE("band design: peaking EQ hits its gain at f0 and is flat far away") {
    EqBand b; b.type = BandType::PEAK; b.freq_hz = 1000; b.gain_db = 6.0f; b.q = 1.0f;
    Designed h;
    REQUIRE(design(b, h));
    CHECK(h(1000) == doctest::Approx(6.0).epsilon(0.02));
    CHECK(std::fabs(h(20)) < 0.1);
    CHECK(std::fabs(h(20000)) < 0.1);

    b.gain_db = -9.0f;
    REQUIRE(design(b, h));
    CHECK(h(1000) == doctest::Approx(-9.0).epsilon(0.02));
}

TEST_CASE("band design: shelves reach their gain at the band edges") {
    EqBand lo; lo.type = BandType::LOW_SHELF; lo.freq_hz = 100; lo.gain_db = 4.0f; lo.q = 0.7f;
    Designed h;
    REQUIRE(design(lo, h));
    CHECK(h(10) == doctest::Approx(4.0).epsilon(0.05));
    CHECK(std::fabs(h(10000)) < 0.1);

    EqBand hi; hi.type = BandType::HIGH_SHELF; hi.freq_hz = 10000; hi.gain_db = -3.0f; hi.q = 0.7f;
    REQUIRE(design(hi, h));
    CHECK(h(20000) == doctest::Approx(-3.0).epsilon(0.05));
    CHECK(std::fabs(h(100)) < 0.1);
}

TEST_CASE("band design: low/high pass and notch") {
    Designed h;
    EqBand lp; lp.type = BandType::LOW_PASS; lp.freq_hz = 1000; lp.q = 0.7071f;
    REQUIRE(design(lp, h));
    CHECK(h(1000) == doctest::Approx(-3.0).epsilon(0.05));
    CHECK(h(10000) < -35.0);

    EqBand hp; hp.type = BandType::HIGH_PASS; hp.freq_hz = 100; hp.q = 0.7071f;
    REQUIRE(design(hp, h));
    CHECK(h(100) == doctest::Approx(-3.0).epsilon(0.05));
    CHECK(h(10) < -35.0);

    EqBand n; n.type = BandType::NOTCH; n.freq_hz = 50; n.q = 10.0f;
    REQUIRE(design(n, h));
    CHECK(h(50) < -40.0);
    CHECK(std::fabs(h(1000)) < 0.1);
}

TEST_CASE("band design: rejects impossible bands") {
    Designed h;
    SvfCoef c;
    EqBand bad; bad.freq_hz = 5.0f;
    CHECK_FALSE(design(bad, h));
    CHECK_FALSE(design_svf(bad, FS_HZ_DEFAULT, c));
    bad.freq_hz = 1000; bad.q = 0.0f;
    CHECK_FALSE(design(bad, h));
    bad.q = 1.0f; bad.freq_hz = FS_HZ_DEFAULT * 0.6f;
    CHECK_FALSE(design(bad, h));
    CHECK_FALSE(design_svf(bad, FS_HZ_DEFAULT, c));
    // A gain that is not a number would design NaN coefficients.
    bad.freq_hz = 1000;
    for (BandType t : {BandType::PEAK, BandType::LOW_SHELF, BandType::HIGH_SHELF}) {
        for (float g : {INFINITY, -INFINITY, NAN}) {
            bad.type = t;
            bad.gain_db = g;
            CHECK_FALSE(design(bad, h));
            CHECK_FALSE(design_svf(bad, FS_HZ_DEFAULT, c));
            CHECK_FALSE(design_svf_params(bad, FS_HZ_DEFAULT, h.d, Design::BILINEAR));
        }
    }
}

TEST_CASE("chain: a band whose gain is not a number is a wire, not a silenced block") {
    for (float g : {INFINITY, -INFINITY, NAN}) {
        DspChain chain;
        chain.reset();
        EqBand b[2];
        b[0].type = BandType::PEAK; b[0].freq_hz = 1000; b[0].gain_db = g; b[0].q = 1.0f; b[0].enabled = true;
        b[1].type = BandType::PEAK; b[1].freq_hz = 1000; b[1].gain_db = -6.0f; b[1].q = 1.0f; b[1].enabled = true;
        chain.set_bands(b, 2);
        chain.set_eq_enabled(true);
        chain.set_volume_percent(100);
        CHECK(std::isfinite(chain.headroom_db()));
        CHECK(band_magnitude_db(b[0], 1000.0f) == 0.0f);   // the screen draws what plays

        float blk[DspChain::BLOCK * 2];
        float phase = 0.0f, peak = 0.0f;
        for (int n = 0; n < 60; n++) {
            fill_sine(blk, DspChain::BLOCK, 0.5f, 1000.0f, phase);
            chain.process(blk, DspChain::BLOCK);
            for (float v : blk) REQUIRE(std::isfinite(v));
            if (n >= 40) peak = std::max(peak, peak_abs(blk, DspChain::BLOCK));
        }
        CHECK(chain.faults() == 0);
        CHECK(20.0 * std::log10(peak / 0.5) == doctest::Approx(-6.0).epsilon(0.03));   // the other band alone
    }
}

TEST_CASE("chain: EQ applies the band gain to a sine at f0 (stereo, in place)") {
    DspChain chain;
    chain.reset();
    EqBand b; b.type = BandType::PEAK; b.freq_hz = 1000; b.gain_db = -12.0f; b.q = 1.0f; b.enabled = true;
    chain.set_bands(&b, 1);
    chain.set_eq_enabled(true);
    chain.set_volume_percent(100);

    float blk[DspChain::BLOCK * 2];
    float phase = 0.0f;
    float peak = 0.0f;
    // let the volume ramp and the filter settle, then measure
    for (int n = 0; n < 60; n++) {
        fill_sine(blk, DspChain::BLOCK, 0.5f, 1000.0f, phase);
        chain.process(blk, DspChain::BLOCK);
        if (n >= 40) peak = std::max(peak, peak_abs(blk, DspChain::BLOCK));
    }
    const double db = 20.0 * std::log10(peak / 0.5);
    CHECK(db == doctest::Approx(-12.0).epsilon(0.03));
}

TEST_CASE("chain: volume taper — 100 % = 0 dB, 50 % ≈ −30 dB, 0 % = silence; mute") {
    DspChain chain;
    float blk[DspChain::BLOCK * 2];

    auto settled_peak = [&](int pct, bool mute) {
        chain.reset();
        chain.set_volume_percent(pct);
        chain.set_mute(mute);
        float phase = 0.0f, peak = 0.0f;
        for (int n = 0; n < 60; n++) {
            fill_sine(blk, DspChain::BLOCK, 0.25f, 1000.0f, phase);
            chain.process(blk, DspChain::BLOCK);
            if (n >= 40) peak = std::max(peak, peak_abs(blk, DspChain::BLOCK));
        }
        return peak;
    };

    CHECK(settled_peak(100, false) == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(20.0 * std::log10(settled_peak(50, false) / 0.25) == doctest::Approx(-30.0).epsilon(0.02));
    CHECK(settled_peak(0, false) < 1e-4f);
    CHECK(settled_peak(100, true) < 1e-4f);
}

TEST_CASE("chain: the limiter keeps peaks at the ceiling and never above it") {
    DspChain chain;
    chain.reset();
    chain.set_volume_percent(100);
    float blk[DspChain::BLOCK * 2];
    float phase = 0.0f, peak = 0.0f;
    for (int n = 0; n < 60; n++) {
        fill_sine(blk, DspChain::BLOCK, 3.0f, 440.0f, phase);   // +9.5 dBFS input
        chain.process(blk, DspChain::BLOCK);
        if (n >= 40) peak = std::max(peak, peak_abs(blk, DspChain::BLOCK));
    }
    CHECK(peak <= 1.0f);
    CHECK(peak == doctest::Approx(CEILING).epsilon(0.02));
    CHECK(chain.limiter_gain() < 0.5f);
    CHECK(chain.peak_l() == doctest::Approx(peak).epsilon(0.05));
}

TEST_CASE("chain: EQ bypass passes the signal untouched") {
    DspChain chain;
    chain.reset();
    EqBand b; b.type = BandType::PEAK; b.freq_hz = 1000; b.gain_db = -12.0f; b.q = 1.0f; b.enabled = true;
    chain.set_bands(&b, 1);
    chain.set_eq_enabled(false);
    chain.set_volume_percent(100);
    float blk[DspChain::BLOCK * 2];
    float phase = 0.0f, peak = 0.0f;
    for (int n = 0; n < 60; n++) {
        fill_sine(blk, DspChain::BLOCK, 0.5f, 1000.0f, phase);
        chain.process(blk, DspChain::BLOCK);
        if (n >= 40) peak = std::max(peak, peak_abs(blk, DspChain::BLOCK));
    }
    CHECK(peak == doctest::Approx(0.5f).epsilon(0.01));
}

TEST_CASE("tone: −12 dBFS sine, phase-continuous, frequency clamped") {
    ToneGen tone;
    tone.set_freq(1000.0f);
    float blk[DspChain::BLOCK * 2];
    float peak = 0.0f;
    for (int n = 0; n < 20; n++) {
        tone.fill(blk, DspChain::BLOCK);
        peak = std::max(peak, peak_abs(blk, DspChain::BLOCK));
    }
    CHECK(peak == doctest::Approx(TONE_LEVEL).epsilon(0.01));
    // both channels identical
    tone.fill(blk, DspChain::BLOCK);
    for (size_t i = 0; i < DspChain::BLOCK; i++) CHECK(blk[2 * i] == blk[2 * i + 1]);
    // clamp: absurd frequencies must not blow up the phase step
    tone.set_freq(1e9f);
    tone.fill(blk, DspChain::BLOCK);
    CHECK(peak_abs(blk, DspChain::BLOCK) <= TONE_LEVEL + 1e-4f);
}

TEST_CASE("tone: the calibration tone is level-exact, on the channels asked for") {
    ToneGen tone;
    tone.set_freq(60.0f);
    float blk[DspChain::BLOCK * 2];
    const float amp = std::pow(10.0f, -26.0f / 20.0f);        // −26 dBFS
    for (int mask : {1, 2, 3}) {
        float pl = 0.0f, pr = 0.0f;
        for (int n = 0; n < 20; n++) {                         // 0.1 s: six cycles of 60 Hz
            tone.fill_at(blk, DspChain::BLOCK, amp, mask);
            for (size_t i = 0; i < DspChain::BLOCK; i++) {
                pl = std::max(pl, std::fabs(blk[2 * i]));
                pr = std::max(pr, std::fabs(blk[2 * i + 1]));
            }
        }
        CAPTURE(mask);
        CHECK(pl == ((mask & 1) ? doctest::Approx(amp).epsilon(1e-4) : doctest::Approx(0.0f)));
        CHECK(pr == ((mask & 2) ? doctest::Approx(amp).epsilon(1e-4) : doctest::Approx(0.0f)));
    }
}

// ═══════════════════════════════════════════════════════════════
// Magnitude response — what the EQ screen draws
// ═══════════════════════════════════════════════════════════════

TEST_CASE("band_magnitude_db: peak lifts only around its centre") {
    audio_core::EqBand b;
    b.type = audio_core::BandType::PEAK;
    b.freq_hz = 1000.0f;
    b.gain_db = 6.0f;
    b.q = 2.0f;
    b.enabled = true;

    // At the centre the peak gain is exact by construction of the RBJ design.
    CHECK(audio_core::band_magnitude_db(b, 1000.0f) == doctest::Approx(6.0f).epsilon(0.01));
    // Three octaves away a Q of 2 has essentially stopped acting.
    CHECK(audio_core::band_magnitude_db(b, 125.0f) == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(audio_core::band_magnitude_db(b, 8000.0f) == doctest::Approx(0.0f).epsilon(0.05));
    // A cut is the mirror image.
    b.gain_db = -6.0f;
    CHECK(audio_core::band_magnitude_db(b, 1000.0f) == doctest::Approx(-6.0f).epsilon(0.01));
}

TEST_CASE("band_magnitude_db: a disabled band is flat, whatever its settings") {
    audio_core::EqBand b;
    b.type = audio_core::BandType::PEAK;
    b.freq_hz = 1000.0f;
    b.gain_db = 12.0f;
    b.q = 1.0f;
    b.enabled = false;
    CHECK(audio_core::band_magnitude_db(b, 1000.0f) == doctest::Approx(0.0f));
}

TEST_CASE("band_magnitude_db: Butterworth low-pass is -3 dB at its corner") {
    audio_core::EqBand b;
    b.type = audio_core::BandType::LOW_PASS;
    b.freq_hz = 1000.0f;
    b.q = 0.7071f;               // Butterworth
    b.enabled = true;
    CHECK(audio_core::band_magnitude_db(b, 1000.0f) == doctest::Approx(-3.01f).epsilon(0.02));
    CHECK(audio_core::band_magnitude_db(b, 100.0f)  == doctest::Approx(0.0f).epsilon(0.02));
    // A decade above the corner a 2nd-order slope is 40 dB down.
    CHECK(audio_core::band_magnitude_db(b, 10000.0f) < -35.0f);
}

TEST_CASE("band_magnitude_db: shelves settle at their gain") {
    audio_core::EqBand b;
    b.type = audio_core::BandType::HIGH_SHELF;
    b.freq_hz = 4000.0f;
    b.gain_db = 6.0f;
    b.q = 0.7f;
    b.enabled = true;
    CHECK(audio_core::band_magnitude_db(b, 20000.0f) == doctest::Approx(6.0f).epsilon(0.05));
    CHECK(audio_core::band_magnitude_db(b, 100.0f)   == doctest::Approx(0.0f).epsilon(0.05));

    b.type = audio_core::BandType::LOW_SHELF;
    b.freq_hz = 100.0f;
    CHECK(audio_core::band_magnitude_db(b, 20.0f)   == doctest::Approx(6.0f).epsilon(0.1));
    CHECK(audio_core::band_magnitude_db(b, 5000.0f) == doctest::Approx(0.0f).epsilon(0.05));
}

TEST_CASE("eq_magnitude_db: biquads in series add in dB") {
    audio_core::EqBand b[2];
    for (auto& x : b) {
        x.type = audio_core::BandType::PEAK;
        x.freq_hz = 1000.0f;
        x.gain_db = 4.0f;
        x.q = 1.5f;
        x.enabled = true;
    }
    CHECK(audio_core::eq_magnitude_db(b, 2, 1000.0f) == doctest::Approx(8.0f).epsilon(0.01));
    // An LR24 crossover is exactly this: two cascaded Butterworths, so -6 dB at the corner.
    audio_core::EqBand lr[2];
    for (auto& x : lr) {
        x.type = audio_core::BandType::LOW_PASS;
        x.freq_hz = 2500.0f;
        x.q = 0.7071f;
        x.enabled = true;
    }
    CHECK(audio_core::eq_magnitude_db(lr, 2, 2500.0f) == doctest::Approx(-6.02f).epsilon(0.02));
}

// ═══════════════════════════════════════════════════════════════
// Stereo stage: width and mono bass
// ═══════════════════════════════════════════════════════════════

namespace {
/// The input frame that output frame `i` carries, when the same `n`-frame block is fed over and
/// over and the chain delays everything by `lat` frames (the brickwall's look-ahead).
size_t src(size_t i, size_t n, uint32_t lat) { return (i + n - lat % n) % n; }

/// Fill a stereo block with distinct per-channel ramps so mid/side errors cannot cancel.
void fill_lr(float* st, size_t n, float lscale, float rscale) {
    for (size_t i = 0; i < n; i++) {
        st[2 * i]     = lscale * (0.01f * (float)(i % 37) - 0.18f);
        st[2 * i + 1] = rscale * (0.01f * (float)(i % 23) - 0.11f);
    }
}
} // namespace

TEST_CASE("width 1.0 is bit-exact passthrough, not merely close") {
    audio_core::DspChain d;
    d.set_eq_enabled(false);
    d.set_volume_percent(100);
    d.set_width(1.0f);

    constexpr size_t N = 64;
    float a[2 * N], b[2 * N];
    fill_lr(a, N, 1.0f, 1.0f);
    memcpy(b, a, sizeof(a));

    audio_core::DspChain ref;
    ref.set_eq_enabled(false);
    ref.set_volume_percent(100);
    // ref keeps the default width, so both take the same path; the point is that neither
    // ran the mid/side maths at all.
    d.process(a, N);
    ref.process(b, N);
    for (size_t i = 0; i < 2 * N; i++) CHECK(a[i] == b[i]);
}

TEST_CASE("width 0 collapses to mono, and both channels carry (L+R)/2") {
    audio_core::DspChain d;
    d.set_eq_enabled(false);
    d.set_volume_percent(100);
    d.set_mute(false);
    d.set_width(0.0f);

    constexpr size_t N = 32;
    float st[2 * N], orig[2 * N];
    fill_lr(st, N, 1.0f, 0.4f);
    memcpy(orig, st, sizeof(st));

    // Run the ramp long enough that the volume ramp has settled before comparing.
    for (int k = 0; k < 300; k++) {
        memcpy(st, orig, sizeof(st));
        d.process(st, N);
    }
    for (size_t i = 0; i < N; i++) {
        CHECK(st[2 * i] == doctest::Approx(st[2 * i + 1]).epsilon(1e-6));
        const size_t j = src(i, N, d.latency_frames());
        const float mid = (orig[2 * j] + orig[2 * j + 1]) * 0.5f;
        CHECK(st[2 * i] == doctest::Approx(mid).epsilon(1e-3));
    }
}

TEST_CASE("width preserves the mono sum at every setting") {
    // The mid channel is untouched by the side scale, so L+R must not move — this is what
    // makes widening safe for anything that later sums to mono.
    constexpr size_t N = 32;
    float base[2 * N];
    fill_lr(base, N, 1.0f, 0.55f);

    // At 80 % (−12 dB): under the volume ceiling that a width above 1 asks for (20·log10 w dB
    // of headroom), so the only gain left on the mono sum is the volume itself.
    const float vol = std::pow(10.0f, -12.0f / 20.0f);
    for (float w : {0.0f, 0.5f, 1.0f, 1.8f}) {
        audio_core::DspChain d;
        d.set_eq_enabled(false);
        d.set_volume_percent(80);
        d.set_width(w);
        float st[2 * N];
        for (int k = 0; k < 300; k++) { memcpy(st, base, sizeof(base)); d.process(st, N); }
        for (size_t i = 0; i < N; i++) {
            const size_t j = src(i, N, d.latency_frames());
            const float want = (base[2 * j] + base[2 * j + 1]) * vol;
            CHECK((st[2 * i] + st[2 * i + 1]) == doctest::Approx(want).epsilon(2e-3));
        }
    }
}

TEST_CASE("mono bass: a pure out-of-phase low tone is removed, in-phase is kept") {
    constexpr size_t N = 240;
    const float f = 40.0f;             // well below the 120 Hz corner
    audio_core::DspChain d;
    d.set_eq_enabled(false);
    d.set_volume_percent(100);
    d.set_width(1.0f);
    d.set_mono_bass(120.0f);

    // Out of phase: pure side signal, nothing in mid.
    float st[2 * N];
    double ph = 0.0;
    float peak = 0.0f;
    for (int blk = 0; blk < 60; blk++) {
        for (size_t i = 0; i < N; i++) {
            const float v = 0.5f * sinf((float)ph);
            st[2 * i] = v; st[2 * i + 1] = -v;
            ph += 2.0 * M_PI * f / 48000.0;
        }
        d.process(st, N);
        if (blk >= 50) for (size_t i = 0; i < N; i++) {
            const float sd = (st[2 * i] - st[2 * i + 1]) * 0.5f;
            if (fabsf(sd) > peak) peak = fabsf(sd);
        }
    }
    // Pinned to the analytic 2nd-order Butterworth response rather than to a hand-picked
    // bound: |H| = (f/fc)^2 / sqrt(1 + (f/fc)^4). At f/fc = 1/3 that is 1/sqrt(82),
    // i.e. -19.1 dB — which is simply 12 dB/oct over the 1.585 octaves from 120 Hz to 40 Hz.
    // The tolerance covers the bilinear transform's frequency warping, nothing more.
    const float want = 0.5f / sqrtf(82.0f);
    CHECK(peak == doctest::Approx(want).epsilon(0.03));

    // In phase at the same frequency: pure mid, must survive untouched.
    audio_core::DspChain d2;
    d2.set_eq_enabled(false);
    d2.set_volume_percent(100);
    d2.set_mono_bass(120.0f);
    ph = 0.0;
    float mpeak = 0.0f;
    for (int blk = 0; blk < 60; blk++) {
        for (size_t i = 0; i < N; i++) {
            const float v = 0.5f * sinf((float)ph);
            st[2 * i] = v; st[2 * i + 1] = v;
            ph += 2.0 * M_PI * f / 48000.0;
        }
        d2.process(st, N);
        if (blk >= 50) for (size_t i = 0; i < N; i++) {
            const float m = (st[2 * i] + st[2 * i + 1]) * 0.5f;
            if (fabsf(m) > mpeak) mpeak = fabsf(m);
        }
    }
    CHECK(mpeak == doctest::Approx(0.5f).epsilon(0.02));
}

TEST_CASE("set_mono_bass rejects frequencies outside bass management") {
    audio_core::DspChain d;
    d.set_eq_enabled(false);
    d.set_volume_percent(100);
    d.set_mono_bass(5000.0f);          // not bass management — must be ignored
    constexpr size_t N = 16;
    float st[2 * N], orig[2 * N];
    fill_lr(st, N, 1.0f, 0.3f);
    memcpy(orig, st, sizeof(st));
    for (int k = 0; k < 300; k++) { memcpy(st, orig, sizeof(orig)); d.process(st, N); }
    for (size_t i = 0; i < N; i++) {
        const size_t j = src(i, N, d.latency_frames());
        CHECK(st[2 * i]     == doctest::Approx(orig[2 * j]).epsilon(2e-3));
        CHECK(st[2 * i + 1] == doctest::Approx(orig[2 * j + 1]).epsilon(2e-3));
    }
}

// ═══════════════════════════════════════════════════════════════
// EQ edits while playing: the filter state is kept and the design is crossfaded
// ═══════════════════════════════════════════════════════════════

namespace {

/// Largest sample-to-sample step in a block, continuing from `prev` (the sample before it).
float max_step(const float* stereo, size_t frames, float& prev) {
    float worst = 0.0f;
    for (size_t i = 0; i < frames; i++) {
        const float d = std::fabs(stereo[2 * i] - prev);
        if (d > worst) worst = d;
        prev = stereo[2 * i];
    }
    return worst;
}

/// A chain playing a 1 kHz tone at `amp` through one +6 dB peaking band, settled. The phase
/// starts at a quarter cycle so every block boundary lands on the PEAK of the tone — the
/// place where a cleared delay line steps furthest.
struct Playing {
    DspChain chain;
    EqBand   band;
    float    phase = 0.25f;
    float    prev  = 0.0f;
    float    blk[DspChain::BLOCK * 2];
    static constexpr float AMP = 0.25f, HZ = 1000.0f;
    // Played at 80 % (−12 dB) with the input 12 dB up: AMP is the level after the volume, and
    // the band's boost stays under the volume ceiling it asks for, so what changes in these
    // tests is the band and nothing else.
    static constexpr float VOL_COMP = 3.98107171f;   // 10^(12/20)

    Playing() {
        chain.reset();
        band.type = BandType::PEAK; band.freq_hz = HZ; band.gain_db = 6.0f; band.q = 1.0f;
        band.enabled = true;
        chain.set_bands(&band, 1);
        chain.set_eq_enabled(true);
        chain.set_volume_percent(80);
        for (int n = 0; n < 80; n++) step();
    }
    float step() {
        fill_sine(blk, DspChain::BLOCK, AMP * VOL_COMP, HZ, phase);
        chain.process(blk, DspChain::BLOCK);
        return max_step(blk, DspChain::BLOCK, prev);
    }
    /// The steepest slope the OUTPUT tone can have: its own amplitude times 2*pi*f/fs.
    static float slope_bound(float gain_db) {
        const float out_amp = AMP * std::pow(10.0f, gain_db / 20.0f);
        return out_amp * 2.0f * (float)M_PI * HZ / FS_HZ_DEFAULT;
    }
};

} // namespace

TEST_CASE("EQ edit: a coefficient change on a steady tone steps no further than the tone itself") {
    Playing p;
    // Sanity: at +6 dB the tone is 0.5 peak and the steady-state step stays under the slope.
    CHECK(p.step() <= Playing::slope_bound(6.0f) * 1.01f);

    // A slider move: +6 -> +5 dB. Both designs are audible; what must not be is the change.
    p.band.gain_db = 5.0f;
    p.chain.set_bands(&p.band, 1);
    float worst = 0.0f;
    for (int n = 0; n < 6; n++) worst = std::max(worst, p.step());
    // The bound is the larger of the two tones' slopes, plus the crossfade's own ramp: the
    // whole gain change, spread over one block.
    const float ramp = (0.25f * (std::pow(10.0f, 6.0f / 20.0f) - std::pow(10.0f, 5.0f / 20.0f)))
                       / (float)DspChain::BLOCK;
    CHECK(worst <= Playing::slope_bound(6.0f) + ramp * 1.5f);

    // And it did change: the tone settles at the new gain.
    float peak = 0.0f;
    for (int n = 0; n < 60; n++) { p.step(); if (n >= 40) peak = std::max(peak, peak_abs(p.blk, DspChain::BLOCK)); }
    CHECK(20.0 * std::log10(peak / Playing::AMP) == doctest::Approx(5.0).epsilon(0.03));
}

TEST_CASE("EQ edit: switching a band off, or the whole EQ off and on, is crossfaded") {
    SUBCASE("band disabled") {
        Playing p;
        p.band.enabled = false;
        p.chain.set_bands(&p.band, 1);
        float worst = 0.0f;
        for (int n = 0; n < 6; n++) worst = std::max(worst, p.step());
        const float ramp = (0.25f * (std::pow(10.0f, 6.0f / 20.0f) - 1.0f)) / (float)DspChain::BLOCK;
        CHECK(worst <= Playing::slope_bound(6.0f) + ramp * 1.5f);
        float peak = 0.0f;
        for (int n = 0; n < 40; n++) { p.step(); if (n >= 20) peak = std::max(peak, peak_abs(p.blk, DspChain::BLOCK)); }
        CHECK(peak == doctest::Approx(Playing::AMP).epsilon(0.02));   // a wire now
    }
    SUBCASE("EQ bypassed, then back") {
        Playing p;
        const float ramp = (0.25f * (std::pow(10.0f, 6.0f / 20.0f) - 1.0f)) / (float)DspChain::BLOCK;
        p.chain.set_eq_enabled(false);
        float worst = 0.0f;
        for (int n = 0; n < 6; n++) worst = std::max(worst, p.step());
        CHECK(worst <= Playing::slope_bound(6.0f) + ramp * 1.5f);
        for (int n = 0; n < 40; n++) p.step();
        p.chain.set_eq_enabled(true);
        worst = 0.0f;
        for (int n = 0; n < 6; n++) worst = std::max(worst, p.step());
        CHECK(worst <= Playing::slope_bound(6.0f) + ramp * 1.5f);
        float peak = 0.0f;
        for (int n = 0; n < 60; n++) { p.step(); if (n >= 40) peak = std::max(peak, peak_abs(p.blk, DspChain::BLOCK)); }
        CHECK(20.0 * std::log10(peak / Playing::AMP) == doctest::Approx(6.0).epsilon(0.03));
    }
}

TEST_CASE("EQ edit: the same design again is not a change at all") {
    Playing p;
    // Re-publishing identical bands — which the UI does — must be bit-exact with doing nothing.
    Playing q;
    q.chain.set_bands(&q.band, 1);
    for (int n = 0; n < 4; n++) {
        p.step(); q.step();
        for (size_t i = 0; i < 2 * DspChain::BLOCK; i++) CHECK(p.blk[i] == q.blk[i]);
    }
}

TEST_CASE("chain: a step far over the threshold never leaves above it, not even its first sample") {
    // The one-pole limiter this replaced let the first millisecond of a transient through, into
    // a soft clip. The brickwall has seen it coming for (M+1)·D frames.
    DspChain chain;
    chain.reset();
    chain.set_volume_percent(100);
    float blk[DspChain::BLOCK * 2];
    for (int n = 0; n < 60; n++) {
        memset(blk, 0, sizeof(blk));
        chain.process(blk, DspChain::BLOCK);
    }
    float worst = 0.0f;
    for (int n = 0; n < 20; n++) {
        for (size_t i = 0; i < DspChain::BLOCK; i++) blk[2 * i] = blk[2 * i + 1] = 1.05f;
        chain.process(blk, DspChain::BLOCK);
        worst = std::max(worst, peak_abs(blk, DspChain::BLOCK));
    }
    CHECK(worst <= CEILING * (1.0f + 1e-6f));
    CHECK(worst == doctest::Approx(CEILING).epsilon(1e-3));   // and it is used, not wasted
}

TEST_CASE("chain: the limiter's release is a time, not a sample count") {
    // Release: samples until the gain is back above 0.9 after a burst, at 48 and at 96 kHz.
    auto release_samples = [](float fs) {
        DspChain c;
        c.reset();
        c.set_sample_rate(fs);
        c.set_volume_percent(100);
        float blk[DspChain::BLOCK * 2];
        float ph = 0.0f;
        for (int n = 0; n < 60; n++) { fill_sine(blk, DspChain::BLOCK, 0.1f, 440.0f, ph); c.process(blk, DspChain::BLOCK); }
        for (int n = 0; n < 40; n++) { fill_sine(blk, DspChain::BLOCK, 3.0f, 440.0f, ph); c.process(blk, DspChain::BLOCK); }
        REQUIRE(c.limiter_gain() < 0.4f);
        int samples = 0;
        while (c.limiter_gain() < 0.9f && samples < (int)(10 * fs)) {
            fill_sine(blk, DspChain::BLOCK, 0.1f, 440.0f, ph);
            c.process(blk, DspChain::BLOCK);
            samples += (int)DspChain::BLOCK;
        }
        return samples;
    };
    const int r48 = release_samples(48000.0f);
    const int r96 = release_samples(96000.0f);
    // 0.7 of the way back at τ = 250 ms takes 1.95 τ ≈ 0.49 s: 23 k samples at 48 kHz.
    CHECK(r48 > 20000);
    CHECK(r48 < 27000);
    CHECK((float)r96 / (float)r48 == doctest::Approx(2.0f).epsilon(0.06));
    // The volume ramp's timing is test_dsp_guard.cpp's.
}

TEST_CASE("chain: the section trim is heard ahead of the EQ, and 0 dB is a wire") {
    DspChain chain;
    chain.reset();
    chain.set_volume_percent(100);
    chain.set_pre_gain_db(-6.0206f);
    float blk[DspChain::BLOCK * 2];
    float phase = 0.0f, peak = 0.0f;
    for (int n = 0; n < 60; n++) {
        fill_sine(blk, DspChain::BLOCK, 0.25f, 1000.0f, phase);
        chain.process(blk, DspChain::BLOCK);
        if (n >= 40) peak = std::max(peak, peak_abs(blk, DspChain::BLOCK));
    }
    CHECK(peak == doctest::Approx(0.125f).epsilon(0.01));

    // 0 dB must not merely be close to doing nothing: the multiply is skipped entirely.
    DspChain a, b;
    a.reset(); b.reset();
    a.set_volume_percent(100); b.set_volume_percent(100);
    b.set_pre_gain_db(0.0f);
    float pa = 0.0f, pb = 0.0f;
    float ba[DspChain::BLOCK * 2], bb[DspChain::BLOCK * 2];
    for (int n = 0; n < 8; n++) {
        fill_sine(ba, DspChain::BLOCK, 0.4f, 1234.0f, pa);
        fill_sine(bb, DspChain::BLOCK, 0.4f, 1234.0f, pb);
        a.process(ba, DspChain::BLOCK);
        b.process(bb, DspChain::BLOCK);
        for (size_t i = 0; i < 2 * DspChain::BLOCK; i++) CHECK(ba[i] == bb[i]);
    }
}
