"""Run current production network functions against deterministic transport mocks."""

import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = Path(__file__).parent / "fixtures"


def extract(source: str, signature: str) -> str:
    start = source.index(signature)
    end = source.index("\n}", start) + 2
    return source[start:end]


def compiler():
    found = shutil.which("g++")
    if found:
        return Path(found)
    mingw = Path("C:/msys64/ucrt64/bin/g++.exe")
    assert mingw.exists(), "A host C++ compiler is required for network regressions"
    return mingw


def build_and_run(tmp_path: Path, source: str, includes: list[Path]):
    cpp = tmp_path / "network.cpp"
    exe = tmp_path / ("network.exe" if os.name == "nt" else "network")
    cpp.write_text(source, encoding="utf-8")
    cc = compiler()
    env = os.environ.copy()
    env["PATH"] = str(cc.parent) + os.pathsep + env.get("PATH", "")
    command = [str(cc), "-std=c++17", "-O0", str(cpp), "-o", str(exe)]
    command += ["-I" + str(path) for path in includes]
    result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
    assert result.returncode == 0, result.stdout + result.stderr
    result = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr + f"exit={result.returncode}"


def test_radio_http_icy_station_and_seek(tmp_path):
    source = (ROOT / "modules/radio/src/radio_module.cpp").read_text(encoding="utf-8")
    harness = (FIXTURES / "radio_network_harness.cpp").read_text(encoding="utf-8")
    signatures = (
        "bool split_url(", "bool next_line(", "bool header_is(", "void copy_header(",
        "Hop http_open_hop(", "bool http_open(", "int stream_read(", "void start_station(",
        "bool seek_stream(", "bool looks_like_playlist(",
    )
    # One hop per call and a loop around them: a redirect handled by recursion took the
    # 12 KB radio stack past its end on a four-redirect CDN chain (04.10.2026).
    assert "http_open(" not in extract(source, "Hop http_open_hop("), "a hop must not open the next one"
    assert extract(source, "bool http_open(").count("http_open(") == 1, "redirects must not recurse"
    for signature in signatures:
        harness = harness.replace(extract(harness, signature), extract(source, signature))
    harness = harness.replace("int main(){", extract(source, "void handle_seek(") + "\nint main(){")
    build_and_run(tmp_path, harness, [ROOT / "modules/radio/include"])


def test_dlna_refresh_cancel_back(tmp_path):
    source = (ROOT / "components/dlna_client/src/dlna_browser.cpp").read_text(encoding="utf-8")
    harness = (FIXTURES / "browser_network_harness.cpp").read_text(encoding="utf-8")
    for signature in ("void status(", "void publish(", "void discover(", "void process("):
        harness = harness.replace(extract(harness, signature), extract(source, signature))
    harness = harness.replace("int browse_selected=999;",
                              extract(source, "bool browse_target_valid(") + "\nint browse_selected=999;")
    build_and_run(tmp_path, harness, [ROOT / "components/dlna_client/include"])


def test_eq_revision_survives_theme_rebuild(tmp_path):
    source = (ROOT / "modules/ui_lvgl/src/screen_eq.cpp").read_text(encoding="utf-8")
    harness = """
#include <cstdint>
struct Config { int gain = 0; };
struct Eq {
  Config cfg, custom;
  int preset = 0, sel = 0;
  uint32_t revision = 0, pushed_rev = 0, store_rev = 0;
  void* widget = nullptr;
};
Eq g;
bool eq_model_loaded = false;
""" + extract(source, "bool reset_eq_widgets(") + """
int main() {
  g.cfg.gain = 3;
  if (reset_eq_widgets() || g.cfg.gain != 0) return 1;
  eq_model_loaded = true;
  g.cfg.gain = 5; g.custom.gain = 2; g.preset = 4; g.sel = 3;
  g.revision = 1; g.pushed_rev = 1; g.store_rev = 7; g.widget = &g;
  if (!reset_eq_widgets() || g.widget || g.cfg.gain != 5 ||
      g.custom.gain != 2 || g.preset != 4 || g.sel != 3 ||
      g.revision != 1 || g.pushed_rev != 1 || g.store_rev != 7) return 2;
  ++g.revision; // the edit after a theme rebuild
  if (!reset_eq_widgets() || g.revision != 2 || g.pushed_rev != 1) return 3;
  return 0;
}
"""
    build_and_run(tmp_path, harness, [])


