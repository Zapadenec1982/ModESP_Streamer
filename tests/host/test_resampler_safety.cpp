#include "doctest.h"
#include "resampler.h"
#include "audio_pipe.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {
void resampler_alloc_fail_at(int);
int resampler_alloc_live(void);
int resampler_alloc_calls(void);
Resample* alloc_resampleInit(int, int, int, double, int);
void alloc_resampleFree(Resample*);
}

namespace {
using Context = std::unique_ptr<Resample, decltype(&resampleFree)>;
constexpr double pi = 3.14159265358979323846;

// End the pointer table exactly at an inaccessible page. This catches even a
// discarded endpoint+1 read (whose interpolation weight would have been zero).
struct GuardedFilters {
    Resample* r;
    artsample_t** original;
    void* allocation = nullptr;
    explicit GuardedFilters(Resample* context) : r(context), original(r->filters) {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        const size_t bytes = (r->numFilters + 1) * sizeof(artsample_t*);
        const size_t accessible = (bytes + info.dwPageSize - 1) / info.dwPageSize * info.dwPageSize;
        allocation = VirtualAlloc(nullptr, accessible + info.dwPageSize,
                                  MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        REQUIRE(allocation != nullptr);
        DWORD previous;
        REQUIRE(VirtualProtect(static_cast<char*>(allocation) + accessible,
                               info.dwPageSize, PAGE_NOACCESS, &previous) != 0);
        r->filters = reinterpret_cast<artsample_t**>(static_cast<char*>(allocation) + accessible - bytes);
        std::copy_n(original, r->numFilters + 1, r->filters);
#endif
    }
    ~GuardedFilters() {
        r->filters = original;
#ifdef _WIN32
        if (allocation) VirtualFree(allocation, 0, MEM_RELEASE);
#endif
    }
};

Context context(int flags, double lowpass = 1.0, int filters = 320) {
    Context r(resampleInit(2, 48, filters, lowpass,
                          BLACKMAN_HARRIS | SUBSAMPLE_INTERPOLATE | flags), resampleFree);
    REQUIRE(r != nullptr);
    return r;
}

// Use the actual process APIs, including their double -> float phase conversion.
// A ramp-filled history makes the last filter observably different from the first.
void endpoint(bool interleaved, int flags, double phase, int filters) {
    auto r = context(flags, 0.8, filters);
    GuardedFilters guard(r.get());
    r->outputOffset = phase;
    r->inputIndex = 128;
    for (int i = 0; i < r->numSamples; ++i) {
        r->buffers[0][i] = 0.01f * i;
        r->buffers[1][i] = -0.01f * i;
    }
    const int base = static_cast<int>(phase);
    double want = 0;
    for (int i = 0; i < r->numTaps; ++i)
        want += r->filters[r->numFilters][i] * r->buffers[0][base - 23 + i];
    float out[4] = {0, 0, 12345, 12345};
    float* planar[] = {out, out + 1};
    const auto result = interleaved
        ? resampleProcessInterleaved(r.get(), nullptr, 0, out, 1, 1.0)
        : resampleProcess(r.get(), nullptr, 0, planar, 1, 1.0);
    REQUIRE(result.output_generated == 1);
    CHECK(result.input_used == 0);
    CHECK(out[0] == doctest::Approx(want).epsilon(1e-6));
    CHECK(out[1] == doctest::Approx(-want).epsilon(1e-6));
    CHECK(out[2] == 12345);
    CHECK(out[3] == 12345);
}

void ppm_stream(bool interleaved, int flags, int ppm, bool variable) {
    auto r = context(flags, 0.98, 48); // deployed Bluetooth ASRC bank
    constexpr int frames = 48000;
    std::vector<float> input(frames * 2), left(frames), right(frames);
    const double omega = 2 * pi * 1000 / 48000;
    for (int i = 0; i < frames; ++i) {
        left[i] = input[2*i] = static_cast<float>(0.4 * std::sin(omega * i));
        right[i] = input[2*i+1] = -left[i];
    }
    const int chunks[] = {1, 17, 1024, 719, 3};
    const int caps[] = {1, 7, 131, 1024, 3};
    int consumed = 0, call = 0, produced = 0;
    double position = -24, worst = 0;
    while (consumed < frames) {
        REQUIRE(call < frames * 3);
        const int n = std::min(frames - consumed, chunks[call % 5]);
        const int cap = caps[call % 5];
        const double ratio = 1.0 + 1e-6 * (variable && (call / 11) % 2 ? -ppm : ppm);
        float out[2050], l[1025], h[1025];
        std::fill_n(out, 2050, 12345);
        std::fill_n(l, 1025, 12345);
        std::fill_n(h, 1025, 12345);
        const float* in[] = {left.data() + consumed, right.data() + consumed};
        float* planar[] = {l, h};
        const auto step = interleaved
            ? resampleProcessInterleaved(r.get(), input.data() + 2*consumed, n, out, cap, ratio)
            : resampleProcess(r.get(), in, n, planar, cap, ratio);
        REQUIRE(step.input_used <= static_cast<unsigned>(n));
        REQUIRE(step.output_generated <= static_cast<unsigned>(cap));
        REQUIRE(step.input_used + step.output_generated > 0);
        CHECK(out[2*cap] == 12345);
        CHECK(l[cap] == 12345);
        CHECK(h[cap] == 12345);
        for (unsigned j = 0; j < step.output_generated; ++j, ++produced) {
            const float a = interleaved ? out[2*j] : l[j];
            const float b = interleaved ? out[2*j+1] : h[j];
            REQUIRE(std::isfinite(a));
            REQUIRE(std::isfinite(b));
            CHECK(a == -b);
            if (produced > 96)
                worst = std::max(worst, std::fabs(a - 0.4 * std::sin(omega * position)));
            position += 1.0 / ratio;
        }
        consumed += step.input_used;
        ++call;
    }
    CHECK(produced > frames - 100);
    CHECK(produced < frames + 100);
    CHECK(worst < 0.0001);
}

double converted_rms(audio_core::RateConverter& converter, int src, int dst, double hz,
                     bool unfiltered = false) {
    REQUIRE(converter.ensure(src, dst));
    Context reference(unfiltered ? resampleFixedRatioInit(2, 48, 320, src, dst, 0,
                      BLACKMAN_HARRIS | SUBSAMPLE_INTERPOLATE) : nullptr, resampleFree);
    if (unfiltered) REQUIRE(reference != nullptr);
    constexpr int frames = 16384;
    std::vector<float> input(frames*2), output;
    for (int i = 0; i < frames; ++i)
        input[2*i] = input[2*i+1] = static_cast<float>(0.5 * std::sin(2*pi*hz*i/src));
    for (int done = 0; done < frames;) {
        float scratch[2048];
        const int n = std::min(frames-done, static_cast<int>(converter.max_in(1024)));
        size_t count = 0;
        const float* result;
        if (unfiltered) {
            const auto step = resampleProcessInterleaved(reference.get(), input.data()+2*done,
                                                        n, scratch, 1024, double(dst)/src);
            REQUIRE(step.input_used == static_cast<unsigned>(n));
            count = step.output_generated;
            result = scratch;
        } else result = converter.process(input.data()+2*done, n, scratch, 1024, count);
        REQUIRE(count <= 1024);
        for (size_t j = 0; j < count; ++j) output.push_back(result[2*j]);
        done += n;
    }
    REQUIRE(output.size() > 1024);
    double power = 0;
    for (size_t i = 128; i < output.size()-128; ++i) power += output[i]*output[i];
    return std::sqrt(power / (output.size()-256));
}
}

TEST_CASE("resampler safety: rounded endpoint through both process APIs") {
    for (bool interleaved : {false, true})
        for (int flags : {0, EXTEND_CONVOLUTION_MATH})
            for (int filters : {1, 3, 48, 320, 1024}) {
                CAPTURE(interleaved);
                CAPTURE(flags);
                CAPTURE(filters);
                endpoint(interleaved, flags, 48.0 + (1.0 - 1e-10), filters);
                endpoint(interleaved, flags, 48.0 + std::nextafter(1.0f, 0.0f), filters);
            }
}

TEST_CASE("resampler safety: constant and changing ppm retain the signal") {
    for (bool interleaved : {false, true})
        for (int flags : {0, EXTEND_CONVOLUTION_MATH})
            for (int ppm : {-500, -50, -40, -1, 1, 40, 50, 500})
                for (bool variable : {false, true}) {
                    CAPTURE(interleaved);
                    CAPTURE(flags);
                    CAPTURE(ppm);
                    CAPTURE(variable);
                    ppm_stream(interleaved, flags, ppm, variable);
                }
}

TEST_CASE("resampler safety: every init allocation failure releases partial state") {
    resampler_alloc_fail_at(0);
    Resample* r = alloc_resampleInit(3, 48, 320, 1.0, SUBSAMPLE_INTERPOLATE);
    REQUIRE(r != nullptr);
    const int allocations = resampler_alloc_calls();
    alloc_resampleFree(r);
    REQUIRE(resampler_alloc_live() == 0);
    REQUIRE(allocations == 8); // context, bank, index, scratch, channels, 3 histories
    for (int fail = 1; fail <= allocations; ++fail) {
        CAPTURE(fail);
        resampler_alloc_fail_at(fail);
        CHECK(alloc_resampleInit(3, 48, 320, 1.0, SUBSAMPLE_INTERPOLATE) == nullptr);
        CHECK(resampler_alloc_live() == 0);
    }
    resampler_alloc_fail_at(0);
    CHECK(alloc_resampleInit(0, 48, 320, 1.0, 0) == nullptr);
    CHECK(alloc_resampleInit(2, 47, 320, 1.0, 0) == nullptr);
    CHECK(alloc_resampleInit(2, 48, 0, 1.0, 0) == nullptr);
    CHECK(resampler_alloc_live() == 0);
    r = alloc_resampleInit(2, 48, 320, 1.0, SUBSAMPLE_INTERPOLATE);
    REQUIRE(r != nullptr);
    alloc_resampleFree(r);
    CHECK(resampler_alloc_live() == 0);
}

TEST_CASE("resampler safety: shared RateConverter suppresses downsampling aliases") {
    audio_core::RateConverter converter;
    // 96001 needs the general interpolated bank; 96000 reduces to one filter.
    for (int source : {96000, 96001}) {
        CAPTURE(source);
        const double pass = converted_rms(converter, source, 48000, 1000);
        const double pass10k = converted_rms(converter, source, 48000, 10000);
        const double stop = converted_rms(converter, source, 48000, 30000);
        const double reference = converted_rms(converter, source, 48000, 30000, true);
        CAPTURE(pass);
        CAPTURE(pass10k);
        CAPTURE(stop);
        CAPTURE(reference);
        INFO("alias suppression dB: " << 20 * std::log10(reference / stop));
        CHECK(pass == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.01));
        CHECK(pass10k == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.01));
        CHECK(stop / reference < 0.0001); // >80 dB versus the unfiltered 18 kHz alias
    }
}

