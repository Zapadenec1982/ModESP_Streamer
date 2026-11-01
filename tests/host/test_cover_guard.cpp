/**
 * @file test_cover_guard.cpp
 * @brief What the software cover decoder may be handed, decided from the file's own bytes.
 *
 * components/cover/include/cover_guard.h. stb_image walks the components of every scan once per
 * scan and cannot be stopped, on a core-0 task that never blocks, so a file of empty or repeated
 * scans held core 0 until the task watchdog panicked. These cases pin the per-scan block
 * accounting it is judged by, the sequential-frame component rule, and the memory estimate.
 */

#include "doctest.h"
#include "../../components/cover/include/cover_guard.h"

#include <vector>

using cover_guard::jpeg_scan_upper_bound;
using cover_guard::soft_scan_work_ok;

namespace {

using Bytes = std::vector<uint8_t>;

void put(Bytes& b, std::initializer_list<uint8_t> v) { b.insert(b.end(), v); }

/// SOI and a frame header: `marker` is C0/C1 (sequential) or C2 (progressive), `nf` components,
/// 2x2 luma when `luma22` and there are three, otherwise all 1x1.
Bytes frame_head(int w, int h, int nf, uint8_t marker, bool luma22) {
    Bytes b;
    put(b, {0xFF, 0xD8});
    const uint8_t len = (uint8_t)(8 + 3 * nf);
    put(b, {0xFF, marker, 0x00, len, 8, (uint8_t)(h >> 8), (uint8_t)h, (uint8_t)(w >> 8), (uint8_t)w,
            (uint8_t)nf});
    for (int i = 0; i < nf; i++)
        put(b, {(uint8_t)(i + 1), (uint8_t)(i == 0 && nf == 3 && luma22 ? 0x22 : 0x11), 0});
    return b;
}
Bytes progressive_head(int w, int h, int nf) { return frame_head(w, h, nf, 0xC2, true); }

cover_guard::Frame frame_of(const Bytes& b) {
    return cover_guard::frame_from_sof(b.data() + 2, b.size() - 2);  // past SOI, at the SOF's FF
}

/// A one-component scan on component `cs`, with a little entropy data carrying the two FF
/// sequences that are not markers stb would start a scan on: a stuffed zero and a restart.
void scan1(Bytes& b, uint8_t cs = 0x01) {
    put(b, {0xFF, 0xDA, 0x00, 0x08, 0x01, cs, 0x00, 0x3F, 0x00, 0x00});
    put(b, {0x12, 0xFF, 0x00, 0x34, 0xFF, 0xD0, 0x56, 0xFF, 0xD7, 0x78});
}

/// A three-component interleaved scan header (Ns = 3, selectors 1/2/3).
void scan3(Bytes& b) {
    put(b, {0xFF, 0xDA, 0x00, 0x0C, 0x03, 0x01, 0x00, 0x02, 0x11, 0x03, 0x11, 0x00, 0x3F, 0x00});
}

}  // namespace

TEST_CASE("cover guard: a 10-scan 1400 px progressive cover is counted right and passes") {
    Bytes b = progressive_head(1400, 1400, 3);
    for (int i = 0; i < 10; i++) scan1(b);
    put(b, {0xFF, 0xD9});
    CHECK(jpeg_scan_upper_bound(b.data(), b.size()) == 10);
    CHECK(soft_scan_work_ok(b.data(), b.size(), frame_of(b), 1400, 1400));
}

TEST_CASE("cover guard: a file of empty DC refinement scans is refused at any size") {
    // A crafted file: 2000 x the 10-byte scan header (Ss = Se = 0, Ah = 1), no data at all.
    Bytes b = progressive_head(2048, 2048, 1);
    for (int i = 0; i < 2000; i++) put(b, {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, 0x00, 0x00, 0x10});
    put(b, {0xFF, 0xD9});
    CHECK(jpeg_scan_upper_bound(b.data(), b.size()) == 2000);
    CHECK_FALSE(soft_scan_work_ok(b.data(), b.size(), frame_of(b), 2048, 2048));  // over MAX_SCANS
    CHECK_FALSE(soft_scan_work_ok(b.data(), b.size(), frame_of(b), 16, 16));
}

