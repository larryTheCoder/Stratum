#!/usr/bin/env python3
"""Stratum — compare this build's old_blended_noise to deepslate's, statistically.

Copyright 2026 the Stratum contributors. SPDX-License-Identifier: Apache-2.0

The seeding a modern dimension uses is unknown, so the two fields cannot be
compared point by point: they are different realisations. Everything measured
here is therefore seed-independent — the power spectrum, the distribution
shape, and the ratio of spreads. Those pin the octave schedule, the shape and
the normalisation without needing the seeding at all.

Three cautions, each learned by getting it wrong first:

  * A ratio of spreads means nothing without a seed-to-seed control. A noise
    field's sample variance fluctuates by ~6% from one seed to the next
    because it is spatially correlated, so the effective sample count is the
    number of independent patches, not the number of points. An error bar
    computed as 1/sqrt(2n) is off by an order of magnitude and will report a
    2% difference as 14 sigma.
  * A difference between two spectra is only real if it exceeds the spread
    across seeds of each spectrum separately. Peak-picking on a single line
    finds structure in noise.
  * Spread and range must agree. If sd_ratio and range_ratio differ, the two
    fields are not related by any affine map, and no single constant will
    reconcile them.
"""
import os
import sys

import numpy as np


def spectra(paths):
    """Octave-banded power fractions, one row per input file."""
    rows, bands = [], None
    for path in paths:
        v = np.loadtxt(path).reshape(32, 4096)
        v = v - v.mean(axis=1, keepdims=True)
        power = (np.abs(np.fft.rfft(v * np.hanning(4096), axis=1)) ** 2).mean(axis=0)
        freq = np.fft.rfftfreq(4096, d=1.0)
        wavelength = 1.0 / np.maximum(freq, 1e-12)
        if bands is None:
            bands = [(2 ** k, 2 ** (k + 1)) for k in range(1, 13)]
            bands = [(lo, hi, (freq > 0) & (wavelength >= lo) & (wavelength < hi))
                     for lo, hi in bands]
            bands = [b for b in bands if b[2].any()]
        total = power[1:].sum()
        rows.append(np.array([power[m].sum() / total for _, _, m in bands]))
    return np.array(rows), bands


def compare_spectra(ours, theirs):
    a, bands = spectra(theirs)
    b, _ = spectra(ours)
    print(f"{'wavelength band':>20} {'deepslate':>10} {'ours':>10} {'diff':>10} "
          f"{'seed noise':>11}  verdict")
    for i, (lo, hi, _) in enumerate(bands):
        diff = b[:, i].mean() - a[:, i].mean()
        noise = max(a[:, i].std(), b[:, i].std(), 1e-12)
        verdict = "DIFFERENT" if abs(diff) > 3 * noise else "same"
        print(f"{lo:7d}-{hi:<7d} blk {a[:, i].mean():10.5f} {b[:, i].mean():10.5f} "
              f"{diff:+10.5f} {noise:11.5f}  {verdict} ({abs(diff) / noise:.1f}x)")


def compare_scale(ours, theirs):
    dv = [np.loadtxt(p) for p in theirs]
    ov = [np.loadtxt(p) for p in ours]
    ratio = np.array([o.std() / d.std() for d, o in zip(dv, ov)])
    sem = ratio.std(ddof=1) / np.sqrt(len(ratio)) if len(ratio) > 1 else float("nan")
    print(f"\nseeds {len(ratio)}, {len(dv[0])} samples each, all at y = 0")
    print(f"sd ratio  mean {ratio.mean():.4f}  sd {ratio.std(ddof=1):.4f}  sem {sem:.4f}")
    for candidate in (64.0, 128.0, 256.0, 512.0):
        print(f"    vs {candidate:7.1f}: {abs(ratio.mean() - candidate) / sem:6.2f} sigma")

    d, o = np.concatenate(dv), np.concatenate(ov)
    print(f"\npooled {len(d):,} each side")
    ks = [np.quantile(o, q) / np.quantile(d, q)
          for q in (0.01, 0.05, 0.1, 0.25, 0.75, 0.9, 0.95, 0.99)]
    print(f"quantile-implied scale: median {np.median(ks):.3f}  "
          f"range {min(ks):.3f}..{max(ks):.3f}")
    print(f"range ratio {(o.max() - o.min()) / (d.max() - d.min()):.3f}  "
          f"(must agree with the sd ratio, or no affine map exists)")
    for name, x in (("deepslate", d), ("ours", o)):
        print(f"  {name:>10}: kurtosis {((x - x.mean()) ** 4).mean() / x.var() ** 2:.5f}  "
              f"skew {((x - x.mean()) ** 3).mean() / x.var() ** 1.5:+.5f}")