TEST_CASE("resampler safety: equal-rate stream bypasses a cached conversion bank") {
    audio_core::RateConverter converter;
    REQUIRE(converter.ensure(96000, 48000));
    REQUIRE(converter.ensure(48000, 48000));
    const float in[] = {0.2f, -0.3f};
    float out[4] = {12345, 12345, 12345, 12345};
    size_t count = 0;
    CHECK(converter.process(in, 1, out, 2, count) == in);
    CHECK(count == 1);
    CHECK(out[0] == 12345);
    REQUIRE(converter.ensure(96000, 48000));
    CHECK(converter.process(in, 1, out, 2, count) == out);
}

TEST_CASE("resampler safety: required-input query fills variable-ratio production blocks") {
    // Exercise the production sizing contract (+2 rounding headroom) across
    // near-unity tracking, +/-2% capture, and rate conversion, including wraps.
    for (bool interleaved : {false, true}) {
        auto r = context(0, 0.98, 48);
        const double ratios[] = {0.49, 0.5, 0.98, 0.9995, 0.999999,
                                 1.0, 1.000001, 1.0005, 1.02, 2.0, 4.0};
        const int capacities[] = {1, 17, 240, 1024, 4096};
        int produced = 0;
        for (int call = 0; call < 330; ++call) {
            const double ratio = ratios[call % 11];
            const int capacity = capacities[call % 5];
            const unsigned required = resampleGetRequiredSamples(r.get(), capacity, ratio);
            const unsigned supplied = required + 2;
            CAPTURE(interleaved);
            CAPTURE(ratio);
            CAPTURE(capacity);
            REQUIRE(supplied < 20000);
            std::vector<float> input(supplied * 2, 0.125f), output(capacity * 2 + 2, 12345);
            std::vector<float> l(capacity + 1, 12345), h(capacity + 1, 12345);
            const float* planar_in[] = {input.data(), input.data() + supplied};
            float* planar_out[] = {l.data(), h.data()};
            const auto result = interleaved
                ? resampleProcessInterleaved(r.get(), input.data(), supplied, output.data(), capacity, ratio)
                : resampleProcess(r.get(), planar_in, supplied, planar_out, capacity, ratio);
            REQUIRE(result.output_generated == static_cast<unsigned>(capacity));
            REQUIRE(result.input_used <= supplied);
            CHECK(std::abs(static_cast<int>(result.input_used) - static_cast<int>(required)) <= 1);
            CHECK(output[capacity*2] == 12345);
            CHECK(l[capacity] == 12345);
            CHECK(h[capacity] == 12345);
            for (int i = 0; i < capacity; ++i) {
                const float a = interleaved ? output[2*i] : l[i];
                REQUIRE(std::isfinite(a));
                if (produced++ > 96) CHECK(a == doctest::Approx(0.125f).epsilon(0.0001));
            }
        }
        CHECK(resampleGetRequiredSamples(r.get(), 0, 1.0) == 0);
        CHECK(resampleGetRequiredSamples(r.get(), -1, 1.0) == 0);
    }
}

