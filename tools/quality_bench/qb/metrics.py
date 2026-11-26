# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Metric functions (pure numpy/scipy; unit-tested in tests/test_metrics.py). Definitions in docs/drafts/QUALITY_BENCH.md."""
from __future__ import annotations

import numpy as np
from scipy import ndimage

from . import display

DUPLICATE_SHARE = 0.10   # PushFrameProbe.h kDuplicateShare: a generated frame within 10 % of d(A, B) of a neighbour


def mean_abs(a, b):
    return float(np.mean(np.abs(np.asarray(a, np.float64) - np.asarray(b, np.float64))))


def unique_frames(out_codes, info, starts):
    """Distinct pictures shown per second (PushFrameProbe.h's near-duplicate rule on the displayed frames).
    A frame with hold 0 is never shown. A generated frame is a near-duplicate when its distance to the previous or the next
    displayed frame is below 10 % of d(A, B) of its pair; real frames always count; a still pair (d(A, B) = 0) counts its
    real frame only. Returns (unique per second, shown per second, near-duplicates)."""
    h = display.holds(starts)
    n = len(h)
    secs = float(starts[n] - starts[0]) * display.PERIOD_S
    shown = [i for i in range(n) if h[i] > 0]
    reals = [i for i in range(len(info)) if not info[i][0]]
    dups = 0
    for i in shown:
        if not info[i][0]:
            continue
        a = max(r for r in reals if r < i)
        b = min(r for r in reals if r > i)
        D = mean_abs(out_codes[a], out_codes[b])
        if D <= 0:
            dups += 1
            continue
        prev_i = max(j for j in shown if j < i) if any(j < i for j in shown) else a
        nxt = [j for j in shown if j > i]
        nxt_i = nxt[0] if nxt else b
        if min(mean_abs(out_codes[i], out_codes[prev_i]), mean_abs(out_codes[i], out_codes[nxt_i])) < DUPLICATE_SHARE * D:
            dups += 1
    return (len(shown) - dups) / secs, len(shown) / secs, dups


def shift_1d(row, ref, upsample=16):
    """Displacement s (px) such that row(x) ~ ref(x - s), periodic, sub-pixel (FFT cross-correlation, upsampled)."""
    a = np.asarray(row, np.float64) - np.mean(row)
    b = np.asarray(ref, np.float64) - np.mean(ref)
    n = len(a)
    X = np.fft.fft(a) * np.conj(np.fft.fft(b))
    m = n * upsample
    pad = np.zeros(m, complex)
    half = n // 2
    pad[:half] = X[:half]
    pad[-(n - half):] = X[half:]
    cc = np.fft.ifft(pad).real
    k = int(np.argmax(cc))
    y0, y1, y2 = cc[k - 1], cc[k], cc[(k + 1) % m]
    den = y0 - 2 * y1 + y2
    frac = 0.5 * (y0 - y2) / den if den != 0 else 0.0
    s = (k + frac) / upsample
    return s - n if s > n / 2 else s


def fwhm(profile):
    """Full width at half maximum (samples) of a single-peaked non-negative profile; 0 when flat."""
    p = np.asarray(profile, np.float64)
    k = int(np.argmax(p))
    peak = p[k]
    if peak <= 0:
        return 0.0
    half = peak / 2.0
    left = k
    while left > 0 and p[left - 1] > half:
        left -= 1
    right = k
    while right < len(p) - 1 and p[right + 1] > half:
        right += 1
    lx = left - (p[left] - half) / (p[left] - p[left - 1]) if left > 0 else 0.0
    rx = right + (p[right] - half) / (p[right] - p[right + 1]) if right < len(p) - 1 else float(len(p) - 1)
    return float(max(rx - lx, 0.0))


def tone_at(series, rate_hz, freq_hz):
    """Amplitude (half peak-to-peak) of the component at freq_hz of a series sampled at rate_hz, as a share of its mean
    (a sinusoidal 30 Hz flicker of +-10 % reads 0.10). Uses a least-squares sine/cosine fit (any length)."""
    s = np.asarray(series, np.float64)
    t = np.arange(len(s)) / rate_hz
    A = np.stack([np.ones_like(t), np.cos(2 * np.pi * freq_hz * t), np.sin(2 * np.pi * freq_hz * t)], 1)
    c, *_ = np.linalg.lstsq(A, s, rcond=None)
    return float(np.hypot(c[1], c[2]) / abs(c[0])) if c[0] != 0 else 0.0


def ghosts(added_codes, footprint, window, threshold):
    """Displaced copies in one frame: `added_codes` = |frame with the overlay - same generator without it| (codes);
    pixels inside `window` and outside `footprint` above `threshold`. Returns (pixels, copies = connected groups of >= 2
    px, energy = sum of the excess over the threshold)."""
    m = window & ~footprint & (added_codes > threshold)
    lab, n = ndimage.label(m, structure=np.ones((3, 3)))
    if n:
        sizes = ndimage.sum(m, lab, index=np.arange(1, n + 1))
        copies = int(np.sum(sizes >= 2))
    else:
        copies = 0
    return int(m.sum()), copies, float(np.sum((added_codes - threshold)[m]))


def per_refresh(values, starts):
    """Per-frame values expanded to one value per refresh (what a fixating eye sees over time)."""
    return np.repeat(np.asarray(values, np.float64)[: len(starts) - 1], display.holds(starts))


def time_weighted_mean(values, starts):
    r = per_refresh(values, starts)
    return float(r.mean()) if len(r) else 0.0


def line_stats(rows_nits, bg):
    """Peak above background and energy (sum above background, output px x nits) of a thin feature per output row/frame."""
    rows = np.atleast_2d(rows_nits) - bg
    return rows.max(axis=1), rows.clip(min=0).sum(axis=1)


def pulse(values):
    """(max - min) / mean: the temporal 'pulse' of a quantity across phases/frames (S12_PACE_REVIEW2 tables)."""
    v = np.asarray(values, np.float64)
    m = v.mean()
    return float((v.max() - v.min()) / m) if m != 0 else 0.0
