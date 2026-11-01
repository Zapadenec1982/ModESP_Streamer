import { writable, derived } from 'svelte/store';
import { canonical } from '../lib/routes.js';

/*
 * Hash routes. The hash never reaches the device's static server, which
 * answers 404 for paths of 52 characters or more; the browser's back button and bookmarks work.
 *
 *   #/player  #/source  #/system                  the three sections, as in the panel's drawer
 *   #/stations  #/stations/<top|countries|…>     the station list and the catalog
 *   #/library  #/dlna                             the card's tracks, the DLNA media servers
 *   #/system/sound/eq                             the equalizer, Налаштування › Звук
 *   #/system/language  #/system/service           pages under Налаштування
 *   #/system/service/<calibration|screenshot|…>   pages under Service; calibration/port its 2nd task
 *
 * An address that moved (#/dsp, #/service/…) is shown as its new one in place (lib/routes.js).
 */

export const SECTIONS = ['player', 'source', 'system'];

function split(hash) {
  return (hash || '').replace(/^#\/?/, '').split('/').filter(Boolean).map(decodeURIComponent);
}

function parse(hash) {
  const path = split(hash);
  return path.length ? canonical(path) : ['player'];
}

/** An old address becomes the new one in the address bar, without a step in the history. */
function tidy() {
  const path = split(location.hash);
  const now = canonical(path);
  if (now !== path) history.replaceState(null, '', '#/' + now.map(encodeURIComponent).join('/'));
}
tidy();

export const route = writable(parse(location.hash));

// In-app history, so ← goes back where the person came from (Service's calibration opened from DSP
// returns to DSP), and falls back to the parent on a fresh deep link.
// A page opened anew starts at its top; ← comes back to where the page was left. The window
// scrolls, not the page, so without this a row low on a long page (Service) opened the next page
// as far down.
const trail = [location.hash || '#/player'];
const scrolls = [];

window.addEventListener('hashchange', () => {
  tidy();
  const h = location.hash || '#/player';
  if (trail.length > 1 && trail[trail.length - 2] === h) {
    trail.pop();
    const y = scrolls.pop() || 0;
    route.set(parse(h));
    requestAnimationFrame(() => window.scrollTo(0, y));
  } else {
    scrolls.push(window.scrollY);
    trail.push(h);
    route.set(parse(h));
    window.scrollTo(0, 0);
  }
});

export function href(path) {
  return '#/' + path;
}

export function go(path) {
  location.hash = href(path);
}

/**
 * A page that is finished (a wizard's «Готово», «Застосувати») hands over to `path`: back to it
 * when it led here, otherwise in this page's place in the history — so ← does not open the
 * finished page again.
 */
export function done(path) {
  const h = href(path);
  if (trail.length > 1 && trail[trail.length - 2] === h) {
    history.back();
    return;
  }
  trail.pop();
  scrolls.pop();
  location.replace(h);
}

/** Where a page belongs when nobody led to it: its parent in the map. */
function parentOf(r) {
  if (r[0] === 'stations') return 'player';
  // a page under one of Налаштування's pages goes back to that page (the wizard to Колонки)
  if (r[0] === 'system' && r.length > 2 && ['service', 'sound', 'speakers'].includes(r[1])) return `system/${r[1]}`;
  if (r[0] === 'system' && r.length > 1) return 'system';
  if (r[0] === 'components') return 'system/service';
  return 'player';
}

export function back(r) {
  if (trail.length > 1) history.back();
  else go(parentOf(r));
}

// The section a page counts as: its own for a section, otherwise the one it was opened from
// (as the panel's drawer marks the page a list was opened from), or its home on a cold deep link.
const HOME = { stations: 'player', library: 'player', dlna: 'player' };
const first = parse(location.hash)[0];
let lastSection = SECTIONS.includes(first) ? first : HOME[first] || 'system';
export const section = derived(route, (r) => {
  if (SECTIONS.includes(r[0])) lastSection = r[0];
  return lastSection;
});

export const isSectionPage = derived(route, (r) => r.length === 1 && SECTIONS.includes(r[0]));
