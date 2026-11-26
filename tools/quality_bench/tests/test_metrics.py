# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Unit tests of the bench's metric functions (pure; known answers)."""
import math

import numpy as np
import pytest

from qb import display, metrics as mt, observer as ob, tf


def test_pq_round_trip_and_known_points():
    nits = np.array([0.0, 0.1, 1.0, 100.0, 203.0, 1000.0, 10000.0])
    assert np.allclose(tf.pq_decode(tf.pq_encode(nits)), nits, rtol=1e-9, atol=1e-9)
    assert tf.pq_encode(10000.0) == pytest.approx(1.0)
    assert tf.pq_encode(100.0) == pytest.approx(0.5081, abs=1e-3)   # ST 2084: 100 nits ~ code 520/1023


def test_delta_e_itp_zero_for_equal_and_positive_otherwise():
    a = np.array([100.0, 50.0, 20.0])
    assert tf.delta_e_itp(a, a) == pytest.approx(0.0)
    assert tf.delta_e_itp(a, a * 1.1) > 1.0


def test_fwhm_of_a_box_and_a_triangle():
    assert mt.fwhm([0, 0, 1, 1, 1, 0, 0]) == pytest.approx(3.0)   # half-maximum crossings at 1.5 and 4.5
    assert mt.fwhm([0, 1, 2, 1, 0]) == pytest.approx(2.0)
    assert mt.fwhm(np.zeros(5)) == 0.0


def test_tone_at_recovers_a_30hz_modulation():
    t = np.arange(360) / display.REFRESH_HZ
    s = 10 * (1 + 0.2 * np.sin(2 * math.pi * 30 * t))
    assert mt.tone_at(s, display.REFRESH_HZ, 30.0) == pytest.approx(0.2, abs=1e-6)
    assert mt.tone_at(np.full(100, 3.0), display.REFRESH_HZ, 30.0) == pytest.approx(0.0, abs=1e-12)


def test_shift_1d_subpixel():
    x = np.arange(512)
    ref = np.exp(-((x - 200) / 6.0) ** 2) + 0.5 * np.exp(-((x - 300) / 3.0) ** 2)
    row = np.exp(-((x - 203.25) / 6.0) ** 2) + 0.5 * np.exp(-((x - 303.25) / 3.0) ** 2)
    assert mt.shift_1d(row, ref) == pytest.approx(3.25, abs=0.05)


def test_ghosts_counts_copies_outside_the_footprint():
    added = np.zeros((40, 40))
    added[10:12, 10:12] = 0.2      # copy 1
    added[30, 30:33] = 0.2         # copy 2
    added[20, 20] = 0.2            # 1-pixel speck: not a copy
    added[5, 5] = 0.5              # inside the footprint: the crosshair itself
    foot = np.zeros_like(added, bool); foot[4:7, 4:7] = True
    px, copies, energy = mt.ghosts(added, foot, np.ones_like(foot), 0.1)
    assert (px, copies) == (8, 2)
    assert energy == pytest.approx(8 * 0.1)


def test_pulse():
    assert mt.pulse([1, 1, 1]) == 0.0
    assert mt.pulse([0.5, 1.5]) == pytest.approx(1.0)


def test_unique_frames_counts_near_duplicates():
    # 3X groups: A, I1, I2, B ... with I1 a copy of A in every group -> one near-duplicate per group
    frames, info = [], []
    for j in range(5):
        A = np.full((4, 4), j * 0.1)
        frames += [A, A.copy(), A + 0.0667]
        info += [(False, 0.0), (True, 1 / 3), (True, 2 / 3)]
    frames.append(np.full((4, 4), 0.5)); info.append((False, 0.0))
    starts, _ = display.schedule(3, len(frames) + 1, "ideal")
    uniq, shown, dups = mt.unique_frames(frames, info, starts)
    assert dups == 5
    assert uniq == pytest.approx(shown * (len(frames) - 5) / len(frames), rel=1e-9)


def test_retina_box_blur_equals_speed_times_hold():
    # a sharp edge panning at v px/s, held 2 refreshes per frame: the tracked edge is a box of v * 2 periods
    n = 400
    v = 960.0
    starts = np.arange(91) * 2
    cs = np.arange(91) / (display.REFRESH_HZ / 2)
    rows = []
    for c in cs:
        x = (np.arange(n) - v * c) % n
        rows.append(np.where(x < n / 2, 100.0, 0.0))
    R = ob.retina_1d(rows, starts, cs, v)
    d = np.diff(np.concatenate([R, R[:1]]))
    e = int(np.argmax(-d)) if -d.max() > d.max() else int(np.argmax(d))
    w = ob.lsf_width(R if d[e] > 0 else -R, e, 60 * ob.FINE)
    assert w == pytest.approx(v * 2 / display.REFRESH_HZ, rel=0.03)
    r, worst, _, _ = ob.judder(starts, cs, v)
    assert r == pytest.approx(0.0, abs=1e-9) and worst == pytest.approx(0.0, abs=1e-9)


def test_judder_of_one_early_frame():
    # 3X, holds 2,2,1,3,2,...: the short frame leaves early and the next starts one refresh early; averaged over their
    # holds each sits half a refresh of motion (v P / 2) off the eye, in opposite directions
    holds = [2] * 10 + [1, 3] + [2] * 10
    starts = np.concatenate([[0], np.cumsum(holds)])
    cs = np.arange(len(starts)) * 2 / display.REFRESH_HZ
    v = 900.0
    r, worst, dev, _ = ob.judder(starts, cs, v)
    assert worst == pytest.approx(0.5 * v * display.PERIOD_S, rel=0.15)
    assert r > 0


def test_composed_holds_never_replace_frames_and_average_k():
    for M in (1, 2, 3):
        s, _ = display.schedule(M, 600, "composed", seed=3)
        h = display.holds(s)
        assert h.min() >= 1
        assert abs(h.mean() - 6 / M) < 0.05 * 6 / M
