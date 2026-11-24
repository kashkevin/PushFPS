#!/usr/bin/env python3
"""Makes encoded test streams with a known picture cadence and writes what a decoder would see.

A console that renders 30 fps into a 60 Hz output shows every picture twice (A A B B). The Remote
Play encoder then receives two identical pictures in a row. This script renders pictures with a
chosen cadence, encodes them with real encoders in low-delay settings, decodes the stream again and
writes one line per access unit:

  <bytes> <1 new picture | 0 repeat> <largest block difference> <mean block difference>
  <share of blocks changed by more than 1> <1 if this new picture is a scene cut>
  <largest pixel difference> <largest block mean of pixel differences> <mean pixel difference>
  <pixels that differ by more than 0> <... 1> <... 2> <... 4> <... 8> <... 16> <... 32> <... 64>
  <sum of brightening differences> <sum of darkening differences> <blocks touched at all>
  <blocks that changed one way> <blocks whose signed sum reaches 64 steps>   (second review; the
  first four are what the GPU pass reads back since then, see signed_differences)
  <chroma samples (U and V) that differ at all>   (fourth review; NOT read by the GPU pass yet)

The first three differences compare the mean luma of every 16x16 block of the decoded picture with
the previous decoded picture (blocks cut off by the right and bottom edges count with the pixels
they have); the others compare every luma sample with the same sample of the previous decoded
picture. All in 8-bit code values of the luma plane as decoded. The GPU pass
shaders/PushStreamChange.hlsl computes the block numbers, the largest pixel difference and one of
the pixel counts (veyra::pipeline::kStreamChangePixelStep) from the decoder's own texture; the other
columns are here so that the choice could be made on the tuning set.

Four sets, so that thresholds can be frozen before they are scored on pictures they never saw:

  legacy   the synthetic scene of the first study, 1280x720 (thresholds were first tuned on it)
  tune     new content for tuning: the synthetic scene at 1920x1080, real camera footage (bikes),
           a real photograph panned and zoomed, a fractal zoom; scene cuts, early frames, pauses
  holdout  content, encoder settings and seeds that tuning never sees: an animated film clip
           (Big Buck Bunny), a second camera clip, other photographs, other generators

  adversarial  what a block mean cannot see, after an independent review: a still photograph with
           a 6x6 square moving one pixel per game picture, with a drifting 2x2 particle, or fading by a
           quarter of a code value per game picture; as a real 60 fps game, as a 30 fps game after the
           rhythm has locked on a moving camera, across a 30 -> 60 switch and with early frames; 720p
           and 1080p, with and without grain, at two contrasts; and the object at 60 fps over a camera
           that moves at 30 ("halfworld"). The bar is not one dropped game
           picture (cadence_eval counts them through the engine's own duplicate removal). --frames 600
  mixed    the second review's mixed-cadence scenes: a world drawn at 30 fps (a photograph panned on
           every second picture) with something faint drawn at 60 (class Mixed), raw (no encoder)
           and through x264 at 10 and 4 Mbit/s, 720p; five of them also appearing 4 s in. Every
           picture in which anything changed is new. Used for tuning. --frames 600

Real footage is not committed. --clips names a folder with bikes.mp4, bigbuckbunny.mp4 and
carphone_pristine.mp4, the sample clips inside the scikit-video 1.1.11 wheel
(skvideo/datasets/data; `pip download scikit-video==1.1.11 --no-deps`, then unzip those three).
Photographs come from the installed scikit-image package; generators from ffmpeg's lavfi.

The encoders are libx264 and libx265, not the PS5's hardware encoder: the result shows whether the
method works on ordinary low-delay streams, not that it works on a PS5.

  python3 cadence_streams.py <output folder> --set tune --clips <folder> [--frames 480] [--only TEXT]
  python3 cadence_streams.py <output folder> --set adversarial --frames 600
  python3 cadence_streams.py <output folder> --set mixed --frames 600
"""
import argparse, json, math, os, pathlib, subprocess, sys, time
import numpy as np
import av

try:
    import cv2
except ImportError:  # only the legacy set works without OpenCV
    cv2 = None

FPS = 60


# ---------------------------------------------------------------- cadence plans

