/**
 * @file station_badge.h
 * @brief A station's stand-in for a logo: a disc in a colour of its own, with a short label.
 *        No LVGL.
 *
 * The Player's tile shows what a station plays — the track's cover, else the station's logo
 * (modules/radio/src/art.cpp) — and for a station with neither, this: a disc whose colour comes
 * from the name, so a station keeps it from day to day and from the tile to its preset button,
 * and a label made from the name. The presets show the same disc, smaller, with the label's
 * first character (docs/UI.md, "Panel pages").
 *
 * The label is the first word that says something — leading numbers and frequencies ("106.5")
 * and the word "radio", which so many names start with, skipped — when it has at most
 * LABEL_MAX characters: "KISS" for "106.5 Kiss FM", "SWISS" for "Radio Swiss Jazz". A longer
 * word gives the initials of the name's words instead, at most INITIALS of them: "DK" for
 * "Deutschlandfunk Kultur"; a word chopped to five letters reads as a typo, initials as a
 * monogram. Upper case, Latin and Cyrillic alike.
 *
 * Split out so the rules are pinned on the host (tests/host/test_station_badge.cpp).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace station_badge {

constexpr int    LABEL_MAX = 5;                  ///< characters of a word that is the label
constexpr int    INITIALS  = 3;                  ///< initials, at most
constexpr size_t LABEL_CAP = 4 * LABEL_MAX + 1;  ///< bytes: five characters of up to four

/// The discs: eight hues at one lightness (CIELAB L* 44) and a muted chroma, so no station
/// shouts, white reads on every one of them (5.3:1 or better) and each stands off the dark
/// surfaces of all four schemes. Fixed rather than the scheme's: the colour is the station's.
constexpr uint32_t PALETTE[] = {0x9D5252, 0x8F5C38, 0x74682E, 0x467342,
                                0x18746B, 0x006F9E, 0x63629B, 0x92537C};
constexpr int      PALETTE_N = sizeof(PALETTE) / sizeof(PALETTE[0]);
constexpr uint32_t INK = 0xFFFFFF;               ///< the label, on every disc

struct Badge {
    char     label[LABEL_CAP] = "";   ///< the tile's: up to five characters, upper case
    char     first[5] = "";           ///< the presets': the label's first character
    uint32_t rgb = PALETTE[0];        ///< the disc, 0xRRGGBB
};

namespace detail {

/// FNV-1a over the bytes: stable across builds and machines, cheap, and spread enough over
/// eight colours.
inline uint32_t fnv1a(const char* s) {
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)s; p && *p; ++p) h = (h ^ *p) * 16777619u;
    return h;
}

/// The code point at `p` (UTF-8) and its length in bytes. A byte that starts no character, or a
/// character cut short, is one byte of U+FFFD.
inline uint32_t decode(const unsigned char* p, int& len) {
    const unsigned char c = p[0];
    const int more = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
    len = 1;
    if (more < 0) return 0xFFFD;
    uint32_t cp = more ? (uint32_t)(c & (0x3F >> more)) : c;
    for (int i = 1; i <= more; ++i) {
        if ((p[i] & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    len = more + 1;
    return cp;
}

/// `cp` as UTF-8 into `out` (four bytes of room); the number of bytes.
inline int encode(uint32_t cp, char* out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

inline bool is_digit(uint32_t cp) { return cp >= '0' && cp <= '9'; }

/// A letter of a name: Latin, accented Latin, Cyrillic and the other alphabets. Spaces,
/// punctuation, dashes, symbols and emoji are what part the words.
inline bool is_letter(uint32_t cp) {
    if (cp < 0x80) return (cp | 0x20) >= 'a' && (cp | 0x20) <= 'z';
    if (cp < 0xC0 || cp == 0xD7 || cp == 0xF7) return false;   // Latin-1's signs: « » · ° × ÷
    if (cp >= 0x2000 && cp <= 0x2BFF) return false;            // punctuation, symbols, arrows
    if (cp >= 0x3000 && cp <= 0x303F) return false;            // CJK punctuation
    if (cp >= 0xFE00 && cp <= 0xFE0F) return false;            // variation selectors
    return cp != 0xFFFD && cp < 0x1F000;                       // a broken byte; emoji
}

/// The capital of `cp` in the scripts the panel's faces draw: ASCII, Latin-1, Latin Extended-A,
/// Romanian ș ț, Cyrillic with Ґ. Anything else comes back as it is.
inline uint32_t upper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 0x20;
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 0x20;   // à…þ
    if (cp == 0xFF) return 0x178;                                    // ÿ
    if (cp >= 0x100 && cp <= 0x17F) {
        if (cp == 0x131) return 'I';                                 // dotless ı
        if (cp == 0x17F) return 'S';                                 // long ſ
        if (cp == 0x138 || cp == 0x149) return cp;                   // ĸ ŉ have none
        // Pairs, the capital first — except from Ĺ to ň and from Ź to ž, where it is second.
        const bool capital_odd = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
        return (cp % 2 == (capital_odd ? 0u : 1u)) ? cp - 1 : cp;
    }
    if (cp == 0x219 || cp == 0x21B) return cp - 1;                   // ș ț
    if (cp >= 0x430 && cp <= 0x44F) return cp - 0x20;                // а…я
    if (cp >= 0x450 && cp <= 0x45F) return cp - 0x50;                // ѐ…џ: ё є і ї
    if (cp == 0x491) return 0x490;                                   // ґ
    return cp;
}

struct Word {
    const unsigned char* p = nullptr;
    int  chars  = 0;
    bool letter = false;   ///< has a letter: "106" and "5" of "106.5" have none
};

/// `w` in capitals, appended to `out` at `o` while it fits `cap`; at most `chars` characters.
inline void put_upper(const Word& w, int chars, char* out, size_t cap, size_t& o) {
    const unsigned char* p = w.p;
    for (int i = 0; i < chars && i < w.chars; ++i) {
        int len;
        const uint32_t cp = decode(p, len);
        p += len;
        char u[4];
        const int k = encode(upper(cp), u);
        if (o + (size_t)k >= cap) return;
        memcpy(out + o, u, (size_t)k);
        o += (size_t)k;
    }
}

/// The word "radio", which says nothing about which station it is: Latin, with the Portuguese
/// and Czech Á, and Cyrillic, with the Ukrainian І and the Russian И. Compared in capitals.
inline bool is_radio(const Word& w) {
    static constexpr uint32_t RADIO[][5] = {{'R', 'A', 'D', 'I', 'O'}, {'R', 0xC1, 'D', 'I', 'O'},
                                            {0x420, 0x410, 0x414, 0x406, 0x41E},
                                            {0x420, 0x410, 0x414, 0x418, 0x41E}};
    if (w.chars != 5) return false;
    for (const auto& word : RADIO) {
        const unsigned char* p = w.p;
        int i = 0;
        for (int len; i < 5 && upper(decode(p, len)) == word[i]; ++i) p += len;
        if (i == 5) return true;
    }
    return false;
}

}  // namespace detail

/// The disc for station `name`: its colour and its label (the file's comment has the rule).
/// An empty name, or one without a letter or a digit, gets an empty label.
inline Badge make(const char* name) {
    using namespace detail;
    Badge b;
    b.rgb = PALETTE[fnv1a(name) % PALETTE_N];
    // The words: runs of letters and digits. A name is at most 63 bytes, so 32 hold them all.
    Word w[32];
    int n = 0;
    const unsigned char* p = (const unsigned char*)(name ? name : "");
    while (*p && n < 32) {
        int len;
        uint32_t cp = decode(p, len);
        if (!is_letter(cp) && !is_digit(cp)) { p += len; continue; }
        Word& x = w[n++];
        x.p = p;
        while (*p && (is_letter(cp = decode(p, len)) || is_digit(cp))) {
            x.letter = x.letter || is_letter(cp);
            x.chars++;
            p += len;
        }
    }
    // The word that names the station: the first with a letter that is not "radio"; else the
    // first with a letter ("Radio 1"); else the first ("538").
    int key = -1;
    for (int i = 0; i < n && key < 0; ++i) if (w[i].letter && !is_radio(w[i])) key = i;
    for (int i = 0; i < n && key < 0; ++i) if (w[i].letter) key = i;
    if (key < 0 && n > 0) key = 0;
    size_t o = 0;
    if (key >= 0 && w[key].chars <= LABEL_MAX) {
        put_upper(w[key], LABEL_MAX, b.label, sizeof(b.label), o);
    } else if (key >= 0) {
        for (int i = 0, k = 0; i < n && k < INITIALS; ++i) {
            if (!w[i].letter) continue;
            put_upper(w[i], 1, b.label, sizeof(b.label), o);
            ++k;
        }
    }
    b.label[o] = '\0';
    int len = 0;
    if (b.label[0]) decode((const unsigned char*)b.label, len);
    memcpy(b.first, b.label, (size_t)len);
    b.first[len] = '\0';
    return b;
}

}  // namespace station_badge
