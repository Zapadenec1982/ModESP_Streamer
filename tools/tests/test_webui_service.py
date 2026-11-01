"""The web's Service page against the panel's.

The thresholds in modules/ui_lvgl/include/service_readout.h are the soak's pass/fail criteria;
the web shows the same numbers and must colour them the same. webui/src/lib/service.js is a port,
so it is run here against the header itself: a small C++ program prints every verdict and format
over the same inputs, Node prints the port's, and the two must agree to the character.
"""

import json
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
WEBUI = ROOT / "webui" / "src"

GRADES = {
    # name in service_readout.h -> (name in service.js, inputs)
    "cpu": ("cpu", list(range(-5, 121))),
    "loop_ms": ("loopMs", list(range(-5, 81))),
    "stack_free": ("stackFree", list(range(0, 4200, 3))),
    "dma_free": ("dmaFree", list(range(0, 200 * 1024, 61))),
    "internal_free": ("internalFree", list(range(0, 200 * 1024, 61))),
    "rtt_ms": ("rttMs", list(range(-5, 401))),
    "counter": ("counter", list(range(-3, 10))),
    "soft_counter": ("softCounter", list(range(-3, 10))),
}
BYTES = [-1, 0, 1, 900, 1023, 1024, 10239, 10240, 10241, 99999, 1048575, 1048576, 1048577,
         5 * 1048576 + 12345, 23946191, 2147483647]
UPTIME = [-1, 0, 59, 3599, 3600, 86399, 86400, 90061, 86400 * 10000 + 5, 2147483647]
GAIN = [-5, 0, 1, 9, 10, 15, 999, 1000, 5000]
BOOTS = ["", "power_on", "software", "panic", "int_wdt", "task_wdt", "wdt", "brownout",
         "deep_sleep", "unknown", "something_new"]
FLOATS = [-1e12, -3.5, -2.5, -0.5, -0.4, 0.0, 0.04, 0.05, 0.06, 0.45, 0.5, 1.25, 3.14159, 59.5,
          84.49, 84.5, 99.85, 99.9, 99.95, 150.0, 3e9]


def extract(text: str, start: str, end: str) -> str:
    i = text.index(start)
    j = text.index(end, i) + len(end)
    return text[i:j]


def compiler():
    found = shutil.which("g++")
    if found:
        return Path(found)
    mingw = Path("C:/msys64/ucrt64/bin/g++.exe")
    assert mingw.exists(), "A host C++ compiler is required for the Service verdicts"
    return mingw


