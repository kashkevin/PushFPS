# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Stream "s12-pace": dependency-free reference of the present blit's downscale variants (SIMULATED, one axis).

Mirrors include/veyra/engine/PushDownscale.h (the C++ reference that shaders/PresentBlit.hlsl is tested against) for one row of
codes; a 2D picture is separable in weights, so the 1D numbers are the per-axis behaviour. Run:

    python -B tools/pace_downscale/downscale_ref.py            # markdown table on stdout
    python -B -m unittest discover -s tools/pace_downscale     # self checks

Kernels: package16 (one bilinear tap on the codes), area (exact box weights, linear light), area_sharp (the committed 0.03 unsharp
of the two neighbouring boxes), the same with a clamp to the range of the texels under the screen pixel's own box (no ringing, no
overshoot past the source peak, no negative light), catmull_rom / lanczos2 (texel-centre weights in screen-pixel units), and
both clamped. Everything is decoded to linear light first (PQ or sRGB), summed, normalised, encoded again.
"""
import math
import sys

SHARPEN = 0.03
M1, M2, C1, C2, C3 = 2610 / 16384, 2523 / 32, 3424 / 4096, 2413 / 128, 2392 / 128


def pq_to_lin(c):
    p = max(0.0, min(1.0, c)) ** (1 / M2)
    return (max(p - C1, 0.0) / max(C2 - C3 * p, 1e-6)) ** (1 / M1)


def lin_to_pq(x):
    p = max(0.0, min(1.0, x)) ** M1
    return ((C1 + C2 * p) / (1 + C3 * p)) ** M2


def srgb_to_lin(c):
    c = max(0.0, min(1.0, c))
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def lin_to_srgb(x):
    x = max(0.0, min(1.0, x))
    return x * 12.92 if x <= 0.0031308 else 1.055 * x ** (1 / 2.4) - 0.055


def decode(c, t):
    return pq_to_lin(c) if t == "pq" else srgb_to_lin(c) if t == "srgb" else c


def encode(x, t):
    return lin_to_pq(x) if t == "pq" else lin_to_srgb(x) if t == "srgb" else x


def cubic(x, b, c):
    x = abs(x)
    if x < 1:
        return ((12 - 9 * b - 6 * c) * x ** 3 + (-18 + 12 * b + 6 * c) * x * x + (6 - 2 * b)) / 6
    if x < 2:
        return ((-b - 6 * c) * x ** 3 + (6 * b + 30 * c) * x * x + (-12 * b - 48 * c) * x + (8 * b + 24 * c)) / 6
    return 0.0


def sinc(x):
    return 1.0 if abs(x) < 1e-9 else math.sin(math.pi * x) / (math.pi * x)


KERNELS = ["package16", "area", "area_sharp", "area_sharp_clamp", "catmull_rom", "catmull_rom_clamp", "lanczos2", "lanczos2_clamp"]


def base(k):
    return k[:-6] if k.endswith("_clamp") else k


def radius(k):
    b = base(k)
    return {"area": 0.5, "area_sharp": 1.5, "package16": 0.0}.get(b, 2.0)


def box(i, a, b):
    return max(0.0, min(i + 1, b) - max(i, a))


def weight(k, c, f, i):
    b = base(k)
    if b == "area":
        return box(i, c - f / 2, c + f / 2)
    if b == "area_sharp":
        return (1 + 2 * SHARPEN) * box(i, c - f / 2, c + f / 2) - SHARPEN * (box(i, c - 1.5 * f, c - f / 2) + box(i, c + f / 2, c + 1.5 * f))
    x = (i + 0.5 - c) / f
    if b == "catmull_rom":
        return cubic(x, 0.0, 0.5)
    return sinc(x) * sinc(x / 2) if abs(x) < 2 else 0.0


def bilinear(codes, c):
    p = c - 0.5
    x0 = math.floor(p)
    fx = p - x0
    n = len(codes)
    a, b = codes[min(max(x0, 0), n - 1)], codes[min(max(x0 + 1, 0), n - 1)]
    return a * (1 - fx) + b * fx


def shrink_row(codes, out_w, k, t):
    """One row of codes in, one row of codes out; footprint f = len / out_w."""
    n, f = len(codes), len(codes) / out_w
    lin = [decode(v, t) for v in codes]
    out = []
    for o in range(out_w):
        c = (o + 0.5) * f
        if k == "package16" or f <= 1.001:
            out.append(bilinear(codes, c))
            continue
        r = radius(k) * f
        s = w_sum = 0.0
        lo_c, hi_c = 1e300, -1e300
        for i in range(math.floor(c - r), math.ceil(c + r)):
            w = weight(k, c, f, i)
            if w == 0:
                continue
            v = lin[min(max(i, 0), n - 1)]
            s += w * v
            w_sum += w
            if k.endswith("_clamp") and box(i, c - f / 2, c + f / 2) > 1e-3:
                lo_c, hi_c = min(lo_c, v), max(hi_c, v)
        v = s / w_sum if w_sum else 0.0
        if k.endswith("_clamp") and lo_c <= hi_c:
            v = min(max(v, lo_c), hi_c)
        out.append(encode(v, t))
    return out


def levels(t):
    return (300 / 10000, 1 / 10000) if t == "pq" else (1.0, 0.01)   # hi, lo in linear light


def line_fit_amplitude(out, period, t):
    """Amplitude of a sine of this screen-pixel period in the linear-light output (least squares at its frequency)."""
    scc = sss = scs = sc = ss = sy = syc = sys_ = m = 0.0
    for o in range(20, len(out) - 20):
        l = decode(out[o], t)
        a = 2 * math.pi * (o + 0.5) / period
        cs, sn = math.cos(a), math.sin(a)
        scc += cs * cs; sss += sn * sn; scs += cs * sn; sc += cs; ss += sn; sy += l; syc += l * cs; sys_ += l * sn; m += 1
    mat = [[scc, scs, sc, syc], [scs, sss, ss, sys_], [sc, ss, m, sy]]
    for i in range(3):
        for r in range(3):
            if r != i:
                g = mat[r][i] / mat[i][i]
                for j in range(4):
                    mat[r][j] -= g * mat[i][j]
    return 2 * math.hypot(mat[0][3] / mat[0][0], mat[1][3] / mat[1][1])


def measure(k, f, t):
    """The bench's numbers for kernel k at footprint f (1.5 = full screen 1440p, 2.63 = his 1462 px window)."""
    n = 480 if f < 2 else 960
    w = round(n / f)
    hi, lo = levels(t)
    r = dict(stroke_mean=0.0, stroke_worst=1e9, pulse=0.0)
    energies, cols = [], 0
    for q in range(120):
        x0 = 200 + q * 0.25
        codes = [encode(lo + (hi - lo) * max(0.0, min(i + 1, x0 + 1) - max(i, x0)), t) for i in range(n)]
        out = shrink_row(codes, w, k, t)
        lin = [decode(v, t) - lo for v in out]
        energies.append(sum(lin))
        if q % 4 == 0:
            pk = max(lin) / (hi - lo)
            r["stroke_mean"] += pk; r["stroke_worst"] = min(r["stroke_worst"], pk); cols += 1
    r["stroke_mean"] /= cols
    r["pulse"] = (max(energies) - min(energies)) / (sum(energies) / len(energies))
    for name, per in (("contrast4", 4.0), ("contrast3", 3.0), ("contrast25", 2.5)):
        ps, tot = per * n / w, 0.0
        for ph in range(8):
            a = 2 * math.pi / ps
            codes = [encode((lo + hi) / 2 + (hi - lo) / 2 * (math.sin(a * (i + ph * ps / 8 + 1)) - math.sin(a * (i + ph * ps / 8))) / a, t) for i in range(n)]
            tot += line_fit_amplitude(shrink_row(codes, w, k, t), per, t) / (hi - lo)
        r[name] = tot / 8
    ov = 0.0
    for q in range(16):
        x0 = 200 + q / 16
        codes = [encode(lo + (hi - lo) * max(0.0, min(1.0, i + 1 - x0)), t) for i in range(n)]
        for v in shrink_row(codes, w, k, t):
            l = decode(v, t)
            ov = max(ov, (l - hi) / (hi - lo), (lo - l) / (hi - lo))
    r["overshoot_pct"] = ov * 100
    # moire: grating beyond Nyquist (period 1.25 source px); shimmer: its temporal RMS while it drifts (per-pixel, 12 phases)
    series = []
    for ph in range(12):
        codes = [encode((lo + hi) / 2 + (hi - lo) / 2 * math.cos(2 * math.pi * (i + ph / 12 * 1.25) / 1.25), t) for i in range(n)]
        series.append([decode(v, t) for v in shrink_row(codes, w, k, t)][20:-20])
    amp = (hi - lo) / 2
    flat = [v for row in series[:1] for v in row]
    mean = sum(flat) / len(flat)
    r["moire"] = math.sqrt(max(0.0, sum(v * v for v in flat) / len(flat) - mean * mean)) / amp
    sh = 0.0
    for o in range(len(series[0])):
        col = [s[o] for s in series]
        m = sum(col) / len(col)
        sh += math.sqrt(sum((v - m) ** 2 for v in col) / len(col))
    r["shimmer"] = sh / len(series[0]) / amp
    return r


