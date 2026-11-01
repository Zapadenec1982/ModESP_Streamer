import { writable, derived, get } from 'svelte/store';
import uk from '../i18n/uk.json';
import { fmt, msg } from '../lib/msg.js';
import { LANGS, known, panelCode, chosen, migratedPick } from '../lib/lang.js';
import { state } from './state.js';
import { setErrorWords } from './toast.js';
import { humanError } from '../lib/errors.js';

/*
 * Interface strings in four languages.
 *
 * The catalogs are webui/src/i18n/<lang>.json, flat keys, the same set in each —
 * tools/tests/test_webui_i18n.py holds them to that. Ukrainian is in the bundle and lies under
 * every other language, so a key a translation lacks reads in Ukrainian, never "undefined".
 * The other three are fetched when chosen: /lang/<lang>.json (scripts/deploy.js puts them
 * there). Markup reads $t[<literal key>]; code that hands a key to a component wraps it in K()
 * so the test can see it is used.
 *
 * Which language: this browser's own pick if it made one, otherwise the panel's (ui.lang), and
 * until the panel has said, the one it said last time or the browser's (lib/lang.js).
 */

const PICK = 'modesp-lang-pick';    // this browser's own choice; absent: as the panel
const PANEL = 'modesp-lang-panel';  // ui.lang as last seen, so a visit does not open in another
const OLD = 'modesp-lang';          // older builds wrote it on every start (lib/lang.js migratedPick)
export { LANGS };
/** Each language by its own name: a person looks for the one they read. */
export const LANG_NAMES = { uk: 'Українська', en: 'English', de: 'Deutsch', pl: 'Polski' };

function stored(key) {
  try { return localStorage.getItem(key); } catch (e) { return null; /* storage blocked */ }
}
function store(key, v) {
  try { v == null ? localStorage.removeItem(key) : localStorage.setItem(key, v); } catch (e) { /* storage blocked */ }
}

const nav = navigator.language || '';
if (stored(OLD) !== null) {
  store(PICK, migratedPick(stored(OLD), stored(PICK), nav));
  store(OLD, null);
}

/** This browser's own choice, or null: it speaks as the panel does. */
export const pick = writable(known(stored(PICK)));
/** The panel's language, ui.lang. */
export const panelLanguage = writable(known(stored(PANEL)));

state.subscribe((s) => {
  const code = panelCode(s['ui.lang']);
  if (!code || code === get(panelLanguage)) return;
  panelLanguage.set(code);
  store(PANEL, code);
});

export const language = derived([pick, panelLanguage], ([$pick, $panel]) => chosen($pick, $panel, nav));
const catalogs = writable({ uk });

async function load(lang) {
  if (get(catalogs)[lang]) return;
  try {
    const r = await fetch(`/lang/${lang}.json`);
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    const dict = await r.json();
    catalogs.update((m) => ({ ...m, [lang]: dict }));
  } catch (e) {
    console.warn(`language '${lang}' not loaded, Ukrainian stays:`, e.message);
  }
}

language.subscribe((lang) => {
  document.documentElement.lang = lang;
  load(lang);
});

/** The strings of the chosen language, Ukrainian under them. */
export const t = derived([language, catalogs], ([$lang, $cats]) =>
  $lang === 'uk' || !$cats[$lang] ? uk : { ...uk, ...$cats[$lang] });

/** This browser's language from now on; null: as the panel. */
export function pickLanguage(lang) {
  const v = known(lang);
  pick.set(v);
  store(PICK, v);
}

/** Marks a catalog key used from code (the i18n test looks for K() around a literal). */
export const K = (key) => key;

// The toasts' words for a failure: the panel's answers and the browser's said in
// the page's language, never `{"error":"busy"}` or «Failed to fetch» (lib/errors.js).
K('err.busy'); K('err.too_large'); K('err.bad_request'); K('err.not_found'); K('err.server'); K('err.code');
setErrorWords((raw) => humanError(raw, get(t), fmt));

/**
 * A count in words: `${base}.one|few|many|other` by the language's plural rules (Ukrainian and
 * Polish use all four, English and German one and other). Every catalog has all four forms, so
 * the key set stays one; mark the forms used with K() where the base is named.
 */
export function plural(dict, lang, base, n) {
  let rule = 'other';
  try { rule = new Intl.PluralRules(lang).select(n); } catch (e) { /* old browser */ }
  return fmt(dict[`${base}.${rule}`] ?? dict[`${base}.other`], { n });
}

// fmt() and msg() are plain functions in lib/msg.js, where the tests can load them in Node.
export { fmt, msg };
