/**
 * @file ui_m3_widgets.h
 * @brief Material 3 components on plain LVGL objects, in the panel's own colours.
 *
 * Every screen of the redesign is built from these and from nothing else that sets a colour
 * or a radius. A component takes its roles from m3::theme() at construction; a scheme change
 * rebuilds the screens (as the palette switch always has), so no component keeps a colour.
 *
 * Geometry is in LOGICAL pixels (800x480 landscape), 8.54 px a millimetre. Whatever a finger
 * takes is at least 64 px (7.5 mm) each way (48 px would be 5.6 mm, under every guideline): the
 * slider's row, a chip, a list row, a dialog's buttons, the switch's reach. Nothing here animates by itself —
 * a morph is an lv_anim the caller starts on the object's radius, so a screen that must not
 * spend frames does not.
 */

#pragma once

#include "ui_m3.h"
#include "touch_gesture.h"
#include "lvgl.h"
#include <cstdint>

namespace m3w {

// ── surfaces ───────────────────────────────────────────────────────────────────────────────

/// Bare container: no background, border, padding, scroll or click. The layout box.
lv_obj_t* box(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h);

/// A tonal surface: flat fill, no border, no shadow. `radius` is one of m3::shape.
lv_obj_t* surface(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                  uint32_t color, int32_t radius);

/// Text in a type role and a colour role.
lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* txt);

/// A Material Symbol (UI_SYM_*) in an icon face; `fill` picks the filled face when the role
/// has one.
lv_obj_t* icon(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* sym);

/// Set a label only when the text changed; LVGL redraws even for equal text.
void set_text(lv_obj_t* lbl, char* cache, size_t cache_sz, const char* txt);

/// Show or hide.
void show(lv_obj_t* o, bool visible);

// ── buttons ────────────────────────────────────────────────────────────────────────────────

struct Button {
    lv_obj_t* obj   = nullptr;
    lv_obj_t* icon  = nullptr;   ///< may be null
    lv_obj_t* label = nullptr;   ///< may be null
};

/// Filled: primary on on_primary. The one button per screen that is THE action (play).
Button filled_button(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                     int32_t radius, const char* sym, const lv_font_t* sym_font,
                     const char* txt = nullptr);

/// Tonal: secondary_container. Prev / next, the segment that is on, the drawer's chosen row.
Button tonal_button(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                    int32_t radius, const char* sym, const lv_font_t* sym_font,
                    const char* txt = nullptr);

/// Text only, no container: primary text on nothing. The least of a group of actions —
/// "Забути всі" beside a list's caption.
Button button_text(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, const char* txt);

/// Icon-only, round, `size` px — 64 wherever a finger is meant to land: the header's ≡ and ←,
/// +/- in the band panel.
/// `outlined` adds the outline_var ring the design puts on the favourite button.
Button icon_button(lv_obj_t* parent, int32_t x, int32_t y, int32_t size,
                   const char* sym, const lv_font_t* sym_font, bool outlined = false);

/// Grey a button out (on_surface at 38 %) or bring it back; the object stays where it is.
void button_enable(Button& b, bool enabled);

// ── chips ──────────────────────────────────────────────────────────────────────────────────

constexpr int32_t CHIP_H = 64;

struct Chip {
    lv_obj_t* obj   = nullptr;
    lv_obj_t* check = nullptr;   ///< the leading check mark, shown only when selected
    lv_obj_t* label = nullptr;
    bool      selected = false;
};

/// CHIP_H tall, radius XS. Unselected: outlined in outline_var, text on_surface_var.
/// Selected: secondary_container with a check mark. Width from the text, plus padding.
Chip chip(lv_obj_t* parent, int32_t x, int32_t y, const char* txt, bool selected);
void chip_set(Chip& c, bool selected);
/// Give the chip a leading glyph of its own (the source chip's icon) in place of the check
/// mark; it stays whether or not the chip is selected.
void chip_lead(Chip& c, const char* sym);
/// The chip's width after layout, so the next one can be placed after it.
int32_t chip_width(const Chip& c);

