/**
 * @file screen_eq.cpp
 * @brief The equalizer — the sounds' field, the response curve with draggable band handles and
 *        the band panel (the limiter's meter is Service's).
 *        The sounds are a field that opens their list, as on the «Звук» sheet (sound_sheet.cpp
 *        ui_sound_pick_show).
 *
 * The curve is computed with audio_core::eq_magnitude_db(), the same function the audio core
 * designs its biquads with. There is deliberately no second copy of the RBJ maths here: a
 * drawing that agrees with the sound only by coincidence would drift the first time either
 * side is touched, and nothing would show it.
 *
 * Edits go to a local EqConfig. The module copies it out under the LVGL lock, hands it to the
 * audio core on the next tick and writes eq.json from the main loop once the user leaves
 * this screen — LittleFS writes must not happen per drag event, and never from this task.
 *
 * The sounds do not destroy anything. «Власний» is the user's own set, kept while a genre
 * plays, and choosing it brings it back; a genre never changes — a hand edit of one is the new
 * «Власний», and the line under the field says so; «Скинути «Власний»» turns its bands off,
 * with «Повернути» for a while. The table and those rules are shared with the web
 * (audio_dsp/eq_presets.h), and so is the document: this page edits a local copy, the module
 * puts it into audio_core::eq_store(), and the page takes the store's version back when the web
 * changed it and nothing here is waiting to be put. The audio module hears and keeps it.
 */

#include "ui_common.h"
#include "ui_m3_widgets.h"
#include "i18n.h"
#include "eq_config.h"
#include "eq_presets.h"
#include "eq_store.h"
#include "user_store.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

using i18n::S;
using i18n::tr;

// ── geometry, all relative to the 752x384 content column ──
// Whatever a finger takes is 64 px. The plot gives up the height that costs; «Смуг немає», the
// one line that stood under the top row, is in the plot, which is empty when it shows.
constexpr int32_t TOP_H    = 64;                       // the switch, the sounds, «Скинути «Власний»»
constexpr int32_t CARD_Y   = TOP_H + 8;                // 72
constexpr int32_t B2_H     = 64;                       // the three steppers, at the foot
constexpr int32_t B2_Y     = uig::CONT_H - B2_H;       // 320
constexpr int32_t B1_H     = 64;                       // band, on/off, type
constexpr int32_t B1_Y     = B2_Y - 8 - B1_H;          // 248
constexpr int32_t CARD_H   = B1_Y - 8 - CARD_Y;        // 168
constexpr int32_t PLOT_X   = 40;                       // room for the dB scale
constexpr int32_t PLOT_W   = uig::CONT_W - PLOT_X - 16;
constexpr int32_t PLOT_Y   = 12;
constexpr int32_t PLOT_H   = CARD_H - PLOT_Y - 26;     // room for the Hz scale: 130
constexpr int32_t STEP_GAP = 8;
constexpr int32_t STEP_W   = (uig::CONT_W - 2 * STEP_GAP) / 3;
constexpr int32_t TYPE_W   = 176;
constexpr int32_t PEQ_W    = 164;                      // the switch and its word «Увімкнено», one target
constexpr int32_t FIELD_X  = PEQ_W + 8;                // the sounds' field, after the switch
constexpr int32_t FIELD_W  = 330;                      // clear of «Скинути «Власний»» (from 522)
static_assert(FIELD_X + FIELD_W <= 522 - 16, "the field stays clear of the reset button");
constexpr int32_t FIELD_H  = TOP_H;
constexpr int32_t RESET_W  = 230;                      // «Скинути «Власний»», at the right while it plays
constexpr int32_t NAV_BTN  = 64;                       // ‹ › of the band, − + of a stepper
constexpr int32_t BAND_W   = 216;                      // ‹ «Смуга 10» ›
constexpr int32_t BSW_X    = BAND_W + 16;              // the band's switch and its word, one target
constexpr int32_t BSW_W    = 200;
constexpr int     CURVE_N  = 97;                       // points across the plot
constexpr int     HANDLE   = 36;
/// A point takes a finger this far beyond what is drawn — 64 px, a fingertip — and nothing else of
/// the plot takes one (touch_gesture.h)
constexpr int32_t HANDLE_REACH = 14;
static_assert(HANDLE + 2 * HANDLE_REACH == 2 * touch::GRAB, "a point's reach is the sliders' grab");

// 20 Hz .. 20 kHz on a log axis — the design's frequency scale.
constexpr float F_LO = 20.0f, DECADES = 3.0f;

/// Half-height of the plot, in dB. +-12 is the right window for shaping with peaks and
/// shelves. A pass filter leaves it inside two thirds of an octave, though, and everything
/// below is flattened against the floor — including the knee, which is the only place Q has
/// any visible effect. So the scale opens to +-24 as soon as a pass or notch band is on.
float s_range_db = 12.0f;

float fx(float f)   { return log10f(f / F_LO) / DECADES; }          // → 0..1
float xf(float x)   { return F_LO * powf(10.0f, x * DECADES); }
float gy(float g)   { return 0.5f - g / (2.0f * s_range_db); }      // → 0..1
float yg(float y)   { return (0.5f - y) * 2.0f * s_range_db; }

// ── presets: audio_dsp/eq_presets.h ──
constexpr int CUSTOM = audio_core::EQ_PRESET_CUSTOM;
/// In BandType's order — the web's names for the filter types.
constexpr S TYPE_NAME[] = {S::eq_type_peak, S::eq_type_lowshelf, S::eq_type_highshelf,
                           S::eq_type_lowpass, S::eq_type_highpass, S::eq_type_notch};
/// The steppers' captions; the third is the Q's own name, the same in every language.
const char* param_name(int i) { return i == 0 ? tr(S::eq_freq) : i == 1 ? tr(S::eq_gain) : tr(S::eq_q_width); }

