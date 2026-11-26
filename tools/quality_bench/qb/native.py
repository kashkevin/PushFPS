# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Builds (g++) and calls the vendored C++ CPU references: HUD Guard (s12-hud), PushWarp + its flow MODEL (s11-ownfg), and
the s12-pace downscale reference (tests only). Binaries and job files live in a temporary folder (QB_BUILD or /tmp)."""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent.parent
VENDOR = HERE / "vendor"
CPP = HERE / "cpp"
BUILD = Path(os.environ.get("QB_BUILD", tempfile.gettempdir())) / "quality_bench_build"


def _include_tree():
    inc = BUILD / "inc" / "veyra" / "pipeline"
    inc.mkdir(parents=True, exist_ok=True)
    for name in ("PushWarp.h", "PushHudGuard.h"):
        shutil.copyfile(VENDOR / name, inc / name)
    return BUILD / "inc"


def build(name):
    """Returns the path of a built tool: 'pushwarp_cli', 'hud_guard_seq' or 'downscale_ref' (built when missing/stale)."""
    BUILD.mkdir(parents=True, exist_ok=True)
    src = {"pushwarp_cli": VENDOR / "pushwarp_cli.cpp", "hud_guard_seq": CPP / "hud_guard_seq.cpp",
           "downscale_ref": CPP / "downscale_ref.cpp"}[name]
    exe = BUILD / name
    deps = [src] + list(VENDOR.glob("*.h"))
    if exe.exists() and exe.stat().st_mtime > max(p.stat().st_mtime for p in deps):
        return exe
    inc = _include_tree()
    subprocess.run(["g++", "-std=c++20", "-O2", f"-I{inc}", f"-I{VENDOR}", str(src), "-o", str(exe)], check=True)
    return exe


def words(codes, ten_bit):
    """Grey code raster (0..1) -> R=G=B words (R10G10B10A2 for HDR10, R8G8B8A8 for SDR)."""
    if ten_bit:
        q = np.round(np.clip(codes, 0, 1) * 1023).astype(np.uint32)
        return (q | (q << 10) | (q << 20) | (np.uint32(3) << 30)).astype(np.uint32)
    q = np.round(np.clip(codes, 0, 1) * 255).astype(np.uint32)
    return (q | (q << 8) | (q << 16) | (np.uint32(255) << 24)).astype(np.uint32)


def unwords(w, ten_bit):
    """Words -> grey codes (mean of R, G, B)."""
    if ten_bit:
        return ((w & 1023) + ((w >> 10) & 1023) + ((w >> 20) & 1023)).astype(np.float64) / (3 * 1023.0)
    return ((w & 255) + ((w >> 8) & 255) + ((w >> 16) & 255)).astype(np.float64) / (3 * 255.0)


def hud_guard(frames, ten_bit):
    """frames: list of code rasters (real frames in order). Returns (masks, cleaned): masks[i] bit 0 = compose mask of pair
    (i-1, i), bit 1 = pre-clean mask; cleaned[i] = frame i as the generator's (pre-cleaned) input, codes."""
    exe = build("hud_guard_seq")
    H, W = frames[0].shape
    with tempfile.TemporaryDirectory(dir=BUILD) as tmp:
        inp, out = Path(tmp) / "in.bin", Path(tmp) / "out.bin"
        with open(inp, "wb") as f:
            f.write(np.array([W, H, len(frames), 1 if ten_bit else 0], np.uint32).tobytes())
            for fr in frames:
                f.write(words(fr, ten_bit).tobytes())
        subprocess.run([str(exe), str(inp), str(out)], check=True)
        raw = np.fromfile(out, np.uint8)
    per = W * H * 5
    masks, cleaned = [], []
    for i in range(len(frames)):
        chunk = raw[i * per:(i + 1) * per]
        masks.append(chunk[: W * H].reshape(H, W).copy())
        cleaned.append(unwords(chunk[W * H:].view(np.uint32).reshape(H, W), ten_bit))
    return masks, cleaned


def pushwarp(A, B, multiplier, ten_bit, overlay=True):
    """PushWarp's generated frames between real frames A and B (codes) via `pushwarp_cli frames`: returns (frames, info)."""
    exe = build("pushwarp_cli")
    with tempfile.TemporaryDirectory(dir=BUILD) as tmp:
        scale = 1023 if ten_bit else 255
        pa, pb = Path(tmp) / "a.npy", Path(tmp) / "b.npy"
        for p, img in ((pa, A), (pb, B)):
            q = np.round(np.clip(img, 0, 1) * scale).astype(np.uint16)
            np.save(p, np.repeat(q[..., None], 3, axis=2))
        cmd = [str(exe), "frames", "--a", str(pa), "--b", str(pb), "--multiplier", str(multiplier), "--out",
               str(Path(tmp) / "gen.npy"), "--max", str(scale), "--overlay", "on" if overlay else "off"]
        if ten_bit:
            cmd.append("--hdr10")
        res = subprocess.run(cmd, check=True, capture_output=True, text=True)
        infos = [json.loads(line) for line in res.stdout.splitlines() if line.strip().startswith("{")]
        outs = []
        for k in range(1, multiplier):
            g = np.load(Path(tmp) / f"gen_{k}of{multiplier}.npy").astype(np.float64)
            outs.append(g.mean(axis=2) / scale)
    return outs, infos


def downscale_ref(plane, out_w, out_h, area, pq):
    exe = build("downscale_ref")
    H, W = plane.shape
    with tempfile.TemporaryDirectory(dir=BUILD) as tmp:
        inp, out = Path(tmp) / "in.bin", Path(tmp) / "out.bin"
        with open(inp, "wb") as f:
            f.write(np.array([W, H, out_w, out_h, int(area), int(pq)], np.uint32).tobytes())
            f.write(np.ascontiguousarray(plane, np.float64).tobytes())
        subprocess.run([str(exe), str(inp), str(out)], check=True)
        return np.fromfile(out, np.float64).reshape(out_h, out_w)
