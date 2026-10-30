/**
 * @file audio_module.h
 * @brief Audio core — source select, ASRC, DSP, I²S output; own task on core 1.
 *
 * Contract with ModESP (docs/CONCEPT.md, "The audio core on core 1"): the module owns
 * SharedState keys (audio.*) and pushes COMMANDS to the audio task through a FreeRTOS queue
 * from on_update(); the task returns only slow STATUS (meters, underruns, measured input
 * rate) through a spinlock-guarded POD. No sample ever crosses the message bus.
 *
 * Signal path (docs/CONCEPT.md, "Signal path and clocking"): source → [ASRC for BT] → PEQ →
 * volume → limiter → I2S-A TX.
 * I2S-A is full duplex in one clock domain, so AUX (ADC) needs no resampling; the BT
 * port is an I²S slave to the module's clock, so a reader task fills a PSRAM ring and
 * the audio task pulls through the resampler with a ratio steered by the ring level.
 */

#pragma once
#include "bt_clock_health.h"

#include "modesp/base_module.h"
#include "modesp/hal/audio_sink.h"
#include "state_enums.h"        // generated from modules/audio/manifest.json
#include "modesp/hal/driver_interfaces.h"
#include "dsp_chain.h"
#include "level_latch.h"
#include "eq_config.h"
#include "fade_gate.h"
#include "spectrum.h"
#include "startup_probe.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstdint>

namespace modesp { class DriverManager; struct BindingTable; class HAL; }
struct resample;   // dbry/audio-resampler context

class AudioModule : public modesp::BaseModule {
public:
    /// The numbers are the contract (docs/CONCEPT.md, "`audio.source` values"): the cards on
    /// the sources screen and the audio.sources bit mask are both indexed by them.
    ///
    /// Spelled from generated/state_enums.h rather than written out here, so that this enum,
    /// the manifest's `options` (which is where the web UI and the API clamp get their
    /// range) and the constants player/radio/dlna compare against are one list. They were
    /// four, and 8.1a had already caught the manifest saying 0..3 while the code used 0..5.
    enum Source : int32_t {
        SRC_OFF   = modesp::gen::AUDIO_SOURCE_OFF,
        SRC_TONE  = modesp::gen::AUDIO_SOURCE_TONE,
        SRC_AUX   = modesp::gen::AUDIO_SOURCE_AUX,
        SRC_BT    = modesp::gen::AUDIO_SOURCE_BT,
        SRC_FILE  = modesp::gen::AUDIO_SOURCE_FILE,
        SRC_RADIO = modesp::gen::AUDIO_SOURCE_RADIO,
        SRC_AIRPLAY = modesp::gen::AUDIO_SOURCE_AIRPLAY,
        SRC_USB   = modesp::gen::AUDIO_SOURCE_USB,
    };

    /// The sources that arrive through the PCM ring rather than through an I2S input: the
    /// card, the radio (and DLNA, which rides it), AirPlay and USB. Everything that treats "a
    /// producer is filling the ring" specially asks this — but a new ring source is not one
    /// line: it also needs ring_owner(), source_refused() and the read switch in run().
    static constexpr bool is_ring_source(int32_t s) {
        return s == SRC_FILE || s == SRC_RADIO || s == SRC_AIRPLAY || s == SRC_USB;
    }

    /// Ring sources whose producer paces itself by the ring's fill rather than by its input:
    /// AirPlay's timing engine and USB's asynchronous feedback. The consumer keeps reading
    /// them through the amplifier's settling, or the fill they steer by stops meaning anything.
    static constexpr bool paces_by_fill(int32_t s) {
        return s == SRC_AIRPLAY || s == SRC_USB;
    }

    AudioModule();

    void on_bind(modesp::DriverManager& drivers, const modesp::BindingTable& bindings,
                 modesp::HAL& hal) override;
    bool on_init() override;
    void on_update(uint32_t dt_ms) override;
    void on_stop() override;

