# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Frame generation stage. Works on CODE rasters (what the generator receives from the capture card: PQ codes for HDR10).

  none     native 30: the real frame A is repeated (no generated frame).
  ideal    the true in-between picture, rendered at content time c0 + t with the same shutter: the UPPER BOUND.
  block    a block-motion interpolator MODEL (not NVIDIA's network): 16x16 blocks at 4K, each block takes the true motion
           of the layer that covers most of it (an ORACLE estimator: the best any block method can know); every pixel is
           (1 - t) A(x - t d) + t B(x + (1 - t) d), bilinear taps, blended in code values. A thin crosshair is far smaller
           than a block, so it moves with the background: the copies the owner sees (HUD_GHOSTING.md section 2).
  weak     the block model mixed with a cross-fade: (1 - beta) block + beta ((1 - t) A + t B), beta = 0.3. A KNOB standing for
           "a lower-quality model" (his VFG level 5 uses NVIDIA's Low model at t = 1/3, SETTINGS_AUDIT.md 9); its size is
           an assumption, not a property of NVIDIA's Low model.
  pushwarp PushWarp's CPU reference (vendored pushwarp_cli `frames`: the GPU path's rules on the FidelityFX-like flow MODEL).
HUD protection: `hudguard.py` (HUD Guard for block / weak) and PushWarp's own overlay rules (--overlay on|off).
"""
from __future__ import annotations

import numpy as np
from scipy import ndimage

BLOCK = 16
WEAK_BETA = 0.3


def block_field(scene, c0, block=BLOCK):
    """Per-pixel vector (dx, dy) in 4K px per real frame from the oracle block estimator."""
    layer, motions = scene.pair_vectors(c0)
    H, W = layer.shape
    vx = np.zeros((H, W)); vy = np.zeros((H, W))
    for by in range(0, H, block):
        for bx in range(0, W, block):
            ids, counts = np.unique(layer[by:by + block, bx:bx + block], return_counts=True)
            m = motions[int(ids[np.argmax(counts)])]
            vx[by:by + block, bx:bx + block] = m[0]
            vy[by:by + block, bx:bx + block] = m[1]
    return vx, vy


def _sample(img, xs, ys):
    return ndimage.map_coordinates(img, [ys, xs], order=1, mode="grid-wrap")


def interpolate(A, B, t, field):
    vx, vy = field
    H, W = A.shape
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float64)
    a = _sample(A, xx - t * vx, yy - t * vy)
    b = _sample(B, xx + (1 - t) * vx, yy + (1 - t) * vy)
    return (1 - t) * a + t * b


def generate(kind, scene, signal, A, B, c0, t, field=None, beta=WEAK_BETA):
    """One generated frame at c0 + t (codes, quantised). A, B: the real frames' codes (possibly HUD-cleaned for block)."""
    if kind == "ideal":
        return signal.encode(scene.render(c0 + t))
    if kind == "none":
        return A.copy()
    field = field if field is not None else block_field(scene, c0)
    out = interpolate(A, B, t, field)
    if kind == "weak":
        out = (1 - beta) * out + beta * ((1 - t) * A + t * B)
    elif kind != "block":
        raise ValueError(kind)
    return signal.quant(out)
