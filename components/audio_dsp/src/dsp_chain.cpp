/**
 * @file dsp_chain.cpp
 * @brief PEQ (RBJ responses on SVF sections), ramped volume, peak limiter, meters, tone.
 */

#include "dsp_chain.h"
#include <cmath>
#include <cstring>

namespace audio_core {

float g_fs_hz = FS_HZ_DEFAULT;

// ── The same responses as one trapezoidal SVF section (A. Simper, SvfLinearTrapOptimised2) ──
//
// The SVF's analogue prototype is H(s) = m0 + (m1·s + m2)/(s² + k·s + 1), with s normalised so
// that the prewarped corner g = tan(πf/fs) maps to 1. Each RBJ type is a mix:
//   peak:       H = (s² + s·A/Q + 1)/(s² + s/(A·Q) + 1)          k = 1/(A·Q), m1 = k(A² − 1)
//   low shelf:  RBJ's A(s² + s√A/Q + A)/(A·s² + s√A/Q + 1)        corner moved by 1/√A
//   high shelf: RBJ's A(A·s² + s√A/Q + 1)/(s² + s√A/Q + A)        corner moved by √A
//   pass/notch: the Butterworth-style 2nd-order prototypes at Q
// and the bilinear transform with g = tan(w0/2) is RBJ's own prewarp, so the digital
// responses are RBJ's exactly (tests/host/test_dsp_svf.cpp holds the two together).
/// Q as the design uses it: a shelf's is capped (SHELF_Q_MAX).
static double design_q(const EqBand& band) {
    const bool shelf = band.type == BandType::LOW_SHELF || band.type == BandType::HIGH_SHELF;
    return shelf && band.q > SHELF_Q_MAX ? (double)SHELF_Q_MAX : (double)band.q;
}

double analog_mag2(const EqBand& band, double f_hz) {
    const double A = pow(10.0, (double)band.gain_db / 40.0), q = design_q(band);
    const double W = f_hz / band.freq_hz, W2 = W * W;     // s = jW
    double nr, ni, dr, di;
    switch (band.type) {
    case BandType::PEAK:       nr = 1 - W2;          ni = W * A / q;          dr = 1 - W2;     di = W / (A * q); break;
    case BandType::LOW_SHELF:  nr = A * (A - W2);    ni = A * W * sqrt(A) / q; dr = 1 - A * W2; di = W * sqrt(A) / q; break;
    case BandType::HIGH_SHELF: nr = A * (1 - A * W2); ni = A * W * sqrt(A) / q; dr = A - W2;     di = W * sqrt(A) / q; break;
    case BandType::NOTCH:      nr = 1 - W2;          ni = 0;                   dr = 1 - W2;     di = W / q; break;
    case BandType::LOW_PASS:   nr = 1;               ni = 0;                   dr = 1 - W2;     di = W / q; break;
    case BandType::HIGH_PASS:  nr = -W2;             ni = 0;                   dr = 1 - W2;     di = W / q; break;
    default: return 1.0;
    }
    const double den = dr * dr + di * di;
    return den > 1e-300 ? (nr * nr + ni * ni) / den : 1.0;
}

/// Vicanek's matched design for a peak, a shelf or a notch, as SVF numbers. False when the
/// magnitude match has no real solution; the caller then keeps the bilinear design.
static bool design_matched(const EqBand& band, double fs, SvfDesign& d) {
    const double A = pow(10.0, (double)band.gain_db / 40.0), q = design_q(band);
    const double w0 = 2.0 * M_PI * band.freq_hz / fs;
    double wp, zeta;
    switch (band.type) {
    case BandType::PEAK:       wp = 1.0;      zeta = 1.0 / (2.0 * A * q); break;
    case BandType::LOW_SHELF:  wp = 1.0 / sqrt(A); zeta = 1.0 / (2.0 * q); break;
    case BandType::HIGH_SHELF: wp = sqrt(A);  zeta = 1.0 / (2.0 * q); break;
    case BandType::NOTCH:      wp = 1.0;      zeta = 1.0 / (2.0 * q); break;
    default: return false;
    }
    // Poles: the analogue ones, mapped by impulse invariance.
    const double Wn = w0 * wp, e = exp(-zeta * Wn);
    const double a2 = e * e;
    const double a1 = zeta <= 1.0 ? -2.0 * e * cos(Wn * sqrt(1.0 - zeta * zeta))
                                  : -2.0 * e * cosh(Wn * sqrt(zeta * zeta - 1.0));
    double b0, b1, b2;
    if (band.type == BandType::NOTCH) {
        // The zero exactly at f0, scaled for unity at DC.
        const double cw = cos(w0), n = (1.0 + a1 + a2) / (2.0 - 2.0 * cw);
        b0 = n; b1 = -2.0 * n * cw; b2 = n;
    } else {
        // Zeros: |H|² = (B0 φ0 + B1 φ1 + B2 φ2)/(A0 φ0 + A1 φ1 + A2 φ2), φ1 = sin²(w/2),
        // φ0 = 1 − φ1, φ2 = 4 φ0 φ1; B0, B1 from DC and Nyquist, B2 from one more frequency.
        const double A0 = (1.0 + a1 + a2) * (1.0 + a1 + a2), A1 = (1.0 - a1 + a2) * (1.0 - a1 + a2), A2 = -4.0 * a2;
        const double wm = w0 < 0.9 * M_PI ? w0 : 0.9 * M_PI;
        const double p1 = sin(wm / 2.0) * sin(wm / 2.0), p0 = 1.0 - p1, p2 = 4.0 * p0 * p1;
        const double B0 = A0 * analog_mag2(band, 0.0);
        const double B1 = A1 * analog_mag2(band, fs / 2.0);
        const double B2 = (analog_mag2(band, wm * fs / (2.0 * M_PI)) * (A0 * p0 + A1 * p1 + A2 * p2) - B0 * p0 - B1 * p1) / p2;
        const double V = (sqrt(B0) + sqrt(B1)) / 2.0;
        if (V * V + B2 < 0.0) return false;
        b1 = (sqrt(B0) - sqrt(B1)) / 2.0;
        b0 = (V + sqrt(V * V + B2)) / 2.0;
        b2 = V - b0;
    }
    // The biquad {b0 b1 b2; 1 a1 a2} as an SVF (the bilinear map inverted):
    // (1+a1+a2)/(1−a1+a2) = g², 1 − a2 = 2kg/a0 with a0 = 4/(1−a1+a2).
    const double dd = 1.0 - a1 + a2;
    if (!(dd > 0.0) || !(1.0 + a1 + a2 > 0.0)) return false;
    const double g = sqrt((1.0 + a1 + a2) / dd);
    if (!(g > 0.0)) return false;
    d.g  = g;
    d.k  = 2.0 * (1.0 - a2) / (dd * g);
    d.m0 = (b0 - b1 + b2) / dd;
    d.m1 = (2.0 * (b0 - b2) / dd - d.m0 * g * d.k) / g;
    d.m2 = (b0 + b1 + b2) / (dd * g * g) - d.m0;
    return true;
}

bool design_svf_params(const EqBand& band, double fs, SvfDesign& d, Design how) {
    if (!(fs >= 1000.0)) return false;
    const double f = band.freq_hz, q = design_q(band);
    if (!(f >= 10.0) || f > fs * 0.49 || !(q > 0.0)) return false;
    if (how == Design::MATCHED && design_matched(band, fs, d)) return true;
    const double A = pow(10.0, (double)band.gain_db / 40.0);   // √ of the linear gain
    double g = tan(M_PI * f / fs);
    double k = 1.0 / q;
    switch (band.type) {
    case BandType::PEAK:       k = 1.0 / (q * A); d.m0 = 1.0; d.m1 = k * (A * A - 1.0); d.m2 = 0.0; break;
    case BandType::LOW_SHELF:  g /= sqrt(A);      d.m0 = 1.0; d.m1 = k * (A - 1.0);     d.m2 = A * A - 1.0; break;
    case BandType::HIGH_SHELF: g *= sqrt(A);      d.m0 = A * A; d.m1 = k * (1.0 - A) * A; d.m2 = 1.0 - A * A; break;
    case BandType::LOW_PASS:   d.m0 = 0.0; d.m1 = 0.0; d.m2 = 1.0; break;
    case BandType::HIGH_PASS:  d.m0 = 1.0; d.m1 = -k;  d.m2 = -1.0; break;
    case BandType::NOTCH:      d.m0 = 1.0; d.m1 = -k;  d.m2 = 0.0; break;
    default: return false;
    }
    d.g = g;
    d.k = k;
    return true;
}

bool design_svf(const EqBand& band, double fs, SvfCoef& c, Design how) {
    SvfDesign d;
    if (!design_svf_params(band, fs, d, how)) return false;
    const double c1 = 1.0 / (1.0 + d.g * (d.g + d.k));
    const double c2 = d.g * c1;
    c.c1 = (float)c1;
    c.c2 = (float)c2;
    c.c3 = (float)(d.g * c2);
    c.m0 = (float)d.m0;
    c.m1 = (float)d.m1;
    c.m2 = (float)d.m2;
    return true;
}

/// |H|² from the prototype on the unit circle. With S = sin(w/2), C = cos(w/2) the bilinear
/// s is j·S/(g·C); multiplying through by g²C² keeps everything finite from DC to Nyquist:
///   den = (g²C² − S²) + j·g·k·S·C,   num = m0·den + m2·g²C² + j·m1·g·S·C.
static double svf_mag2(const SvfDesign& d, double S, double C) {
    const double gc  = d.g * C;
    const double dr  = gc * gc - S * S;
    const double di  = d.k * gc * S;
    const double nr  = d.m0 * dr + d.m2 * gc * gc;
    const double ni  = d.m0 * di + d.m1 * gc * S;
    const double den = dr * dr + di * di;
    return den > 1e-300 ? (nr * nr + ni * ni) / den : 1.0;
}

double svf_magnitude_db(const SvfDesign& d, double fs, double f_hz) {
    const double h = M_PI * f_hz / fs;                 // w/2
    const double m2 = svf_mag2(d, sin(h), cos(h));
    return m2 > 1e-30 ? 10.0 * log10(m2) : -300.0;
}

float band_magnitude_db(const EqBand& band, float f_hz) {
    if (!band.enabled) return 0.0f;
    SvfDesign d;
    if (!design_svf_params(band, fs_hz(), d)) return 0.0f;
    return (float)svf_magnitude_db(d, fs_hz(), f_hz);
}

float eq_magnitude_db(const EqBand* bands, int count, float f_hz) {
    float sum = 0.0f;
    for (int i = 0; i < count; i++) sum += band_magnitude_db(bands[i], f_hz);
    return sum;
}

void eq_curve_db(const EqBand* bands, int count, const float* f_hz, int n, float* out_db) {
    // Each band designed once (in double, as the audio path does), the curve then evaluated in
    // float: one sin/cos per point shared by every band, the bands' |H|² multiplied, one log per
    // point. On the P4 double is soft-float, and per-band sin, cos and log in double over 97
    // points was ~30 ms a redraw; this is a few hundred microseconds. A display needs no more
    // than float's 1e-6 dB.
    struct P { float g, k, m0, m1, m2; };
    P p[MAX_BANDS];
    int nb = 0;
    if (count > MAX_BANDS) count = MAX_BANDS;
    const double fs = fs_hz();
    for (int i = 0; i < count; i++) {
        SvfDesign d;
        if (!bands[i].enabled || !design_svf_params(bands[i], fs, d)) continue;
        p[nb++] = {(float)d.g, (float)d.k, (float)d.m0, (float)d.m1, (float)d.m2};
    }
    for (int j = 0; j < n; j++) {
        if (nb == 0) { out_db[j] = 0.0f; continue; }
        const float h = (float)M_PI * f_hz[j] / (float)fs;
        const float S = sinf(h), C = cosf(h);
        float prod = 1.0f;
        for (int b = 0; b < nb; b++) {
            const float gc = p[b].g * C;
            const float dr = gc * gc - S * S, di = p[b].k * gc * S;
            const float nr = p[b].m0 * dr + p[b].m2 * gc * gc;
            const float ni = p[b].m0 * di + p[b].m1 * gc * S;
            const float den = dr * dr + di * di;
            prod *= den > 1e-30f ? (nr * nr + ni * ni) / den : 1.0f;
        }
        out_db[j] = prod > 1e-30f ? 10.0f * log10f(prod) : -300.0f;
    }
}

// ── DspChain ──

void DspChain::reset() {
    // A hard restart — a source switch or a new clock — and the one place the filter memory
    // IS cleared: what it holds belongs to another signal or another sample rate. A fade in
    // flight is finished on the spot, and an edit waiting behind it lands at once: the
    // envelope opens from silence anyway, so there is nothing to blend.
    auto restart = [](Band* list, int n) {
        for (int i = 0; i < n; i++) {
            Band& b = list[i];
            b.len = 0;
            if (b.pend) {
                b.on = b.pend_on;
                b.c = b.pend_on ? b.pend_c : SvfCoef{};
                b.pend = false;
            }
            b.st = SvfState{};
            b.old_st = SvfState{};
        }
    };
    restart(band_, MAX_BANDS);
    restart(sub_, 3);
    restart(tone_, 2);
    lev_.reset();
    bal_l_ = bal_tl_;                // the balance starts where it is set: nothing to glide from
    bal_r_ = bal_tr_;
    side_s1_ = side_s2_ = 0.0f;
    // The loudness shelves start where the volume has them, from rest: the envelope opens from
    // silence, so there is nothing to glide from.
    loud_lo_db_ = loud_lo_t_;
    loud_hi_db_ = loud_hi_t_;
    loud_lo_c_ = shelf_coef(loud_glo_, LOUD_LO_Q, loud_lo_db_, false);
    loud_hi_c_ = shelf_coef(loud_ghi_, LOUD_HI_Q, loud_hi_db_, true);
    loud_lo_st_ = SvfState{};
    loud_hi_st_ = SvfState{};
    loud_run_ = loud_lo_db_ != 0.0f || loud_hi_db_ != 0.0f;
    lim_reset();
    if (xo_switching_) { xo_switching_ = false; bass_xo_hz_ = pend_xo_; }
    apply_xo(bass_xo_hz_);
    vol_g1_ = vol_gain_ = 0.0f;   // fade in from silence after a source switch
    mute_ramp_ = false;
    peak_l_ = peak_r_ = 0.0f;
}

void DspChain::set_sample_rate(float hz) {
    if (!(hz >= 1000.0f)) hz = FS_HZ_DEFAULT;
    sample_rate_hz_ = hz;
    lev_.set_sample_rate(hz);
    // k = 1 − e^(−1/(τ·fs)): the one-pole that reaches 63 % of a step in τ seconds.
    vol_k_   = 1.0f - expf(-1.0f / (VOL_TAU_S * hz));
    mute_k_  = 1.0f - expf(-1.0f / (MUTE_TAU_S * hz));
    // The brickwall's chunk: about 0.17 ms at every rate, and a divisor of the 240-frame block.
    lim_D_    = hz <= 50000.0f ? 8u : hz <= 100000.0f ? 16u : LIM_MAX_D;
    lim_L_    = (uint32_t)(LIM_M + 1) * lim_D_;
    lim_env_.configure(LIM_M, lim_D_, LIM_HOLD_S, LIM_RELEASE_S, hz);
    lim_reset();
    bass_kp_ = 1.0f - expf(-(float)lim_D_ / (0.05f * hz));     // 50 ms of power averaging
    {
        // Known from the rate alone, so latency_frames() is right before the stage is switched on.
        int mb = (int)lroundf(BASS_LOOKAHEAD_S * hz / (float)lim_D_);
        mb = mb < 1 ? 1 : mb > KnotEnv::MAX_M ? KnotEnv::MAX_M : mb;
        bass_L_ = (uint32_t)(mb + 1) * lim_D_;
    }
    apply_xo(xo_switching_ ? pend_xo_ : bass_xo_hz_);
    xo_switching_ = false;
    fade_len_ = (uint32_t)lroundf(FADE_S * hz);
    if (fade_len_ < 1) fade_len_ = 1;
    // The loudness corners at this rate, and its glide as a step per block. No treble shelf where
    // 10 kHz is too close to Nyquist to design (no rate this player runs at).
    loud_glo_ = tanf((float)M_PI * LOUD_LO_HZ / hz);
    loud_ghi_ = LOUD_HI_HZ < 0.45f * hz ? tanf((float)M_PI * LOUD_HI_HZ / hz) : 0.0f;
    loud_k_   = 1.0f - expf(-(float)BLOCK / (LOUD_TAU_S * hz));
    loud_lo_c_ = shelf_coef(loud_glo_, LOUD_LO_Q, loud_lo_db_, false);
    loud_hi_c_ = shelf_coef(loud_ghi_, LOUD_HI_Q, loud_hi_db_, true);
    redesign_now();
    set_mono_bass(mono_bass_hz_);
}

/// The float coefficients of a prototype (design_svf, without designing twice).
static SvfCoef coef_of(const SvfDesign& d) {
    SvfCoef c;
    const double c1 = 1.0 / (1.0 + d.g * (d.g + d.k)), c2 = d.g * c1;
    c.c1 = (float)c1; c.c2 = (float)c2; c.c3 = (float)(d.g * c2);
    c.m0 = (float)d.m0; c.m1 = (float)d.m1; c.m2 = (float)d.m2;
    return c;
}

void DspChain::redesign_now() {
    stage_subsonic(false);
    stage_tone(false);
    for (int i = 0; i < MAX_BANDS; i++) {
        SvfDesign d;
        want_[i] = i < band_count_ && bands_[i].enabled && design_svf_params(bands_[i], sample_rate_hz_, d);
        want_d_[i] = d;
        want_c_[i] = want_[i] ? coef_of(d) : SvfCoef{};
        Band& b = band_[i];
        b.on = eq_on_ && want_[i];
        b.c = b.on ? want_c_[i] : SvfCoef{};
        b.len = 0;
        b.pend = false;
    }
    update_headroom();
}

void DspChain::begin_fade(Band& b, bool on, const SvfCoef& c, uint32_t len) {
    b.old_c  = b.c;
    b.old_st = b.st;            // the outgoing design keeps its own state to the end of the fade
    b.old_on = b.on;
    // A band that was a wire has memory from whenever it last ran — another signal — so it
    // starts from rest. One that was running continues from its state with the new numbers:
    // a state that belongs to slightly different coefficients is a small transient, and the
    // fade is what hides it.
    if (on && !b.on) b.st = SvfState{};
    b.c   = on ? c : SvfCoef{};
    b.on  = on;
    b.pos = 0;
    b.len = len;
    b.fade_n = len;
}

void DspChain::stage(Band& b, bool on, const SvfCoef& c, uint32_t len) {
    // Where the band is heading once everything already queued has landed.
    const bool     dest_on = b.pend ? b.pend_on : b.on;
    const SvfCoef& dest_c  = b.pend ? b.pend_c : b.c;
    if (on == dest_on && (!on || memcmp(&c, &dest_c, sizeof(c)) == 0)) return;
    if (b.len) {
        // Mid-fade: one edit waits, and a later one takes its place. Fading from a blend to
        // a third design would need a third filter; waiting costs at most one fade.
        b.pend    = true;
        b.pend_on = on;
        b.pend_c  = c;
        return;
    }
    begin_fade(b, on, c, len);
}

void DspChain::set_bands(const EqBand* bands, int count) {
    if (count < 0) count = 0;
    if (count > MAX_BANDS) count = MAX_BANDS;
    for (int i = 0; i < MAX_BANDS; i++) {
        SvfDesign d;
        bands_[i]  = i < count ? bands[i] : EqBand{};
        want_[i]   = i < count && bands[i].enabled && design_svf_params(bands[i], sample_rate_hz_, d);
        want_d_[i] = d;
        want_c_[i] = want_[i] ? coef_of(d) : SvfCoef{};
        stage_band(i, eq_on_ && want_[i], want_c_[i]);
    }
    band_count_ = count;
    update_headroom();
}

void DspChain::set_eq_enabled(bool on) {
    if (on == eq_on_) return;
    eq_on_ = on;
    for (int i = 0; i < MAX_BANDS; i++) stage_band(i, eq_on_ && want_[i], want_c_[i]);
    update_headroom();
}

void DspChain::design_subsonic(SvfDesign d[3], bool on[3]) const {
    on[0] = on[1] = on[2] = false;
    const float fc = sub_corner();
    if (fc >= 20.0f && fc <= 80.0f) {
        // A 4th-order Butterworth as two 2nd-order sections: Q = 1/(2 cos(π/8)), 1/(2 cos(3π/8)).
        static const float Q[2] = {0.5411961f, 1.3065630f};
        bool ok = true;
        for (int j = 0; j < 2 && ok; j++) {
            EqBand b;
            b.type = BandType::HIGH_PASS;
            b.freq_hz = fc;
            b.q = Q[j];
            b.enabled = true;
            ok = design_svf_params(b, sample_rate_hz_, d[j]);
        }
        on[0] = on[1] = ok;
    }
    if (port_hz_ > 0.0f && port_lift_db_ > 0.0f) {
        EqBand b;
        b.type = BandType::PEAK;
        b.freq_hz = port_hz_;
        b.gain_db = port_lift_db_;
        b.q = PORT_Q;
        b.enabled = true;
        on[2] = design_svf_params(b, sample_rate_hz_, d[2]);
    }
}

void DspChain::stage_subsonic(bool fade) {
    SvfDesign d[3];
    bool on[3];
    design_subsonic(d, on);
    sub_fc_ = sub_corner();
    sub_on_ = on[0] || on[1] || on[2];
    for (int j = 0; j < 3; j++) {
        sub_d_[j] = d[j];
        sub_sec_[j] = on[j];
        const SvfCoef c = on[j] ? coef_of(d[j]) : SvfCoef{};
        Band& b = sub_[j];
        if (!fade) {
            b.on = on[j];
            b.c = c;
            b.len = 0;
            b.pend = false;
            continue;
        }
        // Four times an EQ band's fade: just above its corner a 4th-order high-pass turns a
        // bass note by over 100°, and blending two signals that far apart in 20 ms splatters
        // (−78.6 dBFS above 500 Hz on a −12 dBFS note); over 80 ms it does not.
        stage(b, on[j], c, 4 * fade_len_);
    }
}

void DspChain::apply_xo(float hz) {
    bass_xo_hz_ = hz;
    bass_on_ = false;
    memset(det_s_, 0, sizeof(det_s_));
    memset(app_s_, 0, sizeof(app_s_));
    bass_pos_ = bass_k_ = 0;
    bass_ph_ = bass_pl_ = bass_ph_prev_ = bass_pl_prev_ = 0.0f;
    bass_eh_ = bass_el_ = bass_pwh_ = bass_pwl_ = 0.0f;
    bass_g_ = bass_min_ = 1.0f;
    bass_dg_ = 0.0f;
    if (!xo_valid(hz) || !bass_line_) { update_headroom(); return; }
    // Linkwitz–Riley 4th order = two Butterworth 2nd-order sections at the same corner. Section
    // A's two nodes give its low-pass and, mixed as x − 2k·v1, the 2nd-order all-pass that an
    // LR4 pair sums to; section B low-passes A's low-pass to LR4. The high band is then
    // AP − LP4, exactly — so with the bass untouched the stage is a pure all-pass, flat.
    EqBand b;
    b.type = BandType::LOW_PASS;
    b.freq_hz = hz;
    b.q = 0.70710678f;
    b.enabled = true;
    SvfDesign d;
    if (!design_svf_params(b, sample_rate_hz_, d)) { update_headroom(); return; }
    bass_a_ = coef_of(d);
    bass_2k_ = (float)(2.0 * d.k);
    const int mb = (int)lroundf(BASS_LOOKAHEAD_S * sample_rate_hz_ / (float)lim_D_);
    bass_env_.configure(mb, lim_D_, BASS_HOLD_S, BASS_RELEASE_S, sample_rate_hz_);
    bass_L_ = (uint32_t)(bass_env_.M + 1) * lim_D_;            // as set_sample_rate() worked out
    if (bass_L_ > bass_line_cap_) { update_headroom(); return; }
    memset(bass_line_, 0, (size_t)bass_L_ * 2 * sizeof(float));
    bass_on_ = true;
    update_headroom();
}

void DspChain::set_bass_xo(float hz) {
    if (hz == (xo_switching_ ? pend_xo_ : bass_xo_hz_)) return;
    if (vol_gain_ == 0.0f && vol_g1_ == 0.0f) {   // silent already: no need to fade
        xo_switching_ = false;
        apply_xo(hz);
        return;
    }
    pend_xo_ = hz;
    xo_switching_ = true;
    mute_ramp_ = true;
}

void DspChain::set_rms_cap_db(float db) {
    bass_cap_ = (db < 0.0f && db >= -12.0f) ? powf(10.0f, db / 10.0f) : 0.0f;
    bass_pwh_ = bass_pwl_ = 0.0f;
}

void DspChain::set_bass_line(float* buf, size_t frames) {
    bass_line_ = buf;
    bass_line_cap_ = buf ? frames : 0;
    apply_xo(bass_xo_hz_);
}

void DspChain::set_subsonic(float hz) {
    sub_hz_ = hz;
    stage_subsonic(true);
    update_headroom();
}

void DspChain::set_port(float hz, float lift_db) {
    port_hz_ = (hz >= 30.0f && hz <= 80.0f) ? hz : 0.0f;
    port_lift_db_ = !(lift_db > 0.0f) ? 0.0f : lift_db > 6.0f ? 6.0f : lift_db;
    stage_subsonic(true);
    update_headroom();
}

void DspChain::set_loudness(float strength) {
    loud_s_ = !(strength > 0.0f) ? 0.0f : strength > 1.0f ? 1.0f : strength;
    loud_retarget();
}

void DspChain::set_loud_ref(float pct) {
    if (!(pct == pct)) pct = 100.0f;
    loud_ref_ = pct < 20.0f ? 20.0f : pct > 100.0f ? 100.0f : pct;
    loud_retarget();
}

void DspChain::stage_tone(bool fade) {
    static const BandType TYPE[2] = {BandType::LOW_SHELF, BandType::HIGH_SHELF};
    static const float F[2] = {TONE_LO_HZ, TONE_HI_HZ};
    for (int j = 0; j < 2; j++) {
        EqBand e;
        e.type = TYPE[j];
        e.freq_hz = F[j];
        e.gain_db = tone_db_[j];
        e.q = TONE_Q;
        e.enabled = true;
        SvfDesign d;
        // At 0 dB not a filter at all: a wire, so the default path is bit-exact.
        const bool on = tone_db_[j] != 0.0f && design_svf_params(e, sample_rate_hz_, d);
        tone_d_[j] = d;
        tone_sec_[j] = on;
        const SvfCoef c = on ? coef_of(d) : SvfCoef{};
        Band& b = tone_[j];
        if (fade) {
            stage(b, on, c, fade_len_);
            continue;
        }
        b.on = on;
        b.c = c;
        b.len = 0;
        b.pend = false;
    }
}

void DspChain::set_tone(float bass_db, float treble_db) {
    auto clamp = [](float v) {
        if (!(v == v)) return 0.0f;             // NaN: no shelf, rather than the most of one
        return v < -TONE_MAX_DB ? -TONE_MAX_DB : v > TONE_MAX_DB ? TONE_MAX_DB : v;
    };
    tone_db_[0] = clamp(bass_db);
    tone_db_[1] = clamp(treble_db);
    stage_tone(true);
    update_headroom();
}

void DspChain::set_balance_db(float db) {
    if (!(db == db)) db = 0.0f;
    db = db < -BAL_MAX_DB ? -BAL_MAX_DB : db > BAL_MAX_DB ? BAL_MAX_DB : db;
    const float cut = db == 0.0f ? 1.0f : powf(10.0f, -fabsf(db) / 20.0f);
    bal_tl_ = db > 0.0f ? cut : 1.0f;
    bal_tr_ = db < 0.0f ? cut : 1.0f;
}

void DspChain::run_balance(float* stereo, size_t frames) {
    const float tl = bal_tl_, tr = bal_tr_;
    if (bal_l_ == tl && bal_r_ == tr) {
        for (size_t i = 0; i < frames; i++) {
            stereo[2 * i]     *= tl;
            stereo[2 * i + 1] *= tr;
        }
        return;
    }
    // A balance moved is a volume moved, one channel at a time: the same one-pole (VOL_TAU_S),
    // and the same snap once it has arrived or stopped arriving (run_gain()).
    const float k = vol_k_;
    const float l0 = bal_l_, r0 = bal_r_;
    float gl = l0, gr = r0;
    for (size_t i = 0; i < frames; i++) {
        gl += (tl - gl) * k;
        gr += (tr - gr) * k;
        stereo[2 * i]     *= gl;
        stereo[2 * i + 1] *= gr;
    }
    if ((gl == l0 && gr == r0) || (fabsf(gl - tl) <= 1e-6f && fabsf(gr - tr) <= 1e-6f)) {
        gl = tl;
        gr = tr;
    }
    bal_l_ = gl;
    bal_r_ = gr;
}

void DspChain::loud_retarget() {
    // How far under the top of the volume scale the volume sits. The top is drive − B, so a
    // boosted EQ moves it down and the loudness reference with it: the shelves lift at most
    // LOUD_LO_K of what the volume took away, and nothing reaches past what the top plays.
    // At 0 % nothing is heard; the last level stays, so coming back up does not glide.
    if (vol_pct_ > 0.0f) {
        float a;
        if (loud_ref_ >= 100.0f) {
            a = -volume_db(vol_pct_) - headroom_db_;
        } else {
            // «Звичайна гучність»: measured from the reference rather than from the top. Each
            // level is where it sits on the scale, no higher than the top (drive − B; the drive is
            // in both and goes).
            auto level = [this](float p) {
                const float l = volume_db(p);
                return l < -headroom_db_ ? l : -headroom_db_;
            };
            a = level(loud_ref_) - level(vol_pct_);
        }
        loud_att_ = a < 0.0f ? 0.0f : a > LOUD_ATT_MAX_DB ? LOUD_ATT_MAX_DB : a;
    }
    loud_lo_t_ = loud_s_ * LOUD_LO_K * loud_att_;
    loud_hi_t_ = loud_ghi_ > 0.0f ? loud_s_ * LOUD_HI_K * loud_att_ : 0.0f;
}

SvfCoef DspChain::shelf_coef(float g0, float q, float gain_db, bool high) {
    SvfCoef c;
    if (!(g0 > 0.0f)) return c;
    // design_svf_params()'s shelves (BILINEAR), in float. At 0 dB A is exactly 1, m1 and m2 are
    // exactly 0 and the section passes its input unchanged, whatever its state.
    const float A = powf(10.0f, gain_db / 40.0f), sA = sqrtf(A);
    const float g = high ? g0 * sA : g0 / sA, k = 1.0f / q;
    const float c1 = 1.0f / (1.0f + g * (g + k));
    c.c1 = c1;
    c.c2 = g * c1;
    c.c3 = g * c.c2;
    if (high) { c.m0 = A * A; c.m1 = k * (1.0f - A) * A; c.m2 = 1.0f - A * A; }
    else      { c.m0 = 1.0f;  c.m1 = k * (A - 1.0f);     c.m2 = A * A - 1.0f; }
    return c;
}

void DspChain::run_loudness(Passes& p) {
    // One block's step of the glide, snapped once within 0.01 dB of the target.
    auto glide = [this](float cur, float to) {
        const float d = to - cur;
        return fabsf(d) <= 0.01f ? to : std::fmaf(d, loud_k_, cur);
    };
    const float lo0 = loud_lo_db_, hi0 = loud_hi_db_;
    loud_lo_db_ = glide(lo0, loud_lo_t_);
    loud_hi_db_ = glide(hi0, loud_hi_t_);
    if (!loud_run_) {
        if (loud_lo_db_ == 0.0f && loud_hi_db_ == 0.0f) return;
        // Out of a wire: lo0 and hi0 are 0 dB, where a shelf passes its input whatever its state,
        // so the sections start from rest on their 0 dB designs and the ramp below is seamless.
        loud_lo_st_ = SvfState{};
        loud_hi_st_ = SvfState{};
        loud_lo_c_ = shelf_coef(loud_glo_, LOUD_LO_Q, 0.0f, false);
        loud_hi_c_ = shelf_coef(loud_ghi_, LOUD_HI_Q, 0.0f, true);
        loud_run_ = true;
    }
    if (loud_lo_db_ != lo0 || loud_hi_db_ != hi0) {
        p.flush();
        const SvfCoef lo = shelf_coef(loud_glo_, LOUD_LO_Q, loud_lo_db_, false);
        const SvfCoef hi = shelf_coef(loud_ghi_, LOUD_HI_Q, loud_hi_db_, true);
        svf_stereo_ramp(p.x, p.n, loud_lo_c_, lo, loud_lo_st_);
        svf_stereo_ramp(p.x, p.n, loud_hi_c_, hi, loud_hi_st_);
        p.count += 2;
        loud_lo_c_ = lo;
        loud_hi_c_ = hi;
    } else {
        p.add(loud_lo_c_, loud_lo_st_);
        p.add(loud_hi_c_, loud_hi_st_);
    }
    // Back at 0 dB the designs pass the input exactly: a wire again from the next block.
    if (loud_lo_db_ == 0.0f && loud_hi_db_ == 0.0f) loud_run_ = false;
}

float DspChain::volume_cut_db() const {
    if (vol_gain_ <= 1e-6f) return 0.0f;
    float db = -20.0f * log10f(vol_gain_);
    if (db < 0.0f) db = 0.0f; else if (db > -VOL_FLOOR_DB) db = -VOL_FLOOR_DB;
    return db;
}

float DspChain::volume_db(float pct) {
    // From the knee up this is the expression the law always was, so the top half of the scale
    // plays as before. Under it, u runs from 0 at the knee to 1 at 1 % (a little past 1 for a
    // fractional level under «Максимум»), and u² keeps the slope continuous.
    const float db = -60.0f * (1.0f - pct / 100.0f);
    if (pct >= VOL_KNEE_PCT) return db;
    constexpr float EXTRA = VOL_FLOOR_DB + 60.0f * (1.0f - 1.0f / 100.0f);   // −20.6 dB at 1 %
    const float u = (VOL_KNEE_PCT - pct) / (VOL_KNEE_PCT - 1.0f);
    return db + EXTRA * u * u;
}

void DspChain::set_volume(float pct) {
    vol_pct_ = !(pct > 0.0f) ? 0.0f : pct > 100.0f ? 100.0f : pct;   // NaN: silence
    update_gain();
}

void DspChain::set_pre_gain_db(float db) {
    if (!(db >= -24.0f)) db = -24.0f; else if (db > 12.0f) db = 12.0f;   // a trim, not a gain stage
    trim_db_ = db;
    update_headroom();
}

void DspChain::set_drive_db(float db) {
    if (!(db >= 0.0f)) db = 0.0f; else if (db > 6.0f) db = 6.0f;
    drive_db_ = db;
    update_gain();
}

void DspChain::update_gain() {
    loud_retarget();
    if (vol_pct_ <= 0.0f) { gain_target_ = 0.0f; return; }
    // 1 % = −80 dB, 100 % = drive (volume_db) — and never above the point where the EQ's
    // loudest band reaches the ceiling.
    const float v   = drive_db_ + volume_db(vol_pct_);
    const float cap = drive_db_ - headroom_db_;
    const float db  = trim_db_ + (v < cap ? v : cap);
    gain_target_ = db == 0.0f ? 1.0f : powf(10.0f, db / 20.0f);
}

void DspChain::update_headroom() {
    const bool tone = tone_sec_[0] || tone_sec_[1];
    float b = trim_db_ + ((eq_on_ || sub_on_ || tone) ? eq_peak_db() : 0.0f);
    if (width_ > 1.0f) b += 20.0f * log10f(width_);   // fully out-of-phase content, scaled by width
    headroom_db_ = b > 0.0f ? b : 0.0f;
    update_gain();
}

float DspChain::eq_peak_db() const {
    // The summed |H|² of the bands in force, evaluated in float: as a product, so one log per
    // point. 64 points on a log scale, plus each band's own f0 (where a peak's maximum is),
    // then 8 golden-section steps around the best of them — the curve between two grid points
    // is smooth, and that lands within a few hundredths of a dB of the true maximum. About as
    // much work as one block of EQ, once per edit.
    struct P { float g, k, m0, m1, m2; };
    P p[MAX_BANDS + 5];
    float f0[MAX_BANDS + 5];
    int n = 0;
    for (int i = 0; eq_on_ && i < MAX_BANDS; i++) {
        if (!want_[i]) continue;
        const SvfDesign& d = want_d_[i];
        p[n] = {(float)d.g, (float)d.k, (float)d.m0, (float)d.m1, (float)d.m2};
        f0[n] = bands_[i].freq_hz;
        n++;
    }
    // Бас and Високі, whether the EQ is on or not (set_tone).
    for (int j = 0; j < 2; j++) {
        if (!tone_sec_[j]) continue;
        const SvfDesign& d = tone_d_[j];
        p[n] = {(float)d.g, (float)d.k, (float)d.m0, (float)d.m1, (float)d.m2};
        f0[n] = j == 0 ? TONE_LO_HZ : TONE_HI_HZ;
        n++;
    }
    if (n == 0 && !sub_sec_[2]) return 0.0f;   // high-passes alone never boost anything; a port lift does
    for (int j = 0; j < 3; j++) {
        if (!sub_sec_[j]) continue;
        const SvfDesign& d = sub_d_[j];
        p[n] = {(float)d.g, (float)d.k, (float)d.m0, (float)d.m1, (float)d.m2};
        f0[n] = j == 2 ? port_hz_ : sub_fc_;
        n++;
    }
    const float fs = sample_rate_hz_;
    const float f_lo = 20.0f;
    const float f_hi = fminf(20000.0f, 0.49f * fs);
    // With the bass limiter on, what it can take away at f comes off the curve. LR4's two bands
    // are in phase, |LP4| = l = 1/(1 + (f/xo)^4) and |HP4| = h = 1 − l, so a sine at f is h of
    // the high band — the share bass_knot() blends on. Where that share is 30 % or less the stage
    // takes the bass down to its floor; above 70 % it leaves it to the brickwall; in between,
    // the same blend. A sine at f then comes out at h + l·(floor + u·(1 − floor)) of itself: a
    // bass boost it absorbs leaves the volume alone, one above its reach (150 Hz, say) counts.
    const bool bl = bass_on_;
    const float xo = bass_xo_hz_;
    if (!(f_hi > f_lo)) return 0.0f;
    const float span = logf(f_hi / f_lo);
    auto level = [&](float x) {          // x: 0 at f_lo, 1 at f_hi, log scale
        const float f = f_lo * expf(span * x);
        const float h = (float)M_PI * f / fs;
        const float S = sinf(h), C = cosf(h);
        float prod = 1.0f;
        for (int j = 0; j < n; j++) {
            const float gc = p[j].g * C;
            const float dr = gc * gc - S * S, di = p[j].k * gc * S;
            const float nr = p[j].m0 * dr + p[j].m2 * gc * gc;
            const float ni = p[j].m0 * di + p[j].m1 * gc * S;
            const float den = dr * dr + di * di;
            prod *= den > 1e-30f ? (nr * nr + ni * ni) / den : 1.0f;
        }
        if (bl) {
            const float r = f / xo, r4 = (r * r) * (r * r);
            const float l = 1.0f / (1.0f + r4), hh = 1.0f - l;
            float u = (hh - 0.3f) * 2.5f;
            u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
            const float w = hh + l * (BASS_FLOOR + u * (1.0f - BASS_FLOOR));
            prod *= w * w;
        }
        return prod;
    };
    constexpr int GRID = 64;
    const float step = 1.0f / (float)(GRID - 1);
    float best = 0.0f, best_x = 0.0f;
    for (int j = 0; j < GRID; j++) {
        const float x = (float)j * step, v = level(x);
        if (v > best) { best = v; best_x = x; }
    }
    for (int j = 0; j < n; j++) {
        if (!(f0[j] > f_lo && f0[j] < f_hi)) continue;
        const float x = logf(f0[j] / f_lo) / span, v = level(x);
        if (v > best) { best = v; best_x = x; }
    }
    float a = fmaxf(0.0f, best_x - step), b = fminf(1.0f, best_x + step);
    constexpr float GR = 0.618034f;
    float c = b - GR * (b - a), d = a + GR * (b - a);
    float fc = level(c), fd = level(d);
    for (int it = 0; it < 8; it++) {
        if (fc > fd) { b = d; d = c; fd = fc; c = b - GR * (b - a); fc = level(c); }
        else         { a = c; c = d; fc = fd; d = a + GR * (b - a); fd = level(d); }
    }
    best = fmaxf(best, fmaxf(fc, fd));
    return best > 0.0f ? 10.0f * log10f(best) : 0.0f;
}

void DspChain::Passes::add(const SvfCoef& coef, SvfState& st) {
    if (!paired) {
        svf_stereo(x, n, coef, st);
        count++;
        return;
    }
    if (!c) {
        c = &coef;
        s = &st;
        return;
    }
    svf_stereo2(x, n, *c, *s, coef, st);
    c = nullptr;
    count++;
}

void DspChain::Passes::flush() {
    if (!c) return;
    svf_stereo(x, n, *c, *s);
    c = nullptr;
    count++;
}

void DspChain::run_section(Passes& p, Band& b) {
    if (b.len) {
        p.flush();                 // what came before it runs before it
        run_band(b, p.x, p.n);
        p.count += 3;
    } else if (b.on) {
        p.add(b.c, b.st);
    }
}

void DspChain::run_band(Band& b, float* stereo, size_t frames) {
    // Crossfade: the outgoing design (or a wire) into fade_buf_, the incoming one in place,
    // blended along a smoothstep — C1 at both ends, so the change has no corner where it
    // starts or where it lands.
    memcpy(fade_buf_, stereo, frames * 2 * sizeof(float));
    if (b.old_on) svf_stereo(fade_buf_, frames, b.old_c, b.old_st);
    if (b.on) svf_stereo(stereo, frames, b.c, b.st);
    const float inv = 1.0f / (float)b.len;
    for (size_t i = 0; i < frames && b.pos < b.len; i++) {
        b.pos++;
        const float t = (float)b.pos * inv;
        const float w = t * t * (3.0f - 2.0f * t);
        const float ol = fade_buf_[2 * i], orr = fade_buf_[2 * i + 1];
        stereo[2 * i]     = std::fmaf(w, stereo[2 * i] - ol, ol);
        stereo[2 * i + 1] = std::fmaf(w, stereo[2 * i + 1] - orr, orr);
    }
    if (b.pos >= b.len) {
        b.len = 0;
        if (b.pend) {
            b.pend = false;
            if (b.pend_on != b.on || (b.on && memcmp(&b.pend_c, &b.c, sizeof(b.c)) != 0))
                begin_fade(b, b.pend_on, b.pend_c, b.fade_n ? b.fade_n : fade_len_);
        }
    }
}

void DspChain::set_width(float w) {
    if (!(w >= 0.0f)) w = 0.0f; else if (w > 2.0f) w = 2.0f;
    width_ = w;
    update_headroom();
}

void DspChain::set_mono_bass(float hz) {
    mono_bass_hz_ = hz;
    if (hz < 20.0f || hz > 500.0f) {            // outside this it is not bass management
        side_hp_on_ = false;
        return;
    }
    EqBand b;
    b.type    = BandType::HIGH_PASS;
    b.freq_hz = hz;
    b.q       = 0.7071f;                        // Butterworth: no ripple, -3 dB at hz
    b.enabled = true;
    SvfCoef c;
    if (!design_svf(b, sample_rate_hz_, c)) {
        side_hp_on_ = false;
        return;
    }
    side_c_ = c;
    // The state is kept while the corner moves: an SVF takes a moving corner in its stride,
    // and clearing it would be a step. Switching on from off starts from rest, since the
    // memory is from another time.
    if (!side_hp_on_) side_s1_ = side_s2_ = 0.0f;
    side_hp_on_ = true;
}

/// Mid/side: M = (L+R)/2, S = (L-R)/2, scale S, recombine. Skipped entirely when it would
/// be a no-op, so the default path is bit-exact rather than "almost unchanged".
void DspChain::run_stereo(float* __restrict stereo, size_t frames) {
    const float w = width_;
    if (!side_hp_on_) {
        for (size_t i = 0; i < frames; i++) {
            const float l = stereo[2 * i], r = stereo[2 * i + 1];
            const float m = (l + r) * 0.5f;
            const float sd = (l - r) * 0.5f * w;
            stereo[2 * i]     = m + sd;
            stereo[2 * i + 1] = m - sd;
        }
        return;
    }
    // Filtering S alone leaves M untouched, so the sum L+R is preserved exactly and only the
    // difference loses its bottom end — the signal goes mono downwards, with no change to total
    // energy or to tonal balance in mono.
    //
    // One channel's filter is one chain of operations from the input to the output, and on an
    // in-order core a frame waited for it end to end: 41 cycles for 24 instructions. So the
    // loop is skewed by a frame — frame i is formed and filtered while frame i−1's side is
    // scaled and written, two chains in each turn (29 cycles). Every sample meets the same
    // operations as before; only the frames they are interleaved with change.
    if (frames == 0) return;
    const SvfCoef c = side_c_;
    float s1 = side_s1_, s2 = side_s2_;
    float m = (stereo[0] + stereo[1]) * 0.5f;
    float sd = svf_tick((stereo[0] - stereo[1]) * 0.5f, c, s1, s2);
    for (size_t i = 1; i < frames; i++) {
        const float l = stereo[2 * i], r = stereo[2 * i + 1];
        const float mn = (l + r) * 0.5f;
        const float sn = svf_tick((l - r) * 0.5f, c, s1, s2);
        const float sw = sd * w;
        stereo[2 * i - 2] = m + sw;
        stereo[2 * i - 1] = m - sw;
        m = mn;
        sd = sn;
    }
    const float sw = sd * w;
    stereo[2 * frames - 2] = m + sw;
    stereo[2 * frames - 1] = m - sw;
    side_s1_ = s1;
    side_s2_ = s2;
}

void DspChain::run_gain(float* stereo, size_t frames) {
    const float target = (mute_ || xo_switching_) ? 0.0f : gain_target_;
    if (vol_gain_ == target && vol_g1_ == target) {
        if (target == 1.0f) return;                          // a wire: bit-exact, and free
        // Four at a time. One sample's multiply waits for its load and its store waits for the
        // multiply — about 8 cycles a sample, one after another; four independent ones fill
        // each other's waits.
        const size_t n = frames * 2;
        size_t i = 0;
        for (; i + 4 <= n; i += 4) {
            const float a = stereo[i] * target, b = stereo[i + 1] * target;
            const float c = stereo[i + 2] * target, d = stereo[i + 3] * target;
            stereo[i] = a;
            stereo[i + 1] = b;
            stereo[i + 2] = c;
            stereo[i + 3] = d;
        }
        for (; i < n; i++) stereo[i] *= target;
        return;
    }
    // Two one-poles in cascade (VOL_TAU_S each; MUTE_TAU_S for a mute or an unmute).
    const float k = mute_ramp_ ? mute_k_ : vol_k_;
    const float g1_was = vol_g1_, g_was = vol_gain_;
    float g1 = g1_was, g = g_was;
    for (size_t i = 0; i < frames; i++) {
        g1 += (target - g1) * k;
        g  += (g1 - g) * k;
        stereo[2 * i]     *= g;
        stereo[2 * i + 1] *= g;
    }
    // Snap once the ramp has arrived — or has stopped arriving. A one-pole in float never
    // reaches its target: it stalls where (target − g)·k rounds to nothing, which with these
    // coefficients is 7e-6 short at 48 kHz and 3e-5 at 192 kHz. A whole block without a single
    // step is that stall; what is left of the way is a −85 dB step, and 100 % is then exactly
    // 1.0 instead of a hair under it for ever.
    if ((g == g_was && g1 == g1_was) ||
        (fabsf(g - target) <= 1e-6f * target + 1e-9f && fabsf(g1 - target) <= 1e-6f * target + 1e-9f)) {
        g1 = g = target;
        mute_ramp_ = false;
    }
    vol_g1_ = g1;
    vol_gain_ = g;
}

void DspChain::process(float* stereo, size_t frames) {
    if (frames > BLOCK) frames = BLOCK;

    // The leveler, on the source as it comes: what it holds does not depend on the volume.
    lev_.process(stereo, frames);
    // Trim, volume and the EQ's headroom: from here on 1.0 is the amplifier's ceiling.
    run_gain(stereo, frames);
    if (xo_switching_ && vol_gain_ < 1e-4f) {      // −80 dB: the split can move unheard
        xo_switching_ = false;
        apply_xo(pend_xo_);
        mute_ramp_ = true;                         // and the way back up is a fade too
    }
    // The filter sections, in this order, two to a pass (Passes).
    Passes p{stereo, frames, paired_};
    for (Band& b : sub_) run_section(p, b);
    if (loud_run_ || loud_lo_t_ != 0.0f || loud_hi_t_ != 0.0f) run_loudness(p);
    for (Band& b : tone_) run_section(p, b);
    for (Band& b : band_) run_section(p, b);
    p.flush();
    passes_ = p.count;
    if (side_hp_on_ || width_ != 1.0f) run_stereo(stereo, frames);
    if (bal_l_ != 1.0f || bal_r_ != 1.0f || bal_tl_ != 1.0f || bal_tr_ != 1.0f) run_balance(stereo, frames);
    if (bass_on_) run_bass(stereo, frames);

    // ── Brickwall: a look-ahead peak limiter. No sample leaves above CEILING. ──
    //
    // The input is delayed by L = (M+1)·D frames. Every D frames (a "chunk") a knot is set: the
    // largest gain that keeps BOTH chunks it touches under the threshold, r = T/max(T, P, Pprev),
    // then the smallest of the last M+1 of those (the look-ahead), held 40 ms on the same peak,
    // released over 250 ms, and averaged over the last M knots. The gain applied to the delayed
    // signal runs in a straight line from knot to knot. Every knot touching a chunk is at most
    // T over that chunk's peak, and a straight line between two such numbers is too — so the
    // output cannot exceed T, by construction rather than by luck.
    //
    // It replaces a one-pole limiter with no look-ahead, whose gain was driven by the
    // instantaneous peak: on a bass note it moved inside every cycle (−41…−44 dB of THD+N at
    // 6–9 dB over), and what it missed went into a soft clip that bent every crest.
    const uint32_t D = lim_D_, L = lim_L_;
    float* const line = lim_line_;       // a base of its own: off `this` it is four instructions a frame
    float lg = lim_g_, ldg = lim_dg_, gmin = lim_min_, cmax = lim_cmax_;
    uint32_t pos = lim_pos_, lk = lim_k_;
    float pl = 0.0f, pr = 0.0f;
    bool bad = false;
    for (size_t i = 0; i < frames; i++) {
        const float l = stereo[2 * i], r = stereo[2 * i + 1];
        const float al = fabsf(l), ar = fabsf(r);
        // The peaks and the gain's minimum as fmax/fmin, one instruction each: as a compare and
        // a branch they were five branches a frame. The same numbers — every operand is a
        // magnitude or a gain, never −0 — unless one is a NaN; then the block is thrown away
        // below and the limiter reset, so nothing that differs survives it.
        cmax = fmaxf(cmax, fmaxf(al, ar));
        // A NaN fails every comparison: this catches one wherever it came from.
        bad |= !(al <= 1e6f) | !(ar <= 1e6f);
        const float yl = line[2 * pos] * lg, yr = line[2 * pos + 1] * lg;
        line[2 * pos] = l;
        line[2 * pos + 1] = r;
        if (++pos == L) pos = 0;
        stereo[2 * i] = yl;
        stereo[2 * i + 1] = yr;
        pl = fmaxf(pl, fabsf(yl));
        pr = fmaxf(pr, fabsf(yr));
        gmin = fminf(gmin, lg);
        lg += ldg;
        if (++lk == D) {
            lk = 0;
            // The largest gain this knot may have: it touches the chunk just read and the one
            // before it, and a straight line between knots must keep both under T.
            float m = cmax > lim_pprev_ ? cmax : lim_pprev_;
            lim_pprev_ = cmax;
            if (m < CEILING) m = CEILING;
            lim_env_.knot(CEILING / m);
            cmax = 0.0f;
            lg = lim_env_.g;
            ldg = lim_env_.dg;
        }
    }
    lim_g_ = lg; lim_dg_ = ldg; lim_min_ = gmin; lim_cmax_ = cmax;
    lim_pos_ = pos; lim_k_ = lk;
    peak_l_ = pl;
    peak_r_ = pr;

    // The filters' states, which a bad input would have poisoned for good: a NaN in an
    // integrator recirculates for ever, and an unstable design grows without bound.
    auto poisoned = [](const Band* list, int n) {
        bool p = false;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < 4; j++)
                p |= !(fabsf(list[i].st.s[j]) <= 1e6f) | (list[i].len && !(fabsf(list[i].old_st.s[j]) <= 1e6f));
        return p;
    };
    bad |= poisoned(band_, MAX_BANDS) | poisoned(sub_, 3) | poisoned(tone_, 2);
    for (int j = 0; j < 4; j++) bad |= !(fabsf(loud_lo_st_.s[j]) <= 1e6f) | !(fabsf(loud_hi_st_.s[j]) <= 1e6f);
    // The limiter's gain is at most 1 at every knot; between knots it is accumulated in float and
    // may sit an ulp above. Only a NaN, or something that is no longer a gain, is a fault.
    bad |= !(fabsf(side_s1_) <= 1e6f) | !(fabsf(side_s2_) <= 1e6f) | !(lim_g_ < 2.0f) | !(bass_g_ < 2.0f);
    for (int j = 0; j < 8; j++) bad |= !(fabsf(det_s_[j]) <= 1e6f) | !(fabsf(app_s_[j]) <= 1e6f);
    bad |= !lev_.sane();
    if (bad) {
        faults_++;
        reset();
        memset(stereo, 0, frames * 2 * sizeof(float));
        return;
    }
    // Decayed states to zero, once a block (dsp_kernels.h, svf_flush): after a pause every
    // integrator would otherwise sink into subnormals and could stay there.
    for (Band& b : band_) {
        svf_flush(b.st);
        if (b.len) svf_flush(b.old_st);
    }
    for (Band& b : sub_) {
        svf_flush(b.st);
        if (b.len) svf_flush(b.old_st);
    }
    for (Band& b : tone_) {
        svf_flush(b.st);
        if (b.len) svf_flush(b.old_st);
    }
    svf_flush(loud_lo_st_);
    svf_flush(loud_hi_st_);
    if (side_s1_ < 1e-15f && side_s1_ > -1e-15f) side_s1_ = 0.0f;
    if (side_s2_ < 1e-15f && side_s2_ > -1e-15f) side_s2_ = 0.0f;
    for (int j = 0; j < 8; j++) {
        if (det_s_[j] < 1e-15f && det_s_[j] > -1e-15f) det_s_[j] = 0.0f;
        if (app_s_[j] < 1e-15f && app_s_[j] > -1e-15f) app_s_[j] = 0.0f;
    }
}

