/**
 * @file test_cover_thumb.cpp
 * @brief A logo shrunk into a preset's disc: fitted whole, averaged down, on its own ground.
 *
 * components/cover/include/cover_thumb.h. A logo is not cropped as a cover is — half a word is no
 * logo — so the box it lands in, the area average that counts every source pixel once, the alpha
 * over the ground, and the ground itself (the edge's colour for an opaque picture, a light or dark
 * neutral by the ink of a transparent one) are pinned here.
 */

#include "doctest.h"
#include "../../components/cover/include/cover_thumb.h"

#include <vector>

using cover_thumb::Box;
using cover_thumb::Layout;
using cover_thumb::fit;
using cover_thumb::ground;
using cover_thumb::render;

namespace {

constexpr int SIDE = 40;

uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
uint16_t rgb565(uint32_t rgb) { return rgb565((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF); }

/// An RGBA picture, every pixel `fill`.
struct Rgba {
    int w, h;
    std::vector<uint8_t> px;
    Rgba(int w_, int h_, uint32_t rgb, uint8_t a = 255) : w(w_), h(h_), px((size_t)w_ * h_ * 4) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) set(x, y, rgb, a);
    }
    void set(int x, int y, uint32_t rgb, uint8_t a = 255) {
        uint8_t* p = &px[((size_t)y * w + x) * 4];
        p[0] = (uint8_t)(rgb >> 16); p[1] = (uint8_t)(rgb >> 8); p[2] = (uint8_t)rgb; p[3] = a;
    }
    size_t stride() const { return (size_t)w * 4; }
};

std::vector<uint16_t> thumb(const Rgba& p, uint32_t g) {
    std::vector<uint16_t> out((size_t)SIDE * SIDE, 0xDEAD);
    render(p.px.data(), Layout::RGBA8888, p.w, p.h, p.stride(), out.data(), SIDE, g);
    return out;
}

}  // namespace

TEST_CASE("cover thumb: the whole picture, its longer side across the square, centred") {
    Box b = fit(300, 300, SIDE);
    CHECK((b.x == 0 && b.y == 0 && b.w == 40 && b.h == 40));
    b = fit(200, 100, SIDE);                       // a banner: a band across the middle
    CHECK((b.x == 0 && b.y == 10 && b.w == 40 && b.h == 20));
    b = fit(100, 300, SIDE);                       // 13.3 px wide, rounded
    CHECK((b.w == 13 && b.h == 40 && b.x == 13 && b.y == 0));
    b = fit(16, 16, SIDE);                         // a favicon smaller than the disc fills it
    CHECK((b.w == 40 && b.h == 40));
    b = fit(1000, 1, SIDE);                        // never thinner than a pixel
    CHECK((b.w == 40 && b.h == 1 && b.y == 19));
    b = fit(0, 10, SIDE);
    CHECK(b.w == 0);
}

TEST_CASE("cover thumb: one colour stays that colour, and a checkerboard averages to grey") {
    Rgba green(80, 80, 0x20C040);
    for (uint16_t v : thumb(green, 0x0000FF)) CHECK(v == rgb565(0x20C040));

    // Two source pixels to one output pixel each way: two white and two black in every block.
    Rgba board(80, 80, 0x000000);
    for (int y = 0; y < 80; y++)
        for (int x = 0; x < 80; x++)
            if ((x + y) & 1) board.set(x, y, 0xFFFFFF);
    for (uint16_t v : thumb(board, 0x0000FF)) CHECK(v == rgb565(128, 128, 128));
}

TEST_CASE("cover thumb: a banner leaves the ground above and below it") {
    Rgba white(80, 40, 0xFFFFFF);
    const auto out = thumb(white, 0x0000FF);
    for (int y = 0; y < SIDE; y++) {
        const bool band = y >= 10 && y < 30;
        for (int x = 0; x < SIDE; x++)
            CHECK(out[y * SIDE + x] == (band ? rgb565(0xFFFFFF) : rgb565(0x0000FF)));
    }
}

TEST_CASE("cover thumb: a transparent pixel adds ground, a half-transparent one half of it") {
    Rgba red(40, 40, 0xFF0000, 128);
    const auto out = thumb(red, cover_thumb::LIGHT);
    // (255 * 128 + 238 * 127 + 127) / 255 = 247; (238 * 127 + 127) / 255 = 119.
    for (uint16_t v : out) CHECK(v == rgb565(247, 119, 119));

    Rgba clear(40, 40, 0xFF0000, 0);
    for (uint16_t v : thumb(clear, cover_thumb::DARK)) CHECK(v == rgb565(cover_thumb::DARK));
}