def hdr_accuracy(k, f=1.5):
    """PQ only. A 1000-nit patch (flat: exact nits), a 2-px 1000-nit highlight on 5 nits (peak nits kept), the largest overshoot
    past the source peak at a 1000 -> 5 nit edge, and the most negative linear light (a negative channel is clipped by the display:
    a hue shift) of a 1-px 1000-nit stroke on black, before the display's clip."""
    n = 480
    w = round(n / f)
    patch = shrink_row([lin_to_pq(0.1)] * n, w, k, "pq")
    patch_err = max(abs(pq_to_lin(v) * 10000 - 1000) for v in patch)
    best = 0.0
    for q in range(12):
        x0 = 200 + q / 4
        codes = [lin_to_pq((5 + 995 * max(0.0, min(i + 2, x0 + 2) - max(i, x0)) / 2) / 10000) for i in range(n)]
        best = max(best, max(pq_to_lin(v) * 10000 for v in shrink_row(codes, w, k, "pq")))
    over = 0.0
    for q in range(16):
        x0 = 200 + q / 16
        codes = [lin_to_pq((5 + 995 * max(0.0, min(1.0, x0 - i))) / 10000) for i in range(n)]
        for v in shrink_row(codes, w, k, "pq"):
            over = max(over, pq_to_lin(v) * 10000 - 1000)
    # the raw (unencoded) filter output can be negative: reproduce it without the final PQ clamp
    return dict(patch_err_nits=patch_err, thin_peak_nits=best, edge_overshoot_nits=over, min_light_nits=raw_min(k, f))