    // ── task side (public for the static entry points) ──
    void task_loop();
    void bt_rx_loop();

private:
    // WIDTH and MONO_BASS carry a scaled integer (width x100, Hz) because Cmd is int32 —
    // the queue stays POD and lock-free rather than growing a union for two settings.
    enum class CmdType : uint8_t { SOURCE, MUTE, EQ_ENABLE, EQ_RELOAD, TONE_HZ,
                                   WIDTH, MONO_BASS, SPECTRUM, BT_CLOCK, AMP_CLIP, DRIVE,
                                   SUBSONIC, TONE_MODE, TONE_DB10, TONE_CH, BASS_XO, RMS_CAP,
                                   LOUDNESS, PORT, TIMBRE, BALANCE, LOUD_REF, LEVELER, STOP };
    /// audio.tone_mode: what the tone source plays.
    enum ToneMode : int32_t {
        TONE_CHAIN  = 0,   ///< through the whole chain, −12 dB under the ceiling (as ever)
        TONE_RAW    = 1,   ///< past the chain at an exact DAC level: the amplifier's calibration
        TONE_BURSTS = 2,   ///< the same, 250 ms on and 750 ms off, 5 ms edges
    };
    struct Cmd { CmdType type; int32_t value; };

    struct Status {
        float    peak_l = 0, peak_r = 0;      ///< output, after the DSP chain
        float    in_l = 0, in_r = 0;          ///< source, before the DSP chain (AUX/BT debugging)
        float    in_diff = 0;                 ///< peak |L-R|: near zero means the two channels are the same signal
        float    in_rate_hz = 0;
        float    asrc_ppm = 0;
        float    cpu_pct = 0;
        float    lim_gr_db = 0;             ///< limiter gain reduction, dB (0 = none)
        uint32_t underruns = 0;
        uint32_t dsp_faults = 0;            ///< blocks the DSP chain replaced by silence (NaN/Inf)
        float    eq_headroom_db = 0;        ///< B: what the EQ takes off the top of the volume scale
        float    bass_gr_db = 0;            ///< the bass limiter's deepest cut in the window, dB
        float    loud_db = 0;               ///< the loudness bass shelf's gain now, dB
        float    lev_db = 0;                ///< «Вирівнювання гучності»: its gain now, dB
        float    dsp_pct = 0;               ///< DSP cycles over the window, % of one core
        float    dsp_max_pct = 0;           ///< the window's worst block, % of a block's time
        uint32_t out_clamps = 0;            ///< samples the output conversion had to clamp, total
        int32_t  source = SRC_OFF;
        bool     running = false;
        // Measurements the audio task takes but must NOT report itself. ESP_LOG writes to
        // the UART and blocks: a hundred characters at 115200 baud is nine milliseconds,
        // and a block of audio is five. Logging from this task starves the TX DMA and makes
        // exactly the click the numbers are being gathered to find — the instruments were
        // producing the fault they measured. They travel here and the main loop prints them.
        uint32_t late_us = 0;               ///< longest gap between blocks; 5000 is on time
        uint32_t out_rate = 0;              ///< the output clock, for judging late_us against the DMA
        uint32_t tx_short = 0;              ///< blocks I2S would not take in full
        bool     amp_on = false;
        uint32_t edge_seq = 0, edge_rate = 0, edge_blocks = 0;
        float edge_step = 0, edge_boundary = 0, edge_closed = 0;
        uint32_t edge_openings = 0;
        uint32_t startup_seq = 0, startup_rate = 0;
        audio_core::StartupProbe startup_raw{}, startup_out{};
        // The output clock as a servo (stage 13). Reported rather than logged, like
        // everything else the audio task measures.
        int32_t  clock_ppm    = 0;      ///< what the output clock is trimmed to, achieved
        bool     clock_follow = false;  ///< the BT path is riding the clock, not the resampler
        uint32_t bt_asrc_blocks = 0;    ///< blocks that still went through the resampler
        bool bt_active = false;
        uint32_t bt_primes = 0, bt_format_revision = 0;
    };

