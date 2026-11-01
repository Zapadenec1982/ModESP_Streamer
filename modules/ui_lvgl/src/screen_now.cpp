/**
 * @file screen_now.cpp
 * @brief "Плеєр" — the home page: what is playing, from whichever source, and the controls
 *        that source answers to.
 *
 * One page, five sources — card, DLNA, radio, Bluetooth, AirPlay — and an empty face. The
 * frame does not move between them: the artwork tile on the left, the block on the right, the
 * spectrum strip below. A track, whoever sends it, has the title, the progress and the
 * transport, and on that face a control that does not apply is greyed, not hidden, so nothing
 * shifts under a finger that is already reaching for play.
 *
 * A station has no track to move through and nothing to pause: stopping it would only be the
 * silence the mute already gives, and the stream stops by itself once another source is chosen.
 * Its face has no transport and no progress. The block is the station's name between ‹ and ›,
 * which step through the list; what it plays under that, or how far it has got connecting; and
 * where the transport stands, the six presets — the Stations page's buttons with its rules
 * (screen_radio.cpp), so the station after this one is a tap away. A pushed DLNA track plays on
 * the same engine but is a track, and has the track's face.
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
 * shows for it can also be called up with the loudspeaker button.
 *
 * Bluetooth with no phone is not a dead end: the line under «Телефон не підключено» says what a
 * phone will find — «видно як «MODEsp»» — or that new phones do not see the panel (the satellite's
 * radio or its visibility is off), and then the transport, which has nobody to command, gives way
 * to the one thing the panel can do about it: «Увімкнути видимість», which turns both on (an
 * action, not a state). The satellite cannot be asked to call a phone (LINK_PROTOCOL.md), so
 * there is no «connect».
 *
 * The knob's press is this page's play button, from any page (ui_now_press): the knob has one
 * job everywhere — a turn is the volume, a press plays and pauses — rather than ringing this
 * page's buttons in turn, which would be a mode. A station does not pause, so there the press
 * is the mute.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "esp_log.h"
#include "spectrum.h"
#include "playlist.h"
#include "cover.h"
#include "bt_settings.h"
#include "stations.h"
#include "station_badge.h"
#include "radio_catalog_ui.h"   // the presets' logos the radio module keeps (station_logo.h)
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using i18n::S;
using i18n::tr;
using modesp::gen::AUDIO_SOURCE_AIRPLAY;
using modesp::gen::AUDIO_SOURCE_AUX;
using modesp::gen::AUDIO_SOURCE_BT;
using modesp::gen::AUDIO_SOURCE_FILE;
using modesp::gen::AUDIO_SOURCE_RADIO;
using modesp::gen::AUDIO_SOURCE_TONE;
using modesp::gen::AUDIO_SOURCE_USB;

// ── geometry, all relative to the 752x384 content column ──
constexpr int32_t COVER   = player_core::COVER_PX;           // 280: the tile the player decodes
constexpr int32_t COL_X   = COVER + 24;
constexpr int32_t COL_W   = uig::CONT_W - COL_X;             // 448
// For a finger within the cover's 280: the column ends where the cover does, the transport stands
// on that line, and the spectrum under them keeps its 88 (an 88-px transport that ran the column
// past the cover ate the spectrum and set the page crooked).
constexpr int32_t ROW_H   = 48;                              // the source line, «Звук» and the two buttons
constexpr int32_t ROW_BTN = 48;                              // «Звук», the list and the volume as drawn;
constexpr int32_t ROW_EXT = 8;                               // a finger takes 8 px more to the sides and below
constexpr int32_t TITLE_Y = ROW_H;
// The transport as the web draws it (PlayerPage.svelte): play a filled disc, prev and next its
// bare glyphs, no container. Every target the glass's 64 (7.5 mm) at least; play set apart by its
// fill and a disc a size up, 72, which still ends where the progress box does. The group centred
// under the progress bar; on the card shuffle and repeat stand at the bar's two ends, and the five
// are one gap apart across the 448.
constexpr int32_t TRAN_H   = 72;
constexpr int32_t TRAN_GAP = 30;
constexpr int32_t PLAY_W   = TRAN_H;
static_assert(64 + TRAN_GAP + 64 + TRAN_GAP + PLAY_W + TRAN_GAP + 64 + TRAN_GAP + 64 == 448,
              "on the card the five fill the column at one gap");
constexpr int32_t TRAN_Y  = COVER - TRAN_H;                  // 208: bottom-aligned with the cover
constexpr int32_t COL_H   = COVER;
// the progress: the bar near the top of its box, the times under it, and the whole box the
// finger's target (a press reaches a box only inside its parent's)
constexpr int32_t PROG_H    = 50;
constexpr int32_t BAR_Y     = 4;
constexpr int32_t BAR_H     = 4;
constexpr int32_t SEEK_KNOB = 16;
constexpr uint32_t SEEK_HOLD_MS = 2000;   ///< the longest the bar waits at a seek for the position
// the spectrum strip: whatever the tile leaves below it, one self-drawing object
constexpr int32_t SPEC_X    = (uig::CONT_W - uih::SPEC_ROW_W) / 2;
constexpr int32_t SPEC_BASE = uig::CONT_H;
constexpr int32_t SPEC_H    = SPEC_BASE - COVER - 16;          // 88
// a station with no picture: its stand-in on the tile (station_badge.h), a disc with the label
// in the biggest face that keeps 20 px clear of the edge at each end
constexpr int32_t PH_DISC = 200;
constexpr int32_t PH_TEXT = PH_DISC - 40;
// The station face. ‹ and › are a finger's 64 whatever their glyph, and start where the row's
// buttons stop taking a finger, so neither steals the other's; the name is one headline line 4 px
// lower, between them, and their glyphs stand on that line. The presets are two rows of three
// 64-px buttons that end where the tile does, as the transport did: the station's logo in its disc
// (or its stand-in), then its name in up to two lines. Each keeps the outline the playing one
// shows, uncoloured, so nothing in a button moves when it plays (LVGL lays a child out inside the
// border).
constexpr int32_t ST_Y      = ROW_H + ROW_EXT;               // 56
constexpr int32_t ST_ARROW  = 64;
constexpr int32_t ST_NAME_Y = ST_Y + 4;
constexpr int     N_PRE     = radio_core::PRESETS;           // 6
constexpr int32_t PRE_COLS  = 3;
constexpr int32_t PRE_GAP   = 8;
constexpr int32_t PRE_H     = 64;
constexpr int32_t PRE_W     = (COL_W - (PRE_COLS - 1) * PRE_GAP) / PRE_COLS;   // 144
constexpr int32_t PRE_Y     = COL_H - 2 * PRE_H - PRE_GAP;   // 144
constexpr int32_t PRE_LINE  = 2;                             // the outline
constexpr int32_t PRE_PAD   = 8;
constexpr int32_t PRE_LOGO  = 40;
constexpr int32_t PRE_TEXT_X = PRE_PAD + PRE_LOGO + PRE_PAD;   // 56
constexpr int32_t PRE_TEXT_W = PRE_W - PRE_TEXT_X - PRE_PAD;   // 80: «Порожньо», the widest word, is 78.4
static_assert(N_PRE == 2 * PRE_COLS, "the presets are two rows of three");
static_assert(PRE_COLS * PRE_W + (PRE_COLS - 1) * PRE_GAP == COL_W, "the grid fills the column");
static_assert(PRE_LOGO == station_logo::SIDE, "a logo is kept at the disc's own size, drawn unscaled");
static_assert(ST_Y + ST_ARROW <= PRE_Y, "the arrows stand over the grid, not on it");

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
    bool      has_pic      = false; ///< the tile shows a picture
    lv_obj_t* ph       = nullptr;   ///< a station's stand-in for a picture: the disc…
    lv_obj_t* ph_lbl   = nullptr;   ///< …and its label
    char      ph_name[64] = "\x01"; ///< the name it was made from; no name looks like this
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
    int32_t   sub_y   = 0;          ///< where the line under the title stands on a track's face…
    int32_t   now_y   = 0;          ///< …and under the station's name on a station's
    int32_t   badge_dy = 0;         ///< the badge, on that line
    // progress: a bar with times for a track
    lv_obj_t* prog     = nullptr;
    lv_obj_t* prog_hit = nullptr;   ///< the touch target: taller than the bar it drives
    touch::Slide prog_touch;        ///< a finger on it (touch_gesture.h)
    bool      seek_drag = false;     ///< a finger is scrubbing: the position's refresh leaves the bar
    int32_t   seek_to   = -1;        ///< the second the finger has the knob at; once let go, until the position is there
    uint32_t  seek_at   = 0;         ///< when it let go
    lv_obj_t* prog_fill= nullptr;
    lv_obj_t* prog_knob= nullptr;   ///< at the fill's end, where the bar seeks
    lv_obj_t* t_pos    = nullptr;
    lv_obj_t* t_dur    = nullptr;
    // transport
    lv_obj_t*   transport = nullptr;
    m3w::Button prev, play, next, rep, shuf;
    m3w::Button pick;               ///< the empty face's way to the sources
    m3w::Button visible;            ///< Bluetooth, no phone, new phones not let in: let them in
    int8_t    vis_shown = -1;
    // the station face: ‹ the name ›, and the presets
    lv_obj_t*   station = nullptr;
    m3w::Button st_prev, st_next;
    lv_obj_t*   st_name = nullptr;
    lv_obj_t*   pre[N_PRE]      = {};
    lv_obj_t*   pre_disc[N_PRE] = {};   ///< the station's disc, or the empty button's number
    lv_obj_t*   pre_logo[N_PRE] = {};   ///< the station's logo in the disc, when the panel keeps one
    lv_obj_t*   pre_mark[N_PRE] = {};   ///< what is on the disc otherwise
    lv_obj_t*   pre_lbl[N_PRE]  = {};
    lv_image_dsc_t pre_dsc[N_PRE] = {};
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
    char  st_name_txt[64] = "";
    uint32_t pre_gen      = 0xFFFFFFFF;   ///< the station list's generation the presets show
    uint32_t logo_gen     = 0xFFFFFFFF;   ///< …and the logos' (radio_catalog::logo_shelf())
    int32_t  pre_station  = -2;
    int8_t   pre_storable = -1;
    int8_t   st_steps     = -1;           ///< ‹ and › have a station to step to
    PlayerFace face = PlayerFace::NONE;
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

/// The presets' logos as the radio module last published them (radio_catalog::logo_shelf()):
/// copied on this task when they move, into the copy the buttons are not drawing from, which then
/// becomes theirs — a copy cut short by a write is never drawn. Not in `Now`: build() resets that
/// through a temporary on this task's stack, and six logos are 19 KB.
station_logo::Shelf* logos[2] = {&uih::in_psram<station_logo::Shelf>(), &uih::in_psram<station_logo::Shelf>()};
int logos_drawn = 0;                 ///< the copy the buttons draw from

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

// ── interaction ──

/// The list button: the list of the source that is playing, each a page of its own.
void list_cb(lv_event_t*) {
    if (!n.ctx) return;
    const auto& s = n.ctx->s;
    if (s.source == AUDIO_SOURCE_FILE)       ui_shell_go("library");
    else if (s.network_push)                 ui_shell_go("dlna");
    else if (s.source == AUDIO_SOURCE_RADIO) ui_shell_go("radio");
}
void src_cb(lv_event_t*)    { ui_shell_go("src"); }
/// «Увімкнути видимість»: the radio on as well — with it off the satellite is seen by no one
/// (LINK_PROTOCOL.md, BT_CONFIG) — and its other settings as they are (BT_CONFIG_SET is whole).
void vis_cb(lv_event_t*) {
    bt_settings::Config cfg;
    bt_settings::config(cfg);
    if (cfg.known) bt_settings::request_config(true, true, cfg.auto_reconnect, cfg.codecs_enabled);
}
void vol_cb(lv_event_t*)    { ui_shell_show_volume(); }
void sound_cb(lv_event_t*)  { ui_sound_open(); }
void lev_cb(lv_event_t*)    { post(UiLvglModule::Req::LEVELER, -1); }
void rep_cb(lv_event_t*)    { post(UiLvglModule::Req::REPEAT, -1); }
void shuf_cb(lv_event_t*)   { post(UiLvglModule::Req::SHUFFLE, -1); }

/// The transport speaks the source's language: player.cmd codes for a track (the module
/// forwards them to the phone or the DLNA queue), radio.cmd codes for a station's ‹ and ›.
/// `which`: 0 prev, 1 play/pause, 2 next. Read from the snapshot, not this page's buttons: the
/// knob's press comes here from every page, and only the page on the glass is refreshed. A
/// station has no pause, and its "play/pause" — the knob's press — is the mute.
void transport(const UiLvglModule::Snapshot& s, int which) {
    const PlayerFace face = ui_face(s);
    if (face == PlayerFace::EMPTY) return;               // the tone, AUX, USB, off: nothing to command
    // nobody to command: no phone; an AirPlay sender that gave no remote (refresh's "live")
    if ((s.source == AUDIO_SOURCE_BT && !s.bt_connected) || (s.source == AUDIO_SOURCE_AIRPLAY && !s.ap_remote))
        return;
    if (face == PlayerFace::RADIO) {
        if (which == 1) post(UiLvglModule::Req::MUTE, -1);
        else            post(UiLvglModule::Req::RADIO_CMD, which == 0 ? 4 : 3);
    } else if (which == 1 && s.source == AUDIO_SOURCE_FILE && strcmp(s.play_state, "playing") != 0 &&
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

/// A preset of the station face, with the Stations page's own gestures. SHORT_CLICKED, not
/// CLICKED: LVGL sends CLICKED after a long press as well, and a store must not also tune.
void pre_cb(lv_event_t* e) {
    const int slot = (int)(intptr_t)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) ui_preset_store(slot);
    else                                               ui_preset_tap(slot);
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

/// The time under the bar's left end.
void show_pos(int32_t sec) {
    char buf[12];
    mmss(sec, buf, sizeof(buf));
    uih::set_text(n.t_pos, n.pos_txt, sizeof(n.pos_txt), buf);
}

/// A finger that takes the progress bar's knob — where the music is — and goes along scrubs: the
/// knob and the time follow it from there, and the music goes where it lets go; a press, a tap or
/// a finger elsewhere on the bar does not (m3w::slide_touch — a tap meant for the cover or the
/// buttons must not jump the track). One seek, on the release: the bar is read every 15 ms, and
/// each seek costs the card a fade and a flush and a pushed track a new connection — a seek per
/// move would fill the player's queue and drop the one that matters, the last.
void seek_cb(lv_event_t* e) {
    const int32_t w = lv_obj_get_width(n.prog_hit);
    lv_area_t a;
    lv_obj_get_coords(n.prog_hit, &a);
    int32_t x = lv_obj_get_width(n.prog_fill);
    const m3w::Slid r = m3w::slide_touch(e, n.prog_touch, a.x1 + knob_centre(x), x, w, w);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (n.seek_drag) {                        // let go of a scrub: its one seek
            n.seek_drag = false;
            n.seek_at = lv_tick_get();
            post(UiLvglModule::Req::SEEK, n.seek_to);
        }
        return;
    }
    if (r != m3w::Slid::MOVE) return;
    const int32_t dur = n.ctx ? n.ctx->s.play_dur_s : 0;
    if (dur <= 0) return;
    if (w <= 0) return;
    if (x < 0) x = 0; else if (x > w) x = w;
    n.seek_drag = true;
    n.seek_to = (int32_t)((int64_t)dur * x / w);
    // Drawn at once, before the player has been anywhere: a bar that waits half a second
    // for the position to catch up feels broken even when it is not.
    set_fill(x < 2 ? 2 : x);
    show_pos(n.seek_to);
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
    n.ph = m3w::surface(n.tile, (COVER - PH_DISC) / 2, (COVER - PH_DISC) / 2, PH_DISC, PH_DISC,
                        station_badge::PALETTE[0], m3::shape::FULL);
    n.ph_lbl = m3w::text(n.ph, t.t.display, station_badge::INK, "");
    lv_obj_center(n.ph_lbl);
    uih::show(n.ph, false);

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
    n.sub_y = sub_y;
    n.now_y = ST_NAME_Y + lh_title + 4;                 // 104: the station's line, over the presets at 144
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
    n.badge_dy = (lh_sub - lv_font_get_line_height(t.t.label_s) - 4) / 2;
    lv_obj_set_y(n.badge, sub_y + n.badge_dy);
    uih::show(n.badge, false);

    // ── progress: the bar, which is also the seek control ──
    const int32_t prog_y = sub_y + lh_sub + 3;          // 158: the box ends at 208, where the transport starts
    n.prog = uih::box(col, 0, prog_y, COL_W, PROG_H);
    // A 4-pixel bar is a fine thing to look at and an impossible thing to hit, so the touch
    // target is the whole box, twelve times its height, with the times in it. It was a taller box
    // centred on the bar and reaching out of this one, but LVGL looks for a target only inside the
    // parent's box, so a finger above the bar found nothing: 28 px of 44. Where the bar seeks, a
    // knob at the fill's end shows where a finger takes it: knob only, as the sliders.
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

    // ── transport: centred under the progress bar at one gap; the hidden take no room ──
    n.transport = uih::box(col, 0, TRAN_Y, COL_W, TRAN_H);
    lv_obj_set_flex_flow(n.transport, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(n.transport, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(n.transport, TRAN_GAP, 0);
    // the two that change what "next" means: the card's queue, nobody else's
    n.shuf = m3w::icon_button(n.transport, 0, 0, 64, UI_SYM_SHUFFLE, t.t.icon_m);
    lv_obj_add_event_cb(n.shuf.obj, shuf_cb, LV_EVENT_CLICKED, nullptr);
    n.prev = m3w::icon_button(n.transport, 0, 0, 64, UI_SYM_PREV, t.t.icon_xl_fill);
    lv_obj_set_style_text_color(n.prev.icon, m3::col(t.c.on_surface), 0);
    lv_obj_add_event_cb(n.prev.obj, tran_cb, LV_EVENT_CLICKED, (void*)(intptr_t)0);
    n.play = m3w::filled_button(n.transport, 0, 0, PLAY_W, TRAN_H, m3::shape::FULL, UI_SYM_PLAY, t.t.icon_xl_fill);
    lv_obj_add_event_cb(n.play.obj, tran_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);
    n.next = m3w::icon_button(n.transport, 0, 0, 64, UI_SYM_NEXT, t.t.icon_xl_fill);
    lv_obj_set_style_text_color(n.next.icon, m3::col(t.c.on_surface), 0);
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

    // ── the station face: ‹ the station's name ›, and the presets where the transport stands.
    //    A box over the whole column that takes no finger itself, so the row's buttons still do. ──
    n.station = uih::box(col, 0, 0, COL_W, COL_H);
    n.st_prev = m3w::icon_button(n.station, 0, ST_Y, ST_ARROW, UI_SYM_CHEVRON_LEFT, t.t.icon_xl);
    n.st_next = m3w::icon_button(n.station, COL_W - ST_ARROW, ST_Y, ST_ARROW, UI_SYM_CHEVRON, t.t.icon_xl);
    for (m3w::Button* b : {&n.st_prev, &n.st_next}) {
        lv_obj_set_style_text_color(b->icon, m3::col(t.c.on_surface), 0);
        lv_obj_align(b->icon, LV_ALIGN_CENTER, 0, ST_NAME_Y + lh_title / 2 - (ST_Y + ST_ARROW / 2));
    }
    lv_obj_add_event_cb(n.st_prev.obj, tran_cb, LV_EVENT_CLICKED, (void*)(intptr_t)0);
    lv_obj_add_event_cb(n.st_next.obj, tran_cb, LV_EVENT_CLICKED, (void*)(intptr_t)2);
    n.st_name = m3w::text(n.station, t.t.headline, t.c.on_surface, "");
    lv_obj_set_pos(n.st_name, ST_ARROW, ST_NAME_Y);
    lv_obj_set_size(n.st_name, COL_W - 2 * ST_ARROW, lh_title);
    lv_obj_set_style_text_align(n.st_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(n.st_name, LV_LABEL_LONG_DOT);
    for (int i = 0; i < N_PRE; i++) {
        // On the cover's colour, as «Звук» is: on_surface at a tenth, a fifth while pressed.
        lv_obj_t* p = uih::box(n.station, (i % PRE_COLS) * (PRE_W + PRE_GAP),
                               PRE_Y + (i / PRE_COLS) * (PRE_H + PRE_GAP), PRE_W, PRE_H);
        lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(p, m3::shape::M, 0);
        lv_obj_set_style_bg_color(p, m3::col(t.c.on_surface), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_10, 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(p, PRE_LINE, 0);
        lv_obj_set_style_border_color(p, m3::col(t.c.primary), 0);
        lv_obj_set_style_border_opa(p, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(p, pre_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(p, pre_cb, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)i);
        n.pre[i] = p;
        n.pre_disc[i] = m3w::surface(p, PRE_PAD - PRE_LINE, (PRE_H - PRE_LOGO) / 2 - PRE_LINE, PRE_LOGO,
                                     PRE_LOGO, t.c.surface_highest, m3::shape::FULL);
        // The station's logo fills the disc, clipped to its circle; the mark is for the rest.
        lv_obj_set_style_clip_corner(n.pre_disc[i], true, 0);
        n.pre_logo[i] = lv_image_create(n.pre_disc[i]);
        lv_obj_set_pos(n.pre_logo[i], 0, 0);
        uih::show(n.pre_logo[i], false);
        n.pre_mark[i] = m3w::text(n.pre_disc[i], t.t.label_m, t.c.on_surface_var, "");
        lv_obj_center(n.pre_mark[i]);
        n.pre_lbl[i] = m3w::text(p, t.t.label_s, t.c.on_surface, "");
        lv_obj_set_x(n.pre_lbl[i], PRE_TEXT_X - PRE_LINE);
        lv_obj_set_width(n.pre_lbl[i], PRE_TEXT_W);
        lv_label_set_long_mode(n.pre_lbl[i], LV_LABEL_LONG_DOT);
    }
    uih::show(n.station, false);

    // ── spectrum: the strip under the tile ──
    n.spec = uih::spectrum(root, SPEC_X, SPEC_BASE, SPEC_H);

    // The page, not the content box in it: what build() returns is what the shell hides when
    // another page is chosen, and the page is the one that is opaque over the whole screen. Given
    // the box, the shell hid only that, and the cover's colour lay over every other page — the
    // Sources and the Settings showed as empty.
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
/// the text's own, measured, not the label's: a cut label is as wide as its cut. A track's line
/// starts at the column's edge, under the title; a station's is centred, under its name.
void place_meta() {
    const bool badge = !lv_obj_has_flag(n.badge, LV_OBJ_FLAG_HIDDEN);
    int32_t room = COL_W, badge_w = 0;
    if (badge) {
        lv_obj_update_layout(n.badge);
        badge_w = lv_obj_get_width(n.badge);
        room -= badge_w + 12;
    }
    lv_point_t sz;
    lv_text_get_size(&sz, n.sub_txt, m3::theme().t.body_l, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t w = sz.x < room ? sz.x : room;
    lv_obj_set_width(n.sub, w > 1 ? w : 1);
    const int32_t text = n.sub_txt[0] ? w + (badge ? 12 : 0) : 0;
    const int32_t x = n.face == PlayerFace::RADIO ? (COL_W - text - badge_w) / 2 : 0;
    lv_obj_set_x(n.sub, x);
    if (badge) lv_obj_set_x(n.badge, x + text);
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

/// A station's stand-in on the tile (station_badge.h): its colour, and its label in the biggest
/// face that fits across the disc — the display face fits most, the headline any five characters.
void set_placeholder(const char* name) {
    if (strncmp(n.ph_name, name, sizeof(n.ph_name) - 1) == 0) return;
    snprintf(n.ph_name, sizeof(n.ph_name), "%s", name);
    const auto& t = m3::theme();
    const station_badge::Badge b = station_badge::make(name);
    lv_obj_set_style_bg_color(n.ph, m3::col(b.rgb), 0);
    const bool big = lv_text_get_width(b.label, (uint32_t)strlen(b.label), t.t.display, 0) <= PH_TEXT;
    lv_obj_set_style_text_font(n.ph_lbl, big ? t.t.display : t.t.headline, 0);
    lv_label_set_text(n.ph_lbl, b.label);
}

/// What a preset shows for its station: the logo the panel keeps for it (src/logos.cpp in the
/// radio module — a station with a Radio Browser uuid, once its logo has been had), else the
/// generated stand-in (station_badge.h).
struct PresetLogo {
    const uint16_t*      px = nullptr;   ///< station_logo::SIDE square, RGB565; nullptr: the stand-in
    station_badge::Badge badge;
};
PresetLogo logo_for(const radio_core::Station& st) {
    PresetLogo l;
    if (const station_logo::Thumb* kept = logos[logos_drawn]->find(st.uuid)) l.px = kept->px;
    else l.badge = station_badge::make(st.name);
    return l;
}

/// The presets as the Stations page paints its own: the station's name — or «Порожньо», and
/// «Зберегти сюди» while a tap would store — and the playing one outlined in the accent. A
/// station's disc carries its logo, or the first character of its label; an empty button's, its
/// number.
void paint_presets(const UiLvglModule::Snapshot& s, bool storable) {
    const auto& t = m3::theme();
    auto& list = radio_core::stations();
    radio_core::Station st;
    const int32_t lh = lv_font_get_line_height(t.t.label_s);
    for (int i = 0; i < N_PRE; i++) {
        const int idx = list.preset(i);
        const bool have = idx >= 0 && list.at(idx, st);
        lv_obj_set_style_border_opa(n.pre[i], have && idx == s.radio_station ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        const char* txt;
        bool logo = false;
        if (have) {
            const PresetLogo l = logo_for(st);
            logo = l.px != nullptr;
            if (logo) {
                lv_image_dsc_t& d = n.pre_dsc[i];
                d = lv_image_dsc_t{};
                d.header.magic  = LV_IMAGE_HEADER_MAGIC;
                d.header.cf     = LV_COLOR_FORMAT_RGB565;
                d.header.w      = station_logo::SIDE;
                d.header.h      = station_logo::SIDE;
                d.header.stride = station_logo::SIDE * 2;
                d.data          = (const uint8_t*)l.px;
                d.data_size     = station_logo::PIXELS * 2;
                lv_image_set_src(n.pre_logo[i], &d);
            } else {
                lv_obj_set_style_bg_color(n.pre_disc[i], m3::col(l.badge.rgb), 0);
                lv_obj_set_style_text_font(n.pre_mark[i], t.t.title_m, 0);
                lv_obj_set_style_text_color(n.pre_mark[i], m3::col(station_badge::INK), 0);
                lv_label_set_text(n.pre_mark[i], l.badge.first);
            }
            txt = st.name[0] ? st.name : "—";
            lv_obj_set_style_text_color(n.pre_lbl[i], m3::col(t.c.on_surface), 0);
        } else {
            char num[4];
            snprintf(num, sizeof(num), "%d", i + 1);
            lv_obj_set_style_bg_color(n.pre_disc[i], m3::col(t.c.surface_highest), 0);
            lv_obj_set_style_text_font(n.pre_mark[i], t.t.label_m, 0);
            lv_obj_set_style_text_color(n.pre_mark[i], m3::col(t.c.on_surface_var), 0);
            lv_label_set_text(n.pre_mark[i], num);
            txt = tr(storable ? S::st_preset_store : S::st_preset_empty);
            lv_obj_set_style_text_color(n.pre_lbl[i], m3::col(storable ? t.c.primary : t.c.on_surface_var), 0);
        }
        uih::show(n.pre_logo[i], logo);
        uih::show(n.pre_mark[i], !logo);
        lv_label_set_text(n.pre_lbl[i], txt);
        // one line or two, centred in the button either way
        lv_point_t sz;
        lv_text_get_size(&sz, txt, t.t.label_s, 0, 0, PRE_TEXT_W, LV_TEXT_FLAG_NONE);
        const int32_t h = sz.y > lh ? 2 * lh : lh;
        lv_obj_set_height(n.pre_lbl[i], h);
        lv_obj_set_y(n.pre_lbl[i], (PRE_H - h) / 2 - PRE_LINE);
    }
}

/// The station face's controls: ‹ and › while the list has a station to step to, and the presets
/// once the list, the station, what a tap on an empty one does or the logos have changed. The
/// list and the logos are read settled — an odd generation is a write under way — as the Stations
/// page reads the list.
void refresh_station(const UiLvglModule::Snapshot& s) {
    auto& list = radio_core::stations();
    const int count = list.count();
    const bool listed = s.radio_station >= 0 && s.radio_station < count;
    const int8_t steps = count > (listed ? 1 : 0) ? 1 : 0;
    if (steps != n.st_steps) {
        n.st_steps = steps;
        m3w::button_enable(n.st_prev, steps != 0);
        m3w::button_enable(n.st_next, steps != 0);
    }
    const uint32_t gen = list.generation();
    const int8_t storable = ui_preset_storable(s) ? 1 : 0;
    // A logo arrived, or the presets moved: the radio module published the shelf again.
    auto& shelf = radio_catalog::logo_shelf();
    const uint32_t lgen = shelf.generation();
    bool logos_moved = false;
    if (lgen != n.logo_gen && !(lgen & 1u) && shelf.get(*logos[logos_drawn ^ 1])) {
        logos_drawn ^= 1;
        n.logo_gen = lgen;
        logos_moved = true;
    }
    if (logos_moved || (gen != n.pre_gen && !(gen & 1u)) || s.radio_station != n.pre_station ||
        storable != n.pre_storable) {
        n.pre_gen = gen;
        n.pre_station = s.radio_station;
        n.pre_storable = storable;
        paint_presets(s, storable != 0);
    }
}

/// The page's colour: the cover's (cover_backdrop), or the plain surface when there is no
/// picture. The gradient itself is set once, in build().
void set_backdrop(uint32_t rgb) {
    if (rgb == n.backdrop) return;
    n.backdrop = rgb;
    if (rgb) lv_obj_set_style_bg_color(n.page, m3::col(rgb), 0);
    lv_obj_set_style_bg_opa(n.page, rgb && !n.plain ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

/// The satellite has told its settings and new phones cannot find it: its radio is off, or it is
/// not discoverable.
bool bt_hidden() {
    bt_settings::Config cfg;
    bt_settings::config(cfg);
    return cfg.known && (!cfg.radio || !cfg.discoverable);
}

/// Bluetooth with no phone: what a phone would find — the name the panel is seen by — or that
/// Bluetooth is off, or that new phones do not see it; nothing while the satellite has not said (or
/// is not there: the title says).
void bt_wait_line(const UiLvglModule::Snapshot& s, char* out, size_t n) {
    out[0] = '\0';
    if (!s.bt_link) return;
    bt_settings::Config cfg;
    bt_settings::config(cfg);
    if (!cfg.known) return;
    if (!cfg.radio || !cfg.discoverable) {
        snprintf(out, n, "%s", tr(cfg.radio ? S::bt_visible_off : S::bt_radio_off));
        return;
    }
    char name[40];
    bt_settings::name(name, sizeof(name));
    if (name[0]) i18n::fmt(out, n, tr(S::manage_visible_as), {{"name", name}});
}

void set_face(PlayerFace f, const UiLvglModule::Snapshot& s) {
    const auto& t = m3::theme();
    n.face = f;
    const bool empty = (f == PlayerFace::EMPTY);
    const bool radio = (f == PlayerFace::RADIO);
    const bool is_file = (s.source == AUDIO_SOURCE_FILE);
    uih::show(n.transport, f == PlayerFace::TRACK);
    n.vis_shown = -1;                             // the next refresh decides on «Увімкнути видимість»
    n.seek_drag = false;                          // a scrub of another source's track seeks nothing
    n.seek_to = -1;
    uih::show(n.pick.obj, empty);
    uih::show(n.prog, f == PlayerFace::TRACK);
    // A station's block is its own: the name between ‹ and ›, the presets; its line goes under the
    // name, and the next refresh paints the arrows and the presets.
    uih::show(n.title, !radio);
    uih::show(n.station, radio);
    lv_obj_set_y(n.sub, radio ? n.now_y : n.sub_y);
    lv_obj_set_y(n.badge, (radio ? n.now_y : n.sub_y) + n.badge_dy);
    n.st_steps = -1;
    n.pre_gen = 0xFFFFFFFF;
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
    const UiSrcDef src = ui_src_def(ui_src(s));
    lv_label_set_text(n.tile_ic, empty ? src.sym : UI_SYM_MUSIC);
    lv_obj_set_style_text_color(n.tile_ic, m3::col(empty ? t.c.outline : t.c.outline_var), 0);
    uih::set_text(n.from_src, n.src_txt, sizeof(n.src_txt), tr(src.caps));
    uih::show(n.from, src.caps != UI_NO_CAPS);
    n.tran_live = -1;
    n.playing = -1;
    place_row();
}

void refresh(lv_obj_t*, UiCtx& c) {
    const auto& t = m3::theme();
    const auto& s = c.s;
    const bool is_file = (s.source == AUDIO_SOURCE_FILE);
    const bool is_bt   = (s.source == AUDIO_SOURCE_BT);
    const bool is_ap   = (s.source == AUDIO_SOURCE_AIRPLAY);
    // Bluetooth and AirPlay have a track, a position and a transport like the card and DLNA,
    // so they are drawn by the same face (ui_face); repeat and shuffle stay the card's.
    const PlayerFace face = ui_face(s);

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
    const int8_t live = face == PlayerFace::EMPTY ? 0
                      : is_ap ? (s.ap_remote ? 1 : 0)
                      : (!is_bt || s.bt_connected) ? 1 : 0;
    // Bluetooth, no phone, and new phones kept out: the transport gives way to letting them in
    const int8_t vis = face != PlayerFace::EMPTY && is_bt && !s.bt_connected && s.bt_link && bt_hidden() ? 1 : 0;
    if (vis != n.vis_shown) {
        n.vis_shown = vis;
        uih::show(n.visible.obj, vis != 0);
        uih::show(n.transport, face == PlayerFace::TRACK && !vis);
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
            n.has_pic = cv.px && cv.w > 0;
            if (n.has_pic) {
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
            }
            uih::show(n.cover_im, n.has_pic);
            set_backdrop(cv.px ? player_core::cover_backdrop(cv.rgb) : 0);
        }
        // No picture: a station shows its stand-in, every other source the glyph.
        const bool ph = face == PlayerFace::RADIO && !n.has_pic;
        uih::show(n.ph, ph);
        uih::show(n.tile_ic, !n.has_pic && !ph);
    }

    // the phone it plays from, beside «BLUETOOTH»; no other source names its sender
    uih::set_text(n.from_dev, n.dev_txt, sizeof(n.dev_txt), is_bt && s.bt_connected ? s.bt_peer : "");

    if (face == PlayerFace::RADIO) {
        // Who is playing between ‹ and ›, and under it what: the ICY title once the station
        // plays, and until then the state in words rather than a blank line that reads as a fault.
        const char* name = s.radio_name[0] ? s.radio_name : tr(S::src_radio);
        uih::set_text(n.st_name, n.st_name_txt, sizeof(n.st_name_txt), name);
        set_placeholder(name);
        const char* st = s.radio_state;
        set_sub(strcmp(st, "playing") == 0      ? s.radio_now
              : strcmp(st, "connecting") == 0   ? tr(S::now_connecting)
              : strcmp(st, "buffering") == 0    ? tr(S::now_buffering)
              : strcmp(st, "reconnecting") == 0 ? tr(S::now_reconnecting)
              : tr(S::now_stopped));
        set_fmt(s.radio_fmt);
        refresh_station(s);
    } else if (face == PlayerFace::TRACK) {
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
        // After a scrub the bar stays where the finger let go until the position gets there: the
        // seek reaches the player only now, and a pushed track connects again first. A seek
        // refused, or slower than SEEK_HOLD_MS, gives the bar back to the position.
        if (n.seek_to >= 0 && !n.seek_drag &&
            (std::abs(s.play_pos_s - n.seek_to) <= 1 || lv_tick_elaps(n.seek_at) > SEEK_HOLD_MS))
            n.seek_to = -1;
        const bool held = n.seek_drag || n.seek_to >= 0;
        if (!held) show_pos(s.play_pos_s);
        char buf[12];
        if (s.play_dur_s > 0) {
            mmss(s.play_dur_s, buf, sizeof(buf));
            int32_t w = COL_W * s.play_pos_s / s.play_dur_s;
            if (w < 2) w = 2; else if (w > COL_W) w = COL_W;
            if (!held) set_fill(w);                 // under a finger, and after it, the bar is the finger's
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
        const bool is_usb = s.source == AUDIO_SOURCE_USB;
        set_title(s.source == AUDIO_SOURCE_TONE ? tr(S::now_tone)
                    : s.source == AUDIO_SOURCE_AUX ? tr(S::now_aux)
                    : is_usb ? tr(s.usb_connected ? S::now_usb : S::now_usb_waiting)
                    : tr(S::now_off));
        char sub[96];
        if (s.source == AUDIO_SOURCE_TONE) snprintf(sub, sizeof(sub), "%ld %s", (long)s.tone_hz, tr(S::unit_hz));
        else if (is_usb && s.play_fmt[0])  i18n::badge(sub, sizeof(sub), s.play_fmt);
        else if (is_usb)                   snprintf(sub, sizeof(sub), "%s", tr(S::now_usb_pick));
        else if (s.in_rate > 0)            snprintf(sub, sizeof(sub), "%ld %s", (long)s.in_rate, tr(S::unit_hz));
        else                               snprintf(sub, sizeof(sub), "%s", tr(S::now_pick));
        set_sub(sub);
        set_fmt("");
    }

    float db[audio_core::SPECTRUM_BANDS];
    uih::spectrum_set(n.spec, audio_core::spectrum_bus().read(db) ? db : nullptr);
}

}  // namespace

const UiScreenDef UI_SCREEN_NOW = {"now", UI_SYM_MUSIC, build, refresh, nullptr, nullptr};

void ui_now_press() {
    if (n.ctx) transport(n.ctx->s, 1);
}

bool ui_now_plain(bool plain) {
    n.plain = plain;
    if (!n.page) return false;
    const bool grad = n.backdrop && !plain;
    lv_obj_set_style_bg_opa(n.page, grad ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    return n.backdrop != 0;
}
