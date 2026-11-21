#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Where is detail lost? Numbers for the matched frames of the clarity retest (stream "clarity", round 2).

PushFPS writes them with VEYRA_TEST_RP_PROBE_FRAMES=1..4 (include/veyra/engine/PushFrameDump.h): per capture one
JSON and up to three NumPy arrays of the SAME presented frame, copied in the same command list:
  raw     the decoded picture after YUV -> linear RGB, before any processing (float16, linear light)
  output  the graph's output at its work size (8-bit sRGB in SDR)
  screen  the swap chain's back buffer after the presenter's blit, before Present (what the window shows)

  python3 compare.py <folder with <stamp>-p*-f*.json> [--png <folder>] [--json <file>]

What it computes, per capture (SDR; HDR captures are listed, not judged: the PQ / BT.2020 encode is not modelled):
  1:1 (output the size of raw): the identity test. The app's final write is modelled exactly as ScaleBlit.hlsl does
      it at 1:1 (sRGB encode of the linear value, written to an 8-bit UNORM target = round to nearest). 'identical'
      = every code equal; 'identical within rounding' = no code more than 1 apart (float rounding at .5); anything
      else = the app changed the picture. With dither on (the JSON's ditherStep > 0: a colour grade, or the clean-up
      stage, whose final write rounds with an ordered pattern) codes up to 2 apart are EXPECTED and no verdict is given.
  enlarged (output or screen larger than raw): PSNR of the captured picture against
      presenter-model    a CPU MODEL of what PresentBlit.hlsl does in SDR: it samples the graph's 8-bit sRGB CODES through
                         a UNORM view (no sRGB decode), so it interpolates gamma-encoded values, NOT linear light; with
                         the JSON's presentFine true it uses FineSample (Catmull-Rom clamped to the 2x2 texels), with
                         false one bilinear tap (null: both are given). Its input is the captured output when there is one.
      bilinear-linear, catmull-rom-linear, lanczos3-linear   the same enlargement in linear light (each then sRGB-encoded),
                         what a gamma-correct scaler would give. Interpolating codes instead darkens and softens fine
                         high-contrast detail a little; meanLumaVsLinear (captured minus linear-light Catmull-Rom, codes)
                         shows the size of it.
      and its detail ratio = mean absolute Laplacian of its luma / that of the Lanczos-3 reference (above 1: sharper or
      added detail, e.g. sharpening or invention; below 1: softer). Neither number can tell real detail from
      invented detail: that is for the eye, with the PNG copies side by side.
