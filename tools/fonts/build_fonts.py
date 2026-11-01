#!/usr/bin/env python3
"""Generate the LVGL fonts the UI needs, into modules/ui_lvgl/src/fonts/.

Material 3: Roboto for text and Material Symbols Rounded for
icons — the Material typeface and its own icon set. LVGL ships Montserrat without Cyrillic,
and ships no icon set that draws an AirPlay mark, so both families are built here.

Inputs and where they come from; the licences travel with the sources:
  @fontsource/roboto          SIL OFL 1.1   text faces (latin, latin-ext and cyrillic subsets,
                                            separate files)
  Material Symbols Rounded    Apache 2.0    icons — google/material-design-icons, fetched into
                                            build/ by fetch_symbols() and pinned by SHA-256 in
                                            symbols.lock; it is a 15 MB variable font and does
                                            not belong in the repository
  fontTools (pip)             MIT           pins the variable font's FILL axis to 0 and 1: the
                                            converter reads static fonts only
  lv_font_conv                MIT           the converter itself
  svg2ttf, svgpath            MIT           own glyphs (glyphs/*.svg) for what Material lacks —
                                            none today; the mechanism stays (build_glyphs.js)

Output is committed: the firmware build must not need node, pip or the network. Re-run this
script and commit the result whenever FACES, ICONS or FILLED below change.

  python tools/fonts/build_fonts.py            # everything
  python tools/fonts/build_fonts.py --only icons
  python tools/fonts/build_fonts.py --only text  # needs only @fontsource/roboto: no icon font
                                                 # fetch, and ui_fonts.h is left as it is

Run it with the ESP-IDF venv's python (that is where fontTools is installed).
"""

import argparse
import hashlib
import json
import subprocess
import sys
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
OUT = ROOT / "modules" / "ui_lvgl" / "src" / "fonts"
NM = HERE / "node_modules"
BUILD = HERE / "build"
ROBOTO = NM / "@fontsource" / "roboto" / "files"

# ── Material Symbols: fetched, pinned, instanced ──
SYMBOLS_LOCK = HERE / "symbols.lock"
SYMBOLS_VAR = BUILD / "MaterialSymbolsRounded.ttf"
SYMBOLS_CODEPOINTS = BUILD / "MaterialSymbolsRounded.codepoints"
SYMBOLS_FILL = {0: BUILD / "MaterialSymbolsRounded-fill0.ttf",
                1: BUILD / "MaterialSymbolsRounded-fill1.ttf"}

# ── character coverage ──
# The interface itself is Ukrainian, but the text faces also draw what other people wrote: track
# tags, station names, ICY titles, DLNA servers, phone names. Those come in every European
# language, and a glyph the face lacks is a box on the glass. A set cut to Ukrainian ("Russian-only
# letters we never render") and Latin-1 letters drew "Режисс□р" on the standby face, and boxes
# in Latin names too. The set is European:
#
#   latin subset     ASCII; all of Latin-1 — the no-break space tags are full of, © ® ½ ¿ ²,
#                    é ü ñ; ı Œ œ (Turkish, French), which @fontsource keeps here and not in
#                    latin-ext; – —, every quote mark (‘ ’ ‚ “ ” „ ‹ ›), • … ′ ″, € ™ and the
#                    minus. No arrow exists in any subset @fontsource ships, so the tech chip
#                    says "44.1 › 48 kHz" with U+203A; nor do ‐ ‑ ‒ ― ‛ ‡ ‰, in any subset.
#   latin-ext        Latin Extended-A: Polish, Czech, Slovak, Hungarian, Croatian, Slovene,
#                    Baltic, Turkish, Esperanto; Romanian ș ț (Latin Extended-B); and †, which
#                    @fontsource files here.
#   cyrillic         the whole basic block 0x400-0x45F (Ё ё, Belarusian Ў ў, Serbian and
#                    Macedonian), Ґ ґ, and №.
#
# Left out, and still a box: Greek, Vietnamese, CJK, emoji, Arabic, Hebrew. Measured: about
# 210 more glyphs per face, ≈ +235 KB of flash across the seven text faces.
ASCII = "0x20-0x7F"
LATIN1 = "0xA0-0xFF"
# lv_font_conv refuses a range in which the font has nothing, so the codepoints are listed one
# by one where a subset has holes.
PUNCT = ("0x131,0x152,0x153,0x2013,0x2014,0x2018,0x2019,0x201A,0x201C-0x201E,0x2022,0x2026,"
         "0x2032,0x2033,0x2039,0x203A,0x20AC,0x2122,0x2212")
