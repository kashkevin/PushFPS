#!/usr/bin/env python3
"""The third review's investigation (19 November 2026): what the repeat's place of the rhythm looks
like for a true repeat and for a faint thing drawn at 60 fps. Linux model, software encoders.

  python3 cadence_probe.py levels <folder with tune/ holdout/ mixed/ from cadence_streams.py>
      medians and 5th/95th percentiles of the largest sample change, samples over 4 steps (per
      thousand), share of blocks touched, mean absolute change and share of one-way blocks: true
      repeats of the tune and hold-out sets by encoder; repeat-slot pictures of each mixed class;
      true repeats of the mixed world (the late streams before the faint thing appears)
  python3 cadence_probe.py place [frames]
      renders mixed scenes and true 30-in-60 scenes (cadence_streams.Mixed, Still, Scene), raw and
      through x264 at 10 and 4 Mbit/s, and reports how often the 16x16 block holding the largest
      sample change stays within one block from one repeat-slot picture to the next
"""
import glob, pathlib, re, sys, time
import numpy as np
import av
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cadence_streams as cs

NAMES = ["pixMax", "over4/1000", "touched", "absMean", "oneWay"]


def load(path):
    rows = [[float(x) for x in line.split()[:22]] for line in open(path)]
    return np.array(rows) if rows and len(rows[0]) >= 22 else None


def features(a, i):
    samples, blocks = a[0, 9], a[0, 19]
    return [a[i, 6], a[i, 12] / samples * 1e3, a[i, 19] / blocks, (a[i, 17] + a[i, 18]) / samples, a[i, 20] / blocks]


def show(label, values):
    v = np.array(values)
    print(f"{label:40s} n={len(v):5d} " + " ".join(f"{n}{np.percentile(v[:, k], [5, 50, 95]).round(3).tolist()}" for k, n in enumerate(NAMES)))


def levels(folder):
    for which in ("tune", "holdout"):
        groups = {}
        for p in glob.glob(f"{folder}/{which}/*.txt"):
            a = load(p)
            if a is None: continue
            for i in range(2, len(a)):
                if a[i, 1] == 0: groups.setdefault(p.rsplit("_", 1)[1][:-4], []).append(features(a, i))
        for k, v in sorted(groups.items()): show(f"{which} true repeats {k}", v)
    groups, world = {}, {}
    for p in glob.glob(f"{folder}/mixed/*.txt"):
        a = load(p); encoder = p.rsplit("_", 1)[1][:-4]; late = "late" in p
        kind = re.match(r"mixed-([a-z]+)", pathlib.Path(p).name).group(1)
        for i in range(3, len(a), 2):
            if late and i < 240: continue
            if a[i, 1] == 1: groups.setdefault((kind, encoder), []).append(features(a, i))
        if late:
            for i in range(20, 238):
                if a[i, 1] == 0: world.setdefault(encoder, []).append(features(a, i))
    for k, v in sorted(groups.items()): show(f"mixed {k[0]} {k[1]} (repeat slot)", v)
    for k, v in sorted(world.items()): show(f"mixed world true repeats {k}", v)


def place(frames):
    width, height = 1280, 720

    def pictures(name):
        if name.startswith("mixed:"):
            _, kind, amp, start = name.split(":")
            src = cs.Mixed(width, height, kind, ["coffee"], amp=float(amp), start=int(start))
            out, prev = [], None
            for k in range(frames):
                y = src.picture(k / 60, k, 0); out.append((y, prev is None or not np.array_equal(y, prev))); prev = y
            return out
        src = cs.Still(width=width, height=height, profile="slow", images=["coffee", "astronaut"], seed=5, frames=frames) if name == "still30" else \
              cs.Scene(width=width, height=height, profile="slowing", grain=0.0, seed=23, frames=frames)
        out, unique, y = [], 0, None
        for t, new, _ in cs.cadence("30", frames):
            if new or y is None: y = src.picture(t, unique, 0); unique += 1
            out.append((y, new))
        return out

    def decoded(encoder, frs):
        if encoder == "raw": return [f[0][:height] for f in frs]
        e = cs.Encoder(encoder, width, height, 1)
        for i, (y, _) in enumerate(frs):
            v = av.VideoFrame.from_ndarray(y, format="yuv420p"); v.pts = i; e.add(v)
        dec, lumas = av.CodecContext.create("h264", "r"), []
        for unit in e.finish() + [None]:
            for f in dec.decode(av.Packet(unit) if unit is not None else None):
                p = f.planes[0]; lumas.append(np.frombuffer(p, np.uint8).reshape(p.height, p.line_size)[:height, :width].copy())
        return lumas

    columns = (width + 15) // 16
    for name in ("mixed:blob:8:0", "mixed:blob:30:0", "mixed:square:12:0", "mixed:particle:6:0", "mixed:steps:0:0", "mixed:clouds:0:0",
                 "mixed:grain:1:0", "mixed:clean:0:9999", "still30", "scene30"):
        frs = pictures(name)
        for encoder in ("raw", "x264-cbr10", "x264-cbr4"):
            started, lumas, slots = time.time(), decoded(encoder, frs), []
            for i in range(1, len(lumas)):
                d = np.abs(lumas[i].astype(np.int32) - lumas[i - 1].astype(np.int32))
                if i % 2 == 0 or i < 20 or (not name.startswith("mixed") and frs[i][1]): continue
                bm = np.maximum.reduceat(np.maximum.reduceat(d, np.arange(0, d.shape[0], 16), axis=0), np.arange(0, d.shape[1], 16), axis=1)
                at = int(np.argmax(bm.astype(np.int64) * 65536 + np.arange(bm.size).reshape(bm.shape)))
                slots.append((at % columns, at // columns, int(d.max())))
            steps = [max(abs(a[0] - b[0]), abs(a[1] - b[1])) for a, b in zip(slots[1:], slots[:-1]) if a[2] and b[2]]
            exact = sum(1 for s in slots if s[2] == 0)
            near = f"{np.mean([s <= 1 for s in steps]):.2f}" if steps else "n/a (exact)"
            print(f"{name:20s} {encoder:10s} repeat-slot pictures {len(slots)}, exact {exact}, largest change stays within one block: {near} ({time.time() - started:.0f} s)", flush=True)


if __name__ == "__main__":
    if len(sys.argv) >= 3 and sys.argv[1] == "levels": levels(sys.argv[2])
    elif len(sys.argv) >= 2 and sys.argv[1] == "place": place(int(sys.argv[2]) if len(sys.argv) > 2 else 240)
    else: sys.exit(__doc__)
