/**
 * @file audio_pipe.h
 * @brief The stretch between a decoder and the PCM ring, shared by everything that decodes.
 *
 * Two sources decode into the same ring — the card player and the radio streamer — and both
 * need the same two things in between: the decoder's integer PCM turned into interleaved
 * float stereo, and that float stereo brought to the output rate. Writing it twice would
 * mean fixing it twice, and the awkward parts (24-bit sign extension, the cost of building
 * a filter bank) are exactly the parts nobody wants to debug in two places.
 *
 * A component rather than a shared header in one of the modules, because a module may not
 * depend on another module.
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "pcm_ring.h"   // PIPE_OUT_RATE: the rate this converter is built to reach

struct resample;

namespace audio_core {

// ── how long a stream is, and where a second lives in it ─────────────────────────────────
//
// A compressed file carries no index saying where second N starts, and building one means
// decoding the whole thing. What there is, is a length in bytes and a bitrate, and the
// fraction of the track asked for is the fraction of the bytes — give or take what variable
// bitrate does to it. The card player worked this out first (a track is a file with a size);
// the stream engine needs the same arithmetic the moment a server sends Content-Length,
// which is what a media server pushing a track over DLNA does and a radio station never
// does. It lives here so there is one copy of it, including the trap in the middle.

/// The decoder's bitrate, in BITS per second, whatever unit it decided to report in.
///
/// The field is documented as bits per second and for MP3 it arrives in KILOBITS: a
/// 200 kbit/s track reports 201, which turns a four-minute song into seventy-seven hours.
/// Nothing decoded here runs below 10 kbit/s, so a number that small can only be the other
/// unit. When the decoder reports nothing — PCM does not — the rate times the frame size
/// IS the bitrate.
uint32_t stream_bps(uint32_t reported, uint32_t rate, uint8_t ch, uint8_t bits);

/// A FLAC file's length in seconds from its STREAMINFO block (`head`: the first 42 bytes of
/// the file or more), 0 if this is not the start of a FLAC or the encoder left the total out.
/// The decoder reports no bitrate for FLAC, and a length worked out as if the file were
/// uncompressed PCM is short by the compression ratio.
int32_t flac_duration_s(const uint8_t* head, size_t n);

/// How many seconds `bytes` of a stream at `bps` lasts. 0 when it cannot be known.
///
/// Tags and headers are counted as audio, which for a three-minute MP3 with artwork is a
/// second or two — visible only if you are looking for it, and far better than a progress
/// bar with no end.
int32_t stream_duration_s(uint64_t bytes, uint32_t bps);

/// The byte offset where second `sec` lives, for a stream of `bytes` bytes lasting `dur_s`.
///
/// The result is pulled back onto a frame boundary whenever the frame size is known. That
/// matters for PCM, where a byte out of step swaps the channels and turns music into noise;
/// for a compressed stream it moves the offset by at most three bytes and changes nothing,
/// because the decoder resynchronises at the next frame header it recognises — which is what
/// it does after any dropout, and the reason seeking by byte works at all.
uint64_t stream_seek_byte(uint64_t bytes, int32_t sec, int32_t dur_s, uint8_t bits, uint8_t ch);

/// Bound input by both staging and resampled output capacities (eight frames of phase margin).
inline size_t resample_input_limit(size_t input_capacity, size_t output_capacity, double ratio) {
    if (!input_capacity || output_capacity <= 8 || !(ratio > 0.0)) return 0;
    const double available = static_cast<double>(output_capacity - 8) / ratio;
    return available >= static_cast<double>(input_capacity)
        ? input_capacity : static_cast<size_t>(available);
}

/// Decoder PCM to interleaved float stereo, normalised to ±1.
///
/// Handles 8, 16, 24 and 32 bits, mono or stereo; a mono source is duplicated to both sides
/// rather than left silent on one. Returns the number of FRAMES converted, which is bounded
/// by both the bytes available and `max_frames`.
size_t pcm_to_f32(const uint8_t* src, size_t bytes, uint8_t bits, uint8_t ch, float* dst,
                  size_t max_frames);

/// Smooth startup envelope, armed with remaining = length. Leading stereo
/// digital silence must not consume it: codec delay can exceed the whole fade.
void fade_start(float* stereo, size_t frames, size_t& remaining, size_t length);

/// A fixed-ratio resampler that remembers which rate it was built for.
///
/// Building is expensive in a way that is easy to miss: the filter bank evaluates a sinc and
/// a four-term Blackman-Harris window per tap, in double, and this chip has no
/// double-precision FPU — about thirty thousand emulated transcendental calls. Doing that at
/// the start of every track, right after a switch has deliberately emptied the ring, starves
/// the audio task for as long as it takes and is heard as a crackle. Almost everything is
/// 44.1 kHz, so building per RATE rather than per track costs it once.
class RateConverter {
public:
    ~RateConverter();

    /// Makes the bank ready for `src_rate`. Cheap when the rate is unchanged — it only
    /// clears the filter history, which is what a new stream needs. False means no memory.
    /// Make the bank ready to convert `src_rate` to `dst_rate`.
    ///
    /// `dst_rate` used to be PIPE_OUT_RATE and is now a parameter, because the rate the
    /// output settles on is no longer a constant: see fallback_rate() in pcm_ring.h.
    bool ensure(uint32_t src_rate, uint32_t dst_rate);
    /// Throws away the history without touching the bank: for a seek or a reconnect, where
    /// what comes next has nothing to do with what came before.
    void reset();
    /// Gives the filter bank back. The radio holds one only while it is the source, and a
    /// bank is tens of kilobytes that nothing else can use while it sits there.
    void free_bank();

    /// Converts `frames` of interleaved stereo. When the source is already at the output
    /// rate the input is passed straight through, and `out` is not touched — so the caller
    /// must use the returned pointer rather than assume where the result is.
    const float* process(const float* in, size_t frames, float* out, size_t out_cap,
                         size_t& out_frames);

    /// The most input frames process() may be handed for an output buffer of `out_cap`.
    ///
    /// The converter makes `ratio` frames out of every frame it takes — going up from
    /// 22.05 kHz to 48 that is 2.18 of them. Handed more than the output can hold it fills
    /// the buffer, stops, and reports how much of the input it used; a caller that advances
    /// past the whole block instead loses the remainder without a sound to mark it. Asking
    /// this first turns a silent drop into one more call.
    size_t max_in(size_t out_cap) const {
        if (!stage_[0] || ratio_ <= 1.0) return out_cap;
        const size_t room = out_cap > 8 ? out_cap - 8 : 1;   // the phase can carry one over
        const size_t n = (size_t)((double)room / ratio_);
        return n ? n : 1;
    }

    uint32_t rate() const { return rate_; }
    double   ratio() const { return ratio_; }
    /// Whether a bank is built: `!conv` reads as "no converter", which is the question the
    /// card player's input bound asks before dividing by the ratio.
    explicit operator bool() const { return stage_[0] != nullptr; }

    /// Up to three stages: halvings first, then the rest. 192 kHz to 44.1 kHz is three.
    static constexpr int MAX_STAGES = 3;

private:
    void free_stages();

    resample* stage_[MAX_STAGES] = {};
    double    stage_ratio_[MAX_STAGES] = {};
    int       stages_ = 0;
    float*    mid_[2] = {};       ///< between stages, PSRAM; frames of capacity mid_cap_
    size_t    mid_cap_ = 0;
    uint32_t  rate_ = 0;          ///< the source rate the bank was built for; 0 = none
    uint32_t  dst_  = 0;          ///< and the rate it converts TO
    double    ratio_ = 1.0;
};

}  // namespace audio_core
