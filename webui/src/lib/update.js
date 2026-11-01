/*
 * «Веб-інтерфейс оновлено · Перезавантажити». A tab left open across a flash of data.bin keeps
 * running the page it loaded before — no-store does nothing for a page already running — and
 * behaves as the old web did until it is reloaded. So the page notes /build.txt
 * (scripts/deploy.js: a line per deployed file with its hash) the first time the WebSocket opens,
 * and looks again each time it opens again: the panel restarted, or the tab was shown again
 * (stores/state.js wsOpened). Nothing is polled. A different list is said once.
 *
 * No imports: tools/tests/test_webui_update.py runs this in Node; App.svelte wires it.
 */

/**
 * `fetchText()` gives /build.txt's text, or null; `onChange()` is told once that the web on the
 * panel is not the one this page runs. Returns what to call each time the socket opens (true);
 * false is ignored.
 */
export function createBuildWatch(fetchText, onChange) {
  let first = null;      // the list this page started with
  let told = false;
  return async function connected(up) {
    if (!up || told) return;
    let text = null;
    try {
      text = await fetchText();
    } catch (e) {
      return;            // the next time it comes back asks again
    }
    if (typeof text !== 'string' || !text) return;
    if (first === null) {
      first = text;
      return;
    }
    if (text !== first) {
      told = true;
      onChange();
    }
  };
}
