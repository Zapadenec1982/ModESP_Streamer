"""Fault injection executes current production retune/source methods against a fake port."""
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def compile_run(tmp_path, code):
    compiler = shutil.which("g++") or r"C:\msys64\ucrt64\bin\g++.exe"
    assert Path(compiler).exists(), "Host C++ compiler is required"
    cpp = tmp_path / "audio_lifecycle.cpp"
    cpp.write_text(code, encoding="utf-8")
    exe = tmp_path / "audio_lifecycle.exe"
    env = dict(os.environ)
    env["PATH"] = str(Path(compiler).parent) + os.pathsep + env.get("PATH", "")
    subprocess.run([compiler, "-std=c++17", "-O1", str(cpp),
                    str(ROOT / "components/audio_dsp/src/pcm_ring.cpp"),
                    "-I" + str(ROOT / "components/audio_dsp/include"), "-o", str(exe)],
                   check=True, capture_output=True, text=True, env=env, timeout=60)
    subprocess.run([str(exe)], check=True, capture_output=True, text=True, env=env, timeout=10,
                   cwd=tmp_path)


def test_audio_retune_failures_and_source_gate(tmp_path):
    source = (ROOT / "modules/audio/src/audio_module.cpp").read_text(encoding="utf-8")
    retune = source.split("void AudioModule::retune(", 1)[1].split("/// The port has taken", 1)[0]
    switch = source.split("void AudioModule::apply_source(", 1)[1].split("/// Take on the format", 1)[0]
    fixture = (ROOT / "tools/tests/fixtures/audio_lifecycle.cpp").read_text(encoding="utf-8")
    compile_run(tmp_path, fixture + "\nvoid AudioModule::retune(" + retune +
                "\nvoid AudioModule::apply_source(" + switch)


def test_failed_next_previous_does_not_leave_playing_with_null_file(tmp_path):
    source = (ROOT / "modules/player/src/player_module.cpp").read_text(encoding="utf-8")
    def function(name):
        start = source.index("void " + name + "(")
        end = source.index("{", start) + 1
        level = 1
        while level:
            level += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[start:end]
    functions = "\n".join(function(name) for name in ("fail_track", "open_command"))
    code = r'''
#include "pcm_ring.h"
#include <cstdio>
#include <cassert>
struct State { FILE* f=nullptr; size_t tail_len=0; int pending_seek=-1,state=0; uint32_t ring_generation=0; } s;
void close_file(){ if(s.f) fclose(s.f); s.f=nullptr; }
bool open_index(int, bool){ close_file(); return false; } // failed SD open after disposing previous track
'''
    code += functions
    code += r'''
int main(){
 auto& r=audio_core::pcm_ring(); assert(r.init(8)); r.claim(audio_core::PcmRing::OWNER_FILE);
 s.ring_generation=r.generation(); s.state=1;
 s.f=fopen("track.bin","w+b"); assert(s.f);
 open_command(-1); // playlist edge leaves the existing track alone
 assert(s.state==1 && s.f);
 open_command(1);
 assert(s.state==3 && !s.f && r.eos() && r.owner()==audio_core::PcmRing::OWNER_NONE);
 r.claim(audio_core::PcmRing::OWNER_FILE); s.ring_generation=r.generation();
 r.claim(audio_core::PcmRing::OWNER_RADIO); // delayed FILE failure cannot end the new stream
 open_command(0);
 assert(s.state==3 && !s.f && !r.eos() && r.owner()==audio_core::PcmRing::OWNER_RADIO);
}
'''
    compile_run(tmp_path, code)