Never a statement about what the game rendered before Sony's encoder: there is no such reference on the PC.
"""
import argparse
import json
import math
import os
import struct
import sys
import zlib

try:
    import numpy as np
except ImportError:  # pragma: no cover
    sys.exit("compare.py needs NumPy (pip install numpy)")

# ---------------------------------------------------------------- colour

def srgb_encode(linear):
    c = np.clip(linear, 0.0, None)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1.0 / 2.4) - 0.055)

def srgb_decode(code):
    c = np.clip(code, 0.0, 1.0)
    return np.where(c <= 0.04045, c / 12.92, np.power((c + 0.055) / 1.055, 2.4))

def to_codes8(linear):
    """The 8-bit UNORM write of an sRGB-encoded value (round to nearest, clamped), as the GPU does it."""
    return np.clip(np.floor(srgb_encode(linear.astype(np.float64)) * 255.0 + 0.5), 0, 255).astype(np.uint8)

def luma(codes):
    c = codes.astype(np.float64)
    return 0.2126 * c[..., 0] + 0.7152 * c[..., 1] + 0.0722 * c[..., 2]

# ---------------------------------------------------------------- resampling (pixel centres: src = (i + .5) * in / out - .5)

def _taps(n_in, n_out, kernel, radius):
    scale = n_in / n_out
    centre = (np.arange(n_out) + 0.5) * scale - 0.5
    first = np.floor(centre).astype(np.int64) - radius + 1
    offsets = np.arange(2 * radius)
    index = first[:, None] + offsets[None, :]
    weight = kernel(centre[:, None] - index)
    weight /= weight.sum(axis=1, keepdims=True)
    return np.clip(index, 0, n_in - 1), weight

def _resample_axis(img, n_out, kernel, radius, axis):
    n_in = img.shape[axis]
    index, weight = _taps(n_in, n_out, kernel, radius)
    moved = np.moveaxis(img, axis, 0)
    out = np.zeros((n_out,) + moved.shape[1:], dtype=np.float64)
    shape = (n_out,) + (1,) * (moved.ndim - 1)
    for k in range(index.shape[1]):   # one tap at a time: a full gather of 6 taps of a 1440p picture would need ~400 MB
        out += weight[:, k].reshape(shape) * moved[index[:, k]]
    return np.moveaxis(out, 0, axis)

def resample(img, width, height, kernel, radius):
    return _resample_axis(_resample_axis(img, height, kernel, radius, 0), width, kernel, radius, 1)

def k_linear(x):
    return np.clip(1.0 - np.abs(x), 0.0, None)

def k_catmull_rom(x):
    a = np.abs(x)
    return np.where(a < 1, 1.5 * a**3 - 2.5 * a**2 + 1, np.where(a < 2, -0.5 * a**3 + 2.5 * a**2 - 4 * a + 2, 0.0))

def k_lanczos3(x):
    a = np.abs(x)
    return np.where(a < 3, np.sinc(x) * np.sinc(x / 3.0), 0.0)

def fine_sample(img, width, height):
    """CPU model of PresentBlit.hlsl FineSample for an enlargement: Catmull-Rom (a = -0.5) on the 4x4 texels, then
    clamped to the range of the 2x2 texels around the sample. A model, not the GPU's own pixels."""
    h_in, w_in = img.shape[:2]
    out = resample(img, width, height, k_catmull_rom, 2)
    ys = np.floor((np.arange(height) + 0.5) * h_in / height - 0.5).astype(np.int64)
    xs = np.floor((np.arange(width) + 0.5) * w_in / width - 0.5).astype(np.int64)
    y0, y1 = np.clip(ys, 0, h_in - 1), np.clip(ys + 1, 0, h_in - 1)
    x0, x1 = np.clip(xs, 0, w_in - 1), np.clip(xs + 1, 0, w_in - 1)
    corners = [img[y0][:, x0], img[y0][:, x1], img[y1][:, x0], img[y1][:, x1]]
    lo = np.minimum(np.minimum(corners[0], corners[1]), np.minimum(corners[2], corners[3]))
    hi = np.maximum(np.maximum(corners[0], corners[1]), np.maximum(corners[2], corners[3]))
    return np.clip(out, lo, hi)

# ---------------------------------------------------------------- numbers

def psnr(a, b):
    mse = float(np.mean((a.astype(np.float64) - b.astype(np.float64)) ** 2))
    return math.inf if mse == 0 else 10.0 * math.log10(255.0 ** 2 / mse)

def detail(codes):
    y = luma(codes)
    lap = 4 * y[1:-1, 1:-1] - y[:-2, 1:-1] - y[2:, 1:-1] - y[1:-1, :-2] - y[1:-1, 2:]
    return float(np.mean(np.abs(lap)))

def presenter_model(codes, width, height, fine):
    """What PresentBlit does to the graph's 8-bit sRGB output in SDR: it reads the CODES (UNORM view, no sRGB decode)
    and interpolates them as they are, then writes 8-bit codes. fine = FineSample, else one bilinear tap."""
    values = codes.astype(np.float64) / 255.0
    out = fine_sample(values, width, height) if fine else resample(values, width, height, k_linear, 1)
    return np.clip(np.floor(out * 255.0 + 0.5), 0, 255).astype(np.uint8)

