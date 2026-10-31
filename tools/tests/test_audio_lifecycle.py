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
