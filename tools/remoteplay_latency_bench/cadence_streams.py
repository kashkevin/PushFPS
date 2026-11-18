#!/usr/bin/env python3
"""Makes encoded test streams with a known picture cadence and writes each access unit's size.

A console that renders 30 fps into a 60 Hz output shows every picture twice (A A B B). The Remote
Play encoder then receives two identical pictures in a row. This script renders synthetic game-like
scenes with a chosen cadence, encodes them with real encoders in low-delay settings, and writes
one line per access unit: "<bytes> <1 if the picture is new, 0 if it repeats the previous one>
<largest block difference> <mean block difference> <share of blocks changed by more than 1>", the
last three from decoding the stream and comparing 16x16 block means of luma with the previous
decoded picture (8-bit steps).

The encoders are libx264 and libx265, not the PS5's hardware encoder: the result shows whether the
method works on ordinary low-delay streams, not that it works on a PS5.

  python3 cadence_streams.py <output folder> [--frames 600] [--size 1280x720]
"""
import argparse, json, os, pathlib, sys
import numpy as np
import av


def scene(width, height, seed):
    """A large textured world to pan over, a few moving objects, a static HUD."""
    rng = np.random.default_rng(seed)
    world_w, world_h = width * 3, height * 2
    base = rng.integers(0, 255, (world_h // 8, world_w // 8, 3), dtype=np.uint8)
    world = np.kron(base, np.ones((8, 8, 1), dtype=np.uint8)).astype(np.float32)
    # soften block edges a little so it is not a pure checker pattern
    world = (world + np.roll(world, 3, 0) + np.roll(world, 5, 1)) / 3
    return world, rng


def render(world, rng, width, height, t, speed, grain):
    """Picture at game time t (seconds). speed in pixels per second."""
    wh, ww, _ = world.shape
    x = int(t * speed) % (ww - width)
    y = int(40 * np.sin(t * 0.7) + 60) % (wh - height)
    frame = world[y:y + height, x:x + width].copy()
    for k in range(4):  # moving objects
        ox = int((t * (90 + 60 * k) + 200 * k) % (width - 80))
        oy = int(height * (0.2 + 0.15 * k) + 30 * np.sin(t * (1 + k)))
        frame[oy:oy + 60, ox:ox + 80] = (40 * k, 255 - 50 * k, 128)
    frame[20:60, 20:260] = 235       # HUD bar
    frame[height - 90:height - 20, width - 250:width - 20] = 16  # minimap
    if grain:
        frame += rng.normal(0, grain, frame.shape).astype(np.float32)
    return np.clip(frame, 0, 255).astype(np.uint8)


def cadence(kind, frames):
    """For each 60 Hz output frame: (game time, is a new picture)."""
    out, t, held = [], 0.0, 0
    rng = np.random.default_rng(7)
    for i in range(frames):
        if kind == "60":
            new = True
        elif kind == "30":
            new = i % 2 == 0
        elif kind == "30-hitch":       # a late game frame now and then: A A B B B C C
            if held >= 2 and rng.random() > 0.06: new = True
            elif held >= 3: new = True
            else: new = held == 0 and i == 0
        elif kind == "30-then-60":
            new = i % 2 == 0 if i < frames // 2 else True
        else:
            raise ValueError(kind)
        if i == 0: new = True
        if new: t, held = i / 60.0, 1
        else: held += 1
        out.append((t, new))
    return out


def motion_speed(profile, t):
    if profile == "fast": return 900
    if profile == "slow": return 40
    if profile == "mixed":  # fast, then nearly still, then slow, then fast
        phase = int(t / 2.5) % 4
        return (900, 0, 40, 500)[phase]
    raise ValueError(profile)


ENCODERS = {
    # name: (codec, options)  -- all: no B-frames, no lookahead, one reference chain
    "x264-cbr15": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "15M", "maxrate": "15M", "bufsize": "500k", "g": "100000", "bf": "0"}),
    "x264-cbr5": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "5M", "maxrate": "5M", "bufsize": "170k", "g": "100000", "bf": "0"}),
    "x264-cbr30": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "30M", "maxrate": "30M", "bufsize": "1M", "g": "100000", "bf": "0"}),
    "x264-intra-refresh": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "15M", "maxrate": "15M", "bufsize": "500k", "g": "60", "bf": "0", "intra-refresh": "1"}),
    "x264-filler": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "15M", "maxrate": "15M", "bufsize": "500k", "g": "100000", "bf": "0", "nal-hrd": "cbr"}),
    "x264-slices4": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "15M", "maxrate": "15M", "bufsize": "500k", "g": "100000", "bf": "0", "slices": "4"}),
    "x265-cbr15": ("libx265", {"preset": "ultrafast", "tune": "zerolatency", "b": "15M", "maxrate": "15M", "bufsize": "500k", "g": "100000", "bf": "0"}),
}


