#!/usr/bin/env python3
"""Aligning a subwoofer with the speakers from one
microphone at the listening place — a model of the method, not a measurement.

The questions a prototype has to answer, and how this answers them:
  1. a timing reference between the speakers' sound and the sub's in ONE recording, so that the
     phone's latency and the two clocks cancel (as REW's acoustic timing reference does);
  2. a test with a known offset — accuracy, not only repeatability;
  3. which path to delay, the sub or the speakers;
  4. delay and polarity chosen by how the two SUM across the crossover, not by lining up the peaks
     of two impulses of different bandwidth;
  5. the summed sound measured again after the correction.

Everything is simulated in the frequency domain at 48 kHz: the speakers behind a Linkwitz-Riley
24 dB/oct high-pass, the sub behind the matching low-pass, each with its box, its distance, its
reflections and a room mode; the sub may carry a low-pass of its own (a plate amplifier left on)
and may be wired inverted. The "phone" records with an unknown latency, a clock off by up to
±80 ppm, a microphone that rolls off below 80 Hz, and noise. The playback is one file: a short
high chirp on the speakers (the timing reference), a sweep on the sub, a sweep on the speakers,
and a second reference to measure the clock drift.

Ground truth is the alignment the same criterion picks from the true responses; the error is
what the measured responses pick instead. Needs numpy only:

    python3 tools/sub_sync_sim.py            # the report: 20 rooms a placement, two minutes or so
    python3 tools/sub_sync_sim.py --trials 5 # quicker
    python3 tools/sub_sync_sim.py --snr 20   # a noisier phone
"""
import argparse
import math

import numpy as np

FS = 48000
C = 343.0                      # m/s
N = 1 << 19                    # 10.9 s of recording, FFT length
F = np.fft.rfftfreq(N, 1 / FS)
FC = 80.0                      # the crossover
BAND = (FC / 2, FC * 2)        # where the two sum, and where the alignment is judged

# ── filters: RBJ biquads, evaluated on the FFT grid ──

def _biquad(b, a):
    z1 = np.exp(-2j * np.pi * F / FS)
    z2 = z1 * z1
    return (b[0] + b[1] * z1 + b[2] * z2) / (a[0] + a[1] * z1 + a[2] * z2)


def lowpass(f0, q):
    w = 2 * np.pi * f0 / FS
    al = math.sin(w) / (2 * q)
    cw = math.cos(w)
    return _biquad([(1 - cw) / 2, 1 - cw, (1 - cw) / 2], [1 + al, -2 * cw, 1 - al])


def highpass(f0, q):
    w = 2 * np.pi * f0 / FS
    al = math.sin(w) / (2 * q)
    cw = math.cos(w)
    return _biquad([(1 + cw) / 2, -(1 + cw), (1 + cw) / 2], [1 + al, -2 * cw, 1 - al])


def peak(f0, q, db):
    a = 10 ** (db / 40)
    w = 2 * np.pi * f0 / FS
    al = math.sin(w) / (2 * q)
    cw = math.cos(w)
    return _biquad([1 + al * a, -2 * cw, 1 - al * a], [1 + al / a, -2 * cw, 1 - al / a])


BW = 1 / math.sqrt(2)
LR24_HP = highpass(FC, BW) ** 2
LR24_LP = lowpass(FC, BW) ** 2


def delay(t):
    return np.exp(-2j * np.pi * F * t)


def room(reflections):
    h = np.ones_like(F, dtype=complex)
    for t, db in reflections:
        h = h + 10 ** (db / 20) * delay(t)
    return h

# ── the signals the panel plays ──

def log_sweep(f1, f2, secs):
    """Farina's exponential sweep, faded in and out over 5 ms."""
    n = int(secs * FS)
    t = np.arange(n) / FS
    k = math.log(f2 / f1)
    x = np.sin(2 * np.pi * f1 * secs / k * (np.exp(t / secs * k) - 1))
    fade = int(0.005 * FS)
    ramp = 0.5 - 0.5 * np.cos(np.pi * np.arange(fade) / fade)
    x[:fade] *= ramp
    x[-fade:] *= ramp[::-1]
    return x


REF = log_sweep(2000, 16000, 0.05)          # the timing reference: short and high
SUB_SWEEP = log_sweep(15, 300, 2.0)
MAIN_SWEEP = log_sweep(30, 1200, 2.0)
# where each starts in the file the panel plays, s
T_REF1, T_SUB, T_MAIN, T_REF2 = 0.2, 0.5, 3.0, 5.3


def spectrum(x, start):
    """The FFT of `x` placed at `start` seconds (fractional starts by phase)."""
    buf = np.zeros(N)
    buf[: len(x)] = x
    return np.fft.rfft(buf) * delay(start)