/// The presets by id (audio_dsp/eq_presets.cpp), as the web names them (DspPage's PRESET_KEYS):
/// the component's own name is Ukrainian and stays only as the fallback for an id not here.
/// The kept ones are named too — one plays under its name until another is chosen.
/// tools/tests/test_panel_i18n.py holds this list to the component's.
struct PresetName { const char* id; S name; };
constexpr PresetName PRESET_NAMES[] = {
    {"custom", S::eq_preset_custom},           {"flat", S::eq_preset_flat},
    {"pop", S::eq_preset_pop},                 {"rock", S::eq_preset_rock},
    {"jazz", S::eq_preset_jazz},               {"classical", S::eq_preset_classical},
    {"electronic", S::eq_preset_electronic},   {"hiphop", S::eq_preset_hiphop},
    {"loudness", S::eq_preset_loudness},       {"night", S::eq_preset_night},
    {"more_bass", S::eq_preset_more_bass},     {"more_treble", S::eq_preset_more_treble},
    {"bass_treble", S::eq_preset_bass_treble}, {"headphones", S::eq_preset_headphones},
    {"large_hall", S::eq_preset_large_hall},   {"live", S::eq_preset_live},
    {"party", S::eq_preset_party},             {"club", S::eq_preset_club},
    {"reggae", S::eq_preset_reggae},           {"ska", S::eq_preset_ska},
    {"soft", S::eq_preset_soft},               {"soft_rock", S::eq_preset_soft_rock},
    {"dance", S::eq_preset_dance},
};

const char* preset_name(int idx) {
    const audio_core::EqPreset& p = audio_core::eq_preset(idx);
    for (const PresetName& n : PRESET_NAMES) if (strcmp(n.id, p.id) == 0) return tr(n.name);
    return p.name;
}

struct Eq {
    audio_core::EqConfig cfg;          ///< what is heard
    audio_core::EqConfig custom;       ///< the user's own set, while a preset is heard
    int      sel      = 0;             ///< selected band
    int      preset   = CUSTOM;
    uint32_t revision = 0;             ///< bumps on every edit; the module puts it in the store
    uint32_t pushed_rev = 0;           ///< the edit the module last put (ui_eq_pushed)
    uint32_t store_rev = 0;            ///< the store revision this page shows

    lv_obj_t* card    = nullptr;
    lv_obj_t* curve   = nullptr;
    lv_obj_t* hit     = nullptr;
    lv_obj_t* handle[audio_core::MAX_BANDS] = {};
    lv_obj_t* hlabel[audio_core::MAX_BANDS] = {};
    lv_obj_t* grid_y[5]     = {};      ///< the dB lines, repositioned when the scale changes
    lv_obj_t* grid_y_lbl[5] = {};
    // top row
    lv_obj_t* peq     = nullptr;       ///< the switch
    lv_obj_t* sounds  = nullptr;       ///< the field that opens the sounds' list (ui_sound_field)
    lv_obj_t* sound_val = nullptr;
    char      sound_val_txt[48] = "";
    m3w::Button reset;                 ///< «Скинути «Власний»», while «Власний» plays
    lv_obj_t* note    = nullptr;       ///< the line under them
    char      note_txt[192] = "";
    uint16_t  reset_on = 0;            ///< the bands that were on before «Скинути»: «Повернути» turns them on
    // band panel
    lv_obj_t* band_box = nullptr;      ///< the band chooser: ‹ the band ›
    lv_obj_t* band_lbl = nullptr;
    lv_obj_t* band_sw  = nullptr;
    lv_obj_t* type_field = nullptr;    ///< the band's type, a field that opens their list
    lv_obj_t* type_val = nullptr;
    lv_obj_t* step[3]  = {};           ///< freq / gain / Q
    lv_obj_t* step_cap[3] = {};
    lv_obj_t* step_val[3] = {};
    m3w::Button step_dec[3], step_inc[3];

    lv_point_precise_t pts[CURVE_N];
    // a finger on a point (touch_gesture.h)
    touch::Point pt;
    int        pt_band = -1;           ///< the band whose point the finger came down on
    lv_point_t pt_off  = {};           ///< the finger from the point's centre, kept while it moves
    // «Скасувати» for a moved point (ui_shell_undo): the band, and the sound that played
    struct Grab { audio_core::EqBand band; int preset = CUSTOM; };
    Grab       grab;                   ///< at the press
    touch::UndoStart<Grab> band_undo;
    int        undo_band = -1;         ///< the band band_undo is about
    int8_t eq_on = -1;
    UiCtx* ctx = nullptr;
};

Eq g;

bool eq_model_loaded = false;

/// One document's worth of room for copies out of the store — not on the LVGL task's stack.
audio_core::EqDoc& doc_scratch() {
    static audio_core::EqDoc d;
    return d;
}

bool reset_eq_widgets() {
    if (!eq_model_loaded) {
        g = Eq{};
        return false;
    }
    // The theme rebuild destroys widgets while an edit may still be waiting for the
    // module's delayed save. Keep its data and revision when clearing widget pointers.
    const auto cfg = g.cfg;
    const auto custom = g.custom;
    const int preset = g.preset;
    const int sel = g.sel;
    const uint32_t revision = g.revision;
    const uint32_t pushed_rev = g.pushed_rev;
    const uint32_t store_rev = g.store_rev;
    g = Eq{};
    g.cfg = cfg;
    g.custom = custom;
    g.preset = preset;
    g.sel = sel;
    g.revision = revision;
    g.pushed_rev = pushed_rev;
    g.store_rev = store_rev;
    return true;
}

/// Does this band's `gain` mean anything? For a pass or a notch it does not: design_band()
/// computes `A` from the gain and then never uses it, so the filter is shaped by frequency
/// and Q alone. Offering a gain control there is what let the handle float away from the
/// curve — the number moved, the sound and the curve did not.
bool has_gain(audio_core::BandType t) {
    return t == audio_core::BandType::PEAK
        || t == audio_core::BandType::LOW_SHELF
        || t == audio_core::BandType::HIGH_SHELF;
}