TEXT_RANGE = f"{ASCII},{LATIN1},{PUNCT}"
LATIN_EXT = "0x100-0x130,0x132-0x151,0x154-0x17F,0x218-0x21B,0x2020"
CYR = "0x400-0x45F,0x490,0x491,0x2116"
# The standby clock shows "HH:MM" and nothing else. A full charset at 160 px would be
# hundreds of kilobytes of flash for glyphs no screen draws.
CLOCK_RANGE = "0x20,0x2D,0x30-0x39,0x3A"     # space, dash (the "--:--" before the first tick), digits, colon

# ── text faces: (symbol, weight, px, latin range, latin-ext range or None, cyrillic range or None) ──
# One face per type role in modules/ui_lvgl/src/ui_m3.h, and no more: every face is flash.
# Nothing under 16 px: 12 or 14 cannot be read on the glass from where a stereo is looked at,
# and 16 reads small there too — so the whole scale is a step up from the mock's, and the
# smallest role is 16.
FACES = [
    ("ui_font_500_16",  500, 16,  TEXT_RANGE, LATIN_EXT, CYR),   # label_s
    ("ui_font_500_18",  500, 18,  TEXT_RANGE, LATIN_EXT, CYR),   # label_m
    ("ui_font_400_18",  400, 18,  TEXT_RANGE, LATIN_EXT, CYR),   # body_m
    ("ui_font_400_20",  400, 20,  TEXT_RANGE, LATIN_EXT, CYR),   # body_l, title_s
    ("ui_font_400_24",  400, 24,  TEXT_RANGE, LATIN_EXT, CYR),   # title_m
    ("ui_font_500_32",  500, 32,  TEXT_RANGE, LATIN_EXT, CYR),   # headline
    ("ui_font_600_48",  600, 48,  TEXT_RANGE, LATIN_EXT, CYR),   # display — the track title
    ("ui_font_300_160", 300, 160, CLOCK_RANGE, None, None),   # clock — standby, digits only
]