    bool load_eq_file(audio_core::EqBand* bands, int& count);
    /// Section 0 of `cfg` as the chain takes it: its bands, the trim, the output mode.
    void take_eq(const audio_core::EqConfig& cfg, audio_core::EqBand* bands, int& count);
    /// Main task: hand an edited EQ on and write it back when that is not in the way.
    void own_eq();
    /// audio.port_hz and audio.port_lift_db as one command value: hz·16 + lift (Cmd is int32).
    /// No port at all for closed speakers (audio.spk_type), whatever number is kept.
    int32_t read_port();
    /// «Звичайна гучність» (audio.loud_ref), the loudness's reference: held to 20…100 %.
    int32_t loud_ref();
    /// audio.bass_db and audio.treble_db as one command value: bass in the high byte, treble in the
    /// low one, each a signed byte within ±6 (DspChain::set_tone).
    int32_t timbre();
    /// audio.vol_max, «Максимум»: 40…100 % of the scale (S4).
    int32_t vol_max();
    void send(CmdType t, int32_t v);
    /// Switch to `src` now. Audio task only, and only once both gates are shut: the task loop
    /// holds a requested source in pend_source_ until they are.
    void apply_source(int32_t src);
    /// A source this board cannot play, said once in the log. Checked before the fade starts,
    /// so a refused source does not silence the one that is playing.
    bool source_refused(int32_t src);
    /// Everything the task counts in frames but means in milliseconds, at out_rate_.
    void retime();
    /// Bring the last frame a source delivered down to silence along the short tail, for a block
    /// that arrived with nothing in it. Fills one block of `stereo`; returns the frames that carry
    /// the tail.
    size_t fade_from_hold(float* stereo, float l, float r);
    bool pull_bt_block(float* stereo, size_t frames);
    bool stop_bt_diagnostic(uint8_t reason);
    /// Take on a format the satellite announced — the input port AND the output clock, in the
    /// order that makes both possible. false ⇒ nothing is playable, so play nothing.
    bool adopt_bt_format(uint32_t hz, uint8_t bits);
    /// Move the input port onto the crystal, keeping its slot width so IDF does not free and
    /// reallocate its DMA descriptors. Leaves the port stopped. See the .cpp for why width
    /// and memory are the same question on this chip.
    bool park_bt_off_pll();
    /// Protocol 1.9: take on a format with THIS panel clocking the link — the output
    /// retuned to the stream's rate and the main port's clocks fanned out to the satellite,
    /// its data read back on the same clock. Needs 32-bit slots. false ⇒ play nothing.
    bool adopt_bt_format_master(uint32_t hz, uint8_t bits);
    /// Hand the link's clock lines back to the satellite side as inputs.
    void release_link();
    /// Where the BT reader takes its frames from: the main port while we clock the link,
    /// the slave input port otherwise.
    modesp::audio::IAudioPort* bt_rx_port() const { return link_master_.load() ? main_ : bt_; }
    /// Put the output clock back exactly on its nominal rate. Cheap and idempotent; called
    /// whenever the servo stops being responsible for it.
    void reset_out_trim();
    /// One servo step: ring error in frames -> a trim of the output clock. Audio task only.
    void steer_out_clock(int32_t err_frames, uint32_t stream_hz);
    /// Sets the enable line. Deliberately silent: it runs in the audio task.
    void set_amp(bool on);
    /// Tell the ring what sits between it and the pins (pcm_ring.h, set_consumer). Again at
    /// every new rate: the limiter's look-ahead is a number of frames.
    void publish_consumer();
    /// The amplifier's clean ceiling C (audio.amp_clip_db10, tenths of dBFS) → the limiter's
    /// threshold T = min(C − 0.5, −1) dBFS → the output scale k_out·2^31. Audio task.
    void set_amp_clip(int32_t db10);
    /// Gate a raw calibration tone into 250 ms bursts, one a second. Audio task.
    void shape_burst(float* stereo, size_t frames);
    /// The DAC scale of the chain's 1.0: the amplifier's ceiling, not the DAC's full scale.
    float    out_scale_ = 0.0f;
    /// Move the output clock to `hz` and rebuild everything that was designed against the
    /// old one. Audio task only, and only while both gates are shut. A rate the port
    /// refuses gets one try at its family's fallback; after that the clock stays put and
    /// the producer is told which rate to convert to.
    void retune(uint32_t hz, uint16_t mclk_mult = 0);
    /// The second half of retune(): the port has taken `hz`, so record it, answer the
    /// producer and redesign what depends on the rate.
    void adopt_clock(uint32_t hz, uint16_t mclk_mult);
    /// Silence held after the amplifier is enabled, before any audio is let through.
    ///
    /// Switching the enable line is a pop, in the amplifier, downstream of everything this
    /// code can see — which is why it survived a step detector, an I2S shortfall counter and
    /// an underrun counter all reporting clean. It used to fire on the arrival of SIGNAL, so
    /// the pop landed exactly on the first note of a track and on the first sound after boot.
    ///
    /// The cure is sequencing, not filtering: enable the amplifier into silence, give the DAC
    /// output time to settle, and only then open the envelope. 300 ms at startup, because the
    /// converter is also finding its clock; 100 ms afterwards.
    ///
    /// Timed on the clock, not in blocks. A block is 5 ms at 48 kHz but 1.25 ms at 192, and the
    /// first blocks after a start go straight into an empty DMA ring, so a count of blocks was
    /// never the time it claimed to be.
    static constexpr int64_t AMP_SETTLE_US      = 100000;
    static constexpr int64_t AMP_SETTLE_BOOT_US = 300000;
    int64_t amp_settle_until_us_ = 0;       ///< esp_timer time the settle ends; armed at task start
    // Click-free edges. A source that starts or stops mid-waveform is a STEP in the signal,
    // and a step is a click — nothing to do with buffering or with the amplifier relay.
    // Ramping in takes care of the start; ramping the LAST sample down to silence takes
    // care of the stop, which is the half that cannot be seen coming.
    /// The envelope's steps per frame: it opens in FadeGate::OPEN_S (20 ms) and closes in
    /// FadeGate::CLOSE_S (100 ms) at every rate, along smootherstep, exactly as the output gate
    /// does; fade_gate.h says why the lengths and the curve.
    float sig_up_ = 1.0f / 960.0f, sig_down_ = 1.0f / 4800.0f;
    /// The tail a partial or empty block is faded down along: 2 ms, at most one block.
    static constexpr float SIG_FADE_OUT_S = 0.002f;
    size_t sig_fade_out_ = 96;
    float sig_gain_ = 0.0f;
    bool gate_closing_ = false;             ///< paused or flushing: let the envelope close
    /// The second gate: after the DSP chain, in front of the DAC (fade_gate.h). It heads where the
    /// envelope heads, on a clock of its own, so in every ordinary close the two move together,
    /// and when the envelope is cut short by an empty block this one still closes over the tail.
    audio_core::FadeGate out_gate_;
    /// The chain has run since it was last reset, so its filters may still be ringing.
    bool dsp_dirty_ = false;
    /// A source that was asked for and is waiting for both gates to shut. -1: none.
    int32_t pend_source_ = -1;
    /// Frames sent to the DAC since the output gate last let anything through. The amplifier
    /// goes off only once this covers the whole TX queue. Until then the DMA is still playing
    /// the fade (review A03).
    uint32_t quiet_frames_ = 0;
    uint32_t tx_frames_ = 0;                ///< the port's TX queue, frames
    /// Audio the ring must hold before playback resumes after the queue is dropped.
    ///
    /// At the instant of a flush the producer has written nothing, so reading straight away
    /// guarantees an underrun — and in the middle of a steady tone an underrun is heard as a
    /// tick even when the samples either side of it are perfectly continuous, because the
    /// gap itself is the artefact. 100 ms is longer than the gap and far shorter than
    /// opening the next file. A time at every rate, like the fades (review A06).
    static constexpr float PRIME_S = 0.100f;
    size_t prime_frames_ = 4800;
    bool priming_ = true;
    uint32_t ring_generation_ = 0;
    uint32_t tx_short_ = 0;                 ///< blocks I2S would not take in full
    // Step detector. A click IS a discontinuity, so its size is measurable — and measuring it
    // right where the samples leave for the DAC answers the one question guessing cannot:
    // whether the impulse is in what this code produces, or in something after it.
    float last_out_l_ = 0.0f, last_out_r_ = 0.0f;
    /// The last frame a ring source delivered, before the envelope and the DSP: what an
    /// empty block mid-waveform is faded down from, since it has nothing of its own to fade.
    float last_in_l_ = 0.0f, last_in_r_ = 0.0f;
    int64_t last_block_us_ = 0;             ///< when the previous block was handed to I2S
    uint32_t late_us_ = 0;                  ///< worst gap this window, audio task side
    bool     last_amp_on_ = false;          ///< main-loop side, for reporting the change
    uint32_t last_tx_short_ = 0;
    uint32_t last_tx_ovf_ = 0;              ///< TX DMA periods sent as zeros, as last reported
    uint32_t last_dsp_faults_ = 0;
    uint32_t last_out_clamps_ = 0;
    int32_t  last_amp_clip_ = INT32_MIN;
    int32_t  last_drive_ = INT32_MIN;
    int32_t  last_hp_hz_ = INT32_MIN;
    int32_t  last_bass_xo_ = INT32_MIN, last_rms_cap_ = INT32_MIN;
    int32_t  last_loud_ = INT32_MIN, last_port_ = INT32_MIN;
    /// «Звичайна гучність» (S6): the reference the audio task has (LOUD_REF).
    int32_t  last_loud_ref_ = INT32_MIN;
    int32_t  last_timbre_ = INT32_MIN, last_balance_ = INT32_MIN;   ///< Бас and Високі packed (timbre()), balance
    float    last_loud_db_ = -1.0f;
    // «Вирівнювання гучності» (audio.leveler) and its schedule (audio.lev_sched/lev_from/lev_to)
    int32_t  last_leveler_ = INT32_MIN;
    float    last_lev_db_ = 99.0f;
    audio_core::LevelerSchedule lev_sched_;
    uint32_t lev_sched_ms_ = 0;
    /// Once a second: the schedule against the clock, and the switch to what it says at its edges.
    void leveler_schedule(uint32_t dt_ms);
    // ── port meter (audio.port_meas): the onboard microphone at a woofer finds the port ──
    struct i2c_master_bus_t* mic_bus_ = nullptr;   ///< the microphone codec's I2C bus, from on_bind
    std::atomic<bool> meter_busy_{false};
    static void meter_entry(void* self);
    /// The whole measurement, in a task of its own; the .cpp says how it goes.
    void port_meter_loop();
    /// Say what the bass settings are, in the log: the panel's API wants a password, its
    /// console does not, and "why does it sound like that" starts here.
    void log_bass(const char* when);
    // The DSP's load, summed over half-second reports and logged every DSP_LOG_MS of playing.
    static constexpr uint32_t DSP_LOG_MS = 30000;
    uint32_t dsp_log_ms_ = 0, dsp_log_n_ = 0;
    float    dsp_log_sum_ = 0.0f, dsp_log_max_ = 0.0f;
    float*   bass_line_ = nullptr;          ///< the bass limiter's look-ahead, PSRAM
    // Calibration tone. The task side, set through the queue:
    int32_t  tone_mode_ = TONE_CHAIN;
    int32_t  pend_tone_mode_ = TONE_CHAIN;
    bool     tone_switching_ = false;        ///< the gate shuts, the mode changes, it opens again
    float    tone_amp_ = 0.0501f;           ///< linear, of the DAC's full scale (raw modes): −26 dBFS
    float    tone_amp_cur_ = 0.0501f;       ///< what the last block played: level changes ramp
    int32_t  tone_mask_ = 3;
    uint32_t burst_pos_ = 0;                ///< frames into the burst's one-second cycle
    // and the main-loop side, which also switches a forgotten raw tone off.
    int32_t  last_tone_mode_ = INT32_MIN, last_tone_db10_ = INT32_MIN, last_tone_ch_ = INT32_MIN;
    uint32_t tone_raw_ms_ = 0;
    uint32_t tone_idle_ms_ = 0;             ///< a tone through the chain, unchanged for this long
    float    last_headroom_ = -1.0f;
    uint32_t last_edge_seq_ = 0;
    uint32_t last_startup_seq_ = 0;

