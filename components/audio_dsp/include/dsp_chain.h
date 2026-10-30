/**
 * @file dsp_chain.h
 * @brief Stereo DSP chain of the audio core: PEQ → volume → limiter, plus meters.
 *
 * Float, block-based, zero heap after init. Runs inside the audio task only; the
 * module changes parameters through set_* calls that are applied at block boundaries
 * (the task owns the object — no locking, commands arrive via the queue).
 *
 * Filters: one trapezoidal state-variable section per band (dsp_kernels.h says why not a
 * biquad), designed here with the RBJ Audio-EQ-Cookbook responses.
 */

#pragma once

#include <cstdint>
#include <cstddef>

#include "dsp_kernels.h"
#include "leveler.h"

namespace audio_core {

static constexpr int   MAX_BANDS   = 10;
/// The rate everything downstream is running at, right now.
///
/// It used to be a constant, because the DAC and the ADC share a clock and 48 kHz was the
/// one rate they both ran at. That cost a resampler on every 44.1 kHz source,
/// and the resampler cost 800 ms of CPU per second of audio — measured, and more than the
/// MP3 decoder by a factor of thirty-five. Every ESP32 radio and player worth copying does
/// the obvious thing instead: the output clock follows the source
/// (docs/CONCEPT.md, "The output clock follows the source"). So this moves.
///
/// Written by the audio task when it retunes I2S, read by everything that turns a frequency
/// into a coefficient. A plain float: it changes between blocks, never during one.
extern float g_fs_hz;
inline float fs_hz() { return g_fs_hz; }
/// What the coefficients were designed at, where a compile-time value is still wanted.
static constexpr float FS_HZ_DEFAULT = 48000.0f;
/// Full scale INSIDE the chain. Everything after the entry gain is measured against the
/// loudest level the amplifier reproduces cleanly — the output stage (modules/audio) multiplies
/// by k_out to put 1.0 exactly there — so 1.0 is the amplifier's ceiling, not the DAC's full
/// scale, and the brickwall's threshold is 1.0.
static constexpr float CEILING     = 1.0f;
static constexpr float TONE_LEVEL  = 0.251f;    // −12 dB under the ceiling

enum class BandType : uint8_t { PEAK = 0, LOW_SHELF, HIGH_SHELF, LOW_PASS, HIGH_PASS, NOTCH };

struct EqBand {
    BandType type    = BandType::PEAK;
    float    freq_hz = 1000.0f;
    float    gain_db = 0.0f;
    float    q       = 1.0f;
    bool     enabled = false;
};

/// A band as a trapezoidal SVF: g, damping k and the output mix — before anything is rounded to
/// float. False for a band that cannot be designed (it is then a wire).
struct SvfDesign {
    double g = 0.0, k = 0.0, m0 = 1.0, m1 = 0.0, m2 = 0.0;
};

/// How a band's analogue prototype (the RBJ cookbook's) becomes a digital filter.
enum class Design : uint8_t {
    /// The bilinear transform, prewarped at f0: RBJ's own digital filter, exactly. Near
    /// Nyquist the response is squeezed ("cramping") — at 44.1 kHz a 12 kHz shelf is a
    /// different curve from the same shelf at 96 kHz.
    BILINEAR,
    /// M. Vicanek, "Matched Second Order Digital Filters" (2016): the poles by impulse
    /// invariance, the zeros chosen so |H| equals the analogue prototype's at DC, at Nyquist and
    /// at f0. For peaks, shelves and notches; passes stay bilinear. Falls back to bilinear
    /// where the match is not realisable.
    MATCHED,
};

/// Shelves take Q up to this. Above ~0.7 a shelf overshoots its own gain before settling —
/// at Q 8 a +12 dB shelf peaks at +27.6 dB, a boost nobody asked for and the screen's ±12 dB
/// plot does not show.
static constexpr float SHELF_Q_MAX = 1.0f;

bool design_svf_params(const EqBand& band, double fs_hz, SvfDesign& d, Design how = Design::MATCHED);

/// Design a band as one SVF section at `fs_hz`. Worked in double and stored in float: the
/// arithmetic is negligible next to a block, and a low corner at a high rate is exactly where
/// float design loses the tenths of a dB that make two rates sound different.
bool design_svf(const EqBand& band, double fs_hz, SvfCoef& c, Design how = Design::MATCHED);

/// |H|² of a band's analogue prototype at `f_hz` — what MATCHED aims at.
double analog_mag2(const EqBand& band, double f_hz);

/// |H| of a designed section, in dB, at `f_hz` — closed form, no complex arithmetic.
double svf_magnitude_db(const SvfDesign& d, double fs_hz, double f_hz);

/// Magnitude of one band at `f_hz`, in dB. A disabled or undesignable band is 0 dB.
/// This exists so the EQ curve on screen is computed from the SAME design the audio path
/// runs — a second implementation of the maths for drawing would eventually disagree with
/// what is heard, and the disagreement would be invisible.
float band_magnitude_db(const EqBand& band, float f_hz);

/// Summed magnitude of a band list at `f_hz`, in dB (sections in series add in dB).
float eq_magnitude_db(const EqBand* bands, int count, float f_hz);

/// The summed curve at `n` frequencies at once: each band is designed once, not once per
/// point — the screen draws 97 points, and designing ten bands per point was 970 designs a
/// redraw.
void eq_curve_db(const EqBand* bands, int count, const float* f_hz, int n, float* out_db);

/// The gain envelope both limiters run on (DspChain::process). Every D frames — a "knot" — it
/// takes the largest gain the knot may have, keeps the smallest of the last M+1 of those (the
/// look-ahead: the signal it is applied to is delayed by (M+1)·D frames), holds a peak's gain
/// while peaks of that size keep coming, releases slowly, and averages the last M knots. The
/// gain then runs in a straight line from knot to knot: g at the knot, dg per frame.
struct KnotEnv {
    static constexpr int MAX_M = 33;
    int      M = 9;
    uint32_t D = 8, hold = 240;
    float    kr = 0.00067f;
    float    rh[MAX_M + 1];          ///< allowed gain at the last M+1 knots
    int      rh_i = 0;
    float    gh[MAX_M];              ///< released gain at the last M knots, averaged
    int      gh_i = 0;
    float    held = 1.0f, grel = 1.0f;
    uint32_t cnt = 0;
    float    G1 = 1.0f, g = 1.0f, dg = 0.0f;

