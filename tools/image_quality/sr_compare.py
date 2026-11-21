#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Offline, reference-based image-quality comparison of the non-AI super-resolution choices.

OFFLINE CPU MODEL. Nothing here runs the app, a GPU, RTX Video Super Resolution or NVIDIA Image Scaling.

  model   Synthetic game-like reference frames at the display size (default 2560x1440), the "stream" made from them
          (downscale to 1920x1080, then HEVC or H.264 at 15 Mbit/s 60 fps when ffmpeg has the encoder, else no
          compression), upscaled back by CPU reference scalers, scored against the reference.
  score   Scores a recording of the app's processed picture against a reference clip (see docs/SUPER_RESOLUTION.md,
          "Scoring the RTX output on hardware"): frames are decoded with ffmpeg, matched by the best PSNR within a
          search window, and scored like the model rows.

Metrics (luma, BT.709 weights, 8-bit scale): PSNR; SSIM (Gaussian 11x11, sigma 1.5); sharpness = mean Sobel
magnitude of the result divided by the reference's (1.0 = as sharp, above 1 = over-sharpened or noise); overshoot =
mean amount (8-bit levels) by which a pixel leaves the reference's local 3x3 [min, max] range, over edge pixels
(halo / ringing). Higher PSNR/SSIM is better; numbers reward blur, so read sharpness and overshoot beside them.