    // backends
    modesp::audio::IAudioPort*  main_ = nullptr;   // I2S-A: TX (DAC) + RX (ADC)
    modesp::audio::IAudioPort*  bt_   = nullptr;   // I2S-B: RX slave (BT module)
    modesp::audio::LinkPins     link_pins_{};      // I2S-B's pins, for clocking the link from I2S-A
    std::atomic<bool>           link_master_{false}; // we clock the link (protocol 1.9)
    bool                        bt_fmt_master_ = false; // the format was adopted with link_master_
    modesp::IActuatorDriver*    amp_  = nullptr;   // amp enable / DAC XSMT

    // task state (audio task only)
    audio_core::DspChain dsp_;
    audio_core::ToneGen  tone_;
    int32_t  source_ = SRC_OFF;
    bool     muted_  = false;
    bool     amp_on_ = false;
    /// The rate the output is clocked at, and the one a producer has asked for.
    ///
    /// They differ for as long as it takes the envelope to close: retuning I2S drains the
    /// DMA and restarts the clock, which is a step in the middle of whatever was playing,
    /// so it waits for silence exactly as a source switch does.
    uint32_t out_rate_ = 48000;
    /// The MCLK multiple the output is running at. Not cosmetic: it decides where the audio
    /// PLL sits, and the PLL is what a wide-slot Bluetooth stream has to fit inside.
    uint16_t out_mclk_mult_ = 0;
    uint32_t want_rate_ = 0;                ///< 0 = nothing pending
    uint32_t refused_rate_ = 0;             ///< logged once, not once a block
    /// The highest rate the output clock is moved to (the customer, 15.10.2026). A 176.4 or
    /// 192 kHz track plays at 88.2 or 96 kHz, halved by its own producer, as a rate the port
    /// refused would be. What 192 kHz adds over 96 lies above 48 kHz, where the amplifier's
    /// output filter and the speakers stop; the DSP at 192 kHz costs twice what it does at 96.
    static constexpr uint32_t PLAY_MAX_HZ = 96000;
    /// The clock this listening SESSION runs at; 0 until the first track of it has settled
    /// one. See the FILE/RADIO branch of the audio task: the clock is chosen once, when a
    /// source starts playing, and every later track converts to it.
    uint32_t session_rate_ = 0;
    resample* asrc_ = nullptr;
    double   asrc_ratio_ = 1.0;
    float    in_rate_est_ = 48000.0f;
    uint32_t bt_short_   = 0;   ///< blocks the converter could not fill
    uint32_t bt_primes_  = 0;   ///< times the ring had to pre-buffer from empty
    uint32_t bt_resyncs_ = 0;   ///< times the tail was jumped because the ring pinned full
    uint32_t bt_seeded_hz_ = 0; ///< the announcement the estimator was last re-seeded from
    double   asrc_base_  = 0.0; ///< the ratio the servo steers around, for the ppm readout
    /// Frames thrown away because the ring had no room. Written by the reader task, read by
    /// the module task; it only ever grows, so a torn read costs nothing but a stale total.
    volatile uint32_t bt_dropped_ = 0;
    uint32_t bt_log_ms_ = 0;  ///< throttles the BT signal report
    uint32_t bt_fmt_revision_ = 0;
    std::atomic<bool> bt_capture_{false}, bt_reader_paused_{true};
    float bt_last_l_ = 0, bt_last_r_ = 0; // pre-envelope samples for a depleted fade tail
    void pause_bt_reader();
    /// The format the BT port is actually configured for, so an announcement is adopted once
    /// and a refusal is not retried every block.
    uint32_t bt_fmt_hz_   = 0;
    uint8_t  bt_fmt_bits_ = 0;
    /// Whether that adoption WORKED. A refused format used to be recorded the same way an
    /// adopted one was, so the next block went straight on to play a stream the port was not
    /// configured for — 32-bit slots read as 16, which is noise, not quiet.
    bool     bt_fmt_ok_ = false;
    bool     bt_unannounced_ = false;  ///< said once, so the warning is not a stream itself
    bool     bt_refused_     = false;  ///< ditto for a format this board cannot clock
    bool     bt_primed_ = false;

