#!/usr/bin/env python3
"""
m3_scheme.py — a dark Material 3 colour scheme from one seed colour, for modules/ui_lvgl/src/ui_m3.cpp.

The three schemes came from the design package (Material Theme Builder). Two of them are
moved: the blue one more saturated, darker and towards violet, the green one darker and
towards olive, each to a colour of its own. Those two colours are the seeds here, and each
becomes the scheme's primary AS IS: that is the colour chosen, and the swatch on the System
page is drawn in it.

Every other role is a tone of a palette built on the seed's hue, at the tones Material's dark
scheme uses (surface 6, containers 10..22, on_surface 90, primary_container 30, ...). Tone is
CIELAB L*, which is what Material's own "tone" is; hue and chroma here are CIELAB's rather
than CAM16's, which is close enough for a handful of fixed colours and needs nothing but the
standard library. A colour whose chroma does not fit in sRGB at that tone keeps its hue and
loses chroma until it does.

A seed too dark to be a primary — the red of the fourth scheme is L* 27, and a primary that
dark vanishes on a surface at 6 (1.8:1), while icons, the slider, the EQ curve and text
buttons are drawn in it — is given as SEED@TONE: the hue and chroma are the seed's, the
primary is lifted to TONE, and the seed's own darkness shows in the containers.

    python tools/ui/m3_scheme.py 7A82C0 9A9973 771910@52   # prints the C++ rows for ui_m3.cpp
"""

import math
import sys

# ── sRGB <-> CIELAB (D65) ──

def _lin(c):
    c /= 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4

def _gam(c):
    c = 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055
    return c * 255.0

WHITE = (0.95047, 1.0, 1.08883)

def _f(t):
    return t ** (1 / 3) if t > (6 / 29) ** 3 else t / (3 * (6 / 29) ** 2) + 4 / 29

def _finv(t):
    return t ** 3 if t > 6 / 29 else 3 * (6 / 29) ** 2 * (t - 4 / 29)

def rgb_to_lab(rgb):
    r, g, b = (_lin(c) for c in rgb)
    x = 0.4124564 * r + 0.3575761 * g + 0.1804375 * b
    y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b
    z = 0.0193339 * r + 0.1191920 * g + 0.9503041 * b
    fx, fy, fz = _f(x / WHITE[0]), _f(y / WHITE[1]), _f(z / WHITE[2])
    return 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)

def lab_to_rgb(L, a, b):
    fy = (L + 16) / 116
    fx, fz = fy + a / 500, fy - b / 200
    x, y, z = WHITE[0] * _finv(fx), WHITE[1] * _finv(fy), WHITE[2] * _finv(fz)
    r = 3.2404542 * x - 1.5371385 * y - 0.4985314 * z
    g = -0.9692660 * x + 1.8760108 * y + 0.0415560 * z
    bl = 0.0556434 * x - 0.2040259 * y + 1.0572252 * z
    return tuple(_gam(c) for c in (r, g, bl))

def in_gamut(rgb):
    return all(-0.5 <= c <= 255.5 for c in rgb)

def tone(hue_deg, chroma, L):
    """The colour at lightness L on this hue, chroma cut until it is an sRGB colour."""
    h = math.radians(hue_deg)
    c = chroma
    while c > 0:
        rgb = lab_to_rgb(L, c * math.cos(h), c * math.sin(h))
        if in_gamut(rgb):
            break
        c -= 0.25
    else:
        rgb = lab_to_rgb(L, 0, 0)
    return "0x%02X%02X%02X" % tuple(max(0, min(255, round(v))) for v in rgb)

# ── the scheme ──

def scheme(seed_hex, primary_tone=None):
    rgb = tuple(int(seed_hex[i:i + 2], 16) for i in (0, 2, 4))
    L, a, b = rgb_to_lab(rgb)
    hue = math.degrees(math.atan2(b, a)) % 360
    chroma = math.hypot(a, b)
    primary = "0x" + seed_hex.upper() if primary_tone is None else tone(hue, chroma, primary_tone)
    P  = lambda t: tone(hue, max(chroma, 30), t)          # primary palette
    S  = lambda t: tone(hue, 14, t)                       # secondary: the hue, quieter
    T  = lambda t: tone((hue + 60) % 360, 24, t)          # tertiary: sixty degrees on
    N  = lambda t: tone(hue, 4, t)                        # neutral, a trace of the hue
    NV = lambda t: tone(hue, 8, t)                        # neutral variant
    roles = [
        # surface, lowest, low, container, high, highest
        N(6), N(4), N(10), N(12), N(17), N(22),
        # on_surface, on_surface_var, outline, outline_var
        N(90), NV(80), NV(60), NV(30),
        # primary (the seed, or its hue at TONE), on_primary, primary_container, on_primary_container
        primary, P(15), P(30), P(90),
        # secondary_container, on_secondary_container
        S(30), S(90),
        # tertiary, tertiary_container, on_tertiary_container
        T(80), T(30), T(90),
        # inverse_surface, inverse_on_surface
        N(90), N(20),
    ]
    return L, hue, chroma, roles

def main():
    for arg in sys.argv[1:] or ["7A82C0", "9A9973", "771910@52"]:
        seed, _, t = arg.lstrip("#").partition("@")
        L, hue, chroma, r = scheme(seed, float(t) if t else None)
        print(f"    // seed #{seed.upper()}{'@' + t if t else ''}: L* {L:.0f}, hue {hue:.0f}, chroma {chroma:.0f}")
        print("     {" + ", ".join(r[0:6]) + ",")
        print("      " + ", ".join(r[6:10]) + ",")
        print("      " + ", ".join(r[10:14]) + ",")
        print("      " + ", ".join(r[14:16]) + ",")
        print("      " + ", ".join(r[16:19]) + ",")
        print("      " + ", ".join(r[19:21]) + ", ERROR_DARK},")
    return 0

if __name__ == "__main__":
    sys.exit(main())