def raw_min(k, f):
    """The most negative linear light (nits, 1000-nit 1-px stroke on black) the filter produces before the display's clip."""
    if k == "package16" or k.endswith("_clamp"):
        return 0.0
    n = 480
    out_w = round(n / f)
    worst = 0.0
    for q in range(8):
        x0 = 200 + q / 8
        lin = [1000.0 * max(0.0, min(i + 1, x0 + 1) - max(i, x0)) for i in range(n)]
        for o in range(out_w):
            c = (o + 0.5) * f
            r = radius(k) * f
            s = ws = 0.0
            for i in range(math.floor(c - r), math.ceil(c + r)):
                w = weight(k, c, f, i)
                if w:
                    s += w * lin[min(max(i, 0), n - 1)]
                    ws += w
            worst = min(worst, s / ws if ws else 0.0)
    return worst


def taps(k, f):
    if k == "package16":
        return 4, 0
    span = math.ceil(2 * radius(k) * f) + 1
    return span * span, span * span * 6   # loads, decode pow() calls (2 per channel x 3 channels) per output pixel


def table():
    rows = []
    for name, f, t in (("1.5x HDR10", 1.5, "pq"), ("1.5x SDR", 1.5, "srgb"), ("2.63x HDR10", 3840 / 1462, "pq")):
        for k in KERNELS:
            m = measure(k, f, t)
            loads, pows = taps(k, f)
            rows.append((name, k, m, loads, pows))
    return rows


def main():
    print("| case | kernel | stroke mean | stroke worst | pulse | contrast 4 | contrast 3 | contrast 2.5 | overshoot % | moire | shimmer | loads/px | pow()/px |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for name, k, m, loads, pows in table():
        print(f"| {name} | {k} | {m['stroke_mean']:.3f} | {m['stroke_worst']:.3f} | {m['pulse']:.3f} | {m['contrast4']:.3f} | {m['contrast3']:.3f} | "
              f"{m['contrast25']:.3f} | {m['overshoot_pct']:.1f} | {m['moire']:.3f} | {m['shimmer']:.3f} | {loads} | {pows} |")
    print()
    print("| HDR10 1.5x kernel | flat 1000-nit patch error (nits) | 2-px 1000-nit highlight peak kept (nits) | edge overshoot past 1000 nits | most negative light, 1-px 1000-nit stroke (nits) |")
    print("|---|---|---|---|---|")
    for k in KERNELS:
        h = hdr_accuracy(k)
        print(f"| {k} | {h['patch_err_nits']:.3f} | {h['thin_peak_nits']:.1f} | {h['edge_overshoot_nits']:.1f} | {h['min_light_nits']:.1f} |")


if __name__ == "__main__":
    sys.exit(main())
