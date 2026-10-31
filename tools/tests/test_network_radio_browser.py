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