/// A pass filter is specified by frequency and slope everywhere outside this screen, not by
/// a bare Q. It is the same parameter — a two-pole section always ends up at 12 dB/oct and Q
/// only shapes the knee — but the two alignments that have names are the ones anyone
/// actually wants, and reaching 0.707 by adding 0.1 sixteen times never lands on it.
bool is_pass(audio_core::BandType t) {
    return t == audio_core::BandType::LOW_PASS || t == audio_core::BandType::HIGH_PASS;
}

constexpr float Q_LR12 = 0.500f;   ///< Linkwitz-Riley 12 dB/oct: −6 dB at the corner, halves sum flat
constexpr float Q_BW12 = 0.707f;   ///< Butterworth 12 dB/oct: −3 dB, maximally flat — the default HPF

/// The ladder Q's − and + walk on a pass band. It contains both named alignments exactly;
/// above them the section stops being a filter and becomes a resonator, which is a real
/// thing to want (ported-box emulation, subsonic lift) and stays reachable.
constexpr float PASS_Q[] = {
    0.300f, 0.400f, Q_LR12, 0.600f, Q_BW12, 0.800f, 1.000f, 1.300f,
    1.600f, 2.000f, 2.500f, 3.000f, 4.000f, 5.000f, 6.500f, 8.000f,
};
constexpr int PASS_Q_N = (int)(sizeof(PASS_Q) / sizeof(PASS_Q[0]));

int pass_q_index(float q) {
    int best = 0;
    float d = 1e9f;
    for (int i = 0; i < PASS_Q_N; i++) {
        const float e = fabsf(PASS_Q[i] - q);
        if (e < d) { d = e; best = i; }
    }
    return best;
}

/// Writes what the third stepper should show and returns the caption that belongs above it.
/// A named alignment is a slope; anything else is a width, «Ширина (Q)» as the web's band sheet
/// says it — printing "Спад 3.90" would be a lie about what the number means.
const char* q_text(const audio_core::EqBand& b, char* out, int n) {
    if (is_pass(b.type)) {
        if (fabsf(b.q - Q_LR12) < 0.02f) { snprintf(out, (size_t)n, "LR 12"); return tr(S::eq_slope); }
        if (fabsf(b.q - Q_BW12) < 0.02f) { snprintf(out, (size_t)n, "BW 12"); return tr(S::eq_slope); }
    }
    i18n::num(out, (size_t)n, (int32_t)lroundf(b.q * 100.0f), 2);
    return tr(S::eq_q_width);
}

/// Changing the type into a pass filter starts it at Butterworth. Leftover Q from a peak —
/// 3.9, say — would otherwise turn the filter into a +12 dB resonator the instant the type
/// changed, which is not what picking "High pass" is asking for.
void set_type(audio_core::EqBand& b, audio_core::BandType nt) {
    if (is_pass(nt) && !is_pass(b.type)) b.q = Q_BW12;
    if ((nt == audio_core::BandType::LOW_SHELF || nt == audio_core::BandType::HIGH_SHELF) &&
        b.q > audio_core::SHELF_Q_MAX) b.q = audio_core::SHELF_Q_MAX;
    b.type = nt;
}

/// Vertical position of a handle: THIS band's own response at its own centre frequency,
/// asked of the same function that draws the curve. Pinning it to `gain` is only correct for
/// a peaking filter: a shelf reaches half its gain at the corner, a Butterworth pass is 3 dB
/// down there, a notch is far below, and a disabled band contributes nothing at all.
float handle_db(const audio_core::EqBand& b) {
    float db = audio_core::band_magnitude_db(b, b.freq_hz);
    if (db >  s_range_db) db =  s_range_db;
    if (db < -s_range_db) db = -s_range_db;
    return db;
}

/// Inverse of handle_db for the types where gain exists: the gain that puts the band's own
/// response at `target_db` at its centre. A shelf takes twice the target, because half of it
/// is what lands on the corner.
void set_gain_from_handle(audio_core::EqBand& b, float target_db) {
    if (!has_gain(b.type)) return;            // height is not gain here — leave it alone
    float gdb = (b.type == audio_core::BandType::PEAK) ? target_db : target_db * 2.0f;
    if (gdb < -12.0f) gdb = -12.0f; else if (gdb > 12.0f) gdb = 12.0f;
    b.gain_db = gdb;
}

/// The store's document, into this page's copy.
void pull_from_store() {
    audio_core::EqDoc& d = doc_scratch();
    g.store_rev = audio_core::eq_store().get(d);
    g.cfg = d.cfg;
    g.custom = d.custom;
    g.preset = d.preset;
    if (g.sel >= g.cfg.sec[0].count) g.sel = 0;
}

/// The store moved past what this page shows, and nothing here is waiting to be put — an edit
/// still on its way to the store wins over one that came from the web meanwhile.
bool store_moved() {
    return audio_core::eq_store().revision() != g.store_rev && g.revision == g.pushed_rev;
}

void mark_edited() {
    g.revision++;
    if (g.ctx && g.ctx->mod) g.ctx->mod->lvgl_post(UiLvglModule::Req::EQ_APPLY, (int32_t)g.revision);
}

/// A band was changed by hand. On a preset, the result is now the user's own set — the
/// preset was a starting point, and what was edited from it is what they want to keep.
void edited_by_hand() {
    audio_core::eq_hand_edited(g.cfg, g.custom, g.preset);
    mark_edited();
}

// ── drawing ──

/// A grid line's dB as the web's plot (EqPlot.svelte) labels it: "+6", "0", "−6" (U+2212).
void grid_text(char* out, size_t n, int db) {
    if (db < 0) snprintf(out, n, "\xE2\x88\x92%d", -db);
    else        snprintf(out, n, db > 0 ? "+%d" : "%d", db);
}

