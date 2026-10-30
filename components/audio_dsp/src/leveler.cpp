/**
 * @file leveler.cpp
 * @brief «Вирівнювання гучності» (leveler.h): the K-weighted loudness, the gain it decides, the
 *        schedule.
 */

#include "leveler.h"

#include <cmath>

namespace audio_core {

namespace {

/// BS.1770 mean square → LUFS for the mono sum m = (L + R)/2: −0.691 + 10·log10(2·ms), the
/// standard's L + R for a signal that is the same in both channels. Wide, decorrelated content
/// reads 3 dB lower than BS.1770 would make it; a film's dialogue and advertising sit in the middle.
constexpr float LUFS_OF_MS = -0.691f + 3.0103f;

float lufs(float ms) { return LUFS_OF_MS + 10.0f * log10f(ms); }

SvfCoef coef(double g, double k, double m0, double m1, double m2) {
    SvfCoef c;
    const double c1 = 1.0 / (1.0 + g * (g + k)), c2 = g * c1;
    c.c1 = (float)c1;
    c.c2 = (float)c2;
    c.c3 = (float)(g * c2);
    c.m0 = (float)m0;
    c.m1 = (float)m1;
    c.m2 = (float)m2;
    return c;
}

}  // namespace

void Leveler::k_weighting(double fs, SvfCoef& shelf, SvfCoef& hp) {
    // The analogue prototypes behind the standard's 48 kHz biquads, as libebur128 derives them for
    // any rate. Shelf: H(s) = (Vh·s² + Vb·s/Q + 1)/(s² + s/Q + 1), bilinear at f0, which in the
    // SVF's terms (H = m0 + (m1·s + m2)/(s² + k·s + 1)) is m0 = Vh, m1 = k(Vb − Vh), m2 = 1 − Vh.
    // High-pass: s²/(s² + s/Q + 1).
    const double f1 = 1681.974450955533, g1db = 3.999843853973347, q1 = 0.7071752369554196;
    const double vh = pow(10.0, g1db / 20.0), vb = pow(vh, 0.4996667741545416);
    const double k1 = 1.0 / q1;
    shelf = coef(tan(M_PI * f1 / fs), k1, vh, k1 * (vb - vh), 1.0 - vh);
    const double f2 = 38.13547087602444, q2 = 0.5003270373238773;
    const double k2 = 1.0 / q2;
    hp = coef(tan(M_PI * f2 / fs), k2, 1.0, -k2, -1.0);
}

void Leveler::set_sample_rate(float hz) {
    if (!(hz >= 1000.0f)) hz = 48000.0f;
    fs_ = hz;
    k_weighting(hz, kw_shelf_, kw_hp_);
    for (float& v : s_) v = 0.0f;
}

void Leveler::set_on(bool on) {
    if (on && !on_) {
        // Measured from scratch, and no lift until there is something to judge it by.
        for (float& v : s_) v = 0.0f;
        ps_ = pf_ = 0.0f;
        hold_ = LEV_HOLD_S;
    }
    on_ = on;
}

void Leveler::reset() {
    for (float& v : s_) v = 0.0f;
    ps_ = pf_ = 0.0f;
    hold_ = LEV_HOLD_S;
    if (!(fabsf(g_db_) <= LEV_CUT_DB)) g_db_ = 0.0f;
    // The volume fades in from silence after a reset (DspChain::reset): nothing to ramp from.
    lin_ = lin_to_ = g_db_ == 0.0f ? 1.0f : powf(10.0f, g_db_ / 20.0f);
}

bool Leveler::sane() const {
    bool ok = (fabsf(ps_) <= 1e12f) & (fabsf(pf_) <= 1e12f) & (fabsf(g_db_) <= LEV_CUT_DB);
    for (float v : s_) ok &= fabsf(v) <= 1e6f;
    return ok;
}

void Leveler::decide(float ms, float dt) {
    if (!on_) {
        // Off: back to 0 dB at the rise's pace, whichever way that is.
        const float step = LEV_RISE_DB_S * dt;
        g_db_ = g_db_ > step ? g_db_ - step : g_db_ < -step ? g_db_ + step : 0.0f;
        return;
    }
    if (!(lufs(ms) >= LEV_GATE_LUFS)) return;   // a pause (or not a number): nothing to measure
    ps_ += (ms - ps_) * (1.0f - expf(-dt / LEV_SLOW_S));
    pf_ += (ms - pf_) * (1.0f - expf(-dt / LEV_FAST_S));
    // What the slow level asks for, and what the fast one allows over it.
    const float a = LEV_TARGET_LUFS - lufs(ps_);
    const float b = LEV_TARGET_LUFS + LEV_MARGIN_DB - lufs(pf_);
    float want = a < b ? a : b;
    if (!(want >= -LEV_CUT_DB)) want = -LEV_CUT_DB;    // NaN too: down, never up
    if (want > LEV_BOOST_DB) want = LEV_BOOST_DB;
    if (want < g_db_) {
        g_db_ += (want - g_db_) * (1.0f - expf(-dt / LEV_ATTACK_S));
        hold_ = LEV_HOLD_S;
    } else if (hold_ > 0.0f) {
        hold_ -= dt;
    } else {
        const float up = g_db_ + LEV_RISE_DB_S * dt;
        g_db_ = up < want ? up : want;
    }
}

void Leveler::process(float* stereo, size_t frames) {
    if (frames == 0) return;
    if (!on_ && g_db_ == 0.0f && lin_ == 1.0f && lin_to_ == 1.0f) return;   // a wire
    // One pass: the block is measured and multiplied by the gain ramp the last block decided.
    const float g0 = lin_, dg = (lin_to_ - lin_) / (float)frames;
    float s1 = s_[0], s2 = s_[1], s3 = s_[2], s4 = s_[3];
    float acc = 0.0f;
    const bool measure = on_;
    for (size_t i = 0; i < frames; i++) {
        const float l = stereo[2 * i], r = stereo[2 * i + 1];
        if (measure) {
            const float m = 0.5f * (l + r);
            const float y = svf_tick(svf_tick(m, kw_shelf_, s1, s2), kw_hp_, s3, s4);
            acc = std::fmaf(y, y, acc);
        }
        const float g = std::fmaf((float)i, dg, g0);
        stereo[2 * i] = l * g;
        stereo[2 * i + 1] = r * g;
    }
    s_[0] = s1; s_[1] = s2; s_[2] = s3; s_[3] = s4;
    for (float& v : s_) if (v < 1e-15f && v > -1e-15f) v = 0.0f;
    lin_ = lin_to_;
    decide(acc / (float)frames, (float)frames / fs_);
    lin_to_ = g_db_ == 0.0f ? 1.0f : powf(10.0f, g_db_ / 20.0f);
}

bool LevelerSchedule::inside(int now_min, int from_min, int to_min) {
    if (from_min == to_min) return false;
    if (from_min < to_min) return now_min >= from_min && now_min < to_min;
    return now_min >= from_min || now_min < to_min;      // across midnight
}

int LevelerSchedule::tick(int now_min, bool enabled, int from_min, int to_min) {
    if (!enabled || now_min < 0) {
        last_ = -1;          // when it applies again, it applies from scratch
        return -1;
    }
    if (from_min != from_ || to_min != to_) {
        from_ = from_min;
        to_ = to_min;
        last_ = -1;          // moved: it says what it says now
    }
    const int in = inside(now_min, from_min, to_min) ? 1 : 0;
    if (in == last_) return -1;
    last_ = in;
    return in;
}

}  // namespace audio_core