# ── icons: LVGL symbol name → Material Symbols name ──
# Resolved to codepoints through the .codepoints file that ships with the font, so a name
# that does not exist fails here, at build time, rather than as a tofu box on the panel.
ICONS = {
    # transport
    "PLAY":         "play_arrow",
    "PAUSE":        "pause",
    "STOP":         "stop",
    "NEXT":         "skip_next",
    "PREV":         "skip_previous",
    "REPEAT":       "repeat",
    "REPEAT_ONE":   "repeat_one",
    "SHUFFLE":      "shuffle",
    # volume
    "VOLUME":       "volume_up",
    "VOLUME_DOWN":  "volume_down",
    "MUTE":         "volume_off",
    # sources
    "BLUETOOTH":    "bluetooth",
    "RADIO":        "radio",
    "SD":           "sd_card",
    "AIRPLAY":      "airplay",
    "CAST":         "cast",              # DLNA
    "AUX":          "input",
    "USB":          "usb",               # a computer, over USB audio
    "SPEAKER":      "speaker_group",     # the Sources section
    # shell
    "MENU":         "menu",
    "MENU_OPEN":    "menu_open",
    "SEARCH":       "search",
    "MOON":         "bedtime",           # standby
    "POWER":        "power_settings_new",
    "BACK":         "arrow_back",
    "CHEVRON":      "chevron_right",
    "CHEVRON_LEFT": "chevron_left",      # the DLNA browser's previous page
    "DROPDOWN":     "arrow_drop_down",   # a field that opens a menu (the EQ presets)
    "REFRESH":      "refresh",           # look for media servers again
    "CLOSE":        "close",
    "CHECK":        "check",
    "CHECK_CIRCLE": "check_circle",
    "PLAY_CIRCLE":  "play_circle",       # the Player section
    "EQ":           "tune",              # the DSP section
    "SETTINGS":     "settings",          # the System section
    # content
    "MUSIC":        "music_note",
    "PLAYLIST":     "queue_music",
    "LIBRARY":      "library_music",
    "FOLDER":       "folder",
    "ALBUM":        "album",
    "HEART":        "favorite",
    "ADD":          "add",
    "PLAYLIST_ADD": "playlist_add",      # queue a DLNA track to play next
    "REMOVE":       "remove",
    "SWAP":         "swap_horiz",
    "GRAPHIC_EQ":   "graphic_eq",
    "SPEED":        "speed",             # the limiter
    # settings and service
    "WIFI":         "wifi",
    "CLOCK":        "schedule",
    "SUN":          "light_mode",
    "BRIGHTNESS":   "brightness_6",
    "PALETTE":      "palette",
    "CONTRAST":     "contrast",
    "HUB":          "hub",
    "LANGUAGE":     "language",
    "UPDATE":       "system_update",
    "INFO":         "info",
    "WARNING":      "warning",
    "ERROR":        "error",
    "MEMORY":       "memory",
    "CPU":          "developer_board",
    "LINK":         "link",
    "SPLIT":        "call_split",        # Кросовер: the outputs split the sound
    # networks: the Wi-Fi page's signal, the lock on a protected network
    "WIFI_1":       "network_wifi_1_bar",
    "WIFI_2":       "network_wifi_2_bar",
    "WIFI_3":       "network_wifi_3_bar",
    "WIFI_4":       "signal_wifi_4_bar",
    "WIFI_OFF":     "wifi_off",
    "LOCK":         "lock",
    # the on-screen keyboard
    "BACKSPACE":    "backspace",
    "SHIFT":        "shift",
    "VISIBLE":      "visibility",        # show the password
    "HIDDEN":       "visibility_off",
    # Bluetooth settings: the paired phones, forgetting one, renaming the device
    "PHONE":        "smartphone",
    "DELETE":       "delete",
    "EDIT":         "edit",
    "LINK_OFF":     "link_off",          # disconnect the phone
    "BT_SEARCH":    "bluetooth_searching",
    "BT_OFF":       "bluetooth_disabled",
}
# The glyphs the design also draws FILLED: the active rail item, the pressed favourite, the
# transport row, the chosen source. Everything else is outlined only, which keeps the second
# icon family small.
FILLED = {
    "PLAY", "PAUSE", "STOP", "NEXT", "PREV", "VOLUME", "VOLUME_DOWN", "MUTE",
    "HEART", "CHECK_CIRCLE", "PLAY_CIRCLE", "SPEAKER", "EQ", "SETTINGS",
    "BLUETOOTH", "RADIO", "SD", "AIRPLAY", "CAST", "AUX", "USB", "MOON",
}
# Sizes are the type roles' (ui_m3.h): 20 small, 24 the default, 28 large, 40 the play button.
ICON_SIZES = [20, 24, 28, 40]
FILL_SIZES = [24, 28, 40]

# Own glyphs, for what Material Symbols does not draw. None today: Material has an AirPlay
# mark, which is what glyphs/ existed for. The table and build_glyphs.js stay so the next
# missing glyph is an SVG and a line here, not a redesign of the pipeline.
OWN_ICONS = {}          # LVGL symbol name → codepoint in U+F800..F8FF, drawn in glyphs/<name>.svg
OWN_TTF = BUILD / "player-glyphs.ttf"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(args):
    npx = "npx.cmd" if sys.platform == "win32" else "npx"
    cmd = [npx, "--no-install", "lv_font_conv"] + args
    r = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"lv_font_conv failed:\n{r.stdout}\n{r.stderr}")


