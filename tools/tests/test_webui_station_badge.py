"""The web's station disc is the glass's: webui/src/lib/stationbadge.js against
modules/ui_lvgl/include/station_badge.h.

The web's Player draws a station without a logo as the glass does — a disc in a colour taken
from the name, with a label made from it — so a station keeps one face on the glass, in the
browser and on its preset buttons. The JavaScript is a copy of the header's rule; here it is run
over every case tests/host/test_station_badge.cpp pins, read out of that file, so a rule changed
on one side and not on the other fails.
"""

import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
LIB = (ROOT / "webui/src/lib/stationbadge.js").as_uri()
HEADER = ROOT / "modules/ui_lvgl/include/station_badge.h"
CASES = ROOT / "tests/host/test_station_badge.cpp"

LIT = r'"(?:[^"\\]|\\.)*"'
LITS = rf"((?:{LIT}\s*)+)"


def run(script):
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI station badge regression"
    result = subprocess.run([node, "--input-type=module", "-e", script], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return json.loads(result.stdout)


def c_string(src):
    """Adjacent C string literals as the compiler joins them, read as the panel reads a name:
    UTF-8, a byte that starts no character as U+FFFD."""
    out = bytearray()
    for lit in re.findall(LIT, src):
        body, i = lit[1:-1], 0
        while i < len(body):
            if body[i] == "\\" and body[i + 1] == "x":
                j = i + 2
                while j < len(body) and body[j] in "0123456789abcdefABCDEF":
                    j += 1
                out.append(int(body[i + 2:j], 16))
                i = j
            elif body[i] == "\\":
                out.extend(body[i + 1].encode())
                i += 2
            else:
                out.extend(body[i].encode())
                i += 1
    return out.decode("utf-8", "replace")


def c_char(src):
    src = src.strip()
    return ord(src[1]) if src.startswith("'") else int(src, 0)


def pinned():
    text = CASES.read_text(encoding="utf-8")
    labels = [(c_string(a), c_string(b)) for a, b in re.findall(rf"CHECK\(label\({LITS}\)\s*==\s*({LIT})\);", text)]
    assert len(labels) + text.count("CHECK(label(nullptr)") == text.count("CHECK(label("), "a label case not read"
    firsts = [(c_string(a), c_string(b))
              for a, b in re.findall(rf"CHECK\(std::string\(make\({LITS}\)\.first\)\s*==\s*({LIT})\);", text)]
    assert len(firsts) == text.count(".first)"), "a first-character case not read"
    colours = [(c_string(a), int(i)) for a, i in re.findall(rf"CHECK\(make\({LITS}\)\.rgb\s*==\s*PALETTE\[(\d+)\]\);", text)]
    capitals = [(c_char(a), c_char(b)) for a, b in re.findall(r"CHECK\(upper\(([^)]+)\)\s*==\s*([^)]+)\);", text)]
    assert len(capitals) == text.count("CHECK(upper("), "a capital case not read"
    return labels, firsts, colours, capitals


def test_the_palette_is_the_headers():
    header = HEADER.read_text(encoding="utf-8")
    body = re.search(r"PALETTE\[\] = \{(.*?)\};", header, re.S).group(1)
    want = [int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]+", body)]
    got = run(f"import {{ PALETTE }} from {json.dumps(LIB)}; console.log(JSON.stringify(PALETTE));")
    assert got == want


def test_every_pinned_case_of_the_glass():
    labels, firsts, colours, capitals = pinned()
    # the colours the C++ test pins for another implementation of the rule
    assert {name: i for name, i in colours} == {
        "SomaFM Groove Salad": 7, "SomaFM Drone Zone": 4, "Radio Paradise Main": 2,
        "Radio Paradise Mellow": 3, "Хіт FM": 3, "": 5,
    }
    got = run(f"""import {{ stationBadge, upper, PALETTE }} from {json.dumps(LIB)};
const hex = (i) => '#' + PALETTE[i].toString(16).padStart(6, '0');
console.log(JSON.stringify({{
  labels: {json.dumps([n for n, _ in labels])}.map((n) => stationBadge(n).label),
  firsts: {json.dumps([n for n, _ in firsts])}.map((n) => stationBadge(n).first),
  colours: {json.dumps([[n, i] for n, i in colours])}.map(([n, i]) => stationBadge(n).color === hex(i)),
  capitals: {json.dumps([c for c, _ in capitals])}.map(upper),
}}));""")
    assert dict(zip([n for n, _ in labels], got["labels"])) == dict(labels)
    assert dict(zip([n for n, _ in firsts], got["firsts"])) == dict(firsts)
    assert got["colours"] == [True] * len(colours)
    assert got["capitals"] == [c for _, c in capitals]


def test_nothing_given_is_an_empty_disc():
    got = run(f"""import {{ stationBadge }} from {json.dumps(LIB)};
console.log(JSON.stringify([stationBadge(null), stationBadge(undefined), stationBadge('Kiss FM')]));""")
    assert got[0] == got[1] == {"label": "", "first": "", "color": "#006f9e"}
    assert got[2]["label"] == "KISS" and got[2]["first"] == "K"