void pick_range() {
    const audio_core::EqSection& sec = g.cfg.sec[0];
    float want = 12.0f;
    for (int i = 0; i < sec.count; i++) {
        if (sec.bands[i].enabled && !has_gain(sec.bands[i].type)) { want = 24.0f; break; }
    }
    if (want == s_range_db) return;
    s_range_db = want;
    const int step = (int)(s_range_db / 2.0f);
    for (int i = 0; i < 5; i++) {
        if (!g.grid_y[i]) continue;
        const int db = step * (2 - i);
        const int32_t y = PLOT_Y + (int32_t)(gy((float)db) * (float)PLOT_H);
        lv_obj_set_y(g.grid_y[i], y);
        lv_obj_set_y(g.grid_y_lbl[i], y - 10);
        char b[16];
        grid_text(b, sizeof(b), db);
        lv_label_set_text(g.grid_y_lbl[i], b);
    }
}

void redraw_curve() {
    const auto& t = m3::theme();
    const audio_core::EqSection& sec = g.cfg.sec[0];
    // The whole curve in one call: each band is designed once, not once per point.
    static float f_hz[CURVE_N], curve_db[CURVE_N];
    for (int i = 0; i < CURVE_N; i++) f_hz[i] = xf((float)i / (float)(CURVE_N - 1));
    audio_core::eq_curve_db(sec.bands, sec.count, f_hz, CURVE_N, curve_db);
    for (int i = 0; i < CURVE_N; i++) {
        const float x = (float)i / (float)(CURVE_N - 1);
        float db = curve_db[i];
        if (db >  s_range_db) db =  s_range_db;
        if (db < -s_range_db) db = -s_range_db;
        g.pts[i].x = (lv_value_precise_t)(x * (float)PLOT_W);
        g.pts[i].y = (lv_value_precise_t)(gy(db) * (float)PLOT_H);
    }
    lv_line_set_points(g.curve, g.pts, CURVE_N);
    // PEQ off: the curve is still worth seeing — it is what switching on will do — but it is
    // not what is heard, and it says so by colour.
    lv_obj_set_style_line_color(g.curve, m3::col(g.eq_on == 0 ? t.c.outline : t.c.primary), 0);
}