def test_eq_edit_after_rebuild_reaches_the_store_once(tmp_path):
    screen = (ROOT / "modules/ui_lvgl/src/screen_eq.cpp").read_text(encoding="utf-8")
    module = (ROOT / "modules/ui_lvgl/src/ui_lvgl_module.cpp").read_text(encoding="utf-8")
    harness = """
#include <cstdint>
namespace audio_core {
struct EqDoc { int gain = 0; };
struct EqStore {
  uint32_t rev = 1; int puts = 0, last = 0;
  uint32_t put(const EqDoc& d) { ++puts; last = d.gain; return ++rev; }
};
EqStore& eq_store() { static EqStore s; return s; }
}
struct Config { int gain = 0; };
struct Eq {
  Config cfg, custom;
  int preset = 0, sel = 0;
  uint32_t revision = 0, pushed_rev = 0, store_rev = 0;
  void* widget = nullptr;
};
Eq g;
bool eq_model_loaded = false;
""" + extract(screen, "bool reset_eq_widgets(") + """
bool lvgl_port_lock(int) { return true; }
void lvgl_port_unlock() {}
void ui_eq_copy_doc(audio_core::EqDoc& out) { out.gain = g.cfg.gain; }
uint32_t ui_eq_revision() { return g.revision; }
""" + extract(screen, "void ui_eq_pushed(") + """
struct UiLvglModule {
  uint32_t eq_pending_rev_ = 0, eq_live_rev_ = 0;
  void persist_eq();
};
""" + extract(module, "void UiLvglModule::persist_eq(") + """
int main() {
  reset_eq_widgets(); eq_model_loaded = true;
  UiLvglModule mod;
  audio_core::EqStore& st = audio_core::eq_store();
  g.revision = 1; g.cfg.gain = 4;
  // into the store on the next tick, once: hearing and keeping it are the audio module's
  mod.eq_pending_rev_ = 1; mod.persist_eq(); mod.persist_eq();
  if (st.puts != 1 || st.last != 4 || g.pushed_rev != 1 || g.store_rev != 2) return 1;
  if (!reset_eq_widgets() || g.revision != 1 || g.store_rev != 2) return 2;
  g.revision = 2; g.cfg.gain = 6;
  mod.eq_pending_rev_ = 2; mod.persist_eq(); mod.persist_eq();
  if (st.puts != 2 || st.last != 6 || g.pushed_rev != 2 || g.store_rev != 3) return 3;
  return 0;
}
"""
    build_and_run(tmp_path, harness, [])


