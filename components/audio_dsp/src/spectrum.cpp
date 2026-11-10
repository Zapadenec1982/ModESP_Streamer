/**
 * @file spectrum.cpp
 * @brief Hann window → FFT → log-spaced bands, plus the seqlock bus.
 */

#include "spectrum.h"
#include "dsp_chain.h"

#if defined(ESP_PLATFORM)
#include "esp_dsp.h"
#else
#include "dsps_fft_ref.h"      // host: plain radix-2 reference, see tests/host/mocks
#endif

#include <cmath>
#include <atomic>
#include <cstring>

namespace audio_core {
namespace {

constexpr int N    = SPECTRUM_FFT_N;
constexpr int HALF = N / 2;
/// The FFT runs on HALF complex points: the window's samples, two at a time (see analyse()).
constexpr int M    = HALF;

/// Hann window, built once: its first half, since it is symmetric. Stored rather than
/// recomputed: a thousand cosines per frame at 94 frames a second would be 96 k
/// transcendentals a second for nothing.
float g_win_rms = 0.0f;      ///< sqrt(mean(w^2)) — the scale that matches power summing
float g_half_win[HALF];
/// cos(2 pi k / N) for k = 0..N/4: every twiddle the real-input step needs, by symmetry.
float g_cos[N / 4 + 1];

void build_tables() {
    static bool built = false;
    if (built) return;
    double sq = 0.0;
    for (int i = 0; i < N; i++) {
        const float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(N - 1)));
        if (i < HALF) g_half_win[i] = w;
        sq += (double)w * (double)w;
    }
    g_win_rms = (float)sqrt(sq / (double)N);
    for (int k = 0; k <= N / 4; k++) g_cos[k] = cosf(2.0f * (float)M_PI * (float)k / (float)N);
    built = true;
}

inline float window(int i) { return i < HALF ? g_half_win[i] : g_half_win[N - 1 - i]; }

/// cos and sin of 2 pi k / N for 0 < k < N/2, from the quarter-wave table.
inline void twiddle(int k, float& c, float& s) {
    constexpr int Q = N / 4;
    if (k <= Q) { c = g_cos[k];      s = g_cos[Q - k]; }
    else        { c = -g_cos[2 * Q - k]; s = g_cos[k - Q]; }
}

bool fft_ready() {
#if defined(ESP_PLATFORM)
    static bool ok = false;
    static bool tried = false;
    if (!tried) {
        tried = true;
        // The table for N serves every FFT up to N; the analysis runs one of M = N/2.
        ok = (dsps_fft2r_init_fc32(nullptr, SPECTRUM_FFT_N) == ESP_OK);
    }
    return ok;
#else
    return true;
#endif
}

/// Bin range of band `i` at `fs`. Bands are log-spaced, so below roughly 200 Hz a band is
/// narrower than one bin and neighbouring bands read the same data — see the note in
/// spectrum.h. The alternative would be interpolating, which invents detail the transform
/// never captured.
void band_bins(float fs, int i, int& lo, int& hi) {
    const float r  = SPECTRUM_F_HI / SPECTRUM_F_LO;
    const float f0 = SPECTRUM_F_LO * powf(r, (float)i / (float)SPECTRUM_BANDS);
    const float f1 = SPECTRUM_F_LO * powf(r, (float)(i + 1) / (float)SPECTRUM_BANDS);
    const float bin_hz = fs / (float)SPECTRUM_FFT_N;
    lo = (int)(f0 / bin_hz);
    hi = (int)(f1 / bin_hz);
    if (lo < 1) lo = 1;                 // bin 0 is DC — never interesting, often an offset
    if (hi <= lo) hi = lo + 1;
    if (hi > HALF) hi = HALF;
    if (lo >= hi) lo = hi - 1;
}

} // namespace

float spectrum_band_hz(int i) {
    const float r = SPECTRUM_F_HI / SPECTRUM_F_LO;
    return SPECTRUM_F_LO * powf(r, ((float)i + 0.5f) / (float)SPECTRUM_BANDS);
}

void SpectrumAnalyzer::reset() {
    restart();
    dec_.store(1, std::memory_order_relaxed);
    seen_epoch_ = UINT32_MAX;
    last_end_ = 0;
    primed_ = false;
    for (int i = 0; i < SPECTRUM_BANDS; i++) bands_[i] = SPECTRUM_FLOOR_DB;
    // The band table is left alone: it depends on the sample rate, not on the signal, and
    // analyse() rebuilds it the moment the rate is found to have moved.
}

void SpectrumAnalyzer::restart() {
    // The epoch first: an analysis that read the old count and copies now sees it moved.
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    written_.store(0, std::memory_order_release);
    dec_k_ = 0;
    dec_acc_ = 0.0f;
}

