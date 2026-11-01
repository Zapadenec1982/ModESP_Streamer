"""Filters as text, the equalizer page's edits and the whole system's curve, in Node:
webui/src/lib/eqfile.js, lib/eqband.js and lib/syscurve.js.

Import and export in the text Equalizer APO and REW use, up to
10 bands, held to the panel's limits. What must hold: import -> export gives the same bands. Also what the
other dialects look like when they come in (AutoEQ, REW's Generic export, APO's own forms), what
is counted and not read, and the page's add / delete / reset on a set.
"""

import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(script):
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI EQ file regression"
    eqfile = (ROOT / "webui/src/lib/eqfile.js").as_uri()
    eqband = (ROOT / "webui/src/lib/eqband.js").as_uri()
    syscurve = (ROOT / "webui/src/lib/syscurve.js").as_uri()
    head = (
        f"import * as F from {json.dumps(eqfile)};\n"
        f"import * as B from {json.dumps(eqband)};\n"
        f"import * as Y from {json.dumps(syscurve)};\n"
        "const eq = (a, b, what) => { const x = JSON.stringify(a), y = JSON.stringify(b);"
        " if (x !== y) { console.error(what + ': ' + x + ' != ' + y); process.exit(1); } };\n"
        "const near = (a, b, tol, what) => { if (!(Math.abs(a - b) <= tol)) { console.error(what + ': ' + a + ' vs ' + b); process.exit(1); } };\n"
        "const sameBands = (a, b, what) => { eq(a.length, b.length, what + ' count');"
        " a.forEach((x, i) => { eq([x.type, x.on], [b[i].type, b[i].on], `${what} ${i} type`);"
        " near(x.f, b[i].f, 0.01, `${what} ${i} f`); near(x.gain, b[i].gain, 0.01, `${what} ${i} gain`);"
        " near(x.q, b[i].q, 0.001, `${what} ${i} q`); }); };\n"
    )
    result = subprocess.run([node, "--input-type=module", "-e", head + script], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


AUTOEQ = """Preamp: -6.4 dB
Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.71
Filter 2: ON PK Fc 47 Hz Gain -0.7 dB Q 0.39
Filter 3: ON PK Fc 186 Hz Gain -3.1 dB Q 0.63
Filter 4: ON PK Fc 1112 Hz Gain -1.4 dB Q 1.88
Filter 5: ON PK Fc 2519 Hz Gain 3.9 dB Q 2.45
Filter 6: ON PK Fc 4184 Hz Gain -2.2 dB Q 3.41
Filter 7: ON PK Fc 5930 Hz Gain 3.1 dB Q 4.4
Filter 8: ON PK Fc 7591 Hz Gain -4.1 dB Q 3.89
Filter 9: ON PK Fc 9958 Hz Gain 1.9 dB Q 2.09
Filter 10: ON HSC Fc 10000 Hz Gain -2.0 dB Q 0.71
"""

REW = """Filter Settings file

Room EQ V5.31.1
Dated: 14-Oct-2026 21:15:02

Notes:

Equaliser: Generic
Oct 14 21:15:02 2026
Filter  1: ON  PK       Fc   63.0 Hz  Gain  -5.0 dB  Q  2.000
Filter  2: ON  PK       Fc   125 Hz   Gain  -3.5 dB  Q  4.000
Filter  3: OFF None
Filter  4: ON  None
Filter  5: ON  LS       Fc   80.0 Hz  Gain   4.0 dB
"""


def test_import_then_export_gives_the_same_bands():
    run(f"""
const text = {json.dumps(AUTOEQ)};
const a = F.parseFilters(text);
eq(a.bands.length, 10, 'AutoEQ: ten filters');
eq([a.preamp, a.off, a.unsupported, a.unreadable, a.extra, a.clamped], [-6.4, 0, 0, 0, 0, 0], 'nothing left out');
eq(a.bands[0], {{ type: 'lowshelf', f: 105, gain: 5.5, q: 0.71, on: true }}, 'the low shelf');
eq(a.bands[9].type, 'highshelf', 'the high shelf');
const out = F.formatFilters(a.bands, 'P4 Player - Власний');
const b = F.parseFilters(out);
eq(b.bands, a.bands, 'export, then import: the same bands');
eq(F.formatFilters(b.bands, 'P4 Player - Власний'), out, 'and the same text');
eq(out.split('\\n')[0], '# P4 Player - Власний', 'the title is a comment APO and REW skip');
eq(out.split('\\n')[1], 'Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.71', 'APO 1.2.1 reads a shelf with Q');

// what the device keeps: floats a step of the panel leaves (1059.46 Hz), a pass, a notch, an off band
const set = [
  {{ type: 'peak', f: 1059.4631, gain: -2.35, q: 1.4142, on: true }},
  {{ type: 'highpass', f: 31.5, gain: 0, q: 0.707, on: true }},
  {{ type: 'notch', f: 4321.987, gain: 0, q: 6.5, on: true }},
  {{ type: 'lowpass', f: 18000, gain: 0, q: 0.5, on: false }},
];
const back = F.parseFilters(F.formatFilters(set)).bands;
sameBands(back, set.filter((x) => x.on), 'the device\\'s bands, out and back within eq_same_section');
eq(F.formatFilters(set).split('\\n').slice(0, 3), [
  'Filter 1: ON PK Fc 1059.46 Hz Gain -2.35 dB Q 1.414',
  'Filter 2: ON HPQ Fc 31.5 Hz Q 0.707',
  'Filter 3: ON NO Fc 4321.99 Hz Q 6.5',
], 'a pass and a notch have no gain; an off band is not written');
""")


def test_rew_and_apo_dialects():
    run(f"""
const r = F.parseFilters({json.dumps(REW)});
eq(r.bands.map((b) => [b.type, b.f, b.gain, b.q]), [['peak', 63, -5, 2], ['peak', 125, -3.5, 4], ['lowshelf', 80, 4, 0.707]],
   'REW Generic: columns of spaces, a shelf with no Q is 12 dB/oct');
eq([r.off, r.unsupported, r.unreadable], [2, 0, 0], 'OFF and None lines are not filters');

const apo = F.parseFilters([
  'Filter: ON PEQ Fc 100 Hz Gain 1.0 dB BW Oct 1',      // no number, a width in octaves
  'Filter 2: ON LSC 12 dB Fc 300 Hz Gain 5.0 dB',      // a slope: 12 dB/oct is Q 0.707
  'Filter 3: ON HPQ Fc 30 Hz Q 0.5',
  'Filter 4: ON LP Fc 1,2 kHz',                         // no Q: Butterworth; a decimal comma, kHz
  'Filter 5: ON NO Fc 50 Hz Q 4',
  'Filter 6: ON LS 6dB Fc 50.0 Hz Gain 7.2 dB',        // a shelf by its corner
  'Filter 7: ON BP Fc 1000 Hz Q 2',
  'Filter 8: ON IIR Order 1 Coefficients 1 0 1 0',
  'Filter 9: ON PK Fc 1000 Hz Gain 3 dB',               // a peak with no width
  'Filter 10: ON PK Gain 3 dB Q 1',                      // nor a frequency
  '# a note',
].join('\\n'));
eq(apo.bands.map((b) => b.type), ['peak', 'lowshelf', 'highpass', 'lowpass', 'notch'], 'what this panel has');
near(apo.bands[0].q, F.bwToQ(1), 1e-12, 'one octave');
near(apo.bands[0].q, 1.4142, 0.0001, 'one octave is Q 1.414');
near(apo.bands[1].q, 0.7071, 0.0001, '12 dB/oct');
eq(apo.bands[3].f, 1200, 'kHz');
eq(apo.bands[3].q, 0.707, 'a pass without Q');
eq([apo.unsupported, apo.unreadable], [3, 2], 'counted, not read');
""")


def test_the_limits_and_ten_bands():
    run("""
const lines = ['Filter 1: ON PK Fc 5 Hz Gain 15 dB Q 20', 'Filter 2: ON HSC Fc 30000 Hz Gain -14 dB Q 2',
               'Filter 3: ON LSC 24 dB Fc 100 Hz Gain 12 dB'];
for (let i = 0; i < 9; i++) lines.push(`Filter ${4 + i}: ON PK Fc ${100 * (i + 1)} Hz Gain 1 dB Q 1`);
const p = F.parseFilters(lines.join('\\n'));
eq(p.bands.length, 10, 'ten, the most a set holds');
eq(p.extra, 2, 'the rest counted');
eq(p.bands[0], { type: 'peak', f: 20, gain: 12, q: 8, on: true }, 'held to 20 Hz, +12 dB, Q 8');
eq(p.bands[1], { type: 'highshelf', f: 20000, gain: -12, q: 1, on: true }, 'a shelf to Q 1');
eq(p.bands[2].q, 1, 'a slope steeper than a shelf here goes');
eq(p.clamped, 3, 'three filters had a number held');
""")


def test_the_page_adds_to_a_short_set_and_resets():
    run("""
const flat = Array.from({ length: 10 }, (_, i) => ({ type: 'peak', f: 100 * (i + 1), gain: 0, q: 1, on: false }));
eq(B.shownBands(flat), [], '«Рівно»: none on');
eq(B.addBand(flat), null, 'ten bands: none to add — a band is switched on in its sheet');
let r = B.addBand([]);
eq([r.i, r.bands], [0, [{ type: 'peak', f: 1000, gain: 0, q: 1, on: true }]], 'an empty set: a 0 dB peak at 1 kHz');
r = B.addBand(r.bands);
eq([r.i, r.bands[1].f], [1, 140], 'the next goes in the widest free stretch: between 20 Hz and 1 kHz');
const three = [{ type: 'lowshelf', f: 80, gain: 3, q: 0.71, on: true }, { type: 'peak', f: 3000, gain: -2, q: 1.4, on: true },
               { type: 'highshelf', f: 10000, gain: 1.5, q: 0.71, on: true }];
r = B.addBand(three);
eq([r.i, r.bands.length, r.bands[3].f, r.bands[3].on], [3, 4, 490, true], 'a short set grows; 490 Hz is between 80 Hz and 3 kHz');
eq(B.shownBands(three.map((b, i) => (i === 1 ? { ...b, on: false } : b))).map((x) => x.i), [0, 2], 'on, by their place');
eq(B.resetBands(three).map((b) => b.on), [false, false, false], 'reset: all off, the layout kept');
const full = Array.from({ length: 10 }, (_, i) => ({ type: 'peak', f: 100 * (i + 1), gain: 1, q: 1, on: true }));
eq(B.addBand(full), null, 'ten: full');
""")


def test_the_whole_system_is_the_cores_sections():
    run("""
const at = (stages, f, bands = [], eqOn = true) => Y.systemCurveDb(bands, stages, 48000, [f], eqOn)[0];
eq(Y.systemBands({}), [], 'nothing on: nothing drawn');
// the subsonic: a 4th-order Butterworth, -3.01 dB at its corner
let st = Y.systemBands({ hpHz: 40 });
eq(st.map((b) => [b.type, b.f]), [['highpass', 40], ['highpass', 40]], 'two sections');
near(at(st, 40), -3.01, 0.01, 'the corner');
eq(Y.systemBands({ hpHz: 90 }), [], 'over 80 Hz the core has none');
// a port: the subsonic under it at 0.8 of it, and its lift
st = Y.systemBands({ hpHz: 40, portHz: 62, liftDb: 4 });
eq(st.map((b) => [b.type, b.f, b.gain]), [['highpass', 49.6, 0], ['highpass', 49.6, 0], ['peak', 62, 4]], 'the port rules the subsonic');
near(at(st, 62), 4 + 10 * Math.log10(Math.pow(1.25, 8) / (1 + Math.pow(1.25, 8))), 0.05, '+4 dB at the port, less the subsonic');
// Бас and Високі: shelves at 100 Hz and 10 kHz, half their gain at the corner
st = Y.systemBands({ bass: 6, treble: -3 });
near(at(st, 100), 3, 0.05, 'Бас +6: +3 dB at 100 Hz');
near(at(st, 20), 6, 0.3, 'and near +6 under it');
near(at(st, 10000), -1.5, 0.05, 'Високі -3: -1.5 dB at 10 kHz');
eq(Y.systemBands({ bass: 9 })[0].gain, 6, 'held to ±6, as the core holds them');
// the loudness: the high shelf is K_HI / K_LO of the low one
st = Y.systemBands({ loudDb: 5.605 });
eq(st.map((b) => [b.f, Math.round(b.gain * 1000) / 1000, !!b.bilinear]), [[148.6, 5.605, true], [10000, 2.253, true]], 'two bilinear shelves');
// the equalizer under them, or not
const peak = [{ type: 'peak', f: 1000, gain: 6, q: 1, on: true }];
near(at([], 1000, peak), 6, 0.01, 'the EQ alone');
near(at([], 1000, peak, false), 0, 1e-9, 'the EQ off');
""")