def test_eq_owner_hears_at_once_and_writes_when_out_of_the_way(tmp_path):
    module = (ROOT / "modules/audio/src/audio_module.cpp").read_text(encoding="utf-8")
    harness = """
#include <cstdint>
#include <cstdio>
#include <cstring>
#define ESP_LOGI(...)
#define ESP_LOGE(...)
const char* TAG = "Audio";
int64_t now_us = 0;
int64_t esp_timer_get_time() { return now_us; }
namespace audio_core {
struct EqSection { int bands[1]{}; int count = 1; };
struct EqConfig { EqSection sec[1]; };
struct EqDoc { EqConfig cfg; };
struct EqStore {
  uint32_t rev = 1, saved = 1; bool keep_ = false; int64_t web = 0; int saves = 0;
  uint32_t revision() const { return rev; }
  uint32_t get(EqDoc& d) const { d.cfg.sec[0].bands[0] = (int)rev; return rev; }
  bool dirty() const { return rev != saved; }
  bool take_keep() { const bool k = keep_; keep_ = false; return k; }
  int64_t web_touched_at() const { return web; }
  bool save(const char*, const char*) { saved = rev; ++saves; return true; }
};
EqStore& eq_store() { static EqStore s; return s; }
struct Bus { int published = 0, last = 0; void publish(const int* b, int) { ++published; last = b[0]; } };
Bus& eq_bus() { static Bus b; return b; }
}
namespace modesp { struct UserStore {
  static void write_path(const char* n, char* out, size_t sz) { std::snprintf(out, sz, "%s", n); }
}; }
audio_core::EqDoc& eq_scratch() { static audio_core::EqDoc d; return d; }
constexpr int64_t EQ_WEB_QUIET_MS = 60000;
enum class CmdType { EQ_RELOAD };
const char* g_screen = "now";
struct AudioModule {
  uint32_t eq_heard_rev_ = 1; int reloads = 0; int32_t eq_rev_state = 0;
  void send(CmdType, int32_t) { ++reloads; }
  void state_set(const char*, int32_t v) { eq_rev_state = v; }
  void read_string(const char*, char* out, size_t n) { std::snprintf(out, n, "%s", g_screen); }
  void own_eq();
};
""" + extract(module, "void AudioModule::own_eq(") + """
int main() {
  AudioModule m;
  audio_core::EqStore& st = audio_core::eq_store();
  audio_core::Bus& bus = audio_core::eq_bus();
  m.own_eq();
  if (bus.published || st.saves) return 1;                     // nothing moved since boot
  // the panel's page: heard at once, kept once the page is left
  g_screen = "eq"; st.rev = 2;
  m.own_eq(); m.own_eq();
  if (bus.published != 1 || bus.last != 2 || m.reloads != 1 || m.eq_rev_state != 2 || st.saves) return 2;
  g_screen = "now"; m.own_eq();
  if (st.saves != 1) return 3;
  // the web dragging: heard at once, written after a quiet minute
  now_us = 100000000; st.rev = 3; st.web = 100000;
  m.own_eq();
  if (bus.published != 2 || st.saves != 1) return 4;
  now_us = 159000000; m.own_eq(); if (st.saves != 1) return 5;
  now_us = 160000000; m.own_eq(); if (st.saves != 2) return 6;
  // the web page left: at once
  now_us = 170000000; st.rev = 4; st.web = 170000; st.keep_ = true;
  m.own_eq(); if (st.saves != 3) return 7;
  // asked while the panel's page is in front: waits for it, and is not lost
  g_screen = "eq"; st.rev = 5; st.keep_ = true;
  m.own_eq(); if (st.saves != 3 || !st.keep_) return 8;
  g_screen = "standby"; m.own_eq(); if (st.saves != 4) return 9;
  // nothing to write: the ask is spent
  st.keep_ = true; m.own_eq(); if (st.keep_ || st.saves != 4) return 10;
  return 0;
}
"""
    build_and_run(tmp_path, harness, [])