TEST_CASE("resampler safety: float interpolation agrees with precise convolution") {
    auto ordinary = context(0, 0.98, 48);
    auto precise = context(EXTEND_CONVOLUTION_MATH, 0.98, 48);
    // Broadband bounded input, not just the easy low-frequency passband. The
    // coefficients and convolution order stay unchanged by the float return fix.
    uint32_t random = 0x12345678;
    for (int i = 0; i < ordinary->numSamples; ++i) {
        random = random * 1664525u + 1013904223u;
        ordinary->buffers[0][i] = precise->buffers[0][i] =
            (static_cast<double>(random) / 4294967296.0 - 0.5) * 1.8;
    }
    double worst = 0;
    for (int i = 0; i <= 4096; ++i) {
        const float fraction = static_cast<float>(i) / 4096;
        const int base = 24 + (i % (ordinary->numSamples - 48));
        const float a = ordinary->subsample(ordinary.get(), ordinary->buffers[0], base, fraction);
        const float b = precise->subsample(precise.get(), precise->buffers[0], base, fraction);
        REQUIRE(std::isfinite(a));
        worst = std::max(worst, std::fabs(double(a) - b));
    }
    CAPTURE(worst);
    CHECK(worst < 0.000001); // below -120 dBFS peak difference
}

TEST_CASE("resampler safety: Q32 phase tracks legacy planar samples and long-term position") {
    // The planar API retains the legacy double phase loop. Drive each API with
    // its independently consumed input so a one-frame boundary decision cannot
    // hide a duplicate/drop. Include the 4096-output fast-path limit and fallback.
    for (int flags : {0, EXTEND_CONVOLUTION_MATH}) {
        for (double base : {0.5, 1.0, 2.0, 4.0}) {
            CAPTURE(flags);
            CAPTURE(base);
            auto fast = context(flags, 0.98, 48);
            auto legacy = context(flags, 0.98, 48);
            uint64_t fast_used = 0, legacy_used = 0;
            long double expected_position = 0;
            double worst_sample = 0, worst_position = 0;
            const int capacities[] = {1, 17, 240, 1024, 4096, 4097};
            const double correction[] = {0.98, 0.9995, 0.999999, 1.0,
                                         1.000001, 1.0005, 1.02};
            for (int call = 0; call < 630; ++call) {
                const int capacity = capacities[call % 6];
                const double ratio = base * correction[call % 7];
                const unsigned nf = resampleGetRequiredSamples(fast.get(), capacity, ratio) + 2;
                const unsigned nl = resampleGetRequiredSamples(legacy.get(), capacity, ratio) + 2;
                REQUIRE(nf < 10000);
                REQUIRE(nl < 10000);
                std::vector<float> input(nf * 2), left(nl), right(nl);
                auto sample = [](uint64_t i) {
                    // 20 kHz plus a low tone at 96 kHz: high phase sensitivity.
                    return float(0.35 * std::sin(2*pi*20000*double(i)/96000) +
                                 0.1 * std::cos(2*pi*997*double(i)/96000));
                };
                for (unsigned i = 0; i < nf; ++i) {
                    input[2*i] = sample(fast_used+i);
                    input[2*i+1] = -input[2*i];
                }
                for (unsigned i = 0; i < nl; ++i) {
                    left[i] = sample(legacy_used+i);
                    right[i] = -left[i];
                }
                std::vector<float> output(capacity*2+2, 12345);
                std::vector<float> out_l(capacity+1, 12345), out_r(capacity+1, 12345);
                const float* planar_input[] = {left.data(), right.data()};
                float* planar_output[] = {out_l.data(), out_r.data()};
                const auto f = resampleProcessInterleaved(fast.get(), input.data(), nf,
                                                          output.data(), capacity, ratio);
                const auto l = resampleProcess(legacy.get(), planar_input, nl,
                                               planar_output, capacity, ratio);
                REQUIRE(f.output_generated == unsigned(capacity));
                REQUIRE(l.output_generated == unsigned(capacity));
                REQUIRE(f.input_used <= nf);
                REQUIRE(l.input_used <= nl);
                fast_used += f.input_used;
                legacy_used += l.input_used;
                expected_position += static_cast<long double>(capacity) / ratio;
                const long double actual_position = fast_used +
                    static_cast<long double>(resampleGetPosition(fast.get()));
                worst_position = std::max(worst_position,
                    double(std::fabs(actual_position-expected_position)));
                CHECK(output[capacity*2] == 12345);
                CHECK(out_l[capacity] == 12345);
                CHECK(out_r[capacity] == 12345);
                for (int i = 0; i < capacity; ++i) {
                    REQUIRE(std::isfinite(output[2*i]));
                    CHECK(output[2*i] == -output[2*i+1]);
                    worst_sample = std::max(worst_sample,
                        std::fabs(double(output[2*i]) - out_l[i]));
                }
            }
            CAPTURE(worst_sample);
            CAPTURE(worst_position);
            CHECK(worst_sample < 0.000002); // <-114 dBFS peak, including a 20 kHz tone
            CHECK(worst_position < 0.000001); // no accumulated Q32 clock drift
        }
    }
}