TEST_CASE("cover thumb: every source pixel counts, at a ratio that is not whole") {
    // 100 px into 40: blocks of two and three. A red last row and column must reach the last
    // output row and column; a lost edge would leave them pure white.
    Rgba p(100, 100, 0xFFFFFF);
    for (int i = 0; i < 100; i++) { p.set(99, i, 0xFF0000); p.set(i, 99, 0xFF0000); }
    const auto out = thumb(p, 0x000000);
    CHECK(out[0] == rgb565(0xFFFFFF));
    CHECK(out[39 * SIDE + 39] != rgb565(0xFFFFFF));
    CHECK(out[39 * SIDE + 0] != rgb565(0xFFFFFF));
    CHECK(out[0 * SIDE + 39] != rgb565(0xFFFFFF));
    CHECK(out[38 * SIDE + 38] == rgb565(0xFFFFFF));

    // One white pixel in a 3x3 block of black: a ninth of white.
    Rgba dot(120, 120, 0x000000);
    dot.set(4, 4, 0xFFFFFF);
    const auto o2 = thumb(dot, 0x0000FF);
    CHECK(o2[1 * SIDE + 1] == rgb565(28, 28, 28));    // (255 * 255 + 1147) / 2295 = 28.8 -> 28
    CHECK(o2[0] == rgb565(0, 0, 0));
}

TEST_CASE("cover thumb: a picture smaller than the square is enlarged by repeating pixels") {
    Rgba one(1, 1, 0x123456);
    for (uint16_t v : thumb(one, 0xFFFFFF)) CHECK(v == rgb565(0x123456));
    Rgba two(2, 1, 0xFF0000);
    two.set(1, 0, 0x00FF00);
    const auto out = thumb(two, 0x000000);
    CHECK(out[10 * SIDE + 0] == rgb565(0xFF0000));     // the band is rows 10..29
    CHECK(out[10 * SIDE + 39] == rgb565(0x00FF00));
    CHECK(out[0] == rgb565(0x000000));
}

TEST_CASE("cover thumb: rows are read by their stride, not by their width") {
    // RGB888 rows of 3 pixels padded to 16 bytes with bytes that would show as magenta.
    const int w = 3, h = 3;
    const size_t stride = 16;
    std::vector<uint8_t> px(stride * h, 0xFF);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = &px[y * stride + x * 3];
            p[0] = 0x10; p[1] = 0x80; p[2] = 0x10;
        }
    std::vector<uint16_t> out((size_t)SIDE * SIDE);
    render(px.data(), Layout::RGB888, w, h, stride, out.data(), SIDE, 0x000000);
    for (uint16_t v : out) CHECK(v == rgb565(0x10, 0x80, 0x10));
}

TEST_CASE("cover thumb: RGB565 from the JPEG engine reads as the colours it holds") {
    // Little-endian words: pure red, then pure blue.
    const uint8_t px[] = {0x00, 0xF8, 0x1F, 0x00};
    std::vector<uint16_t> out((size_t)SIDE * SIDE);
    render(px, Layout::RGB565, 2, 1, 4, out.data(), SIDE, 0x000000);
    CHECK(out[10 * SIDE + 0] == rgb565(0xFF0000));
    CHECK(out[10 * SIDE + 39] == rgb565(0x0000FF));
}

TEST_CASE("cover thumb: an opaque picture's ground is the colour of its edge") {
    Rgba p(10, 10, 0x0000FF);
    for (int i = 0; i < 10; i++) {
        p.set(i, 0, 0xFF0000); p.set(i, 9, 0xFF0000); p.set(0, i, 0xFF0000); p.set(9, i, 0xFF0000);
    }
    CHECK(ground(p.px.data(), Layout::RGBA8888, p.w, p.h, p.stride()) == 0xFF0000);
    // An RGBA file whose every alpha is 255 is opaque all the same.
    Rgba white(4, 4, 0xFFFFFF);
    CHECK(ground(white.px.data(), Layout::RGBA8888, 4, 4, white.stride()) == 0xFFFFFF);
    // One pixel: its own colour.
    Rgba one(1, 1, 0x406080);
    CHECK(ground(one.px.data(), Layout::RGBA8888, 1, 1, one.stride()) == 0x406080);
}

TEST_CASE("cover thumb: transparent ink gets the ground it reads on") {
    // Dark lettering on nothing: the light ground.
    Rgba dark(8, 8, 0x000000, 0);
    for (int x = 2; x < 6; x++) dark.set(x, 4, 0x102030);
    CHECK(ground(dark.px.data(), Layout::RGBA8888, 8, 8, dark.stride()) == cover_thumb::LIGHT);
    // A white logo made for a dark page: the dark ground, or it would vanish.
    Rgba light(8, 8, 0x000000, 0);
    for (int x = 2; x < 6; x++) light.set(x, 4, 0xFFFFFF);
    CHECK(ground(light.px.data(), Layout::RGBA8888, 8, 8, light.stride()) == cover_thumb::DARK);
    // The colour of a fully transparent pixel is not ink: a white logo on transparent black.
    Rgba ghost(8, 8, 0xFFFFFF, 0);
    for (int x = 2; x < 6; x++) ghost.set(x, 4, 0x000000);
    CHECK(ground(ghost.px.data(), Layout::RGBA8888, 8, 8, ghost.stride()) == cover_thumb::LIGHT);
    // Nothing visible at all.
    Rgba none(4, 4, 0xFFFFFF, 0);
    CHECK(ground(none.px.data(), Layout::RGBA8888, 4, 4, none.stride()) == cover_thumb::LIGHT);
}