// ── switch ─────────────────────────────────────────────────────────────────────────────────

/// 52x32, the M3 shape: off = outlined track with a small knob, on = primary track with a
/// large on_primary knob carrying a check. Built on lv_switch, so LVGL owns the state and
/// LV_EVENT_VALUE_CHANGED fires as usual.
lv_obj_t* switch_m3(lv_obj_t* parent, int32_t x, int32_t y, bool on);
/// A row with a switch on it toggles from a tap anywhere on the row, as a list row does — the
/// switch's own 52×32 was the only place that took a finger. The switch's VALUE_CHANGED fires.
/// A switch alone reaches 16 px past its track: 84×64, where its parent has the room.
void switch_row(lv_obj_t* row, lv_obj_t* sw);

// ── slider ─────────────────────────────────────────────────────────────────────────────────

/// The one slider of the glass and the web: a SLIDER_RAIL px rail, the fill in primary from its
/// left end to a round SLIDER_KNOB px knob, the number to the right of the track; the icon at the
/// left when there is one. Бас, Високі and the balance are the same with the zero in the middle
/// (sound_sheet.cpp, screen_speakers.cpp); the web's is m3/Slider.svelte and m3/CenterSlider.svelte.
constexpr int32_t SLIDER_H    = 64;   ///< the row and the touch target
constexpr int32_t SLIDER_RAIL = 6;
constexpr int32_t SLIDER_KNOB = 32;
constexpr int32_t SLIDER_VAL  = 56;   ///< the number's column, right-aligned: "100 %"

struct Slider {
    lv_obj_t* obj    = nullptr;  ///< the row and the touch target, SLIDER_H tall
    lv_obj_t* rail   = nullptr;  ///< surface_highest
    lv_obj_t* fill   = nullptr;  ///< primary, from the rail's left end to the knob's centre
    lv_obj_t* knob   = nullptr;  ///< SLIDER_KNOB round, primary
    lv_obj_t* value  = nullptr;  ///< the number, at the right end of the row
    lv_obj_t* lead   = nullptr;  ///< the icon at the left end (volume glyph); may be null
    int32_t   r0 = 0;            ///< the rail's left end, in the row
    int32_t   k0 = 0, k1 = 0;    ///< the knob's centre at 0 and at 100: it never hangs off the rail
    int32_t   pct    = -1;
    const char* unit = "";       ///< after the number: "" or " %"
};

/// A slider `w` px wide, its icon at the left when `lead_sym` is given, the number and `unit`
/// at the right.
Slider slider(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, const char* lead_sym, const char* unit = "");
/// Move to 0..100. Cheap: two set_x/set_width calls and one label.
void slider_set(Slider& s, int32_t pct);
/// Map a touch x (in the slider's own coordinates) to 0..100.
int32_t slider_value_at(const Slider& s, int32_t local_x);

/// What a finger on a slider-like box asks for (slide_touch).
enum class Slid : uint8_t { NONE, MOVE, END };
/// A finger on a slider-like box, by touch_gesture.h: only a finger within touch::GRAB px of the
/// knob, centred at `knob_x` on screen, takes it; nothing on the press and nothing on a tap; the
/// slider's once it went touch::SLOP px along, more along than across — then a drift up or down no
/// longer hands it to the page — and the page's if it went up or down first. Call it from the
/// box's PRESSED, PRESSING, RELEASED and PRESS_LOST with the box's own touch::Slide; `value` is
/// the setting as the press finds it, `range` of it across `span` px. MOVE: `value` is where the
/// finger has taken the knob — from where it stood, never under the finger — not yet held to the
/// slider's limits. END: a drag let go.
Slid slide_touch(lv_event_t* e, touch::Slide& gesture, int32_t knob_x, int32_t& value, int32_t range,
                 int32_t span);
