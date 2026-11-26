# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""The observer (SIMULATED eye; the standard motion-clarity model of sample-and-hold displays).

Tracking eye: the eye follows the TRUE motion smoothly at v output px per second. Every displayed frame stays on the
panel for its hold; the panel's pixels are boxes (piecewise constant in space). The retinal image is the displayed light
(nits, linear: the eye integrates light) integrated along the eye's trajectory: R(u) = mean over time of
F_shown(t)(u + v (t - L)), L the constant lag that centres it. Time is sampled 16 times per refresh, space 8 times per output
pixel. Physics of the model: a perfect frame held h seconds is smeared into a box of v h pixels (Blur Busters' law).
Fixating eye (HUD): the eye stays on the crosshair; it sees the per-refresh sequence of the crosshair's window.
"""
from __future__ import annotations

import numpy as np

from . import display

FINE = 8          # retinal sub-samples per output pixel
SUB = 16          # time samples per refresh


def eye_offsets(starts, content_s, v_out):
    """Per time sample: (frame index, eye shift in output px relative to the frame's content: e = v (t - L) - v c_i)."""
    frames = display.refresh_frames(starts)
    n = len(frames)
    t = (np.arange(n * SUB) + 0.5) / SUB * display.PERIOD_S
    fi = np.repeat(frames, SUB)
    rel = t - content_s[fi]            # display time minus content time
    lag = rel.mean()
    return fi, v_out * (rel - lag)


def retina_1d(frames_nits, starts, content_s, v_out, shifts=None):
    """Tracked retinal image (fine grid, FINE samples per output px; periodic) of a pan at v_out px/s. frames_nits: list of
    1-D output rows in nits (the displayed frames, each showing the world displaced by its content position v * c_i).
    `shifts`: optional per-frame extra displacement (generator position error, px) - not used for the image itself."""
    fi, e = eye_offsets(starts, content_s, v_out)
    fine = [np.repeat(f, FINE) for f in frames_nits]
    acc = np.zeros_like(fine[0])
    # content of frame i sits at +v c_i; aligning on the content: sample F_i(u + v c_i + e)  ->  roll by -(v c_i + e)
    base = v_out * np.asarray(content_s)
    for k in range(len(fi)):
        i = fi[k]
        acc += np.roll(fine[i], -int(round((e[k] + base[i]) * FINE)))
    return acc / len(fi)


def judder(starts, content_s, v_out, gen_shift_px=None):
    """Position error of each displayed frame for the tracking eye (output px): the hold-weighted mean over its time on
    screen of (picture position - eye position). Ideal holds + a perfect generator give the same value for every frame
    (judder 0). Returns (rms, worst, per-frame errors, sawtooth step per frame = v x hold)."""
    fi, e = eye_offsets(starts, content_s, v_out)
    nfr = len(starts) - 1
    err = np.zeros(nfr); wt = np.zeros(nfr)
    np.add.at(err, fi, -e); np.add.at(wt, fi, 1.0)
    shown = wt > 0
    err = err[shown] / wt[shown]
    if gen_shift_px is not None:
        err = err + np.asarray(gen_shift_px)[: nfr][shown]
    w = wt[shown] / wt[shown].sum()
    mean = float((err * w).sum())
    dev = err - mean
    step = v_out * np.diff(starts)[shown] * display.PERIOD_S
    return float(np.sqrt((dev ** 2 * w).sum())), float(np.abs(dev).max()), dev, step


def lsf_width(profile_fine, edge_at_fine, half_window_fine):
    """Equivalent box width (output px) of the line-spread function of a rising edge in a fine retinal profile:
    sqrt(12) x the standard deviation of d(profile)/du in the window around the edge. A box of width w gives exactly w."""
    n = len(profile_fine)
    idx = (np.arange(-half_window_fine, half_window_fine) + edge_at_fine) % n
    seg = profile_fine[idx]
    lsf = np.diff(seg)
    u = np.arange(len(lsf)) + 0.5
    s = lsf.sum()
    mu = (lsf * u).sum() / s
    var = (lsf * (u - mu) ** 2).sum() / s
    return float(np.sqrt(12.0 * max(var, 0.0)) / FINE)


def edge_rise(profile_fine, edge_at_fine, half_window_fine):
    """10-90 % rise distance of a rising edge / 0.8 (output px): equals w for a box blur of width w, and, unlike the
    second-moment width, ignores faint far copies (a 30 % double image far away changes it little)."""
    n = len(profile_fine)
    idx = (np.arange(-half_window_fine, half_window_fine) + edge_at_fine) % n
    seg = profile_fine[idx]
    lo, hi = seg[:half_window_fine // 4].mean(), seg[-half_window_fine // 4:].mean()
    z = (seg - lo) / (hi - lo)
    a = np.nonzero(z >= 0.1)[0][0]
    b = np.nonzero(z >= 0.9)[0][0]
    xa = a - 1 + (0.1 - z[a - 1]) / (z[a] - z[a - 1]) if a > 0 else 0.0
    xb = b - 1 + (0.9 - z[b - 1]) / (z[b] - z[b - 1]) if b > 0 else 0.0
    return float((xb - xa) / 0.8 / FINE)


def modulation(profile, cycles):
    """Amplitude (half peak-to-peak of the sinusoid) at `cycles` per period of a periodic profile."""
    F = np.fft.rfft(profile - profile.mean())
    return float(2.0 * np.abs(F[cycles]) / len(profile))
