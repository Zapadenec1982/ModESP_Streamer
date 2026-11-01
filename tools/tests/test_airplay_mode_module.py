"""Compile the production AirplayModule update/lifecycle logic against deterministic tasks.

The receiver transport and FreeRTOS are mocked; the mode state machine, source/volume
handling, and event mirroring are extracted from the actual module for every run.
Run directly with Python or collect with pytest; requires a native C++ compiler.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_airplay_mode import compiler_command, function
except ImportError:
    from test_airplay_mode import compiler_command, function

ROOT = Path(__file__).resolve().parents[2]

PREFIX = r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <string>
#include <variant>
#include <vector>

@VOLUME_FOLLOW_H@
@AIRPLAY_META_H@

using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_NO_MEM=257, ESP_FAIL=-1, pdPASS=1;
constexpr int START_STACK=8192, SRC_AIRPLAY=6;
constexpr int64_t COVER_PAIRING_US=5000000;
constexpr auto TAG="AirPlay", NVS_NS="airplay_mod", NVS_TRY="starting";
constexpr int COVER=0;
#define ESP_LOGI(...) ((void)0)
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
enum Start { NOT_YET=0, STARTING=1, UP=2, FAILED=3 };
const char* esp_err_to_name(int e) { return e==ESP_OK ? "ESP_OK" : e==ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : "ESP_FAIL"; }
int64_t now_us=1000000;
int64_t esp_timer_get_time() { return now_us; }
namespace modesp::nvs_helper { void erase_key(const char*,const char*) {} }
namespace player_core {
struct View { void* px=nullptr; };
View cover_view(int) { return {}; }
}
struct CoverJob { uint8_t* jpg; size_t len; };
void post_cover(CoverJob) {}

struct Shared {
 int lock=0; char title[64]="",artist[64]="",album[64]="";
 uint32_t dur_s=0,pos_s=0; int64_t pos_at_us=0;
 std::atomic<uint32_t> meta_gen{0};
 std::atomic<bool> connected{false},playing{false},paused{false},play_edge{false};
 std::atomic<int32_t> volume_pct{-1};
 std::atomic<uint32_t> session{0};
 std::atomic<int> start{UP};
 std::atomic<int32_t> mode_target{0};
 std::atomic<bool> stop_target{false};
 std::atomic<int> mode_result{ESP_OK};
 std::atomic<bool> mode_done{false};
 std::atomic<int64_t> cover_at{0},title_at{0};
 airplay_meta::Smoother meta; char held_title[64]=""; CoverJob held_cover{nullptr,0};
} sh;

using Value=std::variant<int32_t,bool,std::string>;
std::map<std::string,Value> values;
void (*queued_worker)(void*)=nullptr;
bool in_worker=false,create_fails=false,receiver_started=true,legacy=false;
int receiver_result=ESP_OK,tasks_created=0,switch_calls=0,unadvertise_calls=0,start_calls=0,stop_calls=0;
bool guard_fails=false;
bool guard_busy=false;   // a DACP query holds the guard: a bounded wait gives up
int guard_depth=0;
constexpr uint32_t MDNS_TURN_WAIT_MS=20;
namespace modesp {
class MdnsGuard {
 bool held_;
public:
 explicit MdnsGuard(uint32_t wait_ms=UINT32_MAX)
   : held_(!guard_fails && !(guard_busy && wait_ms!=UINT32_MAX)) {
   if(held_) { assert(guard_depth==0); ++guard_depth; }
 }
 ~MdnsGuard() { if(held_) --guard_depth; }
 bool locked() const { return held_; }
};
}
std::vector<int> advertised;
std::vector<bool> output_calls;
std::vector<int> switched;
int xTaskCreatePinnedToCore(void(*task)(void*),const char*,int,void*,int,void*,int) {
 ++tasks_created;
 assert(queued_worker==nullptr && !in_worker && guard_depth==0); // no lifecycle lock held across join
 if(create_fails) return 0;
 queued_worker=task; return pdPASS;
}
void vTaskDelete(void*) { assert(in_worker); }
bool airplay2_started() { return receiver_started; }
bool airplay2_legacy_mode() { return legacy; }
esp_err_t airplay2_set_legacy_mode(bool target) {
 assert(in_worker && guard_depth==0); ++switch_calls; switched.push_back(target ? 1 : 0);
 if(receiver_result==ESP_OK) { legacy=target; receiver_started=true; }
 return receiver_result;
}
esp_err_t airplay2_stop_listening() {
 assert(in_worker && guard_depth==0); ++stop_calls; receiver_started=false;
 return receiver_result;
}
void airplay2_advertise() {
 assert(!in_worker && queued_worker==nullptr && receiver_started && guard_depth==1);
 advertised.push_back(legacy ? 1 : 0);
}
void airplay2_unadvertise() { assert(!in_worker && guard_depth==1); ++unadvertise_calls; }
void airplay2_set_output(bool value) { output_calls.push_back(value); }
float reported_db=-1000.0f;
void airplay2_report_volume(float db) { reported_db=db; }
bool airplay2_pause_sender() { return false; }
namespace modesp { struct BaseModule { struct Command { const char* key=nullptr; int32_t value=0; }; }; }
enum airplay2_remote_t { AIRPLAY2_REMOTE_PLAYPAUSE, AIRPLAY2_REMOTE_NEXT, AIRPLAY2_REMOTE_PREV };
bool remote_ready=false;
std::vector<int> remote_sent;
bool airplay2_remote_ready() { return remote_ready; }
bool airplay2_remote(airplay2_remote_t c) {
 if(!remote_ready) return false;
 remote_sent.push_back((int)c); return true;
}

class AirplayModule {
public:
 int32_t active_mode_=0,attempted_mode_=-1;
 bool mode_in_flight_=false,advertise_dirty_=true,stopping_=false,stop_tried_=false,guard_set_=false;
 bool was_up_=false,was_enabled_=true,was_mine_=false,remote_=false;
 audio_core::VolumeFollow vol_; uint32_t session_seen_=0; int32_t room_seen_=-1;
 uint32_t meta_seen_=0,tick_ms_=0;
 const char* state_="";
 std::vector<int32_t> commands;
 bool next_command(modesp::BaseModule::Command& c) {
   if(commands.empty()) return false;
   c.value=commands.front(); commands.erase(commands.begin()); return true;
 }
 int32_t read_int(const char* key,int32_t fallback) {
   auto p=values.find(key); if(p==values.end()) return fallback;
   auto v=std::get_if<int32_t>(&p->second); return v ? *v : fallback;
 }
 bool read_bool(const char* key,bool fallback) {
   auto p=values.find(key); if(p==values.end()) return fallback;
   auto v=std::get_if<bool>(&p->second); return v ? *v : fallback;
 }
 void read_string(const char* key,char* out,size_t n) {
   auto p=values.find(key); const std::string* v=p==values.end()?nullptr:std::get_if<std::string>(&p->second);
   std::snprintf(out,n,"%s",v?v->c_str():"");
 }
 template<class T> void state_set(const char* key,T value) { values[key]=Value(value); }
 void state_set(const char* key,const char* value) { values[key]=std::string(value); }
 void start_receiver() { ++start_calls; sh.start.store(STARTING); }
 void switch_mode(int32_t selected);
 void stop_listening();
 void publish_mode();
 void on_update(uint32_t dt_ms);
 void follow_source(int32_t source);
 void mirror_events(uint32_t dt_ms,bool mine);
 void apply_remote_cmds();
};
'''

SCENARIOS = r'''
void complete_worker() {
 assert(queued_worker); auto fn=queued_worker; queued_worker=nullptr;
 in_worker=true; fn(nullptr); in_worker=false;
}
AirplayModule fresh() {
 assert(!queued_worker); sh.~Shared(); new(&sh)Shared;
 values.clear(); reported_db=-1000.0f; now_us=1000000; create_fails=false; receiver_started=true; legacy=false;
 assert(guard_depth==0); guard_fails=false; guard_busy=false;
 receiver_result=ESP_OK; tasks_created=switch_calls=unadvertise_calls=start_calls=stop_calls=0;
 advertised.clear(); output_calls.clear(); switched.clear();
 remote_ready=false; remote_sent.clear();
 values["airplay.remote"]=false;                       // what on_init() publishes
 values["airplay.mode"]=int32_t(0); values["airplay.enabled"]=true;
 values["airplay.switching"]=false;
 values["wifi.mode"]=std::string("sta"); values["wifi.connected"]=true;
 values["audio.source"]=int32_t(5); values["audio.volume"]=int32_t(37);
 AirplayModule m; m.on_update(10); advertised.clear(); return m;
}
bool flag(const char* key) { return std::get<bool>(values.at(key)); }
std::string text(const char* key) { return std::get<std::string>(values.at(key)); }
void choose(int mode) { values["airplay.mode"]=int32_t(mode); }

int main() {
 { // Last selection wins, with no overlapping workers or obsolete advertisement.
   auto m=fresh(); choose(1); m.on_update(10);
   assert(queued_worker && flag("airplay.switching") && tasks_created==1);
   choose(0); m.on_update(10); assert(tasks_created==1);
   complete_worker(); m.on_update(10);
   assert(queued_worker && tasks_created==2 && advertised.empty());
   assert(flag("airplay.switching"));
   complete_worker(); m.on_update(10);
   assert(!queued_worker && !flag("airplay.switching"));
   assert((switched==std::vector<int>{1,0}));
   assert((advertised==std::vector<int>{0}));
   assert(std::get<int32_t>(values.at("airplay.active_mode"))==0);
   assert(text("airplay.mode_error").empty());
 }
 { // Offline selection waits and is applied when station connectivity returns.
   auto m=fresh(); values["wifi.connected"]=false; choose(1);
   for(int i=0;i<5;++i)m.on_update(10);
   assert(tasks_created==0 && advertised.empty());
   values["wifi.connected"]=true; m.on_update(10);
   assert(tasks_created==1 && queued_worker);
   complete_worker(); m.on_update(10);
   assert((advertised==std::vector<int>{1}));
 }
 { // Worker error rolls back; no busy retry. Selecting active clears error and rearms.
   auto m=fresh(); receiver_result=ESP_FAIL; choose(1); m.on_update(10);
   complete_worker(); m.on_update(10);
   assert(!text("airplay.mode_error").empty());
   for(int i=0;i<20;++i)m.on_update(10);
   assert(tasks_created==1 && switch_calls==1 && !queued_worker);
   choose(0); m.on_update(10); assert(text("airplay.mode_error").empty());
   receiver_result=ESP_OK; choose(1); m.on_update(10);
   assert(tasks_created==2); complete_worker(); m.on_update(10);
   assert(legacy && text("airplay.mode_error").empty());
 }
 { // Task allocation failure retains the old receiver and does not spin.
   auto m=fresh(); create_fails=true; choose(1); m.on_update(10); m.on_update(10);
   assert(!flag("airplay.switching") && text("airplay.mode_error")=="ESP_ERR_NO_MEM");
   for(int i=0;i<20;++i)m.on_update(10);
   assert(tasks_created==1 && switch_calls==0 && !queued_worker && !legacy);
   choose(0); m.on_update(10); assert(text("airplay.mode_error").empty());
 }
 { // Initial start and mode worker never overlap; latest choice applies after start.
   auto m=fresh(); sh.start.store(NOT_YET); receiver_started=false;
   m.on_update(10); assert(start_calls==1 && sh.start.load()==STARTING);
   choose(1); m.on_update(10); assert(tasks_created==0);
   sh.start.store(UP); receiver_started=true; m.on_update(10);
   assert(tasks_created==1); complete_worker(); m.on_update(10); assert(legacy);
 }
 { // Mode change and an unrelated AirPlay volume event do not touch another source.
   auto m=fresh(); sh.volume_pct.store(93); choose(1); m.on_update(10);
   complete_worker(); m.on_update(10);
   assert(std::get<int32_t>(values.at("audio.source"))==5);
   assert(std::get<int32_t>(values.at("audio.volume"))==37);
   assert(output_calls.empty());
 }
 { // Off during a switch: the switched profile is never announced, the listener stops after
   // it, and on again starts the selected profile and announces it.
   auto m=fresh(); choose(1); m.on_update(10); values["airplay.enabled"]=false;
   m.on_update(10); complete_worker(); m.on_update(10);     // switch done, the stop starts
   assert(advertised.empty() && legacy && queued_worker && unadvertise_calls==2);
   complete_worker(); m.on_update(10);
   assert(!receiver_started && stop_calls==1 && text("airplay.state")=="off");
   assert(text("airplay.mode_status")=="@airplay.st_stopped");
   values["airplay.enabled"]=true; m.on_update(10);          // restart, selected profile
   assert(queued_worker); complete_worker(); m.on_update(10);
   assert(receiver_started && legacy && (advertised==std::vector<int>{1}));
 }
 { // Off: the receiver stops listening, and a sender that starts before the stop is done
   // does not take the output. One attempt per "off", and no restart while off.
   auto m=fresh(); values["airplay.enabled"]=false; m.on_update(10);
   assert(queued_worker && unadvertise_calls==1);
   sh.play_edge.store(true); sh.playing.store(true); m.on_update(10);
   assert(std::get<int32_t>(values.at("audio.source"))==5 && output_calls.empty());
   complete_worker(); m.on_update(10);
   assert(!receiver_started && stop_calls==1 && text("airplay.state")=="off");
   for(int i=0;i<20;++i)m.on_update(10);
   assert(stop_calls==1 && tasks_created==1 && !queued_worker);
 }
 { // Discovery guard allocation failure leaves the current profile and announcement alone.
   auto m=fresh(); guard_fails=true; choose(1); m.on_update(10);
   assert(!flag("airplay.switching") && text("airplay.mode_error")=="ESP_ERR_NO_MEM");
   assert(tasks_created==0 && unadvertise_calls==0 && !queued_worker && !legacy);
   for(int i=0;i<20;++i)m.on_update(10);
   assert(tasks_created==0 && unadvertise_calls==0);
   choose(0); m.on_update(10); assert(text("airplay.mode_error").empty());
   guard_fails=false; choose(1); m.on_update(10);
   assert(tasks_created==1 && unadvertise_calls==1);
   complete_worker(); m.on_update(10); assert(legacy && guard_depth==0);
 }
 { // The transport row reaches the sender only through its DACP remote, in player.cmd's codes.
   auto m=fresh();
   m.commands={4,6}; m.on_update(10);
   assert(!flag("airplay.remote") && remote_sent.empty() && m.commands.empty());  // no remote
   remote_ready=true; sh.playing.store(true);
   m.commands={1,2,3,4,5,6,0,7}; m.on_update(10);
   assert(flag("airplay.remote"));
   assert((remote_sent==std::vector<int>{AIRPLAY2_REMOTE_PLAYPAUSE,AIRPLAY2_REMOTE_PLAYPAUSE,
           AIRPLAY2_REMOTE_NEXT,AIRPLAY2_REMOTE_PREV,AIRPLAY2_REMOTE_PLAYPAUSE}));  // no play
   remote_sent.clear(); sh.playing.store(false);
   m.commands={1,2,3}; m.on_update(10);
   assert((remote_sent==std::vector<int>{AIRPLAY2_REMOTE_PLAYPAUSE}));  // play, not pause or stop
   remote_ready=false; m.on_update(10);
   assert(!flag("airplay.remote"));
   assert(std::get<int32_t>(values.at("audio.source"))==5 && output_calls.empty());
 }
 { // A DACP query holding the mDNS guard: the user-made stop and restart wait for it; only
   // the per-turn announcement gives up and comes back on the next pass. Main never waits on it.
   auto m=fresh(); guard_busy=true; values["airplay.enabled"]=false; m.on_update(10);
   assert(unadvertise_calls==1 && queued_worker);
   complete_worker(); m.on_update(10); assert(!receiver_started);
   values["airplay.enabled"]=true; m.on_update(10);
   assert(queued_worker); complete_worker(); m.on_update(10);
   assert(receiver_started && advertised.empty());
   guard_busy=false; m.on_update(10); assert((advertised==std::vector<int>{0}));
   m.on_update(10); assert((advertised==std::vector<int>{0}) && guard_depth==0);
 }
 auto room=[]{ return std::get<int32_t>(values.at("audio.volume")); };
 { // A sender that took initialVolume sends nothing at the start: its slider is the room's,
   // and every press lands where the slider is: the whole range, slider and panel agreeing.
   auto m=fresh(); values["audio.source"]=int32_t(6); m.on_update(10);   // AirPlay is heard
   assert(reported_db > -19.0f && reported_db < -18.8f);                  // 37 % announced
   sh.session.fetch_add(1); m.on_update(10); m.on_update(3000);
   sh.volume_pct.store(43); m.on_update(10); assert(room()==43);
   sh.volume_pct.store(100); m.on_update(10); assert(room()==100);
   sh.volume_pct.store(0); m.on_update(10); assert(room()==0);
   sh.volume_pct.store(37); m.on_update(10); assert(room()==37);
 }
 { // A sender that restates its own level as it starts: the room keeps its level, and the
   // phone's travel maps onto the room's through it, both ends still reachable.
   auto m=fresh(); values["audio.source"]=int32_t(6); m.on_update(10);
   sh.session.fetch_add(1); sh.volume_pct.store(80); m.on_update(10);
   assert(room()==37);                                                    // not 80
   m.on_update(3000);
   sh.volume_pct.store(90); m.on_update(10); assert(room()==69);          // half way up
   sh.volume_pct.store(100); m.on_update(10); assert(room()==100);        // the top
   sh.volume_pct.store(40); m.on_update(10); assert(room()==19);          // half way down
   sh.volume_pct.store(0); m.on_update(10); assert(room()==0);            // the bottom
 }
 { // The knob moves the room mid-session; it is announced, and the phone counts from there.
   auto m=fresh(); values["audio.source"]=int32_t(6); m.on_update(10);
   sh.session.fetch_add(1); m.on_update(10); m.on_update(3000);
   sh.volume_pct.store(50); m.on_update(10); assert(room()==50);
   values["audio.volume"]=int32_t(30); m.on_update(10);
   assert(reported_db > -21.1f && reported_db < -20.9f);                  // 30 % announced
   sh.volume_pct.store(56); m.on_update(10); assert(room()==38);
   sh.volume_pct.store(50); m.on_update(10); assert(room()==30);
   sh.volume_pct.store(0); m.on_update(10); assert(room()==0);
 }
 { // While another source plays the phone moves nothing, and nothing waits for later.
   auto m=fresh();
   sh.session.fetch_add(1); m.on_update(3010);
   sh.volume_pct.store(100); m.on_update(10); assert(room()==37);
   values["audio.source"]=int32_t(6); m.on_update(10); assert(room()==37); // AirPlay picked
   sh.volume_pct.store(94); m.on_update(10); assert(room()==35);          // a press, from there
 }
 { // A level restated as playback starts is not a press either.
   auto m=fresh(); values["audio.source"]=int32_t(6); m.on_update(10);
   sh.session.fetch_add(1); m.on_update(10); m.on_update(5000);
   sh.play_edge.store(true); sh.playing.store(true); sh.volume_pct.store(60); m.on_update(10);
   assert(room()==37);
   m.on_update(3000);
   sh.volume_pct.store(54); m.on_update(10); assert(room()==33);
 }
 { // A station's title held in the window is shown only once nothing overtook it.
   auto m=fresh(); values["audio.source"]=int32_t(6); m.on_update(10);
   std::strcpy(sh.title,"Earned It"); std::strcpy(sh.artist,"The Weeknd"); sh.meta_gen.fetch_add(1);
   m.on_update(10); assert(text("airplay.title")=="Earned It");
   bool drop=false;   // what on_event does with "Apple Music Hits", no artist, no album
   assert(sh.meta.on_names(true,true,false,now_us,drop)==airplay_meta::Smoother::Names::HOLD);
   std::strcpy(sh.held_title,"Apple Music Hits");
   now_us+=airplay_meta::Smoother::WINDOW_US-1; m.on_update(10);
   assert(text("airplay.title")=="Earned It" && text("airplay.artist")=="The Weeknd");
   now_us+=1; m.on_update(10);                                            // nothing came back
   assert(text("airplay.title")=="Apple Music Hits" && text("airplay.artist").empty());
 }
 std::puts("17 AirplayModule lifecycle scenarios passed");
}
'''


class AirplayModeModuleTests(unittest.TestCase):
    def test_production_module_state_machine(self):
        source = (ROOT / 'modules/airplay/src/airplay_module.cpp').read_text(encoding='utf-8')
        signatures = [
            'uint32_t position_now_locked(', 'void mode_task(', 'float db_from_pct(',
            'void AirplayModule::publish_mode(', 'void AirplayModule::switch_mode(',
            'void AirplayModule::stop_listening(',
            'void AirplayModule::follow_source(', 'void AirplayModule::mirror_events(',
            'void AirplayModule::on_update(', 'void AirplayModule::apply_remote_cmds(',
        ]
        production = '\n\n'.join(function(source, signature) for signature in signatures)
        # The volume rule is its own header (components/audio_dsp/include/volume_follow.h, the
        # one USB follows too): the production class, not a copy of it.
        volume_h = (ROOT / 'components/audio_dsp/include/volume_follow.h').read_text(encoding='utf-8')
        meta_h = (ROOT / 'modules/airplay/include/airplay_meta.h').read_text(encoding='utf-8')
        prefix = PREFIX.replace('@VOLUME_FOLLOW_H@', volume_h.replace('#pragma once', ''))
        prefix = prefix.replace('@AIRPLAY_META_H@', meta_h.replace('#pragma once', ''))
        # No restart stub is supplied: introducing any restart call must fail compilation.
        with tempfile.TemporaryDirectory(prefix='airplay-mode-module-') as directory:
            cpp = Path(directory) / 'module_test.cpp'
            exe = Path(directory) / 'module_test.exe'
            cpp.write_text(prefix + production + SCENARIOS, encoding='utf-8')
            command, env = compiler_command(cpp, exe)
            if any(arg.startswith('/std:') for arg in command):
                command = ['/std:c++17' if arg.startswith('/std:') else arg for arg in command]
                command += ['/TP', '/EHsc', '/utf-8']
            else:
                command = ['-std=c++17' if arg == '-std=c11' else arg for arg in command]
                command += ['-lstdc++']
            built = subprocess.run(command, env=env, cwd=directory, capture_output=True,
                                   text=True, errors='replace', timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], env=env, capture_output=True,
                                 text=True, errors='replace', timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('17 AirplayModule lifecycle scenarios passed', ran.stdout)


if __name__ == '__main__':
    unittest.main()
