# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""The whole chain, stage by stage: real frames (4K codes) -> generated frames -> downscale -> output frames (codes, nits).

Generator kinds: none | ideal | block | weak | mixed (3X: weak at t = 1/3, block at 2/3; 2X: block) | pushwarp.
HUD protection: guard=True runs HUD Guard (block / weak / mixed); for pushwarp it switches its overlay rules on.
"""
from __future__ import annotations

import numpy as np

from . import downscale as ds
from . import generators as gens
from . import native


def real_frames(scene, signal, n_src, hud=True):
    return [signal.encode(scene.render(float(j), hud=hud)) for j in range(n_src)]


def sequence(scene, signal, multiplier, kind, n_src, guard=False, reals=None, hud=True):
    """Output frames at 4K (codes) in display order and their content times (source frames). Real frame j is followed by
    its pair's generated frames; the last real frame closes the sequence. Also returns per-frame info (generated, t)."""
    reals = reals if reals is not None else real_frames(scene, signal, n_src, hud=hud)
    frames, times, info = [], [], []
    masks = cleaned = None
    beta = gens.WEAK_BETA
    if kind.startswith("mixed:"):
        kind, beta = "mixed", float(kind.split(":")[1])
    if guard and kind in ("block", "weak", "mixed"):
        masks, cleaned = native.hud_guard(reals, signal.hdr)
    for j in range(n_src - 1):
        A, B = reals[j], reals[j + 1]
        frames.append(A); times.append(float(j)); info.append((False, 0.0))
        if multiplier == 1:
            continue
        if kind == "pushwarp":
            outs, meta = native.pushwarp(A, B, multiplier, signal.hdr, overlay=guard)
            for k, g in enumerate(outs, start=1):
                frames.append(g); times.append(j + k / multiplier); info.append((True, k / multiplier, meta[k - 1]))
            continue
        field = gens.block_field(scene, float(j)) if kind in ("block", "weak", "mixed") else None
        ga, gb = (cleaned[j], cleaned[j + 1]) if masks is not None else (A, B)
        for k in range(1, multiplier):
            t = k / multiplier
            kk = kind
            if kind == "mixed":
                kk = "weak" if (multiplier == 3 and k == 1) else "block"
            if kk == "none":
                g = A.copy()
            elif kk == "ideal":
                g = gens.generate("ideal", scene, signal, A, B, float(j), t)
            else:
                g = gens.generate(kk, scene, signal, ga, gb, float(j), t, field=field, beta=beta)
                if masks is not None:
                    m = (masks[j + 1] & 1) > 0
                    g = g.copy()
                    g[m] = signal.quant((A + t * (B - A))[m])
            frames.append(g); times.append(j + t); info.append((True, t))
    frames.append(reals[-1]); times.append(float(n_src - 1)); info.append((False, 0.0))
    return frames, np.array(times), info


def to_output(frames, signal, filt):
    """Downscaled output codes and nits."""
    outc = [ds.downscale(f, signal, filt) for f in frames]
    return outc, [signal.decode(c) for c in outc]
