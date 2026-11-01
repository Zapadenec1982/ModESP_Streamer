"""«Веб-інтерфейс оновлено · Перезавантажити»: webui/src/lib/update.js in Node.

A tab left open across a flash of data.bin keeps the old page, and its sliders still take a
touch at once, until it is reloaded. The page notes /build.txt the first time its WebSocket is
up and looks again each time it comes back; a different list is said once, and nothing is asked
while the socket is down.
"""

import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(script):
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI update regression"
    lib = (ROOT / "webui/src/lib/update.js").as_uri()
    head = (
        f"import {{ createBuildWatch }} from {json.dumps(lib)};\n"
        "const eq = (a, b, what) => { const x = JSON.stringify(a), y = JSON.stringify(b);"
        " if (x !== y) { console.error(what + ': ' + x + ' != ' + y); process.exit(1); } };\n"
    )
    result = subprocess.run([node, "--input-type=module", "-e", head + script], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_a_new_web_is_said_once_when_the_socket_comes_back():
    run("""
let served = 'aaa  bundle.js.gz\\n', asked = 0, told = 0;
const watch = createBuildWatch(async () => { asked++; return served; }, () => told++);
await watch(false);
eq(asked, 0, 'down: nothing is asked');
await watch(true);
eq([asked, told], [1, 0], 'the first time up: the list this page started with');
await watch(false);
await watch(true);
eq([asked, told], [2, 0], 'back, the same web: nothing said');
served = 'bbb  bundle.js.gz\\n';
await watch(false);
await watch(true);
eq(told, 1, 'back, another web: said');
await watch(false);
await watch(true);
eq([asked, told], [3, 1], 'said once, and not asked again');
""")


def test_a_list_that_cannot_be_read_says_nothing():
    run("""
let served = null, told = 0;
const watch = createBuildWatch(async () => { if (served === 'throw') throw new Error('offline'); return served; },
                               () => told++);
await watch(true);
served = 'throw';
await watch(true);
served = 'aaa\\n';
await watch(true);
eq(told, 0, 'no list, an error, then the first list: that is where it starts');
served = 'bbb\\n';
await watch(true);
eq(told, 1, 'and from there a new one is said');
""")