Scalers (OpenCV): bilinear, bicubic (a=-0.75), lanczos4; "sharpen" = lanczos4 + a luma-adaptive unsharp mask with
overshoot clamped to the local range: a CPU stand-in for a single-pass sharpening scaler, NOT a port of NIS; and the
two-step path the app takes with the default 4K SR target on a 1440p window: an upscale to 3840x2160 followed by
one bilinear sample per output pixel (PresentBlit.hlsl's minification path) or by an exact area reduction.

Needs python3, numpy, opencv-python (cv2). ffmpeg is optional (compression, and the score mode).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

try:
    import cv2
except ImportError:  # pragma: no cover
    sys.exit("opencv-python (cv2) is required")


# --- synthetic game-like material -------------------------------------------------------------------------------
def _noise(h, w, seed, octaves=5):
    rng = np.random.default_rng(seed)
    out = np.zeros((h, w), np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        gh, gw = max(2, h >> (octaves - o + 1)), max(2, w >> (octaves - o + 1))
        g = rng.random((gh, gw), dtype=np.float32)
        out += amp * cv2.resize(g, (w, h), interpolation=cv2.INTER_CUBIC)
        total += amp
        amp *= 0.55
    return out / total


def world(w, h, seed=7):
    """One large picture to pan over: terrain, foliage, buildings with windows, wires, road markings, text."""
    rng = np.random.default_rng(seed)
    n = _noise(h, w, seed)
    img = np.stack([0.35 + 0.4 * n, 0.45 + 0.35 * n, 0.25 + 0.3 * n], -1)
    leaves = (_noise(h, w, seed + 1, 7) > 0.55).astype(np.float32)
    img *= (1.0 - 0.35 * leaves[..., None])
    for _ in range(int(w * h / 90000)):
        x, y = int(rng.integers(0, w - 220)), int(rng.integers(0, h - 300))
        bw, bh = int(rng.integers(60, 220)), int(rng.integers(120, 300))
        col = rng.random(3) * 0.5 + 0.25
        img[y:y + bh, x:x + bw] = col
        for wy in range(y + 8, y + bh - 12, 18):
            for wx in range(x + 6, x + bw - 10, 14):
                img[wy:wy + 9, wx:wx + 7] = col * 0.35 + (0.6 if rng.random() > 0.6 else 0.0)
    img = to8(np.clip(img, 0, 1))   # OpenCV draws text and lines on 8-bit pictures only
    for _ in range(int(w / 64)):
        p1 = (int(rng.integers(0, w)), int(rng.integers(0, h)))
        p2 = (int(rng.integers(0, w)), int(rng.integers(0, h)))
        cv2.line(img, p1, p2, (12, 12, 12), 1, cv2.LINE_AA)
    for _ in range(int(w / 160)):
        cv2.putText(img, "CITY MARKET 24/7", (int(rng.integers(0, w - 400)), int(rng.integers(30, h))),
                    cv2.FONT_HERSHEY_SIMPLEX, float(rng.uniform(0.5, 1.4)), (240, 240, 230), 2, cv2.LINE_AA)
    return img


def hud(frame):
    h, w = frame.shape[:2]
    s = h / 1440.0
    cv2.circle(frame, (int(150 * s), int(h - 150 * s)), int(110 * s), (230, 230, 230), max(1, int(3 * s)), cv2.LINE_AA)
    cv2.putText(frame, "$ 1,250,000", (int(w - 420 * s), int(80 * s)), cv2.FONT_HERSHEY_DUPLEX, 1.4 * s, (50, 230, 80),
                max(1, int(2 * s)), cv2.LINE_AA)
    cv2.rectangle(frame, (int(60 * s), int(h - 30 * s)), (int(420 * s), int(h - 18 * s)), (50, 200, 50), -1)
    return frame


def scene_frames(name, w, h, frames, seed):
    big = world(int(w * 1.6), int(h * 1.4), seed)
    out = []
    for t in range(frames):
        if name == "pan":
            x, y = 40 + 14 * t, 60 + 2 * t
        elif name == "still":
            x, y = 200, 120
        else:  # fast
            x, y = 40 + 46 * t, 60 + 9 * t
        f = big[y:y + h, x:x + w].copy()
        out.append(hud(f) if name != "still" else f)
    return out


# --- scaling ----------------------------------------------------------------------------------------------------
def to8(img):
    return np.clip(img * 255.0 + 0.5, 0, 255).astype(np.uint8)


def luma(img8):
    f = img8.astype(np.float32)
    return 0.2126 * f[..., 2] + 0.7152 * f[..., 1] + 0.0722 * f[..., 0]  # OpenCV order: BGR


def sharpen_scale(img8, size, amount=0.6):
    up = cv2.resize(img8, size, interpolation=cv2.INTER_LANCZOS4).astype(np.float32)
    blur = cv2.GaussianBlur(up, (0, 0), 1.2)
    detail = up - blur
    # Adaptive: less where the picture is already busy (noise, foliage), as sharpening scalers do.
    var = cv2.GaussianBlur((detail ** 2).mean(-1), (0, 0), 3.0)
    gain = amount / (1.0 + var / 60.0)
    out = up + detail * gain[..., None]
    lo = cv2.erode(up, np.ones((3, 3), np.uint8))
    hi = cv2.dilate(up, np.ones((3, 3), np.uint8))
    return np.clip(np.minimum(np.maximum(out, lo), hi) * 0.5 + out * 0.5, 0, 255).astype(np.uint8)


def scalers(display):
    w, h = display
    big = (int(round(h * 16 / 9 * 1.5)) // 2 * 2, h * 3 // 2) if (w, h) == (2560, 1440) else (3840, 2160)
    return {
        "bilinear": lambda s: cv2.resize(s, display, interpolation=cv2.INTER_LINEAR),
        "bicubic": lambda s: cv2.resize(s, display, interpolation=cv2.INTER_CUBIC),
        "lanczos4": lambda s: cv2.resize(s, display, interpolation=cv2.INTER_LANCZOS4),
        "sharpen": lambda s: sharpen_scale(s, display),
        "lanczos4-4k-then-bilinear": lambda s: cv2.resize(cv2.resize(s, big, interpolation=cv2.INTER_LANCZOS4), display,
                                                         interpolation=cv2.INTER_LINEAR),
        "lanczos4-4k-then-area": lambda s: cv2.resize(cv2.resize(s, big, interpolation=cv2.INTER_LANCZOS4), display,
                                                     interpolation=cv2.INTER_AREA),
    }


# --- metrics ----------------------------------------------------------------------------------------------------
def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 99.0 if mse <= 1e-10 else 10.0 * np.log10(255.0 ** 2 / mse)


def ssim(a, b):
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    g = lambda x: cv2.GaussianBlur(x, (11, 11), 1.5)
    ma, mb = g(a), g(b)
    va, vb, cov = g(a * a) - ma * ma, g(b * b) - mb * mb, g(a * b) - ma * mb
    m = ((2 * ma * mb + c1) * (2 * cov + c2)) / ((ma * ma + mb * mb + c1) * (va + vb + c2))
    return float(m[5:-5, 5:-5].mean())


def sobel_mag(y):
    gx = cv2.Sobel(y, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(y, cv2.CV_32F, 0, 1, ksize=3)
    return np.sqrt(gx * gx + gy * gy)


def metrics(ref8, out8):
    ry, oy = luma(ref8), luma(out8)
    rs = sobel_mag(ry)
    edges = rs > np.percentile(rs, 90)
    lo = cv2.erode(ry, np.ones((3, 3), np.uint8))
    hi = cv2.dilate(ry, np.ones((3, 3), np.uint8))
    over = np.maximum(oy - hi, 0) + np.maximum(lo - oy, 0)
    return {"psnr": psnr(ry, oy), "ssim": ssim(ry, oy), "sharpness": float(sobel_mag(oy).mean() / max(rs.mean(), 1e-6)),
            "overshoot": float(over[edges].mean())}


# --- the stream -------------------------------------------------------------------------------------------------
def encoder():
    ff = shutil.which("ffmpeg")
    if not ff:
        return None, None
    names = subprocess.run([ff, "-hide_banner", "-encoders"], capture_output=True, text=True).stdout
    for codec in ("libx265", "libx264"):
        if codec in names:
            return ff, codec
    return ff, None


def compress(frames8, codec, ff, mbps, work):
    """Round trip through the encoder at the stream's bit rate (60 fps, the size of frames8)."""
    h, w = frames8[0].shape[:2]
    src, dst = os.path.join(work, "in.yuv"), os.path.join(work, "out.mkv")
    with open(src, "wb") as f:
        for fr in frames8:
            f.write(cv2.cvtColor(fr, cv2.COLOR_BGR2RGB).tobytes())
    rate = f"{mbps}M"
    extra = ["-x265-params", "log-level=error"] if codec == "libx265" else []
    subprocess.run([ff, "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{w}x{h}", "-r", "60", "-i", src,
                    "-c:v", codec, "-preset", "fast", "-b:v", rate, "-maxrate", rate, "-bufsize", f"{mbps / 2}M",
                    "-pix_fmt", "yuv420p", *extra, dst], check=True)
    out = subprocess.run([ff, "-loglevel", "error", "-i", dst, "-f", "rawvideo", "-pix_fmt", "bgr24", "-"], capture_output=True, check=True).stdout
    n = len(out) // (w * h * 3)
    return [np.frombuffer(out, np.uint8, w * h * 3, i * w * h * 3).reshape(h, w, 3) for i in range(n)]


def run_model(args):
    display = (args.width, args.height)
    stream = (1920, 1080)
    ff, codec = encoder()
    if args.no_encode:
        codec = None
    rows, per_scene = {}, {}
    with tempfile.TemporaryDirectory() as work:
        for i, scene in enumerate(args.scenes.split(",")):
            refs = scene_frames(scene, display[0], display[1], args.frames, 11 + i)
            low = [cv2.resize(r, stream, interpolation=cv2.INTER_AREA) for r in refs]
            if codec:
                # The first frames of a short clip carry the keyframe and rate-control start-up: encode a lead-in.
                lead = [low[0]] * 30
                coded = compress(lead + low, codec, ff, args.mbps, work)[30:]
                low = coded if len(coded) == len(low) else low
            for name, fn in scalers(display).items():
                ms = [metrics(r, fn(s)) for r, s in zip(refs, low)]
                agg = {k: float(np.mean([m[k] for m in ms])) for k in ms[0]}
                agg["worstPsnr"] = float(min(m["psnr"] for m in ms))
                per_scene.setdefault(scene, {})[name] = agg
                for k, v in agg.items():
                    rows.setdefault(name, {}).setdefault(k, []).append(v)
    summary = {name: {k: float(np.mean(v)) for k, v in m.items()} for name, m in rows.items()}
    result = {
        "label": "offline CPU model: synthetic game-like frames, CPU scalers; no GPU, no RTX Video SR, no NIS",
        "display": f"{display[0]}x{display[1]}", "stream": f"{stream[0]}x{stream[1]}",
        "codec": codec or "none", "mbps": args.mbps if codec else 0, "frames": args.frames, "scenes": args.scenes.split(","),
        "summary": summary, "perScene": per_scene,
    }
    with open(args.out, "w") as f:
        json.dump(result, f, indent=1)
    print(f"# {result['label']}\n# display {result['display']}, stream {result['stream']}, codec {result['codec']} "
          f"{result['mbps']} Mbit/s, {args.frames} frames x {len(result['scenes'])} scenes")
    print(f"{'scaler':28s} {'PSNR dB':>8s} {'worst':>7s} {'SSIM':>7s} {'sharp':>6s} {'overshoot':>9s}")
    for name, m in summary.items():
        print(f"{name:28s} {m['psnr']:8.2f} {m['worstPsnr']:7.2f} {m['ssim']:7.4f} {m['sharpness']:6.3f} {m['overshoot']:9.3f}")


# --- scoring a recording ----------------------------------------------------------------------------------------
def decode(ff, path, size):
    w, h = size
    out = subprocess.run([ff, "-loglevel", "error", "-i", path, "-vf", f"scale={w}:{h}:flags=area", "-f", "rawvideo", "-pix_fmt", "bgr24", "-"],
                         capture_output=True, check=True).stdout
    n = len(out) // (w * h * 3)
    return [np.frombuffer(out, np.uint8, w * h * 3, i * w * h * 3).reshape(h, w, 3) for i in range(n)]


def run_score(args):
    ff = shutil.which("ffmpeg")
    if not ff:
        sys.exit("score needs ffmpeg")
    size = (args.width, args.height)
    refs, recs = decode(ff, args.reference, size), decode(ff, args.recording, size)
    if not refs or not recs:
        sys.exit("no frames decoded")
    small = lambda f: cv2.resize(luma(f), (320, 180), interpolation=cv2.INTER_AREA)
    ref_small = [small(r) for r in refs]
    scored = []
    for i, rec in enumerate(recs[:args.max_frames]):
        rs = small(rec)
        best = max(range(len(refs)), key=lambda j: psnr(ref_small[j], rs))
        m = metrics(refs[best], rec)
        m.update({"recordingFrame": i, "referenceFrame": best})
        scored.append(m)
    keys = ("psnr", "ssim", "sharpness", "overshoot")
    result = {"label": args.label or "recording scored against a reference", "recording": os.path.basename(args.recording),
              "reference": os.path.basename(args.reference), "size": f"{size[0]}x{size[1]}", "frames": len(scored),
              "summary": {k: float(np.mean([m[k] for m in scored])) for k in keys} | {"worstPsnr": float(min(m["psnr"] for m in scored))},
              "perFrame": scored}
    with open(args.out, "w") as f:
        json.dump(result, f, indent=1)
    s = result["summary"]
    print(f"{result['label']}: frames={len(scored)} psnr={s['psnr']:.2f} worst={s['worstPsnr']:.2f} ssim={s['ssim']:.4f} "
          f"sharpness={s['sharpness']:.3f} overshoot={s['overshoot']:.3f}")


def run_make_reference(args):
    """Writes the synthetic reference (display size) and its 1080p stream as lossless video files, for feeding the
    app through the Frame Input API and scoring its recording afterwards."""
    ff = shutil.which("ffmpeg")
    if not ff:
        sys.exit("make-reference needs ffmpeg")
    display = (args.width, args.height)
    frames = []
    for i, scene in enumerate(args.scenes.split(",")):
        frames += scene_frames(scene, display[0], display[1], args.frames, 11 + i)
    for path, size in ((args.out + "-reference.mkv", display), (args.out + "-1080p.mkv", (1920, 1080))):
        p = subprocess.Popen([ff, "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "bgr24", "-s", f"{size[0]}x{size[1]}", "-r", "60",
                              "-i", "-", "-c:v", "ffv1", path], stdin=subprocess.PIPE)
        for f in frames:
            p.stdin.write((f if size == display else cv2.resize(f, size, interpolation=cv2.INTER_AREA)).tobytes())
        p.stdin.close()
        p.wait()
        print(path)
    # The stream as the console would send it: the 1080p frames through HEVC (else H.264) at 15 Mbit/s.
    _, codec = encoder()
    if codec:
        coded = args.out + "-1080p-" + ("hevc" if codec == "libx265" else "h264") + "15.mkv"
        subprocess.run([ff, "-loglevel", "error", "-y", "-i", args.out + "-1080p.mkv", "-c:v", codec, "-preset", "fast", "-b:v", "15M",
                        "-maxrate", "15M", "-bufsize", "7.5M", "-pix_fmt", "yuv420p", coded], check=True)
        print(coded)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    m = sub.add_parser("model")
    m.add_argument("--width", type=int, default=2560)
    m.add_argument("--height", type=int, default=1440)
    m.add_argument("--frames", type=int, default=8)
    m.add_argument("--scenes", default="still,pan,fast")
    m.add_argument("--mbps", type=float, default=15.0)
    m.add_argument("--no-encode", action="store_true")
    m.add_argument("--out", default="sr_model_results.json")
    s = sub.add_parser("score")
    s.add_argument("--reference", required=True)
    s.add_argument("--recording", required=True)
    s.add_argument("--width", type=int, default=2560)
    s.add_argument("--height", type=int, default=1440)
    s.add_argument("--max-frames", type=int, default=240)
    s.add_argument("--label", default="")
    s.add_argument("--out", default="sr_score.json")
    r = sub.add_parser("make-reference")
    r.add_argument("--width", type=int, default=2560)
    r.add_argument("--height", type=int, default=1440)
    r.add_argument("--frames", type=int, default=120)
    r.add_argument("--scenes", default="still,pan,fast")
    r.add_argument("--out", default="sr_reference")
    args = ap.parse_args()
    {"model": run_model, "score": run_score, "make-reference": run_make_reference}[args.mode](args)


if __name__ == "__main__":
    main()