def panel_readings(tmp_path: Path) -> dict:
    """Every verdict and format, as the panel's own code computes them."""
    svc = (ROOT / "modules/ui_lvgl/src/screen_svc.cpp").read_text(encoding="utf-8")
    base = (ROOT / "components/modesp_core/include/modesp/base_module.h").read_text(encoding="utf-8")
    # the limiter's conversion and read_number's, verbatim: a change there is a change here
    lim = extract(svc, "const int32_t tenths = ", ";")
    conv = extract(base, "const float f = *fp;", "f - 0.5f);")
    ints = lambda xs: ", ".join(str(x) for x in xs)
    floats = ", ".join(f"{x!r}f" for x in FLOATS)
    grades = "\n".join(
        f'  {{ const int32_t v[] = {{{ints(xs)}}}; out("{c}"); for (auto x : v) printf("%d,", (int)svc::{c}(x)); end(); }}'
        for c, (_, xs) in GRADES.items())
    src = f"""
#include "service_readout.h"
#include <cstdint>
#include <cstdio>
static bool first = true;
static void out(const char* k) {{ printf("%s\\"%s\\":[", first ? "" : ",", k); first = false; }}
static void end() {{ printf("null]"); }}
static int32_t read_number_of(float value) {{
  const float* fp = &value;
  const int32_t def = 0;
  {conv}
}}
int main() {{
  printf("{{");
{grades}
  {{ out("boot_grade"); const char* v[] = {{{", ".join(json.dumps(b) for b in BOOTS)}}};
     for (auto x : v) printf("%d,", (int)svc::boot_grade(x)); end(); }}
  {{ out("bytes"); const int32_t v[] = {{{ints(BYTES)}}};
     for (auto x : v) {{ char b[32]; svc::bytes(x, b, sizeof b); printf("\\"%s\\",", b); }} end(); }}
  {{ out("uptime"); const int32_t v[] = {{{ints(UPTIME)}}};
     for (auto x : v) {{ char b[32]; svc::uptime(x, b, sizeof b); printf("\\"%s\\",", b); }} end(); }}
  {{ out("gain_reduction"); const int32_t v[] = {{{ints(GAIN)}}};
     for (auto x : v) {{ char b[32]; svc::gain_reduction(x, b, sizeof b); printf("\\"%s\\",", b); }} end(); }}
  {{ out("kib"); const int32_t v[] = {{{ints(BYTES)}}};
     for (auto x : v) printf("%ld,", (long)svc::kib(x)); end(); }}
  {{ out("lim_tenths"); const float v[] = {{{floats}}};
     for (auto gr : v) {{ {lim.replace("s.lim_gr_db", "gr")} printf("%ld,", (long)tenths); }} end(); }}
  {{ out("read_number"); const float v[] = {{{floats}}};
     for (auto x : v) printf("%ld,", (long)read_number_of(x)); end(); }}
  printf("}}\\n");
  return 0;
}}
"""
    cpp = tmp_path / "svc.cpp"
    exe = tmp_path / ("svc.exe" if os.name == "nt" else "svc")
    cpp.write_text(src, encoding="utf-8")
    cc = compiler()
    env = os.environ.copy()
    env["PATH"] = str(cc.parent) + os.pathsep + env.get("PATH", "")
    # service_readout.h writes units and the decimal separator in the panel's language
    # (modules/ui_lvgl/include/i18n.h), so the runtime and its tables come along; Ukrainian is on.
    r = subprocess.run([str(cc), "-std=c++17", "-O0", "-Wall", "-Wno-format-truncation", str(cpp),
                        str(ROOT / "modules/ui_lvgl/src/i18n.cpp"),
                        str(ROOT / "modules/ui_lvgl/src/i18n_tables.cpp"),
                        "-o", str(exe), "-I" + str(ROOT / "modules/ui_lvgl/include"),
                        "-I" + str(ROOT / "components/modesp_core/include")],
                       capture_output=True, text=True, env=env, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    r = subprocess.run([str(exe)], capture_output=True, env=env, timeout=30)
    assert r.returncode == 0, r.stderr
    got = json.loads(r.stdout.decode("utf-8"))
    return {k: v[:-1] for k, v in got.items()}      # the trailing null


def node(script: str) -> str:
    exe = shutil.which("node")
    assert exe, "Node.js is required for the WebUI Service checks"
    r = subprocess.run([exe, "--input-type=module", "-e", script], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def test_verdicts_and_formats_match_the_panel(tmp_path):
    panel = panel_readings(tmp_path)
    lib = (WEBUI / "lib" / "service.js").as_uri()
    inputs = {c: xs for c, (_, xs) in GRADES.items()}
    script = f"""
import * as s from {json.dumps(lib)};
const inputs = {json.dumps(inputs)};
const names = {json.dumps({c: js for c, (js, _) in GRADES.items()})};
const out = {{}};
for (const [c, xs] of Object.entries(inputs)) out[c] = xs.map((x) => s[names[c]](x));
out.boot_grade = {json.dumps(BOOTS)}.map(s.bootGrade);
const U = {{ b: 'Б', kb: 'КБ', mb: 'МБ' }};
// The panel's Ukrainian, with its decimal comma.
out.bytes = {json.dumps(BYTES)}.map((b) => s.bytes(b, U, ','));
out.uptime = {json.dumps(UPTIME)}.map((x) => s.uptime(x, 'д'));
out.gain_reduction = {json.dumps(GAIN)}.map((x) => s.gainReduction(x, ',') + ' дБ');
out.kib = {json.dumps(BYTES)}.map(s.kib);
out.lim_tenths = {json.dumps(FLOATS)}.map(s.limTenths);
out.read_number = {json.dumps(FLOATS)}.map(s.num);
console.log(JSON.stringify(out));
"""
    web = json.loads(node(script))
    assert set(web) == set(panel)
    for key in panel:
        diff = [(i, a, b) for i, (a, b) in enumerate(zip(panel[key], web[key])) if a != b]
        assert len(panel[key]) == len(web[key]) and not diff, f"{key}: panel vs web differ at {diff[:5]}"


# ── calibration (docs/AUDIO.md, "Test tone", "Speaker calibration: port frequency") ──

def test_the_tone_keys_match_the_firmware():
    """The test tone as lib/calib.js names it: the manifest's defaults and the firmware's timeout.
    The amplifier ceiling's arithmetic left with its page."""
    lib = (WEBUI / "lib" / "calib.js").as_uri()
    out = json.loads(node(f"""
import * as c from {json.dumps(lib)};
console.log(JSON.stringify({{ defaults: c.TONE_DEFAULTS, calMax: c.CAL_MAX_S }}));
"""))
    manifest = json.loads((ROOT / "modules/audio/manifest.json").read_text(encoding="utf-8"))["state"]
    for key, field in (("audio.tone_mode", "mode"), ("audio.tone_hz", "hz"),
                       ("audio.tone_db10", "db10"), ("audio.tone_ch", "ch")):
        assert out["defaults"][field] == manifest[key]["default"], key
    audio = (ROOT / "modules/audio/src/audio_module.cpp").read_text(encoding="utf-8")
    assert f"TONE_RAW_MAX_MS = {out['calMax'] * 1000};" in audio


def test_a_test_tone_leaves_nothing_saved():
    """The manual port search changes no saved setting. The port off and the volume cap ride
    in keys the device never saves and drops with the tone; a tone through the chain goes off by
    itself, as a calibration tone always did."""
    manifest = json.loads((ROOT / "modules/audio/manifest.json").read_text(encoding="utf-8"))["state"]
    for key in ("audio.cal_port_off", "audio.cal_vol_cap"):
        assert not manifest[key].get("persist"), f"{key} must not be saved"
    assert manifest["audio.cal_port_off"]["default"] is False and manifest["audio.cal_vol_cap"]["default"] == 0
    page = (WEBUI / "pages" / "service" / "CalibrationPage.svelte").read_text(encoding="utf-8")
    assert "'audio.cal_port_off': true" in page and "'audio.cal_vol_cap': 20" in page
    assert "'audio.port_hz': 0" not in page and "extra['audio.volume']" not in page
    audio = (ROOT / "modules/audio/src/audio_module.cpp").read_text(encoding="utf-8")
    assert "TONE_CHAIN_IDLE_MS = 120000;" in audio


def test_calibration_page_and_shell_keep_the_rules():
    """The amplifier's ceiling is set nowhere in the web («Максимум» is the one limit); the tone
    bar is the shell's, so a tone shows, and stops, on every page."""
    import re
    for p in WEBUI.rglob("*"):
        if p.suffix in (".svelte", ".js") and p.is_file():
            text = p.read_text(encoding="utf-8")
            for key in ("audio.amp_clip_db10", "audio.drive_db", "audio.rms_cap_db10"):
                assert f"'{key}'" not in text or p.name == "longtail.js", f"{p.relative_to(WEBUI)} sets {key}"
    shell = (WEBUI / "shell" / "Shell.svelte").read_text(encoding="utf-8")
    assert "import('./ToneBar.svelte')" in shell and "<svelte:component this={ToneBar} />" in shell
    # the shell fetches the bar when audio.source turns to the tone, without importing lib/tone.js
    tone = (WEBUI / "lib" / "tone.js").read_text(encoding="utf-8")
    assert re.search(r"const SRC_TONE = (\d+);", shell).group(1) == re.search(r"export const SRC_TONE = (\d+);", tone).group(1)
    others = [str(p.relative_to(WEBUI)) for p in WEBUI.rglob("*.svelte")
              if p.name != "Shell.svelte" and "ToneBar" in p.read_text(encoding="utf-8")]
    assert not others, f"a second tone bar: {others}"


# ── the long tail (lib/longtail.js) ──

# Not hand-written pages: the generated pages' renderer and its widgets, and the dev showcase.
NOT_HANDWRITTEN = ("components/", "pages/DynamicPage.svelte", "pages/ComponentsPage.svelte",
                   "stores/mqttForm.js", "stores/wifiForm.js", "stores/ui.js", "lib/longtail.js")


def generated_pages():
    """data/ui.json's pages as the build makes them for this board (main(), without writing)."""
    import sys
    sys.path.insert(0, str(ROOT / "tools"))
    from generate_ui import (DriverManifestLoader, DriverManifestValidator, FeatureResolver,
                             ManifestLoader, ManifestValidator, UIJsonGenerator, load_capabilities)
    project = json.loads((ROOT / "project.json").read_text(encoding="utf-8"))
    manifests = ManifestLoader(ROOT / "modules", ManifestValidator()).load_all(project["modules"])
    drivers, cap_index = DriverManifestLoader(ROOT / "drivers", DriverManifestValidator()).load_required(manifests)
    board_dir = ROOT / "boards" / "p4_player"          # sdkconfig.defaults.esp32p4
    board = json.loads((board_dir / "board.json").read_text(encoding="utf-8"))
    bindings = json.loads((board_dir / "bindings.json").read_text(encoding="utf-8"))
    resolver = FeatureResolver(bindings, [m for m in manifests if m.get("requires")])
    ui = UIJsonGenerator().generate(project, manifests, drivers, board, bindings, resolver,
                                    load_capabilities(), cap_index)
    return ui["pages"]


def test_long_tail_accounts_for_every_generated_widget():
    import re
    pages = generated_pages()
    lib = (WEBUI / "lib" / "longtail.js").as_uri()
    js = json.loads(node(f"""
import * as l from {json.dumps(lib)};
const pages = {json.dumps(pages)};
console.log(JSON.stringify({{ shown: l.SHOWN, via: l.VIA, dropped: l.DROPPED, skipped: l.SKIPPED_PAGES,
                              tail: l.longTail(pages) }}));
"""))
    generated = {w["key"] for p in pages if p["id"] not in js["skipped"] for c in p["cards"] for w in c["widgets"]}
    via = [k for keys in js["via"].values() for k in keys]
    listed = js["shown"] + via + js["dropped"]
    assert len(listed) == len(set(listed)), "a key is in two lists"
    assert not set(listed) - generated, f"no generated widget has these keys: {sorted(set(listed) - generated)}"

    text = "\n".join(p.read_text(encoding="utf-8") for p in WEBUI.rglob("*")
                     if p.suffix in (".svelte", ".js") and p.is_file()
                     and not str(p.relative_to(WEBUI)).replace(os.sep, "/").startswith(NOT_HANDWRITTEN))
    quoted = lambda s: re.search(r"""['"`]""" + re.escape(s) + r"""['"`]""", text)
    unseen = [k for k in js["shown"] if not quoted(k)]
    assert not unseen, f"SHOWN but no hand-written page reads them: {unseen}"
    unseen = [e for e in js["via"] if not quoted(e)]
    assert not unseen, f"VIA endpoints no hand-written page calls: {unseen}"
    stray = sorted(k for k in generated - set(listed) if quoted(k))
    assert not stray, f"a hand-written page uses these, so they would show twice — list them in SHOWN: {stray}"

    tail = {w["key"] for p in js["tail"] for c in p["cards"] for w in c["widgets"]}
    assert tail == generated - set(listed)
    assert not {p["id"] for p in js["tail"]} & set(js["skipped"])
