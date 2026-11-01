import { writable, get } from 'svelte/store';
import { state } from '../stores/state.js';
import { writeNow } from './settings.js';
import { CHAIN, TONE_DEFAULTS } from './calib.js';

/*
 * The test tone, as calibration drives it (docs/AUDIO.md, "Test tone", modules/audio).
 *
 * audio.source = 1 plays it. audio.tone_mode 0 sends it through the DSP and the volume; 1 and 2
 * go past both at an exact DAC level (steady, or 250/750 ms bursts), and the device turns those
 * off by itself after 60 s of playing: source off, mode back to 0 (audio_module.cpp,
 * TONE_RAW_MAX_MS). Mode 0 goes off by itself too, after 2 min with nothing about it changed
 * (TONE_CHAIN_IDLE_MS). None of the tone keys persists; until one is written the device reads the
 * defaults in calib.js. Nor do audio.cal_port_off and audio.cal_vol_cap, which a step may send
 * with its tone and the device drops when the tone stops (S0b).
 *
 * A step that plays a tone sends every key it needs in one body, the source last, so the device
 * never starts the tone with the previous step's level (/api/settings sets keys in order, and the
 * audio module may look between two of them). Stopping sends the source first for the same reason.
 * Both go through writeNow (lib/settings.js): shown at once, taken back if the device refuses.
 */

export { CHAIN, RAW, BURSTS, LEFT, RIGHT, BOTH, CAL_MAX_S } from './calib.js';

export const SRC_TONE = 1;

const PREV = 'p4-tone-prev-source';

/** The tone as the device has it now. */
export function toneOf(s) {
  return {
    on: s['audio.source'] === SRC_TONE,
    mode: s['audio.tone_mode'] ?? TONE_DEFAULTS.mode,
    hz: s['audio.tone_hz'] ?? TONE_DEFAULTS.hz,
    db10: s['audio.tone_db10'] ?? TONE_DEFAULTS.db10,
    ch: s['audio.tone_ch'] ?? TONE_DEFAULTS.ch,
  };
}

/**
 * Since when a calibration tone (mode 1 or 2) has been playing, as the device counts it: from
 * when it started, kept across changes of level, frequency or channel, reset once it stops.
 * `exact` is false when it was already playing when this module first looked.
 */
export const calClock = writable({ since: null, exact: false });
let looked = false;
state.subscribe((s) => {
  const t = toneOf(s);
  const cal = t.on && t.mode !== CHAIN;
  const c = get(calClock);
  if (cal && c.since === null) calClock.set({ since: Date.now(), exact: looked });
  else if (!cal && c.since !== null) calClock.set({ since: null, exact: false });
  looked = true;
});

/** The source to come back to: kept across steps, and across the 60 s turning the tone off. */
function remember(s) {
  const src = s['audio.source'] ?? 0;
  if (src === SRC_TONE || (!src && previous() !== null)) return;
  try { sessionStorage.setItem(PREV, String(src)); } catch (e) { /* storage blocked */ }
}
function previous() {
  try {
    const v = sessionStorage.getItem(PREV);
    return v === null ? null : Number(v);
  } catch (e) {
    return null;
  }
}

let stopTimer = null;

/**
 * Play the tone a step asks for. `extra` keys go in the same body before the source (the manual
 * port search sets the port and the volume with it). With `forMs` it stops by itself.
 */
export async function playTone({ mode, hz, db10, ch }, { extra = {}, forMs = 0 } = {}) {
  clearTimeout(stopTimer);
  const s = get(state);
  remember(s);
  const body = { ...extra, 'audio.tone_mode': mode, 'audio.tone_hz': hz };
  if (db10 !== undefined) body['audio.tone_db10'] = db10;
  if (ch !== undefined) body['audio.tone_ch'] = ch;
  body['audio.source'] = SRC_TONE;
  await writeNow(body);
  if (forMs) stopTimer = setTimeout(() => stopTone().catch(() => {}), forMs);
}

/**
 * Stop the tone and put the mode back on "through the DSP". `restore` brings back the source that
 * played before the first step (the last step of the procedure: "джерело — як було"); otherwise
 * the source goes off. `extra` keys ride in the same body (the result being written).
 */
export async function stopTone({ restore = false, extra = {} } = {}) {
  clearTimeout(stopTimer);
  const s = get(state);
  const playing = s['audio.source'] === SRC_TONE;
  const prev = previous();
  const body = {};
  // a source someone chose since the tone stopped is theirs: it is left alone
  if (restore && prev !== null && prev !== SRC_TONE && (playing || !s['audio.source'])) body['audio.source'] = prev;
  else if (playing) body['audio.source'] = 0;
  body['audio.tone_mode'] = CHAIN;
  Object.assign(body, extra);
  await writeNow(body);
  if (restore) {
    try { sessionStorage.removeItem(PREV); } catch (e) { /* storage blocked */ }
  }
}
