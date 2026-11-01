/**
 * @file ui_common.h
 * @brief What every screen file shares: the context, the layout grid, the sources, widget helpers.
 *
 * Screens are built once at startup and then shown/hidden, so navigation costs nothing at
 * run time and no screen has to cope with being half-constructed. Each one exposes a
 * UiScreenDef; ui_shell.cpp owns the top bar, the pop-out menu and the switching.
 *
 * All geometry below is in LOGICAL (post-rotation) pixels — 800x480 landscape — and comes
 * from the design. All colour and type comes from ui_theme.h. A screen file that
 * contains a raw hex colour or a bare font name is a bug.
 */

#pragma once

#include "ui_theme.h"
#include "ui_lvgl_module.h"
#include "fonts/ui_fonts.h"
#include "i18n.h"
#include "state_enums.h"   // audio.source and the other manifest values, generated from the manifests
#include "esp_heap_caps.h"
#include "lvgl.h"
#include <cassert>
#include <new>

// ── the 800x480 grid: a 48 px header, the content below it, nothing permanent beside ──
// No rail: it would take 88 px off every page for a menu that is wanted a few times a day.
// The one menu is the drawer, and it is modal.
namespace uig {
constexpr int32_t SCR_W    = 800;
constexpr int32_t SCR_H    = 480;
constexpr int32_t PAD      = 24;    ///< side gutter of the whole UI
constexpr int32_t HEAD_Y   = 16;    ///< header row: ≡ or ←, the title, Wi-Fi and the clock
constexpr int32_t HEAD_H   = 48;
constexpr int32_t CONT_X   = PAD;                      // 24
constexpr int32_t CONT_Y   = HEAD_Y + HEAD_H + 10;     // 74 — content top
constexpr int32_t CONT_W   = SCR_W - 2 * PAD;          // 752
constexpr int32_t CONT_H   = SCR_H - CONT_Y - 22;      // 384
/// The modal navigation drawer: 340 — «Налаштування» in the rows' headline
/// face is 223 px, and at 300 a row left it 212 (tools/i18n/font_metrics.py).
constexpr int32_t DRAWER_W = 340;
constexpr int32_t EDGE_W   = 18;    ///< invisible left-edge strip that opens the drawer
} // namespace uig

/// Everything a screen needs. One instance, owned by the module, lives in PSRAM.
struct UiCtx {
    UiLvglModule*            mod   = nullptr;  ///< to post requests back to the main loop
    lv_obj_t*                stack = nullptr;  ///< parent of every screen root
    UiLvglModule::Snapshot   s{};              ///< snapshot as of this refresh
    const UiTheme*           th    = nullptr;  ///< == &ui_theme(), cached per refresh
};

/// One screen. `build` returns its root (a child of ctx.stack, sized to the content column).
struct UiScreenDef {
    const char* id;                                  ///< matches `ui.screen`; the title is the shell's
    const char* icon;                                ///< UI_SYM_* for the menu
    lv_obj_t* (*build)(lv_obj_t* parent, UiCtx& c);
    void      (*refresh)(lv_obj_t* root, UiCtx& c);
    /// Optional: the page has just been brought up — land the list on what is playing. (No page
    /// takes the knob: it is the shell's, the same on every page — ui_shell_encoder.)
    void      (*shown)(lv_obj_t* root, UiCtx& c);
    /// Optional, for a page with a hierarchy of its own (the DLNA browser): the header's ←.
    /// Return true when it went up a level; false lets the shell leave the page.
    bool      (*back)(UiCtx& c);
};

// ── the sources ──────────────────────────────────────────────────────────────────────────
// What the glass makes of audio.source — the Player's face, the icon, the name — said once, as the
// web says it once (webui/src/stores/now.js): an entry for each of the manifest's values
// (modesp::gen::AUDIO_SOURCE_*; docs/CONCEPT.md, "`audio.source` values"), and DLNA, a track pushed
// to the radio.

/// A source as the glass tells them apart.
enum class UiSrc : uint8_t {
    OFF     = modesp::gen::AUDIO_SOURCE_OFF,
    TONE    = modesp::gen::AUDIO_SOURCE_TONE,
    AUX     = modesp::gen::AUDIO_SOURCE_AUX,
    BT      = modesp::gen::AUDIO_SOURCE_BT,
    CARD    = modesp::gen::AUDIO_SOURCE_FILE,
    RADIO   = modesp::gen::AUDIO_SOURCE_RADIO,
    AIRPLAY = modesp::gen::AUDIO_SOURCE_AIRPLAY,
    USB     = modesp::gen::AUDIO_SOURCE_USB,
    DLNA,                                       ///< the radio, playing a push (radio.mode "push")
};
static_assert((int)UiSrc::DLNA == modesp::gen::AUDIO_SOURCE_MAX + 1, "an entry for every audio.source value");

