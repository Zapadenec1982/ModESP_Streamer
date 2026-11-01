/*
 * Addresses that moved, and where they live now. DSP is not one of the sections: it is
 * Налаштування › Звук › Еквалайзер. Service has long been
 * under System; a bookmark of #/service/... still lands on it. The section keeps its id "system"
 * (only its name became «Налаштування»), so #/system/... needs nothing.
 *
 * No imports: tools/tests/test_webui_nav.py loads this file in Node.
 */

/** The route an address means: the same array when it has not moved. */
export function canonical(path) {
  if (path[0] === 'dsp') return ['system', 'sound', 'eq', ...path.slice(1)];
  if (path[0] === 'service') return ['system', ...path];
  return path;
}
