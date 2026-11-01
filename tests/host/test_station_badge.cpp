/**
 * @file test_station_badge.cpp
 * @brief A station's stand-in for a logo: the label and the colour.
 *
 * modules/ui_lvgl/include/station_badge.h: the first word that says something — numbers,
 * frequencies and the word "radio" skipped — when it is five characters or fewer, otherwise
 * the initials, at most three; upper case in Latin and Cyrillic; a colour from a stable hash of
 * the name over a palette white reads on.
 */

#include "doctest.h"
#include "../../modules/ui_lvgl/include/station_badge.h"

#include <cmath>
#include <string>

using station_badge::make;
using station_badge::PALETTE;
using station_badge::PALETTE_N;

namespace {

std::string label(const char* name) { return make(name).label; }

/// WCAG relative luminance of 0xRRGGBB.
double luminance(uint32_t rgb) {
    auto lin = [](uint32_t c) {
        const double v = c / 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin((rgb >> 16) & 0xFF) + 0.7152 * lin((rgb >> 8) & 0xFF) + 0.0722 * lin(rgb & 0xFF);
}

double contrast(uint32_t a, uint32_t b) {
    double la = luminance(a), lb = luminance(b);
    if (la < lb) { const double t = la; la = lb; lb = t; }
    return (la + 0.05) / (lb + 0.05);
}

}  // namespace

TEST_CASE("station badge: the first word that says something, when it is short") {
    CHECK(label("106.5 Kiss FM") == "KISS");        // the frequency skipped
    CHECK(label("Kiss FM") == "KISS");
    CHECK(label("BBC Radio 1") == "BBC");
    CHECK(label("Radio Swiss Jazz") == "SWISS");     // "radio" says nothing
    CHECK(label("Radio ROKS") == "ROKS");
    CHECK(label("Хіт FM") == "ХІТ");
    CHECK(label("Люкс FM") == "ЛЮКС");
    CHECK(label("Радіо НВ") == "НВ");
    CHECK(label("Radio Złote Przeboje") == "ZŁOTE"); // Latin Extended-A in capitals
    CHECK(label("ö1 Klassik") == "Ö1");               // a digit inside a word stays
    CHECK(label("101.ru: Relax") == "RU");
}

TEST_CASE("station badge: a longer word gives the initials, at most three") {
    CHECK(label("Deutschlandfunk Kultur") == "DK");
    CHECK(label("Deutschlandfunk") == "D");
    CHECK(label("SomaFM Groove Salad") == "SGS");
    CHECK(label("Radio Paradise Main") == "RPM");    // the word "radio" is an initial still
    CHECK(label("Радіо Релакс") == "РР");
    CHECK(label("Радио Рекорд") == "РР");
    CHECK(label("Rádio Comercial") == "RC");
    CHECK(label("Nightride FM") == "NF");
    CHECK(label("Paradise Radio Main Mix EU") == "PRM");
    CHECK(label("шансон") == "Ш");
}

TEST_CASE("station badge: names with nothing better to say") {
    CHECK(label("Radio 1") == "RADIO");              // no other word with a letter
    CHECK(label("радіо") == "РАДІО");
    CHECK(label("538") == "538");                     // no letter at all
    CHECK(label("") == "");
    CHECK(label(nullptr) == "");
    CHECK(label("— · —") == "");
    CHECK(label("\xF0\x9F\x8E\xB5 Chill") == "CHILL");   // an emoji is not a word
    CHECK(label("Jazz\xC2\xA0" "24") == "JAZZ");         // a no-break space parts words
    CHECK(label("Bad\xFF" "Byte") == "BAD");             // a byte that starts nothing parts them too
}

TEST_CASE("station badge: the presets' first character") {
    CHECK(std::string(make("Kiss FM").first) == "K");
    CHECK(std::string(make("Хіт FM").first) == "Х");    // two bytes, whole
    CHECK(std::string(make("Deutschlandfunk Kultur").first) == "D");
    CHECK(std::string(make("").first) == "");
}

TEST_CASE("station badge: every capital the faces draw") {
    using station_badge::detail::upper;
    CHECK(upper('a') == 'A');
    CHECK(upper(0xE9) == 0xC9);     // é
    CHECK(upper(0xF7) == 0xF7);     // ÷ is not a letter
    CHECK(upper(0xFF) == 0x178);    // ÿ
    CHECK(upper(0x105) == 0x104);   // ą
    CHECK(upper(0x142) == 0x141);   // ł: the capital is the odd one here
    CHECK(upper(0x17C) == 0x17B);   // ż
    CHECK(upper(0x161) == 0x160);   // š
    CHECK(upper(0x131) == 'I');     // ı
    CHECK(upper(0x219) == 0x218);   // ș
    CHECK(upper(0x44F) == 0x42F);   // я
    CHECK(upper(0x456) == 0x406);   // і
    CHECK(upper(0x457) == 0x407);   // ї
    CHECK(upper(0x454) == 0x404);   // є
    CHECK(upper(0x491) == 0x490);   // ґ
    CHECK(upper(0x410) == 0x410);   // А stays
}

TEST_CASE("station badge: one colour per name, from FNV-1a over its bytes") {
    // Pinned: another implementation of the rule must give the same colours.
    CHECK(make("SomaFM Groove Salad").rgb == PALETTE[7]);
    CHECK(make("SomaFM Drone Zone").rgb == PALETTE[4]);
    CHECK(make("Radio Paradise Main").rgb == PALETTE[2]);
    CHECK(make("Radio Paradise Mellow").rgb == PALETTE[3]);
    CHECK(make("Хіт FM").rgb == PALETTE[3]);
    CHECK(make("").rgb == PALETTE[5]);
    CHECK(make("Kiss FM").rgb == make("Kiss FM").rgb);
}

TEST_CASE("station badge: white reads on every disc, and every disc on a dark surface") {
    for (int i = 0; i < PALETTE_N; ++i) {
        CAPTURE(i);
        CHECK(contrast(PALETTE[i], station_badge::INK) >= 4.5);
        CHECK(contrast(PALETTE[i], 0x000000) >= 3.0);
        for (int j = 0; j < i; ++j) CHECK(PALETTE[i] != PALETTE[j]);
    }
}
