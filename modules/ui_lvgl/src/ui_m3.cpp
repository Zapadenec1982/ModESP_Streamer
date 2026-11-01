/**
 * @file ui_m3.cpp
 * @brief The four Material 3 schemes.
 *
 * Бурштин is copied from the `schemes` table of «P4 Player M3» (artifact export of
 * 24.09.2026) role for role, so a diff against the design is a straight comparison. Синя and
 * Зелена were moved by the customer on 25.09.2026 (darker; towards violet and olive) and are
 * generated from the colour they pointed at by tools/ui/m3_scheme.py, which run on the mock's
 * own seeds reproduces the mock's tables to within a few units; Червона, added the same day,
 * comes from it too. `error` is not in the design
 * table — the mock never draws a fault — and is Material's standard dark error tone.
 *
 * The type roles point at the Roboto and Material Symbols faces that tools/fonts/build_fonts.py
 * produces (fonts/ui_fonts.h).
 */

#include "ui_m3.h"
#include "fonts/ui_fonts.h"

namespace m3 {
namespace {

// The type roles, one face each (tools/fonts/build_fonts.py). One macro, so the three schemes
// cannot drift apart in type: they differ in colour and nothing else.
#define M3_TYPE {                                                         \
    /* label_s   */ &ui_font_500_16,                                      \
    /* label_m   */ &ui_font_500_18,                                      \
    /* body_m    */ &ui_font_400_18,                                      \
    /* body_l    */ &ui_font_400_20,                                      \
    /* title_s   */ &ui_font_400_20,                                      \
    /* title_m   */ &ui_font_400_24,                                      \
    /* headline  */ &ui_font_500_32,                                      \
    /* display   */ &ui_font_600_48,                                      \
    /* clock     */ &ui_font_300_160,                                     \
    /* icon_s    */ &ui_font_icons_20,                                    \
    /* icon_m    */ &ui_font_icons_24,                                    \
    /* icon_l    */ &ui_font_icons_28,                                    \
    /* icon_xl   */ &ui_font_icons_40,                                    \
    /* icon_m_fill  */ &ui_font_icons_24_fill,                            \
    /* icon_l_fill  */ &ui_font_icons_28_fill,                            \
    /* icon_xl_fill */ &ui_font_icons_40_fill,                            \
}

constexpr uint32_t ERROR_DARK = 0xFFB4AB;

const Theme THEMES[(int)Scheme::COUNT] = {
    // ── Бурштин — the mock's "amber", the scheme the panel has always worn ──
    {"amber",
     {0x17130D, 0x110E08, 0x1F1B14, 0x231F18, 0x2E2922, 0x39342C,
      0xEBE1D5, 0xD3C4B1, 0x9C8E7D, 0x4F4537,
      0xFFB95F, 0x472A00, 0x653E00, 0xFFDDB7,
      0x574327, 0xFADEBC,
      0xBFCD8E, 0x3F4A1A, 0xDBE9A8,
      0xEBE1D5, 0x352F28, ERROR_DARK},
     M3_TYPE},

    // ── Синя — darker, more saturated and towards violet than the mock's #9ECAFF (the
    //    customer's swatch, 25.09.2026). tools/ui/m3_scheme.py 7A82C0 ──
    {"blue",
     {0x131319, 0x0E0E14, 0x1B1B20, 0x1F1F25, 0x29292F, 0x34343A,
      0xE2E2E9, 0xC5C5D4, 0x8F909E, 0x454652,
      0x7A82C0, 0x092355, 0x36437B, 0xDFE0FF,
      0x44455B, 0xE0E0FB,
      0xEFB7CF, 0x66384E, 0xFFD8E8,
      0xE2E2E9, 0x303036, ERROR_DARK},
     M3_TYPE},

    // ── Зелена — darker and towards olive than the mock's #7DDBA5 (the customer's swatch,
    //    25.09.2026). tools/ui/m3_scheme.py 9A9973 ──
    {"green",
     {0x14140C, 0x0F0F05, 0x1C1C16, 0x20201A, 0x2B2A24, 0x35352F,
      0xE3E3DB, 0xC8C7B8, 0x929183, 0x48473B,
      0x9A9973, 0x262800, 0x474917, 0xE7E6AB,
      0x484832, 0xE5E4C9,
      0x99D2BB, 0x17503D, 0xB4EFD7,
      0xE3E3DB, 0x31312B, ERROR_DARK},
     M3_TYPE},

    // ── Червона — the customer's fourth, 25.09.2026: a cherry red, #771910. That is L* 27,
    //    too dark to be the primary itself (1.8:1 on the surface, and the icons, the slider
    //    and the EQ curve are drawn in it), so the primary is its hue and chroma at tone 52 —
    //    4.4:1, like the other two — and the seed's own depth is the primary container.
    //    tools/ui/m3_scheme.py 771910@52 ──
    {"red",
     {0x19110F, 0x160C09, 0x211A18, 0x251E1C, 0x302827, 0x3B3331,
      0xEBE0DE, 0xD6C2BE, 0x9F8D88, 0x53433F,
      0xC45D4A, 0x4F0B00, 0x832519, 0xFFDAD2,
      0x5C403A, 0xFEDBD3,
      0xD2C79A, 0x4E4721, 0xEEE3B5,
      0xEBE0DE, 0x362F2D, ERROR_DARK},
     M3_TYPE},
};

#undef M3_TYPE

Scheme s_active = Scheme::AMBER;

} // namespace

const Theme& theme()            { return THEMES[(int)s_active]; }
Scheme       scheme()           { return s_active; }
const Theme& theme_at(Scheme s) { return THEMES[(int)s < (int)Scheme::COUNT ? (int)s : 0]; }

bool set_scheme(Scheme s) {
    if ((int)s >= (int)Scheme::COUNT || s == s_active) return false;
    s_active = s;
    return true;
}

} // namespace m3
