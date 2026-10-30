/**
 * @file audio_module.cpp
 * @brief Audio core — glue between SharedState, the audio task and the audio seam.
 */

#include "audio_module.h"
#include "user_store.h"
#include "sat_link/stream_control.h"
#include "eq_config.h"
#include "eq_store.h"
#include "out_caps.h"
#include "eq_web.h"
#include "spectrum.h"
#include "i2s_clock.h"
#include "pcm_ring.h"
#include "mic_capture.h"
#include "mic_input.h"
#include "port_finder.h"
#include "modesp/hal/driver_registry.h"
#include "modesp/hal/driver_manager.h"
#include "modesp/hal/hal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_cpu.h"
#include "esp_http_server.h"
#include "modesp/net/http_routes.h"
#include "modesp/liveness.h"
#include "resampler.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>

#ifndef MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT
#define MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT 0
#endif
#ifndef MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO
#define MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO 0
#endif
#ifndef MODESP_AUDIO_BT_DIRECT_CONTINUOUS
#define MODESP_AUDIO_BT_DIRECT_CONTINUOUS 0
#endif
#if (MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT + MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO + MODESP_AUDIO_BT_DIRECT_CONTINUOUS) > 1
#error "Select only one Bluetooth PCM override"
#endif

/// Between a catalog message and its arguments: "@cal.m_done" MSG_ARG "hz=63" (U+001F; the same as
/// i18n.h's I18N_MSG_ARG, which this module does not include).
#define MSG_ARG "\x1f"

static const char* TAG = "Audio";

// Provided by modules/airplay when it is part of the product, absent otherwise. A weak
// reference, so this module can offer the source without knowing the receiver exists: the
// definition sits in the same object as the module class, which main links whenever
// project.json names the module, and nowhere else. It is declared here, at file scope,
// because inside the unnamed namespace below it would have internal linkage and never
// meet that definition.
extern "C" bool airplay_offered(void) __attribute__((weak));
// The same for USB audio: defined in modules/usb_audio, true once the device has started.
extern "C" bool usb_audio_offered(void) __attribute__((weak));

namespace {

using audio_core::DspChain;
constexpr size_t   BLOCK        = DspChain::BLOCK;         // 240 frames = 5 ms
constexpr uint32_t IO_TIMEOUT   = 200;                     // ms, > DMA ring
constexpr size_t   BT_RING_SEC  = 1;                       // ring capacity, seconds @ 48 kHz
/// AUX needs the ADC, not only the main port's data-in, which the Bluetooth link uses too
/// (modules/audio/Kconfig, AUDIO_LINE_IN_FITTED).
#if CONFIG_AUDIO_LINE_IN_FITTED
constexpr bool LINE_IN_FITTED = true;
#else
constexpr bool LINE_IN_FITTED = false;
#endif
constexpr bool BT_DIAGNOSTIC = MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT ||
                               MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO;
enum : uint8_t {
    BT_DIAG_DIRECT = 7, BT_DIAG_FIXED_RATIO, BT_DIAG_BAD_RATE, BT_DIAG_UNDERRUN,
    BT_DIAG_RESYNC, BT_DIAG_DROP, BT_DIAG_RX_LOSS, BT_DIAG_TX_LOSS,
    BT_DIAG_COMPLETE, BT_DIAG_SHORT, BT_DIAG_TRIM,
    BT_CONTINUOUS_DIRECT, BT_CONTINUOUS_BAD_RATE
};
// The set point, in milliseconds of the stream's OWN rate, not as a fraction of the ring.
//
// Half of a one-second ring — the old target — was 500 ms of queue at 48 kHz and 544 at
// 44.1, and every one of those milliseconds is latency between the phone and the speaker.
// The queue exists to give the servo room: it absorbs the difference between two crystals
// while the ratio catches up, and the satellite measured that difference at 4–22 ppm. At
// 22 ppm a queue drifts a millisecond every 45 seconds, and +-500 ppm of authority takes it
// back twenty times faster than it moves. Tens of milliseconds would do; 120 leaves room
// for a phone that stalls its stream for a moment, which phones do. The ring itself is
// allocated once, for a second at 48 kHz, and is simply longer or shorter in time at the
// other rates: nothing is reallocated from the audio task.
constexpr uint32_t BT_TARGET_MS  = 120;                    ///< queue the servo steers to
constexpr uint32_t BT_TRACK_MS   = 40;                     ///< inside this of the set point: tracking authority only
constexpr uint32_t BT_CAPTURE_MS = 100;                    ///< at and beyond this: full capture authority
constexpr uint32_t BT_RESYNC_MS  = 500;                    ///< past this the tail is repositioned once
constexpr int      ASRC_TAPS    = 48, ASRC_FILTERS = 48;   // dbry preset -1 (~16 KB stereo)
// Of the source's Nyquist. 0.92 was a guard from the fixed-48 kHz days, when a 44.1 kHz
// stream was converted UP by 1.088 and the filter had to stop well under the source's
// Nyquist to keep its transition band away from the image. The output clock follows the
// stream now, so the ratio is 1.0 +-500 ppm, and a 48-tap 4-term Blackman-Harris transition
// is already a sixth of the band wide: at 0.92 the corner sat at 20.3 kHz for a 44.1 kHz
// stream and cost about 2 dB at 20 kHz; at 0.98 it sits at 21.6 kHz and the top octave is
// flat. Nothing else in the converter moves — 256 phases wait for the customer's WIP.
constexpr double   ASRC_LOWPASS = 0.98;
// The servo exists to absorb the difference between two crystals, which is tens of ppm —
// the satellite measured its own APLL at +40 ppm. It was given +-1 %, fifty times wider than
// the job, and a servo with authority it does not need spends it: every error in the rate it
// steers against becomes pitch modulation instead of being rejected. +-1 % is 17 cents of
// wow, and that is audible on anything with a sustained note in it.
//
// +-500 ppm is still ten times the expected drift. At 44.1 kHz it is 22 frames a second, and
// half a one-second ring takes a quarter of an hour to drain at that rate.
constexpr double   ASRC_MAX_DEV = 0.0005;                  // +-500 ppm while tracking
// Tracking authority is not capture authority. A ring that starts far from its set point —
// and it always does, because the converter runs on a guessed rate until STREAM_FMT arrives
// about a second and a half in — has to be brought back, and +-500 ppm moves 22 frames a
// second: eighteen minutes to drain half a ring. Meanwhile the ring sits full and throws
// away everything that will not fit, which is a click per drop.
//
// So: wide while far, narrow once near. And past the point where slewing could ever catch
// up, stop pretending and reposition the tail once — one discontinuity instead of thousands.
constexpr double   ASRC_CAPTURE = 0.02;                    // +-2 % while recovering
// How often the OUTPUT CLOCK servo takes a step, in blocks of 5 ms.
//
// There are two of these loops in the chain now — the satellite catches the phone, this
// panel catches the satellite — and a cascade is only stable while the inner loop is the
// faster one. The satellite steps once a second (plan item 10.1); this steps four times in
// that second, so by the time the satellite has moved once this loop has already settled
// against the move rather than chasing it.
//
// The error is AVERAGED over the window rather than sampled at the end of it. The satellite
// delivers in DMA descriptors of 120 frames, so the instantaneous ring level carries two and
// a half milliseconds of sawtooth that has nothing to do with drift; a mean over 250 ms
// leaves the drift and drops the sawtooth.
constexpr uint32_t BT_SERVO_MS = 250;
// How much the answer must move before the PLL is actually rewritten.
//
// Writing it is not free and not silent. periph_rtc_apll_freq_set rewrites the sigma-delta
// coefficients and then RECALIBRATES, spinning on clk_ll_apll_calibration_is_done()
// (esp32p4/rtc_clk.c) — so every step is a moment of analogue settling on the clock the DAC
// locks its own PLL to. One of those is what a track-boundary retune costs, and the whole
// reason stage 14 exists is that the customer's bench can hear it.
//
// Two parts per million of hysteresis makes the difference between a loop that writes four
// times a second forever and one that writes when something actually changed. The drift it
// tracks is tens of ppm and moves over minutes: at this gain a steady 22 ppm of drift walks
// the answer about 0.014 ppm per window, so the threshold is crossed roughly twice a minute
// while the loop is settling and not at all once it has. The cost is a 2 ppm parking error,
// which is 0.003 of a cent.
constexpr int32_t  BT_TRIM_HYST_PPM = 2;
// Authority blends between the two over this band instead of switching at a threshold. A
// step in the loop gain is a step in the ratio, which is a step in pitch: at a bare 10 %
// threshold the jump was 40x, about 7 cents at a block boundary, and a loop with a slightly
// wrong base parks on the threshold and chatters across it — a limit cycle sitting on the
// discontinuity, which is the warble the narrow authority was chosen to remove.
// The band's edges are BT_TRACK_MS and BT_CAPTURE_MS above: 40 ms of error at +-500 ppm is
// eighty seconds of correction, which is where tracking stops being enough.
constexpr float    I32_SCALE    = 2147483648.0f;
// The amplifier's clean ceiling until someone calibrates it, in tenths of dBFS: −9.8 dBFS.
// TPA3255 at the customer's 24 V PVDD (30.09.2026): each half-bridge swings at most ~0.95·PVDD/2
// = 11.4 V either side of mid-rail and its gain is 21.5 dB (SLASEA8A §7.5), so the PCM5102A's
// 2.97 V peak (2.1 Vrms, SLAS859C §8.5) clips it 9.8 dB under the DAC's full scale. That holds
// both for a DAC driving one input (INPUT_A, "input signal for half bridge A") and for the
// TPA3255EVM-style front end that drives both inputs at the DAC's level. Calibration
// replaces it with the module's real number.
constexpr int32_t  AMP_CLIP_DEFAULT_DB10 = -98;
// A raw calibration tone is the DAC at a known level: 1.0 is the DAC's full scale. The largest
// float under 2^31, so a full-scale sample cannot overflow the conversion.
constexpr float    RAW_SCALE = 2147483520.0f;
// A raw tone left on goes off by itself: it can be loud, and on a bench it is easy to forget.
constexpr uint32_t TONE_RAW_MAX_MS = 60000;
// A tone through the chain (the manual port search) goes off by itself too, after this long with
// nothing about it changed: a search left open must not hum all night.
constexpr uint32_t TONE_CHAIN_IDLE_MS = 120000;

inline void i32_to_f32(const int32_t* in, float* out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = (float)in[i] / I32_SCALE;
}
// Never a digital zero on the bus. The PCM5102A has a zero-data detect that cannot be
// switched off: after about 21 ms of exact zeros it puts the analog output into a full
// mute, and coming out of that mute is a pop — the "power-on" click heard at every pause
// and every start, and never between tracks, where the fades are shorter than 21 ms.
// TI's own cure (E2E, "PCM5102: Pop Noise when Pause and Play") is to send 1 LSB instead
// of 0 while paused. One LSB at 24 bits is −138 dBFS, and it keeps the DAC awake.
constexpr int32_t DAC_KEEPALIVE = 1 << 8;
// The chain's 1.0 is the amplifier's ceiling, so the conversion scales by `scale` = k_out·2^31
// (k_out ≤ −1 dBFS, see set_amp_clip) after clamping to ±1 — no product can overflow.
// fmaxf/fminf rather than two comparisons: a NaN fails every comparison, so the old clamp let
// it through to the conversion, and RISC-V's fcvt.w.s answers a NaN with 2^31 − 1 — full-scale
// DC into the amplifier. fmaxf returns its other operand for a NaN. The DSP chain throws such
// blocks away before they get here (DspChain::process); this is the second line.
// Returns how many samples it had to clamp: none, after the brickwall, which keeps |y| ≤ 1 to a
// float step (and to 1e-6 where its release snaps to 1). Anything past 1 + 1e-5 is a fault, and
// so is a NaN — which becomes silence here rather than −1 (DC at the ceiling).
inline uint32_t f32_to_i32(const float* in, int32_t* out, size_t n, float scale) {
    uint32_t clamped = 0;
    for (size_t i = 0; i < n; i++) {
        const float x = in[i];
        clamped += !(fabsf(x) <= 1.00001f);
        const float v = x == x ? fminf(fmaxf(x, -1.0f), 1.0f) : 0.0f;
        const int32_t q = (int32_t)(v * scale);
        out[i] = q ? q : DAC_KEEPALIVE;
    }
    return clamped;
}

/// The limiter's threshold for an amplifier that clips at `clip_dbfs`: half a dB under it for
/// supply sag and heat, never above −1 dBFS (the DAC's own intersample margin).
inline float ceiling_dbfs(float clip_dbfs) {
    const float t = clip_dbfs - 0.5f;
    return t < -1.0f ? t : -1.0f;
}
/// The ring sources this build can actually play: the card and the radio whenever the ring
/// exists, AirPlay only when its receiver is linked in, USB only when its device started.
int32_t ring_sources_offered() {
    int32_t m = (1 << AudioModule::SRC_FILE) | (1 << AudioModule::SRC_RADIO);
    if (airplay_offered && airplay_offered()) m |= (1 << AudioModule::SRC_AIRPLAY);
    if (usb_audio_offered && usb_audio_offered()) m |= (1 << AudioModule::SRC_USB);
    return m;
}

/// The producer that writes the ring for a ring source (DLNA rides the radio); nobody for the rest.
audio_core::PcmRing::Owner ring_owner(int32_t src) {
    switch (src) {
    case AudioModule::SRC_FILE:    return audio_core::PcmRing::OWNER_FILE;
    case AudioModule::SRC_RADIO:   return audio_core::PcmRing::OWNER_RADIO;
    case AudioModule::SRC_AIRPLAY: return audio_core::PcmRing::OWNER_AIRPLAY;
    case AudioModule::SRC_USB:     return audio_core::PcmRing::OWNER_USB;
    default:                       return audio_core::PcmRing::OWNER_NONE;
    }
}

inline float to_dbfs(float peak) { return peak > 1e-6f ? 20.0f * log10f(peak) : -120.0f; }

const char* source_name(int32_t s) {
    switch (s) {
    case AudioModule::SRC_TONE: return "tone";
    case AudioModule::SRC_AUX:  return "aux";
    case AudioModule::SRC_BT:   return "bt";
    case AudioModule::SRC_FILE: return "file";
    case AudioModule::SRC_RADIO: return "radio";
    case AudioModule::SRC_AIRPLAY: return "airplay";
    case AudioModule::SRC_USB:  return "usb";
    default:                    return "off";
    }
}

// block buffers (audio task only) — static, internal RAM
int32_t s_io_i32[BLOCK * 2];
float   s_blk[BLOCK * 2];
float   s_asrc_in[BLOCK * 4];   // ≤ 2× BLOCK input frames per output block at ratio ≥ 0.5

void audio_task_entry(void* arg) { static_cast<AudioModule*>(arg)->task_loop(); vTaskDelete(nullptr); }
void bt_rx_task_entry(void* arg) { static_cast<AudioModule*>(arg)->bt_rx_loop(); vTaskDelete(nullptr); }

/// Main task: room for one copy of the EQ document, which is over a kilobyte and the main
/// task's stack has little to spare (own_eq, on_init).
audio_core::EqDoc& eq_scratch() {
    static audio_core::EqDoc d;
    return d;
}

/// A web edit holds the write back this long after it: a drag sends one per step, and every
/// write stops both cores' caches — the speakers click (own_eq).
constexpr int64_t EQ_WEB_QUIET_MS = 60000;

#include "eq_http.inc"

} // namespace

AudioModule::AudioModule()
    : BaseModule("audio", modesp::ModulePriority::NORMAL)
{}

// ═══════════════════════════════════════════════════════════════
// Bind: I²S transports from the audio registry, amp enable from DriverManager
// ═══════════════════════════════════════════════════════════════

void AudioModule::on_bind(modesp::DriverManager& drivers,
                          const modesp::BindingTable& bindings, modesp::HAL& hal) {
    for (const auto& b : bindings.bindings) {
        if (!(b.module_name == "audio")) continue;
        const char* type = b.driver_type.c_str();
        if (!modesp::DriverRegistry::has_audio(type)) continue;   // actuators are DriverManager's
        auto* sink = modesp::DriverRegistry::create_audio(type, b, hal);
        auto* port = sink ? sink->as_port() : nullptr;
        if (!port) {
            ESP_LOGW(TAG, "backend '%s' [hw=%s, role=%s] has no IAudioPort", type,
                     b.hardware_id.c_str(), b.role.c_str());
            continue;
        }
        if (b.role == "port_main")     main_ = port;
        else if (b.role == "port_bt")  bt_ = port;
        else ESP_LOGW(TAG, "unknown audio role '%s' ignored", b.role.c_str());
        ESP_LOGI(TAG, "%s '%s' [hw=%s]", b.role.c_str(), type, b.hardware_id.c_str());
    }
    amp_ = drivers.find_actuator("amp_enable");
    if (auto* bus = hal.find_i2c_bus("i2c_0")) {
        mic_bus_ = bus->bus_handle;
        mic_capture::start(bus->bus_handle);
    }
    ESP_LOGI(TAG, "bound: main=%s bt=%s amp=%s", main_ ? "yes" : "no", bt_ ? "yes" : "no",
             amp_ ? amp_->type() : "no");
}

// ═══════════════════════════════════════════════════════════════
// Init: ports, EQ file, ASRC, tasks
// ═══════════════════════════════════════════════════════════════

bool AudioModule::load_eq_file(audio_core::EqBand* bands, int& count) {
    count = 0;
    audio_core::EqConfig cfg;
    // Through UserStore: the EQ someone tuned lives in /userdata since 22.09.2026, and the
    // image's /data/eq.json is the factory-flat fallback a panel boots with before anyone
    // has touched a slider (plan item 11.1).
    char path[160];
    modesp::UserStore::read_path("eq.json", path, sizeof(path));
    if (!audio_core::eq_load(path, cfg)) {
        ESP_LOGW(TAG, "%s missing or unreadable — EQ flat", path);
        return false;
    }
    take_eq(cfg, bands, count);
    return true;
}

void AudioModule::take_eq(const audio_core::EqConfig& cfg, audio_core::EqBand* bands, int& count) {
    snprintf(out_mode_, sizeof(out_mode_), "%s", cfg.mode);
    if (strcmp(out_mode_, "2.0") != 0) {
        ESP_LOGW(TAG, "eq.json: mode '%s' needs a second DAC — running 2.0", out_mode_);
    }
    // Only section 0 drives the one stereo pair this firmware has. The parser keeps the
    // rest so nothing is lost when the file is written back (docs/CONCEPT.md, "Outputs").
    const audio_core::EqSection& sec = cfg.sec[0];
    count = sec.count;
    for (int i = 0; i < count; i++) bands[i] = sec.bands[i];
    eq_trim_db_ = sec.gain_db;

    int active = 0;
    for (int i = 0; i < count; i++) if (bands[i].enabled) active++;
    ESP_LOGI(TAG, "eq.json: mode %s, %d section(s), %d bands, %d active, trim %d dB",
             out_mode_, cfg.sections, count, active, (int)eq_trim_db_);
}