namespace {
/// The loudest spectral line under 20 kHz in what `hz` at `src` became at `dst`, in dB against
/// a full-scale-0.5 sine's line.
double inband_spur_db(audio_core::RateConverter& converter, int src, int dst, double hz) {
    REQUIRE(converter.ensure(src, dst));
    constexpr int frames = 32768;
    std::vector<float> input(frames * 2), output;
    for (int i = 0; i < frames; ++i)
        input[2*i] = input[2*i+1] = static_cast<float>(0.5 * std::sin(2*pi*hz*i/src));
    for (int done = 0; done < frames;) {
        float scratch[2048];
        const int n = std::min(frames - done, static_cast<int>(converter.max_in(1024)));
        size_t count = 0;
        const float* result = converter.process(input.data() + 2*done, n, scratch, 1024, count);
        for (size_t j = 0; j < count; ++j) output.push_back(result[2*j]);
        done += n;
    }
    constexpr int N = 4096;
    REQUIRE(output.size() > N + 512);
    const float* x = output.data() + 256;
    double wsum = 0;
    for (int i = 0; i < N; ++i) wsum += 0.5 - 0.5 * std::cos(2*pi*i/N);
    double worst = 0;
    const int top = static_cast<int>(20000.0 * N / dst);
    for (int k = 1; k <= top; ++k) {
        double re = 0, im = 0;
        for (int i = 0; i < N; ++i) {
            const double w = (0.5 - 0.5 * std::cos(2*pi*i/N)) * x[i];
            re += w * std::cos(2*pi*k*i/N);
            im -= w * std::sin(2*pi*k*i/N);
        }
        worst = std::max(worst, std::sqrt(re*re + im*im) * 2 / wsum);
    }
    return 20 * std::log10(worst / 0.5 + 1e-30);
}
}  // namespace