void DspChain::run_bass(float* __restrict stereo, size_t frames) {
    // The same LR4 split twice: on the incoming signal to measure what is coming (the detector),
    // and on the signal L frames later to act on it (the applied path). One delay line of the
    // input instead of two of the bands; the arithmetic is identical, so the bands the gain is
    // set from are the bands it is applied to.
    //
    // Every state is a local for the length of the block: as members they are reached through
    // `this`, which the compiler cannot prove apart from the two buffers, so it reloaded and
    // stored all sixteen every frame — 52 memory operations in a 161-instruction loop.
    // One set of coefficients: both sections are the same Butterworth, and the low-pass is the
    // second node itself — no output mix.
    //
    // The two paths take turns, a chunk at a time — the frames up to the next knot: first the
    // detector, then the applied path. As one loop they held sixteen states, four coefficients,
    // the four chunk measures and the gain, more than the FPU's 32 registers, and the compiled
    // loop spilled a dozen of them to the stack every frame (119 instructions). Apart, each loop
    // keeps all it needs in registers: 48 and 51. Nothing is reordered that matters — the
    // detector needs nothing from the applied path, and the knot that ends a chunk is set after
    // both, from the same chunk measures, before the frame after it.
    const SvfCoef a = bass_a_;
    const float k2 = bass_2k_;
    const uint32_t D = lim_D_, L = bass_L_;
    float* __restrict const line = bass_line_;
    float da0 = det_s_[0], da1 = det_s_[1], db0 = det_s_[2], db1 = det_s_[3];
    float da2 = det_s_[4], da3 = det_s_[5], db2 = det_s_[6], db3 = det_s_[7];
    float pa0 = app_s_[0], pa1 = app_s_[1], pb0 = app_s_[2], pb1 = app_s_[3];
    float pa2 = app_s_[4], pa3 = app_s_[5], pb2 = app_s_[6], pb3 = app_s_[7];
    float g = bass_g_, dg = bass_dg_, gmin = bass_min_;
    float ph = bass_ph_, pl = bass_pl_, eh = bass_eh_, el = bass_el_;
    uint32_t pos = bass_pos_, kk = bass_k_;
    for (size_t i0 = 0; i0 < frames;) {
        const size_t i1 = i0 + (D - kk < frames - i0 ? D - kk : frames - i0);
        // detector
        for (size_t i = i0; i < i1; i++) {
            float v1, v2, u1, lpl, lpr;
            const float xl = stereo[2 * i], xr = stereo[2 * i + 1];
            svf_nodes(xl, a, da0, da1, v1, v2);
            const float apl = std::fmaf(-k2, v1, xl);
            svf_nodes(v2, a, db0, db1, u1, lpl);
            const float hpl = apl - lpl;
            svf_nodes(xr, a, da2, da3, v1, v2);
            const float apr = std::fmaf(-k2, v1, xr);
            svf_nodes(v2, a, db2, db3, u1, lpr);
            const float hpr = apr - lpr;
            ph = fmaxf(ph, fmaxf(fabsf(hpl), fabsf(hpr)));
            pl = fmaxf(pl, fmaxf(fabsf(lpl), fabsf(lpr)));
            eh = std::fmaf(hpl, hpl, std::fmaf(hpr, hpr, eh));
            el = std::fmaf(lpl, lpl, std::fmaf(lpr, lpr, el));
        }
        // applied path, L frames behind
        for (size_t i = i0; i < i1; i++) {
            float v1, v2, u1, lpdl, lpdr;
            const float xl = stereo[2 * i], xr = stereo[2 * i + 1];
            const float dl = line[2 * pos], dr = line[2 * pos + 1];
            line[2 * pos] = xl;
            line[2 * pos + 1] = xr;
            if (++pos == L) pos = 0;
            svf_nodes(dl, a, pa0, pa1, v1, v2);
            const float apdl = std::fmaf(-k2, v1, dl);
            svf_nodes(v2, a, pb0, pb1, u1, lpdl);
            svf_nodes(dr, a, pa2, pa3, v1, v2);
            const float apdr = std::fmaf(-k2, v1, dr);
            svf_nodes(v2, a, pb2, pb3, u1, lpdr);
            // y = HP + g·LP = AP + (g − 1)·LP
            stereo[2 * i]     = std::fmaf(g - 1.0f, lpdl, apdl);
            stereo[2 * i + 1] = std::fmaf(g - 1.0f, lpdr, apdr);
            g += dg;
        }
        kk += (uint32_t)(i1 - i0);
        i0 = i1;
        if (kk == D) {
            kk = 0;
            bass_knot(ph, pl, eh, el);
            eh = el = 0.0f;
            ph = pl = 0.0f;
            g = bass_env_.g;
            dg = bass_env_.dg;
            // g runs straight from knot to knot, so its smallest value is at one of them.
            gmin = fminf(gmin, fminf(bass_env_.g, bass_env_.G1));
        }
    }
    det_s_[0] = da0; det_s_[1] = da1; det_s_[2] = db0; det_s_[3] = db1;
    det_s_[4] = da2; det_s_[5] = da3; det_s_[6] = db2; det_s_[7] = db3;
    app_s_[0] = pa0; app_s_[1] = pa1; app_s_[2] = pb0; app_s_[3] = pb1;
    app_s_[4] = pa2; app_s_[5] = pa3; app_s_[6] = pb2; app_s_[7] = pb3;
    bass_g_ = g; bass_dg_ = dg; bass_min_ = gmin;
    bass_ph_ = ph; bass_pl_ = pl; bass_eh_ = eh; bass_el_ = el;
    bass_pos_ = pos; bass_k_ = kk;
}