TEST_CASE("cover guard: a sequential frame that re-scans its components is refused") {
    // SOF1, three 1x1 components, ten interleaved empty scans. The old scans x (w/8 x h/8)
    // metric passed this (10 x 230^2 = 529000 < 600000) because it charged one component's
    // blocks and never saw that each scan runs an IDCT over all three; the sum of Ns over the
    // scans (30) is far past Nf (3), which T.81 forbids in a sequential frame.
    Bytes b = frame_head(1840, 1840, 3, 0xC1, false);
    for (int i = 0; i < 10; i++) scan3(b);
    put(b, {0xFF, 0xD9});
    const auto f = frame_of(b);
    REQUIRE(f.nf == 3);
    CHECK_FALSE(f.progressive);
    CHECK_FALSE(soft_scan_work_ok(b.data(), b.size(), f, 1840, 1840));
    // The old metric's other blind spot: four components, twenty-five scans at ~1220 px.
    Bytes c = frame_head(1220, 1220, 4, 0xC1, false);
    for (int i = 0; i < 25; i++)
        put(c, {0xFF, 0xDA, 0x00, 0x08, 0x01, (uint8_t)(1 + i % 4), 0x00, 0x00, 0x3F, 0x00});
    put(c, {0xFF, 0xD9});
    CHECK_FALSE(soft_scan_work_ok(c.data(), c.size(), frame_of(c), 1220, 1220));
}

TEST_CASE("cover guard: a legitimate single-scan interleaved baseline passes") {
    Bytes b = frame_head(1000, 1000, 3, 0xC0, true);     // YCbCr 4:2:0, one interleaved scan
    scan3(b);
    put(b, {0xFF, 0xD9});
    const auto f = frame_of(b);
    REQUIRE(f.nf == 3);
    CHECK(soft_scan_work_ok(b.data(), b.size(), f, 1000, 1000));
}

TEST_CASE("cover guard: a legitimate three-scan non-interleaved baseline passes") {
    Bytes b = frame_head(1000, 1000, 3, 0xC0, true);     // one scan per component, Ns summing to Nf
    scan1(b, 0x01);
    scan1(b, 0x02);
    scan1(b, 0x03);
    put(b, {0xFF, 0xD9});
    CHECK(soft_scan_work_ok(b.data(), b.size(), frame_of(b), 1000, 1000));
}

TEST_CASE("cover guard: a progressive file whose scans sum past the block budget is refused") {
    // Thirty interleaved progressive scans at 2048 px: each is 16384 MCUs x (4+1+1) = 98304
    // visits, 2.95 M in all, far past 600000, though the scan count alone (30) is under the cap.
    Bytes b = progressive_head(2048, 2048, 3);
    for (int i = 0; i < 30; i++) scan3(b);
    put(b, {0xFF, 0xD9});
    CHECK(jpeg_scan_upper_bound(b.data(), b.size()) == 30);
    CHECK_FALSE(soft_scan_work_ok(b.data(), b.size(), frame_of(b), 2048, 2048));
    // Six such scans fit (589824) and seven do not, so the budget is a real gate on the summed
    // work, not an all-or-nothing cap on the scan count.
    Bytes c = progressive_head(2048, 2048, 3);
    for (int i = 0; i < 6; i++) scan3(c);
    put(c, {0xFF, 0xD9});
    CHECK(soft_scan_work_ok(c.data(), c.size(), frame_of(c), 2048, 2048));  // 6 x 98304 = 589824
    CHECK_FALSE(soft_scan_work_ok(nullptr, 0, frame_of(c), 2048, 2048));
    CHECK_FALSE(soft_scan_work_ok(c.data(), c.size(), frame_of(c), 0, 100));
}

TEST_CASE("cover guard: fill bytes before a marker count it once") {
    const uint8_t fill[] = {0xFF, 0xFF, 0xFF, 0xDA, 0x00};
    CHECK(jpeg_scan_upper_bound(fill, sizeof fill) == 1);
}

