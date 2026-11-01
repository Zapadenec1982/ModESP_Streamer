/**
 * @file dsp_golden_scenario.h
 * @brief One fixed run of the whole DspChain — the panel's settings and every kind of change —
 *        reduced to a 64-bit FNV-1a hash of every output sample's bits. test_dsp_passes.cpp holds
 *        it to the value the chain gave before the one-pass speed-ups, so a change that
 *        moves a single bit anywhere in the chain is seen. Public API only: the same file runs
 *        against an older chain unchanged.
 */

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "dsp_chain.h"

namespace dsp_golden {

inline uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

inline audio_core::EqBand band(audio_core::BandType t, float f, float g, float q) {
    audio_core::EqBand b;
    b.type = t;
    b.freq_hz = f;
    b.gain_db = g;
    b.q = q;
    b.enabled = true;
    return b;
}

/// The panel's EQ (GET /api/eq, «Власний»): all ten bands on.
inline std::vector<audio_core::EqBand> panel_eq() {
    using audio_core::BandType;
    return {
        band(BandType::LOW_SHELF, 46, 7.2f, 0.707f), band(BandType::PEAK, 62.5f, 3.6f, 1.0f),
        band(BandType::PEAK, 125, 1.3f, 1.0f),       band(BandType::PEAK, 250, -0.8f, 1.0f),
        band(BandType::PEAK, 448, -2.1f, 1.0f),      band(BandType::PEAK, 1000, -1.8f, 1.0f),
        band(BandType::PEAK, 2000, 0.7f, 1.0f),      band(BandType::PEAK, 4000, 2.5f, 1.0f),
        band(BandType::PEAK, 7950, 4.2f, 1.0f),      band(BandType::HIGH_SHELF, 11200, 5.3f, 0.707f),
    };
}

/// What the panel had set when the baseline was measured (WS snapshot): subsonic 40 Hz,
/// loudness 60 %, drive 5 dB, mono bass 80 Hz, the bass limiter at 250 Hz, the EQ on — plus the
/// port lift (62 Hz +4 dB; the panel had it off with its speaker type) for an odd number of
/// subsonic sections, and Бас/Високі +6, at volume 4 % so the loudness runs.
inline void panel_settings(audio_core::DspChain& c) {
    const auto eq = panel_eq();
    c.set_bands(eq.data(), (int)eq.size());
    c.set_subsonic(40.0f);
    c.set_port(62.0f, 4.0f);
    c.set_loudness(0.6f);
    c.set_drive_db(5.0f);
    c.set_mono_bass(80.0f);
    c.set_bass_xo(250.0f);
    c.set_tone(6.0f, 6.0f);
    c.set_volume(4.0f);
    c.reset();
}

/// Music-like input: a bass note, a mid tone and noise, different on each channel. `bright`
/// moves the weight to 3 kHz, where the bass limiter leaves it to the brickwall.
inline void fill(float* x, size_t n, size_t t0, double fs, uint32_t& seed, float amp, bool bright = false) {
    const double b = bright ? 0.15 : 0.5, m = bright ? 0.8 : 0.3, fm = bright ? 3000.0 : 1234.5;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)(t0 + i) / fs;
        const float noise = (float)(int32_t)lcg(seed) / 2147483648.0f;
        x[2 * i] = amp * (float)(b * std::sin(2 * M_PI * 55.0 * t) + m * std::sin(2 * M_PI * fm * t)) + 0.05f * amp * noise;
        x[2 * i + 1] = amp * (float)(b * std::sin(2 * M_PI * 55.0 * t + 0.9) + 0.3 * std::sin(2 * M_PI * 987.0 * t)) - 0.05f * amp * noise;
    }
}

/// The input of block `k`: quiet, then loud bass (the bass limiter works), then loud and bright
/// (the brickwall works).
inline void input(float* x, size_t n, size_t t0, double fs, uint32_t& seed, int k) {
    fill(x, n, t0, fs, seed, k > 600 ? 1.5f : 0.5f, k > 820);
}

/// Every kind of change the chain goes through, at block `k`: a band edit and one waiting on
/// its fade, Бас/Високі, a volume step (the loudness glides), the EQ off and on, the port lift
/// off (three subsonic sections to two), the tone back to wires, the loudness down to a wire
/// and back, loud music at 100 % with balance and width (input() makes the limiters work), an
/// odd band count, the subsonic off, the mono bass off (width alone) and back at another corner.
inline void events(audio_core::DspChain& c, int k, std::vector<audio_core::EqBand>& eq) {
    switch (k) {
    case 40:  eq[3].gain_db = 2.0f; c.set_bands(eq.data(), 10); break;
    case 45:  eq[7].freq_hz = 5000.0f; c.set_bands(eq.data(), 10); break;
    case 90:  c.set_tone(-4.0f, 6.0f); break;
    case 140: c.set_volume(12.0f); break;
    case 200: c.set_eq_enabled(false); break;
    case 260: c.set_eq_enabled(true); break;
    case 330: c.set_port(0.0f, 0.0f); break;
    case 420: c.set_tone(0.0f, 0.0f); break;
    case 480: c.set_loudness(0.0f); break;
    case 560: c.set_loudness(0.6f); c.set_tone(3.0f, 0.0f); break;
    case 620: c.set_volume(100.0f); c.set_balance_db(2.0f); c.set_width(1.3f); break;
    case 700: eq[0].enabled = false; c.set_bands(eq.data(), 10); break;
    case 760: c.set_subsonic(0.0f); break;
    case 790: c.set_mono_bass(0.0f); break;     // width alone
    case 850: c.set_mono_bass(120.0f); break;
    default: break;
    }
}

inline size_t block_len(int k) { return (k % 37 == 5) ? 1 : (k % 23 == 7) ? 100 : audio_core::DspChain::BLOCK; }

constexpr int BLOCKS = 900;

/// The run at `fs`, hashed: every output sample's bits, then the meters and the limiters' gains.
inline uint64_t run(float fs) {
    using audio_core::DspChain;
    auto c = std::make_unique<DspChain>();
    std::vector<float> line(DspChain::BASS_LINE_FRAMES * 2);
    c->set_sample_rate(fs);
    c->set_bass_line(line.data(), DspChain::BASS_LINE_FRAMES);
    panel_settings(*c);
    auto eq = panel_eq();
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    };
    uint32_t seed = 2026;
    size_t t0 = 0;
    float x[DspChain::BLOCK * 2];
    for (int k = 0; k < BLOCKS; k++) {
        events(*c, k, eq);
        const size_t n = block_len(k);
        input(x, n, t0, fs, seed, k);
        c->process(x, n);
        mix(x, sizeof(float) * 2 * n);
        const float m[4] = {c->peak_l(), c->peak_r(), c->limiter_gain(), c->loudness_db()};
        mix(m, sizeof(m));
        t0 += n;
    }
    const uint32_t f = c->faults();
    mix(&f, sizeof(f));
    return h;
}

}  // namespace dsp_golden
