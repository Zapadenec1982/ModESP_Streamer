/**
 * @file ui_search_kb.cpp
 * @brief The search keyboard: a button matrix with a page in the panel's language's letters
 *        and one in the other alphabet. See ui_search_kb.h.
 *
 * Built and styled the way m3w::keyboard is (ui_m3_widgets.cpp): the key caps take the text
 * face with the icon face behind it, so backspace draws from the same matrix as the letters,
 * and a key types on release, so a finger sliding across the board types nothing it did not
 * lift from.
 */

#include "ui_search_kb.h"
#include "ui_m3.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "fonts/ui_fonts.h"

#include <cstring>

namespace search_kb {
namespace {

// Two pages: the panel's language's letters first, and the other alphabet a key away —
// Ukrainian for English, German and Polish, Latin for Ukrainian. Station names come in both,
// whatever the panel speaks. The page key shows the other page's code, and the codes stay as they
// are; the space bar and the search key are words of the panel's language. The maps hold
// placeholders for those three, and each keyboard keeps its own copies with them put in (State,
// in LVGL's heap — PSRAM).
enum Layout : uint8_t { L_UK, L_EN, L_DE, L_PL, L_COUNT };
constexpr const char* CODE[L_COUNT] = {"УК", "EN", "DE", "PL"};
constexpr const char* KEY_SPACE  = "\x01";        // placeholders: never a key's own text
constexpr const char* KEY_SEARCH = "\x02";
constexpr const char* KEY_OTHER  = "\x03";        // the other page's code

// The Ukrainian layout (ЙЦУКЕН) with ґ and the apostrophe brought onto the board, as on a phone.
const char* const MAP_UK[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х", "ї", "\n",
    "ф", "і", "в", "а", "п", "р", "о", "л", "д", "ж", "є", "'", "\n",
    "ґ", "я", "ч", "с", "м", "и", "т", "ь", "б", "ю", UI_SYM_BACKSPACE, "\n",
    KEY_OTHER, "&", "-", KEY_SPACE, ".", KEY_SEARCH, "",
};
const char* const MAP_EN[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "z", "x", "c", "v", "b", "n", "m", UI_SYM_BACKSPACE, "\n",
    KEY_OTHER, "&", "-", KEY_SPACE, ".", KEY_SEARCH, "",
};
// German: QWERTZ, the umlauts where a German keyboard has them, ß beside the m.
const char* const MAP_DE[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "z", "u", "i", "o", "p", "ü", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "ö", "ä", "\n",
    "y", "x", "c", "v", "b", "n", "m", "ß", UI_SYM_BACKSPACE, "\n",
    KEY_OTHER, "&", "-", KEY_SPACE, ".", KEY_SEARCH, "",
};
// Polish: QWERTY with the nine letters on the board at the rows' ends, no AltGr to hold.
const char* const MAP_PL[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "ą", "ę", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "ł", "ó", "ś", "\n",
    "z", "x", "c", "v", "b", "n", "m", "ć", "ń", "ź", "ż", UI_SYM_BACKSPACE, "\n",
    KEY_OTHER, "&", "-", KEY_SPACE, ".", KEY_SEARCH, "",
};

constexpr lv_buttonmatrix_ctrl_t K  = (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG);
constexpr lv_buttonmatrix_ctrl_t BS = (lv_buttonmatrix_ctrl_t)(LV_BUTTONMATRIX_CTRL_CLICK_TRIG | 2);
constexpr lv_buttonmatrix_ctrl_t W2 = (lv_buttonmatrix_ctrl_t)(K | 2);
constexpr lv_buttonmatrix_ctrl_t W3 = (lv_buttonmatrix_ctrl_t)(K | 3);
constexpr lv_buttonmatrix_ctrl_t W4 = (lv_buttonmatrix_ctrl_t)(K | 4);
/// The search key in the primary colours: the one key that does the thing.
constexpr lv_buttonmatrix_ctrl_t GO = (lv_buttonmatrix_ctrl_t)(W3 | LV_BUTTONMATRIX_CTRL_CHECKED);

const lv_buttonmatrix_ctrl_t CTRL_UK[] = {
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K, BS,
    W2, K, K, W4, K, GO,
};
const lv_buttonmatrix_ctrl_t CTRL_EN[] = {
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, BS,
    W2, K, K, W4, K, GO,
};
const lv_buttonmatrix_ctrl_t CTRL_DE[] = {
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, BS,
    W2, K, K, W4, K, GO,
};
const lv_buttonmatrix_ctrl_t CTRL_PL[] = {
    K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K, K, K,
    K, K, K, K, K, K, K, K, K, K, K, BS,
    W2, K, K, W4, K, GO,
};

struct LayoutDef {
    const char* const*             map;
    size_t                         n;
    const lv_buttonmatrix_ctrl_t*  ctrl;
};
const LayoutDef LAYOUTS[L_COUNT] = {
    {MAP_UK, sizeof(MAP_UK) / sizeof(MAP_UK[0]), CTRL_UK},
    {MAP_EN, sizeof(MAP_EN) / sizeof(MAP_EN[0]), CTRL_EN},
    {MAP_DE, sizeof(MAP_DE) / sizeof(MAP_DE[0]), CTRL_DE},
    {MAP_PL, sizeof(MAP_PL) / sizeof(MAP_PL[0]), CTRL_PL},
};
constexpr size_t MAX_KEYS = sizeof(MAP_PL) / sizeof(MAP_PL[0]);   // the longest of the four
static_assert(sizeof(MAP_UK) <= sizeof(MAP_PL) && sizeof(MAP_EN) <= sizeof(MAP_PL) &&
              sizeof(MAP_DE) <= sizeof(MAP_PL), "MAX_KEYS is the longest map");

/// The panel's language's layout first, and the other alphabet second.
void pages_for(i18n::Lang l, Layout out[2]) {
    switch (l) {
    case i18n::Lang::EN: out[0] = L_EN; out[1] = L_UK; break;
    case i18n::Lang::DE: out[0] = L_DE; out[1] = L_UK; break;
    case i18n::Lang::PL: out[0] = L_PL; out[1] = L_UK; break;
    default:             out[0] = L_UK; out[1] = L_EN; break;
    }
}

struct State {
    lv_obj_t*   target = nullptr;
    Enter       enter  = nullptr;
    void*       user   = nullptr;
    uint8_t     page   = 0;              ///< 0 the language's layout, 1 the other alphabet
    Layout      layout[2];
    const char* map[2][MAX_KEYS];        ///< the two pages' maps with the words and codes in
};

const char* space_word()  { return i18n::tr(i18n::S::kb_space); }
const char* search_word() { return i18n::tr(i18n::S::cat_search); }

void localise(const LayoutDef& d, const char* other, const char** out) {
    for (size_t i = 0; i < d.n; i++) {
        const char* k = d.map[i];
        out[i] = k == KEY_SPACE ? space_word() : k == KEY_SEARCH ? search_word() : k == KEY_OTHER ? other : k;
    }
}

/// The key caps' face: the text face with the icon face behind it. A copy, because the
/// generated faces are const; it points at the same glyph data.
lv_font_t s_font;

void set_page(lv_obj_t* kb, uint8_t page) {
    auto* st = static_cast<State*>(lv_obj_get_user_data(kb));
    if (!st || page > 1) return;
    st->page = page;
    lv_buttonmatrix_set_map(kb, st->map[page]);
    lv_buttonmatrix_set_ctrl_map(kb, LAYOUTS[st->layout[page]].ctrl);
}

void key_cb(lv_event_t* e) {
    lv_obj_t* kb = static_cast<lv_obj_t*>(lv_event_get_target(e));
    auto* st = static_cast<State*>(lv_obj_get_user_data(kb));
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        lv_free(st);
        lv_obj_set_user_data(kb, nullptr);
        return;
    }
    if (!st) return;
    const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
    const char* key = lv_buttonmatrix_get_button_text(kb, id);
    if (!key) return;
    if (!strcmp(key, CODE[st->layout[1 - st->page]])) { set_page(kb, (uint8_t)(1 - st->page)); return; }
    if (!strcmp(key, search_word())) { if (st->enter) st->enter(st->user); return; }
    if (!st->target) return;
    if (!strcmp(key, UI_SYM_BACKSPACE)) { lv_textarea_delete_char(st->target); return; }
    lv_textarea_add_text(st->target, !strcmp(key, space_word()) ? " " : key);
}

}  // namespace

