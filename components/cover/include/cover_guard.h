/**
 * @file cover_guard.h
 * @brief What the software decoder is allowed to cost, decided before it starts. No IDF.
 *
 * stb_image (src/cover_soft.c) has no budget of its own, and the task it runs on is pinned to
 * core 0 and never blocks while it decodes: whatever it is handed, it finishes, and IDLE0 does
 * not run until it has. The task watchdog panics after 5 s of that (main/main.cpp). And stb
 * asks for its output only after the last scan, so a picture too big for the memory there is
 * fails only once all the work is done. So both are judged here, from the file's own bytes,
 * before stb is called: the time by the scan count, the memory by the frame header. Pinned on
 * the host by tests/host/test_cover_guard.cpp; used by src/cover.cpp.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace cover_guard {

/// More scans than any encoder writes: libjpeg's progressive script is 10 for YCbCr and 6 for
/// grayscale. A count far above that is a file built to keep the decoder busy.
constexpr size_t MAX_SCANS = 32;

/// The 8x8 block visits stb makes across every scan, summed: the walk it cannot be stopped
/// in. It visits every block of the components a scan names, once per scan, whether or not the
/// scan carries any data - past the next marker its bit reader feeds zeros (stb_image.h,
/// stbi__grow_buffer_unsafe) - and for a sequential frame each of those visits runs an IDCT,
/// so a file that repeats scans or interleaves N components per scan multiplies the work.
/// 600000 lets a real 10-scan progressive cover through to about 1950 px a side.
/// A starting value: calibrate on the panel so the worst case stays well under a second.
constexpr uint64_t SCAN_BLOCK_BUDGET = 600000;

/// An upper bound on the scans stb_image will walk in `p`: every FF DA in the file.
///
/// stb starts a scan only on a marker, and it reads a marker as FF (any further FF fill) and
/// then the marker byte - in stbi__get_marker, in the bit reader that stops at one, and in the
/// junk skip after a scan. So every scan it walks ends an FF DA pair, and each pair is read at
/// most once. A pair inside an APPn payload counts too; that only ever over-counts.
inline size_t jpeg_scan_upper_bound(const uint8_t* p, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i + 1 < len; i++)
        if (p[i] == 0xFF && p[i + 1] == 0xDA) n++;
    return n;
}

/// What stb_image's memory and per-scan work depend on in a JPEG's frame header.
struct Frame {
    bool    progressive = false;
    int     nf = 0;                  ///< components; 0 when the header was not found or not read
    uint8_t id[4] = {};              ///< component ids, as SOS selectors name them
    uint8_t h[4] = {};               ///< sampling factors, per component
    uint8_t v[4] = {};
};

/// The frame header at `p` - FF C0/C1/C2, length, precision, height, width, Nf, then three bytes
/// a component (id, HV, Tq) - with `n` bytes of file from there. nf stays 0 for anything stb
/// would refuse.
inline Frame frame_from_sof(const uint8_t* p, size_t n) {
    Frame f;
    if (n < 10 || p[0] != 0xFF) return f;
    const int nf = p[9];
    if (nf < 1 || nf > 4 || n < 10 + 3 * (size_t)nf) return f;
    for (int i = 0; i < nf; i++) {
        f.id[i]         = p[10 + 3 * i];
        const uint8_t hv = p[10 + 3 * i + 1];
        f.h[i] = hv >> 4;
        f.v[i] = hv & 15;
        if (!f.h[i] || !f.v[i] || f.h[i] > 4 || f.v[i] > 4) return Frame{};
    }
    f.progressive = p[1] == 0xC2;
    f.nf = nf;
    return f;
}

/// True when the work stb_image would do decoding the JPEG at `p` is work a cover may cost.
///
/// `f` is its frame header (frame_from_sof), `w` x `h` the size stb reported. The scan count
/// alone misses two things the attacker uses: an interleaved scan visits all the components it
/// names, not one, and a sequential frame runs an IDCT on every block of every scan. So this
/// reads each SOS, charges the MCU-padded blocks of exactly the components that scan names, and
/// sums them. T.81 puts each component in one scan of a sequential frame, so a sequential frame
/// whose scans name more than Nf components in total is a file built to inflate that work and is
/// refused outright; past that, the summed visits are held to SCAN_BLOCK_BUDGET and the count to
/// MAX_SCANS. A scan header this cannot read, or a selector that names no component, is charged
/// the whole frame - over-counting only ever refuses, it never lets more work through.
inline bool soft_scan_work_ok(const uint8_t* p, size_t len, const Frame& f, int w, int h) {
    if (!p || w <= 0 || h <= 0) return false;

    // Geometry. A frame that was not read is costed as one full-size component per scan.
    int nf = f.nf;
    uint8_t cid[4], ch[4], cv[4];
    if (nf < 1) { nf = 1; cid[0] = 0; ch[0] = cv[0] = 1; }
    else for (int i = 0; i < nf; i++) { cid[i] = f.id[i]; ch[i] = f.h[i]; cv[i] = f.v[i]; }

    uint64_t hmax = 1, vmax = 1, frame_hv = 0;
    for (int i = 0; i < nf; i++) { if (ch[i] > hmax) hmax = ch[i]; if (cv[i] > vmax) vmax = cv[i]; }
    for (int i = 0; i < nf; i++) frame_hv += (uint64_t)ch[i] * cv[i];
    const uint64_t mcus = (((uint64_t)w + 8 * hmax - 1) / (8 * hmax)) *
                          (((uint64_t)h + 8 * vmax - 1) / (8 * vmax));

    size_t   scans  = 0;
    uint64_t sum_ns = 0;
    uint64_t visits = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (p[i] != 0xFF || p[i + 1] != 0xDA) continue;
        if (++scans > MAX_SCANS) return false;
        const unsigned ns = (i + 4 < len) ? p[i + 4] : 0;
        sum_ns += ns ? ns : 1;
        uint64_t hv = 0;
        if (ns >= 1 && ns <= 4 && i + 5 + 2 * (size_t)ns <= len) {
            for (unsigned k = 0; k < ns; k++) {
                const uint8_t cs = p[i + 5 + 2 * k];
                uint64_t comp = frame_hv;          // a selector that names no component
                for (int c = 0; c < nf; c++)
                    if (cid[c] == cs) { comp = (uint64_t)ch[c] * cv[c]; break; }
                hv += comp;
            }
        } else {
            hv = frame_hv;                         // a scan header we could not read
        }
        visits += mcus * hv;
    }
    if (f.nf >= 1 && !f.progressive && sum_ns > (uint64_t)f.nf) return false;
    return visits <= SCAN_BLOCK_BUDGET;
}

/// stb_image's peak allocation for a `w` x `h` JPEG, in bytes.
///
/// Per component an MCU-padded w2 x h2 byte plane (stb_image.h:3337) and, progressive, a w2 x h2
/// plane of shorts beside it (:3346), allocated at the frame header and held through every scan
/// until the 3 x w x h RGB output is asked for after the last one (:3922), with a line buffer
/// per component (:3904). A frame that was not read is costed as the worst stb accepts: four
/// full-size components, progressive.
inline uint64_t jpeg_soft_peak(const Frame& frame, int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    Frame f = frame;
    if (f.nf < 1) {
        f.progressive = true;
        f.nf = 4;
        for (int i = 0; i < 4; i++) f.h[i] = f.v[i] = 1;
    }
    uint64_t hmax = 1, vmax = 1;
    for (int i = 0; i < f.nf; i++) {
        if (f.h[i] > hmax) hmax = f.h[i];
        if (f.v[i] > vmax) vmax = f.v[i];
    }
    const uint64_t mcu_x = ((uint64_t)w + 8 * hmax - 1) / (8 * hmax);
    const uint64_t mcu_y = ((uint64_t)h + 8 * vmax - 1) / (8 * vmax);
    uint64_t total = 3ull * (uint64_t)w * (uint64_t)h;
    for (int i = 0; i < f.nf; i++) {
        const uint64_t plane = (mcu_x * f.h[i] * 8) * (mcu_y * f.v[i] * 8);
        total += plane + 15 + (f.progressive ? plane * 2 + 15 : 0) + (uint64_t)w + 3;
    }
    return total;
}

/// stb_image's peak for a `w` x `h` PNG, in bytes: 11 bytes a pixel covers every 8-bit layout
/// (an interlaced RGBA one holds the inflated rows, the finished picture and its last pass at
/// once: 4 + 4 + 2), and 16 bits a sample doubles all of it.
inline uint64_t png_soft_peak(int w, int h, bool sixteen_bit) {
    if (w <= 0 || h <= 0) return 0;
    return (sixteen_bit ? 22ull : 11ull) * (uint64_t)w * (uint64_t)h;
}

/// Kept free on top of the peak. The peak is several allocations measured against one block,
/// and the rest of the firmware keeps allocating while a decode runs.
constexpr uint64_t SOFT_RESERVE = 2u << 20;

/// The free PSRAM block a decode that peaks at `peak` bytes needs before it starts: the peak
/// and the reserve, in one piece. decode_soft() asks for it by allocating it.
inline uint64_t soft_memory_need(uint64_t peak) {
    return peak + SOFT_RESERVE;
}

}  // namespace cover_guard
