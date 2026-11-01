"""The panel's strings: one catalog with the web, tables in flash.

The C++ side (tr, plural, fmt, num) is tests/host/test_i18n.cpp. These hold the generator to its
input and output: the committed tables are what the catalogs and the code say, every string the
panel draws has glyphs in every text face, no translation is wider than its Ukrainian unless
i18n_fit.json says so, and screens that have moved to the catalog keep no text of their own.
"""

import json
import re
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "i18n"))

import fit                          # noqa: E402
import font_metrics                 # noqa: E402
import gen_panel_strings as gen     # noqa: E402

UI = ROOT / "modules" / "ui_lvgl"
CYRILLIC = re.compile(r"[Ѐ-ӿ]")

# Files of modules/ui_lvgl that still spell their text out. Empty: every screen reads the
# catalog. The list stays for a file that may need it again, and the test below fails both ways: a
# file not on it with Cyrillic in a string, and a file on it with none left.
PENDING = set()
# Never translated: bench screens, behind their own Kconfig switches, and the
# languages' own names (i18n::lang_name), each spelled in its own language.
NOT_TRANSLATED = {"src/screen_m3test.cpp", "src/ui_scroll_bench.cpp", "src/i18n.cpp"}
# A keyboard's caps: its letters and its layout's code ("УК") are what they are in any language.
# Longer text there is a word, and words come from the catalog.
KEYBOARDS = {"src/ui_search_kb.cpp"}
KEY_CAP = 2


@pytest.fixture(scope="module")
def cats():
    return gen.load_catalogs()


@pytest.fixture(scope="module")
def model(cats):
    return gen.resolve(cats, gen.scan_uses())


def test_generated_files_are_fresh(cats):
    _, files = gen.generate(cats)
    for path, text in files.items():
        assert path.exists() and path.read_text(encoding="utf-8") == text, (
            f"{path.relative_to(ROOT)} is stale: run tools/i18n/gen_panel_strings.py")


def test_the_panel_uses_the_catalog(model):
    # The tables are not empty by accident: the shell and System are on the catalog.
    assert "nav.player" in model["S"] and "pair.numeric" in model["S"] and "page.language" in model["S"]


def test_every_panel_string_has_glyphs(cats, model):
    faces = font_metrics.text_faces()
    for lang in gen.LANGS:
        for label, text in gen.entries(cats, model, lang):
            for face in faces:
                missing = face.missing(text)
                assert not missing, f"{label} [{lang}] {text!r}: {face.name} has no {sorted(missing)}"


def test_country_and_genre_names_have_glyphs():
    # Names from data, not the catalog: CLDR's countries in four columns and the genre file's
    # names. A letter the faces lack would be a box in the station list.
    countries = (ROOT / "components/radio_catalog/src/countries.inc").read_text(encoding="utf-8")
    names = [n for row in re.findall(r'^\{"\w\w", \{(.*)\}\},$', countries, re.M)
             for n in re.findall(r'"((?:[^"\\]|\\.)*)"', row)]
    assert len(names) == 4 * 264
    genres = json.loads((ROOT / "data/www/radio_genres.json").read_text(encoding="utf-8"))["genres"]
    names += [g[k] for g in genres for k in ("name", "name_en", "name_de", "name_pl") if k in g]
    face = font_metrics.Face(font_metrics.FONTS / "ui_font_400_20.c")
    missing = {n: sorted(face.missing(n)) for n in names if face.missing(n)}
    assert not missing, f"no glyph for: {missing}"


def test_translations_fit(cats):
    probs = fit.problems(fit.measure(cats), fit.load())
    assert not probs, "\n".join(probs)