void SpectrumAnalyzer::build_bands(float fs) {
    for (int b = 0; b < SPECTRUM_BANDS; b++) band_bins(fs, b, lo_[b], hi_[b]);
    bands_fs_ = fs;
}

void SpectrumAnalyzer::feed(const float* stereo, size_t frames) {
    const float fs = fs_hz();
    const int dec = fs > 100000.0f ? 4 : fs > 50000.0f ? 2 : 1;
    if (dec != dec_.load(std::memory_order_relaxed)) {
        // A new rate: the window in progress means nothing now.
        restart();
        dec_.store(dec, std::memory_order_release);
    }
    const float inv = 0.5f / (float)dec;
    uint32_t w = written_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frames; i++) {
        dec_acc_ += stereo[2 * i] + stereo[2 * i + 1];
        if (++dec_k_ < dec) continue;
        ring_[w % SPECTRUM_RING] = dec_acc_ * inv;
        w++;
        dec_k_ = 0;
        dec_acc_ = 0.0f;
    }
    written_.store(w, std::memory_order_release);
}

bool SpectrumAnalyzer::analyse() {
    if (!fft_ready()) return false;
    build_tables();

    const uint32_t epoch = epoch_.load(std::memory_order_acquire);
    if (epoch != seen_epoch_) {
        // The feed started over: what was on screen belongs to what played before.
        seen_epoch_ = epoch;
        primed_ = false;
        for (int i = 0; i < SPECTRUM_BANDS; i++) bands_[i] = SPECTRUM_FLOOR_DB;
    }
    const uint32_t written = written_.load(std::memory_order_acquire);
    // Windows end on a hop, as they did when the feed ran the FFT itself: the same windows,
    // so the same bars, however late this runs. The ring and the hop both divide 2^32, so the
    // positions stay right across the counter's wrap.
    static_assert((SPECTRUM_RING & (SPECTRUM_RING - 1)) == 0 && (SPECTRUM_HOP & (SPECTRUM_HOP - 1)) == 0,
                  "the ring and the hop are powers of two");
    const uint32_t end = written - written % SPECTRUM_HOP;
    if (!primed_ && written < (uint32_t)N) return false;          // not a whole window yet
    const uint32_t since = end - last_end_;
    if (primed_ && (since == 0 || since > 0x80000000u)) return false;
    const uint32_t start = end - N;

    // Window the samples straight into the FFT's buffer: sample 2m is point m's real part and
    // sample 2m + 1 its imaginary part, so a real window of N is a complex FFT of M = N/2.
    //
    // A window of exact zeros (a pause, a stopped source, the standby face with nothing
    // playing) transforms to zeros: every band reads the floor and only the decay is left to
    // do. Found by a scan that stops at the first sample that is not zero, the window, the
    // transform and the band sums are skipped for it — the same bars for a twentieth of the work.
    bool silent = true;
    for (int i = 0; i < N; i++) {
        if (ring_[(start + (uint32_t)i) % SPECTRUM_RING] != 0.0f) { silent = false; break; }
    }
    if (!silent) {
        for (int i = 0; i < N; i++) work_[i] = ring_[(start + (uint32_t)i) % SPECTRUM_RING] * window(i);
    }

    // The feed kept writing meanwhile: if it reached the window's start, or started over,
    // the copy is torn and the frame is skipped.
    const uint32_t after = written_.load(std::memory_order_acquire);
    const uint32_t hops = primed_ ? since / SPECTRUM_HOP : 1;
    last_end_ = end;
    primed_ = true;
    if (epoch_.load(std::memory_order_acquire) != epoch || after - start > (uint32_t)SPECTRUM_RING) {
        return false;
    }

    if (!silent) {
#if defined(ESP_PLATFORM)
    // The plain-C FFT, not the P4 assembly one (dsps_fft2r_fc32 → _arp4 on this chip). One
    // build died in the assembly one ~5 s in, the moment a source started: "Illegal
    // instruction", the inner loop left after 29 of its 64 passes and the PC 0x44 bytes BEFORE
    // the function. That loop runs on the core's hardware-loop unit (esp.lp.setup) against two
    // of the unit's rules, ESP32-P4 v1.3 TRM §1.7.1.5: its start and end are not word-aligned,
    // and its last instruction is a 16-bit one. The offsets are fixed inside the function, so
    // every build carries them; when an interrupt lands decides which build falls over.
    // Espressif reproduced the fault that way (esp-idf#19025) and announced an esp-dsp fix;
    // 1.8.2, the current release, does not have it.
    dsps_fft2r_fc32_ansi(work_, M);
    dsps_bit_rev_fc32(work_, M);
#else
    dsps_fft_ref(work_, M);
#endif
    }

    // Scale so a full-scale sine reads 0 dBFS. Because the bands sum POWER, the divisor is
    // the window's RMS, not its mean: Hann spreads an on-bin tone as 1, 0.5, 0.5 in
    // amplitude, so the power sum is 1.5x the peak bin and dividing by the mean would
    // over-read by sqrt(1.5) = 1.76 dB. Taken from the window itself rather than written as
    // a constant, so changing the window cannot silently break the calibration.
    const float norm = 2.0f / (g_win_rms * (float)SPECTRUM_FFT_N);
    const float offset_db = offset_db_.load(std::memory_order_relaxed);
    // Instant attack, 1.2 dB per hop decay (≈ 110 dB/s at 94 hops a second) — fast enough to
    // follow music, slow enough that the bars do not flicker between frames. A hop the
    // analysis missed still falls.
    const float decay = 1.2f * (float)(hops < 50 ? hops : 50);

    // Bin numbers are a frequency over the ANALYSED rate: the output's, divided by the
    // decimation, and the output's follows the source.
    const float fs_eff = fs_hz() / (float)dec_.load(std::memory_order_acquire);
    if (fs_eff != bands_fs_) build_bands(fs_eff);

    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        const int lo = lo_[b], hi = hi_[b];
        // Sum POWER across the band, then take the root. Taking the loudest bin instead
        // would under-read by up to 1.4 dB whenever a tone falls between bin centres (Hann
        // spreads it over three bins) — the reading would depend on the tone's frequency
        // rather than on its level. Summing is also the honest meaning of "level in this
        // band" for anything that is not a single tone.
        float power = 0.0f;
        for (int k = lo; !silent && k < hi; k++) {
            // Bin k of the real window's N-point transform from the M-point one, Z:
            // X[k] = E[k] + W^k O[k], E and O the transforms of the even and odd samples,
            // E[k] = (Z[k] + Z*[M-k]) / 2, O[k] = (Z[k] - Z*[M-k]) / 2j, W = e^(-2 pi j / N).
            // 0 < k < M here: bin 0 is never in a band and hi <= HALF = M.
            const float zr = work_[2 * k], zi = work_[2 * k + 1];
            const float cr = work_[2 * (M - k)], ci = work_[2 * (M - k) + 1];
            const float er = 0.5f * (zr + cr), ei = 0.5f * (zi - ci);
            const float orr = 0.5f * (zi + ci), oi = -0.5f * (zr - cr);
            float c, s;
            twiddle(k, c, s);
            const float re = er + orr * c + oi * s;
            const float im = ei + oi * c - orr * s;
            power += re * re + im * im;
        }
        const float mag = sqrtf(power) * norm;
        // The offset goes on before the clamps: a band that is only under the floor
        // because the volume is low has to be able to come back above it.
        float db = mag > 1e-7f ? 20.0f * log10f(mag) + offset_db : SPECTRUM_FLOOR_DB;
        if (db < SPECTRUM_FLOOR_DB) db = SPECTRUM_FLOOR_DB;
        if (db > SPECTRUM_BAND_REF_DB) db = SPECTRUM_BAND_REF_DB;
        // Falls no lower than what it reads now: subtracted blind, a steady band was published
        // a step under its level on every hop that read a hair lower than the one before.
        const float fall = bands_[b] - decay;
        bands_[b] = db > fall ? db : fall;
        if (bands_[b] < SPECTRUM_FLOOR_DB) bands_[b] = SPECTRUM_FLOOR_DB;
    }
    return true;
}

// ── bus ──

void SpectrumBus::publish(const float* db) {
    seq_++;                                    // odd: a write is in progress
    std::atomic_signal_fence(std::memory_order_seq_cst);
    memcpy(db_, db, sizeof(db_));
    std::atomic_signal_fence(std::memory_order_seq_cst);
    seq_++;                                    // even: complete
}

bool SpectrumBus::read(float* db) const {
    for (int attempt = 0; attempt < 4; attempt++) {
        const uint32_t s0 = seq_;
        if (s0 == 0) return false;             // nothing published yet
        if (s0 & 1u) continue;                 // writer mid-frame
        std::atomic_signal_fence(std::memory_order_seq_cst);
        memcpy(db, db_, sizeof(db_));
        std::atomic_signal_fence(std::memory_order_seq_cst);
        if (seq_ == s0) return true;
    }
    return false;                              // let the caller keep the previous frame
}

SpectrumBus& spectrum_bus() {
    static SpectrumBus bus;
    return bus;
}

} // namespace audio_core