/// What the Player draws: a track with its position and transport — the card, DLNA, Bluetooth and
/// AirPlay alike — a station, or neither: the tone, AUX, USB (the computer is its own remote) and
/// the output off. NONE is a page's "nothing drawn yet".
enum class PlayerFace : int8_t { NONE = -1, EMPTY, TRACK, RADIO };

/// No name to give: tr() of either is "" (i18n.h).
constexpr i18n::S  UI_NO_NAME = i18n::S::COUNT;
constexpr i18n::SU UI_NO_CAPS = i18n::SU::COUNT;

struct UiSrcDef {
    PlayerFace  face;   ///< what the Player draws while it plays
    const char* sym;   ///< the icon: the Player's tile while nothing plays, the Sources page's card
    i18n::S     name;   ///< the standby face's line and the Sources page's card; none for the output
                        ///< off and the tone
    i18n::SU    caps;   ///< the line over the Player's block; none where the sound is played from
                        ///< nowhere — the tone, AUX, off — as on the web (stores/now.js fromKey)
};

/// What the glass makes of `src`.
constexpr UiSrcDef ui_src_def(UiSrc src) {
    using i18n::S;
    using i18n::SU;
    switch (src) {
    case UiSrc::OFF:     return { PlayerFace::EMPTY, UI_SYM_POWER,      UI_NO_NAME,     UI_NO_CAPS };
    case UiSrc::TONE:    return { PlayerFace::EMPTY, UI_SYM_GRAPHIC_EQ, UI_NO_NAME,     UI_NO_CAPS };
    case UiSrc::AUX:     return { PlayerFace::EMPTY, UI_SYM_AUX,        S::src_aux,     UI_NO_CAPS };
    case UiSrc::BT:      return { PlayerFace::TRACK, UI_SYM_BLUETOOTH,  S::src_bt,      SU::src_bt };
    case UiSrc::CARD:    return { PlayerFace::TRACK, UI_SYM_SD,         S::src_card,    SU::src_card };
    case UiSrc::RADIO:   return { PlayerFace::RADIO, UI_SYM_RADIO,      S::src_radio,   SU::src_radio };
    case UiSrc::AIRPLAY: return { PlayerFace::TRACK, UI_SYM_AIRPLAY,    S::src_airplay, SU::src_airplay };
    case UiSrc::USB:     return { PlayerFace::EMPTY, UI_SYM_USB,        S::src_usb,     SU::src_usb };
    case UiSrc::DLNA:    return { PlayerFace::TRACK, UI_SYM_CAST,       S::src_dlna,    SU::src_dlna };
    }
    return ui_src_def(UiSrc::OFF);
}

/// The audio.source value `src` plays from: its own, and the radio's for DLNA.
constexpr int32_t ui_src_value(UiSrc src) {
    return src == UiSrc::DLNA ? modesp::gen::AUDIO_SOURCE_RADIO : (int32_t)src;
}

/// What plays, as the glass tells it: audio.source `value`, and DLNA where the radio plays a push;
/// OFF for a value that is none of them (−1: a build without the audio module).
constexpr UiSrc ui_src(int32_t value, bool push) {
    if (value == modesp::gen::AUDIO_SOURCE_RADIO && push) return UiSrc::DLNA;
    if (value < modesp::gen::AUDIO_SOURCE_MIN || value > modesp::gen::AUDIO_SOURCE_MAX) return UiSrc::OFF;
    return (UiSrc)value;
}
inline UiSrc ui_src(const UiLvglModule::Snapshot& s) { return ui_src(s.source, s.network_push); }

/// The Player's face for what plays. The Player, its transport, the standby face and the module's
/// "music is playing" all ask this, rather than each spelling the rule again.
constexpr PlayerFace ui_face(int32_t value, bool push) { return ui_src_def(ui_src(value, push)).face; }
inline PlayerFace ui_face(const UiLvglModule::Snapshot& s) { return ui_face(s.source, s.network_push); }

// ── widget helpers ───────────────────────────────────────────────────────────────────────
// These exist so a screen file reads as layout, not as twenty lv_obj_set_style_* calls.

