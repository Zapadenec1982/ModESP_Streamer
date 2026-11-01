/**
 * @file test_dsp_passes.cpp
 * @brief HOST tests for the DSP speed-ups (components/audio_dsp):
 *        none of them changes a bit of the output. The
 *        filter sections run two to a pass (DspChain::Passes, svf_stereo2) give what one section
 *        a pass gives, through every kind of change; in the steady state they really make half
 *        the passes; and the whole chain still hashes to what it gave before.
 */

#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "doctest.h"
#include "dsp_chain.h"
#include "dsp_golden_scenario.h"

using namespace audio_core;

namespace {

/// Two chains with the same settings: `a` runs its sections two to a pass, as the firmware
/// does, `b` one to a pass. Everything is done to both, in the same order.
struct Twin {
    DspChain a, b;
    std::vector<float> la, lb;
    explicit Twin(float fs) : la(DspChain::BASS_LINE_FRAMES * 2), lb(DspChain::BASS_LINE_FRAMES * 2) {
        b.set_paired_passes(false);
        a.set_sample_rate(fs);
        b.set_sample_rate(fs);
        a.set_bass_line(la.data(), DspChain::BASS_LINE_FRAMES);
        b.set_bass_line(lb.data(), DspChain::BASS_LINE_FRAMES);
        dsp_golden::panel_settings(a);
        dsp_golden::panel_settings(b);
    }
};

}  // namespace

TEST_CASE("one pass: two sections a pass give the chain of one section a pass, bit for bit") {
    for (float fs : {192000.0f, 48000.0f}) {
        CAPTURE(fs);
        auto t = std::make_unique<Twin>(fs);
        auto eq_a = dsp_golden::panel_eq(), eq_b = eq_a;
        uint32_t seed = 2026;
        size_t t0 = 0, mismatched = 0;
        float x[DspChain::BLOCK * 2], y[DspChain::BLOCK * 2];
        for (int k = 0; k < dsp_golden::BLOCKS; k++) {
            dsp_golden::events(t->a, k, eq_a);
            dsp_golden::events(t->b, k, eq_b);
            const size_t n = dsp_golden::block_len(k);
            dsp_golden::input(x, n, t0, fs, seed, k);
            memcpy(y, x, sizeof(float) * 2 * n);
            t->a.process(x, n);
            t->b.process(y, n);
            if (memcmp(x, y, sizeof(float) * 2 * n) != 0) mismatched++;
            t0 += n;
        }
        CHECK(mismatched == 0);
        CHECK(t->a.faults() == 0);
        CHECK(t->a.peak_l() == t->b.peak_l());
        CHECK(t->a.limiter_gain() == t->b.limiter_gain());
    }
}

TEST_CASE("one pass: the panel's settings take half the passes, and an odd section runs alone") {
    // 3 subsonic (with the port lift) + 2 loudness + 2 tone + 10 bands = 17 sections: 9 passes
    // paired, 17 one at a time. Without the port lift, 16 sections: 8 and 16.
    for (float fs : {192000.0f, 48000.0f}) {
        CAPTURE(fs);
        auto t = std::make_unique<Twin>(fs);
        float x[DspChain::BLOCK * 2], y[DspChain::BLOCK * 2];
        uint32_t seed = 1;
        size_t t0 = 0;
        auto settle = [&]() {
            // Long enough for every fade (the subsonic's are four EQ fades) and the loudness glide.
            for (int k = 0; k < (int)(0.5f * fs / DspChain::BLOCK); k++) {
                dsp_golden::fill(x, DspChain::BLOCK, t0, fs, seed, 0.5f);
                memcpy(y, x, sizeof(x));
                t->a.process(x, DspChain::BLOCK);
                t->b.process(y, DspChain::BLOCK);
                t0 += DspChain::BLOCK;
            }
        };
        settle();
        CHECK(t->a.filter_passes() == 9u);
        CHECK(t->b.filter_passes() == 17u);
        t->a.set_port(0.0f, 0.0f);
        t->b.set_port(0.0f, 0.0f);
        settle();
        CHECK(t->a.filter_passes() == 8u);
        CHECK(t->b.filter_passes() == 16u);
    }
}

TEST_CASE("one pass: the whole chain still gives, bit for bit, what it gave before the speed-ups") {
    // dsp_golden_scenario.h: the panel's settings, every kind of change, 900 blocks, every
    // output sample's bits hashed. The values are the chain's before any of the speed-ups,
    // built with g++ and glibc (-O0 and -O2 alike). A speed-up that moves one bit anywhere
    // changes them; a deliberate change to the sound has to change them here, with its reason.
    // Another C library rounds sinf, expf, powf in the designs its own way, so elsewhere the
    // hashes are only printed.
    const struct { float fs; uint64_t want; } runs[] = {
        {192000.0f, 0x7903ebca4168c82aull}, {96000.0f, 0x4e5106105010ddd6ull},
        {48000.0f, 0x9776851a312ecb3aull},  {44100.0f, 0xc3be15339476ea87ull},
    };
    for (const auto& r : runs) {
        const uint64_t h = dsp_golden::run(r.fs);
        CAPTURE(r.fs);
#if defined(__GLIBC__)
        CHECK(h == r.want);
#else
        MESSAGE("chain hash at " << r.fs << " Hz: " << std::hex << h);
#endif
    }
}
