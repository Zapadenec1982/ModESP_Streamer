#!/usr/bin/env python3
"""
fit_presets.py — the DSP page's presets, fitted onto the panel's own ten peaking bands.

A preset is a curve: gains at a handful of frequencies with straight lines between them on a
log axis — how a graphic EQ's sliders are read. The panel does not have sliders, it has ten
parametric peaking filters, and they overlap. Setting each filter to the curve's value at its
own centre (what the firmware did first) sums the overlaps into bumps: up to 4 dB of ripple
per octave and 3.5 dB off the curve with octave-wide filters.

So the gains are FITTED: least squares on the heard response — the sum of the ten filters'
analogue prototypes (the RBJ cookbook's), which is what the audio core plays at every rate since
it designs peaks and shelves by matching them (Design::MATCHED, components/audio_dsp/src/
dsp_chain.cpp; within 0.9 dB at 44.1 kHz and 0.6 at 48, closer above) — plus a small penalty on
the second difference of neighbouring gains, so the handles on the screen read as a shape instead
of a saw. The result is a C++ table for components/audio_dsp/src/eq_presets.cpp, which the
panel's DSP page and the web both use; the firmware applies it as is.

The fit is not against the bilinear (RBJ digital) responses at 48 kHz: near Nyquist those are a
different curve from the same filters at 96 kHz — and from what the panel plays.

Sources of the preset curves:
  * Winamp's presets, as Audacious ships them (0.375 is Winamp's neutral step, read as 0):
    https://gist.github.com/chughpiyush/1bc18dc9c982e325a0422a93a9007db5
  * Winamp's band frequencies, as VLC's modules/audio_filter/equalizer_presets.h lists them.
  * "Тонкомпенсація" and "Ніч" are this panel's own, given at the octave centres.
  * "Джаз" and "Хіп-хоп" are this panel's own too, given at the octave centres: no music app
    publishes the numbers behind those names, so the curves follow how the apps describe them.
    WiiM Home (faq.wiimhome.com/en/support/solutions/articles/72000594355): Jazz lifts
    "sub-bass, midrange, and brilliance", Hip-Hop "sub-bass and bass"; Producer Hive and
    MacObserver on Apple Music's Hip-Hop: bass-heavy, a little top-end lift, the voice kept
    clear. Moderate on purpose, and checked by ear on the panel.
  * "Рівно" is every band at 0 dB, so all ten are off (eq_build_preset).

The order is the table's: the sounds offered first
(«Власний» before them is the user's own set, not a curve), then the presets not offered,
kept so that a set saved as one of them still plays under its name.

  python tools/eq/fit_presets.py --eval               # how Q and the penalty trade off
  python tools/eq/fit_presets.py                      # the C++ table, with the chosen Q, MU
Needs numpy (host only; the firmware build never runs this).
"""

import argparse
import math
import sys

import numpy as np

OCT_F = np.array([31.25, 62.5, 125, 250, 500, 1000, 2000, 4000, 8000, 16000], dtype=float)
WINAMP_F = [60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000]

# The band layout every preset uses, and the chosen trade-off (see --eval): peaks one octave
# apart at Q 1.0, the two end bands shelves so the curve holds its level out to 20 Hz and 20 kHz
# instead of rolling off past the last peak, and a light smoothness penalty on the gains.
Q = 1.0
MU = 0.002
LOW_SHELF_F, HIGH_SHELF_F, SHELF_Q = 40.0, 12000.0, 0.707

