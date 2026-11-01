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

constexpr int HALF = SPECTRUM_FFT_N / 2;

/// Hann window, built once. Stored rather than recomputed: a thousand cosines per frame at
/// 94 frames a second would be 96 k transcendentals a second on the audio task for nothing.
float g_win_rms = 0.0f;      ///< sqrt(mean(w^2)) — the scale that matches power summing

const float* hann() {
    static float w[SPECTRUM_FFT_N];
    static bool built = false;
    if (!built) {
        double sq = 0.0;
        for (int i = 0; i < SPECTRUM_FFT_N; i++) {
            w[i] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i / (float)(SPECTRUM_FFT_N - 1)));
            sq += (double)w[i] * (double)w[i];
        }
        g_win_rms = (float)sqrt(sq / (double)SPECTRUM_FFT_N);
        built = true;
    }
    return w;
}

bool fft_ready() {
#if defined(ESP_PLATFORM)
    static bool ok = false;
    static bool tried = false;
    if (!tried) {
        tried = true;
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
    fill_ = 0;
    dec_k_ = 0;
    dec_acc_ = 0.0f;
    for (int i = 0; i < SPECTRUM_BANDS; i++) bands_[i] = SPECTRUM_FLOOR_DB;
    // The band table is left alone: it depends on the sample rate, not on the signal, and
    // analyse() rebuilds it the moment the rate is found to have moved.
}

void SpectrumAnalyzer::build_bands(float fs) {
    for (int b = 0; b < SPECTRUM_BANDS; b++) band_bins(fs, b, lo_[b], hi_[b]);
    bands_fs_ = fs;
}

bool SpectrumAnalyzer::feed(const float* stereo, size_t frames) {
    bool produced = false;
    const float fs = fs_hz();
    const int dec = fs > 100000.0f ? 4 : fs > 50000.0f ? 2 : 1;
    if (dec != dec_) {                   // a new rate: the window in progress means nothing now
        dec_ = dec;
        dec_k_ = 0;
        dec_acc_ = 0.0f;
        fill_ = 0;
    }
    const float inv = 0.5f / (float)dec;
    for (size_t i = 0; i < frames; i++) {
        dec_acc_ += stereo[2 * i] + stereo[2 * i + 1];
        if (++dec_k_ < dec) continue;
        acc_[fill_++] = dec_acc_ * inv;
        dec_k_ = 0;
        dec_acc_ = 0.0f;
        if (fill_ == SPECTRUM_FFT_N) {
            analyse();
            // 50 % overlap: keep the second half as the first half of the next window, so
            // the display updates twice per window without doubling the FFT rate.
            memmove(acc_, acc_ + SPECTRUM_HOP, SPECTRUM_HOP * sizeof(float));
            fill_ = SPECTRUM_HOP;
            produced = true;
        }
    }
    return produced;
}

void SpectrumAnalyzer::analyse() {
    if (!fft_ready()) return;

    const float* w = hann();
    for (int i = 0; i < SPECTRUM_FFT_N; i++) {
        cplx_[2 * i]     = acc_[i] * w[i];
        cplx_[2 * i + 1] = 0.0f;
    }

#if defined(ESP_PLATFORM)
    // The plain-C FFT, not the P4 assembly one (dsps_fft2r_fc32 → _arp4 on this chip). On
    // 25.09.2026 one build died in the assembly one ~5 s in, the moment a source started:
    // "Illegal instruction", the inner loop left after 29 of its 64 passes and the PC 0x44 bytes
    // BEFORE the function. That
    // loop runs on the core's hardware-loop unit (esp.lp.setup) against two of the unit's rules,
    // ESP32-P4 v1.3 TRM §1.7.1.5: its start and end are not word-aligned, and its last
    // instruction is a 16-bit one. The offsets are fixed inside the function, so every build
    // carries them; when an interrupt lands decides which build falls over. Espressif
    // reproduced the fault that way (esp-idf#19025) and announced an esp-dsp fix; 1.8.2, the
    // current release, does not have it.
    dsps_fft2r_fc32_ansi(cplx_, SPECTRUM_FFT_N);
    dsps_bit_rev_fc32(cplx_, SPECTRUM_FFT_N);
#else
    dsps_fft_ref(cplx_, SPECTRUM_FFT_N);
#endif

    // Scale so a full-scale sine reads 0 dBFS. Because the bands sum POWER, the divisor is
    // the window's RMS, not its mean: Hann spreads an on-bin tone as 1, 0.5, 0.5 in
    // amplitude, so the power sum is 1.5x the peak bin and dividing by the mean would
    // over-read by sqrt(1.5) = 1.76 dB. Taken from the window itself rather than written as
    // a constant, so changing the window cannot silently break the calibration.
    const float norm = 2.0f / (g_win_rms * (float)SPECTRUM_FFT_N);

    // Bin numbers are a frequency over the ANALYSED rate: the output's, divided by the
    // decimation, and the output's follows the source.
    const float fs_eff = fs_hz() / (float)dec_;
    if (fs_eff != bands_fs_) build_bands(fs_eff);

    for (int b = 0; b < SPECTRUM_BANDS; b++) {
        const int lo = lo_[b], hi = hi_[b];
        // Sum POWER across the band, then take the root. Taking the loudest bin instead
        // would under-read by up to 1.4 dB whenever a tone falls between bin centres (Hann
        // spreads it over three bins) — the reading would depend on the tone's frequency
        // rather than on its level. Summing is also the honest meaning of "level in this
        // band" for anything that is not a single tone.
        float power = 0.0f;
        for (int k = lo; k < hi; k++) {
            const float re = cplx_[2 * k], im = cplx_[2 * k + 1];
            power += re * re + im * im;
        }
        const float mag = sqrtf(power) * norm;
        // The offset goes on before the clamps: a band that is only under the floor
        // because the volume is low has to be able to come back above it.
        float db = mag > 1e-7f ? 20.0f * log10f(mag) + offset_db_ : SPECTRUM_FLOOR_DB;
        if (db < SPECTRUM_FLOOR_DB) db = SPECTRUM_FLOOR_DB;
        if (db > SPECTRUM_BAND_REF_DB) db = SPECTRUM_BAND_REF_DB;
        // Instant attack, 1.2 dB per frame decay (≈ 110 dB/s at 94 frames a second) — fast
        // enough to follow music, slow enough that the bars do not flicker between frames.
        bands_[b] = db > bands_[b] ? db : bands_[b] - 1.2f;
        if (bands_[b] < SPECTRUM_FLOOR_DB) bands_[b] = SPECTRUM_FLOOR_DB;
    }
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
