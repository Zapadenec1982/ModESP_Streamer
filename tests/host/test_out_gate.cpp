/**
 * @file test_out_gate.cpp
 * @brief HOST tests for the edges of the output:
 *        the FadeGate curve, a pause and a source change through the real DspChain, and a ring
 *        claimed from a producer whose music is still playing.
 *
 * The click is measured as test_dsp_fade.cpp measures it: an 8th-order Butterworth high-pass at
 * 500 Hz over the output, which leaves a steady 62 Hz tone near -150 dBFS. Whatever the edge puts
 * above the note comes through, and its peak is the click. The audio task's sequence is copied
 * here block for block: the envelope in front of the chain, the chain, the output gate after
 * it, and a reset once the input is silent and the output gate is shut.
 */

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "doctest.h"
#include "dsp_chain.h"
#include "fade_gate.h"
#include "pcm_ring.h"

using namespace audio_core;

namespace {

struct Hp8 {
    struct Sec { double b0, b1, b2, a1, a2, x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    Sec s[4];
    Hp8(double fc, double fs) {
        const double qs[4] = {0.50979558, 0.60134489, 0.89997622, 2.56291545};
        for (int i = 0; i < 4; i++) {
            const double w0 = 2 * M_PI * fc / fs, cw = std::cos(w0), al = std::sin(w0) / (2 * qs[i]);
            const double a0 = 1 + al;
            s[i] = {(1 + cw) / 2 / a0, -(1 + cw) / a0, (1 + cw) / 2 / a0, -2 * cw / a0, (1 - al) / a0};
        }
    }
    double tick(double x) {
        for (Sec& q : s) {
            const double y = q.b0 * x + q.b1 * q.x1 + q.b2 * q.x2 - q.a1 * q.y1 - q.a2 * q.y2;
            q.x2 = q.x1; q.x1 = x; q.y2 = q.y1; q.y1 = y;
            x = y;
        }
        return x;
    }
};

double db(double x) { return 20.0 * std::log10(std::fabs(x) + 1e-30); }

/// The customer's panel's settings: port 62 Hz +4 dB, loudness 90 %, subsonic 50 Hz,
/// bass limiter at 120 Hz, volume 30 %. And a second case: one 62 Hz band, +6 dB at Q 5.
struct Setup {
    const char* name;
    bool customer;
};

std::vector<float> g_bass_line(DspChain::BASS_LINE_FRAMES * 2);

void configure(DspChain& ch, float fs, bool customer) {
    ch.set_sample_rate(fs);
    if (customer) {
        ch.set_bass_line(g_bass_line.data(), DspChain::BASS_LINE_FRAMES);
        ch.set_bass_xo(120.0f);
    }
    ch.reset();
    if (customer) {
        ch.set_volume_percent(30);
        ch.set_subsonic(50.0f);
        ch.set_loudness(0.9f);
        ch.set_port(62.0f, 4.0f);
    } else {
        ch.set_volume_percent(100);
        EqBand b;
        b.type = BandType::PEAK; b.freq_hz = 62.0f; b.gain_db = 6.0f; b.q = 5.0f; b.enabled = true;
        ch.set_bands(&b, 1);
    }
}

/// The audio task's last stage, for one tone and one edge.
struct Path {
    double fs;
    DspChain chain;
    FadeGate env, out;          // the envelope in front of the chain, the gate after it
    bool dirty = false;
    double phase, hz;
    float blk[DspChain::BLOCK * 2];
    Hp8 hp;                     // runs from the first block, so it has no start-up of its own
    std::vector<float> y;       // left channel of everything played since the warm-up
    std::vector<double> above;  // and what the high-pass leaves of it
    size_t edge = 0;            // where the edge starts, in frames
    long reset_at = -1;         // the first frame after the block the chain was reset in

