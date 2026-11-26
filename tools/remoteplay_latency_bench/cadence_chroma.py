#!/usr/bin/env python3
# The fourth review's chroma scenes (the independent reviewer's generator, copied 19 November 2026;
# seeds 5101-5103, 720p, 420 pictures): a colour-only change at 60 fps over a world whose luma
# repeats are exact or nearly so. Run: python3 cadence_chroma.py <out folder> [scene ...]
# The reviewer's other (luma) scenes are kept for completeness. Original note:
# Reviewer's adversarial streams for the third dedup round. Reuses cadence_streams.py's encoders and
# its luma measurement (decoded_differences) and writes the same line format; "new" = the full YUV
# picture (luma AND chroma) differs from the one before, as the author's mixed/holdout3 sets do.
import sys, pathlib, json, time
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import numpy as np, av, cv2
import cadence_streams as cs

W, H = 1280, 720
rng = np.random.default_rng(91373)


def texture(w, h, seed):
    r = np.random.default_rng(seed)
    img = r.random((h // 8, w // 8, 3)).astype(np.float32)
    img = cv2.resize(img, (w, h), interpolation=cv2.INTER_CUBIC)
    img = cv2.GaussianBlur(img, (0, 0), 3)
    img = (img - img.min()) / (img.max() - img.min())
    detail = r.random((h, w, 1)).astype(np.float32) * 0.15
    return np.clip((img * 0.85 + detail) * 255, 0, 255).astype(np.uint8)


WORLDS = [texture(W + 1200, H, s) for s in (5101, 5102, 5103)]


def planes(rgb):
    yuv = cv2.cvtColor(np.ascontiguousarray(rgb), cv2.COLOR_RGB2YUV_I420)
    h, w = rgb.shape[:2]
    y = yuv[:h].copy(); u = yuv[h:h + h // 4].reshape(h // 2, w // 2).copy(); v = yuv[h + h // 4:].reshape(h // 2, w // 2).copy()
    return y, u, v


def pack(y, u, v):
    h, w = y.shape
    return np.concatenate([y, u.reshape(-1, w), v.reshape(-1, w)])


def world(k, offset_fn, which=0, w=W, h=H):
    off = int(offset_fn(k))
    tex = WORLDS[which] if w == W else WTEX1080
    return planes(tex[:h, off:off + w])


def pan30(k): return (k // 2) * 2
def pan60(k): return k * 2


def scene(name, k):
    """One 60 Hz picture, index k."""
    if name == "chroma60":           # world at 30; HUD box: luma fixed, hue changes every picture
        y, u, v = world(k, pan30)
        y[100:164, 100:164] = 128
        u[50:82, 50:82] = 128 + int(40 * np.cos(k * 0.7)); v[50:82, 50:82] = 128 + int(40 * np.sin(k * 0.7))
    elif name == "chroma-blink":     # isoluminant HUD blink red/green, toggles every 5 pictures (hits both slots)
        y, u, v = world(k, pan30)
        y[600:640, 1100:1220] = 110
        on = (k // 5) % 2
        u[300:320, 550:610] = 90 if on else 160; v[300:320, 550:610] = 200 if on else 70
    elif name == "chroma-grade":     # colour grade drift: whole-picture V +-1 per picture at 60, luma untouched
        y, u, v = world(k, pan30)
        d = (k % 40) if (k % 80) < 40 else 80 - (k % 80)
        v = np.clip(v.astype(np.int16) + d // 4 - 5, 0, 255).astype(np.uint8)
        v = np.clip(v.astype(np.int16) + (k & 1), 0, 255).astype(np.uint8)
    elif name == "chroma60-static":  # still background, one sprite moving at 30 (encoder may send exact repeats), chroma HUD at 60
        y, u, v = planes(WORLDS[1][:H, :W])
        x = 200 + (k // 2) * 4 % 800
        y[300:340, x:x + 40] = 230; u[150:170, x // 2:x // 2 + 20] = 128; v[150:170, x // 2:x // 2 + 20] = 128
        u[20:40, 20:60] = 128 + int(30 * np.cos(k * 0.9)); v[20:40, 20:60] = 128 + int(30 * np.sin(k * 0.9))
    elif name == "luma1-edges":      # world at 30; one luma sample +1 at 60 at chosen spots (corners, block edges)
        y, u, v = world(k, pan30)
        spots = [(0, 0), (W - 1, H - 1), (15, 15), (16, 16), (W - 1, 0), (0, H - 1), (639, 359), (640, 360)]
        if k & 1:
            x, yy = spots[(k // 2) % len(spots)]
            y[yy, x] = np.uint8(min(255, int(y[yy, x]) + 1)) if y[yy, x] < 255 else 254
    elif name == "luma-pm1":         # +1 / -1 inside one block on the repeat slot: block sum and mean unchanged
        y, u, v = world(k, pan30)
        if k & 1:
            a, b = int(y[200, 200]), int(y[201, 207])
            y[200, 200] = a + 1 if a < 255 else a - 1
            y[201, 207] = b - 1 if b > 0 else b + 1
    elif name == "exact-then-faint":  # 2 s of a true 30 fps world (proof), then a faint 1-step dot moving at 60
        y, u, v = world(k, pan30)
        if k >= 120:
            x = 300 + k % 600; y[400:402, x:x + 2] = np.clip(y[400:402, x:x + 2].astype(int) + 1, 0, 255)
    elif name == "three-one":        # repeat slots: 3 exact, then one with a faint change, for ever
        y, u, v = world(k, pan30)
        if k % 8 == 7:
            y[500:506, 500:506] = np.clip(y[500:506, 500:506].astype(int) + 2, 0, 255)
    elif name == "pause-then-60":    # 2 s world 30, 2 s paused (identical), then 60 fps motion
        if k < 120: y, u, v = world(k, pan30)
        elif k < 240: y, u, v = world(119, pan30)
        else: y, u, v = world(119 + (k - 239), pan60)
    elif name == "static60":         # 60 fps game; static menu for 3 s; motion again
        if k < 120: y, u, v = world(k, pan60)
        elif k < 300: y, u, v = world(119, pan60)
        else: y, u, v = world(119 + (k - 299), pan60)
    elif name == "cut-on-slot":      # world at 30; scene cuts placed on the repeat slot (odd pictures)
        which = 0 if k < 151 else (1 if k < 251 else 2)
        y, u, v = world(k, pan30, which)
    elif name == "switch30to60":     # 3 s at 30 (proof), then 60
        y, u, v = world(k, pan30) if k < 180 else world(179 + (k - 179), pan60)
    elif name == "faint-vanishes":   # still bg + 30 sprite (exact repeats possible); a faint 1-step 4x4 patch moving at 60
        y, u, v = planes(WORLDS[1][:H, :W])
        x = 200 + (k // 2) * 4 % 800
        y[300:340, x:x + 40] = 230
        px = 100 + k % 900
        y[600:604, px:px + 4] = np.clip(y[600:604, px:px + 4].astype(int) + 1, 0, 255)
    elif name == "chroma-pause":     # paused game (identical luma), but a colour HUD keeps pulsing at 60
        y, u, v = world(min(k, 119), pan30)
        if k >= 120:
            u[40:60, 40:80] = 128 + int(30 * np.cos(k * 0.5))
    else:
        raise KeyError(name)
    return pack(y, u, v)


SCENES = {
    "chroma60": ["raw", "x264-cbr10", "x264-cbr4", "x265-cbr10", "x265-cbr4"],
    "chroma-blink": ["raw", "x264-cbr10", "x264-cbr4", "x265-cbr10", "x265-cbr4"],
    "chroma-grade": ["raw", "x264-cbr10", "x264-cbr4", "x265-cbr10", "x265-cbr4"],
    "chroma60-static": ["raw", "x264-cbr10", "x264-cbr4", "x265-cbr10", "x265-cbr4"],
    "chroma-pause": ["raw", "x264-cbr10", "x264-cbr4", "x265-cbr10", "x265-cbr4"],
    "luma1-edges": ["raw", "x265-cbr10"],
    "luma-pm1": ["raw", "x265-cbr10"],
    "exact-then-faint": ["raw", "x265-cbr10"],
    "three-one": ["raw", "x265-cbr10"],
    "pause-then-60": ["raw", "x265-cbr10"],
    "static60": ["raw", "x265-cbr10"],
    "cut-on-slot": ["raw", "x265-cbr10"],
    "switch30to60": ["raw", "x265-cbr10"],
    "faint-vanishes": ["raw", "x264-cbr4", "x265-cbr10", "x265-cbr4"],
}


def run(name, encs, frames, out):
    todo = [e for e in encs if not (out / f"{name}_{e}.txt").exists()]
    if not todo: return
    encoders = {e: cs.RawEncoder(W, H) if e == "raw" else cs.Encoder(e, W, H, 2) for e in todo}
    plan, prev = [], None
    chroma_only = []
    for k in range(frames):
        yuv = scene(name, k)
        new = prev is None or not np.array_equal(yuv, prev)
        chroma_only.append(bool(prev is not None and new and np.array_equal(yuv[:H], prev[:H])))
        plan.append((k / 60, new, False)); prev = yuv
        for enc in encoders.values():
            f = av.VideoFrame.from_ndarray(yuv, format="yuv420p"); f.pts = k; enc.add(f)
    for e, enc in encoders.items():
        units = enc.finish()
        diffs = cs.decoded_differences(units, enc.codec, W, H)
        assert len(diffs) == len(plan) == len(units), (len(diffs), len(units), len(plan))
        (out / f"{name}_{e}.txt").write_text("".join(
            f"{len(u)} {int(n)} {d[0]:.4f} {d[1]:.5f} {d[2]:.5f} {int(c)} {d[3]:.0f} {d[4]:.4f} {d[5]:.5f} " + " ".join(str(v) for v in d[6:]) + "\n"
            for u, (_, n, c), d in zip(units, plan, diffs)))
    (out / f"{name}.chromaonly.json").write_text(json.dumps([i for i, c in enumerate(chroma_only) if c]))
    print(name, todo, "new", sum(1 for p in plan if p[1]), "chroma-only new", sum(chroma_only), flush=True)


if __name__ == "__main__":
    out = pathlib.Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
    only = sys.argv[2:] or [n for n in SCENES if n.startswith("chroma")]
    for n in only:
        t = time.time(); run(n, SCENES[n], 420, out); print(f"  {time.time() - t:.0f} s", flush=True)
