#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Why the FidelityFX start-up self-check found nothing on the owner's card (Package 16 log, 25 Nov 2026), SIMULATED.

Builds the check's frames exactly as PushPredictPass::runFidelityCheck does (pushFlowCheckWindows: six windows at
3840x2160 scrolling 0, 1, 4, 12, 30, 4 flow px a frame; box-downsampled 4x to 960x540) with the OLD periodic texture
(pushFlowCheckGrey) and the NEW broadband one (pushFlowCheckGreyBroadband, the same hash and octaves as
include/veyra/pipeline/PushPredictCheck.h), and runs a FidelityFX-like coarse-to-fine block search on frames 9 -> 10:
7 levels, 8x8 blocks, 16 x 16 candidates (-8..7) per level, ties to the centre (FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS),
the zero-vector fallback at level 0 (FFX_LOCAL_SEARCH_FALLBACK), a 3x3 vector median per level (the Filter pass). It is
a MODEL of AMD's shaders (SDK 1.1.4), not AMD's code; it runs on the CPU.

  python3 tools/pushwarp_eval/ffx_check_sim.py
"""
import numpy as np

W, H, FW, FH = 3840, 2160, 960, 540
SPEC = [(0, 0), (1, 1), (4, -4), (-12, 4), (30, 0), (-4, -1)]
OCTAVES = [(512.0, 24.0), (128.0, 18.0), (32.0, 12.0), (8.0, 8.0)]


def windows():
    sx, sy = W / FW, H / FH
    out = []
    for i, (fx, fy) in enumerate(SPEC):
        x0, x1 = W * (i % 3) // 3, W * (i % 3 + 1) // 3
        y0, y1 = H * (i // 3) // 2, H * (i // 3 + 1) // 2
        cx, cy = max(1, int((x1 - x0) / sx / 8)), max(1, int((y1 - y0) / sy / 8))
        fvx = 0 if fx == 0 else int(np.clip(fx, -cx, cx)); fvy = 0 if fy == 0 else int(np.clip(fy, -cy, cy))
        out.append((x0, y0, x1, y1, int(round(fvx * sx)), int(round(fvy * sy)), fvx, fvy))
    return out


def grey_old(x, y):
    return 128 + 45 * np.sin(0.21 * x) + 35 * np.sin(0.17 * y) + 25 * np.sin(0.53 * x) * np.cos(0.29 * y)


def noise(t, cell, seed):
    fi = np.floor(t / cell); f = t / cell - fi
    def h(i):
        n = (i.astype(np.int64).astype(np.uint32) * np.uint32(374761393) + np.uint32(seed) * np.uint32(668265263)).astype(np.uint32)
        n = ((n ^ (n >> np.uint32(13))) * np.uint32(1274126177)).astype(np.uint32)
        return ((n ^ (n >> np.uint32(16))) & np.uint32(0xFFFF)).astype(np.float64) / 65535.0 * 2 - 1
    s = f * f * (3 - 2 * f)
    return h(fi) * (1 - s) + h(fi + 1) * s


def grey_new(x, y):
    g = 128.0
    for k, (cell, amp) in enumerate(OCTAVES):
        g = g + 0.5 * amp * noise(x, cell, 2 * k + 1) + 0.5 * amp * noise(y, cell, 2 * k + 2) \
            + amp * noise(x, cell * 0.7, 2 * k + 11) * noise(y, cell * 0.7, 2 * k + 12)
    return g


def frame(k, grey, wins):
    img = np.zeros((H, W), np.float32)
    for (x0, y0, x1, y1, vx, vy, _, _) in wins:
        xs = np.arange(x0, x1, dtype=np.float64) - vx * k
        ys = np.arange(y0, y1, dtype=np.float64) - vy * k
        img[y0:y1, x0:x1] = np.clip(grey(xs[None, :], ys[:, None]), 0, 255)
    return np.round(img.reshape(FH, 4, FW, 4).mean(axis=(1, 3))).astype(np.int32)


def search(cur, prev, pred, radius=8, block=8, zero_fallback=False):
    h, w = cur.shape
    bh, bw = h // block, w // block
    c = cur[: bh * block, : bw * block].reshape(bh, block, bw, block).transpose(0, 2, 1, 3)
    pp = np.pad(prev, radius + 2048, mode="edge"); off = radius + 2048
    by, bx = np.mgrid[0:bh, 0:bw]
    best = np.full((bh, bw), np.iinfo(np.int64).max, np.int64); bestv = np.zeros((bh, bw, 2), np.int32)
    iy = np.arange(block)
    for dx, dy in sorted(((dx, dy) for dy in range(-radius, radius) for dx in range(-radius, radius)), key=lambda d: (abs(d[1]), abs(d[0]), d[1], d[0])):
        sx = np.clip(bx * block + pred[..., 0] + dx + off, 0, pp.shape[1] - block)
        sy = np.clip(by * block + pred[..., 1] + dy + off, 0, pp.shape[0] - block)
        patch = pp[sy[..., None, None] + iy[None, None, :, None], sx[..., None, None] + iy[None, None, None, :]]
        sad = np.abs(patch - c).sum(axis=(2, 3))
        better = sad < best
        best = np.where(better, sad, best); bestv[better] = (pred + np.array([dx, dy]))[better]
    if zero_fallback:
        patch = pp[(by * block + off)[..., None, None] + iy[None, None, :, None], (bx * block + off)[..., None, None] + iy[None, None, None, :]]
        z = np.abs(patch - c).sum(axis=(2, 3)) <= best
        bestv[z] = 0
    return bestv


def median3(v):
    h, w = v.shape[:2]
    p = np.pad(v, ((1, 1), (1, 1), (0, 0)), mode="edge")
    cand = np.stack([p[1 + dy: 1 + dy + h, 1 + dx: 1 + dx + w] for dy in (-1, 0, 1) for dx in (-1, 0, 1)], 0)
    d = ((cand[:, None] - cand[None, :]) ** 2).sum(-1).sum(1)
    return np.take_along_axis(cand, d.argmin(0)[None, ..., None].repeat(2, -1), 0)[0]


def flow(cur, prev, levels=7):
    pc, pp = [cur.astype(np.float64)], [prev.astype(np.float64)]
    for _ in range(levels - 1):
        for pyr in (pc, pp):
            a = pyr[-1]; h2, w2 = a.shape[0] // 2, a.shape[1] // 2
            pyr.append(a[: 2 * h2, : 2 * w2].reshape(h2, 2, w2, 2).mean(axis=(1, 3)))
    vec = None
    for lvl in range(levels - 1, -1, -1):
        c, p = np.round(pc[lvl]).astype(np.int32), np.round(pp[lvl]).astype(np.int32)
        bh, bw = max(1, c.shape[0] // 8), max(1, c.shape[1] // 8)
        if c.shape[0] < 8 or c.shape[1] < 8:
            continue
        if vec is None:
            pred = np.zeros((bh, bw, 2), np.int32)
        else:
            ys = np.clip(np.arange(bh) // 2, 0, vec.shape[0] - 1); xs = np.clip(np.arange(bw) // 2, 0, vec.shape[1] - 1)
            pred = vec[ys][:, xs] * 2
        vec = median3(search(c, p, pred, zero_fallback=(lvl == 0)))
    return vec


def main():
    np.seterr(over="ignore")   # the hash wraps in uint32 on purpose, as in C++
    wins = windows()
    for name, grey in (("old periodic texture (Package 16)", grey_old), ("broadband texture (this fix)", grey_new)):
        vec = flow(frame(9, grey, wins), frame(8, grey, wins))
        print(name)
        for i, (x0, y0, x1, y1, _, _, fvx, fvy) in enumerate(wins):
            v = vec[y0 // 32 + 2: y1 // 32 - 2, x0 // 32 + 2: x1 // 32 - 2].reshape(-1, 2)
            right = np.mean((np.abs(v[:, 0] + fvx) <= 1) & (np.abs(v[:, 1] + fvy) <= 1))
            print(f"  w{i} truth=({-fvx},{-fvy}) right={right:.2f} median=({np.median(v[:, 0]):.0f},{np.median(v[:, 1]):.0f})")


if __name__ == "__main__":
    main()