void DspChain::bass_knot(float ph, float pl, float eh, float el) {
    // The bass gain this knot may have: with |high| + g·|low| ≤ the threshold over both chunks
    // the knot touches, a straight line of g between knots keeps it there.
    const float Ph = fmaxf(ph, bass_ph_prev_), Pl = fmaxf(pl, bass_pl_prev_);
    bass_ph_prev_ = ph;
    bass_pl_prev_ = pl;
    float req = 1.0f;
    if (Pl > 0.0f && Ph + Pl > BASS_THR) {
        // The bass alone takes the whole overshoot...
        float gb = (BASS_THR - Ph) / Pl;
        gb = gb < BASS_FLOOR ? BASS_FLOOR : gb > 1.0f ? 1.0f : gb;
        // ...unless the high band is what is loud: then a deep bass cut fixes little and is heard
        // as the bass pumping under every cymbal, while the brickwall has to act anyway. There
        // the bass is left alone and the brickwall takes it all. In between, a straight blend on
        // the high band's share of the peak: 30 % or less is the bass's job alone (a kick, a bass
        // line), 70 % or more is the brickwall's (a crash, a vocal peak).
        float u = (Ph / (Ph + Pl) - 0.3f) * 2.5f;
        u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
        req = std::fmaf(u, 1.0f - gb, gb);
    }
    if (bass_cap_ > 0.0f) {
        // The supply's side: power over the last ~50 ms, the bass turned down until the two
        // bands' RMS fit under the cap.
        bass_pwh_ += (eh / (float)lim_D_ - bass_pwh_) * bass_kp_;
        bass_pwl_ += (el / (float)lim_D_ - bass_pwl_) * bass_kp_;
        if (bass_pwl_ > 0.0f) {
            float c = (sqrtf(bass_cap_) - sqrtf(bass_pwh_)) / sqrtf(bass_pwl_);
            c = c < BASS_FLOOR ? BASS_FLOOR : c > 1.0f ? 1.0f : c;
            if (c < req) req = c;
        }
    }
    bass_env_.knot(req);
}

