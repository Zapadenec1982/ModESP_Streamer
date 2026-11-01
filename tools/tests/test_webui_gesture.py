"""A finger that scrolls does not set anything: webui/src/lib/gesture.js and lib/undo.js in Node.

Without them scrolling a page of the web interface moves the equalizer, the tone or another
setting. The rules: a press changes nothing; a slider is the finger's once it went 8 px
along, more along than across, and the page's if it went up or down first; a tap changes
nothing; a mouse takes a slider at once. An EQ point moves once held 300 ms or once the finger
went 8 px from it. After a slider or a drag moved a sound setting, a snackbar offers to put it
back — one for the setting moved last, from where it started.
"""

import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(script):
    node = shutil.which("node")
    assert node, "Node.js is required for the WebUI gesture regression"
    gesture = (ROOT / "webui/src/lib/gesture.js").as_uri()
    undo = (ROOT / "webui/src/lib/undo.js").as_uri()
    head = (
        f"import * as g from {json.dumps(gesture)};\n"
        f"import * as u from {json.dumps(undo)};\n"
        "const eq = (a, b, what) => { const x = JSON.stringify(a), y = JSON.stringify(b);"
        " if (x !== y) { console.error(what + ': ' + x + ' != ' + y); process.exit(1); } };\n"
    )
    result = subprocess.run([node, "--input-type=module", "-e", head + script], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


def test_the_threshold_and_the_direction():
    run("""
eq(g.SLOP, 8, 'the threshold');
eq(g.intent(7, 0), 'press', 'under the threshold');
eq(g.intent(-7, 7), 'press', 'under it both ways');
eq(g.intent(8, 0), 'drag', 'along, at the threshold');
eq(g.intent(-9, 8), 'drag', 'more along than across');
eq(g.intent(8, 8), 'scroll', 'as much across as along is the page');
eq(g.intent(3, -9), 'scroll', 'across');
eq(g.intent(0, 8), 'scroll', 'straight down');
""")


def test_a_press_a_tap_and_a_vertical_swipe_change_nothing():
    run("""
// a slider 0…100 across 200 px, at 40: what a finger does to it
function finger(type, path) {
  let value = 40;
  const s = g.slideGesture(type);
  const set = (x) => { value = g.valueAt(g.fraction(x, 0, 200), 0, 100, 1); };
  if (s.down(path[0][0], path[0][1]) === 'drag') set(path[0][0]);
  for (const [x, y] of path.slice(1)) if (s.move(x, y) === 'drag') set(x);
  return [s.end(), value];
}
eq(finger('touch', [[150, 100]]), ['tap', 40], 'a tap: nothing, wherever it lands');
eq(finger('touch', [[150, 100], [154, 103], [147, 98]]), ['tap', 40], 'a finger that shook');
eq(finger('touch', [[150, 100], [151, 110], [152, 160], [190, 170]]), ['scroll', 40],
   'down first: the page, and along afterwards does not take it back');
eq(finger('pen', [[150, 100], [149, 92]]), ['scroll', 40], 'a pen up the page');
eq(finger('touch', [[150, 100], [160, 102]]), ['drag', 80], 'along: the slider, where the finger is');
eq(finger('touch', [[150, 100], [140, 101], [100, 140], [60, 300]]), ['drag', 30],
   'once it is the slider\\'s, across does not give it to the page');
eq(finger('mouse', [[20, 100]]), ['drag', 10], 'a mouse takes it at once');
""")


def test_where_a_finger_is_on_a_slider():
    run("""
eq(g.fraction(8, 0, 216, 8), 0, 'the M3 handle stops 8 px in');
eq(g.fraction(108, 0, 216, 8), 0.5, 'the middle');
eq(g.fraction(300, 0, 216, 8), 1, 'past the end');
eq(g.fraction(5, 10, 0), 0, 'no width');
eq(g.valueAt(0.5, -6, 6, 0.5), 0, 'the balance, centre');
eq(g.valueAt(0.54, -6, 6, 0.5), 0.5, 'its half steps');
eq(g.valueAt(0.3, 0, 1, 0.1), 0.3, 'a tenth is a tenth, not 0.30000000000000004');
eq(g.valueAt(0.99, 0, 100, 5), 100, 'onto the steps');
eq(g.valueAt(1.5, 0, 100, 5), 100, 'held to the top');
""")


def test_an_eq_point_moves_after_a_hold_or_a_clear_move():
    run("""
eq(g.HOLD_MS, 300, 'the hold');
let p = g.pointGesture('touch');
p.down(100, 100, 0);
eq(p.tick(299), 'press', 'not yet');
eq(p.end(), 'tap', 'a tap picks the band and moves nothing');

p = g.pointGesture('touch');
p.down(100, 100, 0);
eq(p.move(103, 104, 120), 'press', '5 px: still a press');
eq(p.tick(300), 'lift', 'held: picked up');
eq(p.move(104, 104, 400), 'drag', 'picked up, any move drags it');
eq(p.end(), 'drag', 'it moved');

p = g.pointGesture('touch');
p.down(100, 100, 0);
eq(p.move(100, 108, 50), 'drag', '8 px from the point: moves at once');

p = g.pointGesture('touch');
p.down(100, 100, 0);
eq(p.move(100, 100, 350), 'lift', 'a move event with no movement after the hold');
eq(p.end(), 'tap', 'held and let go where it was: picked, not moved');

p = g.pointGesture('mouse');
eq(p.down(100, 100, 0), 'drag', 'a mouse moves it at once');
""")


def test_the_snackbar_puts_back_where_it_started():
    run("""
let clock = 0, shown = [], hidden = [], reverted = [], next = 0;
const undo = u.createUndo({
  show: (text, action, run) => { shown.push({ text, action, run, id: ++next }); return next; },
  hide: (id) => hidden.push(id),
  now: () => clock,
});
const bass = (from, to) => undo.note({ key: 'bass', from, to, action: 'Скасувати',
  text: (a, b) => `Бас: ${a} → ${b}`, revert: (v) => reverted.push(['bass', v]) });
const width = (from, to) => undo.note({ key: 'width', from, to, action: 'Скасувати',
  text: (a, b) => `Ширина: ${a} → ${b}`, revert: (v) => reverted.push(['width', v]) });

eq(u.UNDO_MS, 5000, 'five seconds');
eq(bass(0, 3), 1, 'a snackbar');
eq([shown[0].text, shown[0].action], ['Бас: 0 → 3', 'Скасувати'], 'what it says');
clock = 2000;
bass(3, 5);
eq(shown[1].text, 'Бас: 0 → 5', 'moved again while it shows: from where it started');
eq(hidden, [1], 'one snackbar, not two');
shown[1].run();
eq(reverted, [['bass', 0]], '«Скасувати» puts the start back');
eq(undo.current, null, 'and it is gone');

clock = 10000;
bass(0, 2);
clock = 11000;
eq(bass(2, 0), null, 'moved back to where it started: nothing to put back');
eq(hidden.slice(-1), [3], 'its snackbar goes');

clock = 20000;
bass(0, 4);
clock = 21000;
width(100, 120);
eq(shown.slice(-1)[0].text, 'Ширина: 100 → 120', 'another setting: its own start');
eq(undo.current, { key: 'width', from: 100 }, 'the last setting moved');
clock = 27000;
width(120, 140);
eq(shown.slice(-1)[0].text, 'Ширина: 120 → 140', 'after five seconds a move starts afresh');

reverted = [];
const band = (from, to) => undo.note({ key: 'eq.band.2', from, to, action: 'x', text: () => '',
  revert: (v) => reverted.push(v) });
clock = 40000;
band({ band: { f: 1000, gain: 2 }, preset: 'rock' }, { band: { f: 1200, gain: 3 }, preset: 'custom' });
clock = 41000;
band({ band: { f: 1200, gain: 3 }, preset: 'custom' }, { band: { f: 1500, gain: 4 }, preset: 'custom' });
shown.slice(-1)[0].run();
eq(reverted, [{ band: { f: 1000, gain: 2 }, preset: 'rock' }], 'a band dragged twice goes back to the genre it was');
""")
