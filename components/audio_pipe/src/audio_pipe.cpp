/**
 * @file audio_pipe.cpp
 * @brief Implementation of the shared decode-to-ring stretch. See audio_pipe.h.
 */

#include "audio_pipe.h"

#include <cstring>

#include "resampler.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace audio_core {
namespace {

const char* TAG = "AudioPipe";

// A stream has ONE rate for its whole length. 44100:48000 reduces to 147:160, so with
// enough filter phases every output sample lands exactly on one of them and the
// interpolation disappears — the library's own notes say this roughly doubles throughput
// and improves accuracy as well. 320 is comfortably past the 160 that needs.
constexpr int ASRC_TAPS    = 48;
constexpr int ASRC_FILTERS = 320;

/// One stage of a conversion: `taps` long, its low-pass at `lowpass` Hz (0: the library's).
struct StagePlan { uint32_t from, to; int taps; int lowpass; };

/// The filter for a stage that goes DOWN to `to` and is heard as it is (no halving follows).
///
/// The audible band is 20 kHz. What folds into it on the way down comes from at or above
/// `to` − 20 kHz, so the filter has to pass 20 kHz flat and be stopped there. 48 taps with the
/// library's own cut-off (0.8 of the new Nyquist) did neither: 96 → 48 was −3 dB at 18 kHz and
/// −9 at 20; 88.2 → 44.1 −7 and −18. The two exact halvings use the shortest filters measured
/// to reach both (−0.1 dB at 20 kHz, ≥ 97 dB on what folds to ≤ 20 kHz); the rest are sized from
/// the Blackman-Harris transition, about 4 × the source rate / taps wide.
StagePlan audible_down(uint32_t from, uint32_t to) {
    // At 88.2 kHz and up the library's cut-off (0.8 of the new Nyquist, ≥ 35 kHz) is far above
    // the audible band and flat across it: the ceiling's 192 → 96 keeps its filter, bit for bit.
    if (to >= 88200) return {from, to, ASRC_TAPS, 0};
    if (from == 96000 && to == 48000) return {from, to, 76, 23250};
    if (from == 88200 && to == 44100) return {from, to, 132, 21600};
    const uint32_t stop = to > 40000 ? to - 20000 : to / 2;
    if (from / 2 <= stop) return {from, to, ASRC_TAPS, (int)(to / 2)};   // nothing can fold in
    const double half_width = (double)to / 2.0 - 20000.0;
    int taps = half_width > 0 ? (int)(4.0 * from / half_width + 0.999) : 192;
    taps = (taps + 3) & ~3;                            // the library takes multiples of four
    if (taps < ASRC_TAPS) taps = ASRC_TAPS;
    if (taps > 192) taps = 192;
    return {from, to, taps, (int)(to / 2)};
}

/// The stages from `src` to `dst`: exact halvings while the result is still at or above `dst`,
/// then whatever is left. A halving that another stage follows only has to keep what the next
/// stage keeps, so the library's 48 taps do; the stage that ends where it is heard gets
/// audible_down(). Going up is one stage, as before.
int plan_stages(uint32_t src, uint32_t dst, StagePlan* out) {
    int n = 0;
    uint32_t r = src;
    auto halves = [&](uint32_t x, int at) {
        return at < RateConverter::MAX_STAGES - 1 && x % 2 == 0 && x / 2 >= dst && x != dst;
    };
    while (halves(r, n)) {
        const uint32_t half = r / 2;
        const bool heard = !halves(half, n + 1);       // no further halving cuts what it lets by
        out[n++] = heard ? audible_down(r, half) : StagePlan{r, half, ASRC_TAPS, 0};
        r = half;
    }
    if (r != dst) out[n++] = r > dst ? audible_down(r, dst) : StagePlan{r, dst, ASRC_TAPS, 0};
    return n;
}

}  // namespace

uint32_t stream_bps(uint32_t reported, uint32_t rate, uint8_t ch, uint8_t bits) {
    // The kilobits trap. See the header for what it costs to miss it.
    if (reported && reported < 10000) reported *= 1000;
    if (reported) return reported;
    return rate * (uint32_t)ch * (uint32_t)bits;       // PCM reports nothing; this IS its rate
}

