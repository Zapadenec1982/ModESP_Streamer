/**
 * @file ui_search_kb.h
 * @brief A keyboard for a search line: the panel's language's letters or the other alphabet's,
 *        digits and a few signs.
 *
 * Station names are looked for in Ukrainian as
 * often as in English, so this is not m3w::keyboard, which is Latin and ASCII by design — what
 * Wi-Fi passwords are made of. No shift: Radio Browser matches a name without regard to case,
 * and a key fewer is a bigger key. The last row has the other page's code, the space and the
 * search key. The first page follows the panel's language — ЙЦУКЕН, QWERTY, QWERTZ
 * with the umlauts and ß, QWERTY with the Polish letters — and the second is Ukrainian, or Latin
 * for Ukrainian.
 */

#pragma once

#include "lvgl.h"
#include <cstdint>

namespace search_kb {

/// Called on "Шукати" with the `user` given to create().
using Enter = void (*)(void* user);

/// The keyboard at (x, y), w x h, typing into `target` (a text field); the panel's language first.
lv_obj_t* create(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                 lv_obj_t* target, Enter enter, void* user);

/// Back to the first page, the panel's language's: what a new search starts on.
void reset(lv_obj_t* kb);

}  // namespace search_kb