void KnotEnv::configure(int m, uint32_t d, float hold_s, float release_s, float fs) {
    M    = m < 1 ? 1 : m > MAX_M ? MAX_M : m;
    D    = d;
    hold = (uint32_t)lroundf(hold_s * fs / (float)d);
    kr   = 1.0f - expf(-(float)d / (release_s * fs));
    reset();
}

void KnotEnv::reset() {
    for (int j = 0; j <= M; j++) rh[j] = 1.0f;
    for (int j = 0; j < M; j++) gh[j] = 1.0f;
    rh_i = gh_i = 0;
    held = grel = 1.0f;
    cnt = 0;
    G1 = g = 1.0f;
    dg = 0.0f;
}

void KnotEnv::knot(float r) {
    rh[rh_i] = r;
    if (++rh_i == M + 1) rh_i = 0;
    float h = rh[0];
    for (int j = 1; j <= M; j++) if (rh[j] < h) h = rh[j];   // the look-ahead
    // Hold: the gain a peak asked for stays while peaks of that size keep coming — on bass
    // that is every cycle, and a gain that let go between them would move at the note's
    // frequency, which is distortion. 10^(0.5/20): within half a dB counts as the same peak.
    if (h <= held)                      { held = h; cnt = hold; }
    else if (h <= held * 1.0592537f)    { cnt = hold; h = held; }
    else if (cnt > 0)                   { cnt--; h = held; }
    else                                { held = h; }
    // Down at once (the look-ahead has already given it M knots to get there), up slowly — and
    // the rest of the way at once when the release stalls: (h − grel)·kr rounds to nothing about
    // 5e-5 short of the target in float, which would leave a released limiter at −0.0004 dB for
    // ever. The last step is a −87 dB change of gain.
    if (h < grel) {
        grel = h;
    } else {
        const float next = grel + (h - grel) * kr;
        grel = next == grel ? h : next;
    }
    if (grel > 1.0f - 1e-6f) grel = 1.0f;
    gh[gh_i] = grel;
    if (++gh_i == M) gh_i = 0;
    // A fresh sum each knot, not a running one: a running sum in float drifts.
    float sum = 0.0f;
    for (int j = 0; j < M; j++) sum += gh[j];
    const float G0 = G1;
    G1 = sum * (1.0f / (float)M);
    if (G1 > 1.0f) G1 = 1.0f;               // M ones sum to exactly M; guard the rest
    g  = G0;
    dg = (G1 - G0) / (float)D;
}