def identity(raw_linear, output_codes, dither_step=0.0):
    model = to_codes8(raw_linear)
    diff = np.abs(model.astype(np.int16) - output_codes.astype(np.int16))
    worst = int(diff.max())
    differing = int(np.count_nonzero(diff))
    beyond_one = int(np.count_nonzero(diff > 1))
    if dither_step and dither_step > 0:
        verdict = "dither on: up to ±2 codes expected (no verdict)"
    else:
        verdict = "identical" if worst == 0 else "identical within rounding" if worst <= 1 else "differs: the app changed the picture"
    return {"verdict": verdict, "maxCodeDiff": worst, "codesDiffering": differing, "codesBeyondOne": beyond_one,
            "codes": int(diff.size), "psnr": psnr(model, output_codes)}

def enlarged(raw_linear, codes, source_codes=None, fine=None):
    """source_codes: the presenter's input (the captured 1:1 output; else raw encoded as the 1:1 write does it).
    fine: the JSON's presentFine (True / False / None = unknown: both kernels)."""
    height, width = codes.shape[:2]
    source = source_codes if source_codes is not None else to_codes8(raw_linear)
    refs = {}
    if fine is None:
        refs["presenter-model-fine"] = presenter_model(source, width, height, True)
        refs["presenter-model-bilinear"] = presenter_model(source, width, height, False)
    else:
        refs["presenter-model"] = presenter_model(source, width, height, bool(fine))
    refs["bilinear-linear"] = to_codes8(resample(raw_linear, width, height, k_linear, 1))
    linear_cr = to_codes8(fine_sample(raw_linear, width, height))
    refs["catmull-rom-linear"] = linear_cr
    refs["lanczos3-linear"] = to_codes8(resample(raw_linear, width, height, k_lanczos3, 3))
    base = detail(refs["lanczos3-linear"])
    out = {"size": f"{width}x{height}", "presentFine": fine, "psnr": {k: psnr(codes, v) for k, v in refs.items()},
           "meanLumaVsLinear": float(np.mean(luma(codes)) - np.mean(luma(linear_cr))),
           "detailRatio": detail(codes) / base if base > 0 else None,
           "detailRatioOfReferences": {k: (detail(v) / base if base > 0 else None) for k, v in refs.items()}}
    return out, refs

# ---------------------------------------------------------------- files

def write_png(path, codes):
    """8-bit RGB PNG with the standard library only (zlib), lossless."""
    height, width = codes.shape[:2]
    raw = b"".join(b"\x00" + codes[y].tobytes() for y in range(height))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

def safe_name(name):
    return isinstance(name, str) and name and "/" not in name and "\\" not in name and ".." not in name and name.endswith(".npy")

def load_capture(folder, meta):
    pictures = {}
    for part in ("raw", "output", "screen"):
        entry = meta.get(part)
        if not entry or not safe_name(entry.get("file")): continue
        path = os.path.join(folder, entry["file"])
        if not os.path.isfile(path): continue
        pictures[part] = (np.load(path, allow_pickle=False), entry)
    return pictures