def encode(pictures, width, height, codec, options):
    container = av.open(os.devnull, "w", format="h264" if codec == "libx264" else "hevc")
    stream = container.add_stream(codec, rate=60)
    stream.width, stream.height, stream.pix_fmt = width, height, "yuv420p"
    opts = dict(options)
    stream.bit_rate = int(opts.pop("b").rstrip("M")) * 1_000_000
    stream.options = opts
    sizes, units = [], []
    for index, picture in enumerate(pictures):
        frame = av.VideoFrame.from_ndarray(picture, format="rgb24")
        frame.pts = index
        for packet in stream.encode(frame):
            sizes.append(packet.size); units.append(bytes(packet))
    for packet in stream.encode(None):
        sizes.append(packet.size); units.append(bytes(packet))
    container.close()
    return sizes, units


def block_means(luma, block):
    """Mean luma of every block x block square (what a small GPU pass would produce)."""
    h, w = luma.shape[0] // block * block, luma.shape[1] // block * block
    return luma[:h, :w].astype(np.float32).reshape(h // block, block, w // block, block).mean(axis=(1, 3))


def decoded_differences(units, codec, block=16):
    """Decodes the stream and compares each picture's block means with the previous picture's.
    Returns per access unit (largest block difference, mean block difference, share of blocks
    that changed by more than one 8-bit step), in 8-bit luma steps."""
    decoder = av.CodecContext.create("h264" if codec == "libx264" else "hevc", "r")
    out, previous = [], None
    for unit in units:
        for frame in decoder.decode(av.Packet(unit)):
            grid = block_means(frame.to_ndarray(format="gray"), block)
            if previous is None: out.append((255.0, 255.0, 1.0))
            else:
                d = np.abs(grid - previous)
                out.append((float(d.max()), float(d.mean()), float((d > 1.0).mean())))
            previous = grid
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--size", default="1280x720")
    parser.add_argument("--encoders", default=",".join(ENCODERS))
    args = parser.parse_args()
    width, height = (int(v) for v in args.size.split("x"))
    args.out.mkdir(parents=True, exist_ok=True)
    index = []
    for kind, profile, grain in (("30", "fast", 0), ("30", "mixed", 0), ("30", "slow", 2.0), ("30-hitch", "fast", 0),
                                 ("60", "fast", 0), ("60", "mixed", 0), ("60", "slow", 2.0), ("30-then-60", "mixed", 0)):
        world, rng = scene(width, height, seed=11)
        plan = cadence(kind, args.frames)
        pictures, last = [], None
        for t, new in plan:
            if new or last is None:
                last = render(world, rng, width, height, t, motion_speed(profile, t), grain)
            pictures.append(last)          # a repeat is the very same picture, as on a console
        for name in args.encoders.split(","):
            codec, options = ENCODERS[name]
            label = f"{kind}_{profile}{'_grain' if grain else ''}_{name}"
            sizes, units = encode(pictures, width, height, codec, options)
            diffs = decoded_differences(units, codec)
            if len(diffs) != len(plan):
                print(f"{label}: decoder returned {len(diffs)} pictures for {len(plan)} frames", file=sys.stderr); return 1
            if len(sizes) != len(plan):
                print(f"{label}: encoder returned {len(sizes)} access units for {len(plan)} frames", file=sys.stderr); return 1
            (args.out / (label + ".txt")).write_text("".join(f"{s} {int(n)} {d[0]:.4f} {d[1]:.5f} {d[2]:.5f}\n" for s, (_, n), d in zip(sizes, plan, diffs)))
            rmax = [d[0] for d, (_, n) in zip(diffs[2:], plan[2:]) if not n]; nmax = [d[0] for d, (_, n) in zip(diffs[2:], plan[2:]) if n]
            rmean = [d[1] for d, (_, n) in zip(diffs[2:], plan[2:]) if not n]; nmean = [d[1] for d, (_, n) in zip(diffs[2:], plan[2:]) if n]
            new = [s for s, (_, n) in zip(sizes[2:], plan[2:]) if n]; rep = [s for s, (_, n) in zip(sizes[2:], plan[2:]) if not n]
            index.append({"label": label, "cadence": kind, "motion": profile, "grain": grain, "encoder": name, "width": width, "height": height,
                          "frames": len(sizes), "newMedian": int(np.median(new)) if new else None, "repeatMedian": int(np.median(rep)) if rep else None,
                          "repeatMax": int(max(rep)) if rep else None, "newMin": int(min(new)) if new else None,
                          "repeatMaxBlockDiff": max(rmax) if rmax else None, "newMinBlockDiff": min(nmax) if nmax else None,
                          "repeatMaxMeanDiff": max(rmean) if rmean else None, "newMinMeanDiff": min(nmean) if nmean else None})
            r = index[-1]
            def fmt(v): return "   n/a" if v is None else f"{v:6.2f}"
            print(f"{label:40s} size rep/new median {('%.2f' % (r['repeatMedian'] / r['newMedian'])) if rep else ' n/a'} | block diff: repeats max {fmt(r['repeatMaxBlockDiff'])}  new min {fmt(r['newMinBlockDiff'])} | mean diff: repeats max {fmt(r['repeatMaxMeanDiff'])}  new min {fmt(r['newMinMeanDiff'])}", flush=True)
    (args.out / "index.json").write_text(json.dumps(index, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