# id (what the web names it by), name (as the panel shows it), frequencies, gains (dB).
# The sounds offered, in the order they are offered (after «Власний», the user's own set):
SOUNDS = [
    ("flat", "Рівно",          list(OCT_F), [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    ("pop", "Поп",            WINAMP_F, [-1.125, 3, 4.5, 4.875, 3.375, -0.75, -1.5, -1.5, -1.125, -1.125]),
    ("rock", "Рок",            WINAMP_F, [4.875, 3, -3.375, -4.875, -2.25, 2.625, 5.625, 6.75, 6.75, 6.75]),
    ("jazz", "Джаз",           list(OCT_F), [3, 2.5, 1, 0, 0, 1, 1.5, 1, 2, 3]),
    ("classical", "Класика",        WINAMP_F, [0, 0, 0, 0, 0, 0, -4.5, -4.5, -4.5, -6]),
    ("electronic", "Електроніка",  WINAMP_F, [4.875, 3.75, 0, -3.375, -3, 0, 4.875, 6, 6, 5.625]),
    ("hiphop", "Хіп-хоп",        list(OCT_F), [5, 4.5, 3, 1, -0.5, 0, 0.5, 1, 1.5, 2]),
]
# Not offered: kept, so that a set saved as one of them still plays under its name.
KEPT = [
    ("loudness", "Тонкомпенсація", list(OCT_F), [4, 4, 4, 0, 0, 0, 0, 0, 0, 2]),
    ("night", "Ніч",            list(OCT_F), [-6, -6, -6, 0, 0, 0, 0, 0, -3, -3]),
    ("more_bass", "Більше басу",    WINAMP_F, [6, 6, 6, 3.75, 1.125, -2.625, -5.25, -6.375, -6.75, -6.75]),
    ("more_treble", "Більше верхів",  WINAMP_F, [-6, -6, -6, -2.625, 1.875, 6.75, 9.75, 9.75, 9.75, 10.5]),
    ("bass_treble", "Бас і верхи",    WINAMP_F, [4.5, 3.75, 0, -4.5, -3, 1.125, 5.25, 6.75, 7.5, 7.5]),
    ("headphones", "Навушники",      WINAMP_F, [3, 6.75, 3.375, -2.25, -1.5, 1.125, 3, 6, 7.875, 9]),
    ("large_hall", "Великий зал",    WINAMP_F, [6.375, 6.375, 3.75, 3.75, 0, -3, -3, -3, 0, 0]),
    ("live", "Концерт",        WINAMP_F, [-3, 0, 2.625, 3.375, 3.75, 3.75, 2.625, 1.875, 1.875, 1.5]),
    ("party", "Вечірка",        WINAMP_F, [4.5, 4.5, 0, 0, 0, 0, 0, 0, 4.5, 4.5]),
    ("club", "Клуб",           WINAMP_F, [0, 0, 2.25, 3.75, 3.75, 3.75, 2.25, 0, 0, 0]),
    ("reggae", "Реггі",          WINAMP_F, [0, 0, -0.375, -3.75, 0, 4.125, 4.125, 0, 0, 0]),
    ("ska", "Ска",            WINAMP_F, [-1.5, -3, -2.625, -0.375, 2.625, 3.75, 5.625, 6, 6.75, 6]),
    ("soft", "М'який звук",    WINAMP_F, [3, 1.125, -0.75, -1.5, -0.75, 2.625, 5.25, 6, 6.75, 7.5]),
    ("soft_rock", "Софт-рок",       WINAMP_F, [2.625, 2.625, 1.5, -0.375, -2.625, -3.375, -2.25, -0.375, 1.875, 5.625]),
    ("dance", "Танцювальна",    WINAMP_F, [6, 4.5, 1.5, 0, 0, -3.75, -4.5, -4.5, 0, 0]),
]
PRESETS = SOUNDS + KEPT

GRID = 20.0 * 10.0 ** (3.0 * np.arange(401) / 400.0)       # 20 Hz .. 20 kHz
EVAL = GRID[(GRID >= 25.0) & (GRID <= 18000.0)]            # where it is judged


def target(freqs, gains, hz):
    """The preset's curve: straight lines on a log axis, flat beyond its ends."""
    return np.interp(np.log(hz), np.log(np.asarray(freqs, float)), np.asarray(gains, float))


def peak_db(f0, gain, q, hz):
    """A peaking filter's analogue prototype in dB at `hz`: (s² + s·A/Q + 1)/(s² + s/(A·Q) + 1)."""
    A = 10.0 ** (gain / 40.0)
    s = 1j * np.asarray(hz, float) / f0
    h = (s * s + s * A / q + 1) / (s * s + s / (A * q) + 1)
    return 20.0 * np.log10(np.abs(h))


def shelf_db(kind, f0, gain, q, hz):
    """A shelf's analogue prototype in dB — the cookbook's low and high shelf."""
    A = 10.0 ** (gain / 40.0)
    s = 1j * np.asarray(hz, float) / f0
    r = math.sqrt(A) / q
    if kind == "low":
        h = A * (s * s + s * r + A) / (A * s * s + s * r + 1)
    else:
        h = A * (A * s * s + s * r + 1) / (s * s + s * r + A)
    return 20.0 * np.log10(np.abs(h))


def heard(gains, q, hz, shelves=True):
    tot = np.zeros_like(hz)
    for i, (f, g) in enumerate(zip(OCT_F, gains)):
        if abs(g) < 0.05:
            continue
        if shelves and i == 0:
            tot += shelf_db("low", LOW_SHELF_F, g, SHELF_Q, hz)
        elif shelves and i == len(OCT_F) - 1:
            tot += shelf_db("high", HIGH_SHELF_F, g, SHELF_Q, hz)
        else:
            tot += peak_db(f, g, q, hz)
    return tot


def fit(freqs, gains, q, mu, iters=12, shelves=True):
    """Gauss-Newton on the heard curve, with mu * |second difference of the gains|^2."""
    t = target(freqs, gains, EVAL)
    g = target(freqs, gains, OCT_F).copy()
    d2 = np.zeros((8, 10))
    for r in range(8):
        d2[r, r:r + 3] = (1, -2, 1)
    s = d2.T @ d2 * mu * len(EVAL)
    for _ in range(iters):
        base = heard(g, q, EVAL, shelves)
        r = base - t
        jac = np.empty((len(EVAL), 10))
        for i in range(10):
            gg = g.copy()
            gg[i] += 0.05
            jac[:, i] = (heard(gg, q, EVAL, shelves) - base) / 0.05
        step = np.linalg.solve(jac.T @ jac + s + 1e-6 * np.eye(10), -(jac.T @ r) - s @ g)
        g = np.clip(g + step, -12.0, 12.0)
    return np.round(g, 1)


def turning(y):
    d = np.diff(y)
    return int(np.sum(d[:-1] * d[1:] < -1e-9))


def zigzag(g):
    d = np.diff(g)
    return int(np.sum(d[:-1] * d[1:] < -0.01))


def judge(q, mu, shelves):
    dev = bumps = zz = 0
    for _, _, f, g in PRESETS:
        fitted = fit(f, g, q, mu, shelves=shelves)
        y, t = heard(fitted, q, EVAL, shelves), target(f, g, EVAL)
        dev = max(dev, float(np.max(np.abs(y - t))))
        bumps = max(bumps, turning(y) - turning(t))
        zz = max(zz, zigzag(fitted) - zigzag(target(f, g, OCT_F)))
    return dev, bumps, zz


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eval", action="store_true", help="compare Q and the smoothness penalty")
    a = ap.parse_args()
    if a.eval:
        naive = max(float(np.max(np.abs(heard(target(f, g, OCT_F), 1.41, EVAL, False)
                                        - target(f, g, EVAL)))) for _, _, f, g in PRESETS)
        print(f"first cut (Q 1.41, gain = curve at the centre): worst {naive:.2f} dB off the curve")
        for shelves in (False, True):
            for q in (0.7, 1.0, 1.41):
                for mu in (0.0, 0.002, 0.01):
                    dev, bumps, zz = judge(q, mu, shelves)
                    print(f"{'shelves' if shelves else 'peaks  '} Q {q:4.2f}  mu {mu:5.3f}:  worst "
                          f"{dev:4.2f} dB off, extra bumps {bumps:2d}, extra zig-zags {zz}")
        return 0
    print(f"    // Generated by tools/eq/fit_presets.py (Q {Q}, MU {MU}, shelves at "
          f"{LOW_SHELF_F:g} Hz / {HIGH_SHELF_F:g} Hz, against the analogue prototypes). "
          f"Do not edit by hand.")
    for part, rows in (("sounds", SOUNDS), ("kept", KEPT)):
        if part == "kept":
            print("    // Not offered: kept so a set saved as one of them still plays under its name.")
            print("    // EQ_PRESET_SHOWN (eq_presets.h) is where this part starts.")
        for pid, name, f, g in rows:
            fitted = fit(f, g, Q, MU)
            dev = float(np.max(np.abs(heard(fitted, Q, EVAL) - target(f, g, EVAL))))
            vals = ", ".join(f"{x:.1f}f" for x in fitted)
            print(f'    {{ "{pid}", "{name}", {{ {vals} }} }},   // {dev:.1f} dB off at worst')
    return 0


if __name__ == "__main__":
    sys.exit(main())
