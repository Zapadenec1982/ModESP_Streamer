/**
 * @file out_convert.cpp
 * @brief Float to int32 without a branch in the loop.
 */
#include "out_convert.h"

#include <cmath>

namespace audio_core {

uint32_t f32_to_i32(const float* in, int32_t* out, size_t n, float scale) {
    // No branch per sample: the two tests that used to be branches (is it a number, is the
    // word zero) are a mask and a shifted flag, so the samples do not wait on one another.
    // fmaxf and fminf return the operand that is a number, so a NaN leaves the clip as -1 and
    // the mask then makes it the silence the branch gave.
    uint32_t inside = 0;
    for (size_t i = 0; i < n; i++) {
        const float x = in[i];
        inside += (uint32_t)(std::fabs(x) <= 1.00001f);
        const float v = std::fmin(std::fmax(x, -1.0f), 1.0f);
        int32_t q = (int32_t)(v * scale);
        q &= -(int32_t)(x == x);
        q |= (int32_t)(q == 0) << 8;
        out[i] = q;
    }
    return (uint32_t)n - inside;
}

}  // namespace audio_core
