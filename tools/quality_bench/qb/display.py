# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Display stage: how many refreshes of his 179.952 Hz OLED (sample-and-hold, no strobing) each output frame stays.

  ideal     source locked to the panel (6 refreshes = one source frame): 6 / 3 / 2 refreshes at 30 / 60 / 90 FPS.
  composed  a MODEL of the composed (DWM, fixed refresh) window path, as fitted by stream s12-pace to his measured phase
            bins (SMOOTHNESS_60_VS_90.md section 7, "Phase lock, simulated"): Presents on the content grid (29.97 Hz source
            x M), the compositor's cut-off before each vblank uniform 0.3-3.0 ms, and a 12 % chance per vblank that its pass
            is late. A frame is shown from the first vblank whose (on-time) pass comes after its Present, and never before the
            refresh after the previous frame's (SyncInterval 1 queues; nothing is replaced). This reproduces
            their 67-68 % of frames held exactly k refreshes; the share on HIS screen is not measured (the [cadence] line
            will measure it). `composed29.6` is the same with session A's 29.60 Hz source (2.35 forced long holds per second).
Returns per output frame its first refresh index; holds = differences (0 = replaced, never shown).
"""
from __future__ import annotations

import numpy as np

REFRESH_HZ = 179.952
PERIOD_S = 1.0 / REFRESH_HZ


def schedule(multiplier, n_frames, mode="ideal", seed=0, lead_ms=(0.3, 3.0), late_p=0.12, src_hz=29.97):
    """First refresh of each of n_frames output frames (int array, starting at 0) and the content time of each frame in
    seconds (array). Content frame i shows source time i / multiplier."""
    k = 6 // multiplier
    if mode == "ideal":
        src = REFRESH_HZ / 6.0
        starts = np.arange(n_frames) * k
        return starts, np.arange(n_frames) / (multiplier * src)
    if mode.startswith("composed"):
        if mode == "composed29.6":
            src_hz = 29.60
        rng = np.random.default_rng(seed)
        tout = 1.0 / (src_hz * multiplier)
        t = 0.0021 + np.arange(n_frames) * tout + rng.normal(0, 0.00005, n_frames)   # 2.1 ms phase start, 50 us jitter
        n_vb = int(t[-1] / PERIOD_S) + 8
        lead = rng.uniform(lead_ms[0], lead_ms[1], n_vb) / 1000.0
        late = rng.random(n_vb) < late_p
        cut = np.arange(n_vb) * PERIOD_S - lead
        starts = np.empty(n_frames, np.int64)
        n = 0
        for i in range(n_frames):
            n = starts[i - 1] + 1 if i else 0   # SyncInterval 1: every queued Present is shown for at least one refresh
            while cut[n] < t[i] or late[n]:
                n += 1
            starts[i] = n
        return starts - starts[0], np.arange(n_frames) * tout
    raise ValueError(mode)


def holds(starts):
    return np.diff(starts)


def hold_histogram(starts, multiplier):
    h = holds(starts)
    k = 6 // multiplier
    return {"k": k, "even_pct": 100.0 * float(np.mean(h == k)), "short_pct": 100.0 * float(np.mean(h < k)),
            "long_pct": 100.0 * float(np.mean(h > k)), "replaced_pct": 100.0 * float(np.mean(h == 0)),
            "hist": {int(v): int(c) for v, c in zip(*np.unique(h, return_counts=True))}}


def refresh_frames(starts):
    """Frame index shown at every refresh from starts[0] to starts[-1] - 1 (frames with hold 0 never appear)."""
    out = np.empty(int(starts[-1] - starts[0]), np.int64)
    for i in range(len(starts) - 1):
        out[starts[i] - starts[0]: starts[i + 1] - starts[0]] = i
    return out