    /// M knots of look-ahead and smoothing, D frames a knot, a hold and a release in seconds.
    void configure(int m, uint32_t d, float hold_s, float release_s, float fs);
    void reset();
    /// The knot just reached may have at most gain `r` (≤ 1). Sets g and dg for the next chunk.
    void knot(float r);
};

class DspChain {
public:
    /// Block size the caller must use (frames per process()).
    static constexpr size_t BLOCK = 240;   // 5 ms @ 48 kHz

    // The dynamics, as TIMES. A one-pole coefficient is a time only at one sample rate,
    // and the output clock follows the source now: at 96 kHz the constants designed for
    // 48 gave a limiter that let go in an eighth of a second and a volume ramp half as
    // long. set_sample_rate() turns these into coefficients for the rate actually running.
    /// Volume ramp: two one-poles in cascade, each this long. One pole leaves with a corner —
    /// full slope at the first sample — and on a loud bass note a 1 ms corner was a click
    /// (−42 dBFS above the note); two poles leave and arrive with zero slope.
    static constexpr float VOL_TAU_S     = 0.005f;
    static constexpr float MUTE_TAU_S    = 0.010f;   ///< mute and unmute: a fade, not a cut
    /// The volume law (volume_db): 0.6 dB a step from the top down to VOL_KNEE_PCT, then steps
    /// that grow until 1 % is VOL_FLOOR_DB.
    static constexpr float VOL_KNEE_PCT  = 50.0f;
    static constexpr float VOL_FLOOR_DB  = -80.0f;
    // The brickwall: a look-ahead peak limiter whose output never exceeds CEILING.
    static constexpr int   LIM_M         = 9;        ///< knots of look-ahead, and of smoothing
    static constexpr float LIM_HOLD_S    = 0.040f;   ///< a peak's gain is held this long
    static constexpr float LIM_RELEASE_S = 0.25f;    ///< then released with this time constant
    // The bass limiter (set_bass_xo).
    static constexpr float BASS_LOOKAHEAD_S = 0.005f;
    static constexpr float BASS_HOLD_S      = 0.050f;
    static constexpr float BASS_RELEASE_S   = 0.200f;
    static constexpr float BASS_FLOOR       = 0.25119f;   ///< −12 dB: the most the bass gives up
    static constexpr float BASS_THR         = 0.94406f;   ///< −0.5 dB: leaves the brickwall idle
    static constexpr size_t BASS_LINE_FRAMES = 33 * 30;   ///< its look-ahead at 192 kHz
    /// How long an EQ change takes to arrive: the outgoing and the incoming design both run
    /// and their outputs are blended over this time. One block — what this used to be — is
    /// 1.25 ms at 192 kHz, short enough to hear as a tick on a bass note.
    static constexpr float FADE_S        = 0.020f;
    // Loudness (set_loudness): two RBJ shelves fitted to ISO 226:2003 by tools/eq/fit_loudness.py —
    // the boost at f, re 1 kHz, that keeps the balance heard at 85 phon when listening a = 10…50 dB
    // quieter, re 1 kHz, weighted from 40 Hz up. Gains are K·a; within 1.1 dB of the contours up to
    // a = 30 dB, 1.8 dB at 50.
    static constexpr float LOUD_LO_HZ = 148.6f, LOUD_LO_Q = 0.359f, LOUD_LO_K = 0.5605f;
    static constexpr float LOUD_HI_HZ = 10000.0f, LOUD_HI_Q = 0.710f, LOUD_HI_K = 0.2253f;
    /// 50 dB under the top is 35 phon, background music; the compensation stops growing there.
    static constexpr float LOUD_ATT_MAX_DB = 50.0f;
    static constexpr float LOUD_TAU_S = 0.050f;     ///< how fast the shelves follow the volume
    static constexpr float PORT_Q = 1.4f;           ///< the port lift's peak (set_port)
    static constexpr float PORT_SUB_RATIO = 0.8f;   ///< the subsonic under a port, × its frequency
    // Бас and Високі (set_tone): where the shelves turn, to be settled by ear on the owner's
    // speakers, and how far they go.
    static constexpr float TONE_LO_HZ = 100.0f, TONE_HI_HZ = 10000.0f, TONE_Q = 0.71f;
    static constexpr float TONE_MAX_DB = 6.0f;
    static constexpr float BAL_MAX_DB = 6.0f;       ///< set_balance_db()

