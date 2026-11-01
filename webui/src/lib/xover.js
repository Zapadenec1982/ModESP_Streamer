/*
 * «Кросовер»: the output schemes and which of them play.
 * A scheme is offered as working only once the firmware says it plays it, in audio.out_caps
 * (components/audio_dsp/include/out_caps.h, the same bits); the rest are «ще не підтримується».
 * A firmware that does not say has the one stereo pair this board was built with. Which scheme
 * plays is stereo until the outputs' profile says another (docs/CONCEPT.md, "Outputs").
 *
 * No imports: tools/tests/test_webui_xover.py runs this in Node.
 */

export const OUT_STEREO = 1, OUT_SUB = 2, OUT_BIAMP = 4, OUT_FOUR = 8;

/** In the order the page shows them. */
export const SCHEMES = [
  { id: 'stereo', bit: OUT_STEREO },
  { id: 'sub', bit: OUT_SUB },
  { id: 'biamp', bit: OUT_BIAMP },
  { id: 'four', bit: OUT_FOUR },
];

/** Each scheme, and whether it plays here. */
export function schemesOf(caps) {
  const c = Number.isInteger(caps) && caps > 0 ? caps : OUT_STEREO;
  return SCHEMES.map((s) => ({ ...s, supported: (c & s.bit) !== 0 }));
}

/** The scheme that plays now. */
export const playing = () => 'stereo';
