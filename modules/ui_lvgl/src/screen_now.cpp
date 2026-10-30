/**
 * @file screen_now.cpp
 * @brief "Плеєр" — the home page: what is playing, from whichever source, and the controls
 *        that source answers to.
 *
 * One page, five faces — card, DLNA, radio, Bluetooth, AirPlay — and an empty one. The
 * layout does not move between them: the artwork tile on the left, the track block on the
 * right, the spectrum strip below. What changes is what the block says and which controls
 * are live; a control that does not apply is greyed, not hidden, so nothing shifts under a
 * finger that is already reaching for play.
 *
 * Over the block, where the sound comes from — «BLUETOOTH» and the phone's name — and «Звук»,
 * the sheet with the sounds, Бас, Високі and the loudness; the codec beside the artist; and
 * behind it all the cover's colour, fading into the surface.
 *
 * The list button leads to the list of the source that is playing — the card's library, the
 * radio's stations, the DLNA browser — each a page of its own whose ← comes back here. A list
 * is read when you want to change track and ignored the rest of the time, so it is one tap
 * away rather than on this page.
 *
 * Volume is not on this page: the encoder is the volume, and the pop-up slider the shell
 * shows for it can also be called up with the loudspeaker button (customer, 24.09.2026).
 *
 * Bluetooth with no phone is not a dead end: the line under «Телефон не підключено» says what a
 * phone will find — «видно як «MODEsp»» — or that new phones do not see the panel, and then the
 * transport, which has nobody to command, gives way to the one thing the panel can do about it:
 * «Увімкнути видимість» (an action, not a state; the customer,
 * 20.10.2026). The satellite cannot be asked to call a phone (LINK_PROTOCOL.md), so there is no
 * «connect».
 *
 * The knob's press is this page's play button, from any page (ui_now_play_pause): the knob has
 * one job everywhere — a turn is the volume, a press plays and pauses (customer, 18.10.2026).
 * For a day it rang this page's buttons in turn instead: a mode.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "esp_log.h"
#include "spectrum.h"
#include "playlist.h"
#include "cover.h"
#include "bt_settings.h"
#include <cstdio>
#include <cstring>

namespace {

using i18n::S;
using i18n::SU;
using i18n::tr;

// ── geometry, all relative to the 752x384 content column ──
constexpr int32_t COVER   = player_core::COVER_PX;           // 280: the tile the player decodes
constexpr int32_t COL_X   = COVER + 24;
constexpr int32_t COL_W   = uig::CONT_W - COL_X;             // 448
// For a finger (the glass for a finger, 20.10.2026: the row's buttons were 40, the seek bar's reach
// 28) within the cover's 280: the column ends where the cover does, the transport stands on that
// line, and the spectrum under them keeps its 88 (an 88-px transport that ran the column past the
// cover ate the spectrum and set the page crooked: the customer, 21.10.2026).
constexpr int32_t ROW_H   = 48;                              // the source line, «Звук» and the two buttons
constexpr int32_t ROW_BTN = 48;                              // «Звук», the list and the volume as drawn;
constexpr int32_t ROW_EXT = 8;                               // a finger takes 8 px more to the sides and below
constexpr int32_t TITLE_Y = ROW_H;
// The transport by the rules rather than by swings (the customer, 21.10.2026: 88, then bunched at
// the left, then flung to the column's ends): one height, the glass's 64 (7.5 mm, every row and
// button's); the main button set apart by its width and its fill, not its height — play 96, prev
// and next 64; one gap, 24 px (~3 mm), near enough to read as one group and far enough apart for
// a finger; the group centred under the progress bar, symmetric with it and its two times. On the
// card shuffle and repeat stand at the bar's two ends, and the five are 24 apart across the 448.
constexpr int32_t TRAN_H   = 64;
constexpr int32_t TRAN_GAP = 24;
constexpr int32_t PLAY_W   = 96;
static_assert(64 + TRAN_GAP + 64 + TRAN_GAP + PLAY_W + TRAN_GAP + 64 + TRAN_GAP + 64 == 448,
              "on the card the five fill the column at one gap");
constexpr int32_t TRAN_Y  = COVER - TRAN_H;                  // 216: bottom-aligned with the cover
constexpr int32_t COL_H   = COVER;
// the progress: the bar near the top of its box, the times under it, and the whole box the
// finger's target (a press reaches a box only inside its parent's)
constexpr int32_t PROG_H    = 50;
constexpr int32_t BAR_Y     = 4;
constexpr int32_t BAR_H     = 4;
constexpr int32_t SEEK_KNOB = 16;
// the spectrum strip: whatever the tile leaves below it, one self-drawing object
constexpr int32_t SPEC_X    = (uig::CONT_W - uih::SPEC_ROW_W) / 2;
constexpr int32_t SPEC_BASE = uig::CONT_H;
constexpr int32_t SPEC_H    = SPEC_BASE - COVER - 16;          // 88

enum class Face : int8_t { NONE = -1, EMPTY, TRACK, RADIO };

struct Now {
    lv_obj_t* page     = nullptr;   ///< the whole screen: the cover's colour behind everything
    lv_obj_t* root     = nullptr;   ///< the content box in it, where the stack holds every other page
    uint32_t  backdrop = 0;         ///< the colour it is painted (cover_backdrop), 0 for none
    bool      plain    = false;     ///< the frame meter took the gradient away (ui_now_plain)
    // artwork
    lv_obj_t* tile     = nullptr;
    lv_obj_t* tile_ic  = nullptr;
    lv_obj_t* cover_im = nullptr;
    // Two descriptors, one per slot of the store, used in turn. LVGL keys its image cache on
    // the source pointer, so alternating them means a new cover is never confused with the
    // previous one still sitting in the cache.
    lv_image_dsc_t cover_dsc[2] = {};
    uint8_t   cover_which = 0;
    uint32_t  cover_gen   = 0xFFFFFFFF;
    int       cover_source = -1;
    int       cover_tile   = -1;   ///< which CoverSource the image came from: gens are per tile
    // the row over the block: where it plays from, «Звук», the moon, the list and the volume
    lv_obj_t*   from     = nullptr;
    lv_obj_t*   from_src = nullptr;
    lv_obj_t*   from_dev = nullptr;
    m3w::Button sound, lev_btn, list_btn, vol_btn;
    int32_t     sound_w = 0;
    // the block
    lv_obj_t* title = nullptr;
    lv_obj_t* sub   = nullptr;
    lv_obj_t* badge = nullptr;      ///< the codec, after the artist
    // progress: a bar with times for a track, the buffer line for the radio
    lv_obj_t* prog     = nullptr;
    lv_obj_t* prog_hit = nullptr;   ///< the touch target: taller than the bar it drives
    touch::Slide prog_touch;        ///< a finger on it (touch_gesture.h)
    bool      seek_drag = false;     ///< a finger is scrubbing: the position's refresh leaves the bar
    lv_obj_t* prog_fill= nullptr;
    lv_obj_t* prog_knob= nullptr;   ///< at the fill's end, where the bar seeks
    lv_obj_t* t_pos    = nullptr;
    lv_obj_t* t_dur    = nullptr;
    lv_obj_t* wave     = nullptr;
    float     phase    = 0.0f;
    // transport
    lv_obj_t*   transport = nullptr;
    m3w::Button prev, play, next, rep, shuf;
    m3w::Button pick;               ///< the empty face's way to the sources
    m3w::Button visible;            ///< Bluetooth, no phone, new phones not let in: let them in
    int8_t    vis_shown = -1;
    // spectrum
    lv_obj_t* spec = nullptr;

    // change detection
    char  title_txt[128]  = "";
    char  sub_txt[210]    = "";
    char  fmt_txt[48]     = "";
    char  src_txt[24]     = "";
    char  dev_txt[32]     = "";
    char  pos_txt[12]     = "";
    char  dur_txt[12]     = "";
    Face    face    = Face::NONE;
    int32_t source  = -2;
    bool    network_push = false;
    int8_t  tran_live = -1;
    int8_t  playing = -1;
    int8_t  muted   = -1;             ///< what the volume button shows
    int8_t  leveler = -1;             ///< what the moon shows
    int32_t repeat  = -1;
    int8_t  shuffle = -1;
    UiCtx* ctx = nullptr;
};

Now& n = uih::in_psram<Now>();   // internal RAM is the Wi-Fi co-processor's and the I2S descriptors'

/// m:ss, with the minutes clamped so a nonsense duration cannot overrun the field.
void mmss(int32_t sec, char* out, size_t n_out) {
    if (sec < 0) sec = 0;
    long m = sec / 60;
    if (m > 999) m = 999;
    snprintf(out, n_out, "%ld:%02ld", m, (long)(sec % 60));
}

/// Halfway between two colours, RGB888.
uint32_t halfway(uint32_t a, uint32_t b) { return ((a >> 1) & 0x7F7F7Fu) + ((b >> 1) & 0x7F7F7Fu); }

void post(UiLvglModule::Req kind, int32_t v) {
    if (n.ctx && n.ctx->mod) n.ctx->mod->lvgl_post(kind, v);
}

// ── the sources, as the page names them ──
/// `caps`: the line over the block, the source's name in capitals; null where the sound is not
/// played from anywhere (the tone, AUX, off), as on the web (stores/now.js fromKey).
struct SourceFace { const char* caps; const char* sym; };
SourceFace source_face(int32_t src, bool push) {
    switch (src) {
    case 1:  return { nullptr,              UI_SYM_GRAPHIC_EQ };
    case 2:  return { nullptr,              UI_SYM_AUX };
    case 3:  return { tr(SU::src_bt),       UI_SYM_BLUETOOTH };
    case 4:  return { tr(SU::src_card),     UI_SYM_SD };
    case 5:  return push ? SourceFace{ tr(SU::src_dlna), UI_SYM_CAST } : SourceFace{ tr(SU::src_radio), UI_SYM_RADIO };
    case 6:  return { tr(SU::src_airplay),  UI_SYM_AIRPLAY };
    case 7:  return { tr(SU::src_usb),      UI_SYM_USB };
    default: return { nullptr,              UI_SYM_POWER };
    }
}

// ── interaction ──

/// The list button: the list of the source that is playing, each a page of its own.
void list_cb(lv_event_t*) {
    if (!n.ctx) return;
    const auto& s = n.ctx->s;
    if (s.source == 4)             ui_shell_go("library");
    else if (s.network_push)       ui_shell_go("dlna");
    else if (s.source == 5)        ui_shell_go("radio");
}
void src_cb(lv_event_t*)    { ui_shell_go("src"); }
/// «Увімкнути видимість»: the satellite's other settings as they are (BT_CONFIG_SET is whole).
void vis_cb(lv_event_t*) {
    bt_settings::Config cfg;
    bt_settings::config(cfg);
    if (cfg.known) bt_settings::request_config(cfg.radio, true, cfg.auto_reconnect, cfg.codecs_enabled);
}
void vol_cb(lv_event_t*)    { ui_shell_show_volume(); }
void sound_cb(lv_event_t*)  { ui_sound_open(); }
void lev_cb(lv_event_t*)    { post(UiLvglModule::Req::LEVELER, -1); }
void rep_cb(lv_event_t*)    { post(UiLvglModule::Req::REPEAT, -1); }
void shuf_cb(lv_event_t*)   { post(UiLvglModule::Req::SHUFFLE, -1); }

/// The transport speaks the source's language: player.cmd codes for a track (the module
/// forwards them to the phone or the DLNA queue), radio.cmd codes for a station. `which`: 0 prev,
/// 1 play/pause, 2 next. Read from the snapshot, not this page's buttons: the knob's press comes
/// here from every page, and only the page on the glass is refreshed.
void transport(const UiLvglModule::Snapshot& s, int which) {
    const bool is_radio = s.source == 5 && !s.network_push;
    const bool has_track = s.source == 4 || s.network_push || s.source == 3 || s.source == 6;
    if (!is_radio && !has_track) return;                 // the tone, AUX, off: nothing to command
    // nobody to command: no phone; an AirPlay sender that gave no remote (refresh's "live")
    if ((s.source == 3 && !s.bt_connected) || (s.source == 6 && !s.ap_remote)) return;
    if (is_radio) {
        const bool playing = strcmp(s.radio_state, "playing") == 0;
        post(UiLvglModule::Req::RADIO_CMD, which == 0 ? 4 : which == 2 ? 3 : playing ? 2 : 1);
    } else if (which == 1 && s.source == 4 && strcmp(s.play_state, "playing") != 0 &&
               strcmp(s.play_state, "paused") != 0) {
        // Play on a card with nothing open: the toggle only pauses and resumes a track that is
        // open, so from idle it did nothing. Start the track the title shows — the cursor's.
        post(UiLvglModule::Req::PLAY_QUEUE, player_core::playlist().cursor());
    } else {
        post(UiLvglModule::Req::TRANSPORT, which == 0 ? 5 : which == 2 ? 4 : 6);
    }
}

void tran_cb(lv_event_t* e) {
    if (n.ctx) transport(n.ctx->s, (int)(intptr_t)lv_event_get_user_data(e));
}

/// The knob's centre for a fill `w` px long: at its end, held inside the bar.
int32_t knob_centre(int32_t w) {
    return w < SEEK_KNOB / 2 ? SEEK_KNOB / 2 : w > COL_W - SEEK_KNOB / 2 ? COL_W - SEEK_KNOB / 2 : w;
}

/// The fill `w` px long, and the knob at its end.
void set_fill(int32_t w) {
    lv_obj_set_width(n.prog_fill, w);
    lv_obj_set_x(n.prog_knob, knob_centre(w) - SEEK_KNOB / 2);
}

/// The bar seeks or only reads out; the knob shows which.
void set_seekable(bool on) {
    if (on) lv_obj_add_flag(n.prog_hit, LV_OBJ_FLAG_CLICKABLE);
    else    lv_obj_remove_flag(n.prog_hit, LV_OBJ_FLAG_CLICKABLE);
    uih::show(n.prog_knob, on);
}

/// A finger that takes the progress bar's knob — where the music is — and goes along moves the
/// music with it, from there, so it scrubs; a press, a tap or a finger elsewhere on the bar does
/// not (m3w::slide_touch — a tap meant for the cover or the buttons must not jump the track).
/// The seek is cheap: the player moves the file position and lets the decoder find the next
/// frame header.
void seek_cb(lv_event_t* e) {
    const int32_t w = lv_obj_get_width(n.prog_hit);
    lv_area_t a;
    lv_obj_get_coords(n.prog_hit, &a);
    int32_t x = lv_obj_get_width(n.prog_fill);
    const m3w::Slid r = m3w::slide_touch(e, n.prog_touch, a.x1 + knob_centre(x), x, w, w);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) n.seek_drag = false;
    if (r != m3w::Slid::MOVE) return;
    n.seek_drag = true;
    const int32_t dur = n.ctx ? n.ctx->s.play_dur_s : 0;
    if (dur <= 0) return;
    if (w <= 0) return;
    if (x < 0) x = 0; else if (x > w) x = w;
    // Drawn at once, before the player has been anywhere: a bar that waits half a second
    // for the position to catch up feels broken even when it is not.
    set_fill(x < 2 ? 2 : x);
    post(UiLvglModule::Req::SEEK, (int32_t)((int64_t)dur * x / w));
}

// ── build ──
lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    n = Now{};
    n.ctx = &c;

    // The page is the whole screen, not the content box: behind everything goes the cover's
    // colour, and that has to reach the edges and run under the header. So it is a child of the
    // screen, built between the page stack and the header: LVGL clips an invalidation to the
    // parent whether or not the parent lets its children overflow (lv_obj_area_is_visible), and a
    // page that outgrew the stack would repaint only inside it. The shell shows and hides it as it
    // does every page — it is what build() returns. The colour is a two-stop gradient — all this LVGL is built for
    // (LV_GRADIENT_MAX_STOPS) — under the picture on the left, into the surface on the right; on
    // this page only, since behind a list it would cost the list its accelerated scroll
    // (ppa_flush.cpp, plain_backdrop).
    n.page = uih::box(lv_obj_get_screen(parent), 0, 0, uig::SCR_W, uig::SCR_H);
    lv_obj_set_style_bg_grad_color(n.page, m3::col(t.c.surface), 0);
    lv_obj_set_style_bg_grad_dir(n.page, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_main_stop(n.page, 0, 0);
    lv_obj_set_style_bg_grad_stop(n.page, 230, 0);
    lv_obj_t* root = uih::box(n.page, uig::CONT_X, uig::CONT_Y, uig::CONT_W, uig::CONT_H);
    n.root = root;

    // ── artwork: the tile, with the picture clipped to its corners ──
    n.tile = m3w::surface(root, 0, 0, COVER, COVER, t.c.surface_low, m3::shape::XXL);
    lv_obj_set_style_clip_corner(n.tile, true, 0);
    n.cover_im = lv_image_create(n.tile);
    lv_obj_set_pos(n.cover_im, 0, 0);
    uih::show(n.cover_im, false);
    n.tile_ic = m3w::icon(n.tile, t.t.icon_xl, t.c.outline, UI_SYM_MUSIC);
    lv_obj_center(n.tile_ic);

    lv_obj_t* col = uih::box(root, COL_X, 0, COL_W, COL_H);

    // ── the row over the block: where the sound comes from — «BLUETOOTH» and the phone — which
    //    leads to the sources, and «Звук», the sheet with the sounds, Бас, Високі and the
    //    loudness (sound_sheet.cpp) ──
    n.from = uih::box(col, 0, 0, COL_W, ROW_H);
    lv_obj_add_flag(n.from, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(n.from, src_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_set_flex_flow(n.from, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(n.from, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(n.from, 10, 0);
    n.from_src = m3w::text(n.from, t.t.label_s, t.c.on_surface_var, "");
    lv_obj_set_style_text_letter_space(n.from_src, 1, 0);
    n.from_dev = m3w::text(n.from, t.t.label_m, t.c.on_surface, "");
    lv_obj_set_flex_grow(n.from_dev, 1);
    lv_obj_set_height(n.from_dev, lv_font_get_line_height(t.t.label_m));
    lv_label_set_long_mode(n.from_dev, LV_LABEL_LONG_DOT);
    // «Звук» on the cover's colour rather than on a fill of its own: on_surface at a tenth
    n.sound = m3w::tonal_button(col, 0, 0, LV_SIZE_CONTENT, ROW_BTN, m3::shape::FULL, UI_SYM_EQ,
                                t.t.icon_s, tr(S::sys_sound));
    lv_obj_set_ext_click_area(n.sound.obj, ROW_EXT);
    lv_obj_set_style_bg_color(n.sound.obj, m3::col(t.c.on_surface), 0);
    lv_obj_set_style_bg_opa(n.sound.obj, LV_OPA_10, 0);
    lv_obj_set_style_bg_opa(n.sound.obj, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_pad_left(n.sound.obj, 14, 0);
    lv_obj_set_style_pad_right(n.sound.obj, 18, 0);
    lv_obj_set_style_text_color(n.sound.icon, m3::col(t.c.on_surface), 0);
    lv_obj_set_style_text_color(n.sound.label, m3::col(t.c.on_surface), 0);
    lv_obj_add_event_cb(n.sound.obj, sound_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_update_layout(n.sound.obj);
    n.sound_w = lv_obj_get_width(n.sound.obj);
    // «Вирівнювання гучності» right after «Звук» (place_row puts it there): a moon, filled in
    // the accent while it works, like shuffle and repeat
    n.lev_btn = m3w::icon_button(col, 0, 0, ROW_BTN, UI_SYM_MOON, t.t.icon_m);
    lv_obj_set_ext_click_area(n.lev_btn.obj, ROW_EXT);
    lv_obj_add_event_cb(n.lev_btn.obj, lev_cb, LV_EVENT_CLICKED, nullptr);
    // the two buttons at the row's end: the source's list, and the volume pop-up
    n.vol_btn = m3w::icon_button(col, COL_W - ROW_BTN - ROW_EXT, 0, ROW_BTN, UI_SYM_VOLUME, t.t.icon_m);
    lv_obj_add_event_cb(n.vol_btn.obj, vol_cb, LV_EVENT_CLICKED, nullptr);
    n.list_btn = m3w::icon_button(col, COL_W - 2 * (ROW_BTN + 2 * ROW_EXT) + ROW_EXT, 0, ROW_BTN, UI_SYM_PLAYLIST, t.t.icon_m, true);
    lv_obj_add_event_cb(n.list_btn.obj, list_cb, LV_EVENT_CLICKED, nullptr);
    // 48 px drawn, 64 wide for a finger and 56 tall (above them is the header's), the two side by
    // side with nothing between their reaches
    lv_obj_set_ext_click_area(n.vol_btn.obj, ROW_EXT);
    lv_obj_set_ext_click_area(n.list_btn.obj, ROW_EXT);

    // ── the block: title over two lines, the line under it ──
    const int32_t lh_title = lv_font_get_line_height(t.t.headline);
    const int32_t lh_sub   = lv_font_get_line_height(t.t.body_l);
    n.title = m3w::text(col, t.t.headline, t.c.on_surface, "");
    lv_obj_set_pos(n.title, 0, TITLE_Y);
    // A height as well as a width: LV_LABEL_LONG_DOT only has somewhere to put the ellipsis
    // once the box is bounded in both directions.
    lv_obj_set_size(n.title, COL_W, 2 * lh_title);
    lv_label_set_long_mode(n.title, LV_LABEL_LONG_DOT);
    const int32_t sub_y = TITLE_Y + 2 * lh_title + 2;   // 130
    n.sub = m3w::text(col, t.t.body_l, t.c.on_surface_var, "");
    lv_obj_set_pos(n.sub, 0, sub_y);
    lv_obj_set_size(n.sub, COL_W, lh_sub);
    lv_label_set_long_mode(n.sub, LV_LABEL_LONG_DOT);
    // the codec, after the artist: small and outlined, between the palette's two outlines
    n.badge = m3w::text(col, t.t.label_s, t.c.on_surface_var, "");
    lv_obj_set_style_border_width(n.badge, 1, 0);
    lv_obj_set_style_border_color(n.badge, m3::col(halfway(t.c.outline, t.c.outline_var)), 0);
    lv_obj_set_style_radius(n.badge, 6, 0);
    lv_obj_set_style_pad_hor(n.badge, 8, 0);
    lv_obj_set_style_pad_ver(n.badge, 1, 0);
    lv_obj_set_y(n.badge, sub_y + (lh_sub - lv_font_get_line_height(t.t.label_s) - 4) / 2);
    uih::show(n.badge, false);

    // ── progress: the bar (which is also the seek control) or the radio's buffer line ──
    const int32_t prog_y = sub_y + lh_sub + 3;          // 158: the box ends at 208, the transport is at 216
    n.prog = uih::box(col, 0, prog_y, COL_W, PROG_H);
    // A 4-pixel bar is a fine thing to look at and an impossible thing to hit, so the touch
    // target is the whole box, twelve times its height, with the times in it. It was a taller box
    // centred on the bar and reaching out of this one, but LVGL looks for a target only inside the
    // parent's box, so a finger above the bar found nothing: 28 px of 44 (20.10.2026). Where the
    // bar seeks, a knob at the fill's end shows where a finger takes it: knob only, as the
    // sliders (the customer, 16.10.2026: the seek was there, and nothing said where).
    n.prog_hit = uih::box(n.prog, 0, 0, COL_W, PROG_H);
    lv_obj_add_flag(n.prog_hit, LV_OBJ_FLAG_CLICKABLE);
    for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
        lv_obj_add_event_cb(n.prog_hit, seek_cb, code, nullptr);
    m3w::surface(n.prog, 0, BAR_Y, COL_W, BAR_H, t.c.surface_highest, 2);
    n.prog_fill = m3w::surface(n.prog, 0, BAR_Y, 2, BAR_H, t.c.primary, 2);
    n.prog_knob = m3w::surface(n.prog, 0, BAR_Y + BAR_H / 2 - SEEK_KNOB / 2, SEEK_KNOB, SEEK_KNOB,
                               t.c.primary, LV_RADIUS_CIRCLE);
    uih::show(n.prog_knob, false);
    n.t_pos = m3w::text(n.prog, t.t.label_s, t.c.on_surface_var, "0:00");
    lv_obj_set_pos(n.t_pos, 0, BAR_Y + BAR_H + 6);
    n.t_dur = m3w::text(n.prog, t.t.label_s, t.c.outline, "");
    lv_obj_align(n.t_dur, LV_ALIGN_TOP_RIGHT, 0, BAR_Y + BAR_H + 6);
    n.wave = m3w::wave(col, 0, prog_y - 6, COL_W);
    uih::show(n.wave, false);

    // ── transport: centred under the progress bar at one gap; the hidden take no room ──
    n.transport = uih::box(col, 0, TRAN_Y, COL_W, TRAN_H);
    lv_obj_set_flex_flow(n.transport, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(n.transport, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(n.transport, TRAN_GAP, 0);
    // the two that change what "next" means: the card's queue, nobody else's
    n.shuf = m3w::icon_button(n.transport, 0, 0, 64, UI_SYM_SHUFFLE, t.t.icon_m);
    lv_obj_add_event_cb(n.shuf.obj, shuf_cb, LV_EVENT_CLICKED, nullptr);
    n.prev = m3w::tonal_button(n.transport, 0, 0, 64, TRAN_H, m3::shape::M, UI_SYM_PREV, t.t.icon_l_fill);
    lv_obj_add_event_cb(n.prev.obj, tran_cb, LV_EVENT_CLICKED, (void*)(intptr_t)0);
    n.play = m3w::filled_button(n.transport, 0, 0, PLAY_W, TRAN_H, m3::shape::XL, UI_SYM_PLAY, t.t.icon_xl_fill);
    lv_obj_add_event_cb(n.play.obj, tran_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);
    n.next = m3w::tonal_button(n.transport, 0, 0, 64, TRAN_H, m3::shape::M, UI_SYM_NEXT, t.t.icon_l_fill);
    lv_obj_add_event_cb(n.next.obj, tran_cb, LV_EVENT_CLICKED, (void*)(intptr_t)2);
    n.rep = m3w::icon_button(n.transport, 0, 0, 64, UI_SYM_REPEAT, t.t.icon_m);
    lv_obj_add_event_cb(n.rep.obj, rep_cb, LV_EVENT_CLICKED, nullptr);
    // the empty face's one action
    n.pick = m3w::tonal_button(col, 0, TRAN_Y + (TRAN_H - 64) / 2, 220, 64, m3::shape::XS, UI_SYM_SPEAKER, t.t.icon_s, tr(S::nav_source));
    lv_obj_add_event_cb(n.pick.obj, src_cb, LV_EVENT_CLICKED, nullptr);
    uih::show(n.pick.obj, false);
    n.visible = m3w::tonal_button(col, 0, TRAN_Y + (TRAN_H - 64) / 2, LV_SIZE_CONTENT, 64, m3::shape::FULL,
                                  UI_SYM_BT_SEARCH, t.t.icon_m, tr(S::now_make_visible));
    lv_obj_set_style_pad_hor(n.visible.obj, 28, 0);
    lv_obj_add_event_cb(n.visible.obj, vis_cb, LV_EVENT_CLICKED, nullptr);
    uih::show(n.visible.obj, false);

    // ── spectrum: the strip under the tile ──
    n.spec = uih::spectrum(root, SPEC_X, SPEC_BASE, SPEC_H);

    // The page, not the content box in it: what build() returns is what the shell hides when
    // another page is chosen, and the page is the one that is opaque over the whole screen. Given
    // the box, the shell hid only that, and the cover's colour lay over every other page — the
    // Sources and the Settings showed as empty (customer, 14.10.2026).
    return n.page;
}

// ── refresh ──

/// Lay the row out: the buttons at its end — the volume always, the list when the source has
/// one, the moon — then «Звук», and the source line takes what is left.
void place_row() {
    const bool has_list = !lv_obj_has_flag(n.list_btn.obj, LV_OBJ_FLAG_HIDDEN);
    // each button's reach is ROW_EXT past what is drawn: «Звук»'s ends where the moon's begins
    const int32_t buttons = has_list ? 3 : 2;
    lv_obj_set_x(n.lev_btn.obj, COL_W - buttons * (ROW_BTN + 2 * ROW_EXT) + ROW_EXT);
    const int32_t sound_x = COL_W - buttons * (ROW_BTN + 2 * ROW_EXT) - ROW_EXT - n.sound_w;
    lv_obj_set_x(n.sound.obj, sound_x);
    lv_obj_set_width(n.from, sound_x - ROW_EXT);
}

/// The line under the title and the codec after it: the line is as wide as its text, so the
/// badge follows the artist, and never wider than leaves the badge on the page. The width is
/// the text's own, measured, not the label's: a cut label is as wide as its cut.
void place_meta() {
    const bool badge = !lv_obj_has_flag(n.badge, LV_OBJ_FLAG_HIDDEN);
    int32_t room = COL_W;
    if (badge) {
        lv_obj_update_layout(n.badge);
        room -= lv_obj_get_width(n.badge) + 12;
    }
    lv_point_t sz;
    lv_text_get_size(&sz, n.sub_txt, m3::theme().t.body_l, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t w = sz.x < room ? sz.x : room;
    lv_obj_set_width(n.sub, w > 1 ? w : 1);
    if (badge) lv_obj_set_x(n.badge, n.sub_txt[0] ? w + 12 : 0);
}

/// The title, over the line under it: a title of one line sits on that line instead of leaving
/// an empty line between them — the room goes above it, under the source's row.
void set_title(const char* txt) {
    if (!txt) txt = "";
    if (strncmp(n.title_txt, txt, sizeof(n.title_txt) - 1) == 0) return;
    uih::set_text(n.title, n.title_txt, sizeof(n.title_txt), txt);
    const lv_font_t* f = m3::theme().t.headline;
    const int32_t lh = lv_font_get_line_height(f);
    lv_point_t sz;
    lv_text_get_size(&sz, n.title_txt, f, 0, 0, COL_W, LV_TEXT_FLAG_NONE);
    const bool one = sz.y <= lh;
    lv_obj_set_y(n.title, one ? TITLE_Y + lh : TITLE_Y);
    lv_obj_set_height(n.title, one ? lh : 2 * lh);
}

void set_sub(const char* txt) {
    if (strncmp(n.sub_txt, txt, sizeof(n.sub_txt) - 1) == 0) return;
    uih::set_text(n.sub, n.sub_txt, sizeof(n.sub_txt), txt);
    place_meta();
}

/// The codec badge: the text when there is one, nothing when there is not — in the language's
/// units, and without the channel count where it is the norm (i18n::badge).
void set_fmt(const char* fmt) {
    char txt[48];
    i18n::badge(txt, sizeof(txt), fmt);
    if (strncmp(n.fmt_txt, txt, sizeof(n.fmt_txt) - 1) == 0) return;
    uih::set_text(n.badge, n.fmt_txt, sizeof(n.fmt_txt), txt);
    uih::show(n.badge, txt[0] != '\0');
    place_meta();
}

/// The page's colour: the cover's (cover_backdrop), or the plain surface when there is no
/// picture. The gradient itself is set once, in build().
void set_backdrop(uint32_t rgb) {
    if (rgb == n.backdrop) return;
    n.backdrop = rgb;
    if (rgb) lv_obj_set_style_bg_color(n.page, m3::col(rgb), 0);
    lv_obj_set_style_bg_opa(n.page, rgb && !n.plain ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

/// The satellite has told its settings and new phones cannot find it.
bool bt_hidden() {
    bt_settings::Config cfg;
    bt_settings::config(cfg);
    return cfg.known && !cfg.discoverable;
}

/// Bluetooth with no phone: what a phone would find — the name the panel is seen by — or that new
/// phones do not see it; nothing while the satellite has not said (or is not there: the title says).
void bt_wait_line(const UiLvglModule::Snapshot& s, char* out, size_t n) {
    out[0] = '\0';
    if (!s.bt_link) return;
    bt_settings::Config cfg;
    bt_settings::config(cfg);
    if (!cfg.known) return;
    if (!cfg.discoverable) {
        snprintf(out, n, "%s", tr(S::bt_visible_off));
        return;
    }
    char name[40];
    bt_settings::name(name, sizeof(name));
    if (name[0]) i18n::fmt(out, n, tr(S::manage_visible_as), {{"name", name}});
}

void set_face(Face f, const UiLvglModule::Snapshot& s) {
    const auto& t = m3::theme();
    n.face = f;
    const bool empty = (f == Face::EMPTY);
    const bool radio = (f == Face::RADIO);
    const bool is_file = (s.source == 4);
    uih::show(n.transport, !empty);
    n.vis_shown = -1;                             // the next refresh decides on «Увімкнути видимість»
    uih::show(n.pick.obj, empty);
    uih::show(n.prog, f == Face::TRACK);
    uih::show(n.wave, radio);
    uih::show(n.rep.obj, is_file);
    uih::show(n.shuf.obj, is_file);
    n.fmt_txt[0] = '\x01'; n.fmt_txt[1] = '\0';   // no text looks like this: the next refresh re-applies
    // The list button: the card's library, the radio's stations, the DLNA browser. Bluetooth
    // and AirPlay keep their lists on the phone.
    uih::show(n.list_btn.obj, is_file || radio || s.network_push);
    // The bar is a SEEK control only where seeking exists (CMD_TRANSPORT has no seek — only
    // ffwd/rew hold — so on Bluetooth it is a read-out; AirPlay's DACP remote has no seek).
    set_seekable(is_file || (s.network_push && s.network_seekable));
    // the tile's stand-in glyph and the source line
    const SourceFace sf = source_face(s.source, s.network_push);
    lv_label_set_text(n.tile_ic, empty ? sf.sym : UI_SYM_MUSIC);
    lv_obj_set_style_text_color(n.tile_ic, m3::col(empty ? t.c.outline : t.c.outline_var), 0);
    uih::set_text(n.from_src, n.src_txt, sizeof(n.src_txt), sf.caps ? sf.caps : "");
    uih::show(n.from, sf.caps != nullptr);
    n.tran_live = -1;
    n.playing = -1;
    place_row();
}

void refresh(lv_obj_t*, UiCtx& c) {
    const auto& t = m3::theme();
    const auto& s = c.s;
    const bool is_file = (s.source == 4);
    const bool is_bt   = (s.source == 3);
    const bool is_ap   = (s.source == 6);
    const bool is_radio = (s.source == 5 && !s.network_push);
    // Bluetooth and AirPlay have a track, a position and a transport like the card and DLNA,
    // so they are drawn by the same face; repeat and shuffle stay the card's.
    const bool has_track = is_file || s.network_push || is_bt || is_ap;
    const Face face = is_radio ? Face::RADIO : has_track ? Face::TRACK : Face::EMPTY;

    if (s.source != n.source || s.network_push != n.network_push || face != n.face) {
        n.source = s.source;
        n.network_push = s.network_push;
        n.cover_gen = 0xFFFFFFFF;
        set_face(face, s);
    }

    // Muted — the web's mute button does it; a turn of the knob ends it — shows on the Player for
    // as long as it lasts, not only in the volume pop-up for its two seconds.
    const int8_t muted = s.mute ? 1 : 0;
    if (muted != n.muted) {
        n.muted = muted;
        if (n.vol_btn.icon) lv_label_set_text(n.vol_btn.icon, muted ? UI_SYM_MUTE : UI_SYM_VOLUME);
    }
    const int8_t lev = s.leveler ? 1 : 0;
    if (lev != n.leveler && n.lev_btn.icon) {
        n.leveler = lev;
        lv_obj_set_style_text_font(n.lev_btn.icon, lev ? t.t.icon_m_fill : t.t.icon_m, 0);
        lv_obj_set_style_text_color(n.lev_btn.icon, m3::col(lev ? t.c.primary : t.c.on_surface_var), 0);
    }

    // ── the transport's mood ──
    // With no phone attached the buttons have nobody to command. An AirPlay sender can be
    // commanded only when it gave the receiver a remote: DACP, which AirPlay 1 senders do —
    // TuneBlade, Music Assistant, iTunes, an iPhone with the panel in AirPlay 1 mode. A phone
    // in AirPlay 2 mode gives none: its remote commands ride MRP, which the receiver does not
    // implement (components/airplay2/UPSTREAM.md).
    const int8_t live = face == Face::EMPTY ? 0
                      : is_ap ? (s.ap_remote ? 1 : 0)
                      : (!is_bt || s.bt_connected) ? 1 : 0;
    // Bluetooth, no phone, and new phones kept out: the transport gives way to letting them in
    const int8_t vis = face != Face::EMPTY && is_bt && !s.bt_connected && s.bt_link && bt_hidden() ? 1 : 0;
    if (vis != n.vis_shown) {
        n.vis_shown = vis;
        uih::show(n.visible.obj, vis != 0);
        uih::show(n.transport, face != Face::EMPTY && !vis);
    }
    if (live != n.tran_live) {
        n.tran_live = live;
        m3w::button_enable(n.prev, live != 0);
        m3w::button_enable(n.play, live != 0);
        m3w::button_enable(n.next, live != 0);
        lv_obj_set_style_opa(n.prog, live ? LV_OPA_COVER : LV_OPA_50, 0);
    }

    // ── artwork ──
    // The picture itself is decoded by the player, in hardware, once per track; all that
    // happens here is pointing the widget at whichever slot is current.
    {
        using player_core::CoverSource;
        // A station's cover is found by its ICY title (CoverSource::Radio, modules/radio/src/
        // art.cpp). A stream pushed over DLNA shows the art its control point sent, and the
        // title's cover when it sent none — a station tuned from the catalog arrives that way.
        // The rule is cover_now()'s, shared with GET /api/cover so the web shows this picture.
        CoverSource tile;
        const auto cv = player_core::cover_now(s.source, s.network_push, tile);
        if (cv.gen != n.cover_gen || n.cover_source != s.source || n.cover_tile != (int)tile) {
            n.cover_source = s.source;
            n.cover_tile = (int)tile;
            n.cover_gen = cv.gen;
            if (cv.px && cv.w > 0) {
                n.cover_which ^= 1;
                lv_image_dsc_t& d = n.cover_dsc[n.cover_which];
                d = lv_image_dsc_t{};
                d.header.magic  = LV_IMAGE_HEADER_MAGIC;
                d.header.cf     = LV_COLOR_FORMAT_RGB565;
                d.header.w      = (uint32_t)cv.w;
                d.header.h      = (uint32_t)cv.h;
                d.header.stride = (uint32_t)cv.w * 2;
                d.data          = (const uint8_t*)cv.px;
                d.data_size     = (uint32_t)cv.w * (uint32_t)cv.h * 2;
                lv_image_set_src(n.cover_im, &d);
                lv_obj_set_pos(n.cover_im, (COVER - cv.w) / 2, (COVER - cv.h) / 2);
                uih::show(n.cover_im, true);
                uih::show(n.tile_ic, false);
            } else {
                uih::show(n.cover_im, false);
                uih::show(n.tile_ic, true);
            }
            set_backdrop(cv.px ? player_core::cover_backdrop(cv.rgb) : 0);
        }
    }

    // the phone it plays from, beside «BLUETOOTH»; no other source names its sender
    uih::set_text(n.from_dev, n.dev_txt, sizeof(n.dev_txt), is_bt && s.bt_connected ? s.bt_peer : "");

    if (face == Face::RADIO) {
        const bool playing = strcmp(s.radio_state, "playing") == 0;
        // What is playing over who is playing it: the ICY title when the station sends one,
        // the station's name until it does — and the state in words while it is not playing,
        // rather than a blank line that reads as a fault.
        const char* title = s.radio_now[0] ? s.radio_now : s.radio_name[0] ? s.radio_name : tr(S::src_radio);
        const char* sub = s.radio_now[0] ? s.radio_name
                        : strcmp(s.radio_state, "connecting") == 0   ? tr(S::now_connecting)
                        : strcmp(s.radio_state, "buffering") == 0    ? tr(S::now_buffering)
                        : strcmp(s.radio_state, "reconnecting") == 0 ? tr(S::now_reconnecting)
                        : playing ? "" : tr(S::now_stopped);
        set_title(title);
        set_sub(sub);
        set_fmt(s.radio_fmt);
        // The buffer line: full and moving while the station plays, flat and still when not.
        if (playing) n.phase += 0.3f;
        const float fill = s.radio_buffer > 0 ? (float)s.radio_buffer / 100.0f : (playing ? 1.0f : 0.0f);
        m3w::wave_set(n.wave, fill, n.phase, playing);
        const int8_t pl = playing ? 1 : 0;
        if (pl != n.playing) {
            n.playing = pl;
            lv_label_set_text(n.play.icon, pl ? UI_SYM_STOP : UI_SYM_PLAY);
        }
    } else if (face == Face::TRACK) {
        const char* title = s.play_title;
        char cur_name[player_core::Playlist::MAX_NAME];
        if (!title[0] && is_file) {
            // Idle on the card: the queue still knows which track the cursor is on, and its
            // name is worth more than a dash.
            auto& q = player_core::playlist();
            const int cur = q.cursor();
            if (cur >= 0 && q.name_at(cur, cur_name, sizeof(cur_name))) title = cur_name;
        }
        set_title(title[0] ? title
                      // A phone that has just connected sends an empty TRACK_META and fills
                      // it in about ten seconds later (LINK_PROTOCOL.md), so "—" would sit
                      // there looking like a fault during the one moment the user is
                      // watching. Say which source it is until the phone says what is on it.
                      : is_bt ? (s.bt_connected ? tr(S::src_bt) : tr(S::now_no_device))
                      : is_ap ? (s.ap_connected ? tr(S::src_airplay) : tr(S::now_airplay_waiting))
                      : "—");
        // Artist and album when the file carries them, the position in the index when it
        // does not: the line is worth more as "who this is" than as a counter.
        char sub[210];
        if (s.play_artist[0] && s.play_album[0])
            snprintf(sub, sizeof(sub), "%s — %s", s.play_artist, s.play_album);
        else if (s.play_artist[0])
            snprintf(sub, sizeof(sub), "%s", s.play_artist);
        else if (s.play_album[0])
            snprintf(sub, sizeof(sub), "%s", s.play_album);
        else if (is_file && s.play_count > 0)
            snprintf(sub, sizeof(sub), "%ld / %ld", (long)(s.play_index + 1), (long)s.play_count);
        else if (is_bt && !s.bt_connected)
            bt_wait_line(s, sub, sizeof(sub));
        else if (is_ap && !s.ap_connected)
            snprintf(sub, sizeof(sub), "%s", tr(S::now_airplay_pick));
        else
            sub[0] = '\0';
        set_sub(sub);
        set_fmt(s.play_fmt);

        // progress
        if (s.network_push) set_seekable(s.network_seekable);
        char buf[12];
        mmss(s.play_pos_s, buf, sizeof(buf));
        uih::set_text(n.t_pos, n.pos_txt, sizeof(n.pos_txt), buf);
        if (s.play_dur_s > 0) {
            mmss(s.play_dur_s, buf, sizeof(buf));
            int32_t w = COL_W * s.play_pos_s / s.play_dur_s;
            if (w < 2) w = 2; else if (w > COL_W) w = COL_W;
            if (!n.seek_drag) set_fill(w);          // under a finger the bar is the finger's
        } else {
            // Duration is not known yet: say so with an empty field rather than draw a bar
            // that pretends to know how far through the track we are.
            buf[0] = '\0';
            set_fill(2);
        }
        uih::set_text(n.t_dur, n.dur_txt, sizeof(n.dur_txt), buf);

        const int8_t playing = (strcmp(s.play_state, "playing") == 0) ? 1 : 0;
        if (playing != n.playing) {
            n.playing = playing;
            lv_label_set_text(n.play.icon, playing ? UI_SYM_PAUSE : UI_SYM_PLAY);
        }
        if (s.repeat != n.repeat) {
            n.repeat = s.repeat;
            const int r = (s.repeat >= 0 && s.repeat <= 2) ? s.repeat : 0;
            lv_label_set_text(n.rep.icon, r == 1 ? UI_SYM_REPEAT_ONE : UI_SYM_REPEAT);
            lv_obj_set_style_text_color(n.rep.icon, m3::col(r ? t.c.primary : t.c.on_surface_var), 0);
        }
        const int8_t shuf = s.shuffle ? 1 : 0;
        if (shuf != n.shuffle) {
            n.shuffle = shuf;
            lv_obj_set_style_text_color(n.shuf.icon, m3::col(shuf ? t.c.primary : t.c.on_surface_var), 0);
        }
    } else {
        // What is left has no track and no transport: the tone generator, the analogue input,
        // USB — the computer is its own remote — and the output switched off.
        const bool is_usb = s.source == 7;
        set_title(s.source == 1 ? tr(S::now_tone)
                    : s.source == 2 ? tr(S::now_aux)
                    : is_usb ? tr(s.usb_connected ? S::now_usb : S::now_usb_waiting)
                    : tr(S::now_off));
        char sub[96];
        if (s.source == 1)      snprintf(sub, sizeof(sub), "%ld %s", (long)s.tone_hz, tr(S::unit_hz));
        else if (is_usb && s.play_fmt[0]) i18n::badge(sub, sizeof(sub), s.play_fmt);
        else if (is_usb)        snprintf(sub, sizeof(sub), "%s", tr(S::now_usb_pick));
        else if (s.in_rate > 0) snprintf(sub, sizeof(sub), "%ld %s", (long)s.in_rate, tr(S::unit_hz));
        else                    snprintf(sub, sizeof(sub), "%s", tr(S::now_pick));
        set_sub(sub);
        set_fmt("");
    }

    float db[audio_core::SPECTRUM_BANDS];
    uih::spectrum_set(n.spec, audio_core::spectrum_bus().read(db) ? db : nullptr);
}

}  // namespace

const UiScreenDef UI_SCREEN_NOW = {"now", UI_SYM_MUSIC, build, refresh, nullptr, nullptr};

void ui_now_play_pause() {
    if (n.ctx) transport(n.ctx->s, 1);
}

bool ui_now_plain(bool plain) {
    n.plain = plain;
    if (!n.page) return false;
    const bool grad = n.backdrop && !plain;
    lv_obj_set_style_bg_opa(n.page, grad ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    return n.backdrop != 0;
}
