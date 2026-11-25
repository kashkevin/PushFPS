#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Frame extrapolation versus interpolation for a 60 fps stream shown at 120 / 180 Hz.

A CPU reference (numpy + OpenCV DIS optical flow), not the product: it answers whether an
extrapolating generator is worth building into the engine, with numbers instead of an opinion.

Scenes are synthetic and rendered at 360 fps, so the true picture exists at every display
instant; every 6th frame is a "decoded stream frame". Three ways to fill the display:

  hold         show the newest real frame until the next one arrives (frame generation off)
  interpolate  blend real frames A and B along their optical flow; B and everything before it
               are shown `delay` source intervals late (1.0: PushFPS on a stream today,
               0.5: 2X with the learned pair phase)
  extrapolate  move the newest real frame B forward along the flow A->B (constant velocity)

Two scores per method, both PSNR/SSIM against a true 360 fps frame:
  fidelity  against the instant the generated frame claims to show (artefacts only)
  on time   against the instant it is on screen (artefacts and delay together); this is the
            number that matters for the hand on the controller
Also: `self error`, the error of predicting B from the pair before it, which exists at run
time without ground truth, and its correlation with the true error of the next prediction.
"""
import argparse, json, math, sys
import numpy as np, cv2
from skimage.metrics import structural_similarity

W, H, SUB = 640, 360, 6          # SUB ground-truth frames per source interval (360 fps truth, 60 fps source)
RNG = np.random.default_rng(807)

def texture(w, h, seed):
    r = np.random.default_rng(seed)
    img = np.zeros((h, w), np.float32)
    for octave in range(1, 7):                       # band-limited noise: something a game background might be
        s = 2 ** octave
        n = r.random((h // s + 2, w // s + 2), dtype=np.float32)
        img += cv2.resize(n, (w, h), interpolation=cv2.INTER_CUBIC) / octave
    img = (img - img.min()) / (img.max() - img.min())
    for _ in range(40):                              # hard edges: flow and warps fail on these first
        x, y = int(r.integers(0, w)), int(r.integers(0, h))
        cv2.rectangle(img, (x, y), (x + int(r.integers(8, 60)), y + int(r.integers(8, 40))), float(r.random()), -1 if r.random() < .5 else 2)
    return img

BG = texture(W * 4, H * 3, 1)
FG = texture(160, 160, 2)

def crop(x, y):
    m = np.float32([[1, 0, -x], [0, 1, -y]])
    return cv2.warpAffine(BG, m, (W, H), flags=cv2.INTER_LINEAR, borderMode=cv2.BORDER_REFLECT)

def paste(frame, x, y, size=120):
    x, y = int(round(x)), int(round(y))
    x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + size, W), min(y + size, H)
    if x1 > x0 and y1 > y0:
        frame[y0:y1, x0:x1] = FG[y0 - y:y1 - y, x0 - x:x1 - x]
    return frame

def hud(frame):
    cv2.rectangle(frame, (20, 20), (220, 50), 0.95, -1); cv2.rectangle(frame, (24, 24), (150, 46), 0.15, -1)
    for i in range(6):                                  # digits of an ammo counter: thin bright strokes
        cv2.rectangle(frame, (W - 190 + i * 26, H - 50), (W - 174 + i * 26, H - 22), 1.0, 2)
    return frame

# Each scene: t in source intervals -> frame. Speeds are pixels per source frame at 640x360
# (multiply by 3 for the same angular speed at 1080p).
def pan(t):            return crop(300 + 12 * t, 200 + 3 * t)                       # steady camera turn
def turn_start_stop(t):                                                              # stick pushed and released
    phase = (t % 16) / 16.0
    speed_integral = 16 * 14 * (phase - math.sin(2 * math.pi * phase) / (2 * math.pi)) / 2
    return crop(300 + 14 * 16 * math.floor(t / 16) / 2 + speed_integral, 220)
def object_over_static(t): return paste(crop(400, 300), 40 + 9 * t % (W - 80), 120)  # disocclusion behind a mover
def bounce(t):                                                                       # direction reversal
    period = 10.0; p = (t % period) / period
    x = 60 + (W - 240) * (2 * p if p < .5 else 2 - 2 * p)
    return paste(crop(400 + 2 * t, 300), x, 130)
def pan_with_hud(t):   return hud(crop(300 + 10 * t, 200))                           # HUD must stay put
SCENES = {"steady pan": pan, "turn start/stop": turn_start_stop, "object over static": object_over_static,
          "bounce": bounce, "pan with HUD": pan_with_hud}

DIS = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
def flow(a, b):
    """Dense flow f with a(p) ~ b(p + f(p))."""
    return DIS.calc((a * 255).astype(np.uint8), (b * 255).astype(np.uint8), None)

GRID_X, GRID_Y = np.meshgrid(np.arange(W, dtype=np.float32), np.arange(H, dtype=np.float32))
def sample(img, dx, dy):
    return cv2.remap(img, GRID_X + dx, GRID_Y + dy, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)

def extrapolate(b, flow_ba, d, iterations=2):
    """B moved d source intervals forward. flow_ba: b(p) ~ a(p + flow_ba(p)); content that is at q in B
    is at q - d*flow_ba(q) later, so the picture at p comes from the q with p = q - d*flow_ba(q)."""
    dx = d * flow_ba[..., 0]; dy = d * flow_ba[..., 1]
    for _ in range(iterations):                       # fixed point: read the flow where the content comes from
        fx = sample(flow_ba[..., 0], dx, dy); fy = sample(flow_ba[..., 1], dx, dy)
        dx, dy = d * fx, d * fy
    return sample(b, dx, dy)

def interpolate(a, b, flow_ab, flow_ba, t):
    """Picture at fraction t between A and B, both warped to t and blended."""
    from_a = sample(a, -t * sample(flow_ab[..., 0], -t * flow_ab[..., 0], -t * flow_ab[..., 1]),
                       -t * sample(flow_ab[..., 1], -t * flow_ab[..., 0], -t * flow_ab[..., 1]))
    s = 1 - t
    from_b = sample(b, -s * sample(flow_ba[..., 0], -s * flow_ba[..., 0], -s * flow_ba[..., 1]),
                       -s * sample(flow_ba[..., 1], -s * flow_ba[..., 0], -s * flow_ba[..., 1]))
    return (1 - t) * from_a + t * from_b

def psnr(x, y):
    mse = float(np.mean((x - y) ** 2))
    return 99.0 if mse <= 1e-10 else 10 * math.log10(1.0 / mse)
def ssim(x, y): return float(structural_similarity(x, y, data_range=1.0))

def run(multiplier, intervals, interp_delays):
    phases = [k / multiplier for k in range(1, multiplier)]          # generated instants within an interval
    out = {}; pooled = []
    for name, scene in SCENES.items():
        truth = lambda units: scene(units / SUB)                       # units: 360 fps ticks
        acc = {}
        def add(key, shown, target_units):
            ref = truth(target_units)
            acc.setdefault(key, []).append((psnr(shown, ref), ssim(shown, ref)))
        self_err, next_err = [], []
        prev_pred_for_b = None
        for n in range(2, intervals):
            a, b = truth((n - 1) * SUB), truth(n * SUB)
            f_ab, f_ba = flow(a, b), flow(b, a)
            if prev_pred_for_b is not None:                            # how well did the last pair predict this B?
                self_err.append(float(np.mean(np.abs(prev_pred_for_b - b))))
            prev_pred_for_b = None
            worst_next = 0.0
            for ph in phases:
                units = round(ph * SUB)                                # 2X: 3, 3X: 2 and 4
                now = n * SUB + units                                  # display instant after B
                # hold: B is still on screen
                add("hold|on time", b, now)
                # extrapolate B forward to the display instant
                e = extrapolate(b, f_ba, ph)
                add("extrapolate|fidelity", e, now); add("extrapolate|on time", e, now)
                # the same with half the predicted movement: what a cautious generator would show
                add("extrapolate, half gain|on time", extrapolate(b, f_ba, ph * 0.5), now)
                worst_next = max(worst_next, float(np.mean(np.abs(e - truth(now)))))
                # interpolate between A and B: correct for (n-1)+ph, on screen `delay` intervals later
                i = interpolate(a, b, f_ab, f_ba, ph)
                add("interpolate|fidelity", i, (n - 1) * SUB + units)
                for delay in interp_delays:
                    add(f"interpolate, {delay} interval late|on time", i, round((n - 1 + ph + delay) * SUB))
            # real frames: on time for hold / extrapolate, late for interpolate
            for delay in interp_delays:
                add(f"interpolate, {delay} interval late|real frames on time", b, round((n + delay) * SUB))
            prev_pred_for_b = extrapolate(b, f_ba, 1.0)                # prediction of the next real frame
            if len(self_err) > 0: next_err.append((self_err[-1], worst_next)); pooled.append((self_err[-1], worst_next))
        res = {k: {"psnr": round(float(np.mean([v[0] for v in vs])), 2), "ssim": round(float(np.mean([v[1] for v in vs])), 4),
                   "psnr_p5": round(float(np.percentile([v[0] for v in vs], 5)), 2)} for k, vs in acc.items()}
        if len(next_err) > 3:
            xs, ys = np.array(next_err).T
            res["self error vs next error (Pearson r)"] = round(float(np.corrcoef(xs, ys)[0, 1]), 3) if xs.std() > 0 and ys.std() > 0 else None
        res["mean self error"] = round(float(np.mean(self_err)), 4)
        out[name] = res
    xs, ys = np.array(pooled).T
    out["_pooled self error vs next error (Pearson r)"] = round(float(np.corrcoef(xs, ys)[0, 1]), 3)
    return out

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--intervals", type=int, default=34)
    ap.add_argument("--json", default="")
    args = ap.parse_args()
    report = {}
    for multiplier in (2, 3):
        report[f"{multiplier}X (60 -> {60 * multiplier})"] = run(multiplier, args.intervals, (0.5, 1.0))
    if args.json: open(args.json, "w").write(json.dumps(report, indent=1))
    for mode, scenes in report.items():
        print(f"\n== {mode}  [{W}x{H}, DIS flow, {args.intervals - 2} pairs per scene]")
        keys = ["hold|on time", "extrapolate|on time", "interpolate, 0.5 interval late|on time", "interpolate, 1.0 interval late|on time",
                "extrapolate|fidelity", "interpolate|fidelity"]
        print(f"{'scene':20s} " + " ".join(f"{k.replace('interpolate','interp').replace('extrapolate','extrap').replace(' interval late','')[:22]:>22s}" for k in keys))
        pooled = scenes.pop("_pooled self error vs next error (Pearson r)")
        for scene, res in scenes.items():
            print(f"{scene:20s} " + " ".join(f"{res[k]['psnr']:6.2f} dB / {res[k]['ssim']:.3f}    " for k in keys))
        print("   extrapolate at half gain, on time: " + ", ".join(f"{s}: {r['extrapolate, half gain|on time']['psnr']} dB" for s, r in scenes.items()))
        print("   extrapolate, 5th percentile PSNR on time: " + ", ".join(f"{s}: {r['extrapolate|on time']['psnr_p5']} dB" for s, r in scenes.items()))
        print("   self error (mean abs, 0..1) per scene: " + ", ".join(f"{s}: {r['mean self error']}" for s, r in scenes.items()))
        print(f"   self error vs error of the next extrapolated frames, all scenes pooled, Pearson r: {pooled}"
              + "; within one scene: " + ", ".join(f"{s}: {r.get('self error vs next error (Pearson r)')}" for s, r in scenes.items()))