/// The EQ document's owner (audio_dsp/eq_store.h). Main task, every tick, whether or not there
/// is audio to hear it: the panel's DSP page and the web put edits into the store, and only
/// this hands them on and writes them back.
///
/// Heard at once: a new revision goes to the audio core through EqBus, of which this is the one
/// producer, and audio.eq_rev tells the web (and the page, through the store) that it moved.
///
/// Kept at a moment that is not in the way. A LittleFS write stops both cores' caches and the
/// speakers click (customer, 04.10.2026), so nothing is written while the DSP page is in front
/// — it is written when the user leaves it, or in standby — nor while the web is still sending
/// edits: EQ_WEB_QUIET_MS after the last one, or at once when the web page is left
/// ({"keep":true}).
void AudioModule::own_eq() {
    audio_core::EqStore& st = audio_core::eq_store();
    if (st.revision() != eq_heard_rev_) {
        audio_core::EqDoc& doc = eq_scratch();
        eq_heard_rev_ = st.get(doc);
        audio_core::eq_bus().publish(doc.cfg.sec[0].bands, doc.cfg.sec[0].count);
        send(CmdType::EQ_RELOAD, 0);              // nothing when the audio core is not running
        state_set("audio.eq_rev", (int32_t)eq_heard_rev_);
    }

    if (!st.dirty()) { st.take_keep(); return; }  // nothing to keep: an old ask is spent
    char screen[16] = "";
    read_string("ui.screen", screen, sizeof(screen));
    if (strcmp(screen, "eq") == 0) return;
    const int64_t web = st.web_touched_at();
    const int64_t now = esp_timer_get_time() / 1000;
    if (!st.take_keep() && web != 0 && now - web < EQ_WEB_QUIET_MS) return;

    // Always to the store: an edit must survive the next full flash (plan item 11.1).
    // Static: the main task had 468 bytes of stack to spare at its deepest (11.10.2026).
    static char eq_path[160], custom_path[160];
    modesp::UserStore::write_path("eq.json", eq_path, sizeof(eq_path));
    modesp::UserStore::write_path("eq_custom.json", custom_path, sizeof(custom_path));
    if (st.save(eq_path, custom_path)) {
        ESP_LOGI(TAG, "eq.json saved (rev %lu)", (unsigned long)eq_heard_rev_);
    } else {
        ESP_LOGE(TAG, "eq.json save failed — keeping the previous file");
    }
}

void AudioModule::log_bass(const char* when) {
    const int32_t hz = last_port_ >> 4, lift = last_port_ & 15;
    if (hz >= 30)
        ESP_LOGW(TAG, "bass (%s): loudness %ld %% from %ld %%, port %ld Hz +%ld dB, subsonic under it at %ld Hz",
                 when, (long)last_loud_, (long)last_loud_ref_, (long)hz, (long)lift, (long)lroundf(0.8f * (float)hz));
    else
        ESP_LOGW(TAG, "bass (%s): loudness %ld %% from %ld %%, no port%s, subsonic %ld Hz", when, (long)last_loud_,
                 (long)last_loud_ref_,
                 read_int("audio.spk_type", 0) == modesp::gen::AUDIO_SPK_TYPE_CLOSED ? " (closed speakers)" : " set",
                 (long)last_hp_hz_);
}

void AudioModule::meter_entry(void* self) {
    auto* m = static_cast<AudioModule*>(self);
    m->port_meter_loop();
    m->meter_busy_.store(false);
    vTaskDelete(nullptr);
}