int32_t flac_duration_s(const uint8_t* head, size_t n) {
    // "fLaC", a 4-byte block header whose type (low 7 bits) is 0 = STREAMINFO, then its 34
    // bytes: block and frame sizes (10), 20 bits of rate, 3 of channels, 5 of depth, 36 of
    // total samples.
    if (!head || n < 26 || memcmp(head, "fLaC", 4) != 0 || (head[4] & 0x7F) != 0) return 0;
    const uint32_t rate = ((uint32_t)head[18] << 12) | ((uint32_t)head[19] << 4) | (head[20] >> 4);
    const uint64_t total = ((uint64_t)(head[21] & 0x0F) << 32) | ((uint64_t)head[22] << 24) |
                           ((uint64_t)head[23] << 16) | ((uint64_t)head[24] << 8) | head[25];
    if (!rate || !total) return 0;
    const uint64_t secs = total / rate;
    return secs > 0x7fffffffULL ? 0 : (int32_t)secs;
}

int32_t stream_duration_s(uint64_t bytes, uint32_t bps) {
    if (!bytes || bps < 8) return 0;
    const uint64_t secs = bytes * 8u / bps;
    return secs > 0x7fffffffULL ? 0 : (int32_t)secs;   // a length that absurd is a bad bitrate
}

uint64_t stream_seek_byte(uint64_t bytes, int32_t sec, int32_t dur_s, uint8_t bits, uint8_t ch) {
    if (!bytes || dur_s <= 0) return 0;
    if (sec < 0) sec = 0;
    if (sec > dur_s - 1) sec = dur_s - 1;

    uint64_t byte = bytes * (uint64_t)sec / (uint64_t)dur_s;
    if (bits && ch) {
        const uint64_t frame = (uint64_t)(bits / 8) * ch;
        if (frame) byte -= byte % frame;
    }
    return byte;
}

size_t pcm_to_f32(const uint8_t* src, size_t bytes, uint8_t bits, uint8_t ch, float* dst,
                  size_t max_frames) {
    const size_t bps = (size_t)(bits / 8) * ch;
    if (!bps) return 0;
    size_t frames = bytes / bps;
    if (frames > max_frames) frames = max_frames;

    // Stereo 16 and 24 bits are nearly everything that plays: a loop each, chosen once, a frame
    // a pass. The general loop below decides the width and the channel for every sample, which
    // costs three times as much. Same arithmetic, same floats.
    if (ch == 2 && bits == 16) {
        const uint8_t* p = src;
        for (size_t i = 0; i < frames; i++, p += 4) {
            const int16_t l = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
            const int16_t r = (int16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
            dst[2 * i]     = (float)l / 32768.0f;
            dst[2 * i + 1] = (float)r / 32768.0f;
        }
        return frames;
    }
    if (ch == 2 && bits == 24) {
        const uint8_t* p = src;
        for (size_t i = 0; i < frames; i++, p += 6) {
            const int32_t l = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
            const int32_t r = (int32_t)((uint32_t)p[3] << 8 | (uint32_t)p[4] << 16 | (uint32_t)p[5] << 24);
            dst[2 * i]     = (float)(l >> 8) / 8388608.0f;
            dst[2 * i + 1] = (float)(r >> 8) / 8388608.0f;
        }
        return frames;
    }

    for (size_t i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            const int sc = (ch == 1) ? 0 : c;          // mono → both sides
            const uint8_t* p = src + (i * ch + (size_t)sc) * (bits / 8);
            float v = 0.0f;
            switch (bits) {
            case 16: {
                const int16_t s = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
                v = (float)s / 32768.0f;
                break;
            }
            case 24: {
                int32_t s = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 |
                                      (uint32_t)p[2] << 24);
                v = (float)(s >> 8) / 8388608.0f;      // arithmetic shift keeps the sign
                break;
            }
            case 32: {
                const int32_t s = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                                            (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
                v = (float)s / 2147483648.0f;
                break;
            }
            case 8:
                v = ((float)p[0] - 128.0f) / 128.0f;   // 8-bit PCM is unsigned
                break;
            default:
                return 0;
            }
            dst[2 * i + c] = v;
        }
    }
    return frames;
}

void fade_start(float* stereo, size_t frames, size_t& remaining, size_t length) {
    if (!length) { remaining = 0; return; }
    for (size_t i = 0; i < frames && remaining; ++i) {
        // Once started, zeros within the waveform are part of the same fade.
        if (remaining == length && stereo[2*i] == 0.0f && stereo[2*i+1] == 0.0f)
            continue;
        const float p = 1.0f - (float)remaining / (float)length;
        const float gain = p * p * (3.0f - 2.0f * p);
        stereo[2*i] *= gain;
        stereo[2*i+1] *= gain;
        --remaining;
    }
}