lv_obj_t* create(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                 lv_obj_t* target, Enter enter, void* user) {
    const auto& t = m3::theme();
    s_font = *t.t.title_m;
    s_font.fallback = t.t.icon_m;

    lv_obj_t* kb = lv_buttonmatrix_create(parent);
    lv_obj_set_pos(kb, x, y);
    lv_obj_set_size(kb, w, h);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_outline_width(kb, 0, 0);
    lv_obj_set_style_radius(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_set_style_pad_row(kb, 6, 0);
    lv_obj_set_style_pad_column(kb, 6, 0);

    lv_obj_set_style_bg_color(kb, m3::col(t.c.surface_high), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, m3::shape::XS, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_outline_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &s_font, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, m3::col(t.c.on_surface), LV_PART_ITEMS);
    // part | state as one selector, spelled out as in m3w::keyboard
    constexpr lv_style_selector_t ITEMS = static_cast<lv_style_selector_t>(LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, m3::col(t.c.secondary_container), ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, m3::col(t.c.on_secondary_container), ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, m3::col(t.c.primary), ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, m3::col(t.c.on_primary), ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(kb, 0, ITEMS | LV_STATE_FOCUS_KEY);

    auto* st = static_cast<State*>(lv_malloc(sizeof(State)));
    if (st) {
        st->target = target;
        st->enter  = enter;
        st->user   = user;
        st->page   = 0;
        pages_for(i18n::lang(), st->layout);
        localise(LAYOUTS[st->layout[0]], CODE[st->layout[1]], st->map[0]);
        localise(LAYOUTS[st->layout[1]], CODE[st->layout[0]], st->map[1]);
    }
    lv_obj_set_user_data(kb, st);
    set_page(kb, 0);
    lv_obj_add_event_cb(kb, key_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(kb, key_cb, LV_EVENT_DELETE, nullptr);
    m3w::mark_modal(kb);       // a rebuild waits while someone is typing a search
    return kb;
}

void reset(lv_obj_t* kb) {
    if (kb) set_page(kb, 0);
}

}  // namespace search_kb