def test_board_endpoint_sends_complete_document_or_error(tmp_path):
    source = (ROOT / "components/modesp_net/src/http_service.cpp").read_text(encoding="utf-8")
    handler = extract(source, "esp_err_t HttpService::handle_get_board(")
    board_path = (tmp_path / "board.json").as_posix()
    handler = handler.replace('"/data/board.json"', '"' + board_path + '"')
    harness = """
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, HTTPD_404_NOT_FOUND = 404,
              HTTPD_500_INTERNAL_SERVER_ERROR = 500, MALLOC_CAP_8BIT = 0;
struct ConfigService { static constexpr size_t MAX_JSON_SIZE = 4096; };
struct httpd_req_t { int status = 200; std::string body; };
void* heap_caps_malloc(size_t n, int) { return malloc(n); }
void heap_caps_free(void* p) { free(p); }
void set_cors_headers(httpd_req_t*) {}
void httpd_resp_set_type(httpd_req_t*, const char*) {}
int httpd_resp_send_err(httpd_req_t* req, int status, const char* message) {
  req->status = status; req->body = message; return ESP_OK;
}
int httpd_resp_send(httpd_req_t* req, const char* data, size_t len) {
  req->body.assign(data, len); return ESP_OK;
}
namespace modesp {
struct HttpService {
  static bool check_auth(httpd_req_t*) { return true; }
  static esp_err_t handle_get_board(httpd_req_t*);
};
""" + handler + "\n}\n" + """
int main() {
  const char* path = """ + '"' + board_path + '"' + """;
  const std::string expected = std::string("{\\"board\\":\\"") + std::string(3650, 'x') + "\\"}";
  { std::ofstream f(path, std::ios::binary); f << expected; }
  httpd_req_t req;
  if (modesp::HttpService::handle_get_board(&req) != ESP_OK ||
      req.status != 200 || req.body != expected || req.body.size() <= 1024) return 1;
  { std::ofstream f(path, std::ios::binary); f << std::string(4097, 'x'); }
  req = {};
  if (modesp::HttpService::handle_get_board(&req) != ESP_FAIL || req.status != 500) return 2;
  return 0;
}
"""
    build_and_run(tmp_path, harness, [])


# The framework's HTTP handlers, compiled as they are against a stub server and a stub store.
HTTPD_STUBS = r"""
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include "jsmn.h"
#include "modesp/net/json_escape.h"
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1;
enum { HTTPD_400_BAD_REQUEST = 400, HTTPD_500_INTERNAL_SERVER_ERROR = 500 };
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
struct httpd_req_t { void* user_ctx = nullptr; std::string body; int status = 200; std::string resp; };
int httpd_req_recv(httpd_req_t* req, char* buf, size_t len) {
  const size_t n = req->body.size() < len ? req->body.size() : len;
  memcpy(buf, req->body.data(), n);
  req->body.erase(0, n);
  return (int)n;
}
int httpd_resp_send_err(httpd_req_t* req, int status, const char* msg) {
  req->status = status; req->resp = msg; return ESP_OK;
}
int httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
int httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
int httpd_resp_sendstr(httpd_req_t* req, const char* s) { req->resp = s; return ESP_OK; }
int httpd_resp_send(httpd_req_t* req, const char* s, long n) {
  req->resp.assign(s, n < 0 ? strlen(s) : (size_t)n); return ESP_OK;
}
"""

SETTINGS_STUBS = r"""
namespace etl { template <class T, class V> auto get_if(V* v) { return std::get_if<T>(v); } }
namespace modesp {
namespace gen {
struct StateMeta { const char* key; const char* type; bool writable; bool persist; float min_val; float max_val; };
constexpr StateMeta STATE_META[] = {
  {"ui.backlight", "int", true, true, 0, 100},
  {"audio.volume", "int", true, true, 0, 100},
  {"audio.mute", "bool", true, true, 0, 0},
  {"radio.cmd", "int", true, false, 0, 4},
  {"radio.web_name", "string", true, false, 0, 0},
  {"system.heap", "int", false, false, 0, 0},
};
constexpr size_t STATE_META_COUNT = sizeof(STATE_META) / sizeof(STATE_META[0]);
inline const StateMeta* find_state_meta(const char* key) {
  for (const auto& m : STATE_META) if (!strcmp(m.key, key)) return &m;
  return nullptr;
}
}  // namespace gen
using Value = std::variant<int32_t, float, bool, std::string>;
struct SharedState {
  std::map<std::string, Value> kv;
  void set(const char* k, int32_t v) { kv[k] = v; }
  void set(const char* k, float v) { kv[k] = v; }
  void set(const char* k, bool v) { kv[k] = v; }
  void set(const char* k, const char* v) { kv[k] = std::string(v); }
  std::optional<Value> get(const char* k) const {
    auto it = kv.find(k);
    if (it == kv.end()) return std::nullopt;
    return it->second;
  }
};
struct ModuleManager { std::string posted; };
static bool post_if_command(ModuleManager* m, const char* key, int32_t value) {
  if (strcmp(key, "radio.cmd")) return false;
  m->posted += std::string(key) + "=" + std::to_string(value) + ";";
  return true;
}
static void log_clamp(const char*, const char*, double, double, const gen::StateMeta*) {}
struct HttpService {
  SharedState* state_ = nullptr;
  ModuleManager* modules_ = nullptr;
  static bool check_auth(httpd_req_t*) { return true; }
  static void set_cors_headers(httpd_req_t*) {}
  static esp_err_t handle_post_settings(httpd_req_t*);
  static esp_err_t handle_get_backup(httpd_req_t*);
};
"""


