"""A helper that reads the catalog must be reactive when the template calls it.

Svelte 4 re-evaluates a template expression only when a variable written in that expression
changes. A plain function that reads `$t` inside, called as `{label(x)}`, is therefore
evaluated once with the catalog that was there at the first render — the bundled Ukrainian
one — and keeps those words after `/lang/<lang>.json` arrives. That is how the English
Stations page said «кнопка 1» although en.json had `st.preset_n`.

The fix is `$: label = (x) => ...` (or passing `$t` into the call). This test finds plain
`function name(...)` and `const name = (...) =>` declarations whose body reads `$t`, and fails
when the markup calls one of them from an expression that does not itself mention `$t`.
Event handlers (`on:click={...}`) run when the event fires, so they are left alone.
"""

import re
import sys
from pathlib import Path

WEBUI_SRC = Path(__file__).resolve().parents[2] / "webui" / "src"

FUNC = re.compile(r"(?:^|\n)[ \t]*(?:export[ \t]+)?function[ \t]+(\w+)[ \t]*\([^)]*\)[ \t]*\{")
ARROW = re.compile(r"(?:^|\n)[ \t]*(?:const|let)[ \t]+(\w+)[ \t]*=[ \t]*(?:\([^)]*\)|\w+)[ \t]*=>")
READS_T = re.compile(r"\$t\b")


def _body(text, open_at):
    """The text from the brace at `open_at` to its match (inclusive)."""
    depth = 0
    for k in range(open_at, len(text)):
        if text[k] == "{":
            depth += 1
        elif text[k] == "}":
            depth -= 1
            if depth == 0:
                return text[open_at:k + 1]
    return text[open_at:]


def _arrow_body(script, end):
    """An arrow function's body: a braced block, or the expression up to the statement's end."""
    rest = script[end:].lstrip()
    if rest.startswith("{"):
        return _body(rest, 0)
    stop = re.search(r";|\n\s*\n|\n\s*(?:const|let|function|\$:|export)\b", rest)
    return rest[: stop.start()] if stop else rest


def catalog_helpers(script):
    """Names of non-reactive helpers whose body reads $t."""
    names = set()
    for m in FUNC.finditer(script):
        if READS_T.search(_body(script, script.index("{", m.end() - 1))):
            names.add(m.group(1))
    for m in ARROW.finditer(script):
        line_start = script.rfind("\n", 0, m.start() + 1) + 1
        if script[line_start:m.start() + 1].strip().startswith("$:"):
            continue
        if READS_T.search(_arrow_body(script, m.end())):
            names.add(m.group(1))
    return names


def template_expressions(markup):
    """Every {...} expression in the markup, with the text just before it."""
    i = 0
    while True:
        i = markup.find("{", i)
        if i < 0:
            return
        expr = _body(markup, i)
        yield markup[max(0, i - 40):i], expr
        i += len(expr)


def stale_calls(path):
    text = path.read_text(encoding="utf-8")
    m = re.search(r"<script(?![^>]*context=)[^>]*>(.*?)</script>", text, re.S)
    if not m:
        return []
    helpers = catalog_helpers(m.group(1))
    if not helpers:
        return []
    markup = text[: m.start()] + text[m.end():]
    markup = re.sub(r"<style.*?</style>", "", markup, flags=re.S)
    markup = re.sub(r"<script.*?</script>", "", markup, flags=re.S)
    found = []
    for before, expr in template_expressions(markup):
        if re.search(r"on:[\w|]+=\s*$", before):
            continue                     # an event handler: evaluated when it fires
        if READS_T.search(expr):
            continue                     # $t in the expression itself: Svelte tracks it
        for name in helpers:
            if re.search(r"\b%s\s*\(" % re.escape(name), expr):
                found.append(f"{path.relative_to(WEBUI_SRC)}: {name}() in {expr[:80]!r}")
    return found


def test_catalog_helpers_called_from_markup_are_reactive():
    problems = []
    for path in sorted(WEBUI_SRC.rglob("*.svelte")):
        problems += stale_calls(path)
    assert not problems, (
        "a helper reads $t but the template call does not mention it, so the text keeps the "
        "first catalog (make it `$: name = (...) => ...`):\n  " + "\n  ".join(problems))


def test_the_check_catches_the_stations_bug(tmp_path, monkeypatch):
    # The exact shape of the Stations bug, so the check above is known to bite.
    bad = tmp_path / "Bad.svelte"
    bad.write_text(
        "<script>\n  import { t } from './i18n.js';\n"
        "  function subtitle(st, i) {\n    return $t['st.preset_n'] + i;\n  }\n</script>\n"
        "<Row subtitle={subtitle(st, i)} />\n", encoding="utf-8")
    good = tmp_path / "Good.svelte"
    good.write_text(
        "<script>\n  import { t } from './i18n.js';\n"
        "  $: subtitle = (st, i) => $t['st.preset_n'] + i;\n"
        "  function tap() { return $t['x']; }\n</script>\n"
        "<Row subtitle={subtitle(st, i)} on:click={() => tap()} />\n", encoding="utf-8")
    monkeypatch.setattr(sys.modules[__name__], "WEBUI_SRC", tmp_path)
    assert stale_calls(bad)
    assert not stale_calls(good)
