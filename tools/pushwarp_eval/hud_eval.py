#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Crosshair / HUD ghosting in PushWarp's frames, before and after its static-overlay rules (PushWarp.h).

OFFLINE PICTURE MEASUREMENT ON SYNTHETIC MATERIAL, CPU ONLY: nothing about speed, nothing about any graphics card, not
game footage. A broadband background (value noise, as the FidelityFX check's texture) moves under a static overlay:
  overlays  crosshair 1 / 2 / 3 px (opaque white), crosshair 2 px at 50 % (translucent), HUD text, minimap ring
  motions   straight pan (24 px a frame at 1920x1080 = 48 at 4K) and a circular camera move (radius 40 px, 8 frames
            a turn: about 31 px a frame, the direction turning 45 degrees a frame: the owner's case)
Work size 1920x1080 with a 480x270 field (the play ratio 4:1); the field is the FidelityFX-like block search of
ffx_check_sim.py (7 levels, 8x8 blocks, +-8, centre ties, level-0 zero fallback, median) densified as the app's
NvofDensify (nearest block, photometric trust), NOT AMD's code. Frames at 2X (t = 1/2) and 3X (t = 1/3, 2/3).

Variants (pushwarp_cli PUSHWARP_OVERLAY_RULES; the mask is PushWarp's own: still in A and B where its field moves,
plus the anti-aliased rim, PushWarp.h pushWarpOverlayMask):
  before        PushWarp as it is (its still rule keeps exact A == B pixels)
  override      rule 1: overlay pixels = A + t (B - A)
  +sources      rules 1 + 2: taps within 2 px of an overlay core pixel are not used, the other side's tap alone
  +clean flow   rules 1 + 2 and the flow fed pictures with the mask's pixels filled from their surround (in play that
                would be the previous pair's mask: a static overlay's mask does not change)
  ideal mask    as "+clean flow" with the scene's true overlay mask instead of PushWarp's own (an upper bound)
Metrics (grey levels, RGB mean, against the rendered truth at t):
  overlay error   mean |error| on the overlay (alpha >= 0.5)
  ghost energy    around the overlay (96 px box, outside the grown overlay): mean of max(0, error - error of the same
                  generator on the same scene WITHOUT the overlay): what the overlay adds
  ghost pixels    pixels there whose added error exceeds 16 grey levels
  background     mean |error| outside the 96 px box (what a false mask pixel costs elsewhere), and its change
  mask recall / false share: PushWarp's own mask (bit 1) against the true overlay pixels (alpha >= 0.5) / elsewhere

  python3 tools/pushwarp_eval/hud_eval.py --cli $BIN [--scene crosshair2_circle] > report.md
"""
from __future__ import annotations

import argparse
import math
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ffx_check_sim as S  # noqa: E402  (the FidelityFX-like search)

W, H, FW, FH = 1920, 1080, 480, 270
CENTRE = (960, 540)


def noise2(x, y):
    g = 128.0
    for k, (cell, amp) in enumerate(S.OCTAVES):
        g = g + 0.5 * amp * S.noise(x, cell, 2 * k + 1) + 0.5 * amp * S.noise(y, cell, 2 * k + 2) \
            + amp * S.noise(x, cell * 0.7, 2 * k + 11) * S.noise(y, cell * 0.7, 2 * k + 12)
    return g


def background(shift):
    xs = np.arange(W, dtype=np.float64) - shift[0]
    ys = np.arange(H, dtype=np.float64) - shift[1]
    g = noise2(xs[None, :], ys[:, None])
    tint = np.stack([g * 0.9 + 10, g, g * 1.1 - 10], -1)
    return np.clip(tint, 0, 255)


def overlay(kind):
    """(alpha 0..1, colour) at 1920x1080, drawn at 4x and box-reduced (anti-aliased like a game's HUD)."""
    k = 4
    big = np.zeros((H * k, W * k), np.uint8)
    cx, cy = CENTRE[0] * k, CENTRE[1] * k
    opacity, colour = 1.0, (255, 255, 255)
    if kind.startswith("crosshair"):
        width = int(kind[9]) if kind[9].isdigit() else 2
        if kind.endswith("translucent"):
            opacity = 0.5
        arm, gap = 22 * k, 5 * k
        for (x0, y0, x1, y1) in ((cx - arm, cy, cx - gap, cy), (cx + gap, cy, cx + arm, cy), (cx, cy - arm, cx, cy - gap), (cx, cy + gap, cx, cy + arm)):
            cv2.line(big, (x0, y0), (x1, y1), 255, width * k)
        cv2.circle(big, (cx, cy), max(1, width * k // 2), 255, -1)
    elif kind == "text":
        cv2.putText(big, "AMMO 30 / 120   HP 87", (cx - 260 * k, cy + 60 * k), cv2.FONT_HERSHEY_SIMPLEX, 1.2 * k, 255, 2 * k, cv2.LINE_AA)
    elif kind == "minimap":
        cv2.circle(big, (cx, cy), 80 * k, 255, 3 * k)
        cv2.line(big, (cx - 6 * k, cy + 6 * k), (cx, cy - 8 * k), 255, 2 * k)
        cv2.line(big, (cx + 6 * k, cy + 6 * k), (cx, cy - 8 * k), 255, 2 * k)
    alpha = big.reshape(H, k, W, k).mean(axis=(1, 3)) / 255.0 * opacity
    return alpha, np.array(colour, np.float64)


def shift_at(motion, t):
    if motion == "pan":
        return (24.0 * t, 0.0)
    a = 2 * math.pi * t / 8.0
    return (40.0 * math.cos(a), 40.0 * math.sin(a))


def frame(motion, t, alpha, colour):
    bg = background(shift_at(motion, t))
    if alpha is None:
        return bg
    return bg * (1 - alpha[..., None]) + colour[None, None, :] * alpha[..., None]


def u8(img):
    return np.clip(np.round(img), 0, 255).astype(np.uint8)


def luma_small(img):
    y = 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]
    return np.round(y.reshape(FH, 4, FW, 4).mean(axis=(1, 3))).astype(np.int32)


def field_of(A, B):
    """B -> A field at 480x270 as the app densifies FidelityFX's blocks: nearest block, photometric trust."""
    la, lb = luma_small(A), luma_small(B)
    vec = S.flow(lb, la, levels=6)
    v = np.repeat(np.repeat(vec, 8, 0), 8, 1)[:FH, :FW].astype(np.float64)
    if v.shape[0] < FH or v.shape[1] < FW:
        v = np.pad(v, ((0, FH - v.shape[0]), (0, FW - v.shape[1]), (0, 0)), mode="edge")
    yy, xx = np.mgrid[0:FH, 0:FW].astype(np.float64)
    qx, qy = np.clip(xx + v[..., 0], 0, FW - 1), np.clip(yy + v[..., 1], 0, FH - 1)
    prev = cv2.remap(la.astype(np.float32) / 255, qx.astype(np.float32), qy.astype(np.float32), cv2.INTER_LINEAR)
    err = np.abs(lb / 255.0 - prev)
    s = np.clip((err - 0.03) / 0.12, 0, 1)
    trust = 1 - s * s * (3 - 2 * s)
    trust = np.round(trust * 255) / 255
    return v * trust[..., None], trust


def clean(img, maskbits):
    """The overlay pixels (grown mask) filled from their surround (iterated neighbour mean): the flow's input."""
    m = (maskbits & 2) > 0
    out = img.astype(np.float64).copy()
    known = ~m
    for _ in range(12):
        if known.all():
            break
        s = np.zeros_like(out); c = np.zeros(out.shape[:2])
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                if dx == 0 and dy == 0:
                    continue
                kk = np.roll(np.roll(known, dy, 0), dx, 1)
                s += np.roll(np.roll(out, dy, 0), dx, 1) * kk[..., None]; c += kk
        fill = (~known) & (c > 0)
        out[fill] = s[fill] / c[fill][..., None]
        known = known | fill
    return out


def words(img):
    a = np.empty((H, W, 4), np.uint8); a[..., :3] = u8(img); a[..., 3] = 255
    return a.reshape(-1).view(np.uint32)


def run_cli(cli, A, B, field, t, rules, mask, tmp):
    stored, trust = field
    job = Path(tmp) / "job.bin"; out = Path(tmp) / "out.rgba"
    with open(job, "wb") as f:
        f.write(np.array([W, H, FW, FH, 0], np.uint32).tobytes())
        f.write(np.array([t], np.float32).tobytes()); f.write(np.array([0], np.uint32).tobytes())
        f.write(words(A).tobytes()); f.write(words(B).tobytes())
        f.write(np.ascontiguousarray(stored[..., 0], np.float32).tobytes())
        f.write(np.ascontiguousarray(stored[..., 1], np.float32).tobytes())
        f.write(np.ascontiguousarray(trust, np.float32).tobytes())
    env = dict(os.environ)
    env.pop("PUSHWARP_OVERLAY_MASK", None)
    if rules:
        env["PUSHWARP_OVERLAY_RULES"] = str(rules)
        if mask is not None:
            p = Path(tmp) / "mask.bin"; mask.astype(np.uint8).tofile(p); env["PUSHWARP_OVERLAY_MASK"] = str(p)
    subprocess.run([str(cli), str(job), str(out)], check=True, env=env)
    data = np.fromfile(out, np.uint8)
    raw = data[: (W * H + 7) * 4].view(np.uint32)
    whole = int(raw[W * H + 4])
    img = raw[: W * H].view(np.uint8).reshape(H, W, 4)[..., :3].astype(np.float64)
    if whole:
        img = (B if t >= 0.5 else A).astype(np.float64)
    used = data[(W * H + 7) * 4:].reshape(H, W) if rules else None
    return img, used


VARIANTS = (("before", 0, "raw", False), ("override", 1, "raw", False), ("+sources", 3, "raw", False),
            ("+clean flow", 3, "clean", False), ("ideal mask", 3, "clean", True))


def scene(cli, kind, motion, tmp):
    alpha, colour = overlay(kind)
    t0 = 4.0
    A, B = u8(frame(motion, t0, alpha, colour)), u8(frame(motion, t0 + 1, alpha, colour))
    An, Bn = u8(frame(motion, t0, None, None)), u8(frame(motion, t0 + 1, None, None))
    ideal_core = alpha >= 0.5
    ideal = (alpha > 0.02).astype(np.uint8) | ((cv2.dilate((alpha > 0.02).astype(np.uint8), np.ones((5, 5), np.uint8)) > 0).astype(np.uint8) << 1)
    raw_field = field_of(A, B)
    _, mask = run_cli(cli, A, B, raw_field, 0.5, 3, None, tmp)
    recall = float(((mask & 1) > 0)[ideal_core].mean()) if ideal_core.any() else float("nan")
    false_share = float(((mask & 1) > 0)[~(alpha > 0.02)].mean())
    fields = {"raw": raw_field, "clean": field_of(u8(clean(A, mask)), u8(clean(B, mask)))}
    fields_ideal = field_of(u8(clean(A, ideal)), u8(clean(B, ideal)))
    field_noov = field_of(An, Bn)
    ys, xs = np.nonzero(alpha > 0.02)
    box = np.zeros((H, W), bool)
    box[max(0, ys.min() - 96): ys.max() + 97, max(0, xs.min() - 96): xs.max() + 97] = True
    ring = box & ~((ideal & 2) > 0)
    rows = []
    for t in (0.5, 1 / 3, 2 / 3):
        truth = frame(motion, t0 + t, alpha, colour); truth_n = frame(motion, t0 + t, None, None)
        gen_n, _ = run_cli(cli, An, Bn, field_noov, t, 0, None, tmp)
        base_err = np.abs(gen_n - truth_n).mean(-1)
        for name, rules, fk, use_ideal in VARIANTS:
            field = fields_ideal if use_ideal else fields[fk]
            gen, _ = run_cli(cli, A, B, field, t, rules, ideal if use_ideal else None, tmp)
            err = np.abs(gen - truth).mean(-1)
            added = np.maximum(0.0, err - base_err)
            rows.append((t, name, float(err[ideal_core].mean()), float(added[ring].mean()), int((added[ring] > 16).sum()), float(err[~box].mean())))
    return recall, false_share, rows



# ---- Damage cases (review of HUD Guard, S12_HUD_REVIEW1 MUST 1 / SHOULD 1-4; orchestrator 25 Nov 2026) -------------------
# What the rules must never do: make a picture worse than the same generator without them. Every scene below is rendered
# at real-frame times 4 and 5 (A, B) and at 4 + t (the truth); PushWarp runs on the same field without rules and with
# rules 1 + 2 (which also let a still pixel under a moving field be interpolated when its two moved samples agree:
# PushWarp.h pushWarpStillHolds). Damage = per pixel max(0, error with rules - error without), against the truth, grey
# levels (RGB mean; HDR10: PQ codes scaled to 0..255). "worse px" / "better px" = pixels made worse / better by more than 8.
# Verdicts: "pass" = at most 64 pixels worse and a mean damage below 0.005 (rounding noise); "trade" = better on the whole
# (mean change below 0) with at most half as many pixels worse as better: reported, not hidden; "DAMAGE" otherwise.
# Thin objects ("a_" scenes): the share of the object's pixels at t wrong by more than 32, without -> with the rules.

def _noise_bg(shift, contrast=1.0):
    g = background(shift)
    return 128.0 + (g - 128.0) * contrast


def _cover(x0, width):
    """Anti-aliased coverage of the columns [x0, x0 + width) at every pixel column (box filter)."""
    xs = np.arange(W, dtype=np.float64)
    return np.clip(np.minimum(xs + 1, x0 + width) - np.maximum(xs, x0), 0, 1)


_GRAIN = np.random.default_rng(7).integers(-1, 2, size=(H, W, 1)).astype(np.float64)   # +-1 code of grain, the same in every frame


def _thin_object(speed, width, plain=False):
    """Still camera; a thin object (dark over texture, bright over a plain grey with +-1 code of grain) moving `speed` px a frame."""
    def f(t):
        bg = 120.0 + _GRAIN + np.zeros((H, W, 3)) if plain else _noise_bg((0.0, 0.0))
        c = _thin_cover(speed, width, t)
        return bg * (1 - c[..., None]) + (230.0 if plain else 40.0) * c[..., None]
    return f


def _thin_cover(speed, width, t):
    return _cover(700.0 + speed * t, width)[None, :] * ((np.arange(H) >= 300) & (np.arange(H) < 780))[:, None]


def _trunks(t):
    ys = np.arange(H, dtype=np.float64)[:, None]
    sky = 150.0 + 50.0 * ys / H + (_noise_bg((6.0 * t, 0.0))[..., 1] - 128.0) * 0.15
    img = np.repeat(sky[..., None], 3, -1) * np.array([0.85, 0.95, 1.1])
    cov = np.zeros(W)
    for k in range(-2, W // 40 + 3):
        cov = np.maximum(cov, _cover(k * 40.0 + 24.0 * t, 6.0))
    c = cov[None, :] * (ys >= 360)
    return img * (1 - c[..., None]) + np.array([60.0, 48.0, 36.0]) * c[..., None]


def _low_contrast(t):
    return _noise_bg((24.0 * t, 0.0), 0.1)


def _periodic(kind):
    def f(t):
        if kind == "fence":   # vertical bars 4 px every 16 px over a moving background, all panning one period (16 px)
            period, shift = 16.0, 16.0 * t
            cov = sum(_cover(k * period + shift, 4.0) for k in range(-2, int(W / period) + 3))[None, :] * np.ones((H, 1))
        elif kind == "grille":   # 2-px lines every 12 px both ways, panning 12 px
            period, shift = 12.0, 12.0 * t
            colsv = sum(_cover(k * period + shift, 2.0) for k in range(-2, int(W / period) + 3))
            ys = np.arange(H, dtype=np.float64)
            rows = np.clip(np.minimum(ys + 1, 0) * 0 + ((ys % period) < 2), 0, 1)
            cov = np.maximum(colsv[None, :], rows[:, None])
        else:   # bricks 32 x 16 with 2-px mortar, rows offset by half a brick, panning 32 px
            period, shift = 32.0, 32.0 * t
            ys = np.arange(H)
            cov = np.zeros((H, W))
            for r in range(0, H, 16):
                off = shift + (16.0 if (r // 16) % 2 else 0.0)
                line = sum(_cover(k * period + off, 2.0) for k in range(-2, int(W / period) + 3))
                cov[r + 2: r + 16] = line[None, :]
                cov[r: r + 2] = 1.0
            brick = 150.0 + 20.0 * np.sin(np.arange(W)[None, :] * 0 + ys[:, None] * 0)
            bg = np.repeat(brick[..., None], 3, -1) * np.array([1.0, 0.6, 0.45])
            return bg * (1 - cov[..., None]) + 200.0 * cov[..., None]
        bg = _noise_bg((period * t, 0.0))
        band = np.zeros((H, 1)); band[200:880] = 1.0
        cov = cov * band
        return bg * (1 - cov[..., None]) + np.array([90.0, 90.0, 95.0]) * cov[..., None]
    return f


def _bloom(t, at_b):
    """Crosshair (2 px) over the circular move; at B it blooms (gap 5 -> 10 px, arms 22 -> 26, colour to orange)."""
    k = 4
    big = np.zeros((H * k, W * k), np.uint8)
    cx, cy = CENTRE[0] * k, CENTRE[1] * k
    arm, gap = (26 * k, 10 * k) if at_b else (22 * k, 5 * k)
    for (x0, y0, x1, y1) in ((cx - arm, cy, cx - gap, cy), (cx + gap, cy, cx + arm, cy), (cx, cy - arm, cx, cy - gap), (cx, cy + gap, cx, cy + arm)):
        cv2.line(big, (x0, y0), (x1, y1), 255, 2 * k)
    alpha = big.reshape(H, k, W, k).mean(axis=(1, 3)) / 255.0
    colour = np.array([255.0, 170.0, 60.0]) if at_b else np.array([255.0, 255.0, 255.0])
    bg = background(shift_at("circle", t))
    return bg * (1 - alpha[..., None]) + colour[None, None, :] * alpha[..., None]


def _subtitle(t, shown):
    bg = background(shift_at("pan", t))
    if not shown:
        return bg
    k = 4
    big = np.zeros((H * k, W * k), np.uint8)
    edge = big.copy()
    org = (560 * k, 960 * k)
    cv2.putText(edge, "Where were you last night?", org, cv2.FONT_HERSHEY_SIMPLEX, 1.6 * k, 255, 7 * k, cv2.LINE_AA)
    cv2.putText(big, "Where were you last night?", org, cv2.FONT_HERSHEY_SIMPLEX, 1.6 * k, 255, 3 * k, cv2.LINE_AA)
    a_out = edge.reshape(H, k, W, k).mean(axis=(1, 3)) / 255.0
    a_in = big.reshape(H, k, W, k).mean(axis=(1, 3)) / 255.0
    img = bg * (1 - a_out[..., None])
    return img * (1 - a_in[..., None]) + 255.0 * a_in[..., None]


def _letterbox(t):
    img = background((24.0 * t, 12.0 * t))
    img[:132] = 0.0; img[H - 132:] = 0.0
    return img


def _vignette(t):
    ys, xs = np.mgrid[0:H, 0:W].astype(np.float64)
    r = np.hypot((xs - W / 2) / (W / 2), (ys - H / 2) / (H / 2))
    v = np.clip(1.0 - 0.55 * np.clip(r - 0.45, 0, None) ** 1.5, 0, 1)
    return background(shift_at("circle", t)) * v[..., None]


def _crosshair_control(t):
    alpha, colour = overlay("crosshair2")
    return frame("circle", t, alpha, colour)


# name -> (picture at real time t, picture at the truth time t, the nearest real frame's picture for ambiguous scenes)
DAMAGE_SCENES = {
    "a_thin6_still_12": _thin_object(12.0, 6.0),
    "a_thin6_still_20": _thin_object(20.0, 6.0),
    "a_thin6_still_40": _thin_object(40.0, 6.0),
    "a_thin2_still_12": _thin_object(12.0, 2.0),
    "a_plain6_still_12": _thin_object(12.0, 6.0, True),
    "a_plain6_still_20": _thin_object(20.0, 6.0, True),
    "a_plain6_still_40": _thin_object(40.0, 6.0, True),
    "b_trunks_sky_pan": _trunks,
    "c_lowcontrast_pan": _low_contrast,
    "d_fence_one_period": _periodic("fence"),
    "d_grille_one_period": _periodic("grille"),
    "d_bricks_one_period": _periodic("bricks"),
    "e_crosshair_bloom": None,      # A plain, B bloomed: the truth is the nearer real frame's crosshair
    "g_subtitle_appears": None,     # absent in A, shown in B: the truth is the nearer real frame's state
    "h_letterbox_diag_pan": _letterbox,
    "h_vignette_circle": _vignette,
    "control_crosshair2_circle": _crosshair_control,
}


def _picture(name, t, t0):
    """The picture at real time t (t0 = A's time); for the A/B-change scenes the state switches at t0 + 1/2."""
    fn = DAMAGE_SCENES[name]
    if name == "e_crosshair_bloom":
        return _bloom(t, t >= t0 + 0.5)
    if name == "g_subtitle_appears":
        return _subtitle(t, t >= t0 + 0.5)
    return fn(t)


def _to_words(img, ten):
    if not ten:
        return words(img)
    import hdr_eval as HE  # noqa: E402
    return HE.words10(HE.to_hdr_codes(u8(img)))


def _as_grey(img, ten):
    """The picture in the scale the damage is measured in: 8-bit grey levels, or PQ codes scaled to 0..255."""
    if not ten:
        return u8(img).astype(np.float64)
    import hdr_eval as HE  # noqa: E402
    return HE.to_hdr_codes(u8(img)).astype(np.float64) * 255.0 / 1023.0


def _run(cli, A, B, field, t, rules, ten, tmp):
    stored, trust = field
    job = Path(tmp) / "job.bin"; out = Path(tmp) / "out.rgba"
    with open(job, "wb") as f:
        f.write(np.array([W, H, FW, FH, 0], np.uint32).tobytes())
        f.write(np.array([t], np.float32).tobytes()); f.write(np.array([0], np.uint32).tobytes())
        f.write(_to_words(A, ten).tobytes()); f.write(_to_words(B, ten).tobytes())
        f.write(np.ascontiguousarray(stored[..., 0], np.float32).tobytes())
        f.write(np.ascontiguousarray(stored[..., 1], np.float32).tobytes())
        f.write(np.ascontiguousarray(trust, np.float32).tobytes())
    env = dict(os.environ)
    env.pop("PUSHWARP_OVERLAY_MASK", None)
    env.pop("PUSHWARP_OVERLAY_RULES", None)
    if ten:
        env["PUSHWARP_TEN_BIT"] = "1"
    else:
        env.pop("PUSHWARP_TEN_BIT", None)
    if rules:
        env["PUSHWARP_OVERLAY_RULES"] = str(rules)
    subprocess.run([str(cli), str(job), str(out)], check=True, env=env)
    data = np.fromfile(out, np.uint8)
    raw = data[: (W * H + 7) * 4].view(np.uint32)
    whole = int(raw[W * H + 4])
    px = raw[: W * H]
    if ten:
        img = np.stack([px & 1023, (px >> 10) & 1023, (px >> 20) & 1023], -1).reshape(H, W, 3).astype(np.float64) * 255.0 / 1023.0
    else:
        img = px.view(np.uint8).reshape(H, W, 4)[..., :3].astype(np.float64)
    if whole:
        img = _as_grey(B if t >= 0.5 else A, ten)
    mask = data[(W * H + 7) * 4:].reshape(H, W) if rules else None
    return img, mask


def damage_scene(cli, name, ten, tmp):
    t0 = 4.0
    A, B = u8(_picture(name, t0, t0)), u8(_picture(name, t0 + 1, t0))
    if ten:   # the field from the PQ codes reduced to 8 bits, as hdr_eval.py
        fa, fb = _as_grey(A, True), _as_grey(B, True)
        field = field_of(u8(fa), u8(fb))
    else:
        field = field_of(A, B)
    rows = []
    for t in (1 / 3, 2 / 3):
        truth = _as_grey(_picture(name, t0 + t, t0), ten)
        off, _ = _run(cli, A, B, field, t, 0, ten, tmp)
        on, mask = _run(cli, A, B, field, t, 3, ten, tmp)
        e_off = np.abs(off - truth).mean(-1); e_on = np.abs(on - truth).mean(-1)
        diff = e_on - e_off
        obj_off = obj_on = float("nan")
        if name.startswith("a_"):   # the thin object's pixels at t: the share wrong by more than 32 grey levels
            speed, width = float(name.rsplit("_", 1)[1]), float(name.split("_")[1][-1])
            obj = _thin_cover(speed, width, t0 + t) > 0.5
            obj_off, obj_on = float((e_off[obj] > 32).mean()), float((e_on[obj] > 32).mean())
        rows.append(dict(obj_off=obj_off, obj_on=obj_on, t=t, mask=int(((mask & 1) > 0).sum()), changed=int((np.abs(on - off).max(-1) > 0.5).sum()),
                         worse=int((diff > 8).sum()), better=int((diff < -8).sum()), damage=float(np.maximum(diff, 0).mean()),
                         net=float(diff.mean()), err_off=float(e_off.mean())))
    return rows


def _obj(r):
    return "" if math.isnan(r["obj_off"]) else f"{100 * r['obj_off']:.1f} -> {100 * r['obj_on']:.1f} %"


def damage_main(cli, names, hdr):
    print("OFFLINE, synthetic, CPU; FidelityFX-like field model; PushWarp CPU reference with rules 1 + 2 vs without. "
          "Grey levels (HDR10: PQ codes scaled to 0..255).\n")
    print("| scene | signal | frame | overlay px | changed px | worse px (> 8) | better px (> 8) | damage (mean) | net change (mean) | error without rules | object wrong off -> on | verdict |")
    print("|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|")
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name in names:
            for ten in ((False, True) if hdr else (False,)):
                for r in damage_scene(cli, name, ten, tmp):
                    ok = r["worse"] <= 64 and r["damage"] < 0.005
                    trade = not ok and r["net"] < 0 and 2 * r["worse"] <= r["better"]
                    failed += 0 if ok or trade else 1
                    print(f"| {name} | {'HDR10' if ten else 'SDR'} | 3X {'1/3' if r['t'] < 0.5 else '2/3'} | {r['mask']} | {r['changed']} | {r['worse']} | {r['better']} | "
                          f"{r['damage']:.4f} | {r['net']:+.4f} | {r['err_off']:.3f} | {_obj(r)} | {'pass' if ok else 'trade' if trade else 'DAMAGE'} |", flush=True)
    return failed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", required=True, type=Path)
    ap.add_argument("--scene", action="append", default=[])
    ap.add_argument("--damage", action="store_true", help="the damage cases (DAMAGE_SCENES) instead of the ghosting table")
    ap.add_argument("--sdr-only", action="store_true", help="damage cases: SDR only (default: SDR and HDR10)")
    a = ap.parse_args()
    np.seterr(over="ignore")
    if a.damage:
        sys.exit(1 if damage_main(a.cli, a.scene or list(DAMAGE_SCENES), not a.sdr_only) else 0)
    scenes = a.scene or [f"{k}_{m}" for k in ("crosshair1", "crosshair2", "crosshair3", "crosshair2_translucent", "text", "minimap") for m in ("pan", "circle")]
    print("OFFLINE, synthetic, CPU; FidelityFX-like field model; PushWarp CPU reference (pushWarpReference). Grey levels.\n")
    print("| scene | mask recall / false | frame | variant | overlay error | ghost energy | ghost pixels | background |")
    print("|---|---|---|---|---:|---:|---:|---:|")
    with tempfile.TemporaryDirectory() as tmp:
        for sc in scenes:
            kind, motion = sc.rsplit("_", 1)
            recall, false_share, rows = scene(a.cli, kind, motion, tmp)
            for t, name, oe, ge, gp, bg in rows:
                label = "2X 1/2" if abs(t - 0.5) < 1e-6 else ("3X 1/3" if t < 0.5 else "3X 2/3")
                print(f"| {sc} | {recall:.2f} / {false_share:.4f} | {label} | {name} | {oe:.1f} | {ge:.2f} | {gp} | {bg:.3f} |", flush=True)


if __name__ == "__main__":
    main()
