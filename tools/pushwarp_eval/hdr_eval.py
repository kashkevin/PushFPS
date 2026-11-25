#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""HDR10 (PQ code values) picture numbers for the implemented PushWarp, and what blending in linear light would change.

OFFLINE PICTURE MEASUREMENT ON SYNTHETIC MATERIAL, CPU ONLY: nothing about speed, nothing about any graphics card, not
game footage, and not real HDR content. The study's SDR renders (harness: tools/ownfg_eval on exp/s9-ownfg) are turned
into HDR10-like pictures: sRGB decoded to linear, diffuse white at 203 nits, the top quarter of the SDR range expanded
quadratically up to about 4000 nits (highlights: sky, lamps, flashes), PQ (ST 2084) encoded to 10-bit codes. The primaries
stay BT.709 (no BT.2020 conversion): the transfer function is what is under test. The field is the harness's
FidelityFX-like model run on the PQ codes reduced to 8 bits (what a luma-based flow sees of a PQ picture).

Variants (pushwarp_cli, PUSHWARP_TEN_BIT=1):
  codes   the implemented scheme: the GPU samples (bilinear) and blends A and B in PQ code values (PushWarpCompose.hlsl,
          pushWarpReference)
  linear  the same decisions (still, conflict, hole: in codes) and the same filled field, but the bilinear samples and
          the blend in linear light, PQ-encoded at the end (pushwarp_cli PUSHWARP_LINEAR_COMPOSE=1; first checked to
          reproduce the reference exactly in code values, PUSHWARP_LINEAR_COMPOSE=check)
Only interpolated frames are compared (I(1/3), I(2/3)): a predicted frame takes one bilinear sample of B and blends nothing.