    // Bench modes latch silent after the first local loss or 60 seconds. A new
    // source/format re-arms them; ordinary builds never use these fields.
    bool bt_diag_stopped_ = false;
    int64_t bt_diag_start_us_ = 0;
    uint32_t bt_diag_dropped_ = 0;
    modesp::audio::AudioPortDiagnostics bt_diag_rx_{}, bt_diag_tx_{};

    // ── the output clock as the Bluetooth servo (stage 13) ──
    //
    // The satellite owns the input clock: it is the I2S master of that port and this panel
    // its slave, so the samples arrive at the satellite's rate whatever the P4 believes. The
    // few parts per million between the two crystals have to go somewhere. Today they go
    // into the resampler — 192 float multiply-accumulates per output frame, about a quarter
    // of core 1, and never bit-exact. They can go into the OUTPUT CLOCK instead, which costs
    // one register write every quarter second and leaves the samples untouched.
    //
    // Which of the two is in use is decided ONCE, while the ring is pre-buffering and the
    // output is silent, and never changed while music is playing: the resampler's 48-tap
    // filter delays the signal by half its length and the direct path does not, so switching
    // between them mid-stream would skip about twenty-four samples — a step, which is a click.
    bool     bt_clock_follow_ = false;  ///< this stream is riding the clock, not the resampler
    bool     bt_trim_dead_    = false;  ///< the port refused a trim; resampler from the next prime
    bool     bt_clock_unhealthy_ = false;
    bool     bt_clock_recover_ = false; ///< fade to silence before changing the path
    audio_core::BtClockHealth bt_clock_health_;
    /// audio.bt_clock_follow: whether riding the clock is allowed at all. Set from the
    /// command queue, read at the prime; a change takes effect at the next prime, never
    /// mid-stream (the decision in pull_bt_block says why, and carries the bench history).
    bool     bt_clock_allowed_ = true;
    /// Which path the last prime chose and why, for on_update to say once (this task does
    /// not log): 0 undecided, 1 clock, 2 setting off, 3 rate differs, 4 trim refused earlier,
    /// 5 the port cannot trim. One byte, so a torn read is impossible.
    volatile uint8_t bt_path_ = 0;
    int32_t  bt_trim_ppm_     = 0;      ///< what the clock was last asked for
    int32_t  bt_trim_got_     = 0;      ///< what the port says it actually achieved
    uint32_t bt_asrc_blocks_  = 0;      ///< blocks that went through the resampler after all
    int32_t  bt_err_sum_      = 0;      ///< ring error summed over the servo window
    uint16_t bt_err_n_        = 0;      ///< blocks in that window
    uint32_t bt_servo_frames_ = 0;     ///< period in stream time, independent of block count

