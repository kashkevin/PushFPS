#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Re-measures the IMPLEMENTED PushWarp (review 1 of stream ownfg, SHOULD 7) on the offline study's synthetic 4K scenes.

OFFLINE PICTURE MEASUREMENT ON SYNTHETIC MATERIAL, CPU ONLY: nothing about speed, nothing about any graphics card, not
game footage. Uses the study's harness (tools/ownfg_eval on branch exp/s9-ownfg, passed with --harness) for the scenes,
the renderer, the FidelityFX-like 8x8 block field and the metrics, and pushwarp_cli (built from pushwarp_cli.cpp against
this branch's include/veyra/pipeline/PushWarp.h) for the frames: exactly the rules the GPU passes are checked against.

The field is the app's FidelityFX path as the study modelled it: one direction (B -> A), 8x8 integer blocks at
960x540, photometric trust (NvofDensify.hlsl's rule), stored as v x trust; PushWarp then applies its own 4x4-cell SAD
trust, splat, fill, compose and whole-frame verdict. P uses acceleration from the previous pair's field (Z -> A).
The learned reference is the study's RIFE v4.26 ("rife_quality") I(2/3) of the same pair, read from its results.

  g++ -std=c++20 -O2 -Iinclude tools/pushwarp_eval/pushwarp_cli.cpp -o $BIN
  python3 tools/pushwarp_eval/measure.py --harness <s9>/tools/ownfg_eval --cli $BIN --out $OUT --study $STUDY --sequence drive
  python3 tools/pushwarp_eval/measure.py --harness ... --out $OUT --study $STUDY --report
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np


def rgba_words(rgb):
    h, w = rgb.shape[:2]
    a = np.empty((h, w, 4), np.uint8); a[..., :3] = rgb; a[..., 3] = 255
    return a.reshape(-1).view(np.uint32)


def run_cli(cli, A, B, field, previous, t, accel, tmp):
    h, w = B.shape[:2]
    gh, gw = field.trust.shape
    job = Path(tmp) / "job.bin"; out = Path(tmp) / "out.rgba"
    with open(job, "wb") as f:
        f.write(np.array([w, h, gw, gh, 1 if previous is not None else 0], np.uint32).tobytes())
        f.write(np.array([t], np.float32).tobytes()); f.write(np.array([1 if accel else 0], np.uint32).tobytes())
        f.write(rgba_words(A).tobytes()); f.write(rgba_words(B).tobytes())
        for fl in [field] + ([previous] if previous is not None else []):
            f.write(np.ascontiguousarray(fl.stored[..., 0], np.float32).tobytes())
            f.write(np.ascontiguousarray(fl.stored[..., 1], np.float32).tobytes())
            f.write(np.ascontiguousarray(fl.trust, np.float32).tobytes())
    subprocess.run([str(cli), str(job), str(out)], check=True)
    raw = np.fromfile(out, np.uint32)
    words, counters = raw[: w * h], raw[w * h:]
    rgb = words.view(np.uint8).reshape(h, w, 4)[..., :3].astype(np.float32)
    still, blend, conflict, hole, whole = (int(x) for x in counters[:5])   # then cutOffCells, trustedCells
    if whole:   # the GPU's predicated copy of the nearer real frame (PushWarpVerdict.hlsl)
        rgb = (B if t >= 0.5 else A).astype(np.float32)
    moving = blend + conflict + hole
    return rgb, {"hole_share": hole / moving if moving else 0.0, "whole_frame": bool(whole)}


def eval_pair(H, cli, cache, t0, tmp):
    F, M = H["flow"], H["metrics"]
    third = 1 / 3
    times = {"Z": t0 - 1, "A": t0, "B": t0 + 1, "I2": t0 + 2 * third, "P1": t0 + 1 + third, "P2": t0 + 1 + 2 * third}
    rgb = {k: cache.color(times[k]) for k in ("Z", "A", "B")}
    field = F.ffx_flows(rgb["A"], rgb["B"], 960, 540, "photo")
    previous = F.ffx_flows(rgb["Z"], rgb["A"], 960, 540, "photo")
    res = {}
    for key, t, prev in (("I2", 2 * third, None), ("P1", 1 + third, previous), ("P2", 1 + 2 * third, previous)):
        truth = cache.color(times[key]).astype(np.float32)
        mo, ids = cache.motion(times[key], times[key] + third)
        mo = np.nan_to_num(mo)
        band = M.silhouette_band(ids, 3 * mo)
        speed = 3 * np.hypot(mo[..., 0], mo[..., 1])
        start = time.time()
        out, extra = run_cli(cli, rgb["A"], rgb["B"], field, prev, t, prev is not None, tmp)
        r = M.measure(out, truth, band, speed)
        r.update(extra); r["cpu_s"] = round(time.time() - start, 2)
        res[f"{key}/pushwarp"] = r
        if key != "I2":   # without acceleration too: what the previous field adds
            out, extra = run_cli(cli, rgb["A"], rgb["B"], field, None, t, False, tmp)
            r = M.measure(out, truth, band, speed); r.update(extra)
            res[f"{key}/pushwarp_noacc"] = r
    return res


def report(out: Path, study: Path, tag: str = "pushwarp"):
    rows = {}
    for p in sorted((out / f"results-{tag}").glob("*.json")):
        d = json.loads(p.read_text())
        s = json.loads((study / "results" / p.name).read_text()) if (study / "results" / p.name).exists() else {"pairs": {}}
        for k, pr in d["pairs"].items():
            ref = s["pairs"].get(k, {})
            for name in ("I2/pushwarp", "P1/pushwarp", "P2/pushwarp", "P1/pushwarp_noacc", "P2/pushwarp_noacc"):
                rows.setdefault(name, []).append((d["scene"], k, pr[name], ref.get("I2/rife_quality"), ref.get("I2/rife_fast")))
    lines = ["OFFLINE, synthetic 4K, CPU; the implemented PushWarp rules (pushWarpReference) on the FidelityFX-like block field.",
             "", "| frame | pairs | PSNR mean | PSNR worst 5 % | RIFE quality I(2/3) mean, same pairs | difference | SSIM | band PSNR | misplacement P95 (px) | held share | whole frame held |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    per_seq = {}
    for name, v in rows.items():
        ps = [x[2]["psnr"] for x in v]; rq = [x[3]["psnr"] for x in v if x[3]]
        lines.append(f"| {name} | {len(v)} | {np.mean(ps):.2f} | {np.percentile(ps, 5):.2f} | {np.mean(rq) if rq else float('nan'):.2f} | "
                     f"{np.mean(ps) - np.mean(rq) if rq else float('nan'):+.2f} | {np.mean([x[2]['ssim'] for x in v]):.4f} | "
                     f"{np.mean([x[2]['edge_psnr'] for x in v]):.2f} | {np.mean([x[2]['disp_p95'] for x in v]):.1f} | "
                     f"{np.mean([x[2]['hole_share'] for x in v]):.3f} | {sum(x[2]['whole_frame'] for x in v)} |")
        for sc, k, r, q, _ in v:
            per_seq.setdefault((name, sc), []).append((r["psnr"], q["psnr"] if q else float("nan")))
    lines += ["", "Per sequence (PSNR mean: PushWarp / RIFE quality I(2/3), same pairs):", ""]
    lines += ["| frame | sequence | pairs | PushWarp | RIFE quality | difference |", "|---|---|---:|---:|---:|---:|"]
    for (name, sc), v in sorted(per_seq.items()):
        a = np.mean([x[0] for x in v]); b = np.mean([x[1] for x in v])
        lines.append(f"| {name} | {sc} | {len(v)} | {a:.2f} | {b:.2f} | {a - b:+.2f} |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True, type=Path)
    ap.add_argument("--cli", type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--study", required=True, type=Path, help="the study's output folder (its results/*.json hold RIFE's numbers)")
    ap.add_argument("--sequence", action="append", default=[])
    ap.add_argument("--pairs", type=float, nargs="*")
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--tag", default="pushwarp", help="results folder suffix (a variant: PUSHWARP_ACCEL_SHARE set for the CLI)")
    a = ap.parse_args()
    sys.path.insert(0, str(a.harness.resolve()))
    import flow, metrics, run_eval, scenes  # noqa: E401  (the study's harness)
    H = {"flow": flow, "metrics": metrics}
    for name in a.sequence:
        sc = scenes.scene_by_name(name)
        cache = run_eval.Cache(a.out, sc, run_eval.renderer)
        path = a.out / f"results-{a.tag}" / f"{name}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        done = json.loads(path.read_text()) if path.exists() else {"scene": name, "pairs": {}}
        for t0 in (a.pairs or sc.pairs):
            k = f"{t0:g}"
            if k in done["pairs"]:
                continue
            start = time.time()
            with tempfile.TemporaryDirectory() as tmp:
                done["pairs"][k] = eval_pair(H, a.cli, cache, t0, tmp)
            path.write_text(json.dumps(done, indent=1))
            print(f"{name} pair {k}: {time.time() - start:.0f} s", flush=True)
    if a.report:
        print(report(a.out, a.study, a.tag))


if __name__ == "__main__":
    main()