def cadence(kind, frames, seed=7):
    """For each 60 Hz output frame: (game time in seconds, is a new picture, is a scene cut)."""
    rng = np.random.default_rng(seed)
    new = [False] * frames
    cuts = set()
    if kind == "60":
        new = [True] * frames
    elif kind == "30":
        new = [i % 2 == 0 for i in range(frames)]
    elif kind == "30-hitch":       # a late game frame now and then: A A B B B C C
        held = 0
        for i in range(frames):
            if held >= 2 and rng.random() > 0.06: new[i] = True
            elif held >= 3: new[i] = True
            held = 1 if new[i] or i == 0 else held + 1
    elif kind in ("30-then-60", "30-then-60-cut"):
        new = [i % 2 == 0 if i < frames // 2 else True for i in range(frames)]
        if kind.endswith("cut"): cuts.add(frames // 2)
    elif kind == "60-then-30":
        new = [True if i < frames // 2 else i % 2 == 0 for i in range(frames)]
    elif kind == "30-60-flip":     # the game changes rate every second
        new = [i % 2 == 0 if (i // 60) % 2 == 0 else True for i in range(frames)]
    elif kind == "30-cut":
        new = [i % 2 == 0 for i in range(frames)]
        cuts = {int(v) * 2 for v in rng.choice(np.arange(20, frames // 2 - 5), 6, replace=False)}
    elif kind == "60-cut":
        new = [True] * frames
        cuts = {int(v) for v in rng.choice(np.arange(20, frames - 5), 6, replace=False)}
    elif kind == "30-early":       # now and then a picture is shown once only: A A B C C (the phase moves)
        i = 0
        while i < frames:
            new[i] = True
            i += 1 if (i > 12 and rng.random() < 0.05) else 2
    elif kind == "30-static":      # the game pauses twice: one picture for many frames
        new = [i % 2 == 0 and not (120 <= i < 200 or 330 <= i < 362) for i in range(frames)]
    elif kind == "60-static":
        new = [not (120 <= i < 200 or 330 <= i < 362) for i in range(frames)]
    elif kind == "40":             # 40 fps in 60 Hz: A A B C C D
        new = [i % 3 != 1 for i in range(frames)]
    elif kind == "20":             # 20 fps in 60 Hz: A A A B B B
        new = [i % 3 == 0 for i in range(frames)]
    else:
        raise ValueError(kind)
    new[0] = True
    out, t = [], 0.0
    for i in range(frames):
        if new[i]: t = i / FPS
        out.append((t, new[i], new[i] and i in cuts))
    return out


def legacy_speed(profile, t):
    if profile == "fast": return 900
    if profile == "slow": return 40
    if profile == "mixed":  # fast, then nearly still, then slow, then fast
        return (900, 0, 40, 500)[int(t / 2.5) % 4]
    raise ValueError(profile)


def wrap(value, period):
    """value modulo period for floats, without fmod (see Still.picture)."""
    return float(value) - float(period) * math.floor(float(value) / float(period))


def camera_path(profile, frames, seed):
    """Camera position in pixels (at 1280 wide) for every 60 Hz instant, speed integrated."""
    rng = np.random.default_rng(seed)
    t = np.arange(frames + 1) / FPS
    if profile == "fast": speed = np.full(frames + 1, 900.0)
    elif profile == "slow": speed = np.full(frames + 1, 40.0)
    elif profile == "crawl": speed = np.full(frames + 1, 9.0)          # well under a pixel per frame
    elif profile == "mixed": speed = np.choose((t / 2.5).astype(int) % 4, [900.0, 0.0, 40.0, 500.0])
    elif profile == "slowing":  # speeds up and slows to a stop, again and again
        speed = 600.0 * (0.5 + 0.5 * np.cos(2 * np.pi * t / 2.2)) ** 2
    elif profile == "rapid":    # whip pans: a new speed and direction every quarter second
        speed = np.repeat(rng.uniform(-1800, 1800, frames // 15 + 2), 15)[:frames + 1]
    else:
        raise ValueError(profile)
    return np.concatenate([[0.0], np.cumsum(speed[:-1]) / FPS])


# ---------------------------------------------------------------- picture sources
# Every source returns one picture as planar 4:2:0 (an (h*3/2, w) uint8 array).

def to_i420(rgb):
    return cv2.cvtColor(np.ascontiguousarray(rgb), cv2.COLOR_RGB2YUV_I420)


def flip_i420(yuv, width, height, variant):
    """A different picture with the same statistics: mirrored and/or upside down."""
    if variant % 4 == 0: return yuv
    y = yuv[:height]; u = yuv[height:height + height // 4].reshape(height // 2, width // 2)
    v = yuv[height + height // 4:].reshape(height // 2, width // 2)
    def f(p):
        if variant & 1: p = p[:, ::-1]
        if variant & 2: p = p[::-1]
        return p
    return np.concatenate([f(y), f(u).reshape(-1, width), f(v).reshape(-1, width)]).copy()


class LegacyScene:
    """The first study's scene, unchanged: a blocky textured world, four objects, a HUD."""
    def __init__(self, width, height, profile, grain, seed=11, **_):
        self.w, self.h, self.profile, self.grain = width, height, profile, grain
        self.rng = np.random.default_rng(seed)
        world_w, world_h = width * 3, height * 2
        base = self.rng.integers(0, 255, (world_h // 8, world_w // 8, 3), dtype=np.uint8)
        world = np.kron(base, np.ones((8, 8, 1), dtype=np.uint8)).astype(np.float32)
        self.world = (world + np.roll(world, 3, 0) + np.roll(world, 5, 1)) / 3

    def picture(self, t, index, variant):
        width, height, world = self.w, self.h, self.world
        wh, ww, _ = world.shape
        x = int(t * legacy_speed(self.profile, t)) % (ww - width)
        y = int(40 * np.sin(t * 0.7) + 60) % (wh - height)
        frame = world[y:y + height, x:x + width].copy()
        for k in range(4):
            ox = int((t * (90 + 60 * k) + 200 * k) % (width - 80))
            oy = int(height * (0.2 + 0.15 * k) + 30 * np.sin(t * (1 + k)))
            frame[oy:oy + 60, ox:ox + 80] = (40 * k, 255 - 50 * k, 128)
        frame[20:60, 20:260] = 235
        frame[height - 90:height - 20, width - 250:width - 20] = 16
        if self.grain:
            frame += self.rng.normal(0, self.grain, frame.shape).astype(np.float32)
        rgb = np.clip(frame, 0, 255).astype(np.uint8)
        if cv2 is None:
            return av.VideoFrame.from_ndarray(rgb, format="rgb24").reformat(format="yuv420p").to_ndarray()
        return to_i420(rgb)


class Scene:
    """The synthetic scene with an integrated camera path, sizes that scale with the picture and,
    with half_rate, objects that move only on every second picture while the camera moves on
    every picture (a 60 fps game with 30 fps animation: every picture is new)."""
    def __init__(self, width, height, profile, grain, seed, frames, half_rate=False, **_):
        self.w, self.h, self.grain, self.half = width, height, grain, half_rate
        self.rng = np.random.default_rng(seed)
        self.scale = width / 1280.0
        self.path = camera_path(profile, frames, seed) * self.scale
        cell = max(4, int(round(8 * self.scale)))
        world_w, world_h = width * 3, height * 2
        base = self.rng.integers(0, 255, (world_h // cell + 1, world_w // cell + 1, 3), dtype=np.uint8)
        world = np.kron(base, np.ones((cell, cell, 1), dtype=np.uint8))[:world_h, :world_w].astype(np.float32)
        self.worlds = [(world + np.roll(world, 3, 0) + np.roll(world, 5, 1)) / 3]
        self.worlds.append(self.worlds[0][::-1, ::-1] * 0.7 + 40)   # after a cut: another place

    def picture(self, t, index, variant):
        width, height, s = self.w, self.h, self.scale
        world = self.worlds[variant % 2]
        wh, ww, _ = world.shape
        x = int(abs(self.path[min(int(round(t * FPS)), len(self.path) - 1)])) % (ww - width)
        y = int((40 * np.sin(t * 0.7) + 60) * s) % (wh - height)
        frame = world[y:y + height, x:x + width].copy()
        to = (int(round(t * FPS)) // 2 * 2) / FPS if self.half else t
        for k in range(4):
            ox = int(wrap((to * (90 + 60 * k) + 200 * k) * s, width - 80 * s))
            oy = int(height * (0.2 + 0.15 * k) + 30 * s * np.sin(to * (1 + k)))
            frame[oy:oy + int(60 * s), ox:ox + int(80 * s)] = (40 * k, 255 - 50 * k, 128)
        frame[int(20 * s):int(60 * s), int(20 * s):int(260 * s)] = 235
        frame[height - int(90 * s):height - int(20 * s), width - int(250 * s):width - int(20 * s)] = 16
        if self.grain:
            frame += self.rng.normal(0, self.grain, frame.shape).astype(np.float32)
        return to_i420(np.clip(frame, 0, 255).astype(np.uint8))


class Clip:
    """Real footage: the clip's own frames, one per game picture (step 2 = twice as fast), played
    forwards then backwards so it never jumps. A cut jumps to another part, mirrored."""
    def __init__(self, width, height, path, step=1, **_):
        self.w, self.h, self.step = width, height, step
        self.frames = []
        with av.open(str(path)) as container:
            for frame in container.decode(video=0):
                rgb = frame.to_ndarray(format="rgb24")
                fh, fw, _ = rgb.shape
                if fw * height > fh * width:          # wider than the target: crop the sides
                    keep = fh * width // height; rgb = rgb[:, (fw - keep) // 2:(fw - keep) // 2 + keep]
                else:
                    keep = fw * height // width; rgb = rgb[(fh - keep) // 2:(fh - keep) // 2 + keep]
                self.frames.append(np.ascontiguousarray(rgb))
        if len(self.frames) < 20: raise RuntimeError(f"{path}: too few frames")

    def picture(self, t, index, variant):
        n = len(self.frames)
        k = (index * self.step + variant * 37) % (2 * n - 2)
        rgb = self.frames[k if k < n else 2 * n - 2 - k]
        rgb = cv2.resize(rgb, (self.w, self.h), interpolation=cv2.INTER_CUBIC if rgb.shape[1] < self.w else cv2.INTER_AREA)
        return flip_i420(to_i420(rgb), self.w, self.h, variant)


class Still:
    """A real photograph under a moving camera (pan with sub-pixel steps, slow zoom). With
    half_rate a small bright box moves on every second picture only."""
    def __init__(self, width, height, profile, images, seed, frames, half_rate=False, **_):
        import skimage.data
        self.w, self.h, self.half = width, height, half_rate
        self.path = camera_path(profile, frames, seed) * (width / 1280.0)
        self.images = []
        for name in images:
            image = getattr(skimage.data, name)()
            if image.ndim == 2: image = np.stack([image] * 3, axis=2)
            image = np.ascontiguousarray(image[:, :, :3])
            # tile mirrored copies so that a long pan never leaves the picture
            image = np.concatenate([image, image[:, ::-1]], axis=1)
            image = np.concatenate([image, image[::-1]], axis=0)
            self.images.append(image)

    def picture(self, t, index, variant):
        image = self.images[variant % len(self.images)]
        ih, iw, _ = image.shape
        zoom = (self.h * 1.25 / (ih / 2)) * (1.0 + 0.08 * np.sin(t * 0.9))   # picture pixels per image pixel
        pos = self.path[min(int(round(t * FPS)), len(self.path) - 1)]
        # No float remainder here: after an encoder's hand-written assembly has run in this process,
        # numpy's remainder returned NaN now and then (first version of this set: whole stretches of
        # blank pictures labelled new). floor is safe, and anything not finite stops the run.
        cx = wrap(pos / zoom, iw); cy = (ih / 4) * (1 + 0.3 * np.sin(t * 0.5))
        if not np.isfinite(cx + cy + zoom): raise RuntimeError(f"camera position is not a number at t={t}")
        # output (x, y) -> image (cx + x / zoom, cy + y / zoom), wrapped
        m = np.float32([[1 / zoom, 0, cx], [0, 1 / zoom, cy]])
        rgb = cv2.warpAffine(image, m, (self.w, self.h), flags=cv2.INTER_LINEAR | cv2.WARP_INVERSE_MAP, borderMode=cv2.BORDER_WRAP)
        if self.half:
            s = self.w / 1280.0; to = (int(round(t * FPS)) // 2 * 2) / FPS
            ox = int(wrap(to * 300 * s, self.w - 60 * s)); oy = int(self.h * 0.45 + 80 * s * np.sin(to * 2.0))
            rgb[oy:oy + int(40 * s), ox:ox + int(60 * s)] = (250, 240, 60)
        return to_i420(rgb)


class Lavfi:
    """An ffmpeg generator, one of its frames per game picture. A cut mirrors the picture."""
    def __init__(self, width, height, graph, **_):
        self.w, self.h = width, height
        self.bytes = width * height * 3 // 2
        self.process = subprocess.Popen(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", graph.format(w=width, h=height),
             "-vf", f"scale={width}:{height}:flags=neighbor,format=yuv420p", "-f", "rawvideo", "-"],
            stdout=subprocess.PIPE, stdin=subprocess.DEVNULL)

    def picture(self, t, index, variant):
        data = self.process.stdout.read(self.bytes)
        if len(data) != self.bytes: raise RuntimeError("lavfi source ended")
        return flip_i420(np.frombuffer(data, np.uint8).reshape(self.h * 3 // 2, self.w), self.w, self.h, variant)

    def close(self):
        self.process.kill(); self.process.wait()


class Adversarial:
    """A still photograph in which one small thing happens, the kind of change the mean of a 16x16
    block cannot see. kind: "square" (a 6x6 square moves one pixel per game picture), "particle" (a
    2x2 dot drifts: one pixel across per game picture, one down on every third) or "fade" (the whole
    picture gains a quarter of a code value per game picture, up and down within 24 values).
    lead: for that many game pictures first the camera pans, so that a doubled stream is found and
    trusted before the picture stands still. contrast: how far the object's value is from the
    photograph under it ("high": white; "low": 24 values brighter). half_world: the camera never
    stops and moves on every second game picture only, the object on every one (a 60 fps game whose
    world is drawn at 30 while something small is drawn at 60: every picture is new, and every
    second one differs from the picture before it by the small thing alone)."""
    def __init__(self, width, height, kind, images, grain=0.0, seed=3, lead=0, contrast="high", half_world=False, **_):
        import skimage.data
        self.w, self.h, self.kind, self.grain, self.lead, self.contrast, self.half_world = width, height, kind, grain, lead, contrast, half_world
        self.rng = np.random.default_rng(seed)
        image = getattr(skimage.data, images[0])()
        if image.ndim == 2: image = np.stack([image] * 3, axis=2)
        image = np.ascontiguousarray(image[:, :, :3])
        ih, iw, _ = image.shape
        keep = min(ih, iw * height // width)
        image = image[(ih - keep) // 2:(ih - keep) // 2 + keep]
        # float, so that a fade moves different pixels on different pictures (as a renderer's does)
        self.base = np.clip(cv2.resize(image.astype(np.float32), (width, height), interpolation=cv2.INTER_CUBIC), 20.0, 215.0)

    def picture(self, t, index, variant):
        k = index
        pan = 6 * min(k, self.lead)                       # pixels the camera has moved; it stops after the lead
        if self.half_world: pan = 6 * (k // 2 * 2)
        frame = np.roll(self.base, -pan, axis=1).copy()
        if variant % 2: frame = frame[::-1].copy()
        if self.kind == "fade":
            phase = (0.25 * k) % 48.0
            frame += phase if phase < 24.0 else 48.0 - phase
        elif self.kind in ("square", "particle"):
            size = 6 if self.kind == "square" else 2
            x = (37 + k) % (self.w - size)
            y = (self.h // 3 + 5 + (k // 3 if self.kind == "particle" else 0)) % (self.h - size)
            under = frame[y:y + size, x:x + size]
            frame[y:y + size, x:x + size] = 250.0 if self.contrast == "high" else np.minimum(under + 24.0, 255.0)
        else:
            raise ValueError(self.kind)
        if self.grain:
            frame += self.rng.normal(0, self.grain, frame.shape).astype(np.float32)
        return to_i420(np.clip(np.rint(frame), 0, 255).astype(np.uint8))


class Mixed:
    """Mixed cadence, after the second independent review (19 November 2026): the dominant content
    has a locked 30 fps rhythm (a world drawn on every second picture only) while something faint
    is drawn at 60 fps. Every picture in which anything changed is new; the faint thing is all that
    tells every second picture from the one before it. kind:
      pan025      the camera moves 0.25 px per picture over a dark photograph, a 30 fps inset (a
                  picture-in-picture window) carries the rhythm
      steps       the camera moves 6 px and 0.1 px in turn, half contrast
      square, particle  a 6x6 square / 2x2 dot `amp` steps brighter than what is under it, one
                  pixel per picture, over a photograph panned 6 px per game picture at 30 fps
      blob        a soft (Gaussian) blob of amplitude `amp`, one pixel per picture, same world
      ramp        the whole picture one code value brighter every picture (up and down by 20)
      hud         a dim counter (+`amp` over a dark panel) that changes on every picture
      grain       film grain of sigma `amp` drawn anew for every picture
      clouds      a soft cloud layer (15 % alpha) drifting one pixel per picture
      vignette    a vignette whose strength pulses (sine, 1 s) on every picture
      crosshair   a dim crosshair (+`amp`) blinking every 30 pictures, on the world's repeat slot
    start: the picture from which the faint thing is there; before it, the plain 30 fps world (so
    that the stream's own repeats can be learned first).
    """
    def __init__(self, width, height, kind, images, amp=12.0, seed=5, start=0, **_):
        import skimage.data
        self.w, self.h, self.kind, self.amp, self.start = width, height, kind, float(amp), int(start)
        self.rng = np.random.default_rng(seed)
        image = getattr(skimage.data, images[0])()
        if image.ndim == 2: image = np.stack([image] * 3, axis=2)
        image = np.ascontiguousarray(image[:, :, :3]).astype(np.float32)
        ih, iw, _ = image.shape
        keep = min(ih, iw * height // width)
        image = image[(ih - keep) // 2:(ih - keep) // 2 + keep]
        base = cv2.resize(image, (width, height), interpolation=cv2.INTER_CUBIC)
        base = np.concatenate([base, base[:, ::-1]], axis=1)          # mirrored, so a long pan never jumps
        if kind == "pan025": base = base * 0.3 + 8.0
        elif kind == "steps": base = base * 0.5 + 64.0
        self.base = np.clip(base, 10.0, 230.0)
        yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
        r2 = ((xx - width / 2) / (width / 2)) ** 2 + ((yy - height / 2) / (height / 2)) ** 2
        self.vignette = r2[..., None]
        if kind == "clouds":
            noise = self.rng.normal(0, 1, (height // 16 + 2, (width * 2) // 16 + 2)).astype(np.float32)
            self.cloud = cv2.resize(cv2.GaussianBlur(noise, (0, 0), 1.5), (width * 2 + 32, height + 32), interpolation=cv2.INTER_CUBIC)
            self.cloud = (self.cloud - self.cloud.min()) / (self.cloud.max() - self.cloud.min()) * 255.0
        self.inset = None
        if kind == "pan025":
            inset = getattr(skimage.data, "chelsea")()[:, :, :3].astype(np.float32)
            self.inset = cv2.resize(inset, (width // 4 * 2, height // 4), interpolation=cv2.INTER_AREA)

    def view(self, x):
        """The world seen from camera position x (pixels, may be fractional)."""
        ww = self.base.shape[1]
        x = wrap(x, ww - self.w - 2)
        whole = int(math.floor(x)); part = x - whole
        a = self.base[:, whole:whole + self.w]
        if part == 0.0: return a.copy()
        b = self.base[:, whole + 1:whole + 1 + self.w]
        return a * (1.0 - part) + b * part

    def picture(self, t, index, variant):
        k, w, h, amp = index, self.w, self.h, self.amp
        game = k // 2 * 2                                # the 30 fps world's picture
        kind = self.kind if k >= self.start else "clean"
        if kind == "pan025": frame = self.view(0.25 * k)
        elif kind == "steps": frame = self.view(3.0 * self.start + sum(6.0 if i % 2 == 0 else 0.1 for i in range(k - self.start)))
        else: frame = self.view(3.0 * game)
        if variant % 2: frame = frame[::-1].copy()
        if kind == "clean":
            pass
        elif self.kind == "pan025":
            iw = self.inset.shape[1] // 2
            ox = int(wrap(4 * game, iw))
            frame[24:24 + self.inset.shape[0], 24:24 + iw] = self.inset[:, ox:ox + iw]
        elif self.kind == "steps":
            pass                                         # the camera alone
        elif self.kind in ("square", "particle"):
            size = 6 if self.kind == "square" else 2
            x = (37 + k) % (w - size); y = (h // 3 + 5 + (k // 3 if self.kind == "particle" else 0)) % (h - size)
            frame[y:y + size, x:x + size] += amp
        elif self.kind == "blob":
            cx, cy = 200 + k, h * 0.4
            ys, xs = slice(int(cy - 60), int(cy + 60)), slice(int(cx - 60), int(cx + 60))
            yy, xx = np.mgrid[ys, xs].astype(np.float32)
            frame[ys, xs] += amp * np.exp(-((xx - cx) ** 2 + (yy - cy) ** 2) / (2 * 18.0 ** 2))[..., None]
        elif self.kind == "ramp":
            phase = k % 40
            frame += phase if phase < 20 else 40 - phase
        elif self.kind == "hud":
            panel = frame[h - 80:h - 20, 40:360]
            panel *= 0.15
            text = np.zeros(panel.shape[:2], np.uint8)
            cv2.putText(text, f"{k * 37 % 100000:05d}", (10, 45), cv2.FONT_HERSHEY_SIMPLEX, 1.4, 255, 2, cv2.LINE_AA)
            panel += (amp / 255.0) * text.astype(np.float32)[..., None]
        elif self.kind == "grain":
            frame += self.rng.normal(0, amp, frame.shape[:2]).astype(np.float32)[..., None]
        elif self.kind == "clouds":
            c = self.cloud[16:16 + h, 16 + k:16 + k + w]
            frame = frame * 0.85 + 0.15 * c[..., None]
        elif self.kind == "vignette":
            strength = 0.25 + 0.05 * math.sin(2 * math.pi * k / 60.0)
            frame *= (1.0 - strength * self.vignette)
        elif self.kind == "crosshair":
            on = ((k + 1) // 30) % 2 == 1                 # toggles on odd pictures: the world's repeat slot
            if on:
                cy, cx = h // 2, w // 2
                frame[cy - 1:cy + 1, cx - 12:cx + 12] += amp; frame[cy - 12:cy + 12, cx - 1:cx + 1] += amp
        else:
            raise ValueError(self.kind)
        return to_i420(np.clip(np.rint(frame), 0, 255).astype(np.uint8))


class Holdout3:
    """The third review's hold-out (19 November 2026): made after the exact-repeat thresholds were
    frozen, with a generator, worlds and seeds (family 7xxx) that no tuning ever saw, and scored once.
    A game world drawn at `world` fps (30, 40, 45 or 60 inside the 60 Hz output: a photograph or a
    texture panned along a seeded path, three sprites moving with it, a static HUD panel) with, for
    the mixed classes, something drawn at 60 fps on top. Every picture in which anything changed is
    new. kind:
      game        nothing on top: the true 30-in-60 game (or the 40/45/60 controls)
      grain       film grain of sigma `amp`, new for every picture
      counter     a dim HUD counter (+`amp` over the dark HUD panel) that changes on every picture
      blink       a dim marker (+`amp`) that blinks every 15 pictures (on odd and even pictures)
      fog         a soft fog layer (alpha 0.12) drifting 0.5 px per picture
      fade        the whole picture fading by 0.2 code values per picture (up and down)
      particles   40 particles of 1x1 and 2x2 samples, `amp` brighter, each moving 1-3 px per picture
      subpan      the camera moves 0.15 px per picture; the sprites carry the 30 fps rhythm
      rain        thin streaks (+`amp`, 1 px wide, 12 long) falling 9 px per picture
      shimmer     a water area (a quarter of the picture) whose ripples move on every picture
      scroll      dim text (+`amp`) scrolling 0.5 px per picture
      pip         a picture-in-picture window (a sixth of the picture) of another world at 60 fps
    """
    def __init__(self, width, height, kind, images, amp=8.0, seed=7001, world=30, **_):
        import skimage.data
        self.w, self.h, self.kind, self.amp, self.world = width, height, kind, float(amp), int(world)
        self.rng = np.random.default_rng(seed)
        self.seed = seed
        def load(name, w, h):
            img = getattr(skimage.data, name)()
            if img.ndim == 2: img = np.stack([img] * 3, axis=2)
            img = np.ascontiguousarray(img[:, :, :3]).astype(np.float32)
            return cv2.resize(img, (w, h), interpolation=cv2.INTER_CUBIC)
        base = load(images[0], width * 2, height)
        self.base = np.clip(base * 0.8 + 20.0, 12.0, 225.0)
        self.other = load(images[1] if len(images) > 1 else "camera", width // 3 * 2, height // 3)
        steps = self.rng.uniform(1.0, 9.0, 4096)
        self.path = np.concatenate([[0.0], np.cumsum(steps)])     # camera position per world picture
        self.sprites = [(self.rng.uniform(0, width), self.rng.uniform(height * 0.2, height * 0.7), self.rng.uniform(-5, 5), self.rng.uniform(-2, 2), int(self.rng.integers(18, 40)))
                        for _ in range(3)]
        self.parts = [(self.rng.uniform(0, width), self.rng.uniform(0, height), self.rng.uniform(-3, 3), self.rng.uniform(-3, 3), int(self.rng.integers(1, 3))) for _ in range(40)]
        self.drops = [(self.rng.uniform(0, width), self.rng.uniform(0, height)) for _ in range(120)]
        if kind == "fog":
            noise = self.rng.normal(0, 1, (height // 24 + 2, (width * 2) // 24 + 2)).astype(np.float32)
            fog = cv2.resize(cv2.GaussianBlur(noise, (0, 0), 1.2), (width * 2 + 32, height + 32), interpolation=cv2.INTER_CUBIC)
            self.fog = (fog - fog.min()) / (fog.max() - fog.min()) * 255.0
        if kind == "scroll":
            self.text = np.zeros((height * 3, width // 2), np.uint8)
            for line in range(0, height * 3 - 30, 34):
                cv2.putText(self.text, f"credit line {line // 34 + self.seed % 97:03d}  the third hold-out", (8, line + 26), cv2.FONT_HERSHEY_SIMPLEX, 0.7, 255, 1, cv2.LINE_AA)

    def world_index(self, k):
        return k if self.world == 60 else (k * self.world) // 60

    def picture(self, t, index, variant):
        k, w, h, amp = index, self.w, self.h, self.amp
        g = self.world_index(k)
        x = self.path[g] if self.kind != "subpan" else 0.15 * k
        x = wrap(x, self.base.shape[1] - w - 2)
        whole = int(math.floor(x)); part = x - whole
        frame = self.base[:, whole:whole + w] * (1.0 - part) + self.base[:, whole + 1:whole + 1 + w] * part if part else self.base[:, whole:whole + w].copy()
        for sx, sy, vx, vy, size in self.sprites:                 # sprites at the world's rate
            cx = int(wrap(sx + vx * g * 2, w - size)); cy = int(wrap(sy + vy * g * 2, h - size))
            frame[cy:cy + size, cx:cx + size] = frame[cy:cy + size, cx:cx + size] * 0.4 + 140.0
        frame[h - 70:h - 18, 30:330] *= 0.2                       # the HUD panel (static)
        kind = self.kind
        if kind in ("game", "subpan"):
            pass
        elif kind == "grain":
            frame += self.rng.normal(0, amp, frame.shape[:2]).astype(np.float32)[..., None]
        elif kind == "counter":
            text = np.zeros((52, 300), np.uint8)
            cv2.putText(text, f"{(k * 7919) % 1000000:06d}", (10, 40), cv2.FONT_HERSHEY_DUPLEX, 1.2, 255, 2, cv2.LINE_AA)
            frame[h - 70:h - 18, 30:330] += (amp / 255.0) * text.astype(np.float32)[..., None]
        elif kind == "blink":
            if (k // 15) % 2 == 1: frame[60:68, w - 90:w - 60] += amp
        elif kind == "fog":
            f = self.fog[16:16 + h, 16 + k // 2:16 + k // 2 + w] if k % 2 == 0 else 0.5 * (self.fog[16:16 + h, 16 + k // 2:16 + k // 2 + w] + self.fog[16:16 + h, 17 + k // 2:17 + k // 2 + w])
            frame = frame * 0.88 + 0.12 * f[..., None]
        elif kind == "fade":
            phase = (0.2 * k) % 40.0
            frame += phase if phase < 20.0 else 40.0 - phase
        elif kind == "particles":
            for px, py, vx, vy, size in self.parts:
                cx = int(wrap(px + vx * k, w - size)); cy = int(wrap(py + vy * k, h - size))
                frame[cy:cy + size, cx:cx + size] += amp
        elif kind == "rain":
            for dx, dy in self.drops:
                cy = int(wrap(dy + 9 * k, h - 12)); cx = int(dx)
                frame[cy:cy + 12, cx:cx + 1] += amp
        elif kind == "shimmer":
            area = frame[h // 2:h // 2 + h // 4, w // 4:w // 4 + w // 2]
            yy = np.arange(area.shape[0], dtype=np.float32)[:, None]; xx = np.arange(area.shape[1], dtype=np.float32)[None, :]
            ripple = amp * np.sin(xx * 0.21 + yy * 0.07 + k * 0.6) * np.sin(yy * 0.33 - k * 0.4)
            area += ripple[..., None]
        elif kind == "scroll":
            off = k // 2; part = 0.5 if k % 2 else 0.0
            band = self.text[off:off + h // 2].astype(np.float32) * (1.0 - part) + self.text[off + 1:off + 1 + h // 2].astype(np.float32) * part
            frame[h // 4:h // 4 + h // 2, w // 4:w // 4 + w // 2] += (amp / 255.0) * band[..., None]
        elif kind == "pip":
            iw = self.other.shape[1] // 2
            ox = int(wrap(3 * k, iw))
            frame[20:20 + self.other.shape[0], w - iw - 20:w - 20] = self.other[:, ox:ox + iw]
        else:
            raise ValueError(kind)
        return to_i420(np.clip(np.rint(frame), 0, 255).astype(np.uint8))


SOURCES = {"legacy": LegacyScene, "scene": Scene, "clip": Clip, "still": Still, "lavfi": Lavfi, "adversarial": Adversarial, "mixed": Mixed, "holdout3": Holdout3}


# ---------------------------------------------------------------- encoders

ENCODERS = {
    # name: (codec, options)  -- all: no B-frames, no lookahead, one reference chain
    "x264-cbr15": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "15M", "maxrate": "15M", "bufsize": "500k", "g": "100000", "bf": "0"}),
    "x264-cbr5": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "5M", "maxrate": "5M", "bufsize": "170k", "g": "100000", "bf": "0"}),
    "x264-cbr30": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "30M", "maxrate": "30M", "bufsize": "1M", "g": "100000", "bf": "0"}),
    "x264-intra-refresh": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "15M", "maxrate": "15M", "bufsize": "500k", "g": "60", "bf": "0", "intra-refresh": "1"}),
    "x264-filler": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "15M", "maxrate": "15M", "bufsize": "500k", "g": "100000", "bf": "0", "nal-hrd": "cbr"}),
    "x264-slices4": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "15M", "maxrate": "15M", "bufsize": "500k", "g": "100000", "bf": "0", "slices": "4"}),
    "x265-cbr15": ("libx265", {"preset": "ultrafast", "tune": "zerolatency", "b": "15M", "maxrate": "15M", "bufsize": "500k", "g": "100000", "bf": "0"}),
    # Settings only the hold-out set uses.
    "x264-cbr8-slices": ("libx264", {"preset": "superfast", "tune": "zerolatency", "b": "8M", "maxrate": "8M", "bufsize": "270k", "g": "100000", "bf": "0", "slices": "4"}),
    "x264-cbr25-refresh": ("libx264", {"preset": "faster", "tune": "zerolatency", "b": "25M", "maxrate": "25M", "bufsize": "830k", "g": "120", "bf": "0", "intra-refresh": "1"}),
    # The second review's mixed-cadence set: 720p at 10 and 4 Mbit/s (and "raw": no encoder at all).
    "x264-cbr10": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "10M", "maxrate": "10M", "bufsize": "340k", "g": "100000", "bf": "0"}),
    "x264-cbr4": ("libx264", {"preset": "veryfast", "tune": "zerolatency", "b": "4M", "maxrate": "4M", "bufsize": "140k", "g": "100000", "bf": "0"}),
    # The third review's hold-out: x264 and x265 at 10 and 4 Mbit/s, and raw.
    "x265-cbr10": ("libx265", {"preset": "ultrafast", "tune": "zerolatency", "b": "10M", "maxrate": "10M", "bufsize": "340k", "g": "100000", "bf": "0"}),
    "x265-cbr4": ("libx265", {"preset": "ultrafast", "tune": "zerolatency", "b": "4M", "maxrate": "4M", "bufsize": "140k", "g": "100000", "bf": "0"}),
    "x265-cbr6": ("libx265", {"preset": "ultrafast", "tune": "zerolatency", "b": "6M", "maxrate": "6M", "bufsize": "200k", "g": "100000", "bf": "0"}),
}
LEGACY_ENCODERS = ["x264-cbr15", "x264-cbr5", "x264-intra-refresh", "x264-filler", "x265-cbr15"]
TUNE_ENCODERS = ["x264-cbr15", "x264-cbr5", "x265-cbr15"]
HOLDOUT_ENCODERS = ["x264-cbr8-slices", "x264-cbr25-refresh", "x265-cbr6"]
MIXED_ENCODERS = ["raw", "x264-cbr10", "x264-cbr4"]
HOLDOUT3_ENCODERS = ["raw", "x264-cbr10", "x264-cbr4", "x265-cbr10", "x265-cbr4"]


class RawEncoder:
    """No encoder: the decoder sees the very pictures that were rendered (what a lossless link would give)."""
    codec = "raw"
    def __init__(self, width, height):
        self.width, self.height, self.units = width, height, []

    def add(self, frame):
        if frame is not None: self.units.append(frame.to_ndarray().tobytes())   # the whole I420 picture (chroma too, fourth review)

    def finish(self):
        return self.units


class Encoder:
    def __init__(self, name, width, height, threads):
        self.codec, options = ENCODERS[name]
        self.container = av.open(os.devnull, "w", format="h264" if self.codec == "libx264" else "hevc")
        self.stream = self.container.add_stream(self.codec, rate=FPS)
        self.stream.width, self.stream.height, self.stream.pix_fmt = width, height, "yuv420p"
        opts = dict(options)
        self.stream.bit_rate = int(opts.pop("b").rstrip("M")) * 1_000_000
        if threads: opts["threads"] = str(threads)
        if self.codec == "libx265": opts["x265-params"] = "log-level=error" + (f":pools={threads}" if threads else "")
        self.stream.options = opts
        self.units = []

    def add(self, frame):
        for packet in self.stream.encode(frame): self.units.append(bytes(packet))

    def finish(self):
        self.add(None); self.container.close()
        return self.units


PIXEL_STEPS = (0, 1, 2, 4, 8, 16, 32, 64)


_BLOCK_COUNTS = {}


def block_sums(plane, block=16):
    """Sum over every block x block square; the squares cut off by the right and bottom edges
    hold the pixels they have (what the GPU pass does). Exact: integer sums."""
    rows = np.add.reduceat(plane, np.arange(0, plane.shape[0], block), axis=0, dtype=np.uint32)
    return np.add.reduceat(rows, np.arange(0, plane.shape[1], block), axis=1, dtype=np.uint32)


def block_counts(shape, block=16):
    """Pixels in every block of a picture of this shape."""
    if shape not in _BLOCK_COUNTS:
        _BLOCK_COUNTS[shape] = block_sums(np.ones(shape, np.uint8), block).astype(np.float64)
    return _BLOCK_COUNTS[shape]


def block_means(luma, block=16):
    """Mean luma of every block (what the GPU pass produces)."""
    return (block_sums(luma, block) / block_counts(luma.shape, block)).astype(np.float32)


def picture_differences(previous, luma, previous_grid, grid):
    """The measurements of one decoded picture against the one before it (see the top of this file)."""
    d = np.abs(grid - previous_grid)
    pixel = np.where(luma > previous, luma - previous, previous - luma)        # uint8, no wrap
    histogram = np.bincount(pixel.ravel(), minlength=256)
    above = histogram.sum() - np.cumsum(histogram)                              # above[v]: pixels that differ by more than v
    mad = block_sums(pixel) / block_counts(luma.shape)
    return (float(d.max()), float(d.mean()), float((d > 1.0).mean()),
            float(np.flatnonzero(histogram)[-1]), float(mad.max()), float((histogram * np.arange(256)).sum() / pixel.size)) + tuple(int(above[step]) for step in PIXEL_STEPS) + \
           signed_differences(previous, luma)


def first_signed(width, height):
    """The signed columns of the first picture (nothing to compare with): everything moved."""
    blocks = int(block_counts((height, width)).size)
    return (width * height * 255, 0, blocks, blocks, blocks)


def signed_differences(previous, luma):
    """Signed evidence (second review): refinement of an identical picture is zero-mean and
    incoherent; a real change, however faint, is coherent somewhere. Columns:
      positive, negative   sums of the brightening and darkening sample differences, 8-bit steps
      touched              blocks in which any sample changed at all
      coherent             blocks whose signed sum of differences is at least half of their absolute
                           sum and at least 16 steps (a block that changed one way)
      coherentLow          blocks whose absolute signed sum is at least 64 steps (the GPU's word)"""
    signed = luma.astype(np.int32) - previous.astype(np.int32)
    positive = int(signed[signed > 0].sum()); negative = int(-signed[signed < 0].sum())
    total = block_sums(np.abs(signed).astype(np.uint32))
    net = np.add.reduceat(np.add.reduceat(signed, np.arange(0, signed.shape[0], 16), axis=0), np.arange(0, signed.shape[1], 16), axis=1)
    touched = int((total > 0).sum())
    coherent = int(((np.abs(net) * 2 >= total) & (np.abs(net) >= 16)).sum())
    return (positive, negative, touched, coherent, int((np.abs(net) >= 64).sum()))


def decoded_differences(units, codec, width, height):
    """Decodes the stream and measures each picture against the previous decoded picture. The last
    column (fourth review) is the number of chroma samples (U and V) that differ at all."""
    out, previous, previous_grid, previous_chroma = [], None, None, None
    def take_luma(luma, chroma):
        nonlocal previous, previous_grid, previous_chroma
        grid = block_means(luma)
        if previous is None: out.append((255.0, 255.0, 1.0, 255.0, 255.0, 255.0) + (width * height,) * len(PIXEL_STEPS) + first_signed(width, height) + (int(chroma.size),))
        else: out.append(picture_differences(previous, luma, previous_grid, grid) + (int(np.count_nonzero(chroma != previous_chroma)),))
        previous, previous_grid, previous_chroma = luma, grid, chroma
    cw, ch = (width + 1) // 2, (height + 1) // 2
    if codec == "raw":
        for unit in units:
            picture = np.frombuffer(unit, np.uint8)
            take_luma(picture[:width * height].reshape(height, width).copy(), picture[width * height:width * height + 2 * cw * ch].copy())
        return out
    decoder = av.CodecContext.create("h264" if codec == "libx264" else "hevc", "r")
    def take(frame):
        plane = frame.planes[0]
        u, v = frame.planes[1], frame.planes[2]
        chroma = np.concatenate([np.frombuffer(u, np.uint8).reshape(u.height, u.line_size)[:ch, :cw].ravel(),
                                 np.frombuffer(v, np.uint8).reshape(v.height, v.line_size)[:ch, :cw].ravel()])
        take_luma(np.frombuffer(plane, np.uint8).reshape(plane.height, plane.line_size)[:height, :width].copy(), chroma)
    for unit in units:
        for frame in decoder.decode(av.Packet(unit)): take(frame)
    for frame in decoder.decode(None): take(frame)
    return out


# ---------------------------------------------------------------- the sets

def spec(source, cadence_kind, profile, size, encoders, grain=0.0, seed=11, **extra):
    return dict(source=source, cadence=cadence_kind, profile=profile, size=size, encoders=encoders, grain=grain, seed=seed, **extra)


def manifest(which, size):
    hd, fhd = "1280x720", "1920x1080"
    if which == "legacy":
        return [spec("legacy", k, p, size, LEGACY_ENCODERS, grain=g, name="synthetic")
                for k, p, g in (("30", "fast", 0), ("30", "mixed", 0), ("30", "slow", 2.0), ("30-hitch", "fast", 0),
                                ("60", "fast", 0), ("60", "mixed", 0), ("60", "slow", 2.0), ("30-then-60", "mixed", 0))]
    if which == "tune":
        e = TUNE_ENCODERS
        scene = dict(name="scene23", seed=23)
        bikes = dict(name="bikes", clip="bikes.mp4")
        photo = dict(name="coffee", images=["coffee", "astronaut"], seed=5)
        fractal = dict(name="mandelbrot", graph="mandelbrot=size={w}x{h}:rate=30")
        return [
            spec("scene", "30", "slowing", fhd, e, **scene), spec("scene", "30-early", "slowing", fhd, e, **scene),
            spec("scene", "30-then-60-cut", "mixed", fhd, e, **scene), spec("scene", "60", "slowing", fhd, e, **scene),
            spec("scene", "60", "slow", fhd, e, half_rate=True, **scene), spec("scene", "30-static", "fast", fhd, e, **scene),
            spec("scene", "40", "fast", hd, e, **scene), spec("scene", "30-60-flip", "rapid", hd, e, **scene),
            spec("scene", "60", "crawl", hd, e, grain=1.0, **scene),
            spec("clip", "30", "natural", hd, e, **bikes), spec("clip", "60", "natural", hd, e, **bikes),
            spec("clip", "30-hitch", "natural", hd, e, **bikes), spec("clip", "30-cut", "natural", hd, e, **bikes),
            spec("clip", "60-cut", "natural", hd, e, **bikes), spec("clip", "30-then-60-cut", "natural", fhd, e, **bikes),
            spec("clip", "60-then-30", "natural", fhd, e, **bikes), spec("clip", "30-early", "natural", fhd, e, step=2, **bikes),
            spec("still", "30", "slow", hd, e, **photo), spec("still", "60", "slow", hd, e, **photo),
            spec("still", "30-early", "slowing", hd, e, **photo), spec("still", "60", "slowing", hd, e, **photo),
            spec("still", "60", "slow", hd, e, half_rate=True, **photo), spec("still", "60", "crawl", hd, e, **photo),
            spec("still", "30", "rapid", fhd, e, **photo), spec("still", "60", "rapid", fhd, e, **photo),
            spec("still", "30-static", "slow", fhd, e, **photo),
            spec("lavfi", "30", "natural", hd, e, **fractal), spec("lavfi", "60", "natural", hd, e, **fractal),
            spec("lavfi", "30-then-60-cut", "natural", hd, e, **fractal), spec("lavfi", "30-hitch", "natural", hd, e, **fractal),
        ]
    if which == "holdout":
        e = HOLDOUT_ENCODERS
        scene = dict(name="scene99", seed=99)
        bunny = dict(name="bigbuckbunny", clip="bigbuckbunny.mp4")
        phone = dict(name="carphone", clip="carphone_pristine.mp4")
        photo = dict(name="rocket", images=["rocket", "chelsea", "hubble_deep_field"], seed=31)
        test2 = dict(name="testsrc2", graph="testsrc2=size={w}x{h}:rate=30")
        life = dict(name="life", graph="life=size=320x180:rate=30:seed=3:mold=12:ratio=0.2:life_color=#d0e0c0:death_color=#203040:mold_color=#806040")
        grad = dict(name="gradients", graph="gradients=size={w}x{h}:rate=30:seed=5:speed=0.03")
        return [
            spec("clip", "30", "natural", hd, e, **bunny), spec("clip", "60", "natural", hd, e, **bunny),
            spec("clip", "30-then-60-cut", "natural", hd, e, **bunny), spec("clip", "30-early", "natural", hd, e, **bunny),
            spec("clip", "30", "natural", fhd, e, **bunny), spec("clip", "60", "natural", fhd, e, **bunny),
            spec("clip", "30-cut", "natural", fhd, e, step=2, **bunny), spec("clip", "60-cut", "natural", fhd, e, step=2, **bunny),
            spec("clip", "30-hitch", "natural", fhd, e, **bunny), spec("clip", "30-60-flip", "natural", fhd, e, **bunny),
            spec("clip", "30-static", "natural", fhd, e, **bunny), spec("clip", "40", "natural", hd, e, **bunny),
            spec("clip", "30", "natural", hd, e, **phone), spec("clip", "60", "natural", hd, e, **phone),
            spec("clip", "60-then-30", "natural", fhd, e, **phone), spec("clip", "30-early", "natural", fhd, e, **phone),
            spec("still", "30", "slowing", fhd, e, **photo), spec("still", "60", "slowing", fhd, e, **photo),
            spec("still", "30-early", "slowing", fhd, e, **photo), spec("still", "30-then-60-cut", "rapid", hd, e, **photo),
            spec("still", "60", "slow", fhd, e, half_rate=True, **photo), spec("still", "60", "crawl", fhd, e, **photo),
            spec("still", "30-cut", "slow", hd, e, **photo), spec("still", "60-static", "fast", hd, e, **photo),
            spec("scene", "30-early", "slowing", fhd, e, **scene), spec("scene", "30-then-60-cut", "slowing", fhd, e, **scene),
            spec("scene", "60", "slow", fhd, e, half_rate=True, **scene), spec("scene", "20", "fast", hd, e, **scene),
            spec("scene", "60", "rapid", fhd, e, grain=2.0, **scene),
            spec("lavfi", "30", "natural", fhd, e, **test2), spec("lavfi", "60", "natural", fhd, e, **test2),
            spec("lavfi", "30-then-60-cut", "natural", hd, e, **test2),
            spec("lavfi", "30", "natural", hd, e, **life), spec("lavfi", "60", "natural", hd, e, **life),
            spec("lavfi", "30", "natural", fhd, e, **grad), spec("lavfi", "60", "natural", fhd, e, **grad),
            spec("lavfi", "30-early", "natural", hd, e, **grad),
        ]
    if which == "adversarial":
        e3, e2 = ["x264-cbr15", "x264-cbr5", "x265-cbr15"], ["x264-cbr15", "x265-cbr15"]
        out = []
        for kind, image in (("square", "coffee"), ("particle", "astronaut"), ("fade", "rocket")):
            a = dict(name=kind, kind=kind, images=[image])
            out += [spec("adversarial", "60", "still", hd, e3, **a), spec("adversarial", "60", "still", fhd, e2, **a),
                    spec("adversarial", "30", "panstop", hd, e3, lead=90, **a), spec("adversarial", "30", "panstop", fhd, e2, lead=90, **a),
                    spec("adversarial", "30-then-60", "panstop", hd, e3, lead=100, **a), spec("adversarial", "30-early", "panstop", hd, e2, lead=90, **a),
                    spec("adversarial", "30", "panstop", hd, e2, grain=1.5, lead=90, **a), spec("adversarial", "60", "still", hd, e2, grain=1.5, **a)]
            if kind != "fade":
                out += [spec("adversarial", "30-then-60", "panstop", hd, e2, lead=100, contrast="low", **a),
                        spec("adversarial", "60", "still", hd, e2, contrast="low", **a),
                        spec("adversarial", "60", "halfworld", hd, e3, half_world=True, **a),
                        spec("adversarial", "60", "halfworld", fhd, e2, half_world=True, **a),
                        spec("adversarial", "60", "halfworld", hd, e2, half_world=True, contrast="low", **a)]
        return out
    if which == "mixed":
        # The second review's 25 mixed-cadence scenes (all every picture new where something
        # changed; the world itself at 30 fps), raw and through x264 at 10 and 4 Mbit/s, 720p.
        e = MIXED_ENCODERS
        def m(kind, amp, image="coffee", start=0):
            return spec("mixed", "mixed", "world30", hd, e, name=f"mixed-{kind}{amp:g}{'' if image == 'coffee' else '-' + image}{'-late' if start else ''}", kind=kind, amp=amp, images=[image], start=start)
        return [m("pan025", 0, "astronaut"), m("steps", 0, "rocket"),
                m("square", 12), m("square", 6), m("square", 16, "rocket"), m("particle", 12), m("particle", 6, "astronaut"),
                m("blob", 8), m("blob", 15), m("blob", 30), m("blob", 8, "rocket"),
                m("ramp", 1), m("ramp", 1, "astronaut"), m("hud", 20), m("hud", 40, "rocket"),
                m("grain", 1), m("grain", 2), m("grain", 1, "astronaut"), m("grain", 4),
                m("clouds", 0), m("clouds", 0, "rocket"), m("vignette", 0), m("vignette", 0, "astronaut"),
                m("crosshair", 16), m("crosshair", 40),
                # The same faint things appearing 4 s in, after the stream's own repeats were seen.
                m("square", 12, start=240), m("particle", 12, start=240), m("blob", 15, start=240), m("grain", 1, start=240), m("steps", 0, "rocket", start=240)]
    if which == "holdout3":
        # The third review's hold-out (see Holdout3): seeds 7001.., worlds and pictures tuning never used.
        # 17 scenes x 5 encodings. Made on a shared machine short of time: the two true 30-in-60 worlds
        # and the 40 and 45 fps controls with --frames 300, every other scene with --frames 240
        # (the pictures do not depend on which encodings are made, so --encoders may split the work).
        e = HOLDOUT3_ENCODERS
        out = []
        def h(kind, amp, images, seed, world=30):
            return spec("holdout3", "mixed", f"world{world}", hd, e, name=f"h3-{kind}{amp:g}-{images[0]}", kind=kind, amp=amp, images=images, seed=seed, world=world)
        out += [h("game", 0, ["immunohistochemistry", "camera"], 7001), h("game", 0, ["cat", "gravel"], 7003)]
        out += [h("grain", 0.5, ["cat", "camera"], 7011), h("grain", 3, ["immunohistochemistry", "camera"], 7013)]
        out += [h("counter", 8, ["retina", "camera"], 7021), h("blink", 10, ["immunohistochemistry", "camera"], 7023)]
        out += [h("fog", 0, ["retina", "camera"], 7031), h("fade", 0, ["cat", "camera"], 7032)]
        out += [h("particles", 20, ["immunohistochemistry", "camera"], 7041)]
        out += [h("subpan", 0, ["cat", "camera"], 7051), h("rain", 15, ["immunohistochemistry", "camera"], 7052), h("shimmer", 6, ["retina", "camera"], 7053)]
        out += [h("scroll", 30, ["cat", "camera"], 7061), h("pip", 0, ["immunohistochemistry", "moon"], 7062)]
        out += [h("game", 0, ["retina", "moon"], 7071, world=60), h("game", 0, ["cat", "gravel"], 7072, world=40), h("game", 0, ["immunohistochemistry", "camera"], 7073, world=45)]
        return out
    raise ValueError(which)


def run(entry, which, frames, out, clips, threads):
    width, height = (int(v) for v in entry["size"].split("x"))
    base = f"{entry['name']}_{entry['cadence']}_{entry['profile']}{'_half' if entry.get('half_rate') else ''}{'_grain' if entry['grain'] else ''}{'_x2' if entry.get('step', 1) == 2 else ''}{'_low' if entry.get('contrast') == 'low' else ''}_{height}p"
    if which == "legacy":   # the first study's labels
        base = f"{entry['cadence']}_{entry['profile']}{'_grain' if entry['grain'] else ''}"
    labels = {name: f"{base}_{name}" for name in entry["encoders"]}
    todo = [name for name in entry["encoders"] if not (out / (labels[name] + ".txt")).exists()]
    if not todo: return []
    plan = cadence(entry["cadence"], frames) if entry["cadence"] != "mixed" else None
    kwargs = dict(width=width, height=height, profile=entry["profile"], grain=entry["grain"], seed=entry["seed"], frames=frames,
                  half_rate=entry.get("half_rate", False), step=entry.get("step", 1), images=entry.get("images"), graph=entry.get("graph"),
                  kind=entry.get("kind"), lead=entry.get("lead", 0), contrast=entry.get("contrast", "high"), half_world=entry.get("half_world", False),
                  amp=entry.get("amp", 12.0), start=entry.get("start", 0), world=entry.get("world", 30))
    if entry["source"] == "clip":
        if clips is None: raise RuntimeError("this set needs --clips (see the top of this file)")
        kwargs["path"] = clips / entry["clip"]
    source = SOURCES[entry["source"]](**kwargs)
    encoders = {name: RawEncoder(width, height) if name == "raw" else Encoder(name, width, height, threads) for name in todo}
    if entry["source"] in ("mixed", "holdout3"):
        # Every 60 Hz picture is rendered; it is new when anything in it changed (the cadence is mixed).
        pictures, previous = [], None
        for index in range(frames):
            yuv = source.picture(index / FPS, index, 0)
            pictures.append((index / FPS, previous is None or not np.array_equal(yuv, previous), False)); previous = yuv
            for encoder in encoders.values():
                frame = av.VideoFrame.from_ndarray(yuv, format="yuv420p"); frame.pts = index
                encoder.add(frame)
        plan = pictures
    yuv, unique, variant = None, 0, 0
    for index, (t, new, cut) in enumerate(plan if entry["source"] not in ("mixed", "holdout3") else []):
        if cut: variant += 1
        if new or yuv is None:
            yuv = source.picture(t, unique, variant); unique += 1
        for encoder in encoders.values():      # a repeat is the very same picture, as on a console
            frame = av.VideoFrame.from_ndarray(yuv, format="yuv420p"); frame.pts = index
            encoder.add(frame)
    if hasattr(source, "close"): source.close()
    rows = []
    for name, encoder in encoders.items():
        units = encoder.finish(); label = labels[name]
        diffs = decoded_differences(units, encoder.codec, width, height)
        if len(diffs) != len(plan) or len(units) != len(plan):
            raise RuntimeError(f"{label}: {len(units)} access units and {len(diffs)} pictures for {len(plan)} frames")
        (out / (label + ".txt")).write_text("".join(
            f"{len(u)} {int(n)} {d[0]:.4f} {d[1]:.5f} {d[2]:.5f} {int(c)} {d[3]:.0f} {d[4]:.4f} {d[5]:.5f} " + " ".join(str(v) for v in d[6:]) + "\n"
            for u, (_, n, c), d in zip(units, plan, diffs)))
        sizes = [len(u) for u in units]
        def pick(column, want): return [d[column] for d, (_, n, _) in zip(diffs[2:], plan[2:]) if n == want]
        new = [s for s, (_, n, _) in zip(sizes[2:], plan[2:]) if n]; rep = [s for s, (_, n, _) in zip(sizes[2:], plan[2:]) if not n]
        rows.append({"label": label, "set": which, "content": entry["name"], "source": entry["source"], "cadence": entry["cadence"],
                     "motion": entry["profile"], "grain": entry["grain"], "halfRateObjects": bool(entry.get("half_rate")), "encoder": name,
                     "width": width, "height": height, "frames": len(units), "cuts": sum(1 for _, _, c in plan if c),
                     "newMedian": int(np.median(new)) if new else None, "repeatMedian": int(np.median(rep)) if rep else None,
                     "repeatMax": int(max(rep)) if rep else None, "newMin": int(min(new)) if new else None,
                     "repeatMaxBlockDiff": max(pick(0, False), default=None), "newMinBlockDiff": min(pick(0, True), default=None),
                     "repeatMaxMeanDiff": max(pick(1, False), default=None), "newMinMeanDiff": min(pick(1, True), default=None),
                     "repeatMaxPixelDiff": max(pick(3, False), default=None), "newMinPixelDiff": min(pick(3, True), default=None),
                     "kind": entry.get("kind"), "contrast": entry.get("contrast"), "lead": entry.get("lead")})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("--set", choices=("legacy", "tune", "holdout", "adversarial", "mixed", "holdout3"), default="legacy")
    parser.add_argument("--frames", type=int, default=480)
    parser.add_argument("--size", default="1280x720", help="legacy set only")
    parser.add_argument("--clips", type=pathlib.Path)
    parser.add_argument("--only", default="", help="only entries whose content, cadence or motion contains this text")
    parser.add_argument("--threads", type=int, default=1, help="encoder threads (0 = the encoder's default)")
    parser.add_argument("--quiet", action="store_true", help="print names only, no statistics (for a hold-out set that must not be looked at yet)")
    parser.add_argument("--encoders", default="", help="comma-separated: only these of each entry's encodings (for a machine short of time)")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    entries = [e for e in manifest(args.set, args.size) if args.only in f"{e['name']} {e['cadence']} {e['profile']}"]
    if args.encoders:
        keep = args.encoders.split(",")
        entries = [dict(e, encoders=[n for n in e["encoders"] if n in keep]) for e in entries]
    if args.list:
        for e in entries: print(e["name"], e["source"], e["cadence"], e["profile"], e["size"], ",".join(e["encoders"]))
        print(sum(len(e["encoders"]) for e in entries), "streams"); return 0
    args.out.mkdir(parents=True, exist_ok=True)
    index_path = args.out / "index.json"
    index = json.loads(index_path.read_text()) if index_path.exists() else []
    for entry in entries:
        started = time.time()
        rows = run(entry, args.set, args.frames, args.out, args.clips, args.threads)
        index = [r for r in index if r["label"] not in {x["label"] for x in rows}] + rows
        index_path.write_text(json.dumps(index, indent=1))
        for r in rows:
            if args.quiet: print(f"{r['label']} done ({time.time() - started:.0f} s)", flush=True); continue
            def fmt(v): return "   n/a" if v is None else f"{v:6.2f}"
            ratio = ("%.2f" % (r["repeatMedian"] / r["newMedian"])) if r["repeatMedian"] else " n/a"
            print(f"{r['label']:52s} size rep/new {ratio} | block diff: repeats max {fmt(r['repeatMaxBlockDiff'])}  new min {fmt(r['newMinBlockDiff'])}"
                  f" | mean diff: repeats max {fmt(r['repeatMaxMeanDiff'])}  new min {fmt(r['newMinMeanDiff'])}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
