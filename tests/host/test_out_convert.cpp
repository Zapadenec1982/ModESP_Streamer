/**
 * @file test_out_convert.cpp
 * @brief The words the DAC gets: the clip, the keep-alive word, NaN as silence, the count.
 */
#include "doctest.h"
#include "out_convert.h"

#include <cmath>
#include <initializer_list>
#include <limits>

namespace {
/// The conversion as it was written with branches; the loop under test must agree with it.
uint32_t reference(const float* in, int32_t* out, size_t n, float scale) {
    uint32_t clamped = 0;
    for (size_t i = 0; i < n; i++) {
        const float x = in[i];
        clamped += !(fabsf(x) <= 1.00001f);
        const float v = x == x ? fminf(fmaxf(x, -1.0f), 1.0f) : 0.0f;
        const int32_t q = (int32_t)(v * scale);
        out[i] = q ? q : audio_core::DAC_KEEPALIVE;
    }
    return clamped;
}
}  // namespace

TEST_CASE("the output words: clipped, never zero, a NaN silent, the faults counted") {
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float in[] = {0.0f, -0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 1.000005f, 1.00002f, -1.00002f,
                        inf, -inf, nan, 1e-12f, -1e-12f, std::nextafter(1.0f, 2.0f),
                        std::nextafter(1.00001f, 2.0f), 3.0e-10f, 0.9999999f};
    const size_t n = sizeof(in) / sizeof(in[0]);
    for (float scale : {2147483520.0f, 656000000.0f, 2147483520.0f * 0.3f}) {
        int32_t got[n], want[n];
        const uint32_t c_got = audio_core::f32_to_i32(in, got, n, scale);
        const uint32_t c_want = reference(in, want, n, scale);
        CHECK(c_got == c_want);
        for (size_t i = 0; i < n; i++) CHECK(got[i] == want[i]);
        CHECK(got[0] == audio_core::DAC_KEEPALIVE);          // a zero is never sent
        CHECK(got[11] == audio_core::DAC_KEEPALIVE);         // a NaN is silence, not -1
    }
}

TEST_CASE("the output words agree with the branching form over a sweep") {
    float in[4096];
    int32_t got[4096], want[4096];
    uint32_t seed = 12345u;
    for (int round = 0; round < 64; round++) {
        for (float& x : in) {
            seed = seed * 1664525u + 1013904223u;
            x = ((float)(int32_t)seed / 2147483648.0f) * 1.2f;        // a little past the ceiling
        }
        CHECK(audio_core::f32_to_i32(in, got, 4096, 656000000.0f) == reference(in, want, 4096, 656000000.0f));
        bool same = true;
        for (int i = 0; i < 4096; i++) same = same && got[i] == want[i];
        CHECK(same);
    }
}