def _corr_length(a):
    """Lag in blocks at which the autocorrelation along y first falls below a
    half. The smear is a y-structure parameter, so this is the statistic it
    moves; a horizontal slice cannot see it at all."""
    a = a - a.mean(axis=1, keepdims=True)
    n = a.shape[1]
    denominator = (a * a).mean()
    for lag in range(1, n // 2):
        if (a[:, :n - lag] * a[:, lag:]).mean() / denominator < 0.5:
            return lag
    return n // 2


def compare_smear(ours, theirs):
    """Files come in triples per seed: multiplier 0, 1 and 16, in that order.

    Three statistics, all dimensionless, so none of them needs the seeding:
    the correlation length along y, how much the smear raises the spread over
    having none at all, and how much the field moves between a multiplier of 1
    and of 16. The last is the sharpest — vanilla's field is almost unmoved,
    at r = 0.989, and a candidate that scales amplitude with the multiplier
    fails it by a mile.
    """
    def summarise(paths):
        lengths, boosts, sensitivity = [], [], []
        for i in range(0, len(paths), 3):
            off, one, sixteen = (np.loadtxt(p).reshape(64, 512) for p in paths[i:i + 3])
            lengths.append(_corr_length(one))
            boosts.append(one.std() / off.std())
            sensitivity.append(float(np.corrcoef(one.ravel(), sixteen.ravel())[0, 1]))
        return lengths, boosts, sensitivity

    labels = ("corr-length in y (m=1)", "sd boost over m=0", "r(m=1, m=16)")
    a, b = summarise(theirs), summarise(ours)
    print(f"{'':<26} {'deepslate':>20} {'ours':>20}   agreement")
    for label, x, y in zip(labels, a, b):
        mx, my = np.mean(x), np.mean(y)
        ex = np.std(x, ddof=1) / np.sqrt(len(x)) if len(x) > 1 else float("nan")
        ey = np.std(y, ddof=1) / np.sqrt(len(y)) if len(y) > 1 else float("nan")
        sigma = abs(mx - my) / np.hypot(ex, ey) if ex == ex and ey == ey else float("nan")
        print(f"{label:<26} {mx:12.4f} +/- {ex:5.4f} {my:12.4f} +/- {ey:5.4f}   {sigma:5.2f} sigma")


def compare_fingerprint(ours, theirs):
    """Jump sizes at integer y, as multiples of the median sub-block step.

    A quantisation keyed to the block coordinate is discontinuous at integer y
    and smooth between, which no summary statistic can see. Vanilla's field
    jumps by twenty to sixty times its median step at every integer with the
    smear on, and not at all with the multiplier at 0. Candidates that match
    the spread and the correlation length can still fail this outright, which
    is exactly what happened to the first one that looked good.
    """
    def profile(path):
        raw = np.loadtxt(path)
        y, value = raw[:, 0], raw[:, 1]
        step = np.abs(np.diff(value))
        median = np.median(step)
        return [step[np.argmin(np.abs(y[1:] - k))] / median for k in range(1, 9)]

    for label, paths in (("deepslate", theirs), ("ours", ours)):
        for path in paths:
            print(f"{label:>10} {os.path.basename(path):<28} "
                  f"{' '.join(f'{j:5.0f}' for j in profile(path))}")


def compare_planes(ours, theirs):
    def by_height(path):
        raw = np.loadtxt(path)
        return {int(y): raw[raw[:, 0] == y][:, 1] for y in np.unique(raw[:, 0])}
    a, b = by_height(theirs[0]), by_height(ours[0])
    print(f"\n{'y':>6} {'deepslate sd':>14} {'ours sd':>14} {'ratio':>10}")
    for y in sorted(a):
        print(f"{y:6d} {a[y].std():14.6f} {b[y].std():14.4f} {b[y].std() / a[y].std():10.3f}")


if __name__ == "__main__":
    if len(sys.argv) < 4 or "--" not in sys.argv:
        sys.exit("usage: blended-compare.py {spectra|scale|planes|smear|fingerprint} ours... -- theirs...")
    split = sys.argv.index("--")
    mode, ours, theirs = sys.argv[1], sys.argv[2:split], sys.argv[split + 1:]
    {"spectra": compare_spectra, "scale": compare_scale, "planes": compare_planes,
     "smear": compare_smear, "fingerprint": compare_fingerprint}[mode](ours, theirs)