/// slide_touch for a Slider: MOVE puts 0..100 in `pct`.
Slid slider_touch(lv_event_t* e, Slider& s, touch::Slide& gesture, int32_t& pct);
/// slide_touch for an lv_slider, its knob where its value is: MOVE puts the new value, held to the
/// slider's range, in `value`; the caller sets it. Register it with LV_EVENT_PREPROCESS for
/// PRESSED, PRESSING, RELEASED and PRESS_LOST: it stops the event, so lv_slider's own handler —
/// which puts the knob under the finger, and on a release even without a drag — never runs.
Slid knob_touch(lv_event_t* e, touch::Slide& gesture, int32_t& value);

// ── lists ──────────────────────────────────────────────────────────────────────────────────

/// The pitch of a long list (VList), its rows 4 px apart: a row is ROW_H − 4 = 64 tall. Also the
/// settings row's height (list_row, the bench page's).
constexpr int32_t ROW_H = 68;

struct Row {
    lv_obj_t* obj   = nullptr;   ///< ROW_H tall, clickable
    lv_obj_t* icon  = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* sub   = nullptr;
    lv_obj_t* value = nullptr;   ///< trailing text; null when the row carries a switch
};

/// A settings row: leading icon, title over a subtitle, trailing value. Put a switch_m3 on
/// the right yourself when the row is a toggle (value stays null).
Row list_row(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, const char* sym,
             const char* title, const char* sub, const char* value);

// ── the long list ──────────────────────────────────────────────────────────────────────────
//
// Any length, a handful of objects: the rows float over an invisible spacer as tall as the
// whole list and are moved and refilled as it scrolls, so a card of two thousand tracks costs
// ten rows. Scrolling is LVGL's own — momentum, the elastic edge. A page may reserve a block
// of its own above the rows that scrolls with them (the stations' presets).

/// What one entry shows. The strings are copied into the row at once, so a fill callback may
/// point them into one scratch buffer it reuses for every row.
struct VItem {
    const char* lead    = nullptr;   ///< UI_SYM_* at the start, or null
    const char* title   = "";
    const char* sub     = nullptr;   ///< null or "" = one line, centred
    const char* trail   = nullptr;   ///< UI_SYM_* at the end, or null
    bool        on      = false;     ///< the chosen or playing entry: secondary_container
    bool        enabled = true;      ///< greyed and deaf when false
};

class VList {
public:
    using Fill = void (*)(int idx, VItem& out, void* user);
    using Pick = void (*)(int idx, void* user);

    /// `trail_pick` null: the trailing glyph is a mark. Set: it is a 64 px button with an
    /// action of its own, the row's full height. `head_h` > 0 reserves that much above the rows; build into head().
    /// `row_h`: the pitch, ROW_H.
    void build(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
               Fill fill, Pick pick, Pick trail_pick, void* user, int32_t head_h = 0,
               int32_t row_h = ROW_H);
    lv_obj_t* obj() const  { return scroller_; }
    lv_obj_t* head() const { return head_; }
    int  count() const     { return count_; }
    /// A new list: its length, and back to the top.
    void set_count(int n);
    /// The same list with entries changed in place — the playing one moved, a name arrived.
    void refill();
    /// Scroll so the entry is in view, a row below the top, unless it already is.
    void reveal(int idx);
    void scroll_top();

private:
    static constexpr int POOL = 10;   ///< rows that exist: a screenful and a margin each end
    struct Row {
        lv_obj_t* obj   = nullptr;
        lv_obj_t* lead  = nullptr;
        lv_obj_t* title = nullptr;
        lv_obj_t* sub   = nullptr;
        lv_obj_t* trail = nullptr;       ///< the button, or the mark itself
        lv_obj_t* trail_ic = nullptr;
        int       idx   = -1;
        /// What each label was last given, hashed (0: nothing yet). A LONG_DOT label keeps its
        /// ellipsis inside its text, so the text itself cannot be compared with a new one.
        uint32_t  seen_lead = 0, seen_title = 0, seen_sub = 0, seen_trail = 0;
    };
    static void scroll_cb(lv_event_t* e);
    static void row_cb(lv_event_t* e);
    static void trail_cb(lv_event_t* e);
    void place(bool force);
    void fill_row(Row& r, int idx);

