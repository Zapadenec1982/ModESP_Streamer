/*
 * The test tone's keys as the device has them (docs/AUDIO.md, "Test tone"), for the port's search
 * and lib/tone.js. The amplifier ceiling's arithmetic — the multimeter's, the search by ear's, the
 * supply's sag — left with its page. tools/tests/test_webui_service.py holds these to the
 * firmware. No imports: the test loads this file in Node.
 */

/** audio.tone_mode: through the DSP and the volume, or past both at an exact DAC level. */
export const CHAIN = 0;
export const RAW = 1;
export const BURSTS = 2;
/** audio.tone_ch */
export const LEFT = 1;
export const RIGHT = 2;
export const BOTH = 3;
/** What the device reads for a tone key never written: none persists (manifest, audio_module.cpp). */
export const TONE_DEFAULTS = { mode: CHAIN, hz: 1000, db10: -260, ch: BOTH };
/** A calibration tone (mode 1 or 2) turns itself off after this long (TONE_RAW_MAX_MS). */
export const CAL_MAX_S = 60;