    DspChain() { set_sample_rate(FS_HZ_DEFAULT); }

    void reset();
    /// The rate the blocks arrive at. Rebuilds dynamics and mono-bass coefficients whenever the
    /// output clock moves; reset() deliberately does not change it.
    void set_sample_rate(float hz);

    /// Redesign the bands. Nothing is cleared: each band whose design moved — or that switched
    /// on or off — crossfades from what it was to what it is over FADE_S, the outgoing design
    /// running on its own copy of the state. An edit that arrives while a band is fading waits
    /// for that fade to finish and then fades in turn; a later one replaces it (the last edit
    /// wins, at most one fade behind).
    void set_bands(const EqBand* bands, int count);
    /// Bypass the whole EQ. Crossfaded the same way, so the button does not click either.
    void set_eq_enabled(bool on);

    /// Stereo width as a mid/side scale: 0 = mono, 1 = as recorded, 2 = doubled side.
    /// Exact identity at 1 (the stage is skipped outright), so "off" costs nothing and
    /// cannot colour the signal. Above ~1.4 this boosts out-of-phase content, which is
    /// what makes a recording sound wide and also what makes it collapse in mono.
    void set_width(float w);
    /// Subsonic high-pass at `hz` (0 = off; 20…80 Hz otherwise), 4th-order Butterworth,
    /// ahead of the EQ. Below a ported speaker's tuning a cone has nothing holding it back and
    /// the amplifier delivers current that makes no sound — at 24 V that is current the supply
    /// may not have. Switching it or moving it crossfades like an EQ band.
    void set_subsonic(float hz);
    /// A vented speaker's port frequency `hz` (0 = off; 30…80 Hz) and how much to lift it,
    /// `lift_db` (0…6). The subsonic then sits at PORT_SUB_RATIO·hz whatever set_subsonic()
    /// asked, just under the port, where the cone stops being held and starts making current
    /// instead of sound; and a peak of lift_db at hz, Q PORT_Q, fills in the half-octave the box
    /// rolls off through. At hz the cone barely moves (the port does the work), which is why a
    /// lift there is cheap: on a modelled B4 box tuned to 50 Hz, +4 dB moved the −3 dB point from
    /// 58 to 48 Hz for 2.3 dB more peak excursion than the subsonic alone. It is part of the
    /// subsonic stage, crossfades as it does, and its boost counts in the headroom.
    void set_port(float hz, float lift_db);
    /// Loudness compensation, `strength` 0…1 (0 = off). Turned down, music loses its bass and its
    /// top before its middle — the ear's equal-loudness contours (ISO 226:2003) bunch up at both
    /// ends — so two shelves give back the difference between the contour at the listening level
    /// and the one at the top of the volume scale: for every dB the volume sits under the top,
    /// LOUD_LO_K dB of bass shelf and LOUD_HI_K dB of treble shelf, times `strength`. The shelves
    /// always lift less than the volume took away, so nothing reaches past what the top of the
    /// scale plays; and their gains glide (LOUD_TAU_S, the filters retuned across each block), so
    /// turning the knob does not tick. A wire, bit-exact, at the top of the scale and when off.
    void set_loudness(float strength);
    /// Where the loudness adds nothing — «Звичайна гучність»,
    /// % of the effective level, 20…100. At 100, the default, it is the top of the scale: the
    /// rule above, on its old code path bit for bit — the algebra is the same, the floats would
    /// not be. Under it, the shelves lift for every dB the volume sits under the reference and for
    /// nothing above it: a = level(ref) − level(vol), level(p) = min(volume_db(p), −B). So at
    /// the volume somebody usually listens at, their music plays as it is. NaN is 100.
    void set_loud_ref(float pct);
    float loud_ref() const { return loud_ref_; }
    /// The loudness bass shelf's gain right now, dB (≥ 0).
    float loudness_db() const { return loud_lo_db_; }
    /// Бас and Високі: a low shelf at TONE_LO_HZ and a
    /// high shelf at TONE_HI_HZ, each within ±TONE_MAX_DB, right after the loudness. They are
    /// two sections of their own — none of the ten EQ bands is ever taken — and they play
    /// whether the EQ is on or not. They count in B, a change crossfades as an EQ edit does,
    /// and at 0 dB a shelf is a wire, bit-exact.
    void set_tone(float bass_db, float treble_db);
    /// The balance (S3): `db` > 0 turns the LEFT channel down by that much, so the image moves
    /// right; < 0 the right channel; within ±BAL_MAX_DB. Only ever down, so it costs no
    /// headroom. After the stereo stage, before the bass limiter, gliding as the volume does;
    /// 0 is a wire, bit-exact.
    void set_balance_db(float db);
    /// Bass-first limiting below `hz` (0 = off; 60…250 Hz otherwise). An LR4 crossover splits
    /// the signal; when bass and the rest together would pass the ceiling, the BASS is turned
    /// down — up to 12 dB, 5 ms look-ahead, 50 ms hold, 200 ms release — and the mids are left
    /// alone; the brickwall after it catches whatever is left. Loud bass at 24 V is what puts
    /// the amplifier into protection, and a full-band limiter answering it pumps the vocal.
    /// While it is on, the EQ's headroom counts at each frequency only what this stage cannot
    /// take away there: down to its −12 dB floor where the LR4 low band carries everything, none
    /// of it where the high band does. A boost the stage can absorb does not lower the volume.
    /// A change goes through a 10 ms mute, since the split itself moves.
    void set_bass_xo(float hz);
    /// A long-term power ceiling for the bass, dB under a full-scale sine on both channels
    /// (0 = off, −12…−1): for a supply that sags on sustained bass. Off unless measured.
    void set_rms_cap_db(float db);
    /// Storage for the bass limiter's look-ahead, `frames` stereo frames (BASS_LINE_FRAMES is
    /// enough at every rate). Without it the bass limiter stays off. The module gives it PSRAM.
    void set_bass_line(float* buf, size_t frames);
    /// The smallest gain the bass limiter applied to the bass since the last call.
    float take_bass_min() { const float m = bass_min_; bass_min_ = bass_g_; return m; }
    /// High-pass the SIDE signal at `hz`, making everything below it mono. 0 = off.
    /// This is the honest half of "spatial" processing: it fixes the real problem (out-of-
    /// phase bass wasting excursion and blurring the image) instead of adding an effect.
    void set_mono_bass(float hz);
    /// «Вирівнювання гучності» (leveler.h): ahead of everything, the volume included. Off is a
    /// wire, bit-exact; switched off while it holds a gain, it glides back to 0 dB first.
    void set_leveler(bool on) { lev_.set_on(on); }
    /// What it does now, dB (> 0 lifts, < 0 cuts).
    float leveler_db() const { return lev_.gain_db(); }
    /// The entry gain, the first thing a block meets after the leveler: trim · volume · the EQ's
    /// headroom.
    ///
    /// Volume: 1 % = −80 dB to 100 % = drive (volume_db); 0 = silence. 100 % is the loudest
    /// CLEAN level — the source's full scale lands on the ceiling — and drive (0…6 dB) is how
    /// much further it may push into the limiters, for someone who prefers loud to clean.
    ///
    /// Headroom: a boosted EQ takes B dB off the TOP of the scale, not off every position.
    /// B is the loudest the EQ (plus trim and width) makes anything between 20 Hz and
    /// min(20 kHz, 0.49 fs); the volume stops at drive − B, so at 100 % the boosted band peaks
    /// exactly at the ceiling, and below that point switching the EQ on or off changes nothing
    /// but the bands themselves (the AOSP LVM approach). The alternative, a fixed preamp of −B,
    /// would make "EQ on" quieter at every volume and the A/B dishonest.
    ///
    /// `pct` may be fractional: «Максимум» compresses the scale rather than cutting it,
    /// so the level is pct × limit / 100 of the same law
    /// (LevelLatch::effective) — at an 85 % limit a step is 0.51 dB. The loudness follows it.
    void set_volume(float pct);
    void set_volume_percent(int pct) { set_volume((float)pct); }
    /// The volume law, dB under the top of the scale at `pct` (> 0): −60·(1 − pct/100) from
    /// VOL_KNEE_PCT up, 0.6 dB a step as it always was; under the knee a term that, with its
    /// slope, starts from zero there grows to VOL_FLOOR_DB at 1 %, about 1.4 dB a step at the
    /// bottom. The old floor, −59.4 dB at 1 %, was still too loud up close in a quiet room
    /// (owner, 14.10.2026); −59.4 is now about 17 %.
    static float volume_db(float pct);
    void set_mute(bool on)        { if (on != mute_) mute_ramp_ = true; mute_ = on; }
    /// A trim ahead of the EQ, in dB (eq.json's section gain), −24…+12. Part of the entry gain
    /// and of the headroom; 0 dB at 100 % volume with no EQ boost is a wire, bit-exact.
    void set_pre_gain_db(float db);
    /// How far 100 % volume may push past the clean ceiling into the limiters, 0…6 dB.
    void set_drive_db(float db);
    /// B, the headroom the EQ asks for now, dB ≥ 0 (see set_volume_percent).
    float headroom_db() const { return headroom_db_; }