// The port meter. The panel's microphone is held at the woofer of one speaker; that speaker plays
// one sine at a time, past the DSP at an exact level (the calibration tone's raw mode), at whole
// hertz from 20 to 100; at each, the microphone's level at that frequency is read through a 0.4 s
// Hann window. Before that, with nothing playing, the same windows give the noise at every
// frequency — the room, and mains hum. find_port() takes the dip (port_finder.h). It drives the
// module through its own settings, so the screen shows what is going on, and puts the source and
// the tone back at the end whatever happened. About 40 s, under the raw tone's 60 s limit.
void AudioModule::port_meter_loop() {
    using audio_core::ToneMeter;
    using audio_core::PortFit;
    constexpr uint32_t WIN = 19200;                       // 0.4 s at 48 kHz
    constexpr int MAXG = 64;
    // audio.port_meas_status is a message of the web's catalog ("@<key>", arguments behind
    // MSG_ARG): each browser shows it in its language.
    // The log gets it with the separators as spaces.
    auto say = [this](const char* s) {
        state_set("audio.port_meas_status", s);
        char line[96];
        snprintf(line, sizeof line, "%s", s);
        for (char* c = line; *c; c++) if (*c == '\x1f') *c = ' ';
        ESP_LOGW(TAG, "port meter: %s", line);
    };
    auto stopped = [this]() { return !read_bool("audio.port_meas", false); };
    const int32_t src0 = read_int("audio.source", SRC_OFF), mode0 = read_int("audio.tone_mode", TONE_CHAIN);
    const int32_t hz0 = read_int("audio.tone_hz", 1000), db0 = read_int("audio.tone_db10", -260);
    const int32_t ch0 = read_int("audio.tone_ch", 3);
    const int32_t ch = read_int("audio.port_meas_ch", 1) == 2 ? 2 : 1;
    int grid[MAXG];
    const int n = audio_core::port_sweep_grid(20, 100, grid, MAXG);
    float hz[MAXG], level[MAXG], floor_[MAXG];
    float* buf = (float*)heap_caps_malloc(WIN * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // audio.port_meas_status is a SharedState string: 64 bytes, so a few words; the log has the rest.
    char msg[96];
    bool measured = false;
    int32_t db10 = -460;
    // One continuous 0.4 s stretch: what the DMA held from before is dropped first, and a stretch
    // the DMA queue overflowed in is read again.
    auto window = [&](float* pk) {
        for (int tries = 0; tries < 2; tries++) {
            mic_input::discard(30);
            if (mic_input::read(buf, WIN, 1000, pk)) return true;
        }
        return false;
    };
    auto amp_at = [&](float f) {
        ToneMeter m;
        m.start(f, (float)mic_input::RATE, WIN);
        m.push(buf, WIN);
        return m.amplitude();
    };
    do {
        if (!buf) { say("@cal.m_no_memory"); break; }
        if (!mic_input::open((i2c_master_bus_handle_t)mic_bus_, 18)) { say("@cal.m_mic_open"); break; }
        // Silence: nothing playing, and the codec's first second thrown away.
        say("@cal.m_silence");
        state_set("audio.source", (int32_t)SRC_OFF);
        mic_input::discard(1000);
        for (int k = 0; k < n; k++) { hz[k] = (float)grid[k]; floor_[k] = 0.0f; }
        bool ok = true;
        for (int w = 0; w < 6 && ok; w++) {
            if (!window(nullptr)) { ok = false; break; }
            for (int k = 0; k < n; k++) { const float a = amp_at(hz[k]); floor_[k] += a * a; }
            if (stopped()) ok = false;
        }
        if (!ok) { say(stopped() ? "@cal.m_stopped" : "@cal.m_mic_read"); break; }
        for (int k = 0; k < n; k++) floor_[k] = sqrtf(floor_[k] / 6.0f);

        // The tone: past the DSP, on one speaker, quiet to begin with.
        state_set("audio.tone_mode", (int32_t)TONE_RAW);
        state_set("audio.tone_ch", ch);
        state_set("audio.tone_db10", db10);
        state_set("audio.tone_hz", (int32_t)90);
        state_set("audio.source", (int32_t)SRC_TONE);
        vTaskDelay(pdMS_TO_TICKS(900));                  // the amplifier's settle, the envelope
        // The level: the loudest stretch of the sweep, the passband at 90 Hz, to a tenth of the
        // microphone's range — between −60 and −30 dBFS of drive, and 10 dB down on clipping.
        for (int step = 0; step < 4 && ok; step++) {
            float pk = 0.0f;
            if (!window(&pk)) { ok = false; break; }
            const float a = amp_at(90.0f);
            int32_t want = db10 + (a > 1e-6f ? (int32_t)lroundf(200.0f * log10f(0.1f / a)) : 160);
            if (pk > 0.95f) want = db10 - 100;
            want = want < -600 ? -600 : want > -300 ? -300 : want;
            if (abs(want - db10) < 20) break;
            db10 = want;
            state_set("audio.tone_db10", db10);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (!ok) { say("@cal.m_mic_read"); break; }

        // The sweep.
        for (int k = 0; k < n && ok; k++) {
            state_set("audio.tone_hz", (int32_t)grid[k]);
            if (k % 5 == 0) {
                snprintf(msg, sizeof msg, "@cal.m_progress" MSG_ARG "hz=%d" MSG_ARG "k=%d" MSG_ARG "n=%d",
                         grid[k], k + 1, n);
                state_set("audio.port_meas_status", msg);
            }
            vTaskDelay(pdMS_TO_TICKS(250));              // the tone's way there, the box settling
            if (stopped() || read_int("audio.source", SRC_OFF) != SRC_TONE) { ok = false; break; }
            if (!window(nullptr)) { ok = false; break; }
            level[k] = amp_at(hz[k]);
        }
        if (!ok) { say(stopped() ? "@cal.m_stopped" : "@cal.m_interrupted"); break; }
        measured = true;
    } while (false);

    // Back the way it was, whatever happened.
    mic_input::close();
    if (buf) heap_caps_free(buf);
    state_set("audio.tone_hz", hz0);
    state_set("audio.tone_db10", db0);
    state_set("audio.tone_ch", ch0);
    state_set("audio.tone_mode", mode0);
    // The source the meter found, unless somebody chose another one while it measured: theirs stays.
    const int32_t src_now = read_int("audio.source", SRC_OFF);
    if (src_now == SRC_TONE || src_now == SRC_OFF) state_set("audio.source", src0);

    if (measured) {
        // The curve, for whoever reads the console: dBFS at the microphone, the tone's and the noise's.
        for (int k0 = 0; k0 < n; k0 += 8) {
            char line[200];
            int len = 0;
            for (int k = k0; k < n && k < k0 + 8 && len < (int)sizeof(line) - 24; k++)
                len += snprintf(line + len, sizeof(line) - len, " %d:%.1f/%.1f", grid[k],
                                (double)(20.0f * log10f(level[k] + 1e-9f)), (double)(20.0f * log10f(floor_[k] + 1e-9f)));
            ESP_LOGI(TAG, "port meter, dBFS tone/noise at %.1f dBFS drive:%s", (double)db10 / 10.0, line);
        }
        const PortFit fit = audio_core::find_port(hz, level, floor_, n);
        ESP_LOGW(TAG, "port meter: status %d, %.1f Hz, %.1f dB above / %.1f dB below, %.1f dB over noise (loudest %.1f)",
                 (int)fit.status, (double)fit.hz, (double)fit.depth_db, (double)fit.below_db, (double)fit.snr_db,
                 (double)fit.loud_db);
        const int32_t f = (int32_t)lroundf(fit.hz);
        switch (fit.status) {
        case PortFit::OK:
            // Found, and offered — never written (S6): «Прийняти» in the speakers' wizard or
            // «Застосувати» on the calibration page writes audio.port_hz, as measured. Until then
            // nothing about the sound has changed.
            state_set("audio.port_meas_hz", fit.hz);
            if (f >= 30 && f <= 80) {
                snprintf(msg, sizeof msg, "@cal.m_found" MSG_ARG "hz=%ld", (long)f);
            } else {
                snprintf(msg, sizeof msg, "@cal.m_outside" MSG_ARG "hz=%ld", (long)f);
            }
            break;
        case PortFit::WEAK:
            state_set("audio.port_meas_hz", fit.hz);
            snprintf(msg, sizeof msg, "@cal.m_doubtful" MSG_ARG "hz=%ld", (long)f);
            break;
        case PortFit::NO_DIP:
            snprintf(msg, sizeof msg, "%s", "@cal.m_no_dip");
            break;
        default:
            snprintf(msg, sizeof msg, "%s", "@cal.m_no_tone");
            break;
        }
        say(msg);
    }
    state_set("audio.port_meas", false);
}

int32_t AudioModule::read_port() {
    // The manual port search plays its tone with the port taken out (audio.cal_port_off) rather
    // than writing audio.port_hz = 0: a search that ends any way at all leaves the setting as it was.
    if (read_bool("audio.cal_port_off", false) && read_int("audio.source", SRC_OFF) == SRC_TONE) return 0;
    // Closed speakers have no port: the number stays, for the day somebody answers otherwise.
    if (read_int("audio.spk_type", modesp::gen::AUDIO_SPK_TYPE_UNSET) == modesp::gen::AUDIO_SPK_TYPE_CLOSED) return 0;
    int32_t hz = read_int("audio.port_hz", 0), lift = read_int("audio.port_lift_db", 3);
    hz = hz < 0 ? 0 : hz > 80 ? 80 : hz;
    lift = lift < 0 ? 0 : lift > 6 ? 6 : lift;
    return hz * 16 + lift;
}

int32_t AudioModule::vol_max() {
    const int32_t v = read_int("audio.vol_max", 100);
    return v < 40 ? 40 : v > 100 ? 100 : v;
}

int32_t AudioModule::loud_ref() {
    const int32_t v = read_int("audio.loud_ref", 100);
    return v < 20 ? 20 : v > 100 ? 100 : v;
}

int32_t AudioModule::timbre() {
    auto db = [this](const char* key) {
        const int32_t v = read_int(key, 0);
        return v < -6 ? -6 : v > 6 ? 6 : v;
    };
    return (int32_t)(((uint32_t)(uint8_t)(int8_t)db("audio.bass_db") << 8) | (uint8_t)(int8_t)db("audio.treble_db"));
}

bool AudioModule::on_init() {
    state_set("audio.state", "off");
    state_set("audio.level_l", -120.0f);
    state_set("audio.level_r", -120.0f);
    state_set("audio.underruns", (int32_t)0);
    state_set("audio.in_rate", 0.0f);
    state_set("audio.asrc_ppm", 0.0f);
    state_set("audio.cpu_pct", 0.0f);
    state_set("audio.dsp_faults", (int32_t)0);
    state_set("audio.dsp_pct", 0.0f);
    state_set("audio.dsp_max_pct", 0.0f);
    state_set("audio.out_clamps", (int32_t)0);
    state_set("audio.eq_headroom_db", 0.0f);
    state_set("audio.loud_db", 0.0f);
    // The output schemes this firmware plays (out_caps.h): «Кросовер» offers what is said here
    // and shows the rest as not supported yet.
    state_set("audio.out_caps", audio_core::OUT_CAPS_NOW);
    state_set("audio.port_meas", false);
    state_set("audio.cal_port_off", false);
    state_set("audio.cal_vol_cap", (int32_t)0);
    state_set("audio.port_meas_status", "");
    state_set("audio.port_meas_hz", 0.0f);
    state_set("audio.in_l", -120.0f);
    state_set("audio.in_r", -120.0f);
    state_set("audio.eq_reload", false);
    state_set("audio.sources", (int32_t)(1 << SRC_OFF));   // refined below once the ports are known

    // The EQ document (audio_dsp/eq_store.h), before anything below can give up: this module
    // owns it whether or not there is an audio core to hear it (own_eq).
    {
        static char eq_path[160], custom_path[160];   // the main task's stack, as in own_eq
        modesp::UserStore::read_path("eq.json", eq_path, sizeof(eq_path));
        modesp::UserStore::read_path("eq_custom.json", custom_path, sizeof(custom_path));
        audio_core::EqStore& st = audio_core::eq_store();
        st.load(eq_path, custom_path);
        // Nothing is formatted or rewritten here: a file the store cannot read stays as it
        // is, and the first save after an edit moves it to <name>.bad (eq_store.h).
        const uint8_t notes = st.load_notes();
        if (notes & audio_core::EqStore::EQ_UNREADABLE) {
            ESP_LOGW(TAG, "%s unreadable - EQ flat; kept as it is until an edit, then as .bad", eq_path);
        }
        if (notes & audio_core::EqStore::CUSTOM_UNREADABLE) {
            ESP_LOGW(TAG, "%s unreadable - the own set is what plays; kept until it is written, then as .bad",
                     custom_path);
        }
        if (notes & audio_core::EqStore::CUSTOM_FROM_COPY) {
            ESP_LOGW(TAG, "%s held a preset an older build took for the own set - the set from the .v2 copies",
                     custom_path);
        }
        audio_core::EqDoc& doc = eq_scratch();
        eq_heard_rev_ = st.get(doc);
        // On the bus from the start, so the audio task designs from memory even at a rate
        // change and never has to read the file.
        audio_core::eq_bus().publish(doc.cfg.sec[0].bands, doc.cfg.sec[0].count);
        state_set("audio.eq_rev", (int32_t)eq_heard_rev_);
        if (!modesp::http_routes::add(register_eq_routes)) {
            ESP_LOGW(TAG, "no room in the route table — /api/eq will 404");
        }
    }

    if (!main_) {
        ESP_LOGW(TAG, "no port_main binding — audio core disabled");
        state_set("audio.state", "no-port");
        return true;
    }
    if (!main_->init() || !main_->port_caps().has_tx) {
        ESP_LOGE(TAG, "port_main init failed or has no TX — audio core disabled");
        state_set("audio.state", "error");
        main_ = nullptr;
        return true;
    }
    if (bt_ && (!bt_->init() || !bt_->port_caps().has_rx)) {
        ESP_LOGW(TAG, "port_bt init failed — BT source unavailable");
        bt_ = nullptr;
    }
    // The satellite's three pins, in case it lets us clock the link (protocol 1.9).
    if (bt_) link_pins_ = bt_->link_pins();

    // EQ: the document loaded above
    audio_core::EqBand bands[audio_core::MAX_BANDS];
    int count = 0;
    take_eq(eq_scratch().cfg, bands, count);
    state_set("audio.out_mode", out_mode_);   // after the parse, not before
    dsp_.set_width((float)read_int("audio.width", 100) / 100.0f);
    dsp_.set_mono_bass((float)read_int("audio.mono_bass_hz", 0));
    dsp_.set_bands(bands, count);
    dsp_.set_pre_gain_db(eq_trim_db_);
    dsp_.set_eq_enabled(read_bool("audio.eq_enabled", true));
    {
        // After power-on no louder than audio.vol_start (S4). The saved value itself comes down,
        // not just the level, so every client shows what plays — and /api/restore, which saves
        // and restarts, lands here too.
        const int32_t saved = read_int("audio.volume", 30);
        const int32_t boot = audio_core::LevelLatch::boot(saved, read_int("audio.vol_start", 0));
        if (boot != saved) {
            state_set("audio.volume", boot);
            ESP_LOGW(TAG, "volume after power-on: %ld %%, not the saved %ld %% (audio.vol_start)",
                     (long)boot, (long)saved);
        }
        last_volume_ = boot;
        last_vol_max_ = vol_max();
        level_.put(last_volume_, last_vol_max_);
        dsp_.set_volume(audio_core::LevelLatch::effective(last_volume_, last_vol_max_));
    }
    dsp_.set_drive_db((float)read_int("audio.drive_db", 0));
    last_hp_hz_ = read_int("audio.hp_hz", 40);
    dsp_.set_subsonic((float)last_hp_hz_);
    {
        // Where the port came from (audio.port_src, S6). Before that key the meter wrote what it
        // found by itself, and a number typed in looked the same: one that is set was set by
        // somebody, and nothing shows it was measured — as audio.amp_src at S0a.
        const int32_t src = read_int("audio.port_src", modesp::gen::AUDIO_PORT_SRC_UNSET);
        if (src == modesp::gen::AUDIO_PORT_SRC_UNSET && read_int("audio.port_hz", 0) > 0)
            state_set("audio.port_src", (int32_t)modesp::gen::AUDIO_PORT_SRC_MANUAL);
    }
    last_port_ = read_port();
    dsp_.set_port((float)(last_port_ >> 4), (float)(last_port_ & 15));
    last_loud_ = read_int("audio.loudness", 70);
    dsp_.set_loudness((float)last_loud_ / 100.0f);
    last_loud_ref_ = loud_ref();
    dsp_.set_loud_ref((float)last_loud_ref_);
    last_timbre_ = timbre();
    dsp_.set_tone((float)(int8_t)(last_timbre_ >> 8), (float)(int8_t)last_timbre_);
    last_balance_ = read_int("audio.balance_db10", 0);
    dsp_.set_balance_db((float)last_balance_ / 10.0f);
    last_leveler_ = read_bool("audio.leveler", false) ? 1 : 0;
    dsp_.set_leveler(last_leveler_ != 0);
    log_bass("boot");
    // The bass limiter's look-ahead: up to 8 KB, read and written once a frame in order — PSRAM
    // through the cache serves that well, and internal RAM is what the TLS radio runs short of.
    bass_line_ = (float*)heap_caps_malloc(audio_core::DspChain::BASS_LINE_FRAMES * 2 * sizeof(float),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bass_line_) dsp_.set_bass_line(bass_line_, audio_core::DspChain::BASS_LINE_FRAMES);
    else ESP_LOGW(TAG, "bass limiter: no PSRAM for its look-ahead — off");
    last_bass_xo_ = read_int("audio.bass_xo_hz", 120);
    dsp_.set_bass_xo((float)last_bass_xo_);
    last_rms_cap_ = read_int("audio.rms_cap_db10", 0);
    dsp_.set_rms_cap_db((float)last_rms_cap_ / 10.0f);
    last_drive_ = read_int("audio.drive_db", 0);
    last_amp_clip_ = read_int("audio.amp_clip_db10", AMP_CLIP_DEFAULT_DB10);
    set_amp_clip(last_amp_clip_);
    {
        // Where the ceiling came from (audio.amp_src, 14.10.2026). Before that key, "write" marked
        // any ceiling calibrated, the untouched datasheet one included, and a ceiling set any other
        // way still said "datasheet". Only the number can be trusted: one that is not the
        // datasheet's was set by somebody, and nothing shows it was measured. audio.amp_cal stays
        // for older readers and means "measured", nothing else.
        int32_t src = read_int("audio.amp_src", modesp::gen::AUDIO_AMP_SRC_DATASHEET);
        if (src == modesp::gen::AUDIO_AMP_SRC_DATASHEET && last_amp_clip_ != AMP_CLIP_DEFAULT_DB10) {
            src = modesp::gen::AUDIO_AMP_SRC_MANUAL;
            state_set("audio.amp_src", src);
        }
        const bool measured = src == modesp::gen::AUDIO_AMP_SRC_MEASURED;
        if (read_bool("audio.amp_cal", false) != measured) state_set("audio.amp_cal", measured);
        static const char* const FROM[] = {"TPA3255 datasheet default at 24 V", "set by hand",
                                           "found by ear", "measured"};
        static_assert((int32_t)(sizeof(FROM) / sizeof(FROM[0])) == modesp::gen::AUDIO_AMP_SRC_MAX + 1,
                      "one word for each audio.amp_src option");

        const float c = (float)last_amp_clip_ / 10.0f;
        state_set("audio.ceiling_dbfs", ceiling_dbfs(c));
        // Said once at boot, because "why is it quieter than before" has this as its answer.
        const int c10 = (int)last_amp_clip_, t10 = (int)lroundf(ceiling_dbfs(c) * 10.0f);
        ESP_LOGW(TAG, "amplifier ceiling %s%d.%d dBFS (%s) -> limiter at %s%d.%d dBFS; 100 %% volume "
                      "puts a full-scale source there",
                 c10 < 0 ? "-" : "", abs(c10) / 10, abs(c10) % 10,
                 src >= 0 && src <= modesp::gen::AUDIO_AMP_SRC_MAX ? FROM[src] : "unknown source",
                 t10 < 0 ? "-" : "", abs(t10) / 10, abs(t10) % 10);
    }
    muted_ = read_bool("audio.mute", false);
    dsp_.set_mute(muted_);
    tone_.set_freq((float)read_int("audio.tone_hz", 1000));
    bt_clock_allowed_ = last_bt_clock_ = read_bool("audio.bt_clock_follow", true);

    // ASRC + PSRAM ring for the BT source
    if (bt_) {
        bt_ring_frames_ = 48000 * BT_RING_SEC;
        bt_ring_ = (float*)heap_caps_malloc(bt_ring_frames_ * 2 * sizeof(float),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        asrc_ = resampleInit(2, ASRC_TAPS, ASRC_FILTERS, ASRC_LOWPASS,
                             SUBSAMPLE_INTERPOLATE | BLACKMAN_HARRIS | INCLUDE_LOWPASS);
        if (!bt_ring_ || !asrc_) {
            ESP_LOGE(TAG, "BT ring/ASRC allocation failed — BT source unavailable");
            bt_ = nullptr;
        }
    }

    // Which sources this build can actually play — bindings decide, not the code. The UI
    // greys out the rest, and a persisted source that is no longer wired falls back to OFF
    // (otherwise audio.source would say "bt" forever while the task refuses to switch).
    // Tone is not in the mask. It is always available — it is a generator, not a wire — but
    // the mask answers "what can someone choose to listen to", and a 1 kHz test signal is not
    // that. It keeps its number, fixed by the source contract in docs/CONCEPT.md, and stays
    // reachable by writing audio.source = 1 for the job it exists for: proving the output
    // path when no source is connected.
    tx_frames_ = main_->port_caps().tx_queue_frames;
    retime();
    publish_consumer();

    int32_t sources = (1 << SRC_OFF);
    if (main_->port_caps().has_rx && LINE_IN_FITTED) sources |= (1 << SRC_AUX);
    if (bt_)                       sources |= (1 << SRC_BT);
    // "file" is offered when something has allocated the PCM ring — that is the only
    // signal the audio core needs, and it keeps this module from knowing the player exists.
    // Re-checked in the report loop because module init order is not ours to depend on.
    // Both the card and the radio produce into the same ring, so its existence is what
    // makes either of them available; which one is producing is decided by the source.
    if (audio_core::pcm_ring().capacity() > 0) sources |= ring_sources_offered();
    state_set("audio.sources", sources);
    const int32_t persisted = read_int("audio.source", SRC_OFF);
    if (!(sources & (1 << persisted))) {
        if (is_ring_source(persisted) && audio_core::pcm_ring().capacity() == 0) {
            // Not "unavailable" — not offered YET: the ring's owner initialises after this
            // module. Keep the choice and settle it in the report loop once the ring exists.
            // Falling back here brought every boot with the radio persisted up silent, and
            // the same for AirPlay (24.09.2026).
            pending_source_ = persisted;
            ESP_LOGI(TAG, "source %s waits for the PCM ring", source_name(persisted));
        } else {
            ESP_LOGW(TAG, "source %s not available in this build — falling back to off",
                     source_name(persisted));
            state_set("audio.source", (int32_t)SRC_OFF);
        }
    }

    cmd_q_ = xQueueCreate(16, sizeof(Cmd));
    run_ = true;
    // Audio task: core 1, high priority, stack in INTERNAL RAM (docs/CONCEPT.md, "Board
    // constraints" — a PSRAM stack dies on the first NVS write while the cache is off).
    if (xTaskCreatePinnedToCore(audio_task_entry, "audio", 8192, this, 22, &task_, 1) != pdPASS) {
        ESP_LOGE(TAG, "audio task not created");
        run_ = false;
        state_set("audio.state", "error");
        return true;
    }
    if (bt_ && xTaskCreatePinnedToCore(bt_rx_task_entry, "audio_bt", 4096, this, 23, &bt_task_, 1) != pdPASS) {
        ESP_LOGW(TAG, "BT reader task not created — BT source unavailable");
        bt_ = nullptr;
    }

    ESP_LOGI(TAG, "Initialized: main %lu Hz %u-bit%s%s, BT %s, amp %s, heap DMA %u",
             (unsigned long)main_->port_caps().sample_rate_hz, (unsigned)main_->port_caps().slot_bits,
             main_->port_caps().has_rx ? " duplex" : " tx-only",
             main_->port_caps().is_master ? " master" : " slave",
             bt_ ? "yes" : "no", amp_ ? "yes" : "no",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    return true;
}

// ═══════════════════════════════════════════════════════════════
// Main-loop side: SharedState → commands, status → SharedState
// ═══════════════════════════════════════════════════════════════

void AudioModule::send(CmdType t, int32_t v) {
    if (!cmd_q_) return;
    Cmd c{t, v};
    if (xQueueSend(cmd_q_, &c, 0) != pdTRUE) ESP_LOGW(TAG, "command queue full");
}

void AudioModule::leveler_schedule(uint32_t dt_ms) {
    lev_sched_ms_ += dt_ms;
    if (lev_sched_ms_ < 1000) return;
    lev_sched_ms_ = 0;
    int now_min = -1;
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (t.tm_year + 1900 >= 2020) now_min = t.tm_hour * 60 + t.tm_min;   // SNTP has set the clock
    const int32_t from = read_int("audio.lev_from", 22 * 60), to = read_int("audio.lev_to", 7 * 60);
    const int r = lev_sched_.tick(now_min, read_bool("audio.lev_sched", false), (int)from, (int)to);
    if (r < 0 || (r == 1) == read_bool("audio.leveler", false)) return;
    ESP_LOGI(TAG, "leveler %s by the schedule (%02d:%02d–%02d:%02d)", r ? "on" : "off",
             (int)(from / 60), (int)(from % 60), (int)(to / 60), (int)(to % 60));
    state_set("audio.leveler", r == 1);
}

void AudioModule::on_update(uint32_t dt_ms) {
    own_eq();
    if (!main_ || !run_) return;

    const int32_t src  = read_int("audio.source", SRC_OFF);
    int32_t       vol  = read_int("audio.volume", 30);
    const int32_t tone = read_int("audio.tone_hz", 1000);
    const bool    mute = read_bool("audio.mute", false);
    const bool    eq   = read_bool("audio.eq_enabled", true);
    // What a test tone may change while it plays — the volume capped (audio.cal_vol_cap), the port
    // taken out (audio.cal_port_off, read_port) — lives in keys that are never saved and go with
    // the tone, however it stops: the saved volume and port are never written for a test.
    if (src == SRC_TONE) {
        const int32_t cap = read_int("audio.cal_vol_cap", 0);
        if (cap > 0 && vol > cap) vol = cap;
    } else if (last_source_ == SRC_TONE) {
        if (read_bool("audio.cal_port_off", false)) state_set("audio.cal_port_off", false);
        if (read_int("audio.cal_vol_cap", 0) != 0) state_set("audio.cal_vol_cap", (int32_t)0);
    }
    // anything that changes the tone restarts the clock of a tone through the chain (below)
    bool tone_touched = (src == SRC_TONE && last_source_ != SRC_TONE) || tone != last_tone_;

    if (src != last_source_)  { last_source_ = src;  send(CmdType::SOURCE, src); }
    const int32_t vmax = vol_max();
    if (vol != last_volume_ || vmax != last_vol_max_) {
        last_volume_ = vol;
        last_vol_max_ = vmax;
        level_.put(vol, vmax);          // the last value, read at the next block: never dropped
        vol_published_ = false;
    }
    if (!vol_published_ && level_.applied()) {
        vol_published_ = true;
        state_set("audio.vol_eff", audio_core::LevelLatch::effective(vol, vmax));
    }
    if (tone != last_tone_)   { last_tone_ = tone;   send(CmdType::TONE_HZ, tone); }
    if (mute != last_mute_)   { last_mute_ = mute;   send(CmdType::MUTE, mute); }
    if (eq != last_eq_)       { last_eq_ = eq;       send(CmdType::EQ_ENABLE, eq); }
    const bool btclk = read_bool("audio.bt_clock_follow", true);
    if (btclk != last_bt_clock_) { last_bt_clock_ = btclk; send(CmdType::BT_CLOCK, btclk); }
    if (bt_path_ != last_bt_path_) {
        // The audio task chose the Bluetooth path at its last prime; say why, once, from
        // here: that task does not log.
        static const char* const WHY[] = {
            "undecided", "clock", "resampler: audio.bt_clock_follow off",
            "resampler: stream rate differs from the output clock",
            "resampler: an earlier trim was refused",
            "resampler: the port cannot trim at this format",
            "resampler: clock correction cannot sustain delivery",
            "DIAGNOSTIC DIRECT PCM: ASRC bypassed, P4 trim fixed, 60 s maximum",
            "DIAGNOSTIC FIXED RATIO: ASRC active at ratio 1, P4 trim fixed, 60 s maximum",
            "DIAGNOSTIC STOP: announced/output rates differ or are unknown",
            "DIAGNOSTIC STOP: BT input underrun; window ended before rebuffer",
            "DIAGNOSTIC STOP: ring reached resync threshold; no samples skipped",
            "DIAGNOSTIC STOP: BT ring dropped input frames",
            "DIAGNOSTIC STOP: RX DMA queue overflow",
            "DIAGNOSTIC STOP: TX DMA queue overflow or short write",
            "DIAGNOSTIC COMPLETE: 60 s window ended; OFF/BT or new format to re-arm",
            "DIAGNOSTIC STOP: ASRC produced a short block",
            "DIAGNOSTIC STOP: unexpected P4 clock trim",
            "CONTINUOUS DIRECT PCM: ASRC bypassed, P4 trim fixed, automatic rebuffer",
            "CONTINUOUS DIRECT PCM: waiting for equal announced/output rates"};
        last_bt_path_ = bt_path_;
        const char* why = WHY[last_bt_path_ < sizeof(WHY) / sizeof(WHY[0]) ? last_bt_path_ : 0];
        if (BT_DIAGNOSTIC || MODESP_AUDIO_BT_DIRECT_CONTINUOUS) ESP_LOGW(TAG, "BT path: %s", why);
        else ESP_LOGI(TAG, "BT path: %s", why);
    }
    const int32_t width = read_int("audio.width", 100);
    const int32_t mbass = read_int("audio.mono_bass_hz", 0);
    if (width != last_width_) { last_width_ = width; send(CmdType::WIDTH, width); }
    if (mbass != last_mono_bass_) { last_mono_bass_ = mbass; send(CmdType::MONO_BASS, mbass); }
    const int32_t clip = read_int("audio.amp_clip_db10", AMP_CLIP_DEFAULT_DB10);
    if (clip != last_amp_clip_) {
        last_amp_clip_ = clip;
        send(CmdType::AMP_CLIP, clip);
        state_set("audio.ceiling_dbfs", ceiling_dbfs((float)clip / 10.0f));
    }
    const int32_t drive = read_int("audio.drive_db", 0);
    if (drive != last_drive_) { last_drive_ = drive; send(CmdType::DRIVE, drive); }
    const int32_t hp = read_int("audio.hp_hz", 40);
    bool bass_moved = false;
    if (hp != last_hp_hz_) { last_hp_hz_ = hp; send(CmdType::SUBSONIC, hp); bass_moved = true; }
    const int32_t xo = read_int("audio.bass_xo_hz", 120);
    if (xo != last_bass_xo_) { last_bass_xo_ = xo; send(CmdType::BASS_XO, xo); }
    const int32_t cap = read_int("audio.rms_cap_db10", 0);
    if (cap != last_rms_cap_) { last_rms_cap_ = cap; send(CmdType::RMS_CAP, cap); }
    const int32_t loud = read_int("audio.loudness", 70);
    if (loud != last_loud_) { last_loud_ = loud; send(CmdType::LOUDNESS, loud); bass_moved = true; }
    const int32_t ref = loud_ref();
    if (ref != last_loud_ref_) { last_loud_ref_ = ref; send(CmdType::LOUD_REF, ref); bass_moved = true; }
    const int32_t port = read_port();
    if (port != last_port_) { last_port_ = port; send(CmdType::PORT, port); bass_moved = true; }
    if (bass_moved) log_bass("changed");
    const int32_t tim = timbre();
    if (tim != last_timbre_) { last_timbre_ = tim; send(CmdType::TIMBRE, tim); }
    const int32_t bal = read_int("audio.balance_db10", 0);
    if (bal != last_balance_) { last_balance_ = bal; send(CmdType::BALANCE, bal); }
    leveler_schedule(dt_ms);
    const int32_t lev = read_bool("audio.leveler", false) ? 1 : 0;
    if (lev != last_leveler_) { last_leveler_ = lev; send(CmdType::LEVELER, lev); }
    if (read_bool("audio.port_meas", false) && !meter_busy_.load()) {
        meter_busy_.store(true);
        if (xTaskCreatePinnedToCore(meter_entry, "port_meter", 8192, this, 5, nullptr, 0) != pdPASS) {
            meter_busy_.store(false);
            state_set("audio.port_meas", false);
            state_set("audio.port_meas_status", "@cal.m_no_memory");
        }
    }
    {
        const int32_t mode = read_int("audio.tone_mode", TONE_CHAIN);
        const int32_t db10 = read_int("audio.tone_db10", -260);
        const int32_t ch   = read_int("audio.tone_ch", 3);
        if (mode != last_tone_mode_ || db10 != last_tone_db10_ || ch != last_tone_ch_) tone_touched = true;
        if (mode != last_tone_mode_) {
            last_tone_mode_ = mode;
            send(CmdType::TONE_MODE, mode);
            if (mode != TONE_CHAIN)
                ESP_LOGW(TAG, "calibration tone: %s, past the DSP chain at an exact DAC level",
                         mode == TONE_BURSTS ? "bursts" : "steady");
        }
        if (db10 != last_tone_db10_) { last_tone_db10_ = db10; send(CmdType::TONE_DB10, db10); }
        if (ch != last_tone_ch_)     { last_tone_ch_ = ch;     send(CmdType::TONE_CH, ch); }
        // Off by itself after TONE_RAW_MAX_MS of playing: the source goes off and the mode back.
        if (mode != TONE_CHAIN && src == SRC_TONE) {
            tone_raw_ms_ += dt_ms;
            if (tone_raw_ms_ >= TONE_RAW_MAX_MS) {
                tone_raw_ms_ = 0;
                ESP_LOGW(TAG, "calibration tone off after %lu s", (unsigned long)(TONE_RAW_MAX_MS / 1000));
                state_set("audio.tone_mode", (int32_t)TONE_CHAIN);
                state_set("audio.source", (int32_t)SRC_OFF);
            }
        } else {
            tone_raw_ms_ = 0;
        }
        // Through the chain: off after TONE_CHAIN_IDLE_MS with nothing about it changed.
        if (mode == TONE_CHAIN && src == SRC_TONE && !tone_touched) {
            tone_idle_ms_ += dt_ms;
            if (tone_idle_ms_ >= TONE_CHAIN_IDLE_MS) {
                tone_idle_ms_ = 0;
                ESP_LOGW(TAG, "test tone off after %lu s unchanged", (unsigned long)(TONE_CHAIN_IDLE_MS / 1000));
                state_set("audio.source", (int32_t)SRC_OFF);
            }
        } else {
            tone_idle_ms_ = 0;
        }
    }
    if (read_bool("audio.eq_reload", false)) {
        state_set("audio.eq_reload", false);
        send(CmdType::EQ_RELOAD, 0);
    }
    // The analyser is a 1024-point FFT every 512 frames, and nobody needs it while the
    // panel shows a screen without a spectrum row. ui.screen is the UI's own record of what
    // it is showing; "now" and the standby face draw the row ("radio" did once, and costs
    // nothing to keep). The standby face shows it only while music plays, and with nothing
    // playing the analyser is fed silence or not at all (source off) — not worth a second key.
    {
        char screen[16] = "";
        read_string("ui.screen", screen, sizeof(screen));
        const int want = (strcmp(screen, "now") == 0 || strcmp(screen, "radio") == 0 ||
                          strcmp(screen, "standby") == 0) ? 1 : 0;
        if (want != last_spectrum_) { last_spectrum_ = want; send(CmdType::SPECTRUM, want); }
    }

    report_ms_ += dt_ms;
    if (report_ms_ < 500) return;
    report_ms_ = 0;

    Status st;
    portENTER_CRITICAL(&mux_);
    st = status_;
    portEXIT_CRITICAL(&mux_);

    state_set("audio.level_l", to_dbfs(st.peak_l), false);
    state_set("audio.level_r", to_dbfs(st.peak_r), false);
    state_set("audio.in_l", to_dbfs(st.in_l), false);
    state_set("audio.in_r", to_dbfs(st.in_r), false);
    state_set("audio.cpu_pct", st.cpu_pct, false);
    state_set("audio.lim_gr", st.lim_gr_db, false);
    state_set("audio.dsp_pct", st.dsp_pct, false);
    state_set("audio.dsp_max_pct", st.dsp_max_pct, false);
    // The same numbers in the log while something plays, so the DSP's cost at each rate can be
    // read off the console without the API.
    if (st.running && st.source != SRC_OFF && st.dsp_pct > 0.05f) {
        dsp_log_ms_ += 500;
        dsp_log_sum_ += st.dsp_pct;
        dsp_log_n_++;
        if (st.dsp_max_pct > dsp_log_max_) dsp_log_max_ = st.dsp_max_pct;
        if (dsp_log_ms_ >= DSP_LOG_MS) {
            ESP_LOGI(TAG, "DSP load over %lu s: %.1f %% of a core on average, worst block %.0f %% of its time, at %.0f Hz; loudness +%.1f dB",
                     (unsigned long)(dsp_log_ms_ / 1000), (double)(dsp_log_sum_ / (float)dsp_log_n_),
                     (double)dsp_log_max_, (double)audio_core::fs_hz(), (double)st.loud_db);
            dsp_log_ms_ = dsp_log_n_ = 0;
            dsp_log_sum_ = dsp_log_max_ = 0.0f;
        }
    }
    if (st.out_clamps != last_out_clamps_) {
        ESP_LOGW(TAG, "output: %lu sample(s) clamped at full scale (%lu since boot) — past the "
                      "limiter, which should be impossible", (unsigned long)(st.out_clamps - last_out_clamps_),
                 (unsigned long)st.out_clamps);
        last_out_clamps_ = st.out_clamps;
        state_set("audio.out_clamps", (int32_t)st.out_clamps);
    }
    state_set("audio.bass_gr", st.bass_gr_db, false);
    if (st.bass_gr_db > 0.2f) ESP_LOGW(TAG, "bass limiter −%.1f dB", (double)st.bass_gr_db);
    if (st.eq_headroom_db != last_headroom_) {
        last_headroom_ = st.eq_headroom_db;
        state_set("audio.eq_headroom_db", st.eq_headroom_db);
    }
    {
        const float ld = roundf(st.loud_db * 10.0f) / 10.0f;    // a tenth of a dB is all a reader needs
        if (ld != last_loud_db_) {
            last_loud_db_ = ld;
            state_set("audio.loud_db", ld);
        }
    }
    {
        const float lv = roundf(st.lev_db * 10.0f) / 10.0f;
        if (lv != last_lev_db_) {
            last_lev_db_ = lv;
            state_set("audio.lev_db", lv);
        }
    }
    // Module init order is not guaranteed, so the "file" bit is re-evaluated here
    // rather than only once at startup.
    {
        int32_t srcs = read_int("audio.sources", 0);
        const int32_t with_ring = srcs | ring_sources_offered();
        if (audio_core::pcm_ring().capacity() > 0 && srcs != with_ring) {
            state_set("audio.sources", with_ring);
        }
        // The persisted source that init could not judge: now the ring exists, either it is
        // on offer and stays, or it is not in this build and goes the way it always did.
        if (pending_source_ >= 0 && audio_core::pcm_ring().capacity() > 0) {
            if (!(with_ring & (1 << pending_source_))) {
                ESP_LOGW(TAG, "source %s not available in this build — falling back to off",
                         source_name(pending_source_));
                state_set("audio.source", (int32_t)SRC_OFF);
            }
            pending_source_ = -1;
        }
    }
    if (st.source == SRC_BT) {
        // One line that separates the three things "it sounds bad" can mean: a source that
        // arrives quiet, a source that arrives mono, and a chain that spoils what arrived.
        // Everything from here down runs on the 500 ms gate above, not on dt_ms — adding the
        // raw tick here counted ten milliseconds per half second and would have taken four
        // minutes to reach five seconds.
        bt_log_ms_ += 500;
        if (bt_log_ms_ >= 5000) {
            bt_log_ms_ = 0;
            // Whole decibels and whole ppm, deliberately. Every %f here is a soft-float
            // conversion on this target and newlib's float formatting is the hungriest
            // thing this task does with its stack; a diagnostic has no business being the
            // most expensive call in the loop it diagnoses. Nothing below is worth a
            // tenth of a dB.
            // The output rate is in here because "is the clock following the source or is it
            // pinned" is a question that should be answerable by reading, not by asking.
            const unsigned bt_used = bt_ring_frames_
                ? (unsigned)((bt_head_ + bt_ring_frames_ - bt_tail_) % bt_ring_frames_) : 0u;
            // "clock" or "asrc" says which of the two is absorbing the drift, and the ppm
            // after it is that one's correction: the trim actually achieved when the clock
            // is following, the resampler's departure from its base ratio when it is not.
            // `asrc` counts the blocks that went through the converter since the format was
            // adopted — in steady clock-following it does not move, which is the stage's
            // definition of done stated as a number rather than as a feeling.
            ESP_LOGI(TAG, "BT signal: in %d/%d dBFS, width %d, out %d/%d, %lu Hz announced "
                          "(%d delivered) -> clock %lu Hz, %s %d ppm, asrc %lu, short %lu, "
                          "dropped %lu, primes %lu, resyncs %lu, ring %u%% = %u ms",
                     (int)to_dbfs(st.in_l), (int)to_dbfs(st.in_r), (int)to_dbfs(st.in_diff),
                     (int)to_dbfs(st.peak_l), (int)to_dbfs(st.peak_r),
                     (unsigned long)(uint32_t)read_int("bt.rate_hz", 0),
                     (int)st.in_rate_hz, (unsigned long)out_rate_,
                     st.clock_follow ? "clock" : "asrc",
                     st.clock_follow ? (int)st.clock_ppm : (int)st.asrc_ppm,
                     (unsigned long)st.bt_asrc_blocks, (unsigned long)bt_short_,
                     (unsigned long)bt_dropped_,
                     (unsigned long)bt_primes_, (unsigned long)bt_resyncs_,
                     (unsigned)(bt_ring_frames_ ? (100u * bt_used / (unsigned)bt_ring_frames_) : 0u),
                     (unsigned)((uint64_t)bt_used * 1000u / (bt_fmt_hz_ ? bt_fmt_hz_ : 48000u)));
            modesp::audio::IAudioPort* const rx_port = bt_rx_port();
            const auto rx_diag = rx_port ? rx_port->diagnostics() : modesp::audio::AudioPortDiagnostics{};
            const auto tx_diag = main_->diagnostics();
            const uint32_t low = bt_fill_low_.exchange(UINT32_MAX);
            const uint32_t high = bt_fill_high_.exchange(0);
            const bool steady = st.bt_active && last_bt_diag_active_ &&
                st.bt_primes == last_bt_diag_prime_ && st.bt_format_revision == last_bt_diag_format_;
            ESP_LOGI(TAG, "BT I2S totals (includes transitions): RXq %lu short %lu, "
                          "TXq %lu short %lu, trim applied/asked %lu/%lu, volume %ld mute %d",
                     (unsigned long)rx_diag.rx_queue_overflows, (unsigned long)rx_diag.rx_short_reads,
                     (unsigned long)tx_diag.tx_queue_overflows, (unsigned long)tx_diag.tx_short_writes,
                     (unsigned long)tx_diag.trim_applied, (unsigned long)tx_diag.trim_attempts,
                     (long)vol, (int)mute);
            ESP_LOGI(TAG, "BT I2S %s window: RXq +%lu, TXq +%lu, RX short +%lu, trim +%lu",
                     steady ? "steady" : "transition/idle",
                     (unsigned long)(rx_diag.rx_queue_overflows - last_bt_rx_diag_.rx_queue_overflows),
                     (unsigned long)(tx_diag.tx_queue_overflows - last_bt_tx_diag_.tx_queue_overflows),
                     (unsigned long)(rx_diag.rx_short_reads - last_bt_rx_diag_.rx_short_reads),
                     (unsigned long)(tx_diag.trim_applied - last_bt_tx_diag_.trim_applied));
            // Blocks the receive interrupt actually reported in this window, against what the
            // announced rate fills in five seconds. Fewer means completions were coalesced
            // and whole blocks lost before any counter above could see them.
            if (rx_diag.rx_dma_block_frames) {
                const uint32_t blocks = rx_diag.rx_dma_blocks - last_bt_rx_diag_.rx_dma_blocks;
                const uint32_t expect = bt_fmt_hz_ * 5u / rx_diag.rx_dma_block_frames;
                ESP_LOGI(TAG, "BT RX DMA: +%lu blocks of %lu frames (%lu expected in 5 s at %lu Hz); "
                              "hw: DMA RX FIFO overflow +%lu, DMA TX FIFO underflow +%lu, I2S RX hung +%lu",
                         (unsigned long)blocks, (unsigned long)rx_diag.rx_dma_block_frames,
                         (unsigned long)expect, (unsigned long)bt_fmt_hz_,
                         (unsigned long)(rx_diag.hw_rx_fifo_overflows - last_bt_rx_diag_.hw_rx_fifo_overflows),
                         (unsigned long)(rx_diag.hw_tx_fifo_underflows - last_bt_rx_diag_.hw_tx_fifo_underflows),
                         (unsigned long)(rx_diag.hw_rx_hung - last_bt_rx_diag_.hw_rx_hung));
            }
            last_bt_rx_diag_ = rx_diag; last_bt_tx_diag_ = tx_diag;
            last_bt_diag_active_ = st.bt_active;
            last_bt_diag_prime_ = st.bt_primes; last_bt_diag_format_ = st.bt_format_revision;
            ESP_LOGI(TAG, "BT window: RX max read gap %lu us, ring low/high %lu/%lu frames",
                     (unsigned long)bt_rx_gap_us_.exchange(0),
                     (unsigned long)(low == UINT32_MAX ? 0 : low), (unsigned long)high);
        }
        state_set("audio.in_rate", st.in_rate_hz, false);
        state_set("audio.asrc_ppm", st.asrc_ppm, false);
    }
    // Gain reduction, when there is any: the difference between "the limiter is shaping this"
    // and "something else is distorting" is otherwise inaudible in a log.
    if (st.lim_gr_db > 0.2f) ESP_LOGW(TAG, "limiter −%.1f dB", (double)st.lim_gr_db);

    // Everything the audio task measured, reported from here where blocking is free.
    if (st.amp_on != last_amp_on_) {
        last_amp_on_ = st.amp_on;
        ESP_LOGI(TAG, "amp %s", st.amp_on ? "ON" : "OFF");
    }
    if (st.startup_seq != last_startup_seq_) {
        last_startup_seq_ = st.startup_seq;
        for (int stage = 0; stage < 2; ++stage) {
            const auto& p = stage ? st.startup_out : st.startup_raw;
            for (int ch = 0; ch < 2; ++ch) {
                const auto& c = p.ch[ch];
                ESP_LOGI(TAG, "PCM start #%lu %s %c: %lu/%lu frames, peak %d step %d dBFS, "
                              "leading %lu, holes %lu, longest %lu, trailing %lu",
                         (unsigned long)st.startup_seq, stage ? "out" : "ring", ch ? 'R' : 'L',
                         (unsigned long)p.frames, (unsigned long)st.startup_rate,
                         (int)to_dbfs(c.peak), (int)to_dbfs(c.step),
                         (unsigned long)c.leading, (unsigned long)c.holes,
                         (unsigned long)c.longest, (unsigned long)c.run);
            }
        }
    }
    if (st.edge_seq != last_edge_seq_) {
        last_edge_seq_ = st.edge_seq;
        ESP_LOGI(TAG, "PCM edge #%lu: %lu Hz, %lu blocks, opens %lu, "
                      "step %d dBFS, boundary %d dBFS, closed %d dBFS",
                 (unsigned long)st.edge_seq, (unsigned long)st.edge_rate,
                 (unsigned long)st.edge_blocks, (unsigned long)st.edge_openings,
                 (int)to_dbfs(st.edge_step), (int)to_dbfs(st.edge_boundary),
                 (int)to_dbfs(st.edge_closed));
    }
    // Late means the TX DMA ran dry: a gap between writes longer than the DMA holds. That is
    // a number of frames (8 x 480), so in time it follows the rate — 80 ms at 48 kHz, 20 at
    // 192 — and a fixed 80 ms let every 20-80 ms starvation of a 192 kHz session through. A
    // gap of two 240-frame blocks per 480-frame descriptor is the normal cadence.
    {
        const uint32_t ring_frames = main_->port_caps().tx_queue_frames;
        const uint32_t ring_us = (st.out_rate && ring_frames)
            ? (uint32_t)((uint64_t)ring_frames * 1000000u / st.out_rate) : 80000u;
        if (st.late_us > ring_us) {
            ESP_LOGW(TAG, "audio task late: %lu us between blocks, the DMA holds %lu — it ran dry",
                     (unsigned long)st.late_us, (unsigned long)ring_us);
        }
    }
    // The same fault counted where it happens, whatever the rate and whatever the source: the
    // driver's ISR counts each DMA period that found no fresh buffer and sent an auto-cleared
    // one, exact zeros, to the DAC (on_send_q_ovf). The Bluetooth report below has always
    // printed it; every other source starved without a word.
    {
        const uint32_t ovf = main_->diagnostics().tx_queue_overflows;
        if (ovf != last_tx_ovf_) {
            ESP_LOGW(TAG, "TX DMA ran dry %lu time(s) — zeros went to the DAC (%lu since boot)",
                     (unsigned long)(ovf - last_tx_ovf_), (unsigned long)ovf);
            last_tx_ovf_ = ovf;
        }
    }
    if (st.dsp_faults != last_dsp_faults_) {
        ESP_LOGW(TAG, "DSP: %lu block(s) replaced by silence — a NaN or an infinity in the chain "
                      "(%lu since boot)", (unsigned long)(st.dsp_faults - last_dsp_faults_),
                 (unsigned long)st.dsp_faults);
        last_dsp_faults_ = st.dsp_faults;
        state_set("audio.dsp_faults", (int32_t)st.dsp_faults);
    }
    if (st.tx_short != last_tx_short_) {
        ESP_LOGW(TAG, "i2s refused %lu blocks in full",
                 (unsigned long)(st.tx_short - last_tx_short_));
        last_tx_short_ = st.tx_short;
    }
    if (st.underruns != last_underruns_) {
        // Logged, not just published: an underrun is the audible fault, and a counter that
        // only lives in SharedState cannot be correlated with anything else in a log.
        ESP_LOGW(TAG, "underruns %lu (+%lu)", (unsigned long)st.underruns,
                 (unsigned long)(st.underruns - last_underruns_));
        last_underruns_ = st.underruns;
        state_set("audio.underruns", (int32_t)st.underruns);
    }
    const char* s = st.running ? source_name(st.source) : "off";
    if (strcmp(s, last_state_) != 0) {
        strncpy(last_state_, s, sizeof(last_state_) - 1);
        state_set("audio.state", s);
    }
}

void AudioModule::on_stop() {
    if (task_) send(CmdType::STOP, 0);
    run_ = false;
}

// ═══════════════════════════════════════════════════════════════
// Audio task
// ═══════════════════════════════════════════════════════════════

void AudioModule::publish_consumer() {
    // What this consumer holds on either side of the ring, for a producer that paces itself
    // by the fill (pcm_ring.h, set_consumer): the cushion it waits for after a flush, and the
    // block, the limiter's look-ahead and the port's DMA between the ring and the pins.
    audio_core::pcm_ring().set_consumer((uint32_t)prime_frames_,
                                        main_->port_caps().tx_queue_frames + (uint32_t)BLOCK +
                                        dsp_.latency_frames());
}

void AudioModule::retime() {
    // The fades, the empty-block tail and the priming cushion are times; a block is a number of
    // frames. Counted in frames at 48 kHz, they shrank with the rate: at 192 kHz the envelope was
    // a 2.5 ms fade and the cushion 25 ms.
    const float fs = (float)(out_rate_ ? out_rate_ : 48000u);
    sig_up_   = audio_core::FadeGate::open_step(fs);
    sig_down_ = audio_core::FadeGate::close_step(fs);
    out_gate_.set_rate(fs);
    const size_t tail = (size_t)lroundf(SIG_FADE_OUT_S * fs);
    sig_fade_out_ = tail < 1 ? 1 : tail > BLOCK ? BLOCK : tail;
    prime_frames_ = (size_t)lroundf(PRIME_S * fs);
}

size_t AudioModule::fade_from_hold(float* stereo, float l, float r) {
    // Nothing arrived at all, mid-waveform. There is no sample to fade, so the last one there was
    // is brought to silence along the same curve. A decaying hold is not what the waveform was
    // going to do either — but this block used to step straight to zero from wherever the music
    // was, and the step is the click; a curve from the last sample is not.
    const size_t n = sig_fade_out_;
    for (size_t i = 0; i < n; i++) {
        // Smoothstep, not a straight line, which would turn a corner at each end.
        const float p = 1.0f - (float)i / (float)n;
        const float g = p * p * (3.0f - 2.0f * p);
        stereo[2 * i]     = l * g;
        stereo[2 * i + 1] = r * g;
    }
    memset(stereo + n * 2, 0, (BLOCK - n) * 2 * sizeof(float));
    return n;
}

void AudioModule::shape_burst(float* stereo, size_t frames) {
    // 250 ms on in every second, raised-cosine edges of 5 ms: a burst with no click of its own,
    // so whatever is heard on it is the amplifier.
    const uint32_t fs = out_rate_ ? out_rate_ : 48000u;
    const uint32_t on = fs / 4, edge = fs / 200, cycle = fs;
    for (size_t i = 0; i < frames; i++) {
        const uint32_t p = burst_pos_;
        float w = 0.0f;
        if (p < edge)            w = 0.5f - 0.5f * cosf((float)M_PI * (float)p / (float)edge);
        else if (p < on - edge)  w = 1.0f;
        else if (p < on)         w = 0.5f + 0.5f * cosf((float)M_PI * (float)(p - (on - edge)) / (float)edge);
        stereo[2 * i] *= w;
        stereo[2 * i + 1] *= w;
        if (++burst_pos_ >= cycle) burst_pos_ = 0;
    }
}

void AudioModule::set_amp_clip(int32_t db10) {
    if (db10 > 0) db10 = 0;
    if (db10 < -240) db10 = -240;
    out_scale_ = powf(10.0f, ceiling_dbfs((float)db10 / 10.0f) / 20.0f) * I32_SCALE;
}

void AudioModule::set_amp(bool on) {
    if (on == amp_on_) return;
    amp_on_ = on;
    if (amp_) amp_->set(on);
    // No log here: this runs in the audio task. on_update() reports the change instead.
}

void AudioModule::retune(uint32_t hz, uint16_t mclk_mult) {
    // 0 means "whatever the board configures", which is what every source but a wide-slot
    // Bluetooth stream wants. Resolve it once so the comparison below is against a number.
    const auto board = main_ ? main_->port_caps() : modesp::audio::AudioPortCaps{};
    if (!mclk_mult) mclk_mult = board.mclk_multiple;
    if (!hz || (hz == out_rate_ && mclk_mult == out_mclk_mult_)) return;
    // Over the ceiling (PLAY_MAX_HZ) the port is not even asked: the track converts to the
    // highest rate of its own family under it, by the path a refused rate takes below.
    const uint32_t top = board.max_rate_hz < PLAY_MAX_HZ ? board.max_rate_hz : PLAY_MAX_HZ;
    const bool     over = hz > top;
    if (!over && main_ && main_->set_rate(hz, mclk_mult)) {
        refused_rate_ = 0;
        adopt_clock(hz, mclk_mult);
        return;
    }
    // Over the ceiling, or the port would not take it — a slave clock, a rate the PLL cannot
    // make at this MCLK multiple, the input port holding the PLL, or a TX drain that did not
    // finish in time.
    // The producer has to convert after all, so the clock has to be somewhere it can
    // convert to, and that has to be said once. Where? The nearest rate the clock CAN make
    // inside the source's own family — 22.05 kHz goes to 44.1, not to 48 — because that
    // turns the ratio into a whole number and collapses the filter bank from 320 entries to
    // two; a rate refused inside the range goes to its family's base rate, which is the
    // one this board always makes.
    const uint32_t dst = audio_core::fallback_rate(hz, board.min_rate_hz, top);
    const bool     first = hz != refused_rate_;
    refused_rate_ = hz;
    if (first && over) {
        ESP_LOGI(TAG, "%lu Hz plays at %lu: the output stops at %lu",
                 (unsigned long)hz, (unsigned long)(dst ? dst : out_rate_), (unsigned long)top);
    } else if (first) {
        ESP_LOGW(TAG, "output cannot run at %lu Hz — the source must convert to %lu",
                 (unsigned long)hz, (unsigned long)(dst ? dst : out_rate_));
    }
    // ONE attempt at the fallback, and then the clock stays where it is.
    //
    // This used to call itself with the fallback, and fallback_rate() answers its own
    // result with the same number — so when the port refused the fallback too, as it does
    // for every rate while the input port holds the PLL, retune() called retune() without
    // end, on an 8 KB stack. The clock the output is at now is a rate the producer can
    // convert to like any other; the answer below says which one it is. The clock has to
    // actually BE at the answered rate before the converted frames arrive: leaving it
    // wherever the last track put it would resample 22.05 kHz up to 44.1 and then play the
    // result at 96, an octave out, at full volume — which is why the fallback is tried at
    // all rather than the current clock simply being answered.
    if (dst && dst != hz && (out_rate_ != dst || out_mclk_mult_ != board.mclk_multiple)) {
        if (main_ && main_->set_rate(dst, board.mclk_multiple)) {
            adopt_clock(dst, board.mclk_multiple);
        } else if (first) {
            ESP_LOGW(TAG, "output cannot run at %lu Hz either — staying at %lu",
                     (unsigned long)dst, (unsigned long)out_rate_);
        }
    }
    audio_core::pcm_ring().set_out_rate(out_rate_);
}

/// The port has taken `hz`: record it, answer the producer, and rebuild everything that
/// was designed against the old rate.
void AudioModule::adopt_clock(uint32_t hz, uint16_t mclk_mult) {
    out_rate_      = hz;
    out_mclk_mult_ = mclk_mult;
    // This is the session's clock now, and it will not move again until the source changes.
    // Recorded here rather than at the call sites because every route to a moved clock
    // arrives here, including the fallback one where the rate asked for was refused.
    session_rate_  = hz;
    audio_core::pcm_ring().set_clock_locked(true);
    // set_rate feeds keepalive while draining TX itself. That deliberate clock
    // transition is not a gap between writes that starved DMA in this task.
    last_block_us_ = 0;
    audio_core::pcm_ring().set_out_rate(hz);
    // Everything below was designed against the old rate and has to be designed again: a
    // biquad's coefficients are a frequency divided by the sample rate, and the spectrum's
    // band edges are bin numbers. Cheap — ten biquads and a table — and it happens in the
    // silence the envelope is already holding.
    audio_core::g_fs_hz = (float)hz;
    dsp_.set_sample_rate((float)hz);   // the limiter's and the volume ramp's time constants
    retime();                          // and this task's own: the fades and the cushion
    publish_consumer();                // the limiter's look-ahead is a number of frames: it moved
    {
        audio_core::EqBand bands[audio_core::MAX_BANDS];
        int count = 0;
        if (audio_core::eq_bus().read(bands, count) || load_eq_file(bands, count)) {
            dsp_.set_bands(bands, count);
        }
        dsp_.set_pre_gain_db(eq_trim_db_);
    }
    dsp_.reset();
    dsp_dirty_ = false;
    spectrum_.reset();
    tone_.refresh();            // its step was a frequency over the OLD rate
    priming_ = true;            // the ring restarts; wait for a cushion as at a track start
    {
        const int64_t until = esp_timer_get_time() + AMP_SETTLE_US;
        if (until > amp_settle_until_us_) amp_settle_until_us_ = until;
    }
    ESP_LOGI(TAG, "output retuned to %lu Hz", (unsigned long)hz);
}

void AudioModule::pause_bt_reader() {
    bt_capture_.store(false);
    // Source changes/stop only; format transitions wait without blocking below.
    // RX timeout is 10 ms, not the old 200 ms.
    while (bt_task_ && !bt_reader_paused_.load()) vTaskDelay(1);
}

void AudioModule::apply_source(int32_t src) {
    if (source_refused(src) || src == source_) return;
    ESP_LOGI(TAG, "source %s -> %s", source_name(source_), source_name(src));
    pause_bt_reader();
    sat::stream_control().invalidate_result();
    bt_fmt_revision_ = 0;
    source_ = src;
    // MCLK and the receive DMA serve only the line-in ADC; every other source leaves both off.
    main_->set_line_input(src == SRC_AUX);
    // And the satellite's clock lines go back to being inputs until Bluetooth adopts again.
    release_link();
    bt_fmt_master_ = false;
    dsp_.reset();          // fade in from silence, clear filter memory
    // Pause/flush belongs to the outgoing source. AUX and TONE have no producer
    // to reopen that gate, so every new source starts its own envelope state.
    gate_closing_ = false;
    sig_gain_ = 0.0f;
    last_in_l_ = last_in_r_ = 0.0f;
    priming_ = true;
    // A producer that was deselected releases the ring without flushing it, and up to half a
    // second of its stream would prime this source and play as if it were this one's. Both
    // gates are shut, so it goes now, unheard. The conditional reset refuses if the right
    // producer has claimed in between, and never touches a queue that is already this source's.
    if (is_ring_source(src)) {
        auto& ring = audio_core::pcm_ring();
        const auto was = ring.owner();
        const uint32_t gen = ring.generation();
        if (was != ring_owner(src)) ring.reset(was, gen);
    }
    // Whoever is about to play, the output clock goes back to its nominal rate. The servo
    // owns it only while Bluetooth is playing, and leaving it a hundred ppm out for the next
    // source would be leaving a number behind with nobody responsible for it.
    bt_clock_follow_ = false;
    reset_out_trim();
    // And a new source is a new listening session, which is the one moment the clock is
    // allowed to move. A rate the port refused during the session that just ended is not
    // evidence about the one starting, so that is forgotten with it.
    session_rate_ = 0;
    refused_rate_ = 0;
    audio_core::pcm_ring().set_clock_locked(false);
    if (src == SRC_BT) {
        bt_primed_ = false;
        bt_diag_stopped_ = false;
        bt_diag_start_us_ = 0;
        bt_tail_.store(bt_head_.load());   // drop stale input
        if (asrc_) resampleReset(asrc_);
        asrc_ratio_ = 1.0;
        bt_trim_dead_   = false;           // a fresh connection gets a fresh try at the clock
        bt_clock_unhealthy_ = bt_clock_recover_ = false;
        bt_clock_health_.reset();
        bt_asrc_blocks_ = 0;
    } else if (bt_) {
        // Leaving Bluetooth: hand the input channel back to the crystal.
        //
        // A high-resolution format puts the slave on the APLL, and a second holder freezes
        // that PLL's frequency — which is what stops the output retuning and sends card
        // playback back through the converter. Holding it while nobody is listening to
        // Bluetooth would make that cost permanent instead of paying it only while a
        // hi-res stream is actually playing.
        //
        // But park it at the width it is ALREADY at, which is the difference between
        // handing back a clock and handing back memory. IDF frees and reallocates the DMA
        // descriptors only when the buffer size changes, and that size depends on the slot
        // width alone (i2s_std.c:136, i2s_get_buf_size). Narrowing to 16 bits therefore
        // returns the wide buffers to a heap that, by the time Wi-Fi and a DLNA stream have
        // had their share, can no longer produce them again: the bench log reads
        // ESP_ERR_NO_MEM with 16875 bytes free and a largest block of 6144, and Bluetooth is
        // silent for the rest of the uptime. A clock can be given back. Contiguous DMA
        // memory on this chip cannot.
        //
        // The crystal will not clock 32-bit slots above about 39 kHz — 1.99 x MCLK against
        // 40 MHz — so a wide port parks at 32 kHz where a narrow one parks at 48. The
        // predicate that decides every other clock decision decides this one too.
        if (!park_bt_off_pll())
            ESP_LOGE(TAG, "BT input could not release its clock; it stays stopped");
        // Forgotten either way. If the port would not go back to the modest format, the one
        // thing that is certain is that we no longer know what it is configured for, and the
        // next announcement has to be adopted afresh rather than skipped as "already there".
        bt_fmt_hz_   = 0;
        bt_fmt_bits_ = 0;
        bt_fmt_ok_   = false;
    }
}

bool AudioModule::source_refused(int32_t src) {
    if (src == SRC_BT && !bt_) {
        ESP_LOGW(TAG, "BT source requested but no port_bt — staying %s", source_name(source_));
        return true;
    }
    if (src == SRC_AUX && !(main_->port_caps().has_rx && LINE_IN_FITTED)) {
        ESP_LOGW(TAG, "AUX requested but no line-in ADC is fitted — staying %s",
                 source_name(source_));
        return true;
    }
    if (src == SRC_AIRPLAY && !(ring_sources_offered() & (1 << SRC_AIRPLAY))) {
        ESP_LOGW(TAG, "AirPlay requested but this build has no receiver — staying %s",
                 source_name(source_));
        return true;
    }
    if (src == SRC_USB && !(ring_sources_offered() & (1 << SRC_USB))) {
        ESP_LOGW(TAG, "USB requested but the USB audio device is not running — staying %s",
                 source_name(source_));
        return true;
    }
    return false;
}

/// Take on the format the satellite announced. The ORDER is the whole content of this
/// function, and getting it wrong is silent.
///
/// A slave port cannot move the audio PLL — the master owns it, and while both hold it the
/// frequency is frozen. So for a format the crystal cannot clock, two things have to be true
/// before the input is asked for it: the PLL must already be fast enough, and the input must
/// not be holding it while the master changes it. Hence:
///
///   1. put the input back on the crystal, releasing the PLL if it had it;
///   2. set the output clock the plan asks for — the same rate as the stream, at whatever
///      MCLK multiple puts the PLL where the input needs it;
///   3. move the input to the announced format, which now fits.
///
/// For 16-bit slots — the native SBC and AAC output — none of this changes anything:
/// the crystal carries them, step 1 is a no-op and step 2 is the output following the source,
/// which is what it always did. 32-bit slots are paid for by running the master's MCLK at 512x
/// instead of 256x, which doubles the PLL and costs nothing else on this board: the DAC takes
/// no MCLK at all (its own PLL locks to the bit clock) and there is no ADC fitted. The output
/// still runs at the stream's rate, so the converter's ratio stays 1.0.
/// Take the input port off the audio PLL without giving its DMA memory back.
///
/// Parking means moving the slave onto the crystal, and the crystal will not clock 32-bit
/// slots above about 39 kHz — IDF wants a source above 1.99 x MCLK and MCLK is the rate
/// times 512. So a wide port parks at 32 kHz where a narrow one parks at 48.
///
/// What it must NOT do is narrow the slots to get there. i2s_std.c:136 frees and reallocates
/// the DMA descriptors whenever the buffer size changes, and i2s_get_buf_size derives that
/// size from the slot width alone. Narrowing therefore returns the wide buffers to a heap
/// that, once Wi-Fi and a stream have had their share, cannot produce them again — measured
/// at 16875 B free with a largest block of 6144, after which Bluetooth is silent until the
/// next boot. A clock can be handed back; contiguous DMA memory on this chip cannot.
///
/// The port is left stopped. Both callers start it again themselves, or deliberately do not.
void AudioModule::reset_out_trim() {
    // Not conditional on bt_clock_follow_: the trim outlives the mode that set it, because a
    // retune that does not happen — the next source asking for the rate the clock is already
    // at — leaves it exactly where the servo put it. A hundred ppm is inaudible; a hundred
    // ppm that nobody owns is a number that will be wrong later for a reason nobody can find.
    if (main_ && bt_trim_ppm_) main_->trim_ppm(0, nullptr);
    bt_trim_ppm_ = bt_trim_got_ = 0;
    bt_err_sum_ = 0;
    bt_err_n_ = 0;
    bt_servo_frames_ = 0;
}

void AudioModule::steer_out_clock(int32_t err_frames, uint32_t stream_hz) {
#if MODESP_AUDIO_DIAGNOSTIC_FIXED_TRIM || MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT || MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO || MODESP_AUDIO_BT_DIRECT_CONTINUOUS
    // Independent of constant PCM: nominal format changes still run normally.
    return;
#endif
    // Proportional, over the same band the resampler's tracking authority used: inside 40 ms
    // of queue the clock is enough on its own, and the two loops therefore describe the same
    // error rather than two different ones.
    const int32_t want = audio_core::i2s_trim_ppm(err_frames, stream_hz, BT_TRACK_MS);
    const int32_t move = want > bt_trim_ppm_ ? want - bt_trim_ppm_ : bt_trim_ppm_ - want;
    if (move < BT_TRIM_HYST_PPM) return;        // nothing worth writing; the PLL is left alone
    int32_t got = bt_trim_got_;
    if (!main_ || !main_->trim_ppm(want, &got)) {
        // The port said no — the PLL is held by someone else, or the rate cannot be tuned at
        // this multiple. Do not keep asking: mark the path dead and let the next pre-buffer,
        // which happens in silence, hand the stream back to the resampler.
        bt_trim_dead_ = true;
        bt_clock_recover_ = true;
        return;
    }
    bt_trim_ppm_ = want;
    bt_trim_got_ = got;
}

bool AudioModule::park_bt_off_pll() {
    if (!bt_) return false;
    const uint8_t bits = bt_->slot_bits();
    uint32_t hz = 48000;
    if (!audio_core::i2s_slave_clock_fits(audio_core::I2S_XTAL_HZ, hz, bits)) hz = 32000;
    bt_->stop();
    return bt_->set_format(hz, bits);
}

bool AudioModule::adopt_bt_format(uint32_t hz, uint8_t bits) {
    if (!bt_ || !hz || (bits != 16 && bits != 32)) return false;

    // A new format is a new clock, so whatever the servo had done to the old one is not a
    // trim of anything any more. Put it back on nominal BEFORE the retune rather than relying
    // on the retune to do it: step 2 below is a no-op when the rate is not changing, and a
    // no-op leaves the trim exactly where the last stream left it.
    reset_out_trim();

    const auto caps = main_ ? main_->port_caps() : modesp::audio::AudioPortCaps{};
    const audio_core::SlaveClockPlan plan =
        audio_core::i2s_plan_for_slave(hz, bits, caps.mclk_multiple, caps.max_rate_hz);
    if (!plan.out_rate_hz) {
        ESP_LOGE(TAG, "BT: %lu Hz in %u-bit slots cannot be clocked on this board — no MCLK "
                      "multiple puts the PLL above %lu Hz", (unsigned long)hz, bits,
                 (unsigned long)(audio_core::i2s_slave_mclk_hz(hz, bits) / 100ull * 199ull));
        return false;
    }

    // Step 1. Park off the PLL before retuning. At 96 kHz even 16-bit RX uses
    // the APLL; slot width alone cannot tell whether the slave holds it.
    // set_format skips an unchanged format, so a parked port remains cheap — and the park
    // keeps the width, so it stays cheap in DMA memory too. Parking at 16 bits here ran the
    // free/reallocate cycle on EVERY announcement, not just on a source change.
    if (!park_bt_off_pll()) return false;

    // Step 2.
    retune(plan.out_rate_hz, plan.mclk_multiple);
    if (out_rate_ != plan.out_rate_hz || out_mclk_mult_ != plan.mclk_multiple) {
        ESP_LOGE(TAG, "BT: %lu Hz in %u-bit slots needs the output at %lu Hz with MCLK x%u, "
                      "and it is at %lu with x%u", (unsigned long)hz, bits,
                 (unsigned long)plan.out_rate_hz, (unsigned)plan.mclk_multiple,
                 (unsigned long)out_rate_, (unsigned)out_mclk_mult_);
        return false;
    }

    // Step 3. The driver says why if it will not.
    if (!bt_->set_format(hz, bits) || !bt_->start()) return false;

    bt_primed_ = false;
    bt_tail_.store(bt_head_.load());      // the previous connection's samples are stale
    return true;
}

void AudioModule::release_link() {
    if (!link_master_.load()) return;
    if (main_) main_->set_link_master(nullptr);
    link_master_.store(false);
}

bool AudioModule::adopt_bt_format_master(uint32_t hz, uint8_t bits) {
    if (!main_ || !link_pins_.valid() || !hz) return false;
    // 1.9 promises 32-bit slots: the width the main port already runs its DAC at, so its
    // receive side needs no reconfiguration and its DMA no reallocation.
    if (bits != 32) {
        ESP_LOGW(TAG, "BT: %u-bit slots announced while we clock the link; protocol 1.9 "
                      "says 32 — not adopted", (unsigned)bits);
        return false;
    }
    reset_out_trim();
    // The slave input port lets go of the PLL and stops: its pins are about to carry our
    // own clocks, and reading them back would only waste a DMA channel.
    if (bt_) { park_bt_off_pll(); bt_->stop(); }
    // The output at the stream's own rate, on the board's MCLK multiple. No slave margin to
    // buy any more: nothing on this side samples the satellite's clock.
    retune(hz, 0);
    if (out_rate_ != hz) {
        ESP_LOGE(TAG, "BT: the output will not run at %lu Hz, so the link cannot either",
                 (unsigned long)hz);
        return false;
    }
    if (!main_->set_link_master(&link_pins_)) {
        ESP_LOGE(TAG, "BT: port_main cannot clock the link");
        return false;
    }
    link_master_.store(true);
    bt_primed_ = false;
    bt_tail_.store(bt_head_.load());      // whatever the old port left behind is stale
    ESP_LOGW(TAG, "BT path: we clock the link at %lu Hz / 32-bit — in and out on one clock, "
                  "nothing to resample, nothing to trim", (unsigned long)hz);
    return true;
}

/// Pull one output block from the BT ring through the ASRC. Steers the ratio by the ring
/// fill, PLL-like, with the authority blending from +-500 ppm while tracking to +-2 % while
/// recovering. Returns false -> caller outputs silence.
///
/// Runs in the audio task against a 5 ms deadline, so nothing here logs: faults are counted
/// and on_update prints them. A soft-float ESP_LOG on the underrun path feeds the underrun.
bool AudioModule::stop_bt_diagnostic(uint8_t reason) {
    // Leave the existing caller's last-sample fade and DAC servicing in place.
    // Never silently re-prime or switch this experiment to adaptive ASRC.
    bt_diag_stopped_ = true;
    bt_path_ = reason;
    bt_primed_ = false;
    bt_capture_.store(false);
    return false;
}

bool AudioModule::pull_bt_block(float* stereo, size_t frames) {
    // Everything here is measured in the stream's own frames, so the set point is the same
    // number of milliseconds whatever the rate. Before a format is adopted nothing is
    // pulled, so the guess only sizes the pre-buffer.
    const uint32_t hz     = bt_fmt_hz_ ? bt_fmt_hz_ : 48000;
    size_t         used   = (bt_head_ + bt_ring_frames_ - bt_tail_) % bt_ring_frames_;
    if (MODESP_AUDIO_BT_DIRECT_CONTINUOUS && (!bt_fmt_hz_ || out_rate_ != bt_fmt_hz_)) {
        // A format mismatch waits for the normal format handshake. Never substitute ASRC.
        bt_path_ = BT_CONTINUOUS_BAD_RATE;
        bt_primed_ = false;
        return false;
    }
    if (BT_DIAGNOSTIC) {
        if (bt_diag_stopped_) return false;
        if (!bt_fmt_hz_ || out_rate_ != bt_fmt_hz_)
            return stop_bt_diagnostic(BT_DIAG_BAD_RATE);
        if (bt_diag_start_us_) {
            const auto rx = bt_->diagnostics();
            const auto tx = main_->diagnostics();
            if (bt_dropped_ != bt_diag_dropped_) return stop_bt_diagnostic(BT_DIAG_DROP);
            if (rx.rx_queue_overflows != bt_diag_rx_.rx_queue_overflows)
                return stop_bt_diagnostic(BT_DIAG_RX_LOSS);
            if (tx.tx_queue_overflows != bt_diag_tx_.tx_queue_overflows ||
                tx.tx_short_writes != bt_diag_tx_.tx_short_writes)
                return stop_bt_diagnostic(BT_DIAG_TX_LOSS);
            if (tx.trim_attempts != bt_diag_tx_.trim_attempts || bt_trim_got_ || bt_trim_ppm_)
                return stop_bt_diagnostic(BT_DIAG_TRIM);
            if (esp_timer_get_time() - bt_diag_start_us_ >= 60000000)
                return stop_bt_diagnostic(BT_DIAG_COMPLETE);
        }
    }
    uint32_t low = bt_fill_low_.load(std::memory_order_relaxed);
    while (used < low && !bt_fill_low_.compare_exchange_weak(low, (uint32_t)used)) {}
    uint32_t high = bt_fill_high_.load(std::memory_order_relaxed);
    while (used > high && !bt_fill_high_.compare_exchange_weak(high, (uint32_t)used)) {}
    const size_t   target = (size_t)((uint64_t)hz * BT_TARGET_MS / 1000u);
    size_t         resync = (size_t)((uint64_t)hz * BT_RESYNC_MS / 1000u);
    if (resync > bt_ring_frames_ * 9 / 10) resync = bt_ring_frames_ * 9 / 10;   // at 96 kHz the ring is half a second

    // Past this no rate the converter is allowed to run at will catch up before the music
    // ends — 380 ms of excess at +-2 % is nineteen seconds of pitch shift — and not far
    // beyond it the writer starts throwing frames away every block. Jump the tail to the
    // set point: one discontinuity, and then a ring with room in it again.
    if (used > resync) {
        if (BT_DIAGNOSTIC) return stop_bt_diagnostic(BT_DIAG_RESYNC);
        bt_tail_ = (bt_head_ + bt_ring_frames_ - target) % bt_ring_frames_;
        used = target;
        bt_resyncs_++;
    }
    // The rate the satellite ANNOUNCED, which is what both paths are measured against: the
    // one the clock is asked to follow, and the one the resampler's base ratio is built from.
    const uint32_t announced = bt_fmt_hz_; // the format actually adopted by this task
    // A fresh announcement re-seeds the measurement rather than fighting it. The estimator
    // starts at 48000 and needs seconds to walk to the truth; leaving it there meant the
    // announced and measured rates disagreed by 8.8 % for that whole time, and every reader
    // of the measurement — the ppm readout, any disagreement check — reported the gap as if
    // it were drift.
    if (announced && announced != bt_seeded_hz_) {
        bt_seeded_hz_ = announced;
        in_rate_est_  = (float)announced;
    }

    if (!bt_primed_) {
        // An underrun can refill the ring while the old envelope is still closing.
        // Keep its clock until silence even if the pre-buffer is already full.
        if (bt_clock_recover_ && sig_gain_ > 0.0f) return false;
        if (used < target) return false;   // pre-buffer to the set point
        bt_primed_ = true;
        bt_primes_++;

        // The one place the path is chosen, and it is chosen here because here is the only
        // moment that is BOTH silent and about to start: the block above has been returning
        // false — silence, with the envelope shut — for as long as it took the ring to reach
        // the set point, and the first sample of the new run has not been produced yet.
        //
        // Three things have to be true to ride the clock. The stream's rate has to be the
        // one the output is already clocked at, or riding the clock would be riding it to
        // the wrong place; the port has to be able to tune at all, which on this chip
        // depends on who is holding the audio PLL at this instant and therefore on the
        // format being played; and no earlier trim in this stream may have been refused,
        // because a servo that has been told no once will be told no again.
        //
        // Everything else — a rate the clock could not follow, a wide format that pins the
        // PLL, a port that will not tune — falls through to the resampler, which is what it
        // has always done.
        const bool was_following = bt_clock_follow_;
        // And a fourth thing, decided by the listener rather than the hardware: whether
        // riding the clock is allowed at all (audio.bt_clock_follow). The warning at
        // BT_TRIM_HYST_PPM came true on the bench on 23.09.2026: sporadic clicks, single or
        // in pairs, with the Bluetooth source on, playing or not. A trim is not a divider
        // write. rtc_clk_apll_coeff_set writes I2C_APLL_SDM_STOP and recalibrates the VCO
        // (esp32p4 rtc_clk.c, clk_tree_ll.h), and the PCM5102A, locking its own PLL to
        // BCK, meets a disturbed BCK as a clock error: "switch to an internal oscillator
        // ... the outputs are hard muted to ground", then a restart (SLAS859C 11.2).
        // Muting the data cannot hide that.
        //
        // Nor does the loop settle the way the window arithmetic above assumed. In
        // clock-following mode the reader's 240-frame reads and this task's 240-frame
        // blocks run at the same rate by construction, so the ring is sampled at a fixed
        // phase of its own sawtooth: the 250 ms mean does not average that away, it
        // freezes it, and when the phase drifts across a block edge the mean steps by up
        // to a block, several ppm of command, well past the hysteresis. The captures of
        // 22.09.2026 show the achieved trim walking -13, -15, -17, -24, -26, -27, -29 over
        // four minutes of one stream and -55 to -68 in under a minute of another, at
        // least one rewrite per step; with the satellite sending silence the loop ran
        // just the same. Not every click is a trim, though: a 150 s capture the same day
        // with the source NOT Bluetooth, servo idle, bus static, still clicked. So the
        // setting is the A/B that says whether the trims are audible on this DAC, and
        // the servo stays in the tree for a trim it cannot hear, if one exists.
#if MODESP_AUDIO_BT_DIRECT_CONTINUOUS
        bt_clock_follow_ = false;
        bt_path_ = BT_CONTINUOUS_DIRECT;
#elif MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT || MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO
        // The persisted clock-follow preference must not change the experiment.
        bt_clock_follow_ = false;
        bt_path_ = MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT ? BT_DIAG_DIRECT : BT_DIAG_FIXED_RATIO;
        bt_diag_start_us_ = esp_timer_get_time();
        bt_diag_dropped_ = bt_dropped_;
        bt_diag_rx_ = bt_->diagnostics();
        bt_diag_tx_ = main_->diagnostics();
#else
        bt_clock_follow_ = bt_clock_allowed_ && announced && out_rate_ == announced
                           && !bt_trim_dead_ && !bt_clock_unhealthy_ && main_ && main_->can_trim();
        bt_path_ = bt_clock_follow_ ? 1
                 : !bt_clock_allowed_ ? 2
                 : !(announced && out_rate_ == announced) ? 3
                 : bt_trim_dead_ ? 4 : bt_clock_unhealthy_ ? 6 : 5;
#endif
        bt_err_sum_ = 0;
        bt_err_n_   = 0;
        bt_servo_frames_ = 0;
        if (!bt_clock_follow_) {
            // Back to the resampler with its filter memory cleared: what is in it belongs to
            // samples that were played a pre-buffer ago.
            if (was_following) reset_out_trim();
            if (asrc_) resampleReset(asrc_);
        }
    }

    if (bt_clock_follow_ || MODESP_AUDIO_DIAGNOSTIC_BT_DIRECT || MODESP_AUDIO_BT_DIRECT_CONTINUOUS) {
        // The bit-exact path. No filter, no phase, no ratio — the samples the satellite sent
        // are the samples the DAC gets, and the difference between the two crystals is taken
        // out of the clock instead of out of the waveform.
        if (used < frames) {
            if (BT_DIAGNOSTIC) return stop_bt_diagnostic(BT_DIAG_UNDERRUN);
            bt_primed_ = false;            // underrun: re-buffer, and re-decide the path
            if (bt_clock_follow_ && bt_clock_health_.underrun()) {
                bt_clock_unhealthy_ = true;
                bt_clock_recover_ = true;
            }
            return false;
        }
        size_t tail = bt_tail_;
        for (size_t i = 0; i < frames; i++) {
            stereo[2 * i]     = bt_ring_[2 * tail];
            stereo[2 * i + 1] = bt_ring_[2 * tail + 1];
            if (++tail == bt_ring_frames_) tail = 0;
        }
        bt_tail_ = tail;
        // The servo reads the queue this block consumed from, so the error it steers against
        // is the one that actually existed while these samples were chosen.
        if (bt_clock_follow_) {
        bt_err_sum_ += (int32_t)used - (int32_t)target;
        ++bt_err_n_;
        bt_servo_frames_ += static_cast<uint32_t>(frames);
        if (bt_servo_frames_ >= (uint64_t)hz * BT_SERVO_MS / 1000u) {
            steer_out_clock(bt_err_sum_ / (int32_t)bt_err_n_, hz);
            if (bt_clock_health_.observe(static_cast<uint32_t>(used), hz, bt_trim_got_,
                                         bt_servo_frames_ * 1000u / hz)) {
                bt_clock_unhealthy_ = true;
                bt_clock_recover_ = true;
            }
            bt_err_sum_ = 0;
            bt_err_n_   = 0;
            bt_servo_frames_ = 0;
        }
        }
        // Nothing here has a ratio, and reporting one would be reporting the resampler's
        // last opinion for the rest of the stream.
        asrc_ratio_ = asrc_base_ = 1.0;
        return true;
    }
    bt_asrc_blocks_++;

    // Base ratio from the rate the satellite ANNOUNCED, falling back to the measured one
    // only while nothing has been announced. The measurement carries its own noise, and
    // every bit of that noise used to arrive as pitch: the servo below cannot tell a wrong
    // base from a drifting clock, so it steers against both.
#if MODESP_AUDIO_DIAGNOSTIC_BT_FIXED_RATIO
    const double base = 1.0, ratio = 1.0;
#else
    const double in_hz = announced ? (double)announced : (double)in_rate_est_;
    const double base = (double)out_rate_ / in_hz;
    // The error in SECONDS of queue, so the loop gain is the same at every rate: full
    // authority half a second out, as it was when the ring was a second long at 48 kHz and
    // the error was measured as a fraction of it.
    const double err  = ((double)used - (double)target) / (double)hz;

    const double a   = err < 0.0 ? -err : err;
    constexpr double TRACK_S   = BT_TRACK_MS / 1000.0;
    constexpr double CAPTURE_S = BT_CAPTURE_MS / 1000.0;
    double       dev = ASRC_MAX_DEV;
    if (a > TRACK_S) {
        const double t = (a - TRACK_S) / (CAPTURE_S - TRACK_S);
        dev = ASRC_MAX_DEV + (ASRC_CAPTURE - ASRC_MAX_DEV) * (t > 1.0 ? 1.0 : t);
    }
    double ratio = base * (1.0 - err * (dev / 0.5));   // ring too full → resample slower (consume more)
    const double lo = base * (1.0 - dev), hi = base * (1.0 + dev);
    if (ratio < lo) ratio = lo; else if (ratio > hi) ratio = hi;
#endif
    asrc_ratio_ = ratio;
    asrc_base_  = base;

    // resampleGetRequiredSamples gives the MINIMUM, and a minimum that is right on the line
    // comes up one frame short whenever the rounding tips over. A block short by even one
    // frame used to be padded with zeros below, which is a step to silence and back — a
    // click. Two frames of headroom cost 45 microseconds of latency and remove the whole
    // class of them.
    size_t need = resampleGetRequiredSamples(asrc_, (int)frames, ratio) + 2;
    if (need > sizeof(s_asrc_in) / (2 * sizeof(float))) need = sizeof(s_asrc_in) / (2 * sizeof(float));
    if (used < need) {
        if (BT_DIAGNOSTIC) return stop_bt_diagnostic(BT_DIAG_UNDERRUN);
        bt_primed_ = false;   // underrun: re-buffer
        return false;
    }
    // Copy `need` frames out of the ring (linearize)
    size_t tail = bt_tail_;
    for (size_t i = 0; i < need; i++) {
        s_asrc_in[2 * i]     = bt_ring_[2 * tail];
        s_asrc_in[2 * i + 1] = bt_ring_[2 * tail + 1];
        if (++tail == bt_ring_frames_) tail = 0;
    }
    ResampleResult r = resampleProcessInterleaved(asrc_, s_asrc_in, (int)need, stereo, (int)frames, ratio);
    bt_tail_ = (bt_tail_ + r.input_used) % bt_ring_frames_;
    if ((size_t)r.output_generated < frames) {
        if (BT_DIAGNOSTIC) {
            bt_short_++;
            return stop_bt_diagnostic(BT_DIAG_SHORT);
        }
        // Still short. Hold the last sample rather than writing zeros: a flat spot of a
        // frame or two is inaudible, a step to digital silence and back is the click. The
        // count is kept because a shortfall that keeps happening is a ratio problem, not a
        // rounding one, and the two need different work.
        const float l = r.output_generated ? stereo[(r.output_generated - 1) * 2]     : 0.0f;
        const float rr = r.output_generated ? stereo[(r.output_generated - 1) * 2 + 1] : 0.0f;
        for (size_t i = (size_t)r.output_generated; i < frames; i++) {
            stereo[2 * i]     = l;
            stereo[2 * i + 1] = rr;
        }
        bt_short_++;   // reported by on_update; this task does not log
    }
    return true;
}

namespace {
/// Proof of progress for WatchdogService, bumped once per 5 ms block. Separate from the
/// task watchdog above it: this one only logs, and it is what names `audio` as the stuck
/// task instead of leaving the panic to say `main`.
modesp::liveness::Counter s_audio_beat{0};
}  // namespace

void AudioModule::task_loop() {
    ESP_LOGI(TAG, "audio task on core %d", (int)xPortGetCoreID());
    if (!main_->start()) {
        ESP_LOGE(TAG, "port_main start failed — audio task exits");
        portENTER_CRITICAL(&mux_); status_.running = false; portEXIT_CRITICAL(&mux_);
        return;
    }
    if (bt_ && !bt_->start()) {
        ESP_LOGW(TAG, "port_bt start failed — BT source unavailable");
        bt_ = nullptr;
    }
    main_->set_line_input(source_ == SRC_AUX);
    // The clocks run from here on; the converter gets its boot settle from this moment.
    amp_settle_until_us_ = esp_timer_get_time() + AMP_SETTLE_BOOT_US;

#if MODESP_AUDIO_DIAGNOSTIC_CONSTANT
    ESP_LOGW(TAG, "DIAGNOSTIC CONSTANT OUTPUT: DAC word 256, initial clock %lu Hz; music is disabled",
             (unsigned long)out_rate_);
#endif
    if (BT_DIAGNOSTIC)
        ESP_LOGW(TAG, "BT diagnostic armed: fixed P4 trim, equal rates required; "
                      "stop listening at first satellite rebuffer/zero insertion too");
    if (MODESP_AUDIO_BT_DIRECT_CONTINUOUS)
        ESP_LOGW(TAG, "BT continuous direct PCM enabled: no time limit, ASRC bypassed, "
                      "P4 live trim disabled, automatic rebuffer");
    uint32_t underruns = 0;
    float    win_peak_l = 0.0f, win_peak_r = 0.0f;   // peak hold over the status window
    float    win_in_l = 0.0f, win_in_r = 0.0f;       // same, but measured at the source
    float    win_in_d = 0.0f;                       // peak |L-R| at the source
    int64_t  busy_us = 0, window_start = esp_timer_get_time();
    uint64_t dsp_cycles = 0;
    uint32_t dsp_cycles_max = 0, out_clamps = 0;
    uint32_t blocks = 0;
    uint32_t edge_seq = 0, edge_blocks = 0, edge_openings = 0;
    float edge_step = 0, edge_boundary = 0, edge_closed = 0;
    audio_core::StartupProbe startup_raw{}, startup_out{};
    uint32_t startup_left = 0, startup_rate = 0, startup_seq = 0;
    bool     stop = false;

    // ── watchdog, from the task that must never stall ──
    //
    // A block is 5 ms of audio and the I2S write below waits at most IO_TIMEOUT (200 ms),
    // so a loop that has not come round in five seconds is not slow, it is stuck — and
    // until now a stuck audio task was simply silence, with the board otherwise healthy
    // and nothing in the log. Subscribing here rather than in on_init() is deliberate:
    // esp_task_wdt_add(nullptr) subscribes the CALLING task, and this is it.
    const bool wdt = (esp_task_wdt_add(nullptr) == ESP_OK);
    if (!wdt) ESP_LOGW(TAG, "audio task not subscribed to the task watchdog");
    modesp::liveness::watch("audio", &s_audio_beat, 3000);

    while (run_ && !stop) {
        if (wdt) esp_task_wdt_reset();
        modesp::liveness::tick(s_audio_beat);

        // ── commands ──
        // Their cycles count with the DSP's: an EQ edit redesigns ten bands in double, which is
        // soft-float here, and that lands in this block's time.
        const uint32_t cmd_c0 = esp_cpu_get_cycle_count();
        {
            int32_t pct = 0, limit = 0;
            if (level_.take(pct, limit)) dsp_.set_volume(audio_core::LevelLatch::effective(pct, limit));
        }
        Cmd c;
        while (xQueueReceive(cmd_q_, &c, 0) == pdTRUE) {
            switch (c.type) {
            case CmdType::SOURCE:
                // Not switched here but through the gates, below. Asking again for the source that
                // is playing withdraws a change still waiting, and the gates simply reopen.
                if (c.value == source_) pend_source_ = -1;
                else if (!source_refused(c.value)) pend_source_ = c.value;
                break;
            case CmdType::MUTE:      muted_ = (c.value != 0); dsp_.set_mute(muted_); break;
            case CmdType::EQ_ENABLE: dsp_.set_eq_enabled(c.value != 0); break;
            case CmdType::TONE_HZ:   tone_.set_freq((float)c.value); break;
            case CmdType::WIDTH:     dsp_.set_width((float)c.value / 100.0f); break;
            case CmdType::MONO_BASS: dsp_.set_mono_bass((float)c.value); break;
            case CmdType::SPECTRUM:  spectrum_wanted_ = (c.value != 0); break;
            case CmdType::BT_CLOCK:  bt_clock_allowed_ = (c.value != 0); break;
            case CmdType::AMP_CLIP:  set_amp_clip(c.value); break;
            case CmdType::DRIVE:     dsp_.set_drive_db((float)c.value); break;
            case CmdType::SUBSONIC:  dsp_.set_subsonic((float)c.value); break;
            case CmdType::BASS_XO:
                dsp_.set_bass_xo((float)c.value);
                publish_consumer();             // its look-ahead joins or leaves the pipeline
                break;
            case CmdType::RMS_CAP:   dsp_.set_rms_cap_db((float)c.value / 10.0f); break;
            case CmdType::LOUDNESS:  dsp_.set_loudness((float)c.value / 100.0f); break;
            case CmdType::LOUD_REF:  dsp_.set_loud_ref((float)c.value); break;
            case CmdType::PORT:      dsp_.set_port((float)(c.value >> 4), (float)(c.value & 15)); break;
            case CmdType::TIMBRE:    dsp_.set_tone((float)(int8_t)(c.value >> 8), (float)(int8_t)c.value); break;
            case CmdType::BALANCE:   dsp_.set_balance_db((float)c.value / 10.0f); break;
            case CmdType::LEVELER:   dsp_.set_leveler(c.value != 0); break;
            case CmdType::TONE_MODE: {
                const int32_t m = (c.value == TONE_RAW || c.value == TONE_BURSTS) ? c.value : TONE_CHAIN;
                // Through the gate: shut, change, open. Switching in the middle of a tone is a
                // step in level and, between the chain and the raw path, in phase too.
                if (source_ != SRC_TONE) {               // nothing playing it: just take it
                    tone_mode_ = pend_tone_mode_ = m;
                    tone_switching_ = false;
                    burst_pos_ = 0;
                } else if (m != (tone_switching_ ? pend_tone_mode_ : tone_mode_)) {
                    pend_tone_mode_ = m;
                    tone_switching_ = true;
                }
                break;
            }
            case CmdType::TONE_DB10: {
                const int32_t d = c.value > 0 ? 0 : c.value < -600 ? -600 : c.value;
                tone_amp_ = powf(10.0f, (float)d / 200.0f);   // tenths of a dB → linear
                break;
            }
            case CmdType::TONE_CH:   tone_mask_ = (c.value >= 1 && c.value <= 3) ? c.value : 3; break;
            case CmdType::EQ_RELOAD: {
                audio_core::EqBand bands[audio_core::MAX_BANDS];
                int count = 0;
                // From memory once the UI has published a set; from the file only when
                // nothing has been edited yet this boot. Never a filesystem read here.
                const bool from_bus = audio_core::eq_bus().read(bands, count);
                if (!from_bus && !load_eq_file(bands, count)) break;  // keep what is designed
                dsp_.set_bands(bands, count);
                // The section trim comes with the file; the bus carries bands only, so an
                // in-memory edit leaves whatever the file said.
                if (!from_bus) dsp_.set_pre_gain_db(eq_trim_db_);
                // What the filters were actually given, sampled where it can be heard.
                // "The EQ feels weak" needs to be answerable with a number, and the number
                // has to come from the audio side rather than from the screen that drew it.
                break;
            }
            case CmdType::STOP: stop = true; break;
            }
        }
        // ── a source change, once nothing of the old one is left to hear ──
        //
        // apply_source() used to run the moment the command arrived: it reset the chain mid-note,
        // and the next block the DAC received was silence while the TX queue still played the
        // music before it, which is a step. Now the
        // old source plays on while both gates close over it, and the switch happens in the
        // silence they leave. Going OFF also waits for the TX queue to play that silence out,
        // because the amplifier goes off with it (A03).
        if (pend_source_ >= 0 && sig_gain_ <= 0.0f && out_gate_.shut()) {
            const bool amp_goes_off = pend_source_ == SRC_OFF && amp_on_;
            if (!amp_goes_off || quiet_frames_ >= tx_frames_ + (uint32_t)BLOCK) {
                const int32_t src = pend_source_;
                pend_source_ = -1;
                apply_source(src);
            }
        }
        const uint32_t cmd_cycles = esp_cpu_get_cycle_count() - cmd_c0;

        const int64_t t0 = esp_timer_get_time();
        bool have_signal = false;
        bool raw_block = false;     ///< a calibration tone: past the chain, at the DAC's own scale
        const float gain_before = sig_gain_;
        bool env_done = false;      ///< a source case shaped this block's envelope itself

        // ── source ──
        //
        // Once both gates are shut over a source that is on its way out, it has nothing left to
        // say: the block is silence, and the old source is no longer read. Reading it on while
        // OFF waits for the TX queue would drain its ring and count underruns for a producer
        // that has already stopped.
        const bool leaving = pend_source_ >= 0 && sig_gain_ <= 0.0f && out_gate_.shut();
        switch (leaving ? (int32_t)SRC_OFF : source_) {
        case SRC_TONE:
            if (tone_mode_ == TONE_CHAIN) {
                tone_.fill(s_blk, BLOCK);
            } else {
                tone_.fill_at(s_blk, BLOCK, 1.0f, tone_mask_);
                {
                    // A new level arrives as a straight line across one block, not as a step.
                    const float a0 = tone_amp_cur_, da = (tone_amp_ - a0) / (float)BLOCK;
                    for (size_t i = 0; i < BLOCK; i++) {
                        const float a = a0 + da * (float)(i + 1);
                        s_blk[2 * i] *= a;
                        s_blk[2 * i + 1] *= a;
                    }
                    tone_amp_cur_ = tone_amp_;
                }
                if (tone_mode_ == TONE_BURSTS) shape_burst(s_blk, BLOCK);
                raw_block = true;
            }
            have_signal = true;
            break;
        case SRC_AUX: {
            size_t got = main_->read(s_io_i32, BLOCK, IO_TIMEOUT);
            if (got == BLOCK) {
                i32_to_f32(s_io_i32, s_blk, BLOCK * 2);
                have_signal = true;
            } else {
                memset(s_blk, 0, sizeof(s_blk));
                underruns++;
            }
            break;
        }
        case SRC_RADIO:
        case SRC_FILE:
        case SRC_AIRPLAY:
        case SRC_USB: {
            auto& ring = audio_core::pcm_ring();
            const uint32_t ring_generation = ring.generation();
            if (ring_generation != ring_generation_) {
                ring_generation_ = ring_generation;
                priming_ = true;
                if (sig_gain_ > 0.0f) {
                    // The queue was dropped from under an open envelope, by a claim that could not
                    // wait for the fade (claim_after_fade gives up after 100 ms). Nothing of the
                    // old stream is left to fade, so its last frame comes down along the short
                    // tail, and the output gate closes over whatever the chain still holds.
                    fade_from_hold(s_blk, last_in_l_, last_in_r_);
                    have_signal = true;
                    sig_gain_ = 0.0f;
                    env_done = true;
                    last_in_l_ = last_in_r_ = 0.0f;
                    break;
                }
                last_in_l_ = last_in_r_ = 0.0f;
            }
            // Paused, or asked to drop the queue: either way the music should be on its way
            // out. Keep reading real samples while the envelope closes — fading the actual
            // waveform is the only way to end it without a step. Replacing it with a decaying
            // copy of the last sample, which is what this did before, swaps one discontinuity
            // for another: a sine caught at +0.4 and continued as a straight line is still
            // not what the waveform was going to do.
            // The producer says what rate its frames are at. Changing the output clock
            // means stopping it, so it is treated as a pause: the envelope closes, the
            // clock is retuned in the silence, and the envelope opens again.
            //
            // Only this source's own producer is heard, and only once it has asked: a rate left
            // by the previous one (a new owner's claim clears it) settled the session before
            // its first track had said what it is, and the whole session converted.
            // A producer whose input changed rate within the session — USB, when the computer
            // switches — asks for a new one. Same as a source change for the clock: nothing
            // settled, nothing refused; the producer's next question retunes once the old
            // frames have played out (rate_change_due).
            if (ring.take_new_session()) {
                session_rate_ = 0;
                refused_rate_ = 0;
                ring.set_clock_locked(false);
            }
            const uint32_t r = ring.owner() == ring_owner(source_) ? ring.rate() : 0;
            if (r) {
                // The clock is chosen ONCE, by the first track of the source session, and
                // not touched again until the source changes.
                //
                // Following it per TRACK is what produced the click the customer's bench
                // isolated on 21.09.2026: 48->48 was silent (microphone peak 97, the noise
                // floor), 44.1->48 peaked at 1020 and 48->44.1 at 981. The mechanism is the
                // DAC's, not this code's — a PCM5102A takes its internal PLL from the bit
                // clock, and set_rate() stops that clock to retune it, so the PLL unlocks
                // and relocks. That is an event in the analogue part of the converter, which
                // is why muting the DATA path around it (XSMT, the clock-mute patch) did
                // nothing measurable.
                //
                // The benefit the per-track clock was bought for is kept: an album at one
                // rate still plays bit-exact, and a Bluetooth session still runs natively at
                // 96 kHz, because both are one session at one rate. What is paid instead is
                // a fixed-ratio conversion for the odd track that does not match, and there
                // is exactly one expensive case in it — 44.1 against 48, 160 phases. Family
                // members (96 against 48, 22.05 against 44.1) are a ratio of two.
                const bool locked = session_rate_ != 0 && r != session_rate_;
                if (r == out_rate_ || r == refused_rate_ || locked) {
                    // Nothing to do — but the producer is waiting to be told so, and a
                    // question left open stalls it for the whole length of its timeout.
                    // A rate already refused gets the same answer it got the first time;
                    // asking again every block would close the envelope every block,
                    // which is silence rather than music.
                    if (!session_rate_) {
                        // The first track of the session was already at the clock's rate, so
                        // nothing retuned and adopt_clock() never ran — but the session's
                        // rate is settled all the same, and it is this one.
                        session_rate_ = out_rate_;
                        ring.set_clock_locked(true);
                    }
                    if (ring.out_rate() != out_rate_) ring.set_out_rate(out_rate_);
                    // A request the producer withdrew, or one overtaken by its own answer,
                    // must not be carried out later on frames that were never at that rate.
                    want_rate_ = 0;
                } else if (r != want_rate_) {
                    want_rate_ = r;
                }
            } else {
                // No question open, so nothing to carry out: a request leaves with its producer.
                want_rate_ = 0;
            }
            // A pending rate change is not yet a reason to close the gate. What is queued is
            // at the OLD rate and has not been heard — the tail of the track that just ended,
            // up to half a second of it — and retuning underneath it played that tail at the
            // new clock: 44.1 kHz material at 48 is 8.8 % sharp, a semitone and a half, on
            // every mixed-rate playlist. So the frames play out first, at the clock they were
            // decoded for, and the retune happens in the silence after the last of them. A
            // pending flush is the exception: those frames are about to be dropped anyway.
            // The producer is waiting for its answer meanwhile, and knows to.
            const bool rate_due = ring.rate_change_due(want_rate_);
            gate_closing_ = ring.paused() || ring.flush_pending() || rate_due;
            if (gate_closing_ && sig_gain_ <= 0.0f) {
                // Fully faded. Now the queue can go, if that is what was asked for, and
                // nothing is audible while it happens.
                if (ring.flush_pending()) { ring.reset(ring_generation); priming_ = true; }
                // The clock waits for the output gate as well. retune() resets the chain, and
                // after the envelope shuts the chain can still be ringing (review A02).
                if (rate_due && out_gate_.shut()) {
                    retune(want_rate_);
                    want_rate_ = 0;
                }
                memset(s_blk, 0, sizeof(s_blk));
                break;
            }
            if (priming_) {
                // Wait for a cushion rather than racing the producer for the first block.
                // eos means there will never be one — a track shorter than the cushion, or
                // one that failed to open — and waiting for it would be waiting forever.
                if (ring.available() < prime_frames_ && !ring.eos()) {
                    memset(s_blk, 0, sizeof(s_blk));
                    break;
                }
                priming_ = false;
            }
            // The settle delays the track, it does not consume it: read during it, the head of
            // the track went through a shut envelope and was lost. An amplifier still off is
            // switched on further down this block, and the settle starts with it. AirPlay and
            // USB are left reading: AirPlay's timing engine and USB's feedback both pace by the
            // ring's fill, and frames held here would read as latency they then undo.
            if (!paces_by_fill(source_) &&
                (!amp_on_ || esp_timer_get_time() < amp_settle_until_us_)) {
                memset(s_blk, 0, sizeof(s_blk));
                break;
            }
            const size_t got = ring.read(s_blk, BLOCK);
            size_t filled = got;              ///< frames of the block that carry signal
            if (got == BLOCK) {
                have_signal = true;
            } else {
                // Fill the rest with silence rather than repeating the last block: a short
                // gap is honest, a repeat is a stutter. An underrun only counts when the
                // producer has NOT said the track ended — otherwise this is just the tail.
                //
                // A PARTIAL block ends mid-waveform exactly as an empty one does, and this
                // is the case the general ramp below cannot see: have_signal stays true, so
                // from the outside the block looks complete while inside it the music butts
                // straight against the memset. Fade the part that arrived, and arm the ramp
                // so whatever comes next starts from silence instead of stepping back up.
                if (got) {
                    const size_t fade = got < sig_fade_out_ ? got : sig_fade_out_;
                    for (size_t i = 0; i < fade; i++) {
                        // Smoothstep, not a straight line: a line turns a corner at each end,
                        // and on a steady tone the corners are what is heard.
                        const float p = 1.0f - (float)i / (float)fade;
                        const float g = p * p * (3.0f - 2.0f * p);
                        const size_t k = got - fade + i;
                        s_blk[2 * k] *= g;
                        s_blk[2 * k + 1] *= g;
                    }
                    have_signal = true;
                    sig_gain_ = 0.0f;       // whatever comes next starts from silence
                    env_done = true;        // this block is already shaped; leave it alone
                } else if (sig_gain_ > 0.0f) {
                    // Nothing arrived at all, mid-waveform: fade_from_hold() says how that ends.
                    filled = fade_from_hold(s_blk, last_in_l_, last_in_r_);
                    have_signal = true;
                    sig_gain_ = 0.0f;
                    env_done = true;
                }
                memset(s_blk + filled * 2, 0, (BLOCK - filled) * 2 * sizeof(float));
                // Nor is it an underrun while a rate change is pending: the producer has
                // asked for a new clock and holds its frames until it has one, so an empty
                // ring here is the old rate's tail running out, which is the plan.
                if (!ring.eos() && !want_rate_) underruns++;
            }
            // Remembered here, before the envelope and the DSP, because this is where a hold
            // has to be injected: a post-DSP sample fed back in would pass the volume twice.
            last_in_l_ = s_blk[2 * (BLOCK - 1)];
            last_in_r_ = s_blk[2 * (BLOCK - 1) + 1];
            break;
        }
        case SRC_BT: {
            sat::StreamRequest request;
            if (!sat::stream_control().snapshot(request)) {
                memset(s_blk, 0, sizeof(s_blk));
                break; // publication in progress; never spin in the audio task
            }
            const uint32_t hz = request.format.rate_hz;
            const uint8_t bits = request.format.bits;
            // Protocol 1.9: who clocks the link is part of the format. A change of it, in
            // either direction, is adopted exactly like a newly announced rate.
            const bool master = sat::stream_control().host_clocks() && link_pins_.valid();
            const bool news = request.revision != bt_fmt_revision_ || master != bt_fmt_master_;
            gate_closing_ = news;
            if (news) {
                bt_capture_.store(false);
                if (sig_gain_ > 0.0f) {
                    // Fade actual old-format audio, not a block already replaced by zeros.
                    if (bt_fmt_ok_ && pull_bt_block(s_blk, BLOCK)) {
                        bt_last_l_ = s_blk[(BLOCK-1)*2];
                        bt_last_r_ = s_blk[(BLOCK-1)*2+1];
                    } else {
                        for (size_t i=0; i<BLOCK; ++i) {
                            s_blk[2*i] = bt_last_l_; s_blk[2*i+1] = bt_last_r_;
                        }
                    }
                    have_signal = true;
                    break;
                }
                memset(s_blk, 0, sizeof(s_blk));
                // Adopting may move the output clock, and that resets the chain: wait for the
                // output gate to close over what the chain still holds (review A02).
                if (!out_gate_.shut()) break;
                if (!bt_reader_paused_.load()) break; // wait without blocking DAC servicing
                if (!master) release_link();
                bt_fmt_ok_ = fmt_valid(&request.format) &&
                             (master ? adopt_bt_format_master(hz, bits)
                                     : (bt_ && adopt_bt_format(hz, bits)));
                if (!bt_fmt_ok_) { release_link(); if (bt_) bt_->stop(); }
                bt_fmt_master_ = master;
                bt_fmt_hz_ = hz;
                bt_fmt_bits_ = bits;
                bt_fmt_revision_ = request.revision;
                bt_refused_ = false;
                bt_tail_.store(bt_head_.load());
                bt_primed_ = false;
                bt_seeded_hz_ = 0;
                bt_diag_stopped_ = false;
                bt_diag_start_us_ = 0;
                bt_last_l_ = bt_last_r_ = 0;
                if (asrc_) resampleReset(asrc_);
                asrc_ratio_ = 1.0;
                // The path is re-decided at the pre-buffer that follows, and a format this
                // board could not tune for is not evidence about the next one.
                bt_clock_follow_ = false;
                bt_trim_dead_    = false;
                bt_clock_unhealthy_ = bt_clock_recover_ = false;
                bt_clock_health_.reset();
                bt_asrc_blocks_  = 0;
                bt_capture_.store(bt_fmt_ok_);
                sat::stream_control().complete(request.revision, bt_fmt_ok_);
                break;
            }

            // Silence until a format has been announced, and this is the safe answer rather
            // than the timid one. An unannounced stream leaves the width to a guess, and a
            // wrong width is not quiet — it is half the frames with the two channels packed
            // into one word, which arrives at the speakers as noise and is indistinguishable
            // by ear from a bad codec. Silence is legible; noise is not.
            if (!hz) {
                memset(s_blk, 0, sizeof(s_blk));
                if (!bt_unannounced_) {
                    bt_unannounced_ = true;
                    ESP_LOGW(TAG, "BT: no STREAM_FMT yet — staying silent rather than guessing "
                                  "the slot width");
                }
                break;
            }
            bt_unannounced_ = false;

            // Announced, but the board could not take it. The port is still configured for
            // something else, and what it hands back is not this stream — it is this stream
            // misread, which is the noise the check above exists to prevent. Silence, and one
            // line saying which format and why, because the fix is on the other side of the
            // link and someone has to be able to read it.
            if (!bt_fmt_ok_) {
                memset(s_blk, 0, sizeof(s_blk));
                if (!bt_refused_) {
                    bt_refused_ = true;
                    ESP_LOGW(TAG, "BT: %lu Hz in %u-bit slots was not adopted — silent until "
                                  "something this board can clock is announced",
                             (unsigned long)hz, (unsigned)bt_fmt_bits_);
                }
                break;
            }

            // pull_bt_block clears bt_primed_ on starvation. Count the transition
            // out of playback once, not the subsequent blocks spent rebuffering.
            if (bt_clock_recover_ && sig_gain_ <= 0.0f) {
                bt_clock_recover_ = false;
                bt_primed_ = false;
                bt_clock_follow_ = false;
                reset_out_trim();
            }
            const bool was_primed = bt_primed_;
            if (pull_bt_block(s_blk, BLOCK)) {
                bt_last_l_ = s_blk[(BLOCK-1)*2]; bt_last_r_ = s_blk[(BLOCK-1)*2+1];
                have_signal = true;
            } else {
                memset(s_blk, 0, sizeof(s_blk));
                if (was_primed && (!BT_DIAGNOSTIC || bt_path_ == BT_DIAG_UNDERRUN)) underruns++;
                if (sig_gain_ > 0.0f) {
                    // Drain the envelope smoothly even when the next input frame
                    // is absent. Never substitute an abrupt all-zero block.
                    for (size_t i = 0; i < BLOCK; ++i) {
                        s_blk[2*i] = bt_last_l_; s_blk[2*i+1] = bt_last_r_;
                    }
                    have_signal = true;
                }
            }
            gate_closing_ = bt_clock_recover_ || !bt_primed_;
            break;
        }
        default:
            memset(s_blk, 0, sizeof(s_blk));
            break;
        }

        // ── source meter (before the DSP): tells "is the source silent?" apart from
        //    "is the DSP/volume wrong?" — the question every AUX/BT bring-up asks first ──
        if (have_signal) {
            for (size_t i = 0; i < BLOCK; i++) {
                const float l = fabsf(s_blk[2 * i]), r = fabsf(s_blk[2 * i + 1]);
                if (l > win_in_l) win_in_l = l;
                if (r > win_in_r) win_in_r = r;
                // Stereo width at the source. A stream whose channels are identical sounds
                // flat and quiet no matter what the DSP does with it afterwards, and that is
                // indistinguishable from "poor codec" by ear alone.
                const float d = fabsf(s_blk[2 * i] - s_blk[2 * i + 1]);
                if (d > win_in_d) win_in_d = d;
            }
        }

        // ── the amplifier, sequenced into silence ──
        //
        // It follows the SOURCE, not the signal. Following the signal meant the enable line
        // moved whenever the music paused — and its transition is a pop, in the amplifier,
        // past anything this code can measure. Tied to the source it moves once when
        // listening starts and once when it ends, and both of those are put into silence by
        // the settle below.
        //
        // And only the source. Mute used to drop the
        // line too, in the same pass that started the chain's fade, while the TX queue was still
        // playing music; the DAC and the amplifier then ran their own transients over it. Mute is
        // a fade in the chain now and leaves the line alone. OFF reaches this point only after
        // the TX queue has played the silence (pend_source_, at the top of the loop).
        {
            const bool want_amp = source_ != SRC_OFF;
            if (want_amp != amp_on_) {
                set_amp(want_amp);
                // The longer of the two settles wins. At boot the amplifier turns on inside
                // the 300 ms boot settle, and this used to cut that down to 100.
                if (want_amp) {
                    const int64_t until = esp_timer_get_time() + AMP_SETTLE_US;
                    if (until > amp_settle_until_us_) amp_settle_until_us_ = until;
                }
            }
        }
        const bool settling = esp_timer_get_time() < amp_settle_until_us_;

        // Sample every frame for one second, including after the fade opens.
        // Same input/output observation used by the historical SD investigation;
        // no PCM changes, allocation, or UART work on the real-time task.
        if (is_ring_source(source_) &&
            have_signal && !gate_closing_ && !settling && gain_before == 0.0f && !env_done) {
            startup_raw = {}; startup_out = {};
            mic_capture::mark_start();
            startup_rate = out_rate_;
            startup_left = startup_rate;
        }
        if (!is_ring_source(source_)) startup_left = 0;
        const size_t startup_frames = startup_left < BLOCK ? startup_left : BLOCK;
        if (startup_frames) startup_raw.observe(s_blk, startup_frames);

        // ── the envelope: what is playing is what this gate lets through ──
        //
        // One ramp covers every edge there is — start, stop, pause, resume and the join
        // between two tracks — because they are all the same event seen from here: the
        // difference between what is arriving and what should be heard.
        //
        // Where both gates are heading. The settle keeps them shut while the amplifier's own
        // transient passes, and a source change waiting for silence shuts them over the old one.
        const bool open = have_signal && !gate_closing_ && !settling && !tone_switching_ &&
                          pend_source_ < 0;
        if (!env_done) {
            const float want = open ? 1.0f : 0.0f;
            if (sig_gain_ != want) {
                for (size_t i = 0; i < BLOCK; i++) {
                    sig_gain_ += (want > sig_gain_) ? sig_up_ : -sig_down_;
                    if (sig_gain_ > 1.0f) sig_gain_ = 1.0f;
                    else if (sig_gain_ < 0.0f) sig_gain_ = 0.0f;
                    // A curve, not the straight line sig_gain_ traces. A linear fade is
                    // continuous in VALUE but not in slope: it turns a corner at each end,
                    // and on a steady tone those two corners are heard as a faint tick and
                    // spread energy across the spectrum row. The step detector cannot see
                    // them, because there is no jump between samples to find — which is why
                    // it kept reporting a clean signal while something was still audible.
                    // Smoothstep fixed the slope; smootherstep fixes the curvature as well
                    // (fade_gate.h has the numbers).
                    const float g = audio_core::smootherstep(sig_gain_);
                    s_blk[2 * i]     *= g;
                    s_blk[2 * i + 1] *= g;
                }
                have_signal = true;             // the block carries the ramp: it must be sent
            } else if (sig_gain_ == 0.0f) {
                memset(s_blk, 0, sizeof(s_blk));
            }
        }

        // ── DSP + output ──
        //
        // With the envelope shut the block is silence, but the chain is not: its limiters'
        // look-ahead holds the last milliseconds of the fade, and its filters ring on after it —
        // the subsonic, the loudness shelf, a resonant band, the bass limiter's crossover. So
        // the chain keeps running on silence while the output gate closes over that, and is
        // reset only once the gate is shut: what it holds belongs to what was playing, not to
        // what comes next, and an edit waiting on a fade lands. Resetting after the look-ahead
        // alone, as this did, cut the ringing off mid-cycle — a click at every pause, louder in
        // the bass than the music was.
        const bool silent = !env_done && sig_gain_ == 0.0f;
        if (!raw_block && (!silent || !out_gate_.shut() || dsp_dirty_)) {
            // In cycles of this core, not microseconds of the clock: an interrupt landing in the
            // middle is counted either way, but nothing else is.
            const uint32_t c0 = esp_cpu_get_cycle_count();
            dsp_.process(s_blk, BLOCK);
            const uint32_t dc = esp_cpu_get_cycle_count() - c0 + cmd_cycles;
            dsp_cycles += dc;
            if (dc > dsp_cycles_max) dsp_cycles_max = dc;
            if (dsp_.peak_l() > win_peak_l) win_peak_l = dsp_.peak_l();
            if (dsp_.peak_r() > win_peak_r) win_peak_r = dsp_.peak_r();
            dsp_dirty_ = true;
        } else if (cmd_cycles > dsp_cycles_max) {
            dsp_cycles_max = cmd_cycles;
        }
        // The output gate, the last thing before the DAC. It heads where the envelope heads;
        // see fade_gate.h for why it exists at all.
        const bool gate_was_shut = out_gate_.shut();
        out_gate_.run(s_blk, BLOCK, open);
        if (silent && out_gate_.shut() && dsp_dirty_) {
            dsp_.reset();
            dsp_dirty_ = false;
        }
        // Whether the ring is being heard, for a producer about to claim it (claim_after_fade).
        audio_core::pcm_ring().set_consumer_live(is_ring_source(source_) &&
                                                 (sig_gain_ > 0.0f || !out_gate_.shut()));
        // A tone mode lands once both gates are shut and the chain is reset.
        if (tone_switching_ && silent && out_gate_.shut()) {
            tone_switching_ = false;
            tone_mode_ = pend_tone_mode_;
            burst_pos_ = 0;
        }

        if (startup_frames) {
            startup_out.observe(s_blk, startup_frames);
            startup_left -= startup_frames;
            if (!startup_left) {
                portENTER_CRITICAL(&mux_);
                status_.startup_seq = ++startup_seq;
                status_.startup_rate = startup_rate;
                status_.startup_raw = startup_raw;
                status_.startup_out = startup_out;
                portEXIT_CRITICAL(&mux_);
            }
        }

        // Observe the actual post-DSP output around ring-source gates. Do not log
        // in this task: the module loop reports a retained snapshot. A step is a
        // measurement, not proof of a click (ordinary music also has transients).
        const bool ring_source = is_ring_source(source_);
        const bool edge_block = ring_source &&
            (gain_before < 1.0f || sig_gain_ < 1.0f || gate_closing_ || env_done ||
             !out_gate_.wide_open());
        if (edge_block) {
            ++edge_blocks;
            if (gain_before == 0.0f && sig_gain_ > 0.0f) ++edge_openings;
            for (size_t i = 0; i < BLOCK; ++i) {
                const float l = s_blk[2 * i], r = s_blk[2 * i + 1];
                const float dl = fabsf(l - last_out_l_);
                const float dr = fabsf(r - last_out_r_);
                const float step = dl > dr ? dl : dr;
                if (step > edge_step) edge_step = step;
                if (i == 0 && step > edge_boundary) edge_boundary = step;
                if (gain_before == 0.0f && sig_gain_ == 0.0f && !env_done) {
                    const float peak = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
                    if (peak > edge_closed) edge_closed = peak;
                }
                last_out_l_ = l; last_out_r_ = r;
            }
        } else {
            last_out_l_ = s_blk[2 * (BLOCK - 1)];
            last_out_r_ = s_blk[2 * (BLOCK - 1) + 1];
        }

        // Analyse EVERY block, including the silent ones.
        //
        // Feeding the analyser only when there is signal leaves the bus holding the last
        // frame it was given, so on pause the row freezes at whatever was playing instead of
        // falling away. Silence is a measurement too, and this is the block that carries it —
        // s_blk is already zeroed when there is nothing to play.
        //
        // The volume is added back, or the row would show the position of the volume knob
        // instead of the music.
        //
        // ...but not when there is nothing to analyse or nobody to look at it: with the
        // source off, or the panel on a screen without a spectrum row, the FFT is work for
        // no one. On the way out one frame at the floor is published, so a row that IS
        // visible falls away rather than freezing on the last thing that played.
        const bool analyse = spectrum_wanted_ && source_ != SRC_OFF;
        if (analyse) {
            spectrum_.set_gain_offset_db(dsp_.volume_cut_db());
            if (spectrum_.feed(s_blk, BLOCK)) {
                audio_core::spectrum_bus().publish(spectrum_.bands_db());
            }
        } else if (spectrum_live_) {
            spectrum_.reset();
            audio_core::spectrum_bus().publish(spectrum_.bands_db());
        }
        spectrum_live_ = analyse;

#if MODESP_AUDIO_DIAGNOSTIC_CONSTANT
        // Deliberately bypass all decoded/DSP samples, including volume/mute.
        // The amplifier's normal control remains intact. Do not use digital
        // zero here: keep the DAC's zero-data detector out of this experiment.
        for (auto& sample : s_io_i32) sample = DAC_KEEPALIVE;
#else
        out_clamps += f32_to_i32(s_blk, s_io_i32, BLOCK * 2, raw_block ? RAW_SCALE : out_scale_);
#endif
        busy_us += esp_timer_get_time() - t0;

        // AUX pacing comes from the RX read; tone/off/BT are paced by the TX DMA ring.
        //
        // A short write is NOT the same fault as an empty ring, and counting them together
        // hid that. What the ring fails to supply arrives as silence, which the envelope
        // shapes; what I2S refuses to accept is simply THROWN AWAY — the samples never reach
        // the DAC and the stream jumps, which no amount of work on s_blk can prevent or even
        // see. They deserve separate names.
        // Did this task arrive on time?
        //
        // The blind spot in everything measured so far. A block is 5 ms of audio, so blocks
        // must leave every 5 ms; if this task is late the TX DMA runs out and the converter
        // receives whatever the hardware does when starved — a repeat or silence — which is a
        // click that exists only after the samples, where nothing here can see it. The short
        // write counter checks the opposite case, this task being too FAST for the queue.
        //
        // Late is measured against the clock, which is the only witness either task shares.
        {
            const int64_t now_us = esp_timer_get_time();
            if (last_block_us_) {
                const uint32_t gap = (uint32_t)(now_us - last_block_us_);
                if (gap > late_us_) late_us_ = gap;
            }
            last_block_us_ = now_us;
        }
        size_t written = main_->write(s_io_i32, BLOCK, IO_TIMEOUT);
        if (written < BLOCK) tx_short_++;
        // How much silence the DAC has been sent since anything else: once it covers the TX
        // queue, the music before it has left the pins.
        if (gate_was_shut && out_gate_.shut()) {
            if (quiet_frames_ < UINT32_MAX - (uint32_t)BLOCK) quiet_frames_ += (uint32_t)BLOCK;
        } else {
            quiet_frames_ = 0;
        }

        // ── status window: half a second, at every rate (100 blocks was 0.125 s at 192 kHz, and
        // the main loop, which reads it every 0.5 s, missed three windows in four) ──
        ++blocks;
        if (esp_timer_get_time() - window_start >= 500000) {
            const int64_t now = esp_timer_get_time();
            const float cpu = 100.0f * (float)busy_us / (float)(now - window_start);
            // DSP as a share of one core over the window, and the worst block as a share of a
            // block's own time — the number that says whether 192 kHz fits. Worked out here, not
            // inside the critical section below.
            const float core_hz = (float)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6f;
            const float window_s = (float)(now - window_start) / 1e6f;
            const float block_s = (float)BLOCK / (float)(out_rate_ ? out_rate_ : 48000u);
            const float dsp_pct = window_s > 0.0f ? 100.0f * (float)dsp_cycles / (core_hz * window_s) : 0.0f;
            const float dsp_max_pct = 100.0f * (float)dsp_cycles_max / (core_hz * block_s);
            dsp_cycles = 0;
            dsp_cycles_max = 0;
            portENTER_CRITICAL(&mux_);
            status_.peak_l = win_peak_l;
            status_.peak_r = win_peak_r;
            status_.in_diff = win_in_d;
            status_.in_l = win_in_l;
            status_.in_r = win_in_r;
            status_.underruns = underruns;
            status_.dsp_faults = dsp_.faults();
            status_.dsp_pct = dsp_pct;
            status_.dsp_max_pct = dsp_max_pct;
            status_.out_clamps = out_clamps;
            status_.eq_headroom_db = dsp_.headroom_db();
            status_.loud_db = dsp_.loudness_db();
            status_.lev_db = dsp_.leveler_db();
            {
                const float bg = dsp_.take_bass_min();
                status_.bass_gr_db = bg >= 1.0f ? 0.0f : -20.0f * log10f(bg);
            }
            status_.source = source_;
            status_.running = true;
            status_.cpu_pct = cpu;
            status_.late_us = late_us_;
            status_.out_rate = out_rate_;
            status_.tx_short = tx_short_;
            status_.amp_on = amp_on_;
            if (edge_blocks && (edge_openings || edge_step > 0.0f)) {
                status_.edge_seq = ++edge_seq;
                status_.edge_rate = out_rate_;
                status_.edge_blocks = edge_blocks;
                status_.edge_openings = edge_openings;
                status_.edge_step = edge_step;
                status_.edge_boundary = edge_boundary;
                status_.edge_closed = edge_closed;
            }
            edge_blocks = edge_openings = 0;
            edge_step = edge_boundary = edge_closed = 0.0f;
            late_us_ = 0;
            // The deepest the limiter went in this window, as dB of reduction — what a GR
            // meter shows and what the EQ screen draws. Its gain right now would miss every
            // peak shorter than the window, which on music is nearly all of them.
            const float lg = dsp_.take_limiter_min();
            status_.lim_gr_db = lg >= 1.0f ? 0.0f : -20.0f * log10f(lg);
            status_.in_rate_hz = in_rate_est_;
            // Derived from the quantities the control law actually uses, so the readout cannot
        // drift away from the loop it reports on. It used to divide by a literal 48000 and
        // multiply by the MEASURED rate — correct only while base was 48000/measured, and
        // left behind when base became out_rate_/announced. After that it reported the
        // estimator's noise, printing values outside the servo's own clamp, and with the
        // output retuned to 44.1 kHz by a previous file it printed about -81000 ppm.
        status_.asrc_ppm = (float)(asrc_base_ > 0.0 ? (asrc_ratio_ / asrc_base_ - 1.0) * 1e6 : 0.0);
            // The clock's side of the same story. asrc_ppm stays what it says it is — what
            // the RESAMPLER is doing — and reads zero while the clock is doing the work,
            // which is the honest answer and also the one the stage's DoD asks for.
            status_.clock_ppm      = bt_trim_got_;
            status_.clock_follow   = bt_clock_follow_;
            status_.bt_asrc_blocks = bt_asrc_blocks_;
            status_.bt_active = source_ == SRC_BT && bt_fmt_ok_ && bt_primed_ && !gate_closing_;
            status_.bt_primes = bt_primes_;
            status_.bt_format_revision = bt_fmt_revision_;
            portEXIT_CRITICAL(&mux_);
            busy_us = 0;
            window_start = now;
            blocks = 0;
            win_peak_l = win_peak_r = 0.0f;
            win_in_l = win_in_r = win_in_d = 0.0f;
        }
    }

    // Both subscriptions end before the task does. A task deleted while the TWDT still
    // watches it can never feed again, which is a guaranteed panic a few seconds later.
    modesp::liveness::unwatch("audio");
    if (wdt) esp_task_wdt_delete(nullptr);

    pause_bt_reader();
    sat::stream_control().invalidate_result();
    set_amp(false);
    main_->stop();
    if (bt_) bt_->stop();
    portENTER_CRITICAL(&mux_); status_.running = false; portEXIT_CRITICAL(&mux_);
    ESP_LOGI(TAG, "audio task stopped");
}

// ═══════════════════════════════════════════════════════════════
// BT reader task: I²S slave RX → PSRAM ring, measures the peer's sample rate
// ═══════════════════════════════════════════════════════════════

void AudioModule::bt_rx_loop() {
    static int32_t rx[BLOCK * 2];
    int64_t  win_start = esp_timer_get_time();
    uint32_t win_frames = 0;
    int64_t last_read_us = 0;
    while (run_ && bt_) {
        if (!bt_capture_.load()) {
            bt_reader_paused_.store(true);
            win_frames = 0; win_start = esp_timer_get_time();
            last_read_us = 0;
            vTaskDelay(1);
            continue;
        }
        bt_reader_paused_.store(false);
        // Recheck after leaving PAUSED so the control task cannot race a new read.
        if (!bt_capture_.load()) continue;
        const int64_t read_us = esp_timer_get_time();
        if (last_read_us) {
            const uint32_t gap = (uint32_t)(read_us - last_read_us);
            uint32_t peak = bt_rx_gap_us_.load(std::memory_order_relaxed);
            while (gap > peak && !bt_rx_gap_us_.compare_exchange_weak(peak, gap)) {}
        }
        last_read_us = read_us;
        size_t got = bt_rx_port()->read(rx, BLOCK, 10);
        if (got == 0 || !bt_capture_.load()) {
            // No clocks from the peer (BT module idle/off) — keep the estimate, idle briefly.
            // read() already waited for DMA. A second 10 ms sleep exceeds the
            // complete 96 kHz RX queue and can lose the first resumed samples.
            vTaskDelay(1);
            continue;
        }
        // Write to the ring. When it is full the surplus is thrown away, and every frame
        // thrown away is a discontinuity in the signal — a click, arriving whenever the
        // conditions happen to meet rather than on any beat. That was silent until now:
        // no counter, no line in the log, nothing to tell it from a bad codec by ear.
        size_t head = bt_head_;
        const size_t used = (head + bt_ring_frames_ - bt_tail_) % bt_ring_frames_;
        size_t room = bt_ring_frames_ - 1 - used;
        size_t n = got < room ? got : room;
        if (n < got) bt_dropped_ += (uint32_t)(got - n);
        for (size_t i = 0; i < n; i++) {
            bt_ring_[2 * head]     = (float)rx[2 * i] / I32_SCALE;
            bt_ring_[2 * head + 1] = (float)rx[2 * i + 1] / I32_SCALE;
            if (++head == bt_ring_frames_) head = 0;
        }
        bt_head_ = head;
        bt_frames_total_ += got;

        // rate estimate: frames per second over 2 s windows, EMA
        win_frames += got;
        const int64_t now = esp_timer_get_time();
        if (now - win_start >= 2000000) {
            const float rate = (float)win_frames * 1e6f / (float)(now - win_start);
            in_rate_est_ = in_rate_est_ * 0.7f + rate * 0.3f;
            win_frames = 0;
            win_start = now;
        }
    }
    bt_reader_paused_.store(true);
}
