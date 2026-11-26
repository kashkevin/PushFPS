# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Synthetic 4K scenes with exactly known motion (SIMULATED material, not game footage).

A scene is a region of interest of the 3840x2160 picture (a crop: every stage of the chain is local, so a crop behaves as
the same place in a whole frame). The world is PERIODIC in the crop (period = crop size), so a camera move is an exact
circular shift and the generators can sample outside the crop. Pixels are AREA samples of a fine world grid (q x q
sub-cells per 4K pixel), i.e. an ideally anti-aliased render; camera offsets are rounded to 1 / q pixel.

Layers, composited in linear light (nits): background world (camera path), moving objects (rectangles with their own
screen-space motion), HUD (fixed on screen, never motion-blurred). Console-style motion blur: the background and objects
are averaged over the shutter interval (shutter = share of the 1/30 s frame: 0, 0.5, 1).
Time unit everywhere: source (real) frames; content time c = 2.5 is halfway between real frames 2 and 3.
"""
from __future__ import annotations

import math

import numpy as np

Q = 4   # fine sub-cells per 4K pixel


# ---- textures (functions of fine-grid coordinates in 4K pixels) ----------------------------------------------------------
def value_noise(X, Y, period, cells, amps, seed):
    """Periodic smooth value noise in [-1, 1] (approximately); X (W,), Y (H,) in px; every cell size must divide the period."""
    rng = np.random.default_rng(seed)
    out = np.zeros((len(Y), len(X)))
    for cell, amp in zip(cells, amps):
        nx, ny = max(1, int(round(period[0] / cell))), max(1, int(round(period[1] / cell)))
        G = rng.uniform(-1.0, 1.0, (ny, nx))
        fx, fy = X / cell, Y / cell
        ix, iy = np.floor(fx).astype(np.int64), np.floor(fy).astype(np.int64)
        sx, sy = fx - ix, fy - iy
        sx, sy = sx * sx * (3 - 2 * sx), sy * sy * (3 - 2 * sy)
        x0, x1, y0, y1 = ix % nx, (ix + 1) % nx, iy % ny, (iy + 1) % ny
        top = G[y0][:, x0] * (1 - sx) + G[y0][:, x1] * sx
        bot = G[y1][:, x0] * (1 - sx) + G[y1][:, x1] * sx
        out += amp * (top * (1 - sy)[:, None] + bot * sy[:, None])
    return np.clip(out / sum(amps), -1.0, 1.0)


def _cover1d(x, a, w, period):
    """Exact coverage of pixel cells [x, x+1) by the interval [a, a+w), periodic."""
    c = np.zeros_like(x, dtype=np.float64)
    for k in (-1, 0, 1):
        s = a + k * period
        c += np.clip(np.minimum(x + 1.0, s + w) - np.maximum(x, s), 0.0, 1.0)
    return np.clip(c, 0.0, 1.0)


class Scene:
    """world(X, Y) -> nits on the fine grid; path(c) -> (ox, oy) content offset in 4K px (content moves by +offset)."""

    def __init__(self, width, height, world, path, shutter=0.0, objects=(), hud=None, qy=Q, name=""):
        self.w, self.h, self.qy, self.name = int(width), int(height), int(qy), name
        X = (np.arange(self.w * Q) + 0.5) / Q
        Y = (np.arange(self.h * self.qy) + 0.5) / self.qy
        self.world_fine = np.asarray(world(X, Y), dtype=np.float64)
        self.path, self.shutter, self.objects = path, float(shutter), list(objects)
        self.hud = hud   # (alpha HxW, nits) or None

    # -- rendering --------------------------------------------------------------------------------------------------------
    def _reduce(self, fine):
        return fine.reshape(self.h, self.qy, self.w, Q).mean(axis=(1, 3))

    def _shifted(self, ox, oy):
        return np.roll(self.world_fine, (int(round(oy * self.qy)), int(round(ox * Q))), axis=(0, 1))

    def background(self, c):
        o0 = np.array(self.path(c), dtype=np.float64)
        if self.shutter <= 0:
            return self._reduce(self._shifted(*o0))
        a, b = np.array(self.path(c - self.shutter / 2)), np.array(self.path(c + self.shutter / 2))
        mid = np.array(self.path(c))
        straight = abs(a[1] - b[1]) < 1e-9 and abs(mid[1] - a[1]) < 1e-9 and abs((a[0] + b[0]) / 2 - mid[0]) < 1e-6
        if straight:   # horizontal pan: an exact box along x on the fine grid
            L = int(round(abs(b[0] - a[0]) * Q))
            base = self._shifted(min(a[0], b[0]), a[1])
            if L <= 1:
                return self._reduce(base)
            cs = np.cumsum(np.concatenate([base, base[:, :L]], axis=1), axis=1)
            cs = np.concatenate([np.zeros((cs.shape[0], 1)), cs], axis=1)
            n = base.shape[1]
            box = (cs[:, L:L + n] - cs[:, 0:n]) / L          # forward window: box[k] = mean base[k .. k+L-1]
            box = np.roll(box, L - 1, axis=1)                  # offsets a .. a+(L-1)/Q: img[x] = mean base[x-L+1 .. x]
            return self._reduce(box)
        length = sum(np.hypot(*(np.array(self.path(c - self.shutter / 2 + self.shutter * (i + 1) / 32)) -
                                np.array(self.path(c - self.shutter / 2 + self.shutter * i / 32)))) for i in range(32))
        n = int(np.clip(math.ceil(length * 2), 2, 96))
        acc = np.zeros((self.h, self.w))
        for i in range(n):
            acc += self._reduce(self._shifted(*self.path(c - self.shutter / 2 + self.shutter * (i + 0.5) / n)))
        return acc / n

    def object_cover(self, ob, c, blur=True):
        xs, ys = np.arange(self.w, dtype=np.float64), np.arange(self.h, dtype=np.float64)
        speed = math.hypot(ob.get("vx", 0.0), ob.get("vy", 0.0))
        n = 1 if (not blur or self.shutter <= 0 or speed == 0) else int(np.clip(math.ceil(speed * self.shutter * 2) + 1, 2, 256))
        acc = np.zeros((self.h, self.w))
        for i in range(n):
            cc = c if n == 1 else c - self.shutter / 2 + self.shutter * (i + 0.5) / n
            ax = ob["x"] + ob.get("vx", 0.0) * cc
            ay = ob["y"] + ob.get("vy", 0.0) * cc
            acc += _cover1d(ys, ay % self.h, ob["h"], self.h)[:, None] * _cover1d(xs, ax % self.w, ob["w"], self.w)[None, :]
        return acc / n

    def render(self, c, hud=True):
        img = self.background(c)
        for ob in self.objects:
            cov = self.object_cover(ob, c) * ob.get("alpha", 1.0)
            img = img * (1 - cov) + ob["nits"] * cov
        if hud and self.hud is not None:
            a, col = self.hud
            img = img * (1 - a) + col * a
        return img

    # -- what a perfect motion estimator knows (the block model's oracle) -------------------------------------------------
    def pair_vectors(self, c0):
        """Per-pixel layer id at the pair's middle (0 background, 1.. objects, 255 HUD) and each layer's motion A -> B."""
        layer = np.zeros((self.h, self.w), np.int32)
        for k, ob in enumerate(self.objects):
            layer[self.object_cover(ob, c0 + 0.5, blur=False) >= 0.5] = k + 1
        if self.hud is not None:
            layer[self.hud[0] >= 0.5] = 255
        a, b = np.array(self.path(c0)), np.array(self.path(c0 + 1))
        motions = {0: (b - a)[::1], 255: np.zeros(2)}
        for k, ob in enumerate(self.objects):
            motions[k + 1] = np.array([ob.get("vx", 0.0), ob.get("vy", 0.0)])
        return layer, motions


# ---- camera paths -------------------------------------------------------------------------------------------------------
def pan(v, vy=0.0):
    return lambda c: (v * c, vy * c)


def still():
    return lambda c: (0.0, 0.0)


def circle(speed, frames_per_turn=8):
    """Circular camera move: chord per real frame = `speed` 4K px, direction turning 360/frames_per_turn degrees a frame."""
    w = 2 * math.pi / frames_per_turn
    r = speed / (2 * math.sin(w / 2))
    return lambda c: (r * (math.cos(w * c) - 1.0), r * math.sin(w * c))


# ---- HUD layers (4x super-sampled coverage, fixed on screen) ------------------------------------------------------------
_FONT = {   # 5x7 bitmap glyphs (rows top to bottom)
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "0": ["01110", "10011", "10101", "10101", "11001", "10001", "01110"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    " ": ["00000"] * 7,
}


def hud_alpha(w, h, kind, cx, cy, width=2, opacity=1.0, ss=4):
    """Coverage of a HUD element at 4K: 'crosshair' (arms + centre dot, stroke `width`), 'dot' (width x width square),
    'text' ("AMMO 30 HP 87", strokes `width` px), 'ring' (minimap ring radius 60, stroke `width`). Anti-aliased by area."""
    big = np.zeros((h * ss, w * ss), bool)
    yy, xx = np.mgrid[0:h * ss, 0:w * ss]
    X, Y = (xx + 0.5) / ss, (yy + 0.5) / ss
    if kind in ("crosshair", "dot"):
        half = width / 2.0
        big |= (np.abs(X - cx) <= half) & (np.abs(Y - cy) <= half)
        if kind == "crosshair":
            arm, gap = 28.0, 6.0
            horiz = (np.abs(Y - cy) <= half) & (np.abs(X - cx) >= gap) & (np.abs(X - cx) <= arm)
            vert = (np.abs(X - cx) <= half) & (np.abs(Y - cy) >= gap) & (np.abs(Y - cy) <= arm)
            big |= horiz | vert
    elif kind == "text":
        s = width
        text = "AMMO 30 HP 87"
        x0 = cx - len(text) * 6 * s / 2.0
        y0 = cy - 3.5 * s
        for k, ch in enumerate(text):
            for r, row in enumerate(_FONT[ch]):
                for col, bit in enumerate(row):
                    if bit == "1":
                        gx, gy = x0 + (k * 6 + col) * s, y0 + r * s
                        big |= (X >= gx) & (X < gx + s) & (Y >= gy) & (Y < gy + s)
    elif kind == "ring":
        rad = np.hypot(X - cx, Y - cy)
        big |= np.abs(rad - 60.0) <= width / 2.0
    else:
        raise ValueError(kind)
    return big.reshape(h, ss, w, ss).mean(axis=(1, 3)) * opacity


# ---- worlds -------------------------------------------------------------------------------------------------------------
def textured(width, height, mean=40.0, contrast=0.8, seed=1, cells=None):
    cells = cells or [c for c in (96, 32, 12, 4) if width % c == 0 and height % c == 0] or [4]
    amps = [0.5, 0.3, 0.2, 0.12][: len(cells)]

    def world(X, Y):
        n = value_noise(X, Y, (width, height), cells, amps, seed)
        return np.maximum(mean * (1.0 + contrast * n), 0.2)
    return world


def bars(period, lo, hi):
    def world(X, Y):
        return np.where((X % period) < period / 2, hi, lo)[None, :] * np.ones((len(Y), 1))
    return world


def sine(freq_4k, mean, amp):
    def world(X, Y):
        return (mean * (1.0 + amp * np.sin(2 * math.pi * freq_4k * X)))[None, :] * np.ones((len(Y), 1))
    return world


def lines(bg, items):
    """items: (x, width, nits) vertical lines on a flat background (exact on the fine grid when x, width are k/4)."""
    def world(X, Y):
        row = np.full(len(X), float(bg))
        for x, w, nits in items:
            row = np.where((X >= x) & (X < x + w), nits, row)
        return row[None, :] * np.ones((len(Y), 1))
    return world


def flat(nits):
    return lambda X, Y: np.full((len(Y), len(X)), float(nits))