    /// In-place processing of one block of interleaved stereo float (−1..1).
    ///
    /// A block in which anything stops being a number — a NaN or an infinity in the output or
    /// in a filter's state — comes back as silence, the chain is reset, and faults() counts it.
    /// The reason is the converter after this: RISC-V's fcvt.w.s turns a NaN into 2^31 − 1,
    /// which is full-scale DC into the amplifier for as long as the NaN keeps coming.
    void process(float* stereo, size_t frames);
    /// Blocks thrown away by that guard since start-up.
    uint32_t faults() const { return faults_; }
    /// The filter sections two to a pass (the default), or one, as the chain ran them until
    /// 15.10.2026. The output is the same bit for bit either way; the switch exists so the host
    /// tests can show exactly that. The firmware never turns it off.
    void set_paired_passes(bool on) { paired_ = on; }
    /// How many loops over the block the filter sections took in the last process(): a pair
    /// or a lone section is one, a band crossfading three (outgoing, incoming, the blend), the
    /// loudness gliding two.
    uint32_t filter_passes() const { return passes_; }

    // meters (peak of the last block, linear 0..1+)
    float peak_l() const { return peak_l_; }
    float peak_r() const { return peak_r_; }
    /// The brickwall's gain right now (1 = untouched).
    float limiter_gain() const { return lim_g_; }
    /// The smallest gain the brickwall applied since the last call — what a meter read every
    /// half second should show. The instantaneous value misses every short peak: the gain has
    /// let go again long before the meter looks.
    float take_limiter_min() { const float m = lim_min_; lim_min_ = lim_g_; return m; }
    /// What the look-aheads delay everything by, in frames at the current rate: the
    /// brickwall's, and the bass limiter's while it is on (or about to be).
    uint32_t latency_frames() const {
        const float xo = xo_switching_ ? pend_xo_ : bass_xo_hz_;
        return lim_L_ + (bass_line_ && xo_valid(xo) ? bass_L_ : 0u);
    }
    /// How many dB the volume stage is cutting right now, as a positive number to add back.
    /// Reads the RAMPED gain rather than the target, so a fade does not make a meter jump.
    /// Zero while muted or silent: nothing to show, and the logarithm has no answer there.
    float volume_cut_db() const;

private:
    /// One EQ band as it runs: the design in force and its state, plus — while it changes —
    /// the outgoing design on its own copy of the state, and at most one edit waiting.
    struct Band {
        SvfCoef  c;                 ///< the design in force (meaningless while `on` is false)
        SvfState st;
        bool     on = false;        ///< a filter, or a wire
        SvfCoef  old_c;             ///< during a fade: what it was
        SvfState old_st;
        bool     old_on = false;
        uint32_t pos = 0, len = 0;  ///< fade progress in frames; len 0 = not fading
        uint32_t fade_n = 0;        ///< how long this band's fades are (the subsonic's are longer)
        bool     pend = false;      ///< an edit waiting for the fade to finish
        bool     pend_on = false;
        SvfCoef  pend_c;
    };