def test_airplay_cancels_partial_write_on_session_change_or_stop(tmp_path):
    source = (ROOT / "components/airplay2/port/audio_output_ring.cpp").read_text(encoding="utf-8")
    start = source.index("bool push(const float*")
    push = source[start:source.index("/// The same through the converter", start)]
    code = r'''
#include "pcm_ring.h"
#include <cassert>
using audio_core::PcmRing;
struct Out { std::atomic<bool> run{true}, want{true}, flush{false}; uint32_t generation=0; } g;
int mode=0, waits=0;
void vTaskDelay(int) {
 assert(++waits < 3);
 auto& r=audio_core::pcm_ring();
 if(mode==0) { r.claim(PcmRing::OWNER_RADIO); r.claim(PcmRing::OWNER_AIRPLAY); }
 else g.run=false;
}
'''
    code += push
    code += r'''
int main() {
 auto& r=audio_core::pcm_ring(); assert(r.init(2)); r.claim(PcmRing::OWNER_AIRPLAY);
 g.generation=r.generation(); float pcm[8]={};
 assert(!push(pcm,4)); assert(waits==1 && r.available()==0);
 g.generation=r.generation(); mode=1; waits=0;
 assert(!push(pcm,4)); assert(waits==1);
 r.reset(); g.generation=r.generation(); g.run=true; g.flush=true;
 assert(!push(pcm,1)); assert(r.available()==0);
}
'''
    compile_run(tmp_path, code)

def test_bt_recovery_waits_for_closed_envelope_before_asrc(tmp_path):
    source = (ROOT / "modules/audio/src/audio_module.cpp").read_text(encoding="utf-8")
    pull_start = source.index("bool AudioModule::pull_bt_block(")
    start = source.index("    if (!bt_primed_) {", pull_start)
    end = source.index("{", start) + 1
    level = 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    # Execute the complete production priming/path-selection block. It has no
    # dependency on the preceding sample-copy or following resampler kernels.
    block = source[start:end]
    code = r'''
#include <cassert>
#include <cstdint>
#include <cstddef>
static int asrc_resets=0;
static void resampleReset(void*) { ++asrc_resets; }
struct Port { bool can_trim() const {return true;} } port;
struct AudioModule {
 bool bt_primed_=false, bt_clock_follow_=true, bt_clock_allowed_=true;
 bool bt_trim_dead_=false, bt_clock_unhealthy_=true, bt_clock_recover_=true;
 float sig_gain_=0.5f;
 uint32_t out_rate_=48000, bt_primes_=0, bt_servo_frames_=123;
 int bt_path_=1, bt_err_sum_=17, bt_err_n_=2, trim_resets=0;
 Port* main_=&port;
 void* asrc_=this;
 void reset_out_trim() { ++trim_resets; }
 bool prime(size_t used, size_t target, uint32_t announced) {
'''
    code += block
    code += r'''
 return true;
 }
};
int main() {
 AudioModule audio;
 // The reader filled the reserve while the audio task was descheduled during
 // a recovery fade. Neither ASRC nor clock reset is permitted at nonzero gain.
 assert(!audio.prime(6000,5760,48000));
 assert(!audio.bt_primed_ && audio.bt_clock_follow_ && audio.bt_path_==1);
 assert(audio.bt_primes_==0 && audio.trim_resets==0 && asrc_resets==0);
 // The same production branch may choose ASRC once the envelope reaches zero.
 audio.sig_gain_=0.0f;
 assert(audio.prime(6000,5760,48000));
 assert(audio.bt_primed_ && !audio.bt_clock_follow_ && audio.bt_path_==6);
 assert(audio.bt_primes_==1 && audio.trim_resets==1 && asrc_resets==1);
 assert(audio.bt_servo_frames_==0 && audio.bt_err_sum_==0 && audio.bt_err_n_==0);
}
'''
    compile_run(tmp_path, code)


