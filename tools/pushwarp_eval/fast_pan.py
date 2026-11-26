#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Fast camera turns (500-900 px per real-frame interval at 3840x2160) and PushWarp's motion cut-off maxMotionShare.

OFFLINE PICTURE MEASUREMENT ON SYNTHETIC MATERIAL, CPU ONLY: nothing about speed, nothing about any graphics card, not
game footage. Scenes and metrics come from the study's harness (tools/ownfg_eval on branch exp/s9-ownfg, --harness);
the field is the harness's FidelityFX-like model (7-level pyramid, +-8 px search per level, 8x8 blocks at 960x540,
photometric trust), NOT AMD's code: whether the real FidelityFX finds the same vectors in a 25 deg/frame turn is not
known from this. The frames are pushwarp_cli's (the implemented rules, pushWarpReference) with
PUSHWARP_MAX_MOTION_SHARE = 1/8 (the GPU's constant), 1/4, 1/2. No acceleration (as in play).

For every pair (A = t0, B = t0 + 1) and share: I(1/3), I(2/3), P(1/3) = B + 1/3, P(2/3) = B + 2/3, each against the
rendered truth: PSNR, PSNR in the silhouette band, the hole share (moving pixels nothing landed on: shown from B for P,
from the zero-motion blend for I), whole-frame nearest (the verdict's predicated copy: a visible repeat), and the share
of trusted flow cells the cut-off removed. The nearest real frame (A for I(1/3), B for the others) is the "held"
baseline: what a judder shows.

  python3 tools/pushwarp_eval/fast_pan.py --harness <s9>/tools/ownfg_eval --cli $BIN --out $OUT [--frames <cache>] [--scene pan_900] [--report]
The study's own scenes (pan_fast, crossing, repeat_facade, ...) are accepted too: the control that a larger cut-off
does not admit wrong long vectors where nothing moves that fast.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

FOCAL_PX = 2075.0           # the harness's camera at 3840x2160 (scenes.py: "focal 2075 px")
SHARES = (0.125, 0.25, 0.5)
FRAMES = (("I1", 1 / 3), ("I2", 2 / 3), ("P1", 4 / 3), ("P2", 5 / 3))


def deg_for_px(px):
    return math.degrees(math.atan(px / FOCAL_PX))


def build(S):
    """The fast scenes (speeds per 30 FPS game frame; pixel speeds at the picture's centre)."""
    rng = np.random.default_rng(20261124)
    city = S.street_static(rng)
    d = math.pi / 180
    out = []
    for px in (500, 700, 900):
        rate = deg_for_px(px)
        out.append(S.Scene(f"pan_{px}", "fast turn", f"standing, turning at {rate:.1f} deg/frame (~{px} px/frame at the centre)",
                           camera=lambda t, r=rate: S.Camera((2.0, 1.7, 0.0), 0.9 - r * d * t, 0.0), static=city, pairs=(2.0, 5.0)))
    rate = deg_for_px(700)
    # A turn while walking forward and sideways along the kerb: near poles, fence and wires move faster than the far
    # facades (parallax inside the turn), the rotation-like case of a third-person camera swinging round.
    out.append(S.Scene("turn_parallax_700", "fast turn + parallax", f"walking 0.5 m/frame while turning {rate:.1f} deg/frame",
                       camera=lambda t, r=rate: S.Camera((2.5 - 0.15 * t, 1.6, 0.5 * t), 0.9 - r * d * t, -0.03), static=city,
                       pairs=(2.0, 5.0)))
    out.append(S.Scene("hud_pan_700", "fast turn + HUD", f"turning {rate:.1f} deg/frame under a static HUD",
                       camera=lambda t, r=rate: S.Camera((1.0, 1.7, 10.0), 0.5 - r * d * t, 0.0), static=city, hud=True,
                       pairs=(2.0, 5.0)))
    return out


def rgba_words(rgb):
    h, w = rgb.shape[:2]
    a = np.empty((h, w, 4), np.uint8); a[..., :3] = rgb; a[..., 3] = 255
    return a.reshape(-1).view(np.uint32)


def run_cli(cli, A, B, field, t, share, tmp):
    h, w = B.shape[:2]
    gh, gw = field.trust.shape
    job = Path(tmp) / "job.bin"; out = Path(tmp) / "out.rgba"
    with open(job, "wb") as f:
        f.write(np.array([w, h, gw, gh, 0], np.uint32).tobytes())
        f.write(np.array([t], np.float32).tobytes()); f.write(np.array([0], np.uint32).tobytes())
        f.write(rgba_words(A).tobytes()); f.write(rgba_words(B).tobytes())
        f.write(np.ascontiguousarray(field.stored[..., 0], np.float32).tobytes())
        f.write(np.ascontiguousarray(field.stored[..., 1], np.float32).tobytes())
        f.write(np.ascontiguousarray(field.trust, np.float32).tobytes())
    env = dict(os.environ, PUSHWARP_MAX_MOTION_SHARE=repr(share))
    subprocess.run([str(cli), str(job), str(out)], check=True, env=env)
    raw = np.fromfile(out, np.uint32)
    words, c = raw[: w * h], [int(x) for x in raw[w * h:]]
    rgb = words.view(np.uint8).reshape(h, w, 4)[..., :3].astype(np.float32)
    still, blend, conflict, hole, whole, cut, trusted = c[:7]
    if whole:
        rgb = (B if t >= 0.5 else A).astype(np.float32)
    moving = blend + conflict + hole
    return rgb, {"hole_share": hole / moving if moving else 0.0, "whole_frame": bool(whole),
                 "cut_off_share": cut / trusted if trusted else 0.0, "trusted_cells": trusted}


def eval_pair(H, cli, cache, t0, tmp):
    F, M = H["flow"], H["metrics"]
    A, B = cache.color(t0), cache.color(t0 + 1)
    field = F.ffx_flows(A, B, 960, 540, "photo")
    res = {}
    for key, t in FRAMES:
        truth = cache.color(t0 + t).astype(np.float32)
        mo, ids = cache.motion(t0 + t, t0 + t + 1 / 3)
        mo = np.nan_to_num(mo)
        band = M.silhouette_band(ids, 3 * mo)
        speed = 3 * np.hypot(mo[..., 0], mo[..., 1])
        held = (A if t < 0.5 else B).astype(np.float32)
        res[f"{key}/held"] = {"psnr": M.psnr(held, truth), "edge_psnr": M.psnr(held, truth, band),
                              "speed_p50": float(np.median(speed)), "speed_p95": float(np.percentile(speed, 95))}
        for share in SHARES:
            out, extra = run_cli(cli, A, B, field, t, share, tmp)
            r = {"psnr": M.psnr(out, truth), "edge_psnr": M.psnr(out, truth, band)}
            r.update(extra)
            res[f"{key}/share{share:g}"] = r
    return res


def report(out: Path):
    rows = {}
    for p in sorted((out / "results-fastpan").glob("*.json")):
        d = json.loads(p.read_text())
        for k, pr in d["pairs"].items():
            for name, r in pr.items():
                rows.setdefault((d["scene"], name), []).append(r)
    lines = ["OFFLINE, synthetic 3840x2160, CPU, the harness's FidelityFX-like field (a model, not AMD's code); "
             "pushWarpReference with maxMotionShare 1/8 (shipped), 1/4, 1/2; no acceleration. 'held' = the nearest real frame.", "",
             "| scene | frame | true speed P50 / P95 (px per interval) | held PSNR | 1/8 PSNR | 1/4 PSNR | 1/2 PSNR | 1/8 hole / cut-off | "
             "1/4 hole / cut-off | 1/2 hole / cut-off | whole frame held 1/8 / 1/4 / 1/2 | band PSNR held / 1/8 / 1/4 / 1/2 |",
             "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    scenes = sorted({s for s, _ in rows})
    for sc in scenes:
        for key, _ in FRAMES:
            h = rows.get((sc, f"{key}/held"))
            if not h:
                continue
            v = {s: rows[(sc, f"{key}/share{s:g}")] for s in SHARES}
            m = lambda xs, f: float(np.mean([x[f] for x in xs]))
            lines.append(f"| {sc} | {key} | {m(h, 'speed_p50'):.0f} / {m(h, 'speed_p95'):.0f} | {m(h, 'psnr'):.2f} | "
                         + " | ".join(f"{m(v[s], 'psnr'):.2f}" for s in SHARES) + " | "
                         + " | ".join(f"{m(v[s], 'hole_share'):.3f} / {m(v[s], 'cut_off_share'):.3f}" for s in SHARES) + " | "
                         + " / ".join(str(sum(x['whole_frame'] for x in v[s])) for s in SHARES) + f" of {len(h)} | "
                         + f"{m(h, 'edge_psnr'):.2f} / " + " / ".join(f"{m(v[s], 'edge_psnr'):.2f}" for s in SHARES) + " |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True, type=Path)
    ap.add_argument("--cli", type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--frames", type=Path, help="the harness's frame cache (renders are reused; default --out)")
    ap.add_argument("--scene", action="append", default=[])
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()
    sys.path.insert(0, str(a.harness.resolve()))
    import flow, metrics, run_eval, scenes as S  # noqa: E401  (the study's harness)
    H = {"flow": flow, "metrics": metrics}
    mine = {s.name: s for s in build(S)}
    for name in a.scene:
        sc = mine.get(name) or S.scene_by_name(name)
        cache = run_eval.Cache(a.frames or a.out, sc, run_eval.renderer)
        path = a.out / "results-fastpan" / f"{name}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        done = json.loads(path.read_text()) if path.exists() else {"scene": name, "pairs": {}}
        for t0 in sc.pairs[:2]:
            k = f"{t0:g}"
            if k in done["pairs"]:
                continue
            start = time.time()
            with tempfile.TemporaryDirectory() as tmp:
                done["pairs"][k] = eval_pair(H, a.cli, cache, t0, tmp)
            path.write_text(json.dumps(done, indent=1))
            print(f"{name} pair {k}: {time.time() - start:.0f} s", flush=True)
    if a.report:
        print(report(a.out))


if __name__ == "__main__":
    main()
