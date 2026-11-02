"""The web's WebSocket client against the panel's three slots (components/modesp_net/ws_service.h).

The panel serves three WebSocket clients. A tab nobody looks at should not hold one, so a hidden
tab lets go of its socket and takes a new one when it is shown; and a client the panel has no
slot for is closed with 1013, which the shell shows as "busy" instead of guessing from silence.
These run webui/src/lib/websocket.js and webui/src/stores/state.js under Node with a fake
WebSocket, document and clock.
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
const docL = {}, winL = {};
const add = (bag) => (type, fn) => { (bag[type] ||= []).push(fn); };
const remove = (bag) => (type, fn) => { bag[type] = (bag[type] || []).filter((f) => f !== fn); };
globalThis.document = { visibilityState: 'visible', addEventListener: add(docL), removeEventListener: remove(docL) };
globalThis.window = { addEventListener: add(winL), removeEventListener: remove(winL) };
globalThis.location = { protocol: 'http:', host: 'panel.local' };
function show(visible) {
  document.visibilityState = visible ? 'visible' : 'hidden';
  (docL.visibilitychange || []).forEach((f) => f({}));
}
const fireWin = (type) => (winL[type] || []).forEach((f) => f({}));

const sockets = [];
class FakeWS {
  constructor(url) { this.url = url; this.readyState = 0; this.closedByClient = false; sockets.push(this); }
  open() { this.readyState = 1; this.onopen && this.onopen({}); }
  send(obj) { this.onmessage && this.onmessage({ data: JSON.stringify(obj) }); }
  serverClose(code) { this.readyState = 3; this.onclose && this.onclose({ code }); }
  // A browser reports its own close() through onclose as well; the client must ignore that one.
  close() { this.closedByClient = true; this.readyState = 3; this.onclose && this.onclose({ code: 1005 }); }
}
globalThis.WebSocket = FakeWS;

function check(cond, what) { if (!cond) { console.error('FAILED: ' + what); process.exit(1); } }
"""


def run(script: str) -> None:
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI WebSocket tests"
    result = subprocess.run([node, "--input-type=module", "-e", PRELUDE + script],
                            cwd=WEBUI, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def lib(name: str) -> str:
    return json.dumps((WEBUI / "src" / name).as_uri())


def test_a_hidden_tab_lets_go_of_its_socket_and_reconnects_when_shown():
    run(f"""
const {{ createWebSocket }} = await import({lib("lib/websocket.js")});
const events = [];
createWebSocket((d) => events.push(d));
check(sockets.length === 1 && sockets[0].url === 'ws://panel.local/ws', 'connects at once');
sockets[0].open();
sockets[0].send({{ a: 1 }});

show(false);
check(sockets[0].closedByClient, 'hidden: the socket is closed');
check(!events.some((e) => e._ws_connected === false), 'hidden: no "disconnected" for a tab nobody sees');
advance(120000);
check(sockets.length === 1, 'hidden: nothing reconnects');

show(true);
check(sockets.length === 2, 'shown: a new socket straight away');
sockets[1].open();
check(events.at(-1)._ws_connected === true, 'shown: connected again');

fireWin('pagehide');
check(sockets[1].closedByClient, 'pagehide: the socket is closed');
advance(60000);
check(sockets.length === 2, 'pagehide: nothing reconnects');
fireWin('pageshow');
check(sockets.length === 3, 'pageshow (back from the cache): reconnects');
""")


def test_a_tab_opened_in_the_background_waits_until_it_is_shown():
    run(f"""
document.visibilityState = 'hidden';
const {{ createWebSocket }} = await import({lib("lib/websocket.js")});
createWebSocket(() => {{}});
advance(10000);
check(sockets.length === 0, 'no socket while hidden');
show(true);
check(sockets.length === 1, 'one socket once shown');
fireWin('pageshow');
check(sockets.length === 1, 'pageshow on a live socket opens no second one');
""")


def test_busy_close_retries_steadily_and_other_closes_back_off():
    run(f"""
const {{ createWebSocket, WS_CLOSE_BUSY }} = await import({lib("lib/websocket.js")});
check(WS_CLOSE_BUSY === 1013, 'the firmware code: 1013 Try Again Later');
const events = [];
createWebSocket((d) => events.push(d));
for (let i = 0; i < 4; i++) {{
  const s = sockets.at(-1);
  s.open();
  s.serverClose(1013);
  check(events.at(-1)._ws_connected === false && events.at(-1)._ws_code === 1013, 'busy close is reported with its code');
  const n = sockets.length;
  advance(4999);
  check(sockets.length === n, 'busy: no retry before 5 s');
  advance(1);
  check(sockets.length === n + 1, 'busy: retry at 5 s, every time');
}}
sockets.at(-1).serverClose(1006);
let n = sockets.length;
advance(999); check(sockets.length === n, 'lost: first retry not before 1 s');
advance(1); check(sockets.length === n + 1, 'lost: first retry at 1 s');
sockets.at(-1).serverClose(1006);
n = sockets.length;
advance(1999); check(sockets.length === n, 'lost: then 2 s');
advance(1); check(sockets.length === n + 1, 'lost: second retry at 2 s');
""")


def test_state_store_shows_busy_from_the_close_code_and_from_silence_on_old_firmware():
    run(f"""
const {{ get }} = await import('svelte/store');
const {{ initWebSocket, wsBusy, wsConnected, state }} = await import({lib("stores/state.js")});
initWebSocket();
let s = sockets.at(-1);
s.open();
s.send({{ 'x.y': 1 }});
check(get(wsConnected) && !get(wsBusy) && get(state)['x.y'] === 1, 'snapshot: connected, not busy');

s.serverClose(1013);
check(!get(wsConnected) && get(wsBusy), 'close 1013: busy');
advance(5000);
s = sockets.at(-1);
s.open();
check(get(wsBusy), 'still busy while the retry is open and has said nothing yet');
s.send({{ 'x.y': 2 }});
check(!get(wsBusy) && get(state)['x.y'] === 2, 'the snapshot ends busy');

s.serverClose(1006);
check(!get(wsBusy), 'any other close is not busy');
advance(1000);
s = sockets.at(-1);
s.open();
advance(2999);
check(!get(wsBusy), 'old firmware: not busy before 3 s of silence');
advance(1);
check(get(wsBusy), 'old firmware: an open socket silent for 3 s reads as busy');
""")


def test_heartbeat_is_not_state_and_its_silence_drops_the_socket():
    run(f"""
const {{ createWebSocket }} = await import({lib("lib/websocket.js")});
const events = [];
createWebSocket((d) => events.push(d));
sockets[0].open();
sockets[0].send({{ a: 1 }});
advance(60000);
check(sockets.length === 1, 'no heartbeat yet (older firmware): never timed out');

sockets[0].send({{ _hb: 1 }});
check(!events.some((e) => '_hb' in e), 'the heartbeat is not passed on as state');
advance(14000);
sockets[0].send({{ b: 2 }});
advance(14000);
check(!sockets[0].closedByClient, 'any message keeps it alive');
advance(1000);
check(sockets[0].closedByClient, '15 s silent after a heartbeat: dropped');
check(events.at(-1)._ws_connected === false, 'and reported as disconnected');
advance(1000);
check(sockets.length === 2, 'then reconnects with the usual backoff');
sockets[1].open();
show(false);
advance(60000);
check(sockets.length === 2, 'a released socket is not timed out into a reconnect');
""")
