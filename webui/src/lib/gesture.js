/*
 * A finger that scrolls the page does not set anything: otherwise scrolling a page moves the
 * equalizer, the tone, another setting.
 *
 * A slider: the press changes nothing. The gesture becomes the slider's once the finger has gone
 * SLOP px along it, and more along than across; it is the page's if it went up or down first —
 * the slider's box has touch-action: pan-y, so the browser scrolls. A tap changes nothing. A
 * mouse does not scroll, so it takes the slider at once, as a native one does. The native
 * <input type="range"> stays under the track for the keyboard and screen readers, with
 * pointer-events: none.
 *
 * A point on the equalizer's graph: only the point itself takes a finger (the rest of the graph
 * scrolls), and it moves once it has been held HOLD_MS or the finger went SLOP px away.
 *
 * A button with a second meaning held, as a preset has (a tap tunes it, a hold stores what plays
 * in it): a press held still PRESS_HOLD_MS is the hold, one that went SLOP px is the page's.
 *
 * The decisions are pure — tools/tests/test_webui_gesture.py runs them in Node — and `slide` and
 * `press` wire them to pointer events. No imports, for the same reason.
 */

/** px a finger goes before a press is a drag or a scroll */
export const SLOP = 8;
/** ms a still finger holds an EQ point before it is picked up */
export const HOLD_MS = 300;
/** ms a still finger holds a preset before it stores (the glass's long press) */
export const PRESS_HOLD_MS = 500;

/** What a press has become after moving (dx, dy): 'press' still, 'drag' along x, 'scroll' across. */
export function intent(dx, dy, slop = SLOP) {
  const ax = Math.abs(dx), ay = Math.abs(dy);
  if (ax >= slop && ax > ay) return 'drag';
  if (ay >= slop) return 'scroll';
  return 'press';
}

/**
 * One press on a slider, from down to end. `move` answers what it has become and keeps the first
 * answer that is not 'press'; `end` says 'tap' for a press that never went anywhere.
 */
export function slideGesture(pointerType, slop = SLOP) {
  const mouse = pointerType === 'mouse';
  let x0 = 0, y0 = 0, state = 'idle';
  return {
    get state() { return state; },
    down(x, y) {
      x0 = x;
      y0 = y;
      state = mouse ? 'drag' : 'press';
      return state;
    },
    move(x, y) {
      if (state === 'press') state = intent(x - x0, y - y0, slop);
      return state;
    },
    end() {
      const s = state;
      state = 'idle';
      return s === 'press' ? 'tap' : s;
    },
  };
}

/**
 * One press on an EQ point: 'press' while short and still, 'lift' once held `hold` ms (the point
 * is picked up; any move then drags it), 'drag' once the finger went `slop` px any way. A mouse
 * drags at once. `end` says 'tap' for a press that was never a drag: it picks the band, no more.
 * Times are the caller's (event.timeStamp), so the test needs no clock.
 */
export function pointGesture(pointerType, { slop = SLOP, hold = HOLD_MS } = {}) {
  const mouse = pointerType === 'mouse';
  let x0 = 0, y0 = 0, t0 = 0, state = 'idle';
  const tick = (t) => {
    if (state === 'press' && t - t0 >= hold) state = 'lift';
    return state;
  };
  return {
    get state() { return state; },
    down(x, y, t) {
      x0 = x;
      y0 = y;
      t0 = t;
      state = mouse ? 'drag' : 'press';
      return state;
    },
    tick,
    move(x, y, t) {
      tick(t);
      if (state === 'press' && Math.hypot(x - x0, y - y0) >= slop) state = 'drag';
      else if (state === 'lift' && (x !== x0 || y !== y0)) state = 'drag';
      return state;
    },
    end() {
      const s = state;
      state = 'idle';
      return s === 'drag' ? 'drag' : 'tap';
    },
  };
}

/**
 * One press on a button that a hold means something else on: 'press' while still, 'scroll' once
 * the finger went `slop` px (the page's: neither meaning), 'hold' once the caller says the press
 * has lasted long enough (`held`, from its timer). `end` says 'tap' for a press that was neither.
 */
export function holdGesture(slop = SLOP) {
  let x0 = 0, y0 = 0, state = 'idle';
  return {
    get state() { return state; },
    down(x, y) {
      x0 = x;
      y0 = y;
      state = 'press';
      return state;
    },
    move(x, y) {
      if (state === 'press' && Math.hypot(x - x0, y - y0) >= slop) state = 'scroll';
      return state;
    },
    held() {
      if (state === 'press') state = 'hold';
      return state;
    },
    end() {
      const s = state;
      state = 'idle';
      return s === 'press' ? 'tap' : s;
    },
  };
}

/** The place of x across a box, 0…1, with `inset` px at each end where the value is already
 *  at its limit (the M3 slider's handle stops 8 px in). */
export function fraction(x, left, width, inset = 0) {
  const w = width - 2 * inset;
  if (!(w > 0)) return 0;
  return Math.min(1, Math.max(0, (x - left - inset) / w));
}

