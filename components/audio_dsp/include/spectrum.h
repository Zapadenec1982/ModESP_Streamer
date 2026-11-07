/**
 * @file spectrum.h
 * @brief Real-time spectrum analyser and the bus that carries it to the UI.
 *
 * Why a bus and not a SharedState key: 28 band levels at 90 Hz is a small array on a fast
 * path, and SharedState is a key/value store written from the main loop. Pushing it through
 * there would mean 28 keys or a blob round-trip per frame, for data whose only consumer is
 * one widget.
 *
 * The analyser has two halves on two cores. The audio task (core 1) only feeds it: the mono
 * sum, averaged down above 50 kHz, into a ring. The analysis — window, FFT, bands — runs on a
 * task of its own on core 0, beside the UI that shows it: on core 1 it was 7.9 % of the core
 * at 96 kHz, about 0.8 ms in every tenth block, all of it inside the audio task's deadline and
 * none of it sound. The ring needs no lock: the analysis copies a window out and checks the
 * feed did not overwrite it meanwhile; a window that was is dropped, and a dropped window is
 * one frame the bars wait for.
 *
 * SpectrumBus is a seqlock: the analysis task writes, the LVGL task reads, neither ever
 * blocks the other, and a reader that catches a half-written frame retries instead of drawing
 * torn data.
 *
 * The analyser lives here rather than in the audio module so it is testable on the host,
 * where the FFT kernel is a plain reference implementation.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace audio_core {

constexpr int SPECTRUM_BANDS = 28;      ///< as the design draws
/// 1024 points at 48 kHz = 46.9 Hz per bin, with 50 % overlap giving ~94 frames a second.
/// The window size is a straight trade: it buys low-frequency resolution with internal RAM
/// (1024 costs 15 KB: the ring, the FFT's buffer and the tables) and with latency. Even here
/// the lowest bands are narrower than one bin — a log-spaced band at f is about 0.24*f wide,
/// so bands stop sharing bins only above ~200 Hz. That is inherent to any FFT analyser with
/// log bands; the display shows it rather than interpolating a resolution that is not there.
constexpr int SPECTRUM_FFT_N = 1024;
constexpr int SPECTRUM_HOP   = SPECTRUM_FFT_N / 2;
/// The feed's ring: a window, and as much again for the analysis to be late by (21 ms).
constexpr int SPECTRUM_RING  = 2 * SPECTRUM_FFT_N;
constexpr float SPECTRUM_FLOOR_DB = -60.0f;

/// Ceiling for a band level, and the level a band display should put at the top of the row.
///
/// A band is not the whole signal. A full-scale sound spread evenly across the log-spaced
/// bands leaves 10*log10(SPECTRUM_BANDS) less in each one than in the total — about 14.5 dB
/// here. Drawing a row from 0 dBFS downwards therefore wastes 14.5 dB at the top that
/// nothing can reach, while ordinary music, whose strongest bands sit near −10 dBFS, pegs
/// against it anyway: the row reads as permanently clipped without ever being full.
///
/// Referencing the top to the band figure spreads real programme material across the row
/// and leaves the last stretch for a band that genuinely is at or above full scale, which
/// EQ boost can produce. The ceiling has to move with it, or the top of the row would be
/// unreachable by construction.
constexpr float SPECTRUM_BAND_REF_DB = 14.5f;      // 10*log10(28)
static_assert(SPECTRUM_BANDS == 28, "SPECTRUM_BAND_REF_DB is 10*log10(SPECTRUM_BANDS)");
constexpr float SPECTRUM_F_LO = 40.0f;
constexpr float SPECTRUM_F_HI = 16000.0f;

/// Log-spaced band analyser over a Hann-windowed FFT of the mono sum.
///
/// feed(), restart() and set_gain_offset_db() belong to the audio task; analyse() and
/// bands_db() to the analysis task; reset() to whoever owns both while neither runs (start-up,
/// tests).
class SpectrumAnalyzer {
public:
    void reset();

    /// Feed interleaved stereo into the ring.
    ///
    /// Above 50 kHz the mono sum is averaged over 2 (≤ 100 kHz) or 4 frames first, so the FFT
    /// runs on a 44.1/48 kHz-equivalent stream at every output rate: the same ~94 frames a
    /// second and the same resolution, instead of four times the FFTs at 192 kHz for bins the
    /// display does not draw — it stops at 16 kHz.
    void feed(const float* stereo, size_t frames);
    /// Start the ring over: the rate moved, or feeding resumes after a pause. The analysis
    /// drops whatever window it holds and the bars go back to the floor.
    void restart();
    /// Frames averaged into one analysed sample at the current rate (1, 2 or 4).
    int decimation() const { return dec_.load(std::memory_order_relaxed); }

    /// The newest whole window the ring holds, if it ends past the last one analysed: at every
    /// hop (SPECTRUM_HOP analysed samples), so ~94 times a second, as long as it is called at
    /// least that often. True when the bands moved; publish them then.
    bool analyse();

    /// Band levels in dBFS, SPECTRUM_FLOOR_DB..SPECTRUM_BAND_REF_DB. Valid after analyse()
    /// returned true.
    const float* bands_db() const { return bands_; }

    /// Add `db` back to every band before it is reported.
    ///
    /// The analyser sits at the end of the chain, after the volume control, so without this
    /// it shows how loud you are LISTENING rather than what is playing. At an ordinary
    /// setting the volume stage cuts around 36 dB, which drops every band by 36 dB and
    /// flattens the whole row against the floor — the display appeared to have almost no
    /// sensitivity, and that is exactly what it was measuring.
    ///
    /// Tapping the signal before the volume stage instead would mean carrying a second copy
    /// of every block. Volume is a single scalar gain, so adding it back is the same
    /// arithmetic for none of the cost.
    ///
    /// The limiter's gain reduction is deliberately NOT compensated here: when the limiter
    /// works, that should be visible.
    void set_gain_offset_db(float db) { offset_db_.store(db, std::memory_order_relaxed); }

private:
    /// The bin range of every band at `fs`, built once per sample rate. It used to be
    /// worked out per frame — two powf() per band, 94 times a second — for a table that
    /// only changes when the output clock does.
    void build_bands(float fs);

    // ── the feed's (audio task) ──
    float ring_[SPECTRUM_RING] = {};     ///< analysed samples; sample n at n % SPECTRUM_RING
    std::atomic<uint32_t> written_{0};   ///< samples written since the last restart
    std::atomic<uint32_t> epoch_{0};     ///< restarts so far
    std::atomic<int>      dec_{1};       ///< frames averaged per analysed sample
    std::atomic<float>    offset_db_{0.0f};
    int   dec_k_ = 0;                    ///< frames averaged so far
    float dec_acc_ = 0.0f;

    // ── the analysis's ──
    float work_[SPECTRUM_FFT_N] = {};    ///< the window as 512 complex points (spectrum.cpp)
    float bands_[SPECTRUM_BANDS] = {};
    uint32_t seen_epoch_ = UINT32_MAX;
    uint32_t last_end_ = 0;              ///< where the last analysed window ended...
    bool     primed_ = false;            ///< ...once there is one in this epoch. The counts wrap
                                         ///< after ~25 h of play: compared by difference only
    int   lo_[SPECTRUM_BANDS] = {};      ///< first bin of each band, at bands_fs_
    int   hi_[SPECTRUM_BANDS] = {};      ///< one past the last
    float bands_fs_ = 0.0f;              ///< the rate the table was built for; 0 = not yet
};

/// Single-writer / single-reader seqlock carrying one frame of band levels.
class SpectrumBus {
public:
    void publish(const float* db);
    /// Copies the latest complete frame. False if nothing has ever been published or the
    /// writer kept winning — the caller simply keeps the previous frame on screen.
    bool read(float* db) const;

private:
    mutable volatile uint32_t seq_ = 0;
    float db_[SPECTRUM_BANDS] = {};
};

/// The one instance, shared by the audio core (writer) and the UI (reader).
SpectrumBus& spectrum_bus();

/// Centre frequency of band `i`, for axis labels.
float spectrum_band_hz(int i);

} // namespace audio_core