def fetch_symbols():
    """Material Symbols Rounded into build/, exactly the bytes symbols.lock names."""
    lock = json.loads(SYMBOLS_LOCK.read_text(encoding="utf-8"))
    BUILD.mkdir(exist_ok=True)
    for path, entry in ((SYMBOLS_VAR, lock["ttf"]), (SYMBOLS_CODEPOINTS, lock["codepoints"])):
        if not path.exists() or sha256(path) != entry["sha256"]:
            print(f"  fetching {entry['url']}")
            urllib.request.urlretrieve(entry["url"], path)
            got = sha256(path)
            if got != entry["sha256"]:
                path.unlink()
                sys.exit(f"{path.name}: SHA-256 {got} does not match symbols.lock — "
                         f"upstream moved; review the change and update the lock deliberately")


def instance_symbols():
    """Two static fonts out of the variable one: FILL 0 and FILL 1 at weight 400, opsz 24."""
    for fill, out in SYMBOLS_FILL.items():
        if out.exists() and out.stat().st_mtime >= SYMBOLS_VAR.stat().st_mtime:
            continue
        r = subprocess.run([sys.executable, "-m", "fontTools.varLib.instancer", str(SYMBOLS_VAR),
                            f"FILL={fill}", "wght=400", "GRAD=0", "opsz=24", "-o", str(out)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"fontTools instancer failed (pip install fonttools into the IDF venv):\n"
                     f"{r.stdout}\n{r.stderr}")


def load_codepoints():
    table = {}
    for line in SYMBOLS_CODEPOINTS.read_text(encoding="utf-8").splitlines():
        name, _, hexcode = line.partition(" ")
        if name and hexcode:
            table[name] = int(hexcode, 16)
    return table


def resolve_icons():
    """ICONS by Material name → {LVGL symbol: codepoint}; loud about a name that is not there."""
    table = load_codepoints()
    missing = [n for n in ICONS.values() if n not in table]
    if missing:
        sys.exit(f"not in Material Symbols Rounded: {', '.join(missing)}")
    cps = {key: table[name] for key, name in ICONS.items()}
    cps.update(OWN_ICONS)
    return cps


def build_text():
    for name, weight, size, latin_r, ext_r, cyr_r in FACES:
        latin = ROBOTO / f"roboto-latin-{weight}-normal.woff"
        args = ["--font", str(latin), "-r", latin_r]
        # @fontsource splits subsets into separate files, so each is a --font of its own
        if ext_r:
            args += ["--font", str(ROBOTO / f"roboto-latin-ext-{weight}-normal.woff"), "-r", ext_r]
        if cyr_r:
            args += ["--font", str(ROBOTO / f"roboto-cyrillic-{weight}-normal.woff"), "-r", cyr_r]
        args += ["--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
                 "--lv-include", "lvgl.h", "-o", str(OUT / f"{name}.c")]
        run(args)
        print(f"  {name:<18} {(OUT / (name + '.c')).stat().st_size // 1024:>5} KB (source)")


def build_icons(cps):
    material = ",".join(f"0x{cp:X}" for key, cp in sorted(cps.items()) if key not in OWN_ICONS)
    own = ",".join(f"0x{cp:X}" for key, cp in sorted(cps.items()) if key in OWN_ICONS)
    if OWN_ICONS:
        r = subprocess.run(["node", str(HERE / "build_glyphs.js"), str(OWN_TTF)],
                           cwd=HERE, capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"build_glyphs.js failed:\n{r.stdout}\n{r.stderr}")

    def convert(name, font, ranges, size):
        args = ["--font", str(font), "-r", ranges]
        if OWN_ICONS:
            args += ["--font", str(OWN_TTF), "-r", own]      # two fonts into one face
        run(args + ["--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
                    "--lv-include", "lvgl.h", "-o", str(OUT / f"{name}.c")])
        print(f"  {name:<18} {(OUT / (name + '.c')).stat().st_size // 1024:>5} KB (source)")

    for size in ICON_SIZES:
        convert(f"ui_font_icons_{size}", SYMBOLS_FILL[0], material, size)
    filled = ",".join(f"0x{cps[key]:X}" for key in sorted(FILLED) if key in cps)
    for size in FILL_SIZES:
        convert(f"ui_font_icons_{size}_fill", SYMBOLS_FILL[1], filled, size)


def expected_files():
    names = [name for name, *_ in FACES]
    names += [f"ui_font_icons_{s}" for s in ICON_SIZES]
    names += [f"ui_font_icons_{s}_fill" for s in FILL_SIZES]
    return {f"{n}.c" for n in names}


def prune():
    """A face that left FACES leaves the tree too, or it would sit in flash for ever."""
    keep = expected_files()
    for p in OUT.glob("ui_font_*.c"):
        if p.name not in keep:
            p.unlink()
            print(f"  removed {p.name}")


def write_header(cps):
    """One header declaring every generated face plus the icon string constants."""
    lines = [
        "/**",
        " * @file ui_fonts.h",
        " * @brief Generated LVGL faces and icon glyphs — DO NOT EDIT.",
        " *",
        " * Produced by tools/fonts/build_fonts.py from @fontsource/roboto (SIL OFL 1.1) and",
        " * Material Symbols Rounded (Apache 2.0, google/material-design-icons). Re-run that",
        " * script after changing its FACES / ICONS / FILLED tables and commit the result; the",
        " * firmware build never needs node, pip or the network.",
        " *",
        " * Naming: ui_font_<weight>_<px> for text, ui_font_icons_<px> for outlined icons and",
        " * ui_font_icons_<px>_fill for the filled variants. The UI picks a face through the",
        " * type roles in ui_m3.h, never by referencing these directly.",
        " */",
        "",
        "#pragma once",
        "",
        '#include "lvgl.h"',
        "",
        "#ifdef __cplusplus",
        'extern "C" {',
        "#endif",
        "",
    ]
    for name, *_ in FACES:
        lines.append(f"LV_FONT_DECLARE({name});")
    lines.append("")
    for size in ICON_SIZES:
        lines.append(f"LV_FONT_DECLARE(ui_font_icons_{size});")
    for size in FILL_SIZES:
        lines.append(f"LV_FONT_DECLARE(ui_font_icons_{size}_fill);")
    lines += ["", "// Material Symbols as UTF-8 literals, usable anywhere LVGL takes text. A glyph",
              "// drawn with a *_fill face is the filled variant; the string is the same.", ""]
    for key, cp in cps.items():
        utf8 = "".join(f"\\x{b:02X}" for b in chr(cp).encode("utf-8"))
        what = ICONS.get(key, "own glyph")
        lines.append(f'#define UI_SYM_{key:<12} "{utf8}"   // U+{cp:04X} {what}')
    lines += ["", "#ifdef __cplusplus", "}", "#endif", ""]
    (OUT / "ui_fonts.h").write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"  ui_fonts.h         {len(cps)} icons, {len(FACES)} text faces")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", choices=["text", "icons", "header"])
    a = ap.parse_args()
    if not ROBOTO.exists():
        sys.exit("@fontsource/roboto missing — run `npm install` in tools/fonts first")
    OUT.mkdir(parents=True, exist_ok=True)
    if a.only == "text":
        # Text faces need nothing from the icon pipeline, and ui_fonts.h only names them —
        # rerun everything when FACES gains or loses a face.
        build_text()
        return
    fetch_symbols()
    instance_symbols()
    cps = resolve_icons()
    if a.only in (None, "text"):
        build_text()
    if a.only in (None, "icons"):
        build_icons(cps)
    if a.only is None:
        prune()
    write_header(cps)


if __name__ == "__main__":
    main()
