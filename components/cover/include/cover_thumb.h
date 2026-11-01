/**
 * @file cover_thumb.h
 * @brief A picture shrunk into a small square for a button: the whole of it, centred, on a ground
 *        of its own. No IDF.
 *
 * A station's logo is drawn in a preset's 40 px disc (components/radio_catalog/include/
 * station_logo.h). Unlike a cover it is not cropped: a logo is often a word, and half a word is no
 * logo. So it is fitted — its longer side across the square — and averaged down, every source
 * pixel counted in exactly one output pixel (an area filter; a picture smaller than the square is
 * enlarged by repeating its pixels). What the picture does not cover, and what shows through
 * where it is transparent, is the ground:
 *   - a picture with no transparency: the average colour of its own edge, so the margins of a
 *     banner carry on its background;
 *   - one with transparency: a light neutral under dark ink and a dark one under light ink — a
 *     white logo made for a dark page vanishes on a light ground.
 * Pinned on the host by tests/host/test_cover_thumb.cpp; used by src/cover.cpp (cover_thumb()).
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace cover_thumb {

/// How the source pixels are laid out. RGB565 is little-endian, as the JPEG engine writes it in
/// its BGR order and as LVGL reads it; the others are bytes in R, G, B(, A) order, as stb gives.
enum class Layout : uint8_t { RGB565, RGB888, RGBA8888 };

constexpr uint32_t LIGHT = 0xEEEEEE;   ///< the ground under dark ink
constexpr uint32_t DARK  = 0x303030;   ///< …and under light ink
/// Ink at least this light (mean luma of what is opaque, 0..255) goes on the dark ground.
constexpr uint32_t LIGHT_INK = 158;

/// Where a picture lands in the square: x, y, w, h in the square's pixels.
struct Box {
    int x = 0, y = 0, w = 0, h = 0;
};

/// A `w` x `h` picture in a `side` x `side` square: the whole of it, as large as fits, centred.
/// An empty box for a picture with no size.
inline Box fit(int w, int h, int side) {
    Box b;
    if (w <= 0 || h <= 0 || side <= 0) return b;
    if (w >= h) {
        b.w = side;
        b.h = (int)(((int64_t)h * side + w / 2) / w);
    } else {
        b.h = side;
        b.w = (int)(((int64_t)w * side + h / 2) / h);
    }
    if (b.w < 1) b.w = 1;
    if (b.h < 1) b.h = 1;
    b.x = (side - b.w) / 2;
    b.y = (side - b.h) / 2;
    return b;
}

namespace detail {

inline int bytes_per(Layout l) { return l == Layout::RGB565 ? 2 : l == Layout::RGB888 ? 3 : 4; }

/// One pixel as 8-bit R, G, B and A.
inline void read(const uint8_t* p, Layout l, uint32_t& r, uint32_t& g, uint32_t& b, uint32_t& a) {
    switch (l) {
    case Layout::RGB565: {
        const uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
        r = ((v >> 11) & 0x1F) * 255 / 31;
        g = ((v >> 5) & 0x3F) * 255 / 63;
        b = (v & 0x1F) * 255 / 31;
        a = 255;
        break;
    }
    case Layout::RGB888:   r = p[0]; g = p[1]; b = p[2]; a = 255;  break;
    case Layout::RGBA8888: r = p[0]; g = p[1]; b = p[2]; a = p[3]; break;
    }
}

inline uint16_t pack(uint32_t r, uint32_t g, uint32_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

}  // namespace detail

/// The ground for this picture, 0xRRGGBB (see the file comment). `stride`: bytes from one row to
/// the next.
inline uint32_t ground(const uint8_t* px, Layout l, int w, int h, size_t stride) {
    if (!px || w <= 0 || h <= 0) return LIGHT;
    const int bpp = detail::bytes_per(l);
    uint32_t r, g, b, a;
    if (l == Layout::RGBA8888) {
        bool see_through = false;
        uint64_t luma = 0, alpha = 0;        // luma weighted by alpha, in 1/256ths
        for (int y = 0; y < h; y++) {
            const uint8_t* row = px + (size_t)y * stride;
            for (int x = 0; x < w; x++, row += bpp) {
                detail::read(row, l, r, g, b, a);
                if (a < 255) see_through = true;
                luma  += (uint64_t)(77 * r + 150 * g + 29 * b) * a;
                alpha += a;
            }
        }
        if (see_through) {
            if (!alpha) return LIGHT;        // nothing to see at all
            return luma / alpha >= (uint64_t)LIGHT_INK * 256 ? DARK : LIGHT;
        }
    }
    // Opaque: the colour of its edge, the outermost rows and columns.
    uint64_t sr = 0, sg = 0, sb = 0, n = 0;
    const auto add = [&](int x, int y) {
        detail::read(px + (size_t)y * stride + (size_t)x * bpp, l, r, g, b, a);
        sr += r; sg += g; sb += b; n++;
    };
    for (int x = 0; x < w; x++) {
        add(x, 0);
        if (h > 1) add(x, h - 1);
    }
    for (int y = 1; y + 1 < h; y++) {
        add(0, y);
        if (w > 1) add(w - 1, y);
    }
    return (uint32_t)(((sr / n) << 16) | ((sg / n) << 8) | (sb / n));
}

/// The picture in `out`, `side` x `side` RGB565 (native order): fitted (fit()), averaged down,
/// its transparent pixels over `ground` (0xRRGGBB), and `ground` where it does not reach.
inline void render(const uint8_t* px, Layout l, int w, int h, size_t stride, uint16_t* out, int side,
                   uint32_t ground) {
    if (!out || side <= 0) return;
    const uint32_t gr = (ground >> 16) & 0xFF, gg = (ground >> 8) & 0xFF, gb = ground & 0xFF;
    const uint16_t fill = detail::pack(gr, gg, gb);
    for (int i = 0; i < side * side; i++) out[i] = fill;
    const Box box = fit(w, h, side);
    if (!px || box.w <= 0) return;
    const int bpp = detail::bytes_per(l);
    for (int oy = 0; oy < box.h; oy++) {
        const int y0 = (int)((int64_t)oy * h / box.h);
        int y1 = (int)((int64_t)(oy + 1) * h / box.h);
        if (y1 <= y0) y1 = y0 + 1;
        for (int ox = 0; ox < box.w; ox++) {
            const int x0 = (int)((int64_t)ox * w / box.w);
            int x1 = (int)((int64_t)(ox + 1) * w / box.w);
            if (x1 <= x0) x1 = x0 + 1;
            // Colour premultiplied by alpha, so a transparent pixel adds nothing but ground.
            uint64_t sr = 0, sg = 0, sb = 0, sa = 0;
            for (int y = y0; y < y1; y++) {
                const uint8_t* p = px + (size_t)y * stride + (size_t)x0 * bpp;
                for (int x = x0; x < x1; x++, p += bpp) {
                    uint32_t r, g, b, a;
                    detail::read(p, l, r, g, b, a);
                    sr += (uint64_t)r * a;
                    sg += (uint64_t)g * a;
                    sb += (uint64_t)b * a;
                    sa += a;
                }
            }
            const uint64_t full = 255ull * (uint64_t)(y1 - y0) * (uint64_t)(x1 - x0);
            const uint64_t rest = full - sa;
            out[(box.y + oy) * side + box.x + ox] =
                detail::pack((uint32_t)((sr + gr * rest + full / 2) / full),
                             (uint32_t)((sg + gg * rest + full / 2) / full),
                             (uint32_t)((sb + gb * rest + full / 2) / full));
        }
    }
}

}  // namespace cover_thumb