def test_new_session_asked_between_take_and_read_is_not_refused(tmp_path):
    # usb_pcm runs above the audio task on core 1. A new stream asks for a new session and then
    # for its rate; landing between the consumer's take_new_session() and its rate() read, it
    # looked like a second rate in the old, locked session — refused, the stream silent until
    # the computer restarted it. The production slice runs against a ring whose producer runs
    # exactly there, and right after the read as well.
    source = (ROOT / "modules/audio/src/audio_module.cpp").read_text(encoding="utf-8")
    start = source.index("uint32_t AudioModule::ring_question(")
    end = source.index("{", start) + 1
    level = 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    code = r'''
#include <cassert>
#include <cstdint>
#define PcmRing Ring   // the slice names the real ring; this one lets the producer in on cue
namespace audio_core {
struct Ring {
    enum Owner : uint8_t { OWNER_NONE, OWNER_FILE, OWNER_RADIO, OWNER_AIRPLAY, OWNER_USB };
    Owner who = OWNER_USB;
    uint32_t asked = 48000;
    bool request = false, locked = true;
    int takes = 0, reads = 0;
    int run_after_take = 0, run_after_read = 0;   // the producer runs after this call, 0: never
    uint32_t next = 96000;
    void producer() { request = true; asked = next; }   // request_new_session(), ask_rate()
    bool take_new_session() {
        const bool had = request && who == OWNER_USB;
        request = false;
        if (++takes == run_after_take) producer();
        return had;
    }
    Owner owner() const { return who; }
    uint32_t rate() {
        const uint32_t r = asked;
        if (++reads == run_after_read) producer();
        return r;
    }
    void set_clock_locked(bool on) { locked = on; }
};
}
enum { SRC_OFF, SRC_TONE, SRC_AUX, SRC_BT, SRC_FILE, SRC_RADIO, SRC_AIRPLAY, SRC_USB };
audio_core::Ring::Owner ring_owner(int32_t src) {
    return src == SRC_USB ? audio_core::Ring::OWNER_USB : audio_core::Ring::OWNER_NONE;
}
struct AudioModule {
    int32_t source_ = SRC_USB;
    uint32_t out_rate_ = 48000, session_rate_ = 48000, refused_rate_ = 0;
    uint32_t ring_question(audio_core::Ring& ring);
};
'''
    code += source[start:end]
    code += r'''
int main() {
    {   // the producer between the take and the read: the new session is taken with its rate
        AudioModule m; audio_core::Ring ring; ring.run_after_take = 1;
        assert(m.ring_question(ring) == 96000);
        assert(m.session_rate_ == 0 && m.refused_rate_ == 0 && !ring.locked);
        assert(!ring.request);
    }
    {   // a rate the old session refused, asked again by a new one in the same place
        AudioModule m; m.out_rate_ = m.session_rate_ = 44100; m.refused_rate_ = 88200;
        audio_core::Ring ring; ring.asked = 88200; ring.next = 88200; ring.run_after_take = 1;
        assert(m.ring_question(ring) == 88200);
        assert(m.session_rate_ == 0 && m.refused_rate_ == 0 && !ring.locked);
    }
    {   // the producer right after the read: what was read is the old question, answered as one;
        // the new session waits for the next block, and is never paired with the old rate
        AudioModule m; audio_core::Ring ring; ring.run_after_read = 1;
        const uint32_t first = m.ring_question(ring);
        assert(first == 96000 ? m.session_rate_ == 0 : (first == 48000 && m.session_rate_ == 48000));
        assert(m.ring_question(ring) == 96000);
        assert(m.session_rate_ == 0 && !ring.locked);
    }
    {   // the old session refused a rate, the producer runs after the read of it: rate read again
        AudioModule m; m.out_rate_ = m.session_rate_ = 44100; m.refused_rate_ = 88200;
        audio_core::Ring ring; ring.asked = 88200; ring.next = 96000; ring.run_after_read = 1;
        assert(m.ring_question(ring) == 96000);
        assert(m.session_rate_ == 0 && m.refused_rate_ == 0);
    }
    {   // nothing new: the session stays as it is
        AudioModule m; audio_core::Ring ring; ring.asked = 44100;
        assert(m.ring_question(ring) == 44100);
        assert(m.session_rate_ == 48000 && ring.locked);
    }
    {   // another producer's ring: no question, and no session of its to take
        AudioModule m; audio_core::Ring ring; ring.who = audio_core::Ring::OWNER_FILE; ring.request = true;
        assert(m.ring_question(ring) == 0);
        assert(m.session_rate_ == 48000 && ring.locked);
    }
}
'''
    compile_run(tmp_path, code)


