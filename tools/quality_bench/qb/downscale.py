# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""3840 -> 2560 (1.5x) downscale at Present, DATA-DRIVEN: a filter is one `register(...)` line in KERNELS below.

Three kinds:
  tap      Package 16's PresentBlit: ONE bilinear tap per output pixel at its centre, on the codes as stored (PQ for HDR10).
  taps3    the first s12-pace area filter (no longer upstream): 3x3 bilinear taps on codes, decoded per tap, averaged in
           linear light.
  weights  every source texel under the kernel's support is decoded to LINEAR light, weighted, normalised, encoded again
           (what vendor/PushDownscale.h pushShrinkSample does). A kernel is a weight function w(c, f, i): c = the output
           pixel's centre in source coordinates, f = source px per output px, i = texel indices (array); `radius` is the
           support in OUTPUT pixels. Optional `clamp=True` = anti-ringing: the linear result is limited to the min / max of
           the source texels inside the plain one-output-pixel box footprint (no new overshoot or undershoot; no extra loads
           because those texels are already inside the kernel's support).
Output codes are quantised to the signal's bits (10-bit HDR10, 8-bit SDR). Scenes are periodic, so addressing wraps.
tests/test_downscale.py checks every kernel that exists in the vendored header against its C++ (g++).
`cost(name)` is the cost proxy: texel reads per output pixel (direct 2-D, and as two 1-D passes) and bilinear fetches.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Callable, Optional

import numpy as np

SCALE = 1.5
SHARPEN = 0.03          # kPushShrinkSharpen of vendor/PushDownscale.h


def _box(i, a, b):
    return np.clip(np.minimum(i + 1.0, b) - np.maximum(i, a), 0.0, None)


def _cubic(b, c):
    def k(x):
        x = np.abs(x)
        n = ((12 - 9 * b - 6 * c) * x ** 3 + (-18 + 12 * b + 6 * c) * x ** 2 + (6 - 2 * b)) / 6
        f = ((-b - 6 * c) * x ** 3 + (6 * b + 30 * c) * x ** 2 + (-12 * b - 48 * c) * x + (8 * b + 24 * c)) / 6
        return np.where(x < 1, n, np.where(x < 2, f, 0.0))
    return k


def _lanczos(a):
    def k(x):
        return np.where(np.abs(x) < a, np.sinc(x) * np.sinc(x / a), 0.0)
    return k


def point_kernel(fn):
    """Weight function from a kernel fn(x) in output-pixel units, evaluated at the texel centre (pushShrinkKernel)."""
    return lambda c, f, i: fn((i + 0.5 - c) / f)


def area_sharp(s=SHARPEN):
    return lambda c, f, i: ((1 + 2 * s) * _box(i, c - f / 2, c + f / 2)
                            - s * (_box(i, c - 1.5 * f, c - f / 2) + _box(i, c + f / 2, c + 1.5 * f)))


@dataclass(frozen=True)
class Kernel:
    name: str
    kind: str                                   # tap | taps3 | weights
    note: str = ""
    radius: float = 0.0
    weight: Optional[Callable] = None
    clamp: bool = False


KERNELS: dict = {}


def register(name, kind="weights", note="", radius=0.0, weight=None, clamp=False):
    KERNELS[name] = Kernel(name, kind, note, radius, weight, clamp)


register("p16", "tap", "Package 16: one bilinear tap on codes")
register("area", "taps3", "pace's first filter (3x3 bilinear taps on codes, linear-light average); no longer upstream")
register("exact", radius=0.5, weight=lambda c, f, i: _box(i, c - f / 2, c + f / 2), note="exact box (area) coverage, linear light (header: Area)")
register("area_sharp", radius=1.5, weight=area_sharp(), note="pace's committed kernel: box + 3 % unsharp from the neighbouring boxes (header: AreaSharp)")
register("mitchell", radius=2.0, weight=point_kernel(_cubic(1 / 3, 1 / 3)), note="Mitchell-Netravali B=C=1/3")
register("catmull_rom", radius=2.0, weight=point_kernel(_cubic(0.0, 0.5)), note="Catmull-Rom, linear light")
register("catmull_rom_ar", radius=2.0, weight=point_kernel(_cubic(0.0, 0.5)), clamp=True, note="Catmull-Rom, linear light, anti-ringing clamp")
register("lanczos2", radius=2.0, weight=point_kernel(_lanczos(2)), note="Lanczos-2, linear light")
register("lanczos2_ar", radius=2.0, weight=point_kernel(_lanczos(2)), clamp=True, note="Lanczos-2, linear light, anti-ringing clamp")
register("area_sharp_ar", radius=1.5, weight=area_sharp(), clamp=True, note="AreaSharp (3 %) with the anti-ringing clamp")
# examples of "a new kernel is a few lines":
register("area_sharp8", radius=1.5, weight=area_sharp(0.08), note="box + 8 % unsharp (stronger AreaSharp)")
register("area_sharp8_ar", radius=1.5, weight=area_sharp(0.08), clamp=True, note="box + 8 % unsharp, anti-ringing clamp")

FILTERS = ("p16", "area", "exact")                      # the original three (kept for older tables / tests)
HUD_FILTERS = ("p16", "exact", "area_sharp")            # Q3 grid: today, target, pace's committed kernel
Q2_FILTERS = ("p16", "area", "exact", "area_sharp", "area_sharp_ar", "area_sharp8", "area_sharp8_ar", "mitchell",
              "catmull_rom", "catmull_rom_ar", "lanczos2", "lanczos2_ar")
HEADER_KERNELS = {"p16": 0, "exact": 1, "catmull_rom": 2, "mitchell": 3, "lanczos2": 4, "area_sharp": 5}   # PushShrinkKernel ids

_wcache: dict = {}


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
    return kernel_weights(n_src, n_out, "exact")


def kernel_weights(n_src, n_out, name):
    """Normalised weight matrix (n_out x n_src), periodic, of a `weights` kernel."""
    key = (n_src, n_out, name)
    if key not in _wcache:
        k = KERNELS[name]
        f = n_src / n_out
        M = np.zeros((n_out, n_src))
        for o in range(n_out):
            c = (o + 0.5) * f
            g = max(f, 1.0)
            r = k.radius * g
            i = np.arange(int(math.floor(c - r)), int(math.ceil(c + r)))
            w = k.weight(c, g, i.astype(np.float64))
            np.add.at(M[o], i % n_src, w)
        M /= M.sum(axis=1, keepdims=True)
        _wcache[key] = M
    return _wcache[key]


def _footprint_extrema(lin, n_out, axis, use_max):
    """Min (or max) of the source texels inside each output pixel's plain box footprint along `axis` (periodic)."""
    n = lin.shape[axis]
    f = n / n_out
    lo = np.floor(np.arange(n_out) * f + 1e-9).astype(np.int64)
    hi = np.ceil((np.arange(n_out) + 1) * f - 1e-9).astype(np.int64)      # exclusive
    acc = None
    shape = [1, 1]
    shape[axis] = n_out
    for d in range(int(np.max(hi - lo))):
        ok = ((lo + d) < hi).reshape(shape)
        v = np.take(lin, (lo + d) % n, axis=axis)
        v = np.where(ok, v, -np.inf if use_max else np.inf)
        acc = v if acc is None else (np.maximum(acc, v) if use_max else np.minimum(acc, v))
    return acc


def downscale(codes, signal, filt, out_shape=None, quant=True):
    H, W = codes.shape
    oh, ow = out_shape or (int(round(H / SCALE)), int(round(W / SCALE)))
    k = KERNELS[filt]
    if k.kind == "tap":
        out = _lin_weights(H, oh) @ codes @ _lin_weights(W, ow).T
    elif k.kind == "taps3":
        fx, fy = W / ow, H / oh
        n = int(np.clip(math.ceil(max(fx, fy)) + 1, 3, 5))
        acc = np.zeros((oh, ow))
        for j in range(n):
            rows = _lin_weights(H, oh, ((j + 0.5) / n - 0.5) * fy) @ codes
            for i in range(n):
                acc += signal.decode(rows @ _lin_weights(W, ow, ((i + 0.5) / n - 0.5) * fx).T)
        out = signal.encode(acc / (n * n), quant=False)
    elif k.kind == "weights":
        lin = signal.decode(codes)
        res = kernel_weights(H, oh, filt) @ lin @ kernel_weights(W, ow, filt).T
        if k.clamp:
            lo = _footprint_extrema(_footprint_extrema(lin, oh, 0, False), ow, 1, False)
            hi = _footprint_extrema(_footprint_extrema(lin, oh, 0, True), ow, 1, True)
            res = np.clip(res, lo, hi)
        out = signal.encode(np.clip(res, 0.0, None), quant=False)
    else:
        raise ValueError(filt)
    return signal.quant(out) if quant else out


def cost(name, n_src=3840, n_out=2560):
    """Cost proxy per output pixel at 1.5x: texel reads (direct 2-D = nx*ny; two 1-D passes = nx + ny) and bilinear
    fetches. The anti-ringing clamp adds no loads (its texels are inside the kernel's support)."""
    k = KERNELS[name]
    if k.kind == "tap":
        return {"texels_2d": 4.0, "texels_sep": 4.0, "fetches": 1.0}
    if k.kind == "taps3":
        return {"texels_2d": 36.0, "texels_sep": 36.0, "fetches": 9.0}
    M = kernel_weights(n_src, n_out, name)
    n1 = float(np.mean((np.abs(M) > 1e-12).sum(axis=1)))
    return {"texels_2d": n1 * n1, "texels_sep": 2 * n1, "fetches": n1 * n1}
