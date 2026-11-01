"""The web's sections and addresses.

Three sections — Плеєр · Джерело · Налаштування — on the web and on the glass, in the same order:
the web's NAV (lib/nav.js) is the panel's drawer (ui_shell.cpp PAGES, the first N_MENU). DSP left
the sections for Налаштування › Звук › Еквалайзер, and a bookmark of an old address still lands
on its page (lib/routes.js).
"""

import json
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WEBUI = ROOT / "webui" / "src"


def node(script: str) -> str:
    exe = shutil.which("node")
    assert exe, "Node.js is required for the WebUI checks"
    r = subprocess.run([exe, "--input-type=module", "-e", script], capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


def test_old_addresses_land_on_their_pages():
    lib = (WEBUI / "lib" / "routes.js").as_uri()
    out = json.loads(node(f"""
import {{ canonical }} from {json.dumps(lib)};
const cases = [['dsp'], ['service'], ['service', 'calibration', 'port'],
               ['system', 'wifi'], ['system', 'sound', 'eq'], ['player']];
console.log(JSON.stringify(cases.map((c) => {{ const r = canonical(c); return [r, r === c]; }})));
"""))
    assert out == [
        [["system", "sound", "eq"], False],
        [["system", "service"], False],
        [["system", "service", "calibration", "port"], False],
        # the section kept its id: its addresses never moved, and come back as they were
        [["system", "wifi"], True],
        [["system", "sound", "eq"], True],
        [["player"], True],
    ]


def test_the_web_sections_are_the_panel_drawer():
    shell = (ROOT / "modules/ui_lvgl/src/ui_shell.cpp").read_text(encoding="utf-8")
    n_menu = int(re.search(r"constexpr int N_MENU = (\d+);", shell).group(1))
    pages = re.findall(r"\{ &UI_SCREEN_\w+,\s+S::(\w+),\s+Kind::(\w+)", shell)
    drawer = [key for key, _ in pages[:n_menu]]
    assert [kind for _, kind in pages[:n_menu]] == ["HOME"] + ["SECTION"] * (n_menu - 1)
    assert "SECTION" not in [kind for _, kind in pages[n_menu:]], "a section outside the drawer"

    nav = (WEBUI / "lib" / "nav.js").read_text(encoding="utf-8")
    web = re.findall(r"\{ id: '(\w+)', icon: '\w+', key: K\('([\w.]+)'\) \}", nav)
    assert [key.replace(".", "_") for _, key in web] == drawer
    router = (WEBUI / "stores" / "router.js").read_text(encoding="utf-8")
    sections = re.search(r"export const SECTIONS = \[([^\]]*)\];", router).group(1)
    assert re.findall(r"'(\w+)'", sections) == [i for i, _ in web]
    shell_css = (WEBUI / "shell" / "Shell.svelte").read_text(encoding="utf-8")
    assert f"grid-template-columns: repeat({len(web)}, minmax(0, 1fr));" in shell_css
