/**
 * @file test_uac_tone.cpp
 * @brief HOST tests for the USB audio stream check (components/usb_uac/include/uac_tone.h): a
 *        clean tone is measured to a fraction of a hertz and counts no breaks — at 16-bit
 *        quantisation too, and a low tone at 96 kHz, where its float block sums are tightest;
 *        one lost packet and one repeated packet are each one break; music and a tone being
 *        retuned count nothing.
 */

#include <cmath>
#include <cstdint>
#include <vector>
#include "doctest.h"
#include "../../components/usb_uac/include/uac_tone.h"

using usb_uac::Tone;

namespace {

constexpr uint32_t FS = 48000;

/// `seconds` of a sine at `hz`, amplitude `amp`; quantised to 16 bits when asked.
std::vector<float> sine(double hz, double seconds, double amp = 0.5, bool q16 = false) {
    std::vector<float> v(size_t(seconds * FS));
    for (size_t n = 0; n < v.size(); n++) {
        double x = amp * std::sin(2.0 * M_PI * hz * double(n) / FS);
        if (q16) x = std::round(x * 32767.0) / 32768.0;
        v[n] = float(x);
    }
    return v;
}

/// The same at `fs`, quantised to 24 bits: the computer's widest format.
std::vector<float> sine24(double hz, double seconds, uint32_t fs) {
    std::vector<float> v(size_t(seconds * fs));
    for (size_t n = 0; n < v.size(); n++)
        v[n] = float(std::round(0.5 * std::sin(2.0 * M_PI * hz * double(n) / fs) * 8388607.0) / 8388608.0);
    return v;
}

struct Run {
    Tone tone;
    uint32_t fs = FS;
    uint32_t breaks = 0;
    std::vector<float> hz;   // one per closed second

    void feed(const std::vector<float>& v) {
        float peak = 0;
        uint32_t in_second = 0;
        for (float x : v) {
            tone.push(x);
            peak = std::max(peak, std::fabs(x));
            if (++in_second == fs) {
                hz.push_back(tone.close(fs, peak, breaks));
                in_second = 0;
                peak = 0;
            }
        }
    }
};

}  // namespace

TEST_CASE("uac_tone: a clean tone is measured and breaks nothing") {
    Run r;
    r.feed(sine(1000.0, 4.0));
    REQUIRE(r.hz.size() == 4);
    for (float hz : r.hz) CHECK(hz == doctest::Approx(1000.0).epsilon(0.0005));
    CHECK(r.breaks == 0);
}

TEST_CASE("uac_tone: 16-bit quantisation is not a break") {
    Run r;
    r.feed(sine(997.0, 4.0, 0.5, true));
    CHECK(r.hz.back() == doctest::Approx(997.0).epsilon(0.0005));
    CHECK(r.breaks == 0);
}

TEST_CASE("uac_tone: a low tone at 96 kHz and 24 bits is still one clean tone") {
    // The sums are float over a block. On the second difference they hold even here; on
    // x + x2 the residual is (1 - c)^-2, some 2.7e7, times smaller than the sums it comes from.
    Run r;
    r.fs = 96000;
    r.feed(sine24(300.0, 4.0, 96000));
    REQUIRE(r.hz.size() == 4);
    for (float hz : r.hz) CHECK(hz == doctest::Approx(300.0).epsilon(0.0005));
    CHECK(r.breaks == 0);
}

TEST_CASE("uac_tone: one lost packet is one break") {
    std::vector<float> v = sine(1000.0, 4.0);
    // A microframe at 48 kHz carries 6 frames; lose one in the third second.
    v.erase(v.begin() + 2 * FS + 12345, v.begin() + 2 * FS + 12345 + 6);
    Run r;
    r.feed(v);
    CHECK(r.breaks == 1);
}

TEST_CASE("uac_tone: one lost packet at 96 kHz is one break") {
    std::vector<float> v = sine24(1000.0, 4.0, 96000);
    // A microframe at 96 kHz carries 12 frames.
    v.erase(v.begin() + 2 * 96000 + 12345, v.begin() + 2 * 96000 + 12345 + 12);
    Run r;
    r.fs = 96000;
    r.feed(v);
    CHECK(r.breaks == 1);
}

TEST_CASE("uac_tone: five lost packets in one second are five breaks") {
    std::vector<float> v = sine(1000.0, 4.0);
    for (int k = 4; k >= 0; k--) {   // from the back, so earlier positions stay put
        const size_t at = 2 * FS + 5000 + size_t(k) * 8000;
        v.erase(v.begin() + at, v.begin() + at + 6);
    }
    Run r;
    r.feed(v);
    CHECK(r.breaks == 5);
}

TEST_CASE("uac_tone: one repeated packet is one break") {
    std::vector<float> v = sine(1000.0, 4.0);
    const size_t at = 2 * FS + 20000;
    std::vector<float> dup(v.begin() + at - 6, v.begin() + at);
    v.insert(v.begin() + at, dup.begin(), dup.end());
    Run r;
    r.feed(v);
    CHECK(r.breaks == 1);
}

TEST_CASE("uac_tone: two tones are not one tone, and count nothing") {
    std::vector<float> a = sine(1000.0, 3.0, 0.3), b = sine(1531.0, 3.0, 0.3);
    for (size_t n = 0; n < a.size(); n++) a[n] += b[n];
    Run r;
    r.feed(a);
    for (float hz : r.hz) CHECK(hz == 0.f);
    CHECK(r.breaks == 0);
}

TEST_CASE("uac_tone: a tone being retuned counts nothing") {
    std::vector<float> v = sine(1000.0, 2.0);
    std::vector<float> w = sine(1500.0, 2.0);
    v.insert(v.end(), w.begin(), w.end());
    Run r;
    r.feed(v);
    CHECK(r.hz.back() == doctest::Approx(1500.0).epsilon(0.0005));
    CHECK(r.breaks == 0);
}

TEST_CASE("uac_tone: silence is no tone") {
    Run r;
    r.feed(std::vector<float>(2 * FS, 0.f));
    for (float hz : r.hz) CHECK(hz == 0.f);
    CHECK(r.breaks == 0);
}