    Path(double rate, bool customer, double tone, double phase0)
        : fs(rate), phase(phase0), hz(tone), hp(500.0, rate) {
        configure(chain, (float)rate, customer);
        env.set_rate((float)rate);
        out.set_rate((float)rate);
        // Open both gates and play long enough for every glide and fade to have settled.
        for (int k = 0; k < (int)(rate / DspChain::BLOCK); k++) block(true, true);
        y.clear();
        above.clear();
        reset_at = -1;
    }
    /// One block: `signal` is whether the source delivers, `open` where the gates head.
    void block(bool signal, bool open) {
        const size_t n = DspChain::BLOCK;
        for (size_t i = 0; i < n; i++) {
            const float v = signal ? (float)(0.25 * std::sin(phase)) : 0.0f;   // -12 dBFS
            phase += 2.0 * M_PI * hz / fs;
            blk[2 * i] = v;
            blk[2 * i + 1] = -v;                                               // anti-phase
        }
        env.run(blk, n, open);
        const bool silent = env.shut();
        if (!silent || !out.shut() || dirty) {
            chain.process(blk, n);
            dirty = true;
        }
        out.run(blk, n, open);
        bool reset = false;
        if (silent && out.shut() && dirty) {
            chain.reset();
            dirty = false;
            reset = true;
        }
        for (size_t i = 0; i < n; i++) {
            y.push_back(blk[2 * i]);
            above.push_back(hp.tick(blk[2 * i]));
        }
        if (reset && reset_at < 0) reset_at = (long)y.size();   // the first frame after it
    }
    /// The loudest thing above 500 Hz from the edge on.
    double click_db() const {
        double worst = 0.0;
        for (size_t i = edge; i < above.size(); i++) worst = std::max(worst, std::fabs(above[i]));
        return db(worst);
    }
};

}  // namespace

TEST_CASE("fade gate: opens in 20 ms and closes in 100 ms at every rate, along smootherstep") {
    for (float fs : {44100.0f, 48000.0f, 96000.0f, 192000.0f}) {
        FadeGate g;
        g.set_rate(fs);
        CHECK(g.shut());
        size_t frames = 0;
        float last = 0.0f;
        bool monotonic = true;
        while (!g.wide_open() && frames < 20000) {
            float one[2] = {1.0f, 1.0f};
            g.run(one, 1, true);
            // Within an ulp: near the top the curve flattens below float's resolution at 1.0.
            monotonic &= one[0] >= last - 1e-6f && one[0] <= 1.0f;
            last = one[0];
            frames++;
        }
        CHECK(monotonic);
        CHECK(std::fabs((double)frames - FadeGate::OPEN_S * fs) <= 2.0);
        // Resting open, a wire: bit-exact.
        float x[4] = {0.123456789f, -0.987654321f, 1e-30f, -1.0f};
        const float keep[4] = {x[0], x[1], x[2], x[3]};
        g.run(x, 2, true);
        for (int i = 0; i < 4; i++) CHECK(x[i] == keep[i]);
        // Closing takes five times as long, and resting shut zeroes.
        frames = 0;
        while (!g.shut() && frames < 40000) {
            float one[2] = {1.0f, 1.0f};
            g.run(one, 1, false);
            frames++;
        }
        CHECK(std::fabs((double)frames - FadeGate::CLOSE_S * fs) <= 2.0);
        float z[4] = {0.5f, 0.5f, -0.5f, -0.5f};
        g.run(z, 2, false);
        for (float v : z) CHECK(v == 0.0f);
    }
    CHECK(smootherstep(0.0f) == 0.0f);
    CHECK(smootherstep(1.0f) == 1.0f);
    CHECK(smootherstep(0.5f) == doctest::Approx(0.5f));
}

TEST_CASE("a pause through the chain is silent, and the reset lands in silence") {
    // Before the output gate the chain was reset once the limiters' look-ahead had played out,
    // while the filters still rang: -19.7 dBFS above 500 Hz on this tone with the single band,
    // -36.8 with the customer's settings. Both are measured by the probe in the commit message.
    for (const Setup su : {Setup{"review band 62 Hz +6 dB Q5", false}, Setup{"customer", true}}) {
        for (double fs : {48000.0, 192000.0}) {
            double worst = -300.0;
            for (int p = 0; p < 8; p++) {
                Path path(fs, su.customer, 62.0, 2.0 * M_PI * p / 8.0);
                for (int k = 0; k < 4; k++) path.block(true, true);
                path.edge = path.y.size();
                for (int k = 0; k < (int)(0.25 * fs / DspChain::BLOCK); k++) path.block(true, false);
                REQUIRE(path.reset_at >= 0);
                // The block the reset came in ends in exact silence, and nothing follows it: the
                // chain was cleared behind a shut gate.
                REQUIRE(path.y[(size_t)path.reset_at - 1] == 0.0f);
                for (size_t i = (size_t)path.reset_at; i < path.y.size(); i++) REQUIRE(path.y[i] == 0.0f);
                // And it happened within a block or two of the fade, not after a tail count.
                CHECK((double)(path.reset_at - (long)path.edge) <= FadeGate::CLOSE_S * fs + 2.0 * DspChain::BLOCK);
                worst = std::max(worst, path.click_db());
            }
            INFO(std::string(su.name) << " at " << fs << " Hz: " << worst << " dBFS above 500 Hz");
            CHECK(worst < -80.0);
        }
    }
}

TEST_CASE("a source change closes over the old source while it still plays") {
    // apply_source() used to reset the chain the moment the command came: the next block was
    // silence after a block of music, -13.5 dBFS above 500 Hz with the single band. Now the old
    // source keeps delivering while both gates close, and only then does the switch happen.
    for (double fs : {48000.0, 192000.0}) {
        double worst = -300.0;
        for (int p = 0; p < 8; p++) {
            Path path(fs, false, 62.0, 2.0 * M_PI * p / 8.0);
            path.edge = path.y.size();
            int blocks = 0;
            while (!(path.env.shut() && path.out.shut()) && blocks < 200) {
                path.block(true, false);        // the old source plays on, the gates close
                blocks++;
            }
            REQUIRE(blocks < 200);
            // The switch: a new source starts from a reset chain behind shut gates.
            path.chain.reset();
            path.dirty = false;
            for (int k = 0; k < (int)(0.05 * fs / DspChain::BLOCK); k++) path.block(false, false);
            worst = std::max(worst, path.click_db());
        }
        INFO("source change at " << fs << " Hz: " << worst << " dBFS above 500 Hz");
        CHECK(worst < -80.0);
    }
}

TEST_CASE("a change withdrawn half way turns round without a reset") {
    // Asking again for the source that is playing withdraws a change still waiting: the gates turn
    // round wherever they are, and the chain is never reset. The turn itself flips the curve's
    // slope, which is the one corner left: about -76 dBFS above 500 Hz here, 40 dB under this
    // tone. The hard switch it replaces was -13.
    for (double fs : {48000.0, 192000.0}) {
        Path path(fs, true, 62.0, 0.7);
        path.edge = path.y.size();
        for (int k = 0; k < (int)(0.01 * fs / DspChain::BLOCK); k++) path.block(true, false);
        for (int k = 0; k < (int)(0.1 * fs / DspChain::BLOCK); k++) path.block(true, true);
        CHECK(path.reset_at < 0);
        CHECK(path.click_db() < -70.0);
    }
}

TEST_CASE("claim_after_fade: a ring that is being heard is faded before it changes hands") {
    PcmRing r;
    REQUIRE(r.init(1024));
    struct Free { PcmRing& r; ~Free() { r.deinit(); } } free_ring{r};
    r.claim(PcmRing::OWNER_FILE);
    float pcm[2 * 256] = {};
    REQUIRE(r.write(PcmRing::OWNER_FILE, pcm, 256) == 256);

    SUBCASE("nobody listening: claimed at once") {
        r.set_consumer_live(false);
        int naps = 0;
        CHECK(claim_after_fade(r, PcmRing::OWNER_RADIO, [&] { naps++; }));
        CHECK(naps == 0);
        CHECK(r.owner() == PcmRing::OWNER_RADIO);
        CHECK(r.available() == 0);
    }
    SUBCASE("heard: the consumer fades and drops the queue, then the claim goes ahead") {
        r.set_consumer_live(true);
        const uint32_t g = r.generation();
        int naps = 0;
        bool asked = false;
        CHECK(claim_after_fade(r, PcmRing::OWNER_RADIO, [&] {
            // What the audio task does once its envelope has shut on a pending flush.
            asked = r.flush_pending();
            r.reset(g);
            naps++;
        }));
        CHECK(asked);
        CHECK(naps == 1);
        CHECK(r.owner() == PcmRing::OWNER_RADIO);
        CHECK(!r.flush_pending());
    }
    SUBCASE("heard, but nobody answers: FLUSH_WAIT_NAPS naps and then the old claim") {
        r.set_consumer_live(true);
        int naps = 0;
        CHECK(claim_after_fade(r, PcmRing::OWNER_RADIO, [&] { naps++; }));
        CHECK(naps == FLUSH_WAIT_NAPS);
        // Long enough for the consumer's 100 ms close, with room to spare.
        CHECK(FLUSH_WAIT_NAPS * 5 >= 2 * (int)(FadeGate::CLOSE_S * 1000.0f));
        CHECK(r.owner() == PcmRing::OWNER_RADIO);
        CHECK(r.available() == 0);
    }
    SUBCASE("the same owner: no wait, and the queue stays") {
        r.set_consumer_live(true);
        int naps = 0;
        CHECK(claim_after_fade(r, PcmRing::OWNER_FILE, [&] { naps++; }));
        CHECK(naps == 0);
        CHECK(r.available() == 256);
    }
}