    /// The filter sections of one block in chain order — the subsonic and the port lift, the
    /// loudness, Бас and Високі, the ten bands — run two to a pass (svf_stereo2): every pass
    /// over the block costs loads, stores and a loop on top of the arithmetic, and a pair pays
    /// them once. A steady section (running, not crossfading) waits here for the next one to
    /// share its pass; anything else — a band crossfading, the loudness gliding — first runs
    /// what is waiting, so the sections keep their order and the output does not change by a bit.
    struct Passes {
        float*         x;
        size_t         n;
        bool           paired;
        const SvfCoef* c = nullptr;   ///< a section waiting for a partner
        SvfState*      s = nullptr;
        uint32_t       count = 0;     ///< loops over the block so far (filter_passes())
        void add(const SvfCoef& coef, SvfState& st);
        void flush();
    };
    /// One band, or one subsonic or tone section, into the passes: queued while it is steady,
    /// run on its own while it crossfades, nothing while it is a wire.
    void run_section(Passes& p, Band& b);
    /// One band crossfading: the outgoing design and the incoming one, blended.
    void run_band(Band& b, float* stereo, size_t frames);
    /// The subsonic stage's three sections at the current rate: two Butterworth high-passes at
    /// sub_corner() and the port lift's peak. on[j]: section j is a filter.
    void design_subsonic(SvfDesign d[3], bool on[3]) const;
    /// The subsonic's corner: under the port while set_port() is on, else set_subsonic()'s.
    float sub_corner() const { return port_hz_ > 0.0f ? PORT_SUB_RATIO * port_hz_ : sub_hz_; }
    /// Stage the subsonic's designs, as set_bands() stages a band's.
    void stage_subsonic(bool fade);
    void run_stereo(float* stereo, size_t frames);
    /// Make `on`/`c` what `b` is heading for: now, fading over `len` frames; after the fade
    /// in progress; or not at all if that is where it is heading already.
    void stage(Band& b, bool on, const SvfCoef& c, uint32_t len);
    /// stage() for EQ band i, over an EQ edit's fade.
    void stage_band(int i, bool on, const SvfCoef& c) { stage(band_[i], on, c, fade_len_); }
    /// Design Бас and Високі at the current rate, and stage them (a fade) or adopt them now.
    void stage_tone(bool fade);
    /// The balance's two gains over one block (set_balance_db).
    void run_balance(float* stereo, size_t frames);
    static void begin_fade(Band& b, bool on, const SvfCoef& c, uint32_t len);
    /// Design every band again at the current rate and adopt it at once (no fade): the rate
    /// only moves while the envelope is shut, and reset() follows.
    void redesign_now();
    /// The entry gain: ramp to trim · volume · headroom (see set_volume_percent).
    void run_gain(float* stereo, size_t frames);
    /// The bass limiter over one block (set_bass_xo).
    void run_bass(float* stereo, size_t frames);
    /// One knot of the bass limiter, from the chunk's band peaks and energies.
    void bass_knot(float ph, float pl, float eh, float el);
    /// Adopt a crossover frequency now: design, clear, configure.
    void apply_xo(float hz);
    static bool xo_valid(float hz) { return hz >= 60.0f && hz <= 250.0f; }
    /// B again, after anything it depends on moved; then the gain target.
    void update_headroom();
    /// The gain target from volume, drive, trim and B.
    void update_gain();
    /// The loudest the bands in force make anything in [20 Hz, min(20 kHz, 0.49 fs)], dB.
    float eq_peak_db() const;
    /// The loudness shelves over one block (set_loudness): into the passes while they hold
    /// still, on their own while they glide.
    void run_loudness(Passes& p);
    /// Their targets from the volume and B; called with every gain target.
    void loud_retarget();
    /// One RBJ shelf as an SVF, in float: cheap enough to redesign every block while the volume
    /// moves. Bilinear, which is exact at 149 Hz at every rate; at 10 kHz it differs from the
    /// prototype by 0.6 dB at 16 kHz between 44.1 and 192 kHz, which a loudness top can carry.
    /// g0 = tan(π·f0/fs); g0 ≤ 0 gives a wire.
    static SvfCoef shelf_coef(float g0, float q, float gain_db, bool high);

