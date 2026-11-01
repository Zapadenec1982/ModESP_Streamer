/**
 * @file port_finder.cpp
 * @brief ToneMeter, find_port(), port_sweep_grid() (port_finder.h).
 */

#include "port_finder.h"

#include <cmath>

namespace audio_core {

void ToneMeter::start(float hz, float fs, uint32_t n) {
    const double w = 2.0 * M_PI * (double)hz / (double)fs;
    const double v = n ? 2.0 * M_PI / (double)n : 0.0;
    ref_r_ = 1.0f; ref_i_ = 0.0f;
    step_r_ = (float)std::cos(w); step_i_ = (float)-std::sin(w);
    win_r_ = 1.0f; win_i_ = 0.0f;
    wstep_r_ = (float)std::cos(v); wstep_i_ = (float)std::sin(v);
    acc_r_ = acc_i_ = wsum_ = 0.0f;
    n_ = n;
    pos_ = 0;
}

size_t ToneMeter::push(const float* x, size_t m) {
    size_t i = 0;
    for (; i < m && pos_ < n_; i++, pos_++) {
        const float w = 0.5f - 0.5f * win_r_;            // periodic Hann
        const float xw = x[i] * w;
        acc_r_ = std::fmaf(xw, ref_r_, acc_r_);
        acc_i_ = std::fmaf(xw, ref_i_, acc_i_);
        wsum_ += w;
        const float rr = ref_r_ * step_r_ - ref_i_ * step_i_;
        const float ri = ref_r_ * step_i_ + ref_i_ * step_r_;
        ref_r_ = rr; ref_i_ = ri;
        const float wr = win_r_ * wstep_r_ - win_i_ * wstep_i_;
        const float wi = win_r_ * wstep_i_ + win_i_ * wstep_r_;
        win_r_ = wr; win_i_ = wi;
        if ((pos_ & 255u) == 255u) {
            // Back onto the unit circle: a rotation in float shrinks or grows by an ulp a turn.
            const float g = 1.0f / std::sqrt(ref_r_ * ref_r_ + ref_i_ * ref_i_);
            ref_r_ *= g; ref_i_ *= g;
            const float h = 1.0f / std::sqrt(win_r_ * win_r_ + win_i_ * win_i_);
            win_r_ *= h; win_i_ *= h;
        }
    }
    return i;
}

float ToneMeter::amplitude() const {
    // A sine of amplitude A sums to (A/2)·Σw·e^(jφ) against the phasor.
    return wsum_ > 0.0f ? 2.0f * std::sqrt(acc_r_ * acc_r_ + acc_i_ * acc_i_) / wsum_ : 0.0f;
}

PortFit find_port(const float* hz, const float* level, const float* noise, int n) {
    PortFit r;
    if (n < 5) return r;
    constexpr int MAXN = 160;
    if (n > MAXN) n = MAXN;
    float L[MAXN], snr[MAXN], lf[MAXN];
    float loud = -1e9f;
    for (int k = 0; k < n; k++) {
        const double s2 = (double)level[k] * level[k], n2 = (double)noise[k] * noise[k];
        // Powers add: what the tone contributed is what was heard minus what was there anyway.
        // A point drowned in noise keeps a floor a little under it rather than going to −∞.
        const double clean = s2 - n2 > 0.1 * n2 ? s2 - n2 : 0.1 * n2;
        L[k] = clean > 1e-30 ? (float)(10.0 * std::log10(clean)) : -300.0f;
        snr[k] = n2 > 1e-30 ? (float)(10.0 * std::log10(s2 / n2 + 1e-30)) : 120.0f;
        lf[k] = (float)std::log2((double)hz[k]);
        if (snr[k] > loud) loud = snr[k];
    }
    r.loud_db = loud;
    if (loud < 20.0f) { r.status = PortFit::NOT_HEARD; return r; }

    // Three-point smoothing in dB: a single point thrown by noise makes no dip.
    float S[MAXN];
    S[0] = L[0];
    S[n - 1] = L[n - 1];
    for (int k = 1; k + 1 < n; k++) S[k] = 0.25f * L[k - 1] + 0.5f * L[k] + 0.25f * L[k + 1];

    // Every local minimum, and how far the curve climbs from it within an octave on each side.
    int best = -1;
    float best_score = 0.0f, best_above = 0.0f, best_below = 0.0f;
    for (int k = 1; k + 1 < n; k++) {
        if (!(S[k] <= S[k - 1] && S[k] <= S[k + 1])) continue;
        float below = -1e9f, above = -1e9f;
        for (int j = k - 1; j >= 0 && lf[k] - lf[j] <= 1.0f; j--) if (S[j] > below) below = S[j];
        for (int j = k + 1; j < n && lf[j] - lf[k] <= 1.0f; j++) if (S[j] > above) above = S[j];
        below -= S[k];
        above -= S[k];
        if (below < 1.5f || above < 6.0f) continue;
        const float score = above + 2.0f * below;
        if (score > best_score) { best_score = score; best = k; best_above = above; best_below = below; }
    }
    if (best < 0) { r.status = PortFit::NO_DIP; return r; }

    // Where exactly: the smoothing found the dip, the unsmoothed curve places it (smoothing
    // leans an asymmetric dip towards its gentler side). The lowest raw point next to it, and the
    // vertex of the parabola through that and its two neighbours, in log frequency.
    int k = best;
    if (k - 1 >= 1 && L[k - 1] < L[k]) k--;
    if (k + 1 <= n - 2 && L[k + 1] < L[k]) k++;
    const double x0 = lf[k - 1], x1 = lf[k], x2 = lf[k + 1];
    const double y0 = L[k - 1], y1 = L[k], y2 = L[k + 1];
    const double d1 = (y1 - y0) / (x1 - x0), d2 = (y2 - y1) / (x2 - x1);
    const double a = (d2 - d1) / (x2 - x0);
    double xv = x1;
    if (a > 1e-12) {
        xv = 0.5 * (x0 + x1) - d1 / (2.0 * a);
        if (xv < x0) xv = x0;
        if (xv > x2) xv = x2;
    }
    r.hz = (float)std::exp2(xv);
    r.depth_db = best_above;
    r.below_db = best_below;
    r.snr_db = snr[k];
    // OK is what the panel writes into the port setting on its own, so it takes a dip that is
    // clear on both sides. A room mode at fb fills a dip and moves it: an SC-G101 measured
    // 67.7 Hz with 17 dB above and only 4 dB below, and its cone stood still at 63.
    r.status = (best_above >= 12.0f && best_below >= 6.0f && snr[k] >= 15.0f) ? PortFit::OK : PortFit::WEAK;
    return r;
}
int port_sweep_grid(int lo, int hi, int* out, int max) {
    int n = 0, last = 0;
    for (int j = 0; n < max; j++) {
        const int f = (int)std::lround((double)lo * std::exp2((double)j / 24.0));
        if (f > hi) break;
        if (f == last || f == 50 || f == 60) continue;
        out[n++] = last = f;
    }
    return n;
}

}  // namespace audio_core