def test_settings_take_a_value_of_the_keys_type_unescaped(tmp_path):
    source = (ROOT / "components/modesp_net/src/http_service.cpp").read_text(encoding="utf-8")
    handlers = "\n".join(extract(source, signature) for signature in (
        "static bool value_fits(",
        "esp_err_t HttpService::handle_post_settings(",
        "esp_err_t HttpService::handle_get_backup(",
    ))
    harness = HTTPD_STUBS + SETTINGS_STUBS + handlers + "\n}\n" + r"""
using namespace modesp;
static httpd_req_t post(HttpService& svc, const char* body) {
  httpd_req_t req;
  req.user_ctx = &svc;
  req.body = body;
  HttpService::handle_post_settings(&req);
  return req;
}
int main() {
  SharedState st;
  ModuleManager mm;
  HttpService svc;
  svc.state_ = &st;
  svc.modules_ = &mm;
  // a string is stored as it reads, its JSON escapes undone
  httpd_req_t r = post(svc, R"({"radio.web_name":"Radio \"Rock\" \\ А"})");
  auto name = st.get("radio.web_name");
  if (r.status != 200 || !name || std::get<std::string>(*name) != "Radio \"Rock\" \\ \xd0\x90") return 1;
  // a value of another type is refused, and the key keeps what it held
  st.set("audio.volume", (int32_t)30);
  for (const char* bad : {R"({"audio.volume":"50"})", R"({"audio.volume":true})",
                          R"({"audio.volume":null})", R"({"audio.mute":1})",
                          R"({"radio.web_name":5})", R"({"radio.web_name":"\u0000"})",
                          R"({"system.heap":1})"}) {
    if (post(svc, bad).status != 400) return 2;
  }
  if (std::get<int32_t>(*st.get("audio.volume")) != 30 || st.get("audio.mute")) return 3;
  // a command posted as a string is neither stored nor run
  r = post(svc, R"({"radio.cmd":"2"})");
  if (r.status != 400 || st.get("radio.cmd") || !mm.posted.empty()) return 4;
  // the rest of a request goes in around a refused pair
  r = post(svc, R"({"audio.volume":40,"radio.cmd":2,"audio.mute":true,"radio.web_name":[1]})");
  if (r.status != 200 || std::get<int32_t>(*st.get("audio.volume")) != 40 ||
      mm.posted != "radio.cmd=2;" || !std::get<bool>(*st.get("audio.mute"))) return 5;

  // a backup leaves out a value of another type, and its comma with it
  SharedState bs;
  HttpService backup;
  backup.state_ = &bs;
  bs.set("ui.backlight", "80");          // the first persist key, held as a string
  bs.set("audio.volume", (int32_t)40);
  bs.set("audio.mute", true);
  httpd_req_t req;
  req.user_ctx = &backup;
  HttpService::handle_get_backup(&req);
  if (req.resp != R"({"modesp_backup":1,"keys":{"audio.volume":40,"audio.mute":true}})") return 6;
  return 0;
}
"""
    build_and_run(tmp_path, harness,
                  [ROOT / "components/jsmn", ROOT / "components/modesp_net/include"])