    Row       row_[POOL];
    lv_obj_t* scroller_ = nullptr;
    lv_obj_t* head_     = nullptr;
    lv_obj_t* spacer_   = nullptr;
    Fill      fill_       = nullptr;
    Pick      pick_       = nullptr;
    Pick      trail_pick_ = nullptr;
    void*     user_       = nullptr;
    int32_t   w_ = 0, h_ = 0, head_h_ = 0, row_h_ = ROW_H;
    int       count_ = 0, first_ = -1;
    int32_t   last_sy_ = 0;      ///< the scroll position at the last scroll event
};

// ── text entry ─────────────────────────────────────────────────────────────────────────────
//
// The panel has no keyboard and a network password has to come from somewhere. What is typed
// here is what Wi-Fi passwords and device names are made of — Latin letters, digits and the
// ASCII signs — so that is the whole keyboard: no Cyrillic page, no emoji, no predictions.

/// One line of text in an outlined field, lv_textarea underneath. Its cursor always shows:
/// on a screen with one field there is nothing else for focus to mean.
lv_obj_t* text_field(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
                     const char* placeholder, uint32_t max_len);

/// The keyboard, five rows: digits, then three rows of letters with shift and backspace,
/// then signs · space. A second page carries every ASCII sign. Types into `target`, a
/// text_field, which may be changed later.
lv_obj_t* keyboard(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, lv_obj_t* target);
void      keyboard_target(lv_obj_t* kb, lv_obj_t* target);
/// Back to lower-case letters: the page a new entry starts on.
void      keyboard_reset(lv_obj_t* kb);

// ── dialog ─────────────────────────────────────────────────────────────────────────────────

/// A question that must be answered before anything else: a scrim over `parent`, a card with
/// a title, a line or two of text and two buttons. Hidden until dialog_show(); the caller puts
/// its own click handlers on `yes` and `no` and hides it.
struct Dialog {
    lv_obj_t* scrim = nullptr;
    lv_obj_t* card  = nullptr;
    lv_obj_t* title = nullptr;
    lv_obj_t* body  = nullptr;
    Button    no, yes;
};
Dialog dialog(lv_obj_t* parent, int32_t w, int32_t h);
void   dialog_show(Dialog& d, const char* title, const char* body, const char* yes, const char* no);
void   dialog_hide(Dialog& d);

// ── modality ───────────────────────────────────────────────────────────────────────────────

/// Marks what takes the screen over while it is up — a dialog's scrim, a keyboard someone is
/// typing on — so that rebuilding the UI (a palette or a language changed from the web) can
/// wait for it to go instead of throwing away a question or half a password.
void mark_modal(lv_obj_t* o);
/// A marked object under `root` is showing (it and every parent not hidden). `except`, and what
/// is under it, is not looked at.
bool modal_shown(lv_obj_t* root, const lv_obj_t* except = nullptr);

// ── feedback ───────────────────────────────────────────────────────────────────────────────

/// Snackbar: inverse_surface, radius XS, 48 px, centred at the bottom. Hidden until
/// toast_show(); the caller hides it again after its own timeout.
lv_obj_t* toast(lv_obj_t* parent);
void      toast_show(lv_obj_t* t, const char* txt);

/// The wavy progress line of the design: a sine along `w` up to `fraction`, flat and dimmer
/// after it. `phase` moves the wave; a still line (paused) is phase held.
lv_obj_t* wave(lv_obj_t* parent, int32_t x, int32_t y, int32_t w);
void      wave_set(lv_obj_t* o, float fraction, float phase, bool live);
/// Forget every wave: called before the object tree is rebuilt (see uih::spectrum_reset).
void      wave_reset();

} // namespace m3w