    Leveler  lev_;                       ///< set_leveler()
    Band     band_[MAX_BANDS];
    Band     sub_[3];                    ///< subsonic: two Butterworth high-passes, the port lift
    float    sub_hz_ = 0.0f;             ///< what set_subsonic() asked for
    float    sub_fc_ = 0.0f;             ///< the corner in force (sub_corner())
    float    port_hz_ = 0.0f, port_lift_db_ = 0.0f;
    SvfDesign sub_d_[3];                 ///< their prototypes, for the headroom
    bool     sub_sec_[3] = {};           ///< which of them are filters
    bool     sub_on_ = false;            ///< any of them
    Band     tone_[2];                   ///< Бас, Високі (set_tone): sections of their own
    float    tone_db_[2] = {};           ///< what set_tone() was given, for a new rate
    SvfDesign tone_d_[2];                ///< their prototypes, for the headroom
    bool     tone_sec_[2] = {};          ///< which of them are filters
    EqBand   bands_[MAX_BANDS] = {};     ///< what set_bands() was given, for a new rate
    SvfCoef  want_c_[MAX_BANDS];         ///< the design of each band, EQ switch aside
    SvfDesign want_d_[MAX_BANDS];        ///< and its prototype, for the headroom
    bool     want_[MAX_BANDS] = {};      ///< enabled and designable
    int      band_count_ = 0;
    bool     eq_on_ = true;
    bool     paired_ = true;             ///< set_paired_passes()
    uint32_t passes_ = 0;                ///< filter_passes()
    uint32_t fade_len_ = 960;            ///< FADE_S in frames at the current rate
    float    fade_buf_[BLOCK * 2];       ///< the outgoing design's output, during a fade