# ── a room, a sub and a phone ──

def system(rng, sub_offset_m):
    """True responses at the listening place: (speakers, sub), each the path from its output."""
    d_main = 2.5
    d_sub = d_main + sub_offset_m
    mode = peak(rng.uniform(38, 55), 4.0, rng.uniform(4, 9))         # a room mode, at both
    main = LR24_HP * highpass(45, 0.7) ** 2 * delay(d_main / C) * mode * room(
        [(rng.uniform(2, 5) / 1000, -6), (rng.uniform(6, 10) / 1000, -9)])
    sub = LR24_LP * highpass(25, 0.7) * delay(d_sub / C) * mode * room(
        [(rng.uniform(1, 4) / 1000, -4), (rng.uniform(5, 9) / 1000, -7), (rng.uniform(10, 16) / 1000, -10)])
    if rng.random() < 0.5:                         # a plate amplifier's own low-pass, left on
        sub = sub * lowpass(120, BW) ** 2
    if rng.random() < 0.5:                         # wired the other way round
        sub = -sub
    return main, sub


MIC = highpass(80, 0.7) ** 2                       # a phone microphone's bass roll-off


def plan(main, sub):
    """The measuring file: (signal, start in the file, the path it plays through)."""
    return [(REF, T_REF1, main), (SUB_SWEEP, T_SUB, sub), (MAIN_SWEEP, T_MAIN, main), (REF, T_REF2, main)]


def record(rng, plays, latency=None, drift=None, snr_db=35.0):
    """What the phone hears: `plays` through their paths, after `latency`, on a clock off by `drift`."""
    latency = rng.uniform(0.02, 0.25) if latency is None else latency
    drift = rng.uniform(-80e-6, 80e-6) if drift is None else drift
    Y = np.zeros_like(F, dtype=complex)
    for x, t0, path in plays:
        Y += spectrum(x, latency + t0 * (1 + drift)) * path * MIC
    y = np.fft.irfft(Y, N)
    noise = rng.standard_normal(N)
    noise *= np.sqrt(np.mean(y[y != 0] ** 2)) / np.sqrt(np.mean(noise ** 2)) * 10 ** (-snr_db / 20)
    return y + noise


def arrival(y, x, around=None):
    """Where `x` arrives in `y`, in seconds: the matched filter's peak, to a fraction of a sample."""
    X = np.fft.rfft(np.pad(x, (0, N - len(x))))
    r = np.fft.irfft(np.fft.rfft(y) * np.conj(X), N)
    r = np.abs(r)
    if around is not None:
        lo, hi = (max(0, int((around[0]) * FS)), min(N, int(around[1] * FS)))
        i = lo + int(np.argmax(r[lo:hi]))
    else:
        i = int(np.argmax(r))
    a, b, c = r[i - 1], r[i], r[(i + 1) % N]
    frac = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    return (i + frac) / FS


def measure(y, x, start, tail=0.6):
    """The response of the path that played `x` from `start` (in the recording's time), by division:
    only the recording's stretch where `x` and its echoes are, so another sweep is not divided too."""
    lo = max(0, int((start - 0.05) * FS))
    hi = min(N, int((start + len(x) / FS + tail) * FS))
    w = np.zeros(N)
    w[lo:hi] = 1.0
    fade = int(0.01 * FS)
    w[lo:lo + fade] = np.linspace(0, 1, fade)
    w[hi - fade:hi] = np.linspace(1, 0, fade)
    X = spectrum(x, start)
    Y = np.fft.rfft(y * w)
    mag = np.abs(X)
    keep = mag > 1e-3 * mag.max()                  # only where the sweep put energy
    H = np.zeros_like(Y)
    H[keep] = Y[keep] / X[keep]
    return H

# ── the alignment: by the sum, or by the peaks ──

BAND_I = np.where((F >= BAND[0]) & (F <= BAND[1]))[0]
# judge on a log-spaced set of bins, so the octave above the crossover does not outweigh the one below
LOG_I = np.unique(np.round(np.geomspace(BAND_I[0], BAND_I[-1], 400)).astype(int))
TAUS = np.arange(-15e-3, 15e-3 + 1e-9, 0.05e-3)
WIDE = np.arange(-400e-3, 400e-3 + 1e-9, 0.1e-3)   # where a lost time base can put the answer


def sum_loss_db(main, sub, tau, pol, worst=False):
    """How far the sum falls short of the two magnitudes added, in dB over the band: on average, or
    at its worst point (a narrow notch the average hides)."""
    m, s, f = main[LOG_I], sub[LOG_I], F[LOG_I]
    tot = np.abs(m + pol * s * np.exp(-2j * np.pi * f * tau))
    best = np.abs(m) + np.abs(s)
    loss = 20 * np.log10(best / np.maximum(tot, 1e-12))
    return float(np.max(loss) if worst else np.mean(loss))