void place_handles() {
    const auto& t = m3::theme();
    const audio_core::EqSection& sec = g.cfg.sec[0];
    for (int i = 0; i < audio_core::MAX_BANDS; i++) {
        if (i >= sec.count) { uih::show(g.handle[i], false); continue; }
        uih::show(g.handle[i], true);
        const audio_core::EqBand& b = sec.bands[i];
        float x = fx(b.freq_hz);
        if (x < 0.0f) x = 0.0f; else if (x > 1.0f) x = 1.0f;
        lv_obj_set_pos(g.handle[i],
                       PLOT_X + (int32_t)(x * (float)PLOT_W) - HANDLE / 2,
                       PLOT_Y + (int32_t)(gy(handle_db(b)) * (float)PLOT_H) - HANDLE / 2);
        const bool on = (i == g.sel);
        // Selected: primary. On: a tonal disc. Off: a ring, so a band that does nothing looks
        // like it does nothing.
        const uint32_t bg = on ? t.c.primary : t.c.secondary_container;
        lv_obj_set_style_bg_color(g.handle[i], m3::col(bg), 0);
        lv_obj_set_style_bg_opa(g.handle[i], (on || b.enabled) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(g.handle[i], (on || b.enabled) ? 0 : 2, 0);
        lv_obj_set_style_text_color(g.hlabel[i], m3::col(on ? t.c.on_primary
                                                         : b.enabled ? t.c.on_secondary_container
                                                                     : t.c.outline), 0);
        if (on) lv_obj_move_foreground(g.handle[i]);
    }
}

void refresh_band_panel() {
    const audio_core::EqBand& b = g.cfg.sec[0].bands[g.sel];
    char buf[24], n[12];
    snprintf(n, sizeof(n), "%d", g.sel + 1);
    i18n::fmt(buf, sizeof(buf), tr(S::eq_band_n), {{"n", n}});
    lv_label_set_text(g.band_lbl, buf);
    if (b.enabled) lv_obj_add_state(g.band_sw, LV_STATE_CHECKED);
    else           lv_obj_remove_state(g.band_sw, LV_STATE_CHECKED);
    // Numbers with the language's decimal separator, units in its words.
    char v[16];
    if (b.freq_hz >= 1000.0f) snprintf(buf, sizeof(buf), "%s %s", i18n::num(v, sizeof(v), (int32_t)lroundf(b.freq_hz / 10.0f), 2), tr(S::unit_khz));
    else                      snprintf(buf, sizeof(buf), "%.0f %s", (double)b.freq_hz, tr(S::unit_hz));
    lv_label_set_text(g.step_val[0], buf);
    // Showing "+7.0 dB" on a low pass invites pressing a + that is wired to nothing.
    const bool gain = has_gain(b.type);
    if (gain) snprintf(buf, sizeof(buf), "%s %s", i18n::num(v, sizeof(v), (int32_t)lroundf(b.gain_db * 10.0f), 1, true), tr(S::unit_db));
    else      snprintf(buf, sizeof(buf), "—");
    lv_label_set_text(g.step_val[1], buf);
    m3w::button_enable(g.step_dec[1], gain);
    m3w::button_enable(g.step_inc[1], gain);
    lv_label_set_text(g.step_cap[2], q_text(b, buf, (int)sizeof(buf)));
    lv_label_set_text(g.step_val[2], buf);
    if (g.type_val) lv_label_set_text(g.type_val, tr(TYPE_NAME[(int)b.type]));
}

void choose_preset(int p);
void refresh_all();

/// The sounds' list over the page; the page chooses as its own edit (choose_preset).
void sounds_cb(lv_event_t*) { ui_sound_pick_show(g.preset, choose_preset); }

/// What the line under the field says: the EQ is off; a genre just became «Власний»; a genre
/// does not change; there is nothing to hear. «Скинути «Власний»» is there while «Власний» plays.
void paint_top() {
    uih::set_text(g.sound_val, g.sound_val_txt, sizeof(g.sound_val_txt), preset_name(g.preset));
    const bool mine = g.preset == CUSTOM;
    uih::show(g.reset.obj, mine);

    const audio_core::EqSection& sec = g.cfg.sec[0];
    bool any = false;
    for (int i = 0; i < sec.count; i++) any = any || sec.bands[i].enabled;
    m3w::button_enable(g.reset, any);
    // The line under the row is a state, not a lesson: «Смуг немає» when «Власний» has none on.
    uih::set_text(g.note, g.note_txt, sizeof(g.note_txt), mine && !any ? tr(S::eq_no_bands) : "");
}

/// «Повернути»: the bands that were on are on again.
void unreset_cb(int32_t, void*) {
    audio_core::EqSection& sec = g.cfg.sec[0];
    for (int i = 0; i < sec.count; i++) if (g.reset_on & (1u << i)) sec.bands[i].enabled = true;
    g.reset_on = 0;
    edited_by_hand();
    refresh_all();
}

/// «Скинути «Власний»»: every band off — what «Рівно» is — with «Повернути» for a while.
void reset_cb(lv_event_t*) {
    audio_core::EqSection& sec = g.cfg.sec[0];
    g.reset_on = 0;
    for (int i = 0; i < sec.count; i++) {
        if (sec.bands[i].enabled) g.reset_on |= (uint16_t)(1u << i);
        sec.bands[i].enabled = false;
    }
    edited_by_hand();
    refresh_all();
    ui_shell_undo(UNDO_EQ_RESET, tr(S::eq_reset_done), unreset_cb, 0, nullptr, tr(S::sound_undo));
}

void refresh_all() {
    pick_range();                  // before anything is placed: it moves every y coordinate
    redraw_curve();
    place_handles();
    refresh_band_panel();
    paint_top();
}

// ── interaction ──

void select_band(int i) {
    const int n = g.cfg.sec[0].count;
    if (n <= 0) return;
    g.sel = ((i % n) + n) % n;
    place_handles();
    refresh_band_panel();
}

bool same_band(const audio_core::EqBand& a, const audio_core::EqBand& b) {
    return a.type == b.type && a.enabled == b.enabled && fabsf(a.freq_hz - b.freq_hz) < 0.01f &&
           fabsf(a.gain_db - b.gain_db) < 0.01f && fabsf(a.q - b.q) < 0.001f;
}

/// A band as the snackbar says it — as the band panel writes its numbers.
void band_text(char* out, size_t n, const audio_core::EqBand& b) {
    char f[24], v[12];
    if (b.freq_hz >= 1000.0f) snprintf(f, sizeof(f), "%s %s", i18n::num(v, sizeof(v), (int32_t)lroundf(b.freq_hz / 10.0f), 2), tr(S::unit_khz));
    else                      snprintf(f, sizeof(f), "%.0f %s", (double)b.freq_hz, tr(S::unit_hz));
    if (has_gain(b.type)) snprintf(out, n, "%s %s %s", f, i18n::num(v, sizeof(v), (int32_t)lroundf(b.gain_db * 10.0f), 1, true), tr(S::unit_db));
    else                  snprintf(out, n, "%s", f);
}

void choose_preset(int p);

/// «Скасувати»: the band where the finger found it. A genre a finger changed is «Власний» now
/// (eq_hand_edited); putting it back chooses the genre again.
void undo_band_cb(int32_t i, void*) {
    if (i < 0 || i >= g.cfg.sec[0].count) return;
    const Eq::Grab from = g.band_undo.from;
    g.band_undo.clear();
    if (from.preset != CUSTOM) {
        choose_preset(from.preset);
        return;
    }
    g.cfg.sec[0].bands[i] = from.band;
    edited_by_hand();
    refresh_all();
}

/// A finger moved band `i`: «Скасувати», from where it started (touch_gesture.h).
void note_band(int i) {
    const audio_core::EqBand& now = g.cfg.sec[0].bands[i];
    if (g.undo_band != i) g.band_undo.clear();          // another band: its own start
    g.undo_band = i;
    const Eq::Grab from = g.band_undo.note(g.grab, lv_tick_get());
    if (from.preset == g.preset && same_band(from.band, now)) {   // back where it started
        g.band_undo.clear();
        ui_shell_undo_drop(UNDO_EQ_BAND + i);
        return;
    }
    char a[40], b[40], n[12], name[24], txt[128];
    band_text(a, sizeof(a), from.band);
    band_text(b, sizeof(b), now);
    snprintf(n, sizeof(n), "%d", i + 1);
    i18n::fmt(name, sizeof(name), tr(S::eq_band_n), {{"n", n}});
    // "›" for the arrow, as the tech chip's "44.1 › 48 kHz": no Roboto subset has an arrow
    snprintf(txt, sizeof(txt), "%s: %s \xE2\x80\xBA %s", name, a, b);
    ui_shell_undo(UNDO_EQ_BAND + i, txt, undo_band_cb, i, nullptr);
}

/// A point picked up — held, or moving — wears a ring.
void lift(int i, bool on) {
    if (i < 0 || i >= audio_core::MAX_BANDS || !g.handle[i]) return;
    lv_obj_set_style_outline_width(g.handle[i], on ? 3 : 0, 0);
}

/// The point of band `i`, its centre at (x, y) on the screen: x → frequency, y → gain.
void drag_to(int i, int32_t x, int32_t y) {
    lv_area_t a;
    lv_obj_get_coords(g.hit, &a);                 // the plot on the screen, wherever it is
    float nx = (float)(x - a.x1) / (float)PLOT_W;
    float ny = (float)(y - a.y1) / (float)PLOT_H;
    if (nx < 0.0f) nx = 0.0f; else if (nx > 1.0f) nx = 1.0f;
    if (ny < 0.0f) ny = 0.0f; else if (ny > 1.0f) ny = 1.0f;

    audio_core::EqBand& b = g.cfg.sec[0].bands[i];
    b.freq_hz = xf(nx);
    set_gain_from_handle(b, yg(ny)); // a no-op for pass and notch: only x means anything there
    b.enabled = true;                 // touching a band is how you switch it on
    edited_by_hand();
    refresh_all();
}

/// A finger on a band's point, and only there (touch_gesture.h): a press elsewhere on the plot
/// does not move the chosen band to it. The press picks the band; the point moves once held
/// HOLD_MS or once the finger went SLOP px, and keeps its distance from the finger so it does
/// not jump. A tap picks, and no more.
void point_cb(lv_event_t* e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (g.pt_band != i) return;
        const bool moved = g.pt.end() == touch::Point::DRAG;
        g.pt_band = -1;
        lift(i, false);
        if (moved) note_band(i);
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const uint32_t now = lv_tick_get();
    if (code == LV_EVENT_PRESSED) {
        if (i >= g.cfg.sec[0].count) return;
        select_band(i);
        lv_area_t a;
        lv_obj_get_coords(g.handle[i], &a);
        g.pt_off = {p.x - (a.x1 + a.x2) / 2, p.y - (a.y1 + a.y2) / 2};
        g.grab = {g.cfg.sec[0].bands[i], g.preset};
        g.pt_band = i;
        g.pt.down(p.x, p.y, now);
        return;
    }
    // PRESSING comes every input cycle while the finger is down, still or not: the hold's clock
    if (code != LV_EVENT_PRESSING || g.pt_band != i) return;
    const touch::Point::State was = g.pt.state();
    const touch::Point::State st = g.pt.move(p.x, p.y, now);
    if (st != was) lift(i, true);
    if (st == touch::Point::DRAG) drag_to(i, p.x - g.pt_off.x, p.y - g.pt_off.y);
}

/// One step of one parameter. `which` is 0 freq / 1 gain / 2 Q, `dir` is +1 or -1.
void adjust(int which, int dir) {
    audio_core::EqBand& b = g.cfg.sec[0].bands[g.sel];
    switch (which) {
    case 0: {   // frequency, 1/12 octave per press
        float f = b.freq_hz * (dir > 0 ? 1.0595f : 1.0f / 1.0595f);
        if (f < 20.0f) f = 20.0f; else if (f > 20000.0f) f = 20000.0f;
        b.freq_hz = f;
        break;
    }
    case 1: {
        if (!has_gain(b.type)) return;        // no gain, so no edit and nothing to persist
        float gdb = b.gain_db + dir * 0.5f;
        if (gdb < -12.0f) gdb = -12.0f; else if (gdb > 12.0f) gdb = 12.0f;
        b.gain_db = gdb;
        break;
    }
    default: {
        if (is_pass(b.type)) {
            int i = pass_q_index(b.q) + dir;
            if (i < 0) i = 0; else if (i >= PASS_Q_N) i = PASS_Q_N - 1;
            b.q = PASS_Q[i];
        } else {
            // A shelf's Q stops at SHELF_Q_MAX: above it the shelf overshoots its own gain, and
            // the design caps it there anyway — a value past it would only be a number that lies.
            const bool shelf = b.type == audio_core::BandType::LOW_SHELF ||
                               b.type == audio_core::BandType::HIGH_SHELF;
            const float q_max = shelf ? audio_core::SHELF_Q_MAX : 8.0f;
            float q = b.q + dir * 0.1f;
            if (q < 0.3f) q = 0.3f; else if (q > q_max) q = q_max;
            b.q = q;
        }
        break;
    }
    }
    b.enabled = true;
    edited_by_hand();
    refresh_all();
}

void step_cb(lv_event_t* e) {
    const int code = (int)(intptr_t)lv_event_get_user_data(e);   // param*10 + (0 dec, 1 inc)
    adjust(code / 10, (code % 10) ? 1 : -1);
}

/// The type the list chose for the band: heard once, not on the way through the others.
void choose_type(int i) {
    audio_core::EqBand& b = g.cfg.sec[0].bands[g.sel];
    if (i < 0 || i >= (int)(sizeof(TYPE_NAME) / sizeof(TYPE_NAME[0])) || i == (int)b.type) return;
    set_type(b, (audio_core::BandType)i);
    edited_by_hand();
    refresh_all();
}

/// The six types as a list (ui_pick.cpp), the band's own ticked. Not a button that steps through
/// them: every step on the way would change the response — a low pass at 1 kHz between a shelf
/// and the type wanted.
void pick_type() {
    constexpr int N = (int)(sizeof(TYPE_NAME) / sizeof(TYPE_NAME[0]));
    const char* names[N];
    for (int i = 0; i < N; i++) names[i] = tr(TYPE_NAME[i]);
    ui_pick_show(tr(S::eq_type), names, N, (int)g.cfg.sec[0].bands[g.sel].type, choose_type);
}

void type_cb(lv_event_t*)      { pick_type(); }
void band_prev_cb(lv_event_t*) { select_band(g.sel - 1); }
void band_next_cb(lv_event_t*) { select_band(g.sel + 1); }

/// The band's own switch: the one way to take a single band out without touching the others.
void band_sw_cb(lv_event_t*) {
    audio_core::EqBand& b = g.cfg.sec[0].bands[g.sel];
    b.enabled = lv_obj_has_state(g.band_sw, LV_STATE_CHECKED);
    edited_by_hand();
    refresh_all();
}

void choose_preset(int p) {
    // «Власний» is kept as it is edited, and choosing it brings it back (eq_presets.h).
    if (!audio_core::eq_choose_preset(g.cfg, g.custom, g.preset, p)) return;
    if (g.sel >= g.cfg.sec[0].count) g.sel = 0;
    mark_edited();
    refresh_all();
}

void peq_cb(lv_event_t*) {
    if (g.ctx && g.ctx->mod) {
        g.ctx->mod->lvgl_post(UiLvglModule::Req::EQ_ENABLED, lv_obj_has_state(g.peq, LV_STATE_CHECKED) ? 1 : 0);
    }
}

/// One stepper: − at the left end, + at the right, the value between them — the buttons sit
/// on either side of what they change, a hundred and fifty pixels apart, so a thumb aimed at
/// one cannot land on the other.
lv_obj_t* stepper(lv_obj_t* parent, int32_t x, int i) {
    const auto& t = m3::theme();
    constexpr int32_t BTN = NAV_BTN;
    lv_obj_t* s = m3w::surface(parent, x, B2_Y, STEP_W, B2_H, t.c.surface_container, m3::shape::M);
    const int32_t mid_w = STEP_W - 2 * BTN;
    // the name and the number, label_s's 21 and body_l's 25 lines, in the middle of the 64
    const int32_t cap_y = (B2_H - lv_font_get_line_height(t.t.label_s) - lv_font_get_line_height(t.t.body_l)) / 2;
    g.step_cap[i] = m3w::text(s, t.t.label_s, t.c.on_surface_var, param_name(i));
    lv_obj_set_width(g.step_cap[i], mid_w);
    lv_obj_set_style_text_align(g.step_cap[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.step_cap[i], BTN, cap_y);
    g.step_val[i] = m3w::text(s, t.t.body_l, t.c.on_surface, "");
    lv_obj_set_width(g.step_val[i], mid_w);
    lv_obj_set_style_text_align(g.step_val[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.step_val[i], BTN, cap_y + lv_font_get_line_height(t.t.label_s));
    g.step_dec[i] = m3w::icon_button(s, 0, 0, BTN, UI_SYM_REMOVE, t.t.icon_m);
    g.step_inc[i] = m3w::icon_button(s, STEP_W - BTN, 0, BTN, UI_SYM_ADD, t.t.icon_m);
    lv_obj_add_event_cb(g.step_dec[i].obj, step_cb, LV_EVENT_CLICKED, (void*)(intptr_t)(i * 10));
    lv_obj_add_event_cb(g.step_inc[i].obj, step_cb, LV_EVENT_CLICKED, (void*)(intptr_t)(i * 10 + 1));
    return s;
}

// ── build ──
lv_obj_t* build(lv_obj_t* parent, UiCtx& c) {
    const auto& t = m3::theme();
    const bool have_model = reset_eq_widgets();
    g.ctx = &c;
    s_range_db = 12.0f;
    if (!have_model) {
        // The document is the store's; the audio module loaded it at boot.
        if (!audio_core::eq_store().loaded()) {
            char path[160], custom[160];
            modesp::UserStore::read_path("eq.json", path, sizeof(path));
            modesp::UserStore::read_path("eq_custom.json", custom, sizeof(custom));
            audio_core::eq_store().load(path, custom);
        }
        pull_from_store();
        eq_model_loaded = true;
    } else if (store_moved()) {
        pull_from_store();                     // the web changed it while this page was away
    }

    lv_obj_t* root = uih::box(parent, 0, 0, uig::CONT_W, uig::CONT_H);

    // ── top row: the switch, the sounds' field, «Скинути «Власний»» while it plays; the line
    //    under them (paint_top). The limiter's meter is Service's (screen_svc.cpp R_LIM). ──
    // the switch had no word: «Увімкнено», and the row takes a finger anywhere
    lv_obj_t* peq_row = m3w::box(root, 0, 0, PEQ_W, TOP_H);
    g.peq = m3w::switch_m3(peq_row, 6, (TOP_H - 32) / 2, false);
    lv_obj_add_event_cb(g.peq, peq_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t* peq_lbl = m3w::text(peq_row, t.t.label_m, t.c.on_surface, tr(S::eq_enabled));
    lv_obj_set_pos(peq_lbl, 6 + 52 + 12, (TOP_H - lv_font_get_line_height(t.t.label_m)) / 2);
    lv_obj_set_width(peq_lbl, PEQ_W - (6 + 52 + 12));
    lv_label_set_long_mode(peq_lbl, LV_LABEL_LONG_DOT);
    m3w::switch_row(peq_row, g.peq);

    g.sounds = ui_sound_field(root, FIELD_W, FIELD_H, &g.sound_val);
    lv_obj_set_pos(g.sounds, FIELD_X, (TOP_H - FIELD_H) / 2);
    lv_obj_add_event_cb(g.sounds, sounds_cb, LV_EVENT_CLICKED, nullptr);

    g.reset = m3w::button_text(root, uig::CONT_W - RESET_W, 0, RESET_W, TOP_H, tr(S::eq_reset_mine));
    lv_obj_add_event_cb(g.reset.obj, reset_cb, LV_EVENT_CLICKED, nullptr);

    // ── the curve card ──
    g.card = m3w::surface(root, 0, CARD_Y, uig::CONT_W, CARD_H, t.c.surface_low, m3::shape::L);
    const float grid_f[] = {50, 100, 200, 500, 1000, 2000, 5000, 10000};
    const char* grid_l[] = {"50", "100", "200", "500", "1k", "2k", "5k", "10k"};
    for (int i = 0; i < 8; i++) {
        const int32_t x = PLOT_X + (int32_t)(fx(grid_f[i]) * (float)PLOT_W);
        m3w::surface(g.card, x, PLOT_Y, 1, PLOT_H, t.c.outline_var, 0);
        lv_obj_t* l = m3w::text(g.card, t.t.label_s, t.c.outline, grid_l[i]);
        lv_obj_set_pos(l, x - 12, PLOT_Y + PLOT_H + 6);
    }
    for (int i = 0; i < 5; i++) {
        const int db = (int)(s_range_db / 2.0f) * (2 - i);
        const int32_t y = PLOT_Y + (int32_t)(gy((float)db) * (float)PLOT_H);
        g.grid_y[i] = m3w::surface(g.card, PLOT_X, y, PLOT_W, 1, db == 0 ? t.c.outline : t.c.outline_var, 0);
        char b[16];
        grid_text(b, sizeof(b), db);
        g.grid_y_lbl[i] = m3w::text(g.card, t.t.label_s, t.c.outline, b);
        lv_obj_set_pos(g.grid_y_lbl[i], 6, y - 10);
    }

    g.curve = lv_line_create(g.card);
    lv_obj_set_pos(g.curve, PLOT_X, PLOT_Y);
    lv_obj_set_style_line_width(g.curve, 3, 0);
    lv_obj_set_style_line_rounded(g.curve, true, 0);

    // the plot's area, for where a point is dragged to; it takes no finger itself
    g.hit = m3w::box(g.card, PLOT_X, PLOT_Y, PLOT_W, PLOT_H);

    // «Смуг немає», in the middle of the plot, which is empty while it shows (paint_top)
    g.note = m3w::text(g.card, t.t.label_m, t.c.tertiary, "");
    lv_obj_set_size(g.note, PLOT_W, lv_font_get_line_height(t.t.label_m));
    lv_obj_set_style_text_align(g.note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(g.note, PLOT_X, PLOT_Y + PLOT_H / 3);
    lv_label_set_long_mode(g.note, LV_LABEL_LONG_DOT);

    for (int i = 0; i < audio_core::MAX_BANDS; i++) {
        g.handle[i] = m3w::surface(g.card, 0, 0, HANDLE, HANDLE, t.c.secondary_container, m3::shape::FULL);
        lv_obj_set_style_border_color(g.handle[i], m3::col(t.c.outline), 0);
        lv_obj_set_style_outline_color(g.handle[i], m3::col(t.c.primary), 0);    // lift(): picked up
        lv_obj_set_style_outline_pad(g.handle[i], 3, 0);
        lv_obj_add_flag(g.handle[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(g.handle[i], HANDLE_REACH);
        for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST})
            lv_obj_add_event_cb(g.handle[i], point_cb, code, (void*)(intptr_t)i);
        g.hlabel[i] = m3w::text(g.handle[i], t.t.label_s, t.c.on_secondary_container, "");
        lv_label_set_text_fmt(g.hlabel[i], "%d", i + 1);
        lv_obj_center(g.hlabel[i]);
    }

    // ── the band panel: which band, on or off, what type; then its three numbers ──
    g.band_box = m3w::box(root, 0, B1_Y, BAND_W, B1_H);
    lv_obj_set_style_radius(g.band_box, m3::shape::FULL, 0);
    m3w::Button prev = m3w::icon_button(g.band_box, 0, 0, NAV_BTN, UI_SYM_CHEVRON_LEFT, t.t.icon_m);
    lv_obj_add_event_cb(prev.obj, band_prev_cb, LV_EVENT_CLICKED, nullptr);
    g.band_lbl = m3w::text(g.band_box, t.t.title_s, t.c.on_surface, "");
    lv_obj_set_width(g.band_lbl, BAND_W - 2 * NAV_BTN);
    lv_obj_set_style_text_align(g.band_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g.band_lbl, LV_ALIGN_CENTER, 0, 0);
    m3w::Button next = m3w::icon_button(g.band_box, BAND_W - NAV_BTN, 0, NAV_BTN, UI_SYM_CHEVRON, t.t.icon_m);
    lv_obj_add_event_cb(next.obj, band_next_cb, LV_EVENT_CLICKED, nullptr);

    // the band's switch and its word, one row a finger takes anywhere: the word alone was 23 px
    lv_obj_t* sw_row = m3w::box(root, BSW_X, B1_Y, BSW_W, B1_H);
    g.band_sw = m3w::switch_m3(sw_row, 8, (B1_H - 32) / 2, true);
    lv_obj_add_event_cb(g.band_sw, band_sw_cb, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_t* sw_lbl = m3w::text(sw_row, t.t.label_m, t.c.on_surface, tr(S::eq_band_on));
    lv_obj_set_pos(sw_lbl, 8 + 52 + 12, (B1_H - lv_font_get_line_height(t.t.label_m)) / 2);
    m3w::switch_row(sw_row, g.band_sw);

    lv_obj_t* type_cap = m3w::text(root, t.t.label_s, t.c.on_surface_var, tr(S::eq_type));
    lv_obj_set_pos(type_cap, uig::CONT_W - TYPE_W - 44, B1_Y + (B1_H - lv_font_get_line_height(t.t.label_s)) / 2);
    // the type: a field outlined as the presets' is, that opens their list
    g.type_field = m3w::box(root, uig::CONT_W - TYPE_W, B1_Y, TYPE_W, B1_H);
    lv_obj_set_style_border_width(g.type_field, 1, 0);
    lv_obj_set_style_border_color(g.type_field, m3::col(t.c.outline), 0);
    lv_obj_set_style_radius(g.type_field, m3::shape::XS, 0);
    lv_obj_add_flag(g.type_field, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g.type_field, type_cb, LV_EVENT_CLICKED, nullptr);
    g.type_val = m3w::text(g.type_field, t.t.body_l, t.c.on_surface, "");
    lv_obj_align(g.type_val, LV_ALIGN_LEFT_MID, 14, 0);
    lv_obj_t* drop = m3w::icon(g.type_field, t.t.icon_m, t.c.on_surface_var, UI_SYM_DROPDOWN);
    lv_obj_align(drop, LV_ALIGN_RIGHT_MID, -8, 0);

    for (int i = 0; i < 3; i++) g.step[i] = stepper(root, i * (STEP_W + STEP_GAP), i);

    refresh_all();
    return root;
}

void refresh(lv_obj_t*, UiCtx& c) {
    if (store_moved()) {                       // an edit from the web
        pull_from_store();
        refresh_all();
    }
    const int8_t on = c.s.eq_enabled ? 1 : 0;
    if (on != g.eq_on) {
        g.eq_on = on;
        if (on) lv_obj_add_state(g.peq, LV_STATE_CHECKED);
        else    lv_obj_remove_state(g.peq, LV_STATE_CHECKED);
        redraw_curve();
        paint_top();                           // "only the equalizer is off"
    }
}

} // namespace

const UiScreenDef UI_SCREEN_EQ = {"eq", UI_SYM_EQ, build, refresh, nullptr, nullptr};

void ui_eq_copy_doc(audio_core::EqDoc& out) {
    out.cfg = g.cfg;
    out.custom = g.custom;
    out.preset = g.preset;
}
uint32_t ui_eq_revision() { return g.revision; }
const char* ui_eq_preset_name(int preset) { return preset_name(preset); }
void ui_eq_pushed(uint32_t revision, uint32_t store_rev) {
    g.pushed_rev = revision;
    g.store_rev = store_rev;
}
