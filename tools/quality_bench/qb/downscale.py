# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""3840 -> 2560 (1.5x) downscale at Present, three filters, on CODE rasters (periodic addressing: scenes are periodic).

  p16    Package 16's PresentBlit when shrinking: ONE bilinear tap per output pixel at its centre, on the codes as stored
         (PQ codes for HDR10): PresentBlit.hlsl FineSample (SMOOTHNESS_60_VS_90.md 2(e)).
  area   stream s12-pace's area filter as vendored (vendor/PushDownscale.h pushBlitSample, area = true): n x n bilinear taps
         (n = 3 at 1.5x) over the output pixel's footprint, each tap bilinear ON CODES, decoded (PQ -> linear) per tap,
         averaged in linear light, encoded again. Being reworked upstream (review 2 MUST 2); re-take with vendor/retake.sh.
  exact  the reviewer's recipe (S12_PACE_REVIEW2 MUST 2 fix 2): every source texel decoded, exact box (area) coverage
         weights of the output pixel's footprint, linear light, encoded again. Not in any branch yet: the TARGET filter.
Output codes are quantised to the signal's bits (the swap chain is 10-bit for HDR10, 8-bit for SDR).
tests/test_downscale.py checks `area` and `p16` against the vendored C++ header (built with g++).
"""
from __future__ import annotations

import math

import numpy as np

SCALE = 1.5
FILTERS = ("p16", "area", "exact")


def _lin_weights(n_src, n_out, offset=0.0):
    """Bilinear sampling matrix (n_out x n_src), periodic, at output-pixel centres + offset (source px)."""
    fx = n_src / n_out
    pos = (np.arange(n_out) + 0.5) * fx - 0.5 + offset
    i0 = np.floor(pos).astype(np.int64)
    f = pos - i0
    M = np.zeros((n_out, n_src))
    M[np.arange(n_out), i0 % n_src] += 1 - f
    M[np.arange(n_out), (i0 + 1) % n_src] += f
    return M


def _area_weights(n_src, n_out):
    """Exact box coverage: output pixel o covers source [o*fx, (o+1)*fx), periodic; rows sum to 1."""
    fx = n_src / n_out
    M = np.zeros((n_out, n_src))
    for o in range(n_out):
        a, b = o * fx, (o + 1) * fx
        for s in range(int(math.floor(a)), int(math.ceil(b))):
            cov = min(b, s + 1) - max(a, s)
            if cov > 0:
                M[o, s % n_src] += cov / fx
    return M


def downscale(codes, signal, filt, out_shape=None, quant=True):
    H, W = codes.shape
    oh, ow = out_shape or (int(round(H / SCALE)), int(round(W / SCALE)))
    if filt == "p16":
        out = _lin_weights(H, oh) @ codes @ _lin_weights(W, ow).T
    elif filt == "exact":
        lin = signal.decode(codes)
        out = signal.encode(_area_weights(H, oh) @ lin @ _area_weights(W, ow).T, quant=False)
    elif filt == "area":
        fx, fy = W / ow, H / oh
        n = int(np.clip(math.ceil(max(fx, fy)) + 1, 3, 5))
        acc = np.zeros((oh, ow))
        for j in range(n):
            My = _lin_weights(H, oh, ((j + 0.5) / n - 0.5) * fy)
            rows = My @ codes
            for i in range(n):
                Mx = _lin_weights(W, ow, ((i + 0.5) / n - 0.5) * fx)
                acc += signal.decode(rows @ Mx.T)
        out = signal.encode(acc / (n * n), quant=False)
    else:
        raise ValueError(filt)
    return signal.quant(out) if quant else out