def test_auth_reads_back_a_name_with_quotes_as_json(tmp_path):
    source = (ROOT / "components/modesp_net/src/http_service.cpp").read_text(encoding="utf-8")
    harness = r"""
#include <cstdio>
#include <cstring>
#include <string>
#include "modesp/net/json_escape.h"
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, HTTPD_500_INTERNAL_SERVER_ERROR = 500;
constexpr long HTTPD_RESP_USE_STRLEN = -1;
struct httpd_req_t { int status = 200; std::string resp; };
int httpd_resp_send_err(httpd_req_t* req, int status, const char*) { req->status = status; return ESP_OK; }
int httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
int httpd_resp_send(httpd_req_t* req, const char* s, long n) {
  req->resp.assign(s, n < 0 ? strlen(s) : (size_t)n); return ESP_OK;
}
namespace modesp {
static char s_auth_user[32] = "admin";
static bool s_auth_enabled = true;
struct HttpService {
  static void set_cors_headers(httpd_req_t*) {}
  static esp_err_t handle_get_auth(httpd_req_t*);
};
""" + extract(source, "esp_err_t HttpService::handle_get_auth(") + "\n}\n" + r"""
int main() {
  strcpy(modesp::s_auth_user, "my\"box\\");
  httpd_req_t req;
  if (modesp::HttpService::handle_get_auth(&req) != ESP_OK ||
      req.resp != R"({"enabled":true,"username":"my\"box\\"})") return 1;
  return 0;
}
"""
    build_and_run(tmp_path, harness, [ROOT / "components/modesp_net/include"])


WIFI_STUBS = r"""
#include <atomic>
#define ESP_LOGE(...) ((void)0)
int httpd_resp_set_status(httpd_req_t* req, const char* status) { req->status = atoi(status); return ESP_OK; }
namespace modesp {
std::map<std::string, std::string> g_nvs;     // "namespace/key" -> value
namespace nvs_helper {
// as nvs_get_str: a value that does not fit max_len with its NUL fails and leaves `out` as it was
bool read_str(const char* ns, const char* key, char* out, size_t max_len) {
  auto it = g_nvs.find(std::string(ns) + "/" + key);
  if (it == g_nvs.end() || it->second.size() + 1 > max_len) return false;
  memcpy(out, it->second.c_str(), it->second.size() + 1);
  return true;
}
bool write_str(const char* ns, const char* key, const char* value) {
  g_nvs[std::string(ns) + "/" + key] = value; return true;
}
bool read_i32(const char*, const char*, int32_t&) { return false; }
bool write_i32(const char*, const char*, int32_t) { return true; }
}  // namespace nvs_helper
inline const uint8_t* device_mac() { static const uint8_t mac[6] = {0, 1, 2, 3, 0xab, 0xcd}; return mac; }
struct WiFiService {
  static size_t read_ap_password(char (&out)[64]);
  bool is_trying() const { return false; }
  bool save_credentials(const char* ssid, const char* pass) {
    return nvs_helper::write_str("wifi", "ssid", ssid) && nvs_helper::write_str("wifi", "pass", pass);
  }
};
struct HttpService {
  WiFiService* wifi_ = nullptr;
  char wifi_join_ssid_[33] = {};
  char wifi_join_pass_[65] = {};
  std::atomic<bool> wifi_join_{false};
  std::atomic<bool> wifi_forget_{false};
  static bool check_auth(httpd_req_t*) { return true; }
  static void set_cors_headers(httpd_req_t*) {}
  static esp_err_t handle_post_wifi(httpd_req_t*);
  static esp_err_t handle_get_wifi_ap(httpd_req_t*);
  static esp_err_t handle_post_wifi_ap(httpd_req_t*);
};
"""


