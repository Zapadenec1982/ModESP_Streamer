/**
 * @file ui_pick.cpp
 * @brief The list a choice is made from, over everything: the presets and an equalizer band's
 *        type (a list, not a button that steps through the six — each step would change the
 *        response on the way).
 *
 * A dialog as Налаштування's language is: the title with «Скасувати» beside it, the items as rows,
 * the current one filled, ticked and scrolled into sight. Under the list «Скасувати» would take
 * a strip of 100 px — a row and a half of the list; in the title's row it costs nothing. The
 * rows are a finger's 64 px (ui_m3_widgets.h). A short list gets a card its size; a long one
 * shows four rows and half of the fifth, through its name — a list cut between two rows looks
 * like all there is — and scrolls, from a row or between them. A row chooses and closes it;
 * «Скасувати» or a tap beside it close it with nothing chosen. The knob stays the volume while it
 * is up (ui_shell_encoder).
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"

namespace {

using i18n::S;
using i18n::tr;

constexpr int32_t PICK_W = 480, PICK_H = 388, PICK_ROW_H = 64;
constexpr int32_t PICK_PAD    = 24;        ///< m3w::dialog's card, on every side
constexpr int32_t PICK_LIST_Y = 48;        ///< under the title's row and «Скасувати»'s 64 px centred on it
constexpr int32_t PICK_FOOT   = 8;         ///< under the list, over the card's padding
/// the list of a long one, from the title's row to the card's foot (m3w::dialog makes the card 16 px
/// taller than asked)
constexpr int32_t PICK_LIST_H = PICK_H + 16 - 2 * PICK_PAD - PICK_LIST_Y - PICK_FOOT;
static_assert(PICK_LIST_H > 3 + 4 * (PICK_ROW_H + 2) + PICK_ROW_H / 3 &&
              PICK_LIST_H < 3 + 4 * (PICK_ROW_H + 2) + PICK_ROW_H * 2 / 3, "the fifth row is cut through its name");

struct Pick {
    m3w::Dialog d;
    lv_obj_t*   list = nullptr;
    lv_obj_t*   row[UI_PICK_MAX] = {};
    int         n = 0;
    bool        shown = false;
    int         current = -1;          ///< the row that is on, as the host has it
    void      (*chosen)(int) = nullptr;
};
// in PSRAM (uih::in_psram): internal RAM is the Wi-Fi co-processor's and the I2S descriptors'
Pick& pick = uih::in_psram<Pick>();

/// Hide first, then choose: what the host does with it may build things over the list.
void choose(int i) {
    void (*chosen)(int) = pick.chosen;
    ui_pick_hide();
    if (chosen && i >= 0 && i < pick.n) chosen(i);
}

void row_cb(lv_event_t* e) { choose((int)(intptr_t)lv_event_get_user_data(e)); }
void cancel_cb(lv_event_t*) { ui_pick_hide(); }

}  // namespace

void ui_pick_build(lv_obj_t* scr) {
    pick = Pick{};
    pick.d = m3w::dialog(scr, PICK_W, PICK_H);
    lv_obj_add_flag(pick.d.card, LV_OBJ_FLAG_CLICKABLE);   // a tap on the card is not a tap beside it
    // «Скасувати» in the title's row, its 64 px centred on the title's line (dialog_show puts the
    // only button at the card's right end); the title stops short of it
    const int32_t line = lv_font_get_line_height(m3::theme().t.title_m);
    lv_obj_set_y(pick.d.no.obj, line / 2 - lv_obj_get_style_height(pick.d.no.obj, 0) / 2);
    lv_obj_set_width(pick.d.title, PICK_W - 2 * PICK_PAD - lv_obj_get_style_width(pick.d.no.obj, 0) - 8);
    pick.list = m3w::box(pick.d.card, 0, PICK_LIST_Y, PICK_W - 48, PICK_LIST_H);
    lv_obj_add_flag(pick.list, LV_OBJ_FLAG_CLICKABLE);     // a finger between the rows scrolls it too
    lv_obj_add_flag(pick.list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(pick.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(pick.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(pick.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(pick.list, 3, 0);
    lv_obj_set_style_pad_row(pick.list, 2, 0);
    lv_obj_add_event_cb(pick.d.no.obj, cancel_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(pick.d.scrim, cancel_cb, LV_EVENT_CLICKED, nullptr);   // a tap beside it
}

void ui_pick_show(const char* title, const char* const* items, int n, int current, void (*chosen)(int index)) {
    if (!pick.list) return;
    const auto& t = m3::theme();
    lv_obj_clean(pick.list);
    pick.n = n < UI_PICK_MAX ? n : UI_PICK_MAX;
    lv_obj_t* on_row = nullptr;
    for (int i = 0; i < pick.n; i++) {
        const bool on = i == current;
        lv_obj_t* r = m3w::box(pick.list, 0, 0, PICK_W - 48 - 6, PICK_ROW_H);
        lv_obj_set_style_radius(r, m3::shape::M, 0);
        lv_obj_set_style_bg_color(r, m3::col(t.c.secondary_container), 0);
        lv_obj_set_style_bg_opa(r, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_t* name = m3w::text(r, t.t.body_l, on ? t.c.on_secondary_container : t.c.on_surface,
                                   items[i] ? items[i] : "");
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 16, 0);
        if (on) {
            lv_obj_t* tick = m3w::icon(r, t.t.icon_m, t.c.on_secondary_container, UI_SYM_CHECK);
            lv_obj_align(tick, LV_ALIGN_RIGHT_MID, -16, 0);
            on_row = r;
        }
        pick.row[i] = r;
    }
    pick.current = current;
    pick.chosen = chosen;
    // the card the size of a short list; a long one's is the most there is, the fifth row cut
    const int32_t rows = 6 + pick.n * (PICK_ROW_H + 2) - 2;          // pad_all 3, pad_row 2
    const int32_t list_h = rows < PICK_LIST_H ? rows : PICK_LIST_H;
    lv_obj_set_height(pick.list, list_h);
    lv_obj_set_height(pick.d.card, 2 * PICK_PAD + PICK_LIST_Y + list_h + PICK_FOOT);
    lv_obj_center(pick.d.card);
    m3w::dialog_show(pick.d, title, "", "", tr(S::common_cancel));
    pick.shown = true;
    lv_obj_update_layout(pick.list);
    if (on_row) {
        // The chosen row second from the top, the one above it whole: scrolled into view alone it
        // came to rest at the foot of the three and a half rows, and the top one was cut through
        // its name. Held to the list's ends.
        const int32_t want = lv_obj_get_y(on_row) - (PICK_ROW_H + 2) - 3;
        const int32_t end  = lv_obj_get_scroll_bottom(pick.list);       // from the top: nothing scrolled yet
        lv_obj_scroll_to_y(pick.list, want < 0 ? 0 : want > end ? end : want, LV_ANIM_OFF);
    }
}

bool ui_pick_shown() { return pick.shown; }

const lv_obj_t* ui_pick_scrim() { return pick.d.scrim; }

void ui_pick_hide() {
    if (!pick.shown) return;
    m3w::dialog_hide(pick.d);
    pick.shown = false;
}