/** The value a fraction of a slider stands for, on its steps. */
export function valueAt(f, min, max, step) {
  if (!(max > min)) return min;
  const n = step > 0 ? Math.round((f * (max - min)) / step) : f * (max - min);
  const v = min + (step > 0 ? n * step : n);
  const dec = step > 0 ? (String(step).split('.')[1] || '').length : 6;
  return Math.min(max, Math.max(min, Number(v.toFixed(dec))));
}

/**
 * use:slide={{ at, end, disabled, inset }} — a horizontal slider's pointer input, as above.
 * `at(f)` gets the pointer's place across the node, 0…1, each time it moves once the gesture is
 * the slider's (a mouse: from the press); `end()` once it lets go of a drag. A tap, a scroll and
 * a cancelled press call neither.
 */
export function slide(node, opts) {
  let o = opts || {};
  let g = null;
  let id = null;

  const at = (e) => {
    const r = node.getBoundingClientRect();
    o.at?.(fraction(e.clientX, r.left, r.width, o.inset || 0));
  };
  function take(e) {
    try { node.setPointerCapture(e.pointerId); } catch (err) { /* the pointer is gone */ }
    at(e);
  }
  function down(e) {
    // a press starts afresh — also when the end of the one before never came
    if (o.disabled || (e.pointerType === 'mouse' && e.button !== 0)) return;
    g = slideGesture(e.pointerType);
    id = e.pointerId;
    if (g.down(e.clientX, e.clientY) === 'drag') {
      e.preventDefault();          // a mouse: no text selection while it drags
      take(e);
    }
  }
  function move(e) {
    if (!g || e.pointerId !== id) return;
    const was = g.state;
    const s = g.move(e.clientX, e.clientY);
    if (s !== 'drag') return;
    if (was === 'drag') at(e);
    else take(e);
  }
  function up(e) {
    if (!g || e.pointerId !== id) return;
    const s = g.end();
    g = null;
    id = null;
    try { if (node.hasPointerCapture(e.pointerId)) node.releasePointerCapture(e.pointerId); } catch (err) { /* gone */ }
    if (s === 'drag') o.end?.();
  }

  node.addEventListener('pointerdown', down);
  node.addEventListener('pointermove', move);
  node.addEventListener('pointerup', up);
  node.addEventListener('pointercancel', up);
  return {
    update(n) { o = n || {}; },
    destroy() {
      node.removeEventListener('pointerdown', down);
      node.removeEventListener('pointermove', move);
      node.removeEventListener('pointerup', up);
      node.removeEventListener('pointercancel', up);
    },
  };
}

/**
 * use:press={{ tap, hold }} — a button with two meanings, as above. A still press calls `hold`
 * once it has lasted PRESS_HOLD_MS, while the finger is still down, and its release is then no
 * tap; a press that ends sooner calls `tap`; one the page took for a scroll calls neither. The
 * context menu is the hold too — a mouse's right button, the keyboard's menu key or Shift+F10 —
 * and a long touch's own menu (Android) is not a second one. Enter and Space click the button,
 * which is a tap; a pointer's click is left alone, its tap having come with the release.
 */
export function press(node, opts) {
  let o = opts || {};
  let g = null;
  let id = null;
  let timer = null;

  const stop = () => { clearTimeout(timer); timer = null; };
  function hold() {
    stop();
    g.held();
    o.hold?.();
  }
  function down(e) {
    stop();
    g = null;
    if (e.pointerType === 'mouse' && e.button !== 0) return;   // the right button is the menu's
    g = holdGesture();
    id = e.pointerId;
    g.down(e.clientX, e.clientY);
    // a mouse let go off the button still ends this press
    try { node.setPointerCapture(e.pointerId); } catch (err) { /* the pointer is gone */ }
    timer = setTimeout(() => { if (g && g.state === 'press') hold(); }, PRESS_HOLD_MS);
  }
  function move(e) {
    if (g && e.pointerId === id && g.move(e.clientX, e.clientY) === 'scroll') stop();
  }
  function up(e) {
    if (!g || e.pointerId !== id) return;
    const s = e.type === 'pointerup' ? g.end() : 'scroll';
    g = null;
    stop();
    if (s === 'tap') o.tap?.();
  }
  function click(e) {
    if (e.detail === 0) o.tap?.();
  }
  function menu(e) {
    e.preventDefault();
    if (!g) o.hold?.();
    else if (g.state === 'press') hold();
  }

  node.addEventListener('pointerdown', down);
  node.addEventListener('pointermove', move);
  node.addEventListener('pointerup', up);
  node.addEventListener('pointercancel', up);
  node.addEventListener('click', click);
  node.addEventListener('contextmenu', menu);
  return {
    update(n) { o = n || {}; },
    destroy() {
      stop();
      node.removeEventListener('pointerdown', down);
      node.removeEventListener('pointermove', move);
      node.removeEventListener('pointerup', up);
      node.removeEventListener('pointercancel', up);
      node.removeEventListener('click', click);
      node.removeEventListener('contextmenu', menu);
    },
  };
}