    // BT ring (PSRAM, float stereo frames): producer = bt_rx_loop, consumer = task_loop
    float*   bt_ring_ = nullptr;
    size_t   bt_ring_frames_ = 0;
    std::atomic<size_t> bt_head_{0}, bt_tail_{0};
    volatile uint32_t bt_frames_total_ = 0;
    std::atomic<uint32_t> bt_rx_gap_us_{0};
    std::atomic<uint32_t> bt_fill_low_{UINT32_MAX}, bt_fill_high_{0};

    // control / status plumbing
    QueueHandle_t cmd_q_ = nullptr;
    TaskHandle_t  task_ = nullptr;
    TaskHandle_t  bt_task_ = nullptr;
    volatile bool run_ = false;
    Status        status_{};
    portMUX_TYPE  mux_ = portMUX_INITIALIZER_UNLOCKED;

    // main-loop side change detection
    int32_t  last_source_ = -1, last_volume_ = -1, last_tone_ = -1;
    int32_t  last_vol_max_ = -1;
    /// The volume and «Максимум» to the audio task: the last value, never the command queue,
    /// which drops what does not fit (S4). audio.vol_eff says when it has been taken.
    audio_core::LevelLatch level_;
    bool     vol_published_ = false;
    /// A persisted ring source that was not on offer at init because the ring's owner had
    /// not initialised yet; settled in the report loop once the ring exists. −1 = none.
    int32_t  pending_source_ = -1;
    bool     last_mute_ = false, last_eq_ = true;
    bool     last_bt_clock_ = true;    ///< audio.bt_clock_follow as last sent to the task
    uint8_t  last_bt_path_  = 0;       ///< the last BT path decision reported (bt_path_)
    uint32_t report_ms_ = 0;
    modesp::audio::AudioPortDiagnostics last_bt_rx_diag_{}, last_bt_tx_diag_{};
    uint32_t last_bt_diag_prime_ = 0, last_bt_diag_format_ = 0;
    bool last_bt_diag_active_ = false;
    uint32_t last_underruns_ = 0;
    char     last_state_[16] = "";
    int32_t  last_width_ = -1;
    int32_t  last_mono_bass_ = -1;
    int      last_spectrum_ = -1;      ///< what the UI was last told to want; -1 = not yet asked
    /// Audio task: whether a screen with a spectrum row is showing, and whether the analyser
    /// published anything last block (so the row can be sent to the floor exactly once).
    bool     spectrum_wanted_ = false;
    bool     spectrum_live_   = false;
    /// Output mode from eq.json (docs/CONCEPT.md, "Outputs"). Only "2.0" is implemented; the
    /// value is published as audio.out_mode so the UI and MQTT can see what the file asks for.
    char     out_mode_[8] = "2.0";
    /// The section's gain from eq.json, applied ahead of the EQ. See DspChain::set_pre_gain_db.
    float    eq_trim_db_ = 0.0f;
    /// The EQ store's revision last handed to the audio core (own_eq).
    uint32_t eq_heard_rev_ = 0;
    /// ~12 KB of working buffers, fed from the audio task and published to the UI
    /// through the lock-free bus in components/audio_dsp.
    audio_core::SpectrumAnalyzer spectrum_;
};