def analyse(folder, png_folder=None):
    results = []
    for name in sorted(os.listdir(folder)):
        if not name.endswith(".json"): continue
        try:
            with open(os.path.join(folder, name), encoding="utf-8") as f: meta = json.load(f)
        except (OSError, ValueError):
            continue
        if meta.get("schema") != 1: continue
        pictures = load_capture(folder, meta)
        item = {"capture": name[:-5], "pts100ns": meta.get("pts100ns"), "hdrMode": meta.get("hdrMode"), "ditherStep": meta.get("ditherStep"),
                "presentFine": meta.get("presentFine")}
        if "raw" not in pictures:
            item["note"] = "no raw picture: nothing to compare"; results.append(item); continue
        if meta.get("hdrMode"):
            item["note"] = "HDR capture: listed, not judged (the PQ / BT.2020 encode is not modelled)"; results.append(item); continue
        raw_linear = pictures["raw"][0].astype(np.float64)
        item["raw"] = f"{raw_linear.shape[1]}x{raw_linear.shape[0]}"
        for part in ("output", "screen"):
            if part not in pictures: item[part] = None; continue
            codes, entry = pictures[part]
            if entry.get("encoding") != "srgb8" or codes.dtype != np.uint8:
                item[part] = {"note": f"encoding {entry.get('encoding')}: not judged"}; continue
            if codes.shape[:2] == raw_linear.shape[:2]:
                item[part] = {"kind": "1:1", **identity(raw_linear, codes, float(meta.get("ditherStep") or 0.0))}
            elif codes.shape[0] > raw_linear.shape[0] and abs(codes.shape[1] / codes.shape[0] - raw_linear.shape[1] / raw_linear.shape[0]) < 0.01:
                source = None
                if part == "screen" and "output" in pictures:
                    out_codes, out_entry = pictures["output"]
                    if out_entry.get("encoding") == "srgb8" and out_codes.dtype == np.uint8 and out_codes.shape[:2] == raw_linear.shape[:2]: source = out_codes
                numbers, refs = enlarged(raw_linear, codes, source, meta.get("presentFine"))
                item[part] = {"kind": "enlarged", **numbers}
                if png_folder:
                    for ref, ref_codes in refs.items(): write_png(os.path.join(png_folder, f"{item['capture']}-{part}-ref-{ref}.png"), ref_codes)
            else:
                item[part] = {"kind": "other", "note": f"size {codes.shape[1]}x{codes.shape[0]} is not the stream's shape (window not full screen?): not judged"}
            if png_folder: write_png(os.path.join(png_folder, f"{item['capture']}-{part}.png"), codes)
        if png_folder: write_png(os.path.join(png_folder, f"{item['capture']}-raw.png"), to_codes8(raw_linear))
        results.append(item)
    return results

def finite(value):
    """JSON has no infinity: an infinite PSNR (identical pictures) is written as null."""
    if isinstance(value, float) and not math.isfinite(value): return None
    if isinstance(value, dict): return {k: finite(v) for k, v in value.items()}
    if isinstance(value, list): return [finite(v) for v in value]
    return value

def fmt_psnr(value):
    return "inf" if value == math.inf else f"{value:.2f} dB"

def report(results):
    lines = []
    for item in results:
        lines.append(f"{item['capture']} (pts {item.get('pts100ns')}, raw {item.get('raw', '-')})")
        if item.get("note"): lines.append("  " + item["note"]); continue
        for part in ("output", "screen"):
            r = item.get(part)
            if r is None: lines.append(f"  {part}: not captured"); continue
            if r.get("note"): lines.append(f"  {part}: {r['note']}"); continue
            if r["kind"] == "1:1":
                lines.append(f"  {part} at 1:1: {r['verdict']} (largest difference {r['maxCodeDiff']} code(s); {r['codesDiffering']} of {r['codes']} codes differ, "
                             f"{r['codesBeyondOne']} by more than 1; PSNR {fmt_psnr(r['psnr'])})")
            else:
                ps = ", ".join(f"{k} {fmt_psnr(v)}" for k, v in r["psnr"].items())
                refs = ", ".join(f"{k} {v:.3f}" for k, v in r["detailRatioOfReferences"].items() if v is not None)
                ratio = "n/a" if r["detailRatio"] is None else f"{r['detailRatio']:.3f}"
                kernel = {True: "FineSample", False: "one bilinear tap", None: "unknown (both modelled)"}.get(r.get("presentFine"), "unknown (both modelled)")
                lines.append(f"  {part} enlarged to {r['size']} (presenter kernel: {kernel}, on sRGB codes): PSNR against {ps}; detail ratio {ratio} "
                             f"(references: {refs}); mean luma against linear-light Catmull-Rom {r['meanLumaVsLinear']:+.2f} codes")
    return "\n".join(lines) if lines else "no capture found"

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("folder")
    p.add_argument("--png", help="write lossless PNG copies (raw as 8-bit sRGB, output, screen, references) into this folder")
    p.add_argument("--json", help="write the numbers as JSON")
    a = p.parse_args(argv)
    if a.png: os.makedirs(a.png, exist_ok=True)
    results = analyse(a.folder, a.png)
    print(report(results))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f: json.dump(finite(results), f, indent=1)
    return 0

if __name__ == "__main__":
    sys.exit(main())