def test_wifi_credentials_held_to_what_the_radio_takes(tmp_path):
    http = (ROOT / "components/modesp_net/src/http_service.cpp").read_text(encoding="utf-8")
    wifi = (ROOT / "components/modesp_net/src/wifi_service.cpp").read_text(encoding="utf-8")
    handlers = "\n".join([extract(wifi, "size_t WiFiService::read_ap_password(")] + [
        extract(http, signature) for signature in (
            "esp_err_t HttpService::handle_post_wifi(",
            "esp_err_t HttpService::handle_get_wifi_ap(",
            "esp_err_t HttpService::handle_post_wifi_ap(",
        )])
    harness = HTTPD_STUBS + WIFI_STUBS + handlers + "\n}\n" + r"""
using namespace modesp;
static int call(esp_err_t (*handler)(httpd_req_t*), HttpService& svc, const std::string& body) {
  httpd_req_t req;
  req.user_ctx = &svc;
  req.body = body;
  handler(&req);
  return req.status;
}
static std::string json_body(const std::string& ssid, const std::string& pass, const char* extra = "") {
  return "{\"ssid\":\"" + ssid + "\",\"password\":\"" + pass + "\"" + extra + "}";
}
int main() {
  WiFiService wifi;
  HttpService svc;
  svc.wifi_ = &wifi;
  const std::string s32(32, 's'), s33(33, 's'), p63(63, 'p'), p64(64, 'a'), p65(65, 'a');
  // the access point: an SSID of up to 32 bytes, a WPA2 passphrase of 8..63 or none
  if (call(HttpService::handle_post_wifi_ap, svc, json_body("ap", p64)) != 400) return 1;
  if (call(HttpService::handle_post_wifi_ap, svc, json_body(s33, "")) != 400) return 2;
  if (call(HttpService::handle_post_wifi_ap, svc, json_body("ap", "short")) != 400) return 3;
  if (g_nvs.count("wifi/ap_pass")) return 4;
  if (call(HttpService::handle_post_wifi_ap, svc, json_body(s32, p63)) != 200 ||
      g_nvs["wifi/ap_pass"] != p63) return 5;
  // a network to save, as to join: an SSID of up to 32 bytes, a password of up to 64
  if (call(HttpService::handle_post_wifi, svc, json_body(s33, "secret12")) != 400) return 6;
  if (call(HttpService::handle_post_wifi, svc, json_body("net", p65)) != 400) return 7;
  if (call(HttpService::handle_post_wifi, svc, json_body(s33, "secret12", ",\"join\":true")) != 400) return 8;
  if (g_nvs.count("wifi/ssid")) return 9;
  if (call(HttpService::handle_post_wifi, svc, json_body(s32, p64)) != 200 ||
      g_nvs["wifi/ssid"] != s32 || g_nvs["wifi/pass"] != p64) return 10;
  // a password stored longer than WPA2 takes is read, cut to 63, and reported as set
  g_nvs["wifi/ap_pass"] = std::string(70, 'x');
  char pass[64];
  if (WiFiService::read_ap_password(pass) != 70 || std::string(pass) != std::string(63, 'x')) return 11;
  httpd_req_t req;
  req.user_ctx = &svc;
  HttpService::handle_get_wifi_ap(&req);
  if (req.resp.find("\"password_set\":true") == std::string::npos) return 12;
  g_nvs["wifi/ap_pass"] = "";
  req = httpd_req_t{};
  req.user_ctx = &svc;
  HttpService::handle_get_wifi_ap(&req);
  if (req.resp.find("\"password_set\":false") == std::string::npos) return 13;
  return 0;
}
"""
    build_and_run(tmp_path, harness,
                  [ROOT / "components/jsmn", ROOT / "components/modesp_net/include"])


