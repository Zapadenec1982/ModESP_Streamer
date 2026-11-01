"""The web's EQ edits (webui/src/lib/eq.js) under Node, against a fake device.

The device keeps one EQ document with a revision (components/audio_dsp/src/eq_store.cpp): an edit
names the revision it was made from, and one from any other is refused with 409. The page sends
one edit at a time and queues the newest behind it. Leaving or hiding the page asks the device to
write what it has at once (keep). With a request still on its way, the queued bands went with the
revision from before it: the device refused them, so the last edit of a drag was lost, the keep
with it, and the page went on showing bands the device never got.
"""

import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
WEBUI = ROOT / "webui"

PRELUDE = """
globalThis.sessionStorage = { getItem: () => null, setItem: () => {}, removeItem: () => {} };

// The device: a request takes effect when it arrives; its answer comes back at once, or, while
// `holding`, only when the test releases it (the request is on its way back).
const dev = { rev: 1, bands: [], kept: null, refused: 0 };
const sent = [];
const held = [];
let holding = false;
globalThis.fetch = (url, opts = {}) => {
  const body = opts.body ? JSON.parse(opts.body) : null;
  sent.push({ body, keepalive: !!opts.keepalive });
  let status = 200, reply;
  if (!body) reply = { rev: dev.rev, fs: 48000, preset: 'custom', presets: [], bands: dev.bands };
  else if (body.keep && !body.bands) { dev.kept = dev.bands; reply = { rev: dev.rev }; }
  else if (body.rev !== dev.rev) { status = 409; dev.refused++; reply = { error: 'changed', rev: dev.rev }; }
  else {
    dev.bands = body.bands;
    dev.rev++;
    if (body.keep) dev.kept = dev.bands;
    reply = { rev: dev.rev };
  }
  const res = { ok: status === 200, status, json: async () => reply, text: async () => JSON.stringify(reply) };
  return new Promise((resolve) => (holding ? held.push(() => resolve(res)) : resolve(res)));
};
function release() { holding = false; held.splice(0).forEach((answer) => answer()); }

function check(cond, what) { if (!cond) { console.error('FAILED: ' + what); process.exit(1); } }
// every promise the page chains has run once the next macrotask comes
const settle = () => new Promise((r) => setImmediate(r));
const band = (gain) => ({ type: 'peak', f: 1000, gain, q: 1, on: true });

const { get } = await import('svelte/store');
const { createEq } = await import(%s);
let conflicts = 0;
const errors = [];
const eq = createEq({ onConflict: () => conflicts++, onError: (e) => errors.push(e.message) });
await eq.load();
"""


def run(script: str) -> None:
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI EQ tests"
    lib = json.dumps((WEBUI / "src" / "lib" / "eq.js").as_uri())
    result = subprocess.run([node, "--input-type=module", "-e", PRELUDE % lib + script],
                            cwd=WEBUI, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_a_keep_mid_drag_waits_for_the_revision_on_its_way():
    run("""
holding = true;
eq.setBands([band(1)]);              // on its way; the device took it
await settle();
eq.setBands([band(2)]);              // queued behind it, the newest wins
eq.setBands([band(3)]);
eq.keep();                           // the page is hidden before the answer comes back
await settle();
release();
await settle();
check(dev.refused === 0, 'nothing was sent from a revision the device had left');
check(dev.bands[0].gain === 3, 'the last edit reached the device');
check(dev.kept && dev.kept[0].gain === 3, 'and the device kept it');
const d = get(eq.doc);
check(d.bands[0].gain === 3 && d.rev === dev.rev, 'the page shows what the device has, at its revision');
check(conflicts === 0 && errors.length === 0, 'no conflict of the page with itself');
""")


def test_a_keep_waits_for_the_one_edit_on_its_way():
    run("""
holding = true;
eq.setBands([band(4)]);
await settle();
eq.keep();                           // the edit has not come back yet
await settle();
check(dev.kept === null, 'not kept before the edit landed');
release();
await settle();
check(dev.kept && dev.kept[0].gain === 4, 'kept once it did');
""")


def test_a_keep_with_nothing_on_its_way_goes_at_once():
    run("""
eq.setBands([band(5)]);
await settle();
eq.keep();
const last = sent[sent.length - 1];
check(last.body.keep === true && last.body.bands === undefined && last.keepalive, 'one keep, past the page');
check(dev.kept && dev.kept[0].gain === 5, 'the device kept it');
const n = sent.length;
eq.keep();
check(sent.length === n, 'nothing new sent, nothing more to keep');
""")
