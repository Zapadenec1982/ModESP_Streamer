/*
 * Which language the web speaks: this browser's own
 * pick if it made one; otherwise the panel's, ui.lang; otherwise, until the panel has said, the
 * browser's. Plain functions, so the tests can load them in Node; stores/i18n.js keeps the state.
 */

/** ui.lang 0..3, the panel's order. */
export const LANGS = ['uk', 'en', 'de', 'pl'];

export const known = (v) => (LANGS.includes(v) ? v : null);

/** ui.lang, a number, as a code; null for anything else. */
export const panelCode = (v) => (Number.isInteger(v) && v >= 0 && v < LANGS.length ? LANGS[v] : null);

/** The browser's own language: what a first visit speaks before the panel has said. */
export function browserLang(nav) {
  const n = (nav || '').toLowerCase();
  if (n.startsWith('uk') || n.startsWith('ru')) return 'uk';
  if (n.startsWith('de')) return 'de';
  if (n.startsWith('pl')) return 'pl';
  return 'en';
}

export function chosen(pick, panel, nav) {
  return known(pick) || known(panel) || browserLang(nav);
}

/**
 * The pick to keep from an older build. The old key was written on every start, picked or not, so
 * its value says nothing — unless it is not the browser's own language, which only a pick could
 * have made it. That one stays this browser's pick; the rest go with the panel.
 */
export function migratedPick(old, pick, nav) {
  if (known(pick)) return pick;
  const o = known(old);
  return o && o !== browserLang(nav) ? o : null;
}