def test_no_text_left_in_moved_files():
    offenders, done = [], []
    for p in sorted(UI.rglob("*")):
        if p.suffix not in (".cpp", ".h", ".c") or "fonts" in p.parts or p in gen.GENERATED:
            continue
        rel = p.relative_to(UI).as_posix()
        if rel in NOT_TRANSLATED:
            continue
        spelled = [s for kind, s in gen.cpp_tokens(p.read_text(encoding="utf-8"))
                   if kind == "string" and CYRILLIC.search(s)
                   and not (rel in KEYBOARDS and len(s) <= KEY_CAP)]
        if spelled and rel not in PENDING:
            offenders.append(f"{rel}: {spelled[:3]}")
        if not spelled and rel in PENDING:
            done.append(rel)
    assert not offenders, "text in a file that is on the catalog — use a key: " + "; ".join(offenders)
    assert not done, f"no text left in {done}: take them off PENDING"


def test_every_eq_preset_has_a_name():
    # screen_eq.cpp names the presets by id, as the web does; a preset added to the component
    # and not to that table would show the component's Ukrainian name in every language.
    # the table's rows: an id, a name, the gains (an id a sound had before is not a row)
    ids = re.findall(r'\{\s*"([a-z_]+)",\s*"[^"]*",\s*\{', (ROOT / "components/audio_dsp/src/eq_presets.cpp").read_text(encoding="utf-8"))
    named = re.findall(r'\{"([a-z_]+)",\s*S::eq_preset_', (UI / "src/screen_eq.cpp").read_text(encoding="utf-8"))
    assert ids and sorted(ids) == sorted(named), f"eq_presets.cpp {sorted(ids)} vs screen_eq.cpp {sorted(named)}"


def test_modules_publish_messages_not_text():
    # What these modules put in state or in the DLNA view is "@<key>" of the catalog.
    for rel in ("modules/airplay/src/airplay_module.cpp", "modules/audio/src/audio_module.cpp",
                "components/dlna_client/src/dlna_browser.cpp"):
        spelled = [s for kind, s in gen.cpp_tokens((ROOT / rel).read_text(encoding="utf-8"))
                   if kind == "string" and CYRILLIC.search(s)]
        assert not spelled, f"{rel}: text instead of a message: {spelled[:3]}"


# ── the generator itself, on a catalog and sources made up here ──

def _cats(extra=None):
    base = {"a.b": "Ба", "a.big": "Straße", "c.n.one": "{n} рядок", "c.n.few": "{n} рядки",
            "c.n.many": "{n} рядків", "c.n.other": "{n} рядка"}
    base.update(extra or {})
    return {lang: dict(base) for lang in gen.LANGS}


def _src(tmp_path, code):
    p = tmp_path / "x.cpp"
    p.write_text(code, encoding="utf-8")
    return [p]


def test_generator_reads_ids_from_code_only(tmp_path):
    files = _src(tmp_path, '// S::nope in a comment\nauto s = "S::nope in a string";\n'
                           'tr(S::a_b); tr(SU::a_big); plural(P::c_n, 3); /* P::nope */\n')
    model = gen.resolve(_cats(), gen.scan_uses(files))
    assert model == {"S": ["a.b"], "SU": ["a.big"], "P": ["c.n"], "M": []}


def test_generator_tables(tmp_path):
    files = _src(tmp_path, "tr(S::a_b); tr(SU::a_big); plural(P::c_n, 3);")
    cats = _cats()
    cats["en"]["a.b"] = "B"
    model = gen.resolve(cats, gen.scan_uses(files))
    uk = [t for _, t in gen.entries(cats, model, "uk")]
    assert uk == ["Ба", "STRASSE", "{n} рядок", "{n} рядки", "{n} рядків", "{n} рядка"]
    assert [t for _, t in gen.entries(cats, model, "en")][0] == "B"
    cpp = gen.render_tables(cats, model)
    assert '"Ба\\0"' in cpp and "const uint16_t S_UK[] = {0};" in cpp
    # Ukrainian is 4 bytes + NUL, so the capitals start at 5 and the four forms after them.
    assert "const uint16_t SU_UK[] = {5};" in cpp
    h = gen.render_header(model)
    assert "a_b," in h and "a_big," in h and "c_n," in h