namespace uih {

/// Strip lv_obj_create's default chrome: no background, border, padding, scroll or clicks.
void bare(lv_obj_t* o);

/// Transparent, non-interactive container at (x,y) sized w*h — the usual layout box.
lv_obj_t* box(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h);

/// Solid rectangle (hairlines, meter cells, markers).
lv_obj_t* rect(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h,
               uint32_t color, int32_t radius = 0);

/// A "rich" card: vertical panel gradient, hairline border, drop shadow. `on` = selected.
lv_obj_t* card(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, bool on = false);

/// Text. `ls` is letter spacing — the design leans on it for small caps.
lv_obj_t* label(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* txt,
                int32_t ls = 0);

/// An icon glyph as a label (UI_SYM_*).
lv_obj_t* icon(lv_obj_t* parent, const lv_font_t* font, uint32_t color, const char* sym);

/// Outlined pill with a centred caption; `out_lbl` receives the caption for later recolouring.
lv_obj_t* pill(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, const char* txt,
               lv_obj_t** out_lbl);

/// Accent glow: a shadow-only object placed behind a hero element. Returns it so the caller
/// can fade it (the design kills the glow when muted).
lv_obj_t* glow(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius);

/// A vertically scrollable container: bare like box(), but with LVGL's own scrolling left
/// switched on, so a finger drags the contents and momentum and the elastic edge come for
/// free. Lists used to be drawn as a fixed set of rows moved by index, which cannot be
/// dragged and reimplements badly what the toolkit already does.
lv_obj_t* scroller(lv_obj_t* parent, int32_t x, int32_t y, int32_t w, int32_t h);

/// Set a label's text only when it actually changed — LVGL invalidates even for equal text.
void set_text(lv_obj_t* lbl, char* cache, size_t cache_sz, const char* txt);

// ── the spectrum row ──────────────────────────────────────────────────────────────────────
//
// ONE object that draws all 28 bars itself, and the reason is measured rather than tidy.
//
// A bar per object is 28 objects whose height and y both move on every frame, and LVGL
// invalidates once per property: four to five areas per bar, well past LV_INV_BUF_SIZE (32).
// Once that list overflows LVGL gives up on areas and repaints the WHOLE 800x480 screen, 25
// times a second, on core 1 — the same core the audio task and the Bluetooth resampler sit
// on. That is why the row visibly stalls in Bluetooth and not on the card, where the decoder
// is pinned to core 0.
//
// Drawn as one object it is one invalidation of 560x218 instead of a full screen: a third of
// the pixels, none of the object-tree walking, and the same picture. It helps in every mode.
constexpr int32_t SPEC_BARS  = 28;    ///< == audio_core::SPECTRUM_BANDS (asserted in the .cpp)
constexpr int32_t SPEC_PITCH = 20;    ///< bar to bar
constexpr int32_t SPEC_BAR_W = 10;    ///< the bar itself; the rest is the gap
constexpr int32_t SPEC_ROW_W = SPEC_BARS * SPEC_PITCH;

/// How wide a row is drawn. The defaults are the Player's strip; the look is the same in
/// every row, only the spacing differs.
struct SpecStyle {
    int32_t pitch = SPEC_PITCH;
    int32_t bar_w = SPEC_BAR_W;
};

/// Width of the bars as drawn, first bar's left edge to last bar's right edge.
constexpr int32_t spec_drawn_w(const SpecStyle& s) { return (SPEC_BARS - 1) * s.pitch + s.bar_w; }

/// Build the row. `base_y` is the BASELINE — bars grow upwards from it — and `h` the tallest
/// a bar may be. Returns the object; screens keep it and feed it with spectrum_set().
lv_obj_t* spectrum(lv_obj_t* parent, int32_t x, int32_t base_y, int32_t h,
                   const SpecStyle& style = SpecStyle{});

/// Hand the row a fresh frame of band levels in dBFS, or nullptr when there is none yet.
/// Redraws once, and only when a bar actually moved.
void spectrum_set(lv_obj_t* row, const float* db);

/// Forget every row. The rows live in a fixed table; a scheme change destroys the whole
/// object tree and builds it again, and without this the second build finds the table
/// full and gets no spectrum at all.
void spectrum_reset();

/// Show/hide without the caller remembering which flag means what.
void show(lv_obj_t* o, bool visible);

/// A screen's state in PSRAM rather than in internal RAM, which the Wi-Fi co-processor's
/// buffers and the I2S descriptors come out of — the radio keeps its own the same way
/// (radio_module.cpp). The LVGL task reads it at the screen's pace. For a namespace-scope
/// reference, `Now& n = uih::in_psram<Now>();`, so every use reads as before: static
/// constructors run once the heap exists.
template <class T> T& in_psram() {
    void* mem = heap_caps_calloc_prefer(1, sizeof(T), 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(mem);
    return *new (mem) T();
}

} // namespace uih

// ── screens ──────────────────────────────────────────────────────────────────────────────
namespace audio_core { struct EqDoc; }
/// Copy the EQ screen's working document — bands, "Свій", the preset. Call with the LVGL lock
/// held: the screen edits it from the LVGL task.
void ui_eq_copy_doc(audio_core::EqDoc& out);
/// Bumps on every edit on the page.
uint32_t ui_eq_revision();
/// The module put edit `revision` into the store, which is now at `store_rev`. Same lock rule.
void ui_eq_pushed(uint32_t revision, uint32_t store_rev);
/// A sound's name in the panel's language (audio_dsp/eq_presets.h index; screen_eq.cpp's table).
const char* ui_eq_preset_name(int preset);

/// «Звук» (sound_sheet.cpp): the side sheet over the Player.
/// Built once with the shell, on the screen; all of these with the LVGL lock held.
void ui_sound_build(lv_obj_t* scr, UiCtx& c);
void ui_sound_open();
void ui_sound_close();
bool ui_sound_is_open();
/// Follow the snapshot and the EQ store while it is open; nothing while it is shut.
void ui_sound_refresh(UiCtx& c);
/// The field that opens the sounds' list: outlined as a dropdown is, «Звучання» at the left, the
/// sound (`*value`, the caller's to set) and ▾ at the right. The caller adds the click.
lv_obj_t* ui_sound_field(lv_obj_t* parent, int32_t w, int32_t h, lv_obj_t** value);
/// The presets' list (ui_pick_show): the sounds offered while `now` plays, `now` ticked; a row
/// calls `chosen` with its preset after the list is gone.
void ui_sound_pick_show(int now, void (*chosen)(int preset));

/// The list a choice is made from (ui_pick.cpp): a dialog over everything, built once with the
/// shell, over the «Звук» sheet. `items` as rows, `current` ticked and in sight; a row calls
/// `chosen` with its index after the list is gone; «Скасувати» or a tap beside it close it with
/// nothing chosen. All of these with the LVGL lock held.
constexpr int UI_PICK_MAX = 9;
void ui_pick_build(lv_obj_t* scr);
void ui_pick_show(const char* title, const char* const* items, int n, int current, void (*chosen)(int index));
bool ui_pick_shown();
void ui_pick_hide();
/// The list's scrim — the object m3w::dialog marks modal — for a look past the list
/// (m3w::modal_shown's `except`): the return to the Player closes the list rather than wait for it.
const lv_obj_t* ui_pick_scrim();

extern const UiScreenDef UI_SCREEN_NOW;
extern const UiScreenDef UI_SCREEN_SRC;
extern const UiScreenDef UI_SCREEN_RADIO;     // the stations, with the presets
extern const UiScreenDef UI_SCREEN_CATALOG;   // Radio Browser: Топ · Країни · Жанри · Пошук
extern const UiScreenDef UI_SCREEN_LIBRARY;   // the card's tracks
extern const UiScreenDef UI_SCREEN_DLNA;
extern const UiScreenDef UI_SCREEN_EQ;
extern const UiScreenDef UI_SCREEN_SET;       // Налаштування
extern const UiScreenDef UI_SCREEN_DISPLAY;   // Налаштування → Екран
extern const UiScreenDef UI_SCREEN_XOVER;     // Налаштування → Кросовер (screen_xover.cpp)
extern const UiScreenDef UI_SCREEN_SPEAKERS;  // Налаштування → Колонки (screen_speakers.cpp)
extern const UiScreenDef UI_SCREEN_SOUND;     // Налаштування → Звук (sound_sheet.cpp)
extern const UiScreenDef UI_SCREEN_SVC;
extern const UiScreenDef UI_SCREEN_WIFI;      // Налаштування → Wi-Fi
extern const UiScreenDef UI_SCREEN_BT;        // Налаштування → Bluetooth
#if CONFIG_UI_M3_TEST_SCREEN
extern const UiScreenDef UI_SCREEN_M3TEST;   // the component test page, bench builds only
#endif

/// Build the whole UI on `scr` (the active LVGL screen). Called once, under the LVGL lock.
void ui_shell_build(lv_obj_t* scr, UiCtx& c);
/// Apply a fresh snapshot: top bar, the active screen, overlays.
void ui_shell_refresh(UiCtx& c);
/// Switch screens by id; returns false for an unknown id.
bool ui_shell_go(const char* id);
/// Currently shown screen id (never null).
const char* ui_shell_current();
/// Where the list that is open was opened from (a page id), and the list that led to it (null
/// when none): what ← climbs to. A rebuild of the UI restores them with the page.
const char* ui_shell_from();
const char* ui_shell_parent();
/// Show or hide the standby face. Call with the LVGL lock held.
void ui_shell_set_standby(bool on);
/// A dialog or a keyboard is up on the glass (m3w::mark_modal). A rebuild of the UI from off the
/// glass waits for it. Call with the LVGL lock held.
bool ui_shell_modal_open();
/// The id of the page `o` belongs to, or "shell" for the frame around them (header, drawer,
/// standby face, pairing dialog). For reports about what is on the glass.
const char* ui_shell_page_of(const lv_obj_t* o);
/// Bench aid for /api/screenshot: "drawer" opens the drawer, "sound" the «Звук» sheet, "standby" shows the face as if
/// asleep, "pair" the pairing dialog on a made-up request, "bt18" the Bluetooth page on a
/// made-up 1.8 satellite; null or "" puts back what it changed. Returns false for anything
/// else. The lock rules of ui_shell_set_standby apply.
bool ui_shell_peek(const char* what);
/// The Bluetooth page drawn from made-up 1.8 data (true), or from the satellite again (false).
void ui_bt_preview(bool on);

/// The frame meter's A/B (ui_frame_meter.cpp): true takes the Player's cover gradient away —
/// the page as it is with no cover — and false gives it back. Returns whether the page has a
/// cover's colour to paint. Call with the LVGL lock held.
bool ui_now_plain(bool plain);
/// The knob's press on any page, the Player's play button: play or pause for a track, the mute for
/// a station, which does not pause; nothing where there is nobody to command.
void ui_now_press();
/// Bring the player screen up. Choosing the card as a source is choosing to listen to
/// what is on it, so the list of it is where that choice should land.
void ui_shell_show_player();
/// The header's ≡ / ← pressed: the drawer on a section, back on a list (through the page's
/// own `back` first).
void ui_shell_nav();
/// Same, for the radio.
void ui_shell_show_radio();
/// The tab row heading the station list and the catalog — Улюблені · Топ · Країни · Жанри · Пошук —
/// with tab `on` chosen. A tap on another tab goes there.
lv_obj_t* ui_station_tabs(lv_obj_t* parent, int on);
/// The catalog page on tab 1 Топ, 2 Країни, 3 Жанри or 4 Пошук, each from its top.
void ui_catalog_open(int tab);
/// The six presets' gestures, as the Stations page has them (screen_radio.cpp) and the Player's
/// station face repeats them: a tap tunes preset `slot` — and starts it when it is the station
/// tuned but not playing — or, on an empty one, stores what plays; a long press stores what plays.
void ui_preset_tap(int slot);
void ui_preset_store(int slot);
/// Something plays that a preset can keep: a station of the list, or one from the catalog.
bool ui_preset_storable(const UiLvglModule::Snapshot& s);
/// One encoder update: `delta` detents since the last call, `event` 0 none / 1 click / 2 hold.
/// The knob has one job on every page, over every sheet and dialog: a turn is the volume, a
/// press plays and pauses (mutes a station), a hold opens the menu. What is on the glass is
/// chosen by touch.
/// Called from the LVGL refresh, which is the only place that may touch widgets.
void ui_shell_encoder(int32_t delta, uint8_t event, UiCtx& c);

/// «Скасувати» after a finger moved a sound setting (touch_gesture.h):
/// a snackbar over the pages for touch::UNDO_MS — `text`, «Бас: 0 › +3», and the button, which
/// calls `undo(value, arg)` once. Another note replaces it; ui_shell_undo_drop(key) takes down
/// the one `key` put up. The knob's change — the volume — has none.
/// `action` is the button's word when it is not «Скасувати» — «Повернути» after a reset.
void ui_shell_undo(int key, const char* text, void (*undo)(int32_t value, void* arg), int32_t value, void* arg,
                   const char* action = nullptr);
void ui_shell_undo_drop(int key);
/// What a snackbar is about: Бас, Високі, «Скинути «Власний»», the balance, or EQ band
/// UNDO_EQ_BAND + its index.
constexpr int UNDO_BASS = 1, UNDO_TREBLE = 2, UNDO_EQ_RESET = 3, UNDO_BALANCE = 4, UNDO_EQ_BAND = 16;

/// What has been done about the speakers, in a line — the web's speakerState() line (Колонки's
/// page and Налаштування's row say it the same way). Returns `out`.
const char* ui_spk_line(const UiLvglModule::Snapshot& s, char* out, size_t cap);