def settings_harness():
    """The production on_update, up to its 500 ms report, and follow_settings with the helpers
    they call, against a store that counts its reads and moves a version as SharedState does.
    The caller appends main()."""
    source = (ROOT / "modules/audio/src/audio_module.cpp").read_text(encoding="utf-8")

    def function(signature):
        start = source.index(signature)
        end = source.index("{", start) + 1
        level = 1
        while level:
            level += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[start:end]

    start = source.index("void AudioModule::on_update(uint32_t dt_ms) {")
    on_update = source[start:source.index("    report_ms_ += dt_ms;\n", start)] + "}\n"
    production = "\n".join(function(s) for s in (
        "bool AudioModule::follow_settings(", "int32_t AudioModule::read_port(",
        "int32_t AudioModule::vol_max(", "int32_t AudioModule::loud_ref(",
        "int32_t AudioModule::timbre(")) + "\n" + on_update
    code = r'''
#include "level_latch.h"
#include <atomic>
#include <cassert>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <variant>
#include <vector>
template <class... A> void esp_log(A&&...) {}
#define ESP_LOGW(...) esp_log(__VA_ARGS__)
#define ESP_LOGI(...) esp_log(__VA_ARGS__)
#define MODESP_AUDIO_BT_DIRECT_CONTINUOUS 0
const char* const TAG = "Audio";
constexpr bool BT_DIAGNOSTIC = false;
constexpr int pdPASS = 1;
constexpr int32_t AMP_CLIP_DEFAULT_DB10 = -98;
constexpr uint32_t TONE_RAW_MAX_MS = 60000, TONE_CHAIN_IDLE_MS = 120000;
namespace modesp::gen { constexpr int32_t AUDIO_SPK_TYPE_UNSET = 0, AUDIO_SPK_TYPE_CLOSED = 2; }
enum { SRC_OFF, SRC_TONE, SRC_AUX, SRC_BT, SRC_FILE, SRC_RADIO, SRC_AIRPLAY, SRC_USB };
enum { TONE_CHAIN, TONE_RAW, TONE_BURSTS };
enum class CmdType { SOURCE, MUTE, EQ_ENABLE, EQ_RELOAD, TONE_HZ, WIDTH, MONO_BASS, SPECTRUM,
                     BT_CLOCK, AMP_CLIP, DRIVE, SUBSONIC, TONE_MODE, TONE_DB10, TONE_CH, BASS_XO,
                     RMS_CAP, LOUDNESS, PORT, TIMBRE, BALANCE, LOUD_REF, LEVELER, STOP };
float ceiling_dbfs(float c) { return c; }
const char* source_name(int32_t) { return ""; }
int xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, void*, int) { return pdPASS; }

// The store as SharedState keeps it: a change moves the version, the same value does not.
using Value = std::variant<int32_t, bool, float, std::string>;
std::map<std::string, Value> store;
uint32_t version = 1;
bool store_down = false;            // its mutex timed out: version() answers 0
int reads = 0;                      // settings looked up, audio.port_meas apart (read every pass)
void put(const char* k, Value v) {
    auto it = store.find(k);
    if (it != store.end() && it->second == v) return;
    store[k] = v;
    ++version;
}
template <class T> T get(const char* k, T def) {
    if (std::string(k) != "audio.port_meas") ++reads;
    auto it = store.find(k);
    if (it == store.end()) return def;
    const T* v = std::get_if<T>(&it->second);
    return v ? *v : def;
}
std::vector<std::pair<CmdType, int32_t>> sent;

struct AudioModule {
    void* main_ = this;
    bool run_ = true;
    std::atomic<int32_t> gone_source_{-1};
    int32_t last_source_ = -1, last_volume_ = -1, last_tone_ = -1, last_vol_max_ = -1;
    uint32_t settings_ver_ = 0;
    audio_core::LevelLatch level_;
    bool vol_published_ = false;
    int32_t pending_source_ = -1;
    bool last_mute_ = false, last_eq_ = true, last_bt_clock_ = true;
    volatile uint8_t bt_path_ = 0;
    uint8_t last_bt_path_ = 0;
    int32_t last_width_ = -1, last_mono_bass_ = -1;
    int last_spectrum_ = -1;
    int32_t last_amp_clip_ = INT32_MIN, last_drive_ = INT32_MIN, last_hp_hz_ = INT32_MIN;
    int32_t last_bass_xo_ = INT32_MIN, last_rms_cap_ = INT32_MIN;
    int32_t last_loud_ = INT32_MIN, last_port_ = INT32_MIN, last_loud_ref_ = INT32_MIN;
    int32_t last_timbre_ = INT32_MIN, last_balance_ = INT32_MIN, last_leveler_ = INT32_MIN;
    int32_t last_tone_mode_ = INT32_MIN, last_tone_db10_ = INT32_MIN, last_tone_ch_ = INT32_MIN;
    uint32_t tone_raw_ms_ = 0, tone_idle_ms_ = 0, report_ms_ = 0;
    std::atomic<bool> meter_busy_{false};
    int passes = 0, schedule_on_at = -1;   // the leveler's schedule switches it on at that pass

    static void meter_entry(void*) {}
    void own_eq() {}
    void send(CmdType t, int32_t v) { sent.push_back({t, v}); }
    void log_bass(const char*) {}
    bool source_on_offer(int32_t src) { return src != SRC_AUX; }
    void leveler_schedule(uint32_t) { if (++passes == schedule_on_at) state_set("audio.leveler", true); }
    uint32_t state_version() const { return store_down ? 0 : version; }
    int32_t read_int(const char* k, int32_t def) const { return get<int32_t>(k, def); }
    bool read_bool(const char* k, bool def) const { return get<bool>(k, def); }
    bool read_string(const char* k, char* out, size_t n) const {
        std::snprintf(out, n, "%s", get<std::string>(k, "").c_str());
        return true;
    }
    void state_set(const char* k, int32_t v) { put(k, v); }
    void state_set(const char* k, bool v) { put(k, v); }
    void state_set(const char* k, float v) { put(k, v); }
    void state_set(const char* k, const char* v) { put(k, std::string(v)); }
    bool follow_settings();
    int32_t read_port();
    int32_t vol_max();
    int32_t loud_ref();
    int32_t timbre();
    void on_update(uint32_t dt_ms);
};
'''
    code += production
    code += r'''
int pass(AudioModule& m, uint32_t dt_ms) {    // one pass of the main loop: the settings it read
    sent.clear();
    reads = 0;
    m.on_update(dt_ms);
    return reads;
}
bool sends(CmdType t, int32_t v) {
    for (const auto& c : sent) if (c.first == t && c.second == v) return true;
    return false;
}
'''
    return code