def align_by_sum(main, sub, taus=TAUS):
    """The delay (>0: the sub's, <0: the speakers') and polarity that make the two sum best."""
    m, s, f = main[LOG_I], sub[LOG_I], F[LOG_I]
    best = (1e9, 0.0, 1)
    rot = np.exp(-2j * np.pi * np.outer(taus, f))
    for pol in (1, -1):
        tot = np.abs(m[None, :] + pol * s[None, :] * rot)
        loss = np.mean(20 * np.log10((np.abs(m) + np.abs(s))[None, :] / np.maximum(tot, 1e-12)), axis=1)
        i = int(np.argmin(loss))
        if loss[i] < best[0]:
            best = (float(loss[i]), float(taus[i]), pol)
    return best[1], best[2]


def align_by_peaks(main, sub):
    """Line up the envelope peaks of the two band-limited impulses; polarity from the peaks' signs."""
    w = np.zeros_like(F)
    w[BAND_I] = np.hanning(len(BAND_I) + 2)[1:-1]
    # the time base is the speakers' arrival: look from 20 ms before it to 100 ms after
    span = np.r_[np.arange(N - int(0.02 * FS), N), np.arange(0, int(0.1 * FS))]
    hm = np.fft.irfft(main * w, N)[span]
    hs = np.fft.irfft(sub * w, N)[span]
    im, is_ = int(np.argmax(np.abs(hm))), int(np.argmax(np.abs(hs)))
    tau = (im - is_) / FS                          # delay the sub by this to put its peak on the speakers'
    pol = 1 if np.sign(hm[im]) == np.sign(hs[is_]) else -1
    return tau, pol

# ── one trial, every method ──

def trial(rng, offset_m, snr_db=35.0):
    main, sub = system(rng, offset_m)
    tau0, pol0 = align_by_sum(main, sub)          # what the truth says
    wfloor = sum_loss_db(main, sub, tau0, pol0, worst=True)

    # 1. one recording with the reference: the latency and the clocks cancel
    y = record(rng, plan(main, sub), snr_db=snr_db)
    r1 = arrival(y, REF, (T_REF1 - 0.01, T_REF1 + 0.4))
    r2 = arrival(y, REF, (T_REF2 - 0.01, T_REF2 + 0.4))
    drift = (r2 - r1) / (T_REF2 - T_REF1) - 1
    origin = r1 - T_REF1 * (1 + drift)            # the file's start as the speakers' sound reached the phone
    m_meas = measure(y, MAIN_SWEEP, origin + T_MAIN * (1 + drift))
    s_meas = measure(y, SUB_SWEEP, origin + T_SUB * (1 + drift))
    tau, pol = align_by_sum(m_meas, s_meas)
    ref = (tau - tau0, pol == pol0, sum_loss_db(main, sub, tau, pol, worst=True) - wfloor, drift)
    # the same with one reference: the clock taken as right
    o1 = r1 - T_REF1
    t1, p1 = align_by_sum(measure(y, MAIN_SWEEP, o1 + T_MAIN), measure(y, SUB_SWEEP, o1 + T_SUB))
    oneref = (t1 - tau0, p1 == pol0)

    # 4. the same recording, aligned by the impulses' peaks instead
    tp, pp = align_by_peaks(m_meas, s_meas)
    peaks = (tp - tau0, pp == pol0, sum_loss_db(main, sub, tp, pp, worst=True) - wfloor)

    # 1, without: the sub and the speakers in two recordings, each with its own latency
    ys = record(rng, [(SUB_SWEEP, T_SUB, sub)], snr_db=snr_db)
    ym = record(rng, [(MAIN_SWEEP, T_MAIN, main)], snr_db=snr_db)
    tn, pn = align_by_sum(measure(ym, MAIN_SWEEP, T_MAIN), measure(ys, SUB_SWEEP, T_SUB), WIDE)
    noref = (tn - tau0, pn == pol0, sum_loss_db(main, sub, tn, pn, worst=True) - wfloor)

    # 5. after the correction: both outputs play one sweep together; its magnitude, measured, against
    # what the two separate measurements predicted (the same phone, so its microphone cancels)
    dm, ds = (0.0, tau) if tau >= 0 else (-tau, 0.0)
    main_c = main * np.exp(-2j * np.pi * F * dm)
    sub_c = sub * pol * np.exp(-2j * np.pi * F * ds)
    y2 = record(rng, [(REF, T_REF1, main_c), (MAIN_SWEEP, T_MAIN, main_c), (MAIN_SWEEP, T_MAIN, sub_c)],
                snr_db=snr_db)
    o2 = arrival(y2, REF, (T_REF1 - 0.01, T_REF1 + 0.4)) - T_REF1
    both = np.abs(measure(y2, MAIN_SWEEP, o2 + T_MAIN)[LOG_I])
    pred = np.abs((m_meas * np.exp(-2j * np.pi * F * dm) + s_meas * pol * np.exp(-2j * np.pi * F * ds))[LOG_I])
    dev = 20 * np.log10(both / np.maximum(pred, 1e-12))
    verify = float(np.max(np.abs(dev - np.median(dev))))
    before = sum_loss_db(main, sub, 0.0, 1, worst=True)   # nothing done: as wired, no delay
    # no measurement: the delay from the distances alone, the polarity found by ear (taken as right)
    td = -offset_m / C
    dist = (td - tau0, sum_loss_db(main, sub, td, pol0, worst=True) - wfloor)

    return {"tau0": tau0, "pol0": pol0, "ref": ref, "peaks": peaks, "noref": noref, "verify": verify,
            "before": before - wfloor, "oneref": oneref, "dist": dist}