TEST_CASE("cover guard: stuffed zeros, restarts and a bare DA are not scans") {
    const uint8_t data[] = {0xDA, 0xFF, 0x00, 0xDA, 0xFF, 0xD0, 0xDA, 0xFF, 0xD7, 0xFF, 0xD9, 0xDA};
    CHECK(jpeg_scan_upper_bound(data, sizeof data) == 0);
    const uint8_t tail[] = {0x00, 0xFF};                 // a lone FF at the very end
    CHECK(jpeg_scan_upper_bound(tail, sizeof tail) == 0);
    CHECK(jpeg_scan_upper_bound(tail, 0) == 0);
}

// ── Memory: stb's peak, from the frame header, against the largest free block ──

namespace {

cover_guard::Frame frame(bool progressive, std::initializer_list<uint8_t> hv) {
    cover_guard::Frame f;
    f.progressive = progressive;
    for (uint8_t x : hv) { f.h[f.nf] = x >> 4; f.v[f.nf] = x & 15; f.nf++; }
    return f;
}

/// The largest free PSRAM block the bench reported with the player running.
constexpr size_t BENCH_LARGEST = 23068672;

}  // namespace

TEST_CASE("cover guard: the frame header is read the way stb lays the planes out") {
    Bytes b = progressive_head(1000, 1000, 3);
    const auto f = cover_guard::frame_from_sof(b.data() + 2, b.size() - 2);
    CHECK(f.progressive);
    REQUIRE(f.nf == 3);
    CHECK((f.h[0] == 2 && f.v[0] == 2 && f.h[1] == 1 && f.v[2] == 1));
    b[3] = 0xC0;                                         // the same frame, baseline
    CHECK_FALSE(cover_guard::frame_from_sof(b.data() + 2, b.size() - 2).progressive);
    CHECK(cover_guard::frame_from_sof(b.data() + 2, 12).nf == 0);       // cut short
    b[2 + 11] = 0x02;                                    // H = 0: stb refuses it at SOF
    CHECK(cover_guard::frame_from_sof(b.data() + 2, b.size() - 2).nf == 0);
}

TEST_CASE("cover guard: stb's peak follows sampling, padding and progressive") {
    const auto p420 = frame(true, {0x22, 0x11, 0x11});
    const auto p444 = frame(true, {0x11, 0x11, 0x11});
    // 1000 px at 4:2:0 pads to 1008: 1008^2 + 2 x 504^2 bytes of planes, three times over
    // with the coefficients, plus 3 x 1000^2 of output and three line buffers.
    CHECK(cover_guard::jpeg_soft_peak(p420, 1000, 1000) == 7575387);
    CHECK(cover_guard::jpeg_soft_peak(p444, 1000, 1000) == 12003099);   // 12 bytes a pixel
    CHECK(cover_guard::jpeg_soft_peak(frame(false, {0x22, 0x11, 0x11}), 1000, 1000) == 4527150);
    // 1750 px progressive 4:2:0 holds about 23 MB.
    CHECK(cover_guard::jpeg_soft_peak(p420, 1750, 1750) == 23132049);
    // A frame that was not read is costed as four full-size progressive components.
    CHECK(cover_guard::jpeg_soft_peak(cover_guard::Frame{}, 100, 100) == 160324);
    CHECK(cover_guard::jpeg_soft_peak(p420, 0, 100) == 0);
    CHECK(cover_guard::png_soft_peak(1000, 1000, false) == 11000000);
    CHECK(cover_guard::png_soft_peak(1000, 1000, true) == 22000000);
}

TEST_CASE("cover guard: at the bench's free memory, big progressive covers are refused up front") {
    const auto p420 = frame(true, {0x22, 0x11, 0x11});
    using cover_guard::soft_memory_need;
    using cover_guard::jpeg_soft_peak;
    CHECK(soft_memory_need(jpeg_soft_peak(p420, 1000, 1000)) <= BENCH_LARGEST);
    CHECK(soft_memory_need(jpeg_soft_peak(p420, 1664, 1664)) <= BENCH_LARGEST);
    CHECK(soft_memory_need(jpeg_soft_peak(p420, 1672, 1672)) > BENCH_LARGEST);   // the reserve
    CHECK(soft_memory_need(jpeg_soft_peak(p420, 1750, 1750)) > BENCH_LARGEST);
    CHECK(soft_memory_need(jpeg_soft_peak(p420, 2048, 2048)) > BENCH_LARGEST);
    CHECK(soft_memory_need(1000) == 1000 + cover_guard::SOFT_RESERVE);
}