def test_late_routes_wait_for_a_held_server(tmp_path):
    source = (ROOT / "components/modesp_net/src/http_service.cpp").read_text(encoding="utf-8")
    code = "\n".join(extract(source, signature) for signature in (
        "static SemaphoreHandle_t s_parked",
        "void HttpService::unregister_static_handler(",
        "void HttpService::register_static_handler(",
    ))
    harness = r"""
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using esp_err_t = int;
constexpr int ESP_OK = 0;
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
const char* esp_err_to_name(esp_err_t) { return "error"; }
// FreeRTOS binary semaphores
struct Sem { std::mutex m; std::condition_variable cv; bool given = false; };
using SemaphoreHandle_t = Sem*;
constexpr int pdTRUE = 1;
constexpr unsigned portMAX_DELAY = 0xffffffffu;
SemaphoreHandle_t xSemaphoreCreateBinary() { return new Sem; }
int xSemaphoreGive(SemaphoreHandle_t s) {
  std::lock_guard<std::mutex> l(s->m); s->given = true; s->cv.notify_all(); return pdTRUE;
}
int xSemaphoreTake(SemaphoreHandle_t s, unsigned) {
  std::unique_lock<std::mutex> l(s->m); s->cv.wait(l, [&] { return s->given; }); s->given = false;
  return pdTRUE;
}
// httpd: one task runs queued work and requests in order, between requests, as httpd's does
enum httpd_method_t { HTTP_GET = 1 };
struct httpd_req_t {};
struct httpd_uri_t {
  const char* uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t*); void* user_ctx;
  bool is_websocket;
};
using httpd_handle_t = void*;
std::mutex q_mutex, table_mutex;
std::condition_variable q_cv;
std::deque<std::function<void()>> queue_;
std::vector<std::string> table = {"/api/state", "/*"};
std::vector<std::string> served;
void post(std::function<void()> f) {
  std::lock_guard<std::mutex> l(q_mutex); queue_.push_back(std::move(f)); q_cv.notify_all();
}
esp_err_t httpd_queue_work(httpd_handle_t, void (*fn)(void*), void* arg) {
  post([=] { fn(arg); }); return ESP_OK;
}
esp_err_t httpd_unregister_uri_handler(httpd_handle_t, const char* uri, httpd_method_t) {
  std::lock_guard<std::mutex> l(table_mutex);
  for (auto it = table.begin(); it != table.end(); ++it) if (*it == uri) { table.erase(it); break; }
  return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t* u) {
  std::lock_guard<std::mutex> l(table_mutex); table.push_back(u->uri); return ESP_OK;
}
void page_request() {                      // a page: the server looks for the wildcard
  post([] {
    std::lock_guard<std::mutex> l(table_mutex);
    bool found = false;
    for (const auto& r : table) found = found || r == "/*";
    served.push_back(found ? "200" : "404");
  });
}
namespace modesp {
struct HttpService {
  httpd_handle_t server_ = nullptr;
  static esp_err_t handle_static(httpd_req_t*) { return ESP_OK; }
  void unregister_static_handler();
  void register_static_handler();
};
""" + code + "\n}\n" + r"""
int main() {
  bool stop = false;
  std::thread server([&] {
    for (;;) {
      std::function<void()> f;
      {
        std::unique_lock<std::mutex> l(q_mutex);
        q_cv.wait(l, [] { return !queue_.empty(); });
        f = std::move(queue_.front());
        queue_.pop_front();
      }
      f();
      if (stop) return;
    }
  });
  modesp::HttpService http;
  http.unregister_static_handler();           // the wildcard comes off: the server holds still
  page_request();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  bool waited;
  { std::lock_guard<std::mutex> l(table_mutex); waited = served.empty(); }
  httpd_uri_t late{"/rootDesc.xml", HTTP_GET, nullptr, nullptr, false};
  httpd_register_uri_handler(nullptr, &late);
  http.register_static_handler();              // back on: the server goes on, and finds it
  for (int i = 0; i < 200; i++) {
    { std::lock_guard<std::mutex> l(table_mutex); if (!served.empty()) break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  post([&] { stop = true; });
  server.join();
  if (!waited) return 1;                       // it was served while the wildcard was off
  return served == std::vector<std::string>{"200"} ? 0 : 2;
}
"""
    build_and_run(tmp_path, harness, [])
