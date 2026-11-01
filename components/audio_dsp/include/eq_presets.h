/**
 * @file eq_presets.h
 * @brief The sounds (presets) and the rules for «Власний» — shared by the panel and the web.
 *
 * They lived in modules/ui_lvgl/src/screen_eq.cpp while the screen was the only editor. The
 * web edits the EQ too now, and two copies of "which
 * preset is on" and "what a hand edit does to it" would be two answers. So both live here,
 * pure, and both editors call them.
 *
 * The rules:
 *   - «Власний» is the user's own set, kept while a preset is heard, and choosing it brings
 *     the set back; a preset is an attempt, never a loss.
 *   - an edit by hand makes the edited result «Власний», on a preset or not. So «Власний»
 *     always holds what was last tuned by hand, and leaving it has nothing left to keep.
 *
 * Eight sounds are offered: «Власний» and the
 * seven after it. The presets offered before stay in the table, not offered, so a set saved
 * as one of them still plays under its name. Ids never change, and an id a sound had before
 * is still understood (eq_preset_find).
 */

#pragma once

#include "eq_config.h"

#include <cstddef>

namespace audio_core {

/// Every preset uses the same ten bands: a low shelf, eight peaks an octave apart, a high
/// shelf, with gains fitted by tools/eq/fit_presets.py.
static constexpr int EQ_PRESET_BANDS = 10;
/// Index 0 is not a preset but the user's own set.
static constexpr int EQ_PRESET_CUSTOM = 0;
/// The sounds offered are the table's first eight; the rest are kept for sets saved as them.
static constexpr int EQ_PRESET_SHOWN = 8;

struct EqPreset {
    const char* id;          ///< stable, ASCII — what the web names it by and translates
    const char* name;        ///< as the panel shows it (Ukrainian)
    float       g[EQ_PRESET_BANDS];
};

int eq_preset_count();
/// The preset at `p`; the «Власний» entry for an index out of range.
const EqPreset& eq_preset(int p);
/// The preset `id` names — its own id or one it had before ("techno" is Електроніка); -1 if none.
int eq_preset_find(const char* id, size_t len);

/// How many sounds are offered while `active` plays, and the n-th of them: the eight, then
/// `active` itself when it is one of the presets kept for old sets — it is never shown as
/// something else while it plays.
int eq_offered_count(int active);
int eq_offered(int n, int active);

/// The section preset `p` puts on: the ten bands of the layout at the table's gains. The rest
/// of the section — its id, trim, delay — is taken from `from`. `p` == custom copies `from`.
void eq_build_preset(int p, const EqSection& from, EqSection& out);

/// Same bands, within the rounding a file round trip leaves.
bool eq_same_section(const EqSection& a, const EqSection& b);

/// Which preset `cfg` is: the one whose shape, built from `custom`, it holds; custom if none.
int eq_detect_preset(const EqSection& cfg, const EqSection& custom);

/// Choose preset `p`; «Власний» brings the user's set back — also when what plays under that
/// name is not the set (a sound this build does not have, EqStore::load). False when nothing
/// changes (out of range, or already on).
bool eq_choose_preset(EqConfig& cfg, EqConfig& custom, int& preset, int p);

/// A band of `cfg` was edited by hand: the result is «Власний».
void eq_hand_edited(const EqConfig& cfg, EqConfig& custom, int& preset);

/// The flat ten-band set a panel with no readable eq.json starts from.
void eq_default_config(EqConfig& cfg);

} // namespace audio_core