def test_generator_falls_back_to_ukrainian(tmp_path):
    files = _src(tmp_path, "tr(S::a_b);")
    cats = _cats()
    cats["de"]["a.b"] = " "
    model = gen.resolve(cats, gen.scan_uses(files))
    assert gen.entries(cats, model, "de")[0][1] == "Ба"


@pytest.mark.parametrize("code, extra, message", [
    ("tr(S::a_c);", None, "no key"),
    ("plural(P::a_b, 1);", None, "no <key>.one"),
    ("tr(S::a_b);", {"a_b": "x"}, "would both be"),
    ("plural(P::d, 1);", {"d.one": "x", "d.few": "x", "d.many": "x"}, "lacks the forms"),
])
def test_generator_refuses(tmp_path, code, extra, message):
    cats = _cats(extra)
    with pytest.raises(gen.GenError, match=message):
        gen.resolve(cats, gen.scan_uses(_src(tmp_path, code)))


def test_c_string_escapes():
    assert gen.c_string('a"b\\c') == 'a\\"b\\\\c'
    with pytest.raises(gen.GenError):
        gen.c_string("a\tb")


def test_fit_rule():
    widths = {"x": {"uk": 50.0, "en": 40.0, "de": 60.0, "pl": 50.4}}
    assert fit.wider(widths) == {"x": {"de": 60.0}}
    assert fit.problems(widths, {"checked": {}, "unchecked": {}})
    assert not fit.problems(widths, {"checked": {}, "unchecked": {"x": {"de": 60.0}}})
    assert fit.problems(widths, {"checked": {"x": {"de": 55.0}}, "unchecked": {}})
    assert fit.problems(widths, {"checked": {"gone": {"de": 1}}, "unchecked": {"x": {"de": 60.0}}})


def test_fit_accepts_what_was_seen():
    def state():
        return {"checked": {"a": {"de": 1.0}}, "unchecked": {"a": {"pl": 2.0}, "b": {"de": 3.0, "pl": 4.0}}}
    assert fit.accept(state(), {("b", "pl"), ("c", "de")}) == {
        "checked": {"a": {"de": 1.0}, "b": {"pl": 4.0}},
        "unchecked": {"a": {"pl": 2.0}, "b": {"de": 3.0}}}
    assert fit.accept(state()) == {
        "checked": {"a": {"de": 1.0, "pl": 2.0}, "b": {"de": 3.0, "pl": 4.0}}, "unchecked": {}}


def test_metrics_are_the_faces_own():
    face = font_metrics.Face(font_metrics.FONTS / "ui_font_400_20.c")
    assert face.width("") == 0
    assert face.width("ab") > face.width("a") > 0
    assert face.missing("Łódź ß ї →") == {"→"}


def test_generator_reads_messages_and_their_arguments(tmp_path):
    cats = _cats({"m.p": "{hz} Hz ({k} z {n})", "m.q": "Q"})
    files = _src(tmp_path, 'say("@m.q"); snprintf(b, n, "@m.p" MSG_ARG "hz=%d" MSG_ARG "k=%d" MSG_ARG "n=%d", 1, 2, 3);\n'
                           '// "@m.nope" in a comment\n')
    model = gen.resolve(cats, gen.scan_uses(files))
    assert model["M"] == ["m.p", "m.q"] and "m.p" in model["S"] and "m.q" in model["S"]
    assert '{"m.p", S::m_p},' in gen.render_tables(cats, model)
    assert "MESSAGE_COUNT = 2;" in gen.render_header(model)


@pytest.mark.parametrize("code, message", [
    ('say("@m.nope");', "no such key"),
    ('snprintf(b, n, "@m.p" MSG_ARG "hz=%d", 1);', "arguments"),
])
def test_generator_refuses_a_message(tmp_path, code, message):
    with pytest.raises(gen.GenError, match=message):
        gen.resolve(_cats({"m.p": "{hz} Hz ({k})"}), gen.scan_uses(_src(tmp_path, code)))
