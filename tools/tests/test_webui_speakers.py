"""Колонки: webui/src/lib/speakers.js in Node.

What must hold: a measurement is not applied without «Прийняти» or «Застосувати», «Пропустити» at
any step writes nothing, and neither does the choice of step 1. Also what the speakers page says
for each state, what the port meter's messages mean to the wizard, and the usual volume's scale.
"""

import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(script):
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI speakers regression"
    lib = (ROOT / "webui/src/lib/speakers.js").as_uri()
    head = f"import * as sp from {json.dumps(lib)};\nconst eq = (a, b, what) => {{ const x = JSON.stringify(a), y = JSON.stringify(b); if (x !== y) {{ console.error(what + ': ' + x + ' != ' + y); process.exit(1); }} }};\n"
    result = subprocess.run([node, "--input-type=module", "-e", head + script], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_wizard_writes_only_on_done_or_accept():
    run("""
for (const type of [sp.SPK_UNSET, sp.SPK_PORTED, sp.SPK_CLOSED])
  for (const action of ['choose', 'next', 'skip', 'keep', 'stop', 'retry'])
    eq(sp.wizardWrites(type, action, 62), null, `${type} ${action}`);
eq(sp.wizardWrites(sp.SPK_CLOSED, 'done'), { 'audio.spk_type': 2 }, 'closed, done');
eq(sp.wizardWrites(sp.SPK_PORTED, 'done', 62), null, 'ported, done: there is no «Готово» for a port');
eq(sp.wizardWrites(sp.SPK_PORTED, 'accept', 62.4),
   { 'audio.spk_type': 1, 'audio.port_hz': 62, 'audio.port_src': 3 }, 'ported, accept');
eq(sp.wizardWrites(sp.SPK_CLOSED, 'accept', 62), null, 'a closed speaker accepts no port');
eq(sp.wizardWrites(sp.SPK_PORTED, 'accept', 25), null, 'under 30 Hz is not a port this panel keeps');
eq(sp.wizardWrites(sp.SPK_PORTED, 'accept', 95), null, 'over 80 Hz neither');
eq(sp.wizardWrites(sp.SPK_PORTED, 'accept', NaN), null, 'nothing found, nothing to accept');
""")


def test_the_calibration_page_applies_with_where_it_came_from():
    run("""
eq(sp.portWrites(62.6, true), { 'audio.spk_type': 1, 'audio.port_hz': 63, 'audio.port_src': 3 }, 'measured');
eq(sp.portWrites(45, false), { 'audio.spk_type': 1, 'audio.port_hz': 45, 'audio.port_src': 1 }, 'by hand');
eq(sp.portWrites(20, false)['audio.port_hz'], 30, 'held to 30');
eq(sp.portWrites(120, true)['audio.port_hz'], 80, 'held to 80');
""")


def test_what_the_speakers_page_says():
    run("""
const st = (o) => { const r = sp.speakerState(o); return [r.title, r.line, r.args.hz ?? null, r.again]; };
// a line only where there is a port to say: the title says the rest (18.10.2026, no notes)
eq(st({}), ['spk.unset', '', null, false], 'nothing asked, no port');
eq(st({ 'audio.port_hz': 62, 'audio.port_src': 1 }), ['spk.unset', 'spk.port_manual', 62, false],
   'a port set before the wizard still plays, and is said');
eq(st({ 'audio.spk_type': 1, 'audio.port_hz': 62, 'audio.port_src': 3 }), ['spk.ported', 'spk.port_measured', 62, true], 'measured');
eq(st({ 'audio.spk_type': 1, 'audio.port_hz': 55, 'audio.port_src': 1 }), ['spk.ported', 'spk.port_manual', 55, true], 'by hand');
eq(st({ 'audio.spk_type': 1 }), ['spk.ported', 'spk.port_none', 0, true], 'ported, no port yet');
eq(st({ 'audio.spk_type': 2, 'audio.port_hz': 62, 'audio.port_src': 3 }), ['spk.closed', '', null, true],
   'closed: the kept number does not play');
eq(sp.port({ 'audio.spk_type': 2, 'audio.port_hz': 62 }), { hz: 0, measured: false }, 'closed has no port');
""")


def test_the_port_meters_words():
    run("""
const U = '\\x1f';
eq(sp.meterOutcome('@cal.m_progress' + U + 'hz=45' + U + 'k=21' + U + 'n=41', true), 'running', 'measuring');
eq(sp.meterOutcome('@cal.m_found' + U + 'hz=62', false), 'found', 'found');
eq(sp.meterOutcome('@cal.m_doubtful' + U + 'hz=62', false), 'doubtful', 'doubtful');
for (const k of ['@cal.m_outside' + U + 'hz=95', '@cal.m_no_dip', '@cal.m_no_tone', '@cal.m_mic_open',
                 '@cal.m_mic_read', '@cal.m_no_memory'])
  eq(sp.meterOutcome(k, false), 'failed', k);
for (const k of ['', '@cal.m_stopped', '@cal.m_interrupted', '@cal.m_silence'])
  eq(sp.meterOutcome(k, false), 'idle', k || 'nothing');
eq(sp.subUnder(62), 50, 'the subsonic under a 62 Hz port');
""")


def test_the_usual_volume_scale():
    run("""
eq(sp.refOf(42), 42, 'no limit: the volume itself');
eq(sp.refOf(50, 85), 43, '«Максимум» 85 %: half the scale is 42.5 % of the level');
eq(sp.refOf(10), 20, 'held to 20');
eq(sp.refOf(100, 100), 100, 'the top');
eq(sp.refShown(43, 85), 51, 'said on the volume scale');
eq(sp.refShown(42), 42, 'no limit');
""")
