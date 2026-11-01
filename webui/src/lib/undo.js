/*
 * «Скасувати» after a slider or a drag on the equalizer's graph moved a sound setting (with
 * lib/gesture.js): a snackbar for UNDO_MS, «Бас: 0 → +3».
 *
 * One snackbar, for the setting moved last. The same setting moved again while it shows keeps
 * where it started; moved back there, the snackbar goes. Its action puts the start back. Taps —
 * buttons, chips, switches — have none: a scroll never makes a tap, so what one did was meant.
 *
 * No imports: tools/tests/test_webui_gesture.py runs this in Node. stores/toast.js gives it the
 * snackbar.
 */

export const UNDO_MS = 5000;

const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

/**
 * `show(text, action, run)` puts up a snackbar and returns its id, `hide(id)` takes one down;
 * `now()` is the clock.
 */
export function createUndo({ show, hide, now = () => Date.now(), ms = UNDO_MS }) {
  let cur = null;     // { key, from, at, id } of the snackbar up now
  return {
    /**
     * `key` moved from `from` to `to`. `text(from, to)` words it, `action` is the button's word,
     * `revert(from)` puts it back. Returns the snackbar's id, or null when there is none.
     */
    note({ key, from, to, text, action, revert }) {
      const t = now();
      if (cur && cur.key === key && t - cur.at < ms) from = cur.from;
      if (cur) {
        hide(cur.id);
        cur = null;
      }
      if (same(from, to)) return null;
      const entry = { key, from, at: t, id: null };
      entry.id = show(text(from, to), action, () => {
        if (cur === entry) cur = null;
        revert(from);
      });
      cur = entry;
      return entry.id;
    },
    /** The snackbar up now, for the test: its key and where it started. */
    get current() { return cur && { key: cur.key, from: cur.from }; },
  };
}
