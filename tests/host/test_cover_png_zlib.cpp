/**
 * @file test_cover_png_zlib.cpp
 * @brief The DEFLATE-block cap that stops a crafted PNG from pinning core 0 inside stb.
 *
 * components/cover/src/cover_soft.c defines STBI_MAX_DEFLATE_BLOCKS and components/cover/src/
 * stb_image.h honours it in stbi__parse_zlib. A PNG whose IDAT is a long run of empty DEFLATE
 * blocks makes stb rebuild both fixed Huffman tables for every block, so byte length drives
 * CPU and the core-0 cover task misses the 5 s task watchdog.
 * This compiles stb the way cover_soft.c does, with the cap on, and drives it through the public
 * zlib entry points - so it needs no PNG framing, only a hand-built raw DEFLATE stream.
 */

#include "doctest.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

// stb exactly as the firmware builds it (components/cover/src/cover_soft.c), cap included.
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_GIF
#define STBI_NO_PSD
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_ASSERT(x) ((void)0)
#define STBI_MAX_DEFLATE_BLOCKS 8192
#define STB_IMAGE_IMPLEMENTATION
#include "../../components/cover/src/stb_image.h"

namespace {

/// A least-significant-bit-first bit writer, the order DEFLATE packs bits in.
struct BitWriter {
    std::vector<uint8_t> bytes;
    int nbits = 0;
    void bit(int b) {
        if (nbits % 8 == 0) bytes.push_back(0);
        if (b) bytes.back() |= (uint8_t)(1 << (nbits % 8));
        nbits++;
    }
    void lsb(unsigned v, int n) { for (int i = 0; i < n; i++) bit((v >> i) & 1); }
    void msb(unsigned v, int n) { for (int i = n - 1; i >= 0; i--) bit((v >> i) & 1); }
};

/// An empty fixed-Huffman block: BFINAL, BTYPE = 01, then the end-of-block symbol (256), whose
/// fixed code is seven zero bits. Ten bits, and stb rebuilds both Huffman tables to read it.
void empty_fixed_block(BitWriter& w, int last) {
    w.bit(last);
    w.lsb(1, 2);   // BTYPE = 01 (fixed Huffman)
    w.msb(0, 7);   // symbol 256
}

std::vector<uint8_t> empty_blocks(int n) {
    BitWriter w;
    for (int i = 0; i < n - 1; i++) empty_fixed_block(w, 0);
    empty_fixed_block(w, 1);
    return w.bytes;
}

}  // namespace

TEST_CASE("cover png zlib: a short run of empty deflate blocks decodes") {
    auto d = empty_blocks(16);
    int outlen = -1;
    char* out = stbi_zlib_decode_noheader_malloc((const char*)d.data(), (int)d.size(), &outlen);
    CHECK(out != nullptr);      // under the cap: stb walks all 16 blocks and returns (no output)
    CHECK(outlen == 0);
    free(out);
}

TEST_CASE("cover png zlib: a long run of empty deflate blocks is refused by the cap") {
    auto d = empty_blocks(STBI_MAX_DEFLATE_BLOCKS + 100);
    int outlen = -1;
    char* out = stbi_zlib_decode_noheader_malloc((const char*)d.data(), (int)d.size(), &outlen);
    CHECK(out == nullptr);      // the block cap stops it, not the end of the file
    free(out);                  // free(nullptr) is a no-op
}

TEST_CASE("cover png zlib: a normal fixed-Huffman block still decodes its bytes") {
    // One final fixed block of three literals, then end of block. Fixed codes for symbols
    // 0..143 are the 8-bit values 0x30 + symbol, written most-significant bit first.
    BitWriter w;
    w.bit(1);          // BFINAL
    w.lsb(1, 2);       // BTYPE = 01
    w.msb(0x30 + 'A', 8);
    w.msb(0x30 + 'B', 8);
    w.msb(0x30 + 'C', 8);
    w.msb(0, 7);       // end of block
    int outlen = -1;
    char* out = stbi_zlib_decode_noheader_malloc((const char*)w.bytes.data(), (int)w.bytes.size(), &outlen);
    REQUIRE(out != nullptr);
    REQUIRE(outlen == 3);
    CHECK(out[0] == 'A');
    CHECK(out[1] == 'B');
    CHECK(out[2] == 'C');
    free(out);
}
