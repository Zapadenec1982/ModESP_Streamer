/**
 * @file audio_pipe.cpp
 * @brief Implementation of the shared decode-to-ring stretch. See audio_pipe.h.
 */

#include "audio_pipe.h"

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

}  // namespace

uint32_t stream_bps(uint32_t reported, uint32_t rate, uint8_t ch, uint8_t bits) {
    // The kilobits trap. See the header for what it costs to miss it.
    if (reported && reported < 10000) reported *= 1000;
    if (reported) return reported;
    return rate * (uint32_t)ch * (uint32_t)bits;       // PCM reports nothing; this IS its rate
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
    if (asrc_) resampleFree(asrc_);
}

bool RateConverter::ensure(uint32_t src_rate, uint32_t dst_rate) {
    if (!dst_rate) dst_rate = PIPE_OUT_RATE;
    if (!src_rate) src_rate = dst_rate;
    ratio_ = (double)dst_rate / (double)src_rate;
    if (src_rate == dst_rate) return true;             // nothing to build; keep any bank
    if (asrc_ && rate_ == src_rate && dst_ == dst_rate) {
        resampleReset(asrc_);                          // same pair: only the history is stale
        return true;
    }
    if (asrc_) { resampleFree(asrc_); asrc_ = nullptr; rate_ = 0; dst_ = 0; }

    const int64_t t0 = esp_timer_get_time();
    // The library derives the downsampling cutoff from this rate pair and tap count.
    // Interpolation is retained only when the ratio cannot use a reduced exact bank.
    asrc_ = resampleFixedRatioInit(2, ASRC_TAPS, ASRC_FILTERS,
                                   (double)src_rate, (double)dst_rate, 0,
                                   BLACKMAN_HARRIS | INCLUDE_LOWPASS | SUBSAMPLE_INTERPOLATE);
    if (!asrc_) {
        ESP_LOGE(TAG, "resampler init failed for %lu -> %lu Hz",
                 (unsigned long)src_rate, (unsigned long)dst_rate);
        return false;
    }
    rate_ = src_rate;
    dst_  = dst_rate;
    // The heap figures are here on purpose: this is the moment the filter bank is built,
    // and the bank used to be ~31 KB of internal RAM in 192-byte pieces (see bank_calloc()
    // in components/audio_resampler/resampler.c). A bench run that wants to confirm the bank
    // left internal RAM reads this line and the "Radio: stream heap" one next to it.
    ESP_LOGI(TAG, "resampler %lu -> %lu Hz, %d phases, built in %lld ms; "
                  "internal %u, DMA %u (largest %u)",
             (unsigned long)src_rate, (unsigned long)dst_rate,
             resampleGetNumFilters(asrc_),
             (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    return true;
}

void RateConverter::free_bank() {
    if (asrc_) { resampleFree(asrc_); asrc_ = nullptr; }
    rate_ = 0;
    ratio_ = 1.0;
}

void RateConverter::reset() {
    if (asrc_) resampleReset(asrc_);
}

const float* RateConverter::process(const float* in, size_t frames, float* out, size_t out_cap,
                                    size_t& out_frames) {
    if (!asrc_ || ratio_ == 1.0 || rate_ == 0) {
        out_frames = frames;
        return in;
    }
    const ResampleResult r = resampleProcessInterleaved(asrc_, in, (int)frames, out,
                                                        (int)out_cap, ratio_);
    out_frames = r.output_generated;
    return out;
}

}  // namespace audio_core
