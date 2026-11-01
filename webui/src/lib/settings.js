import { get, writable } from 'svelte/store';
import { apiPost } from './api.js';
import { state, setStateKey } from '../stores/state.js';
import { toastError } from '../stores/toast.js';

const NONE = Symbol('none');

/**
 * A setting, shown at once and sent to the device.
 *
 * - `live` (a slider): the first value of a run goes at once; after it, at most one every
 *   `debounceMs`, the newest — so a dragged volume is heard while it moves, not only when the
 *   finger rests (it used to restart a timer on every move and send nothing until a pause).
 *   Otherwise (typed text, a number field) the newest goes once `debounceMs` passed without one.
 * - Never two requests at a time: a value that comes while one is on its way goes after it.
 * - A value the device refuses is taken back from the page: it shows the device's value from
 *   before the run again (unless something newer came in meanwhile), as writeNow() does.
 * - `cleanup()`, as a page goes, sends what is still held — the value the page showed is the one
 *   the device gets.
 *
 * @param {string} key - State key (e.g. 'audio.volume')
 * @param {object} opts - { debounceMs, endpoint }
 * @returns {{ send, pending, flashOk, cleanup }}
 */
export function createSettingSender(key, { debounceMs = 500, endpoint = '/api/settings', live = false } = {}) {
  let timer = null;          // the next slot of the run
  let flashTimer = null;
  let held = NONE;           // the newest value, waiting for its slot
  let inflight = false;
  let before = NONE;         // the device's value when the run began
  const pending = writable(false);
  const flashOk = writable(false);

  function idle() {
    if (timer || inflight || held !== NONE) return;
    pending.set(false);
    before = NONE;
  }

  async function post(value) {
    inflight = true;
    try {
      await apiPost(endpoint, { [key]: value });
      before = value;
      flashOk.set(true);
      clearTimeout(flashTimer);
      flashTimer = setTimeout(() => flashOk.set(false), 400);
    } catch (e) {
      toastError(e.message);
      if (held === NONE && before !== NONE && get(state)[key] === value) setStateKey(key, before);
    } finally {
      inflight = false;
      if (held !== NONE && !timer) {               // the newest, now that the line is free
        const v = held;
        held = NONE;
        post(v);
      }
      idle();
    }
  }

  /** A slot: the newest goes now, unless one is in flight — then it goes when that lands. */
  function slot(again) {
    timer = null;
    if (held === NONE || inflight) { idle(); return; }
    const v = held;
    held = NONE;
    post(v);
    if (again) timer = setTimeout(() => slot(true), debounceMs);
  }

  function send(value) {
    if (before === NONE) before = get(state)[key];
    setStateKey(key, value);
    pending.set(true);
    held = value;
    if (debounceMs <= 0) {
      if (!inflight) { held = NONE; post(value); }
      return;
    }
    if (live) {
      if (!timer) slot(true);
      return;
    }
    clearTimeout(timer);
    timer = setTimeout(() => slot(false), debounceMs);
  }

  /** The page goes: what is still held is sent now. */
  function cleanup() {
    clearTimeout(timer);
    timer = null;
    clearTimeout(flashTimer);
    if (held !== NONE && !inflight) {
      const v = held;
      held = NONE;
      post(v);
    }
  }

  return { send, pending, flashOk, cleanup };
}

/**
 * Write `keys` now, with no sender's delay, and show them at once. If the device does not take
 * them, the page shows again what it showed before (where nothing newer came in meanwhile) and
 * the error is thrown: a page that goes on after a failed write must not show what was never
 * written. For a one-off write a tap makes (a source, a queue mode, a tone, a measurement), and
 * for the writes that end something — «Прийняти», «Лишити як було» — after which the page goes and
 * a sender's cleanup() would drop a value it had not sent yet.
 */
export async function writeNow(keys) {
  const was = get(state);
  Object.entries(keys).forEach(([k, v]) => setStateKey(k, v));
  try {
    return await apiPost('/api/settings', keys);
  } catch (e) {
    const now = get(state);
    Object.entries(keys).forEach(([k, v]) => { if (now[k] === v) setStateKey(k, was[k]); });
    throw e;
  }
}