Metrics against the PQ-coded truth, on 0..1 code values (PSNR with peak 1, so 1 code of 1023 is about 60 dB): the
whole picture, highlights (truth >= 400 nits), dark edges (truth <= 5 nits inside the harness's silhouette band), and
the mean absolute error in nits at highlights.

  python3 tools/pushwarp_eval/hdr_eval.py --harness <s9>/tools/ownfg_eval --cli $BIN --frames <run2> --out $OUT --scene drive [--report]
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

M1, M2, C1, C2, C3 = 2610 / 16384, 2523 / 4096 * 128, 3424 / 4096, 2413 / 4096 * 32, 2392 / 4096 * 32


def pq_encode(nits):
    y = np.clip(nits / 10000.0, 0, 1) ** M1
    return ((C1 + C2 * y) / (1 + C3 * y)) ** M2


def pq_decode(e):
    p = np.clip(e, 0, 1) ** (1 / M2)
    return 10000.0 * (np.maximum(p - C1, 0) / (C2 - C3 * p)) ** (1 / M1)


def to_hdr_codes(rgb_u8):
    """10-bit PQ codes (int) of the synthetic HDR picture made from an SDR render."""
    s = rgb_u8.astype(np.float64) / 255.0
    lin = np.where(s <= 0.04045, s / 12.92, ((s + 0.055) / 1.055) ** 2.4)
    nits = 203.0 * lin + np.where(lin > 0.75, ((lin - 0.75) / 0.25) ** 2 * 3800.0, 0.0)
    return np.round(pq_encode(nits) * 1023).astype(np.uint32)


def words10(codes):
    return (codes[..., 0] | (codes[..., 1] << 10) | (codes[..., 2] << 20) | (3 << 30)).reshape(-1).astype(np.uint32)


def run_cli(cli, A, B, field, t, variant, tmp):
    h, w = A.shape[:2]
    gh, gw = field.trust.shape
    job = Path(tmp) / "job.bin"; out = Path(tmp) / "out.rgba"
    with open(job, "wb") as f:
        f.write(np.array([w, h, gw, gh, 0], np.uint32).tobytes())
        f.write(np.array([t], np.float32).tobytes()); f.write(np.array([0], np.uint32).tobytes())
        f.write(words10(A).tobytes()); f.write(words10(B).tobytes())
        f.write(np.ascontiguousarray(field.stored[..., 0], np.float32).tobytes())
        f.write(np.ascontiguousarray(field.stored[..., 1], np.float32).tobytes())
        f.write(np.ascontiguousarray(field.trust, np.float32).tobytes())
    env = dict(os.environ, PUSHWARP_TEN_BIT="1")
    if variant != "codes":
        env["PUSHWARP_LINEAR_COMPOSE"] = "1" if variant == "linear" else "check"
    subprocess.run([str(cli), str(job), str(out)], check=True, env=env)
    raw = np.fromfile(out, np.uint32)[: w * h].reshape(h, w)
    return np.stack([raw & 1023, (raw >> 10) & 1023, (raw >> 20) & 1023], -1).astype(np.float64) / 1023.0


def psnr1(a, b, mask=None):
    d = a - b
    if mask is not None:
        if not mask.any():
            return float("nan")
        d = d[mask]
    mse = float(np.mean(d * d))
    return 99.0 if mse <= 1e-14 else 10 * np.log10(1.0 / mse)


def eval_pair(H, cli, cache, t0, tmp, checked):
    F, M = H["flow"], H["metrics"]
    A8, B8 = cache.color(t0), cache.color(t0 + 1)
    A, B = to_hdr_codes(A8), to_hdr_codes(B8)
    field = F.ffx_flows((A >> 2).astype(np.uint8), (B >> 2).astype(np.uint8), 960, 540, "photo")
    if not checked[0]:
        run_cli(cli, A, B, field, 2 / 3, "check", tmp)   # raises if the re-run in code values is not the reference
        checked[0] = True
    res = {}
    for key, t in (("I1", 1 / 3), ("I2", 2 / 3)):
        truth = to_hdr_codes(cache.color(t0 + t)).astype(np.float64) / 1023.0
        nits = pq_decode(truth).max(-1)
        mo, ids = cache.motion(t0 + t, t0 + t + 1 / 3)
        band = M.silhouette_band(ids, 3 * np.nan_to_num(mo))
        hi = nits >= 400.0
        dark = band & (nits <= 5.0)
        for v in ("codes", "linear"):
            out = run_cli(cli, A, B, field, t, v, tmp)
            err_nits = np.abs(pq_decode(out) - pq_decode(truth)).max(-1)
            res[f"{key}/{v}"] = {"psnr": psnr1(out, truth), "hi_psnr": psnr1(out, truth, hi), "dark_edge_psnr": psnr1(out, truth, dark),
                                 "hi_nits_mae": float(err_nits[hi].mean()) if hi.any() else float("nan"),
                                 "hi_share": float(hi.mean()), "dark_edge_share": float(dark.mean())}
    return res


def report(out: Path):
    rows = {}
    for p in sorted((out / "results-hdr").glob("*.json")):
        d = json.loads(p.read_text())
        for k, pr in d["pairs"].items():
            for name, r in pr.items():
                rows.setdefault((d["scene"], name), []).append(r)
    m = lambda xs, f: float(np.nanmean([x[f] for x in xs]))
    lines = ["OFFLINE, synthetic HDR10-like pictures made from SDR renders (BT.709 primaries, PQ codes, up to ~4000 nits), CPU, "
             "pushWarpReference in 10-bit; 'codes' = the implemented scheme, 'linear' = samples and blend in linear light.", "",
             "| scene | frame | PSNR codes / linear | highlights (>= 400 nits) codes / linear | dark edges (<= 5 nits) codes / linear | "
             "highlight error, nits, codes / linear | highlight share | dark-edge share |", "|---|---|---:|---:|---:|---:|---:|---:|"]
    allv = {"codes": [], "linear": []}
    for sc in sorted({s for s, _ in rows}):
        for key in ("I1", "I2"):
            c, l = rows.get((sc, f"{key}/codes")), rows.get((sc, f"{key}/linear"))
            if not c:
                continue
            allv["codes"] += c; allv["linear"] += l
            lines.append(f"| {sc} | {key} | {m(c, 'psnr'):.2f} / {m(l, 'psnr'):.2f} | {m(c, 'hi_psnr'):.2f} / {m(l, 'hi_psnr'):.2f} | "
                         f"{m(c, 'dark_edge_psnr'):.2f} / {m(l, 'dark_edge_psnr'):.2f} | {m(c, 'hi_nits_mae'):.1f} / {m(l, 'hi_nits_mae'):.1f} | "
                         f"{m(c, 'hi_share'):.3f} | {m(c, 'dark_edge_share'):.4f} |")
    c, l = allv["codes"], allv["linear"]
    if c:
        lines.append(f"| all | I1+I2 ({len(c)}) | {m(c, 'psnr'):.2f} / {m(l, 'psnr'):.2f} | {m(c, 'hi_psnr'):.2f} / {m(l, 'hi_psnr'):.2f} | "
                     f"{m(c, 'dark_edge_psnr'):.2f} / {m(l, 'dark_edge_psnr'):.2f} | {m(c, 'hi_nits_mae'):.1f} / {m(l, 'hi_nits_mae'):.1f} | | |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", required=True, type=Path)
    ap.add_argument("--cli", type=Path)
    ap.add_argument("--frames", type=Path, help="the harness's frame cache (renders are reused)")
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--scene", action="append", default=[])
    ap.add_argument("--report", action="store_true")
    a = ap.parse_args()
    sys.path.insert(0, str(a.harness.resolve()))
    import flow, metrics, run_eval, scenes as S  # noqa: E401
    H = {"flow": flow, "metrics": metrics}
    checked = [False]
    for name in a.scene:
        sc = S.scene_by_name(name)
        cache = run_eval.Cache(a.frames or a.out, sc, run_eval.renderer)
        path = a.out / "results-hdr" / f"{name}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        done = json.loads(path.read_text()) if path.exists() else {"scene": name, "pairs": {}}
        for t0 in sc.pairs[:2]:
            k = f"{t0:g}"
            if k in done["pairs"]:
                continue
            start = time.time()
            with tempfile.TemporaryDirectory() as tmp:
                done["pairs"][k] = eval_pair(H, a.cli, cache, t0, tmp, checked)
            path.write_text(json.dumps(done, indent=1))
            print(f"{name} pair {k}: {time.time() - start:.0f} s", flush=True)
    if a.report:
        print(report(a.out))


if __name__ == "__main__":
    main()
