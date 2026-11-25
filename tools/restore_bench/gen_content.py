#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Pristine 1920x1080 test content for tools/restore_bench (docs/RESTORATION.md).

MODEL CONTENT, drawn by this script: it is not game footage. Per frame it writes
  ref_<n>.rgb48   the ideal picture, 16 bits per component (sRGB code values, little endian)
  src_<n>.rgb24   what a console would send to its encoder: the same picture in 8 bits with a
                  one-code random dither (as a renderer's output dither)
and once  mask.gray  region labels per pixel (see REGIONS) and  layout.json.
The scene: a sky gradient with soft clouds and thin wires, a dark room with a dim gradient, three
procedural textures (foliage-like noise, brick, fabric) and a HUD (text of several sizes with and
without a panel, bars, a minimap ring, a crosshair). Sky, clouds, wires and textures move a few
pixels per frame; the HUD stays where it is, as in a game.
"""
import argparse
import json
import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

W, H = 1920, 1080
REGIONS = {"other": 0, "gradient": 1, "hud": 2, "texture": 3, "lines": 4, "dark": 5}
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def value_noise(rng, w, h, cell):
    """Smooth noise in 0..1: a coarse random grid enlarged with a bicubic filter."""
    gw, gh = w // cell + 3, h // cell + 3
    grid = (rng.random((gh, gw)) * 255).astype(np.uint8)
    big = Image.fromarray(grid).resize((gw * cell, gh * cell), Image.BICUBIC)
    return np.asarray(big, dtype=np.float32)[:h, :w] / 255.0


def fractal(rng, w, h, cells):
    out = np.zeros((h, w), np.float32)
    weight = 0.0
    for i, cell in enumerate(cells):
        a = 0.5 ** i
        out += a * value_noise(rng, w, h, cell)
        weight += a
    return out / weight


def shifted(field, dx, dy):
    return np.roll(np.roll(field, dy, axis=0), dx, axis=1)


def build_static(seed):
    rng = np.random.default_rng(seed)
    wide = W + 256
    s = {}
    s["clouds"] = fractal(rng, wide, 400, (160, 80, 40))
    s["foliage"] = fractal(rng, wide, 300, (24, 12, 6, 3, 2))
    s["foliage_fine"] = rng.random((300, wide)).astype(np.float32)
    s["fabric_noise"] = fractal(rng, wide, 420, (48, 8))
    s["brick_noise"] = fractal(rng, wide, 420, (32, 4, 2))
    s["dark"] = fractal(rng, wide, 300, (200, 100))
    return s


def hud_layer():
    """The HUD as an RGBA picture (straight alpha) and its mask."""
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    fonts = {n: ImageFont.truetype(FONT, n) for n in (11, 14, 20, 32)}
    # Text on a translucent panel (mission text), over the textures.
    d.rectangle((60, 800, 700, 1020), fill=(10, 12, 16, 170))
    d.text((80, 815), "OBJECTIVE  Reach the marina before 21:40", font=fonts[20], fill=(255, 255, 255, 255))
    d.text((80, 850), "Cash $1,284,650   Wanted level 2   Ammo 24 / 180", font=fonts[14], fill=(250, 220, 90, 255))
    d.text((80, 878), "Press E to enter vehicle. Hold TAB for the weapon wheel. Map: M", font=fonts[11], fill=(220, 225, 230, 255))
    d.text((80, 905), "MISSION PASSED", font=fonts[32], fill=(255, 210, 60, 255))
    for i, (frac, colour) in enumerate(((0.8, (90, 200, 90, 255)), (0.45, (70, 150, 230, 255)), (0.2, (230, 80, 70, 255)))):
        y = 965 + i * 16
        d.rectangle((80, y, 380, y + 8), outline=(230, 230, 230, 255), width=1)
        d.rectangle((82, y + 2, 82 + int(296 * frac), y + 6), fill=colour)
    # Text with no panel: straight over the sky and over the textures.
    d.text((1380, 40), "12:47  Vespucci Beach", font=fonts[20], fill=(255, 255, 255, 255))
    d.text((1380, 70), "FPS 60  PING 9 ms  1080p", font=fonts[11], fill=(255, 255, 255, 255))
    d.text((760, 700), "Subtitles: You never listen to me, do you?", font=fonts[20], fill=(255, 255, 255, 255))
    d.text((760, 730), "small print over a texture 0123456789", font=fonts[11], fill=(240, 240, 240, 255))
    d.text((760, 750), "dark text over a texture", font=fonts[14], fill=(15, 15, 20, 255))
    # Minimap: a ring, a route and a marker.
    d.ellipse((1640, 800, 1860, 1020), fill=(20, 26, 30, 150), outline=(240, 240, 240, 255), width=2)
    d.line((1680, 980, 1730, 900, 1790, 880, 1830, 840), fill=(190, 90, 220, 255), width=3)
    d.polygon(((1750, 905), (1742, 925), (1758, 925)), fill=(255, 255, 255, 255))
    # Crosshair and one-pixel lines.
    d.line((950, 540, 970, 540), fill=(255, 255, 255, 255), width=1)
    d.line((960, 530, 960, 550), fill=(255, 255, 255, 255), width=1)
    d.rectangle((1000, 620, 1300, 660), outline=(255, 255, 255, 200), width=1)
    rgba = np.asarray(img, dtype=np.float32) / 255.0
    return rgba


def frame(static, hud, index):
    dx, dy = 3 * index, index
    pic = np.zeros((H, W, 3), np.float32)
    mask = np.zeros((H, W), np.uint8)
    yy = np.linspace(0.0, 1.0, 360, dtype=np.float32)[:, None]
    xx = np.linspace(0.0, 1.0, W, dtype=np.float32)[None, :]
    # Sky: a slow gradient (about one 8-bit code per 6 lines) and soft clouds; thin wires across it.
    sky = np.stack([0.34 + 0.22 * yy + 0.03 * xx, 0.50 + 0.14 * yy + 0.02 * xx, 0.78 - 0.10 * yy + 0.0 * xx], axis=2)
    cloud = shifted(static["clouds"], dx, 0)[:360, :W]
    sky = sky + (np.clip(cloud - 0.55, 0, 1) * 0.5)[:, :, None]
    pic[:360] = sky
    mask[:360] = REGIONS["gradient"]
    wires = Image.new("L", (W, 360), 0)
    wd = ImageDraw.Draw(wires)
    for k in range(4):
        y0 = 60 + 50 * k
        wd.line(((0 + dx * 2) % 200 - 200, y0, W, y0 + 90), fill=255, width=1)
        wd.line((0, y0 + 20, W, y0 + 20 - 30 * k), fill=255, width=1)
    wire = np.asarray(wires, dtype=np.float32) / 255.0
    pic[:360] = pic[:360] * (1 - 0.85 * wire[:, :, None])
    mask[:360][wire > 0.5] = REGIONS["lines"]
    # Dark room: 2..30 codes with a dim gradient and a faint light cone.
    dark = shifted(static["dark"], dx // 3, 0)[:300, :960]
    y2 = np.linspace(0.0, 1.0, 300, dtype=np.float32)[:, None]
    x2 = np.linspace(0.0, 1.0, 960, dtype=np.float32)[None, :]
    lum = (4.0 + 26.0 * np.clip(1.2 - np.hypot(x2 - 0.3, (y2 - 0.4) * 0.6) * 1.6, 0, 1) ** 2 + 6.0 * dark) / 255.0
    pic[360:660, :960] = np.stack([lum * 1.05, lum, lum * 0.9], axis=2)
    mask[360:660, :960] = REGIONS["dark"]
    # Foliage-like texture.
    fol = shifted(static["foliage"], dx, dy)[:300, :960]
    fine = shifted(static["foliage_fine"], dx, dy)[:300, :960]
    g = 0.18 + 0.5 * fol + 0.08 * (fine - 0.5)
    pic[360:660, 960:] = np.stack([g * 0.55, g, g * 0.35], axis=2)
    mask[360:660, 960:] = REGIONS["texture"]
    # Brick: courses of 28 x 64 with mortar lines, moving sideways.
    by, bx = np.mgrid[0:420, 0:960]
    bx = bx + dx
    course = by // 28
    offs = np.where(course % 2 == 0, 0, 32)
    mortar = ((by % 28) < 3) | (((bx + offs) % 64) < 3)
    bn = shifted(static["brick_noise"], dx, 0)[:420, :960]
    brick = np.where(mortar, 0.62, 0.42 + 0.16 * bn)
    pic[660:, :960] = np.stack([brick * 1.1, brick * 0.72, brick * 0.6], axis=2)
    mask[660:, :960] = REGIONS["texture"]
    # Fabric: a fine weave (2 and 3 pixel periods) with soft folds.
    fy, fx = np.mgrid[0:420, 0:960]
    fx = fx + dx
    fy = fy + dy
    fn = shifted(static["fabric_noise"], dx, dy)[:420, :960]
    weave = 0.5 + 0.06 * np.sin(fx * np.pi * 2 / 3) * np.sin(fy * np.pi) + 0.25 * (fn - 0.5)
    pic[660:, 960:] = np.stack([weave * 0.5, weave * 0.6, weave * 0.95], axis=2)
    mask[660:, 960:] = REGIONS["texture"]
    # HUD on top, never moving.
    a = hud[:, :, 3:4]
    pic = pic * (1 - a) + hud[:, :, :3] * a
    mask[hud[:, :, 3] > 0.02] = REGIONS["hud"]
    return np.clip(pic, 0.0, 1.0), mask


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--frames", type=int, default=6)
    ap.add_argument("--seed", type=int, default=20261120)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    static = build_static(args.seed)
    hud = hud_layer()
    rng = np.random.default_rng(args.seed + 1)
    mask = None
    for n in range(args.frames):
        pic, mask = frame(static, hud, n)
        (np.round(pic * 65535.0)).astype("<u2").tofile(os.path.join(args.out, f"ref_{n:03d}.rgb48"))
        dither = rng.random(pic.shape, dtype=np.float32) - 0.5
        src = np.clip(np.floor(pic * 255.0 + 0.5 + dither), 0, 255).astype(np.uint8)
        src[pic <= 0.0] = 0
        src.tofile(os.path.join(args.out, f"src_{n:03d}.rgb24"))
    mask.tofile(os.path.join(args.out, "mask.gray"))
    with open(os.path.join(args.out, "layout.json"), "w", encoding="utf-8") as f:
        json.dump({"width": W, "height": H, "frames": args.frames, "seed": args.seed, "regions": REGIONS,
                   "note": "model content drawn by gen_content.py; not game footage"}, f, indent=1)
    print(f"wrote {args.frames} frames of {W}x{H} to {args.out}")


if __name__ == "__main__":
    main()