RateConverter::~RateConverter() {
    free_stages();
    for (float*& m : mid_) { heap_caps_free(m); m = nullptr; }
    mid_cap_ = 0;
}

void RateConverter::free_stages() {
    for (int i = 0; i < MAX_STAGES; i++) {
        if (stage_[i]) resampleFree(stage_[i]);
        stage_[i] = nullptr;
    }
    stages_ = 0;
}

bool RateConverter::ensure(uint32_t src_rate, uint32_t dst_rate) {
    if (!dst_rate) dst_rate = PIPE_OUT_RATE;
    if (!src_rate) src_rate = dst_rate;
    ratio_ = (double)dst_rate / (double)src_rate;
    if (src_rate == dst_rate) return true;             // nothing to build; keep any bank
    if (stage_[0] && rate_ == src_rate && dst_ == dst_rate) {
        reset();                                       // same pair: only the history is stale
        return true;
    }
    if (stage_[0]) { free_stages(); rate_ = 0; dst_ = 0; }

    const int64_t t0 = esp_timer_get_time();
    // Interpolation is retained only when a ratio cannot use a reduced exact bank.
    StagePlan plan[MAX_STAGES];
    const int n = plan_stages(src_rate, dst_rate, plan);
    for (int i = 0; i < n; i++) {
        stage_[i] = resampleFixedRatioInit(2, plan[i].taps, ASRC_FILTERS,
                                           (double)plan[i].from, (double)plan[i].to,
                                           plan[i].lowpass,
                                           BLACKMAN_HARRIS | INCLUDE_LOWPASS | SUBSAMPLE_INTERPOLATE);
        stage_ratio_[i] = (double)plan[i].to / (double)plan[i].from;
        if (!stage_[i]) {
            ESP_LOGE(TAG, "resampler init failed for %lu -> %lu Hz (stage %lu -> %lu)",
                     (unsigned long)src_rate, (unsigned long)dst_rate,
                     (unsigned long)plan[i].from, (unsigned long)plan[i].to);
            free_stages();
            return false;
        }
        stages_ = i + 1;
    }
    rate_ = src_rate;
    dst_  = dst_rate;
    // The heap figures are here on purpose: this is the moment the filter bank is built,
    // and the bank used to be ~31 KB of internal RAM in 192-byte pieces (see bank_calloc()
    // in components/audio_resampler/resampler.c). A bench run that wants to confirm the bank
    // left internal RAM reads this line and the "Radio: stream heap" one next to it.
    ESP_LOGI(TAG, "resampler %lu -> %lu Hz, %d stage(s), last %d taps x %d phases, built in %lld ms; "
                  "internal %u, DMA %u (largest %u)",
             (unsigned long)src_rate, (unsigned long)dst_rate, stages_,
             plan[stages_ - 1].taps, resampleGetNumFilters(stage_[stages_ - 1]),
             (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return true;
}

void RateConverter::free_bank() {
    free_stages();
    for (float*& m : mid_) { heap_caps_free(m); m = nullptr; }
    mid_cap_ = 0;
    rate_ = 0;
    ratio_ = 1.0;
}

void RateConverter::reset() {
    for (int i = 0; i < stages_; i++) resampleReset(stage_[i]);
}

const float* RateConverter::process(const float* in, size_t frames, float* out, size_t out_cap,
                                    size_t& out_frames) {
    if (!stage_[0] || ratio_ == 1.0 || rate_ == 0) {
        out_frames = frames;
        return in;
    }
    if (stages_ > 1 && mid_cap_ < frames + 16) {
        // Between stages every one goes down, so a stage never makes more than it was given.
        const size_t cap = frames + 16;
        for (float*& m : mid_) {
            heap_caps_free(m);
            m = (float*)heap_caps_malloc(cap * 2 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (!mid_[0] || !mid_[1]) { out_frames = 0; mid_cap_ = 0; return out; }
        mid_cap_ = cap;
    }
    const float* cur = in;
    size_t n = frames;
    for (int i = 0; i < stages_; i++) {
        const bool last = i == stages_ - 1;
        float* dst = last ? out : mid_[i & 1];
        const size_t cap = last ? out_cap : mid_cap_;
        const ResampleResult r = resampleProcessInterleaved(stage_[i], cur, (int)n, dst,
                                                            (int)cap, stage_ratio_[i]);
        n = r.output_generated;
        cur = dst;
    }
    out_frames = n;
    return out;
}

}  // namespace audio_core