TEST_CASE("resampler safety: a conversion down keeps the audible band and stops what folds into it") {
    // Every way down the producers can take inside a session. Flat to 20 kHz within half a dB,
    // and a tone that would fold to 18 kHz at least 90 dB down.
    struct Pair { int src, dst; };
    const Pair pairs[] = {{96000, 48000}, {88200, 44100}, {96000, 44100}, {88200, 48000},
                          {192000, 48000}, {176400, 44100}, {192000, 44100}, {48000, 44100},
                          {192000, 96000}};
    const double full = 0.5 / std::sqrt(2.0);
    for (const Pair& p : pairs) {
        audio_core::RateConverter converter;
        CAPTURE(p.src);
        CAPTURE(p.dst);
        double at[5] = {}, *a = at;
        for (double hz : {1000.0, 10000.0, 16000.0, 18000.0, 20000.0}) {
            CAPTURE(hz);
            const double db = 20 * std::log10(converted_rms(converter, p.src, p.dst, hz) / full);
            *a++ = db;
            CHECK(db > -0.5);
            CHECK(db < 0.2);
        }
        double worst_fold = -400;
        // Tones the output cannot carry: whatever of them is left must lie above 20 kHz. Read
        // as the loudest DFT bin under 20 kHz of a Hann-windowed stretch of the output.
        for (double fold : {p.dst - 18000.0, p.dst - 10000.0, p.dst * 0.75}) {
            if (fold <= p.dst / 2.0 || fold >= p.src / 2.0 - 500) continue;
            CAPTURE(fold);
            const double spur = inband_spur_db(converter, p.src, p.dst, fold);
            worst_fold = std::max(worst_fold, spur);
            CHECK(spur < -90);
        }
        MESSAGE(p.src << " -> " << p.dst << ": 16 kHz " << at[2] << " dB, 18 kHz " << at[3]
                << " dB, 20 kHz " << at[4] << " dB; worst fold under 20 kHz " << worst_fold << " dB");
    }
}
