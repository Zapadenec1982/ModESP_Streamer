"""The web's setting writes (webui/src/lib/settings.js) under Node, with a fake fetch and clock.

A slider's sender sends while it moves (the first value at once, then the newest every interval,
one request at a time); a typed one after a pause. A value the device refuses is taken back from
the page. A page that goes sends what is still held (cleanup) — or drops it on purpose (cancel):
«Лишити як було» once ended the loudness trial through a sender whose pending 0 was dropped, and
the trial played on for the device's five minutes. Writes that end something go through
writeNow(): at once, and taken back from the page when the device does not take them.
"""

import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
WEBUI = ROOT / "webui"

PRELUDE = """
let now = 0, nextId = 1;
const timers = new Map();
globalThis.setTimeout = (fn, ms) => { const id = nextId++; timers.set(id, { at: now + (ms || 0), fn }); return id; };
globalThis.clearTimeout = (id) => { timers.delete(id); };
function advance(ms) {
  const end = now + ms;
  for (;;) {
    let next = null;
    for (const [id, t] of timers) if (t.at <= end && (!next || t.at < next[1].at)) next = [id, t];
    if (!next) break;
    timers.delete(next[0]);
    now = next[1].at;
    next[1].fn();
  }
  now = end;
}
const noop = () => {};
globalThis.document = { visibilityState: 'visible', addEventListener: noop, removeEventListener: noop };
globalThis.window = { addEventListener: noop, removeEventListener: noop };
globalThis.location = { protocol: 'http:', host: 'panel.local' };
globalThis.sessionStorage = { getItem: () => null, setItem: noop, removeItem: noop };

// The device: every POST is recorded; `refuse` makes it answer 500.
const posts = [];
let refuse = false;
globalThis.fetch = async (url, opts) => {
  posts.push({ url, body: JSON.parse(opts.body) });
  if (refuse) return { status: 500, ok: false, text: async () => 'busy', json: async () => ({}) };
  return { status: 200, ok: true, text: async () => '{}', json: async () => ({ ok: true }) };
};

function check(cond, what) { if (!cond) { console.error('FAILED: ' + what); process.exit(1); } }
// the fake fetch resolves on the microtask queue: let it land
const settle = async () => { for (let i = 0; i < 20; i++) await Promise.resolve(); };
"""


def run(script: str) -> None:
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI settings tests"
    result = subprocess.run([node, "--input-type=module", "-e", PRELUDE + script],
                            cwd=WEBUI, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def lib(name: str) -> str:
    return json.dumps((WEBUI / "src" / name).as_uri())


def test_cleanup_sends_what_is_held():
    run(f"""
const {{ createSettingSender }} = await import({lib("lib/settings.js")});
const bass = createSettingSender('audio.bass_db', {{ debounceMs: 120 }});
bass.send(3);
bass.cleanup();                       // the sheet closed within 120 ms
await settle();
check(posts.length === 1 && posts[0].body['audio.bass_db'] === 3, 'the value the page showed is sent');
""")


def test_a_live_sender_sends_while_it_moves():
    run(f"""
const {{ createSettingSender }} = await import({lib("lib/settings.js")});
const vol = createSettingSender('audio.volume', {{ debounceMs: 150, live: true }});
vol.send(10);
await settle();
check(posts.length === 1 && posts[0].body['audio.volume'] === 10, 'the first value goes at once');
vol.send(11); vol.send(12); vol.send(13);
await settle();
check(posts.length === 1, 'then not before the interval');
advance(150);
await settle();
check(posts.length === 2 && posts[1].body['audio.volume'] === 13, 'the newest at the next slot');
advance(150);
await settle();
check(posts.length === 2, 'nothing new, nothing sent');
""")


def test_a_typed_sender_waits_for_a_pause():
    run(f"""
const {{ createSettingSender }} = await import({lib("lib/settings.js")});
const name = createSettingSender('bt.name', {{ debounceMs: 500 }});
name.send('M'); advance(200); name.send('MO'); advance(200); name.send('MOD');
await settle();
check(posts.length === 0, 'no letter goes while typing');
advance(500);
await settle();
check(posts.length === 1 && posts[0].body['bt.name'] === 'MOD', 'the whole name after the pause');
""")


def test_a_refused_value_is_taken_back():
    run(f"""
const {{ get }} = await import('svelte/store');
const {{ state, setStateKey }} = await import({lib("stores/state.js")});
const {{ createSettingSender }} = await import({lib("lib/settings.js")});
setStateKey('ui.backlight', 60);
refuse = true;
const bl = createSettingSender('ui.backlight', {{ debounceMs: 150, live: true }});
bl.send(90);
check(get(state)['ui.backlight'] === 90, 'shown at once');
await settle();
check(get(state)['ui.backlight'] === 60, 'refused: the device value again');
""")


def test_write_now_posts_at_once_and_shows_it():
    run(f"""
const {{ get }} = await import('svelte/store');
const {{ state, setStateKey }} = await import({lib("stores/state.js")});
const {{ writeNow }} = await import({lib("lib/settings.js")});
setStateKey('audio.port_hz', 62);
const done = writeNow({{ 'audio.port_hz': 0 }});
check(posts.length === 1, 'posted with no timer to wait for');
check(JSON.stringify(posts[0].body) === '{{"audio.port_hz":0}}', 'the keys as given');
check(get(state)['audio.port_hz'] === 0, 'shown at once');
await done;
check(get(state)['audio.port_hz'] === 0, 'and kept once written');
""")


def test_write_now_takes_it_back_when_the_device_refuses():
    run(f"""
const {{ get }} = await import('svelte/store');
const {{ state, setStateKey }} = await import({lib("stores/state.js")});
const {{ writeNow }} = await import({lib("lib/settings.js")});
setStateKey('audio.spk_type', 1);
setStateKey('audio.port_hz', 0);
refuse = true;
const p = writeNow({{ 'audio.spk_type': 0, 'audio.port_hz': 62, 'audio.new_key': 5 }});
check(get(state)['audio.port_hz'] === 62, 'shown at once');
// the device sends a newer type meanwhile: that one stays
setStateKey('audio.spk_type', 2);
let failed = null;
try {{ await p; }} catch (e) {{ failed = e; }}
check(failed && /busy/.test(failed.message), 'the error is thrown to the page');
check(get(state)['audio.port_hz'] === 0, 'what was shown before comes back');
check(get(state)['audio.new_key'] === undefined, 'a key that was not there is not left behind');
check(get(state)['audio.spk_type'] === 2, 'a newer value from the device is not overwritten');
""")


def test_a_tone_the_device_refuses_is_taken_back():
    """lib/tone.js writes through writeNow(): a tone the device did not start is not left on the
    page as playing — the shell's tone bar and the source cards read audio.source."""
    run(f"""
const {{ get }} = await import('svelte/store');
const {{ state, setStateKey }} = await import({lib("stores/state.js")});
const {{ playTone, CHAIN }} = await import({lib("lib/tone.js")});
setStateKey('audio.source', 4);
refuse = true;
let failed = null;
try {{ await playTone({{ mode: CHAIN, hz: 40 }}, {{ extra: {{ 'audio.cal_vol_cap': 20 }} }}); }} catch (e) {{ failed = e; }}
check(failed && /busy/.test(failed.message), 'the error is thrown to the step');
check(posts.length === 1 && posts[0].body['audio.source'] === 1, 'the tone was asked for');
check(get(state)['audio.source'] === 4, 'the source that played is shown again');
check(get(state)['audio.cal_vol_cap'] === undefined, 'the step keys are not left behind');
""")
