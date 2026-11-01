import { writable } from 'svelte/store';
import { apiGet, apiPost, apiPostKeepalive } from './api.js';
import { clampBand } from './eqband.js';

/*
 * The EQ as the web edits it (GET / POST /api/eq, modules/audio/src/eq_http.inc).
 *
 * The device keeps one document for both editors — the panel's DSP page and this — with a
 * revision that moves on every change (components/audio_dsp/include/eq_store.h). An edit names
 * the revision it was made from; if the panel moved it meanwhile the device answers 409 and the
 * page reloads rather than write over that change. audio.eq_rev in the state tells the page when
 * the panel edits.
 *
 * Edits are sent one at a time, newest wins: a drag makes dozens a second, and only the band
 * set the finger is at when the previous request comes back is worth sending. The device writes
 * the file a minute after the last edit, or at once when the page is left (keep).
 *
 * A band, its limits and the page's edits on a set are lib/eqband.js's (no imports, so Node
 * tests it); they are passed on from here as the pages have always taken them.
 */

export {
  F_MIN, F_MAX, GAIN_MAX, Q_MIN, Q_MAX, SHELF_Q_MAX, Q_LR12, Q_BW12, PASS_Q, MAX_BANDS,
  clampBand, withType, passQIndex,
} from './eqband.js';

/**
 * One editing session. `doc` is { rev, fs, preset, presets, bands } as the device last said,
 * with this page's own edits on top; `onConflict` runs after a 409 reloaded it, `onError` on
 * anything else.
 */
export function createEq({ onConflict, onError } = {}) {
  const doc = writable(null);
  let current = null;        // what doc holds, for code that is not a subscriber
  let rev = 0;               // the device revision this page's view is from
  let pending = null;        // bands waiting to go
  let busy = 0;              // requests queued or in flight
  let edited = false;        // something was sent since the last keep
  let keepAfter = false;     // a keep asked for while requests were on their way
  let seen = 0;              // the last device revision followed
  let chain = Promise.resolve();

  doc.subscribe((d) => { current = d; });

  function enqueue(fn) {
    busy++;
    chain = chain
      .then(fn)
      .catch(async (e) => {
        pending = null;
        if (/changed/.test(e?.message || '')) {
          await reload().catch(() => {});
          onConflict && onConflict();
        } else {
          onError && onError(e);
        }
      })
      .finally(() => {
        busy--;
        if (busy === 0 && keepAfter) keepNow();
      });
    return chain;
  }

  /** Everything sent has landed: the device writes it at once. The keep names no revision. */
  function keepNow() {
    keepAfter = false;
    if (!edited) return;
    edited = false;
    apiPostKeepalive('/api/eq', { keep: true });
  }

  async function reload() {
    const d = await apiGet('/api/eq');
    rev = d.rev;
    doc.set(d);
    return d;
  }

  async function sendPending() {
    const bands = pending;
    pending = null;
    if (!bands) return;
    const r = await apiPost('/api/eq', { rev, bands });
    rev = r.rev;
    edited = true;
    doc.update((d) => (d ? { ...d, rev } : d));
  }

  return {
    doc,
    load: () => enqueue(reload),

    /** Section 0's bands, set by hand: the result is «Власний» (eq_presets.h). */
    setBands(bands) {
      const clean = bands.map(clampBand);
      doc.update((d) => (d ? { ...d, bands: clean, preset: 'custom' } : d));
      const queued = pending !== null;
      pending = clean;
      if (!queued) enqueue(sendPending);
    },

    /** A preset by id; "custom" brings the user's own set back. */
    choose(id) {
      if (!current || current.preset === id) return chain;
      doc.update((d) => ({ ...d, preset: id }));
      return enqueue(async () => {
        const r = await apiPost('/api/eq', { rev, preset: id });
        rev = r.rev;
        edited = true;
        await reload();     // the device built the bands
      });
    },

    /**
     * The panel moved the document (audio.eq_rev): take its version. Not while an edit of this
     * page is on its way — the next tick looks again — and once per revision, since a tick can
     * still carry the one before this page's own last edit.
     */
    follow(deviceRev) {
      if (!deviceRev || deviceRev === rev || deviceRev === seen) return;
      if (busy !== 0 || pending !== null) return;
      seen = deviceRev;
      enqueue(reload);
    },

    /**
     * The page is left or hidden: the device writes what it has at once. Not before the requests
     * on their way have landed: bands still waiting would go with the revision from before the
     * one in flight, and the device refuses that (409) — the last edit lost and the keep with it,
     * or a conflict that is the page's own. They go in turn, from the revision it brings back,
     * and the keep after them.
     */
    keep() {
      if (busy !== 0) keepAfter = true;
      else keepNow();
    },
  };
}
