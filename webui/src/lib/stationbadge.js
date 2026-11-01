/*
 * A station's stand-in for a logo: a disc in a colour of its own, with a short label — the glass's
 * rule (modules/ui_lvgl/include/station_badge.h), copied so the web draws the same disc in the
 * same colour for the same name. tools/tests/test_webui_station_badge.py holds this copy to the
 * cases the header's own test pins (tests/host/test_station_badge.cpp).
 *
 * The colour: FNV-1a over the name's UTF-8 bytes, over eight muted hues white reads on. The
 * label: the first word that says something — leading numbers and frequencies and the word
 * "radio" skipped — when it has at most five characters, otherwise the initials of the name's
 * words, at most three; capitals, by the header's own table rather than toUpperCase(), which
 * knows more scripts than the glass draws.
 *
 * No imports: the test loads this file in Node.
 */

const LABEL_MAX = 5;
const INITIALS = 3;
const WORDS = 32;   // the header's array: a 63-byte name never has more

/** The discs, 0xRRGGBB: station_badge::PALETTE. */
export const PALETTE = [0x9D5252, 0x8F5C38, 0x74682E, 0x467342, 0x18746B, 0x006F9E, 0x63629B, 0x92537C];

const utf8 = new TextEncoder();

function fnv1a(text) {
  let h = 2166136261;
  for (const b of utf8.encode(text)) h = Math.imul(h ^ b, 16777619) >>> 0;
  return h;
}

const isDigit = (cp) => cp >= 0x30 && cp <= 0x39;

/** A letter of a name: Latin, accented Latin, Cyrillic and the other alphabets (is_letter). */
function isLetter(cp) {
  if (cp < 0x80) return (cp | 0x20) >= 0x61 && (cp | 0x20) <= 0x7A;
  if (cp < 0xC0 || cp === 0xD7 || cp === 0xF7) return false;
  if (cp >= 0x2000 && cp <= 0x2BFF) return false;
  if (cp >= 0x3000 && cp <= 0x303F) return false;
  if (cp >= 0xFE00 && cp <= 0xFE0F) return false;
  return cp !== 0xFFFD && cp < 0x1F000;
}

/** The capital of `cp` in the scripts the glass draws; anything else as it is (upper). */
export function upper(cp) {
  if (cp >= 0x61 && cp <= 0x7A) return cp - 0x20;
  if (cp >= 0xE0 && cp <= 0xFE && cp !== 0xF7) return cp - 0x20;
  if (cp === 0xFF) return 0x178;
  if (cp >= 0x100 && cp <= 0x17F) {
    if (cp === 0x131) return 0x49;
    if (cp === 0x17F) return 0x53;
    if (cp === 0x138 || cp === 0x149) return cp;
    const capitalOdd = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
    return cp % 2 === (capitalOdd ? 0 : 1) ? cp - 1 : cp;
  }
  if (cp === 0x219 || cp === 0x21B) return cp - 1;
  if (cp >= 0x430 && cp <= 0x44F) return cp - 0x20;
  if (cp >= 0x450 && cp <= 0x45F) return cp - 0x50;
  if (cp === 0x491) return 0x490;
  return cp;
}

// "radio" in capitals: Latin, with the Portuguese and Czech Á, and Cyrillic with І and И
const RADIO = [[0x52, 0x41, 0x44, 0x49, 0x4F], [0x52, 0xC1, 0x44, 0x49, 0x4F],
               [0x420, 0x410, 0x414, 0x406, 0x41E], [0x420, 0x410, 0x414, 0x418, 0x41E]];
const isRadio = (w) => w.cps.length === 5 && RADIO.some((r) => r.every((c, i) => upper(w.cps[i]) === c));

const caps = (cps) => String.fromCodePoint(...cps.map(upper));

/**
 * The disc for station `name`: { label, first, color } — the tile's label (up to five
 * characters), the presets' first character of it, and the disc's colour as CSS. A name without a
 * letter or a digit gets an empty label.
 */
export function stationBadge(name) {
  const text = name || '';
  const color = '#' + PALETTE[fnv1a(text) % PALETTE.length].toString(16).padStart(6, '0');
  // the words: runs of letters and digits
  const words = [];
  let word = null;
  for (const ch of text) {
    const cp = ch.codePointAt(0);
    if (!isLetter(cp) && !isDigit(cp)) { word = null; continue; }
    if (!word) {
      if (words.length >= WORDS) break;
      word = { cps: [], letter: false };
      words.push(word);
    }
    word.cps.push(cp);
    word.letter ||= isLetter(cp);
  }
  // the word that names the station: the first with a letter that is not "radio"; else the first
  // with a letter ("Radio 1"); else the first ("538")
  let key = words.findIndex((w) => w.letter && !isRadio(w));
  if (key < 0) key = words.findIndex((w) => w.letter);
  if (key < 0 && words.length) key = 0;
  let label = '';
  if (key >= 0 && words[key].cps.length <= LABEL_MAX) label = caps(words[key].cps);
  else if (key >= 0) label = caps(words.filter((w) => w.letter).slice(0, INITIALS).map((w) => w.cps[0]));
  const first = label ? String.fromCodePoint(label.codePointAt(0)) : '';
  return { label, first, color };
}