def test_settings_are_read_again_only_when_the_store_moved(tmp_path):
    # AudioModule::on_update runs every 10 ms and used to read some thirty settings each time,
    # every lookup under the store's mutex. It reads them only when the store's version moved
    # (BaseModule::state_version): a quiet pass reads no setting, a change is sent in its pass,
    # a store that does not answer (version 0) is read anyway, and what runs on every pass —
    # the schedule, the volume taken, a tone's clock — still runs while nothing is read.
    compile_run(tmp_path, settings_harness() + r'''
int main() {
    put("audio.source", (int32_t)SRC_FILE);
    put("audio.volume", (int32_t)30);
    put("audio.loudness", (int32_t)70);
    put("ui.screen", std::string("now"));
    AudioModule m;

    // The first pass reads every setting and sends each to the audio task.
    const int all = pass(m, 10);
    assert(all >= 25);
    assert(sends(CmdType::SOURCE, SRC_FILE) && sends(CmdType::LOUDNESS, 70) && sends(CmdType::SPECTRUM, 1));
    // Its own write (audio.ceiling_dbfs) moved the store: one more pass reads them and sends
    // nothing; from then on a pass with nothing changed reads none of them.
    assert(pass(m, 10) == all && sent.empty());
    assert(pass(m, 10) == 0 && sent.empty());
    assert(pass(m, 10) == 0 && !store.count("audio.vol_eff"));

    // The audio task takes the volume while nothing changes: the pass says so all the same.
    int32_t pct = 0, limit = 0;
    assert(m.level_.take(pct, limit) && pct == 30 && limit == 100);
    assert(pass(m, 10) == 0 && std::get<float>(store.at("audio.vol_eff")) == 30.0f);
    pass(m, 10);
    assert(pass(m, 10) == 0);

    // A change is sent in the pass that finds it, and alone.
    put("audio.loudness", (int32_t)40);
    pass(m, 10);
    assert(sent.size() == 1 && sends(CmdType::LOUDNESS, 40));
    assert(pass(m, 10) == 0);

    // A store that does not answer is read every pass, until it does again.
    store_down = true;
    assert(pass(m, 10) == all && sent.empty());
    assert(pass(m, 10) == all && sent.empty());
    store_down = false;
    assert(pass(m, 10) == all);
    assert(pass(m, 10) == 0);

    // The leveler's schedule runs before the gate: what it switches is sent in the same pass.
    m.schedule_on_at = m.passes + 1;
    pass(m, 10);
    assert(sent.size() == 1 && sends(CmdType::LEVELER, 1));

    // A tone through the chain left unchanged goes off by itself, read or not.
    put("audio.source", (int32_t)SRC_TONE);
    pass(m, 10);
    assert(sends(CmdType::SOURCE, SRC_TONE));
    for (int i = 0; i < 11; ++i) assert(pass(m, 10000) == 0);
    assert(std::get<int32_t>(store.at("audio.source")) == SRC_TONE);
    pass(m, 10000);
    assert(std::get<int32_t>(store.at("audio.source")) == SRC_OFF);
    pass(m, 10);
    assert(sends(CmdType::SOURCE, SRC_OFF));
}
''')


def test_loudness_switched_off_keeps_the_strength_it_had(tmp_path):
    # Loudness off is audio.loudness = 0, and on puts back the strength it had: the audio module
    # keeps it in audio.loud_back (saved), so the glass and every browser put back the same one.
    compile_run(tmp_path, settings_harness() + r'''
int main() {
    put("audio.source", (int32_t)SRC_FILE);
    put("audio.loudness", (int32_t)40);
    AudioModule m;
    pass(m, 10);
    assert(!store.count("audio.loud_back"));                    // on: nothing to keep yet
    put("audio.loudness", (int32_t)0);
    pass(m, 10);
    assert(sends(CmdType::LOUDNESS, 0) && std::get<int32_t>(store.at("audio.loud_back")) == 40);
    put("audio.loudness", (int32_t)55);                         // on again, at another strength
    pass(m, 10);
    assert(std::get<int32_t>(store.at("audio.loud_back")) == 40);
    put("audio.loudness", (int32_t)30);                         // moved while on: no switch
    pass(m, 10);
    assert(std::get<int32_t>(store.at("audio.loud_back")) == 40);
    put("audio.loudness", (int32_t)0);
    pass(m, 10);
    assert(std::get<int32_t>(store.at("audio.loud_back")) == 30);
}
''')