def tolerance():
    """How much a delay error costs, at this crossover: the worst dip it opens in the band."""
    rows = []
    main = LR24_HP * delay(2.5 / C)
    sub = LR24_LP * delay(2.5 / C)
    for err_ms in (0.25, 0.5, 1.0, 2.0, 4.0):
        s = sub * np.exp(-2j * np.pi * F * err_ms / 1000)
        tot = np.abs(main + s)[LOG_I]
        rows.append((err_ms, float(20 * np.log10(np.min(tot)))))
    return rows


def main_():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--trials", type=int, default=20, help="random rooms per sub placement")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--snr", type=float, default=35.0, help="dB, the phone's recording")
    args = ap.parse_args()
    rng = np.random.default_rng(args.seed)

    print(f"Crossover LR24 at {FC:.0f} Hz; judged over {BAND[0]:.0f}–{BAND[1]:.0f} Hz. "
          f"Phone: latency 20–250 ms, clock ±80 ppm, mic −12 dB/oct below 80 Hz, SNR {args.snr:.0f} dB.")
    print()
    print("Errors are against the alignment the same criterion picks from the true responses; "
          "a dip is the worst point of the 40–160 Hz sum, beyond what the true alignment leaves.")
    print()
    print("| sub vs speakers | true delay (− = the speakers') | as wired: dip | with the reference: "
          "error, polarity, dip | one reference only: error | by impulse peaks: error, polarity, dip "
          "| no reference: error | distances only: error, dip | after, measured vs predicted |")
    print("|---|---|---|---|---|---|---|---|---|")
    for off in (-1.0, 0.0, 1.0, 2.0, 3.5):
        rs = [trial(rng, off, args.snr) for _ in range(args.trials)]
        ms = lambda k, i: np.array([r[k][i] for r in rs]) * 1000
        pc = lambda k: np.mean([r[k][1] for r in rs]) * 100
        db = lambda k: np.array([r[k][2] for r in rs])
        t0 = np.array([r["tau0"] for r in rs]) * 1000
        before = np.array([r["before"] for r in rs])
        ver = np.array([r["verify"] for r in rs])
        print(f"| {off:+.1f} m | {np.median(t0):+.1f} ms ({t0.min():+.1f}…{t0.max():+.1f}) "
              f"| {np.median(before):.1f} dB (to {before.max():.1f}) "
              f"| ≤ {np.max(np.abs(ms('ref', 0))):.2f} ms, {pc('ref'):.0f} %, ≤ {db('ref').max():.2f} dB "
              f"| ≤ {np.max(np.abs(ms('oneref', 0))):.2f} ms "
              f"| {np.median(np.abs(ms('peaks', 0))):.1f} ms (to {np.max(np.abs(ms('peaks', 0))):.1f}), "
              f"{pc('peaks'):.0f} %, {np.median(db('peaks')):.1f} dB (to {db('peaks').max():.1f}) "
              f"| {np.median(np.abs(ms('noref', 0))):.0f} ms "
              f"| {np.median(np.abs(ms('dist', 0))):.1f} ms (to {np.max(np.abs(ms('dist', 0))):.1f}), "
              f"{np.median([r['dist'][1] for r in rs]):.1f} dB (to {max(r['dist'][1] for r in rs):.1f}) "
              f"| ≤ {ver.max():.2f} dB |")
    print()
    print("A delay error's cost at this crossover (deepest point of the sum, speakers and sub ideal):")
    for err, loss in tolerance():
        print(f"  {err:4.2f} ms off -> {loss:+.2f} dB")


if __name__ == "__main__":
    main_()