    // stereo stage (mid/side)
    float    width_ = 1.0f;
    float    mono_bass_hz_ = 0.0f;
    float    sample_rate_hz_ = FS_HZ_DEFAULT;
    SvfCoef  side_c_;                    ///< 2nd-order Butterworth high-pass on S, mono bass on
    float    side_s1_ = 0.0f, side_s2_ = 0.0f;
    bool     side_hp_on_ = false;
    // balance (run_balance)
    float    bal_tl_ = 1.0f, bal_tr_ = 1.0f;   ///< the channel gains set_balance_db() asks for
    float    bal_l_ = 1.0f, bal_r_ = 1.0f;     ///< the gains applied, gliding to them

    // loudness (run_loudness)
    float    loud_s_ = 0.0f;                   ///< strength, 0…1
    float    loud_att_ = 0.0f;                 ///< dB under the top of the volume scale (or the reference)
    float    loud_ref_ = 100.0f;               ///< where the loudness adds nothing, % (set_loud_ref)
    float    loud_lo_db_ = 0.0f, loud_hi_db_ = 0.0f;   ///< the shelves' gains in force
    float    loud_lo_t_ = 0.0f, loud_hi_t_ = 0.0f;     ///< where they are gliding to
    float    loud_glo_ = 0.0f, loud_ghi_ = 0.0f;       ///< tan(π·f0/fs) of the two corners
    float    loud_k_ = 0.1f;                   ///< one block's step of the LOUD_TAU_S glide
    SvfCoef  loud_lo_c_, loud_hi_c_;           ///< the designs in force
    SvfState loud_lo_st_, loud_hi_st_;
    bool     loud_run_ = false;                ///< running, rather than a wire