void DspChain::lim_reset() {
    memset(lim_line_, 0, sizeof(lim_line_));
    lim_env_.reset();
    lim_pos_ = lim_k_ = 0;
    lim_cmax_ = lim_pprev_ = 0.0f;
    lim_g_ = lim_min_ = 1.0f;
    lim_dg_ = 0.0f;
}

// ── ToneGen ──

void ToneGen::set_freq(float hz) {
    if (hz < 1.0f) hz = 1.0f;
    if (hz > fs_hz() * 0.45f) hz = fs_hz() * 0.45f;
    hz_ = hz;
    step_ = hz / fs_hz();
}

void ToneGen::fill_at(float* stereo, size_t frames, float amp, int mask) {
    const float al = (mask & 1) ? amp : 0.0f, ar = (mask & 2) ? amp : 0.0f;
    for (size_t i = 0; i < frames; i++) {
        const float s = sinf(2.0f * (float)M_PI * phase_);
        stereo[2 * i] = al * s;
        stereo[2 * i + 1] = ar * s;
        phase_ += step_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
    }
}

void ToneGen::fill(float* stereo, size_t frames) {
    for (size_t i = 0; i < frames; i++) {
        const float s = TONE_LEVEL * sinf(2.0f * (float)M_PI * phase_);
        stereo[2 * i] = s;
        stereo[2 * i + 1] = s;
        phase_ += step_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
    }
}

} // namespace audio_core