    float  vol_pct_     = 0.0f;  // the effective level, % of the law's scale
    float  drive_db_    = 0.0f;
    float  trim_db_     = 0.0f;
    float  headroom_db_ = 0.0f;
    float  gain_target_ = 0.0f;  // linear: what the entry gain is heading for
    float  vol_g1_      = 0.0f;  // first pole of the ramp
    float  vol_gain_    = 0.0f;  // second pole: what is applied
    bool   mute_ = false;
    bool   mute_ramp_ = false;   // the ramp in progress is a mute or an unmute (MUTE_TAU_S)
    float  mute_k_ = 0.0104f;
    uint32_t faults_ = 0;
    float  vol_k_ = 0.02f;       // one-pole coefficients at the current rate: see set_sample_rate()

    // ── brickwall state (see process()) ──
    void     lim_reset();
    static constexpr uint32_t LIM_MAX_D = 30;                      ///< chunk at 192 kHz
    static constexpr uint32_t LIM_MAX_L = (LIM_M + 1) * LIM_MAX_D;  ///< look-ahead at 192 kHz
    float    lim_line_[LIM_MAX_L * 2];   ///< the delay line, interleaved stereo
    KnotEnv  lim_env_;
    uint32_t lim_D_ = 8;                 ///< frames per knot
    uint32_t lim_L_ = (LIM_M + 1) * 8;   ///< look-ahead, frames
    uint32_t lim_pos_ = 0;               ///< next slot of the delay line
    uint32_t lim_k_ = 0;                 ///< frames into the current chunk
    float    lim_cmax_ = 0.0f;           ///< peak of the chunk being read
    float    lim_pprev_ = 0.0f;          ///< peak of the chunk before it
    float    lim_g_ = 1.0f, lim_dg_ = 0.0f;
    float    lim_min_ = 1.0f;

    // ── bass limiter state (see run_bass()) ──
    float*   bass_line_ = nullptr;       ///< look-ahead, interleaved stereo, set_bass_line()
    size_t   bass_line_cap_ = 0;
    float    bass_xo_hz_ = 0.0f, pend_xo_ = 0.0f;
    bool     bass_on_ = false;
    bool     xo_switching_ = false;      ///< muting for a crossover change
    SvfCoef  bass_a_;                    ///< both LR4 sections at xo: A gives LP2 and AP2, again LP4
    float    bass_2k_ = 2.8284271f;      ///< the allpass mix: AP = x − 2k·BP
    float    det_s_[8] = {}, app_s_[8] = {};   ///< A and B states, L and R: detector, applied
    KnotEnv  bass_env_;
    uint32_t bass_L_ = 0, bass_pos_ = 0, bass_k_ = 0;
    float    bass_ph_ = 0.0f, bass_pl_ = 0.0f, bass_ph_prev_ = 0.0f, bass_pl_prev_ = 0.0f;
    float    bass_eh_ = 0.0f, bass_el_ = 0.0f, bass_pwh_ = 0.0f, bass_pwl_ = 0.0f, bass_kp_ = 0.0f;
    float    bass_cap_ = 0.0f;           ///< power cap, linear; 0 = off
    float    bass_g_ = 1.0f, bass_dg_ = 0.0f, bass_min_ = 1.0f;

    float  peak_l_ = 0.0f, peak_r_ = 0.0f;
};

/// Test-tone generator (sine, −12 dBFS), phase-continuous across frequency changes.
class ToneGen {
public:
    void set_freq(float hz);
    void fill(float* stereo, size_t frames);
    /// The same sine at amplitude `amp` on the channels in `mask` (1 left, 2 right, 3 both);
    /// the other channel silent. For the amplifier's calibration, where the block skips the
    /// chain and `amp` is a fraction of the DAC's own full scale.
    void fill_at(float* stereo, size_t frames, float amp, int mask);
    /// The step is a frequency over a sample rate, so it has to be worked out again when
    /// the output clock moves. Phase is kept: a tone that jumps is a click.
    void refresh() { set_freq(hz_); }
private:
    float phase_ = 0.0f;
    float hz_    = 1000.0f;
    float step_  = 1000.0f / FS_HZ_DEFAULT;
};

} // namespace audio_core
