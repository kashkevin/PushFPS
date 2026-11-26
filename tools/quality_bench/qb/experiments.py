# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""The experiments behind OBJECTIVE_COMPARISON.md. Every function returns plain rows (dicts); run_all.py prints tables.
SIMULATED throughout: a model chain on synthetic pictures."""
from __future__ import annotations

import math

import numpy as np
from scipy import ndimage

from . import chain, display, downscale as ds, generators as gens, metrics as mt, observer as ob, scenes, tf

HDR = tf.Signal(True)
SDR = tf.Signal(False)
STRIP_W = 1500          # 4K px (1000 output px), periodic
STRIP_H = 6             # 4K rows (4 output rows)


def out_speed_to_4k(v_out_per_s):
    return v_out_per_s * 1.5 / 30.0      # 4K px per source frame (30 Hz nominal)


# ---- Q1: 30 / 60 / 90, motion clarity, judder, uniqueness ---------------------------------------------------------------
class StripRun:
    """Frames of one strip pattern through generator + downscale, cached; observers on top for each hold model."""

    def __init__(self):
        self.cache = {}

    def frames(self, pattern, v4k, M, gen, shutter, filt, n_src=61, signal=HDR):
        key = (pattern, v4k, M, gen, shutter, filt, n_src, signal.name)
        if key not in self.cache:
            world = {"bars": scenes.bars(STRIP_W, 5.0, 200.0),
                     "sine0.1": scenes.sine(0.1 / 1.5, 50.0, 0.3), "sine0.2": scenes.sine(0.2 / 1.5, 50.0, 0.3),
                     "sine0.3": scenes.sine(0.3 / 1.5, 50.0, 0.3),
                     "texture": scenes.textured(STRIP_W, 12, cells=[300, 60, 12, 4])}[pattern]
            h = 12 if pattern == "texture" else STRIP_H
            sc = scenes.Scene(STRIP_W, h, world, scenes.pan(v4k), shutter=shutter, qy=1)
            fr, times, info = chain.sequence(sc, signal, M, gen, n_src)
            oc, on = chain.to_output(fr, signal, filt)
            self.cache[key] = (oc, [n[n.shape[0] // 2] for n in on], times, info)
        return self.cache[key]


def q1_point(run, v_out, M, gen, shutter, filt, holds_mode, seeds=(0,), n_src=61):
    v4k = out_speed_to_4k(v_out)
    res = {"fps": 30 * M, "v_out": v_out, "gen": gen, "shutter": shutter, "filter": filt, "holds": holds_mode}
    bw, rise, mtf = [], [], {f: [] for f in (0.1, 0.2, 0.3)}
    jr, jw, uq, evens = [], [], [], []
    for seed in seeds:
        oc, rows, times, info = run.frames("bars", v4k, M, gen, shutter, filt, n_src)
        starts, cs = display.schedule(M, len(rows), holds_mode, seed=seed)
        vo = v4k / 1.5 / (cs[M] - cs[0]) if M > 1 else v4k / 1.5 / (cs[1] - cs[0])
        R = ob.retina_1d(rows, starts, cs, vo)
        d = np.diff(np.concatenate([R, R[:1]]))
        bw.append(ob.lsf_width(R, int(np.argmax(d)), 300 * ob.FINE))
        rise.append(ob.edge_rise(R, int(np.argmax(d)), 300 * ob.FINE))
        for f in mtf:
            _, srows, _, _ = run.frames(f"sine{f}", v4k, M, gen, shutter, filt, n_src)
            Rs = ob.retina_1d(srows, starts, cs, vo)
            cycles = int(round(f * STRIP_W / 1.5))
            mtf[f].append(ob.modulation(Rs, cycles) / (50.0 * 0.3))
        r, w, _, _ = ob.judder(starts, cs, vo)
        jr.append(r); jw.append(w)
        evens.append(display.hold_histogram(starts, M)["even_pct"])
        if M > 1 and seed == seeds[0]:
            toc, trows, _, tinfo = run.frames("texture", v4k, M, gen, shutter, filt, n_src)
            uq.append(mt.unique_frames(toc, tinfo, starts)[0])
        elif seed == seeds[0]:
            uq.append(len(rows) / ((starts[-1] - starts[0]) * display.PERIOD_S) * (len(rows) - 1) / len(rows))
    res.update(blur_px=float(np.mean(bw)), rise_px=float(np.mean(rise)), mtf01=float(np.mean(mtf[0.1])), mtf02=float(np.mean(mtf[0.2])),
               mtf03=float(np.mean(mtf[0.3])), judder_rms=float(np.mean(jr)), judder_worst=float(np.max(jw)),
               unique_fps=float(np.mean(uq)), even_pct=float(np.mean(evens)),
               step_px=v_out / (30.0 * M))
    return res


def q1(quick=True):
    run = StripRun()
    speeds = [960] if quick else [480, 960, 1920]
    seeds = (0, 1) if quick else (0, 1, 2, 3)
    rows = []
    # (a) the ideal chain at every speed and rate
    for v in speeds:
        for M in (1, 2, 3):
            rows.append(q1_point(run, v, M, "ideal", 0.0, "exact", "ideal"))
    # (b) one factor at a time at 960 px/s, then everything together (his likely chain)
    variants = [("ideal chain", "ideal", 0.0, "exact", "ideal"),
                ("(i) composed holds", "ideal", 0.0, "exact", "composed"),
                ("(ii) mixed quality 3X (weak t=1/3)", "mixed", 0.0, "exact", "ideal"),
                ("(ii) mixed, weaker knob (beta 0.15)", "mixed:0.15", 0.0, "exact", "ideal"),
                ("(ii') block model, pan", "block", 0.0, "exact", "ideal"),
                ("(iii) source blur 50 %", "ideal", 0.5, "exact", "ideal"),
                ("(iii) source blur 100 %", "ideal", 1.0, "exact", "ideal"),
                ("(iv) Package 16 downscale", "ideal", 0.0, "p16", "ideal"),
                ("(iv') s12-pace area downscale", "ideal", 0.0, "area", "ideal"),
                ("all: composed + mixed + blur 50 % + P16", "mixed", 0.5, "p16", "composed"),
                ("all, blur 0", "mixed", 0.0, "p16", "composed"),
                ("all, blur 100 %", "mixed", 1.0, "p16", "composed")]
    factor_rows = []
    for name, gen, sh, filt, hm in variants:
        pts = {M: q1_point(run, 960, M, gen, sh, filt, hm, seeds=seeds if hm != "ideal" else (0,)) for M in (1, 2, 3)}
        factor_rows.append((name, pts))
    return rows, factor_rows


# ---- Q2: downscale, fine detail -----------------------------------------------------------------------------------------
def _strip_out(world, signal, filt, w=48, h=6, quant=True):
    sc = scenes.Scene(w, h, world, scenes.still(), qy=1)
    c = signal.encode(sc.render(0.0))
    return signal.decode(ds.downscale(c, signal, filt, quant=quant))


def q2_lines(signal=HDR, nits=300.0, bg=10.0):
    rows = []
    for width in (1, 2, 3):
        for filt in ds.FILTERS:
            pk, en, wd = [], [], []
            for k in range(12):      # quarter-pixel phases over 3 4K px (one whole period of the 1.5x grid)
                o = _strip_out(scenes.lines(bg, [(20 + k / 4, width, nits)]), signal, filt)[2]
                p, e = mt.line_stats(o, bg)
                pk.append(p[0]); en.append(e[0]); wd.append(mt.fwhm(o - bg))
            truth_energy = (nits - bg) * width / 1.5
            rows.append({"width": width, "filter": filt, "peak_mean": float(np.mean(pk)), "peak_worst": float(np.min(pk)),
                         "energy_err_pct": 100 * (np.mean(en) / truth_energy - 1), "energy_pulse": mt.pulse(en),
                         "peak_pulse": mt.pulse(pk), "fwhm_mean": float(np.mean(wd)), "fwhm_range": float(np.ptp(wd))})
    return rows


def q2_strokes(signal=HDR):
    """Pixel-aligned HUD strokes (300 nits on 0 nits), the reviewer's table: peak mean / worst over the 3 column phases."""
    rows = []
    for width in (1, 2):
        for filt in ds.FILTERS:
            pk = [float(_strip_out(scenes.lines(0.0, [(21 + k, width, 300.0)]), signal, filt)[2].max()) for k in range(3)]
            rows.append({"width": width, "filter": filt, "peak_mean": float(np.mean(pk)), "peak_worst": float(min(pk))})
    return rows


def q2_text(signal=HDR, quick=True):
    """Small text (5x7 font, strokes 2 px at 4K = 1.33 output px, 300 nits on a 20-nit panel): static at the 3 column
    phases (a HUD's fixed phase) and drifting by a quarter pixel a frame (world text): stroke contrast and its pulse."""
    rows = []
    W, H = 240, 48
    for filt in ds.FILTERS:
        energies, peaks = [], []
        for k in range(12):
            a = scenes.hud_alpha(W, H, "text", 120 + k / 4, 24, width=2)
            img = 20.0 * (1 - a) + 300.0 * a
            c = signal.encode(img)
            o = signal.decode(ds.downscale(c, signal, filt))
            aw, ah = ds._area_weights(W, W * 2 // 3), ds._area_weights(H, H * 2 // 3)
            foot = (ah @ a @ aw.T) > 0.5          # output pixels mostly covered by a stroke
            energies.append(float((o - 20.0).clip(min=0).sum()))
            peaks.append(float((o[foot] - 20.0).mean()))   # mean stroke contrast (nits above the panel)
        truth = float((signal.decode(ds.downscale(signal.encode(20.0 * (1 - a) + 300.0 * a), signal, "exact")) - 20).clip(min=0).sum())
        static = [peaks[0], peaks[4], peaks[8]]
        rows.append({"filter": filt, "stroke_peak_static_mean": float(np.mean(static)), "stroke_peak_static_worst": float(min(static)),
                     "energy_vs_exact_pct": 100 * (np.mean(energies) / truth - 1), "energy_pulse_moving": mt.pulse(energies),
                     "peak_pulse_moving": mt.pulse(peaks)})
    return rows


def q2_moire_and_sharpness(signal=HDR):
    rows = []
    for filt in ds.FILTERS:
        r = {"filter": filt}
        # beyond the output's Nyquist: a 2.5-px 4K grating (1.67 output px); ideal = flat at the mean
        frames = []
        for k in range(10):
            sc = scenes.Scene(STRIP_W, 6, scenes.sine(1 / 2.5, 50.0, 0.5), scenes.pan(0.25), qy=1)
            frames.append(signal.decode(ds.downscale(signal.encode(sc.render(float(k))), signal, filt))[2])
        F = np.array(frames)
        r["moire_rms_pct"] = float(100 * np.mean([np.std(f) for f in F]) / 25.0)
        r["shimmer_pct"] = float(100 * np.mean(np.std(F, axis=0)) / 25.0)
        for f in (0.1, 0.2, 0.25, 0.333, 0.4):
            period = 1.5 / f
            world = scenes.sine(1 / period, 50.0, 0.3)
            vals = []
            for ph in range(4):   # average over 4 sub-pixel phases
                sc = scenes.Scene(STRIP_W, 6, world, scenes.pan(ph * period / 4), qy=1)
                o = signal.decode(ds.downscale(signal.encode(sc.render(1.0)), signal, filt))[2]
                cyc = int(round(f * STRIP_W / 1.5))
                vals.append(ob.modulation(o, cyc) / 15.0)
            r[f"contrast_{f}"] = float(np.mean(vals))
        rows.append(r)
    return rows


def q2_highlights(signal=HDR):
    rows = []
    for width in (1, 2):
        for filt in ds.FILTERS:
            pk, en = [], []
            for k in range(12):
                o = _strip_out(scenes.lines(50.0, [(20 + k / 4, width, 1000.0)]), signal, filt)[2]
                p, e = mt.line_stats(o, 50.0)
                pk.append(p[0]); en.append(e[0])
            truth = 950.0 * width / 1.5
            rows.append({"width": width, "filter": filt, "peak_mean": float(np.mean(pk)), "energy_err_pct": 100 * (np.mean(en) / truth - 1),
                         "energy_pulse": mt.pulse(en)})
    big = []
    for filt in ds.FILTERS:   # a large 1000-nit patch: every filter must keep it
        o = _strip_out(scenes.lines(50.0, [(12, 24, 1000.0)]), signal, filt)[2]
        big.append({"filter": filt, "patch_nits": float(o[12]), "patch_pq_err_codes": float((tf.pq_encode(o[12]) - tf.pq_encode(1000.0)) * 1023)})
    return rows, big


def hue_test():
    """Saturated BT.2020 edges (HDR10, per-channel PQ codes), 3 column phases: a 1000-nit red stroke (1 px and 2 px) on a
    100-nit green field, through the filters, against the exact linear-light area result. delta E ITP (about 1 = just
    noticeable) and hue angle of the stroke's brightest output pixel."""
    rows = []
    red, green = np.array([1000.0, 0.0, 0.0]), np.array([0.0, 100.0, 0.0])
    for width in (1, 2):
        for filt in ("p16", "area", "exact"):
            de, dh = [], []
            for k in range(3):
                outs = {}
                for f2 in (filt, "exact"):
                    ch = []
                    for c in range(3):
                        sc = scenes.Scene(48, 6, scenes.lines(green[c], [(21 + k, width, red[c])]), scenes.still(), qy=1)
                        ch.append(HDR.decode(ds.downscale(HDR.encode(sc.render(0.0)), HDR, f2))[2])
                    outs[f2] = np.stack(ch, -1)
                x = int(np.argmax(outs["exact"][:, 0]))
                de.append(float(tf.delta_e_itp(outs[filt][x], outs["exact"][x])))
                d = float(tf.hue_angle_deg(outs[filt][x]) - tf.hue_angle_deg(outs["exact"][x]))
                dh.append((d + 180.0) % 360.0 - 180.0)
            rows.append({"width": width, "filter": filt, "dEITP_mean": float(np.mean(de)), "dEITP_worst": float(np.max(de)),
                         "hue_shift_deg_worst": float(np.max(np.abs(dh)))})
    return rows


# ---- Q3 / Q4: crosshair over a circular camera move; ghosting and HUD stability ----------------------------------------
PLANE = 384                         # 4K px (256 output px)
CX = CY = 192                       # crosshair centre (4K px); + phase


def hud_scene(kind="crosshair", width=2, opacity=1.0, speed=48.0, phase=0, motion="circle", with_hud=True, signal=HDR):
    nits = 300.0 if signal.hdr else 200.0
    a = scenes.hud_alpha(PLANE, PLANE, kind, CX + phase + 0.5 * (width % 2), CY + 0.5 * (width % 2), width=width, opacity=opacity)
    path = scenes.circle(speed) if motion == "circle" else scenes.pan(speed)
    return scenes.Scene(PLANE, PLANE, scenes.textured(PLANE, PLANE, mean=30.0, contrast=0.7, seed=3), path,
                        hud=(a, nits) if with_hud else None), a


def _ds_all(frames, signal, filt):
    return [ds.downscale(f, signal, filt) for f in frames]


_SEQ = {}


def _hud_sequences(M, guard, gen, kind, width, opacity, speed, phase, n_src, signal, motion):
    key = (M, guard, gen, kind, width, opacity, speed, phase, n_src, signal.name, motion)
    if key not in _SEQ:
        sc, alpha = hud_scene(kind, width, opacity, speed, phase, motion, True, signal)
        sc0, _ = hud_scene(kind, width, opacity, speed, phase, motion, False, signal)
        fr, _, info = chain.sequence(sc, signal, M, gen, n_src, guard=guard)
        # the same generator on the overlay-free pictures: what the overlay ADDS is the ghost (for PushWarp: rules off
        # there, as nothing is to protect; HUD Guard: off)
        fr0, _, _ = chain.sequence(sc0, signal, M, gen, n_src, guard=False)
        ideal, _, _ = chain.sequence(sc, signal, M, "ideal", n_src)
        _SEQ.clear() if len(_SEQ) > 8 else None
        _SEQ[key] = (sc, sc0, alpha, fr, fr0, info, ideal)
    return _SEQ[key]


def hud_case(M, guard, filt, gen="block", kind="crosshair", width=2, opacity=1.0, speed=48.0, phase=0, n_src=26,
             measure_from=20, signal=HDR, motion="circle"):
    """One cell of the Q3 grid. Returns HUD-stability and ghost metrics at OUTPUT resolution (and the ghost count at 4K).
    HUD Guard needs real frames of history before it protects (6 still frames with motion around; a 1-px crosshair over
    this circle pan reached its full mask after about 20 real frames), so frames before `measure_from` are warm-up."""
    sc, sc0, alpha, fr, fr0, info, ideal = _hud_sequences(M, guard, gen, kind, width, opacity, speed, phase, n_src, signal, motion)
    first = (measure_from) * M
    fr, fr0, info, ideal = fr[first:], fr0[first:], info[first:], ideal[first:]
    oc, oc0 = _ds_all(fr, signal, filt), _ds_all(fr0, signal, filt)
    on, on0 = [signal.decode(c) for c in oc], [signal.decode(c) for c in oc0]
    # truth: the true pictures through the SAME filter (what a perfect generator would give on this display)
    ta = ds._area_weights(PLANE, PLANE * 2 // 3)
    foot_out = (ta @ alpha @ ta.T) > 0.05
    foot_grown = ndimage.binary_dilation(foot_out, iterations=2)
    oy = ox = PLANE // 3
    win = np.zeros_like(foot_out); win[oy - 40:oy + 40, ox - 40:ox + 40] = True
    thr = 8.0 / 255.0
    true_on = signal.decode(ds.downscale(signal.encode(sc.render(float(measure_from))), signal, filt))
    true_on0 = signal.decode(ds.downscale(signal.encode(sc0.render(float(measure_from))), signal, filt))
    e_true = float((true_on - true_on0)[foot_out].sum())
    ex_on = HDR.decode(ds.downscale(signal.encode(sc.render(float(measure_from))), signal, "exact")) if signal.hdr else None
    ex_on0 = HDR.decode(ds.downscale(signal.encode(sc0.render(float(measure_from))), signal, "exact")) if signal.hdr else None
    e_exact = float((ex_on - ex_on0)[foot_out].sum()) if signal.hdr else e_true
    peak_true = float((true_on - true_on0)[foot_out].max())
    peak_exact = float((ex_on - ex_on0)[foot_out].max()) if signal.hdr else peak_true
    bright, posx, posy, wid, gpx, gcp, gen_ = [], [], [], [], [], [], []
    ar0, ar1 = oy - 5, oy + 6
    ac0, ac1 = ox + int(round(8 / 1.5)), ox + int(round(24 / 1.5))
    for c, c0, n, n0 in zip(oc, oc0, on, on0):
        added = n - n0
        bright.append(float(added[foot_out].sum()) / e_true)
        local = np.clip(added, 0, None) * ndimage.binary_dilation(foot_out, iterations=3)
        s = local.sum()
        yy, xx = np.mgrid[0:local.shape[0], 0:local.shape[1]]
        posx.append(float((local * xx).sum() / s - (true_on - true_on0).clip(0)[foot_out].dot(xx[foot_out]) / e_true) if s > 0 else np.nan)
        posy.append(float((local * yy).sum() / s - (true_on - true_on0).clip(0)[foot_out].dot(yy[foot_out]) / e_true) if s > 0 else np.nan)
        wid.append(mt.fwhm(added[ar0:ar1, ac0:ac1].mean(axis=1)))
        p, cp, en = mt.ghosts(np.abs(c - c0), foot_grown, win, thr)
        gpx.append(p); gcp.append(cp); gen_.append(en)
    # what he sees vs the ideal view (true frames through the exact filter): the error in the crosshair's window, PQ / SDR
    # codes x 1023, mean over the window's pixels and the displayed frames: covers BOTH defects (dimmed crosshair, copies)
    ideal_oc = [ds.downscale(f, signal, "exact") for f in ideal]
    hud_err = float(np.mean([np.abs(c - t)[win].mean() * 1023 for c, t in zip(oc, ideal_oc)]))
    starts, _ = display.schedule(M, len(oc) + 1, "ideal")
    series = mt.per_refresh(bright, starts)
    fps = 30.0 * M
    # copies at 4K (before the downscale) for the mechanism
    foot4 = ndimage.binary_dilation(alpha > 0.02, iterations=3)
    win4 = np.zeros_like(foot4); win4[CY - 60:CY + 60, CX - 60:CX + 60] = True
    g4 = [mt.ghosts(np.abs(a - b), foot4, win4, thr) for a, b in zip(fr, fr0)]
    gen_idx = [i for i in range(len(info)) if info[i][0]]
    pos_err = np.hypot(np.nan_to_num(posx, nan=99), np.nan_to_num(posy, nan=99))
    return {"M": M, "guard": guard, "filter": filt, "gen": gen, "width_4k": width, "phase": phase, "hud_err": hud_err,
            "static_energy_vs_exact": e_true / e_exact, "static_peak_nits": peak_true, "static_peak_exact": peak_exact,
            "bright_mean": float(np.mean(bright)), "bright_min": float(np.min(bright)),
            "flicker30_pct": 100 * mt.tone_at(series, display.REFRESH_HZ, 30.0),
            "pos_err_mean": float(np.mean(pos_err)), "pos_err_max": float(np.max(pos_err)),
            "width_mean": float(np.mean(wid)), "width_sd": float(np.std(wid)),
            "ghost_px_per_frame": float(np.mean(gpx)), "copies_per_frame": float(np.mean(gcp)),
            "copies_per_s": float(np.mean(gcp)) * fps, "ghost_energy_per_s": float(np.mean(gen_)) * fps,
            "ghost_px_4k_gen": float(np.mean([g4[i][0] for i in gen_idx])) if gen_idx else 0.0,
            "ghost_px_out_gen": float(np.mean([gpx[i] for i in gen_idx])) if gen_idx else 0.0,
            "copy_bright_cv": _copy_cv(oc, oc0, foot_grown, win, thr, info)}


def _copy_cv(oc, oc0, foot, win, thr, info):
    """Variation of the copies' brightness from generated frame to generated frame (coefficient of variation of the summed
    added code over the copies): how much the copies twinkle."""
    vals = []
    for i, (c, c0) in enumerate(zip(oc, oc0)):
        if not info[i][0]:
            continue
        d = np.abs(c - c0)
        m = win & ~foot
        vals.append(float(d[m].sum()))
    v = np.array(vals)
    return float(v.std() / v.mean()) if len(v) and v.mean() > 0 else 0.0


def copy_count(M, speed=96.0, n_src=12):
    """Sanity: distinct crosshair images in one real-frame period (a 4-px dot, straight pan; block model; 4K, no downscale).
    Expected 3 at 2X and 5 at 3X (HUD_GHOSTING.md section 2)."""
    sc, alpha = hud_scene("dot", 4, 1.0, speed, 0, "pan", True)
    sc0, _ = hud_scene("dot", 4, 1.0, speed, 0, "pan", False)
    fr, _, info = chain.sequence(sc, HDR, M, "block", n_src)
    fr0, _, _ = chain.sequence(sc0, HDR, M, "block", n_src)
    j = 6 * M
    places = []
    for f, f0 in zip(fr[j:j + M], fr0[j:j + M]):
        added = np.clip(HDR.decode(f) - HDR.decode(f0), 0, None)
        lab, n = ndimage.label(added > 5.0)
        for k in range(1, n + 1):
            ys, xs = np.nonzero(lab == k)
            places.append(round(float(xs.mean()) / 2) * 2)
    return len(set(places)), sorted(set(places))


# ---- Q4: block MODEL vs PushWarp (CPU reference on the FidelityFX-like flow MODEL) vs ideal ----------------------------
def _plane(w, h, world, path, objects=(), shutter=0.0):
    # large planes: 1 vertical sub-sample (the textures are smooth vertically; objects have exact coverage) to stay small
    return scenes.Scene(w, h, world, path, shutter=shutter, objects=objects, qy=1 if w * h > 600000 else scenes.Q)


def q4_scenes(quick=True):
    tex = lambda w, h, c=0.7: scenes.textured(w, h, mean=30.0, contrast=c, seed=5)
    out = [("texture pan 24 px/f (480 px/s)", _plane(512, 256, tex(512, 256), scenes.pan(24.0))),
           ("texture pan 96 px/f (1920 px/s)", _plane(512, 256, tex(512, 256), scenes.pan(96.0))),
           ("fast turn 300 px/f (6000 px/s)", _plane(1536, 512, tex(1536, 512), scenes.pan(300.0))),
           ("thin object 6 px, 20 px/f, still camera", _plane(512, 256, tex(512, 256, 0.03), scenes.still(),
                                                              objects=[{"x": 100, "y": 60, "w": 6, "h": 120, "vx": 20.0, "nits": 200.0}])),
           ("thin object 6 px, 12 px/f, still camera", _plane(512, 256, tex(512, 256, 0.03), scenes.still(),
                                                              objects=[{"x": 100, "y": 60, "w": 6, "h": 120, "vx": 12.0, "nits": 200.0}])),
           ("object 48 px over a counter-pan (occlusion)", _plane(512, 256, tex(512, 256), scenes.pan(-24.0),
                                                                  objects=[{"x": 200, "y": 64, "w": 48, "h": 128, "vx": 24.0, "nits": 150.0}]))]
    if not quick:
        out.append(("fast turn 900 px/f (18000 px/s)", _plane(3840, 512, tex(3840, 512), scenes.pan(900.0))))
    return out


def q4(quick=True, n_src=6):
    rows = []
    thr = 32.0 / 1023.0
    for name, sc in q4_scenes(quick):
        reals = chain.real_frames(sc, HDR, n_src)
        for M in (2, 3):
            truth, _, info = chain.sequence(sc, HDR, M, "ideal", n_src, reals=reals)
            tout = [ds.downscale(f, HDR, "exact") for f in truth]
            for gen in ("block", "mixed", "pushwarp"):
                if gen == "mixed" and M == 2:
                    continue
                fr, _, info = chain.sequence(sc, HDR, M, gen, n_src, reals=reals, guard=(gen == "pushwarp"))
                oc = [ds.downscale(f, HDR, "exact") for f in fr]
                gi = [i for i in range(len(info)) if info[i][0]]
                err = [np.abs(oc[i] - tout[i]) * 1023 for i in gi]
                r = {"scene": name, "M": M, "gen": gen, "mae_codes": float(np.mean([e.mean() for e in err])),
                     "p99_codes": float(np.mean([np.percentile(e, 99) for e in err])),
                     "bad_px_pct": float(100 * np.mean([(e > 32).mean() for e in err]))}
                starts, _ = display.schedule(M, len(oc) + 1, "ideal")
                r["unique_fps"] = mt.unique_frames(oc, info, starts)[0]
                r["whole_frame_fallbacks"] = int(sum(1 for i in gi if len(info[i]) > 2 and info[i][2].get("wholeFrameNearest")))
                if sc.objects and "thin" in name:   # share of the thin object's pixels wrong by > 32 PQ codes (4K)
                    kept = []
                    for i in gi:
                        cov = sc.object_cover(sc.objects[0], (i // M) + info[i][1], blur=False) > 0.5
                        kept.append(float((np.abs(fr[i] - truth[i])[cov] * 1023 <= 32).mean()))
                    r["object_kept_pct"] = 100 * float(np.mean(kept))
                rows.append(r)
    return rows


# ---- HDR: thin highlights through the GENERATOR (blending in PQ codes) ------------------------------------------------
def hdr_generator(n_src=8):
    """A 2-px 1000-nit highlight line on 50 nits panning 1.5 px a real frame (sub-pixel positions in every generated frame)
    through the block MODEL (bilinear taps + blend in PQ codes, as the generators receive PQ) vs the truth; and a cross-fade
    copy's brightness in nits (a crosshair copy at code weight 1/3 or 2/3)."""
    rows = []
    sc = scenes.Scene(96, 12, scenes.lines(50.0, [(40, 2, 1000.0)]), scenes.pan(1.5), qy=1)
    for M in (2, 3):
        for gen in ("ideal", "block"):
            fr, _, info = chain.sequence(sc, HDR, M, gen, n_src)
            en = []
            for f in fr:
                o = HDR.decode(ds.downscale(f, HDR, "exact"))[4]
                en.append(float((o - 50.0).clip(min=0).sum()))
            starts, _ = display.schedule(M, len(fr) + 1, "ideal")
            gi = [i for i in range(len(info)) if info[i][0]]
            truth = 950.0 * 2 / 1.5
            rows.append({"M": M, "gen": gen, "energy_real_pct": 100 * np.mean([en[i] for i in range(len(en)) if i not in gi]) / truth,
                         "energy_generated_pct": 100 * np.mean([en[i] for i in gi]) / truth,
                         "flicker30_pct": 100 * mt.tone_at(mt.per_refresh(en, starts), display.REFRESH_HZ, 30.0)})
    copies = []
    for w in (1 / 3, 2 / 3):
        code = w * tf.pq_encode(300.0) + (1 - w) * tf.pq_encode(30.0)
        copies.append({"weight": w, "nits_code_blend": float(tf.pq_decode(code)), "nits_linear_blend": float(w * 300 + (1 - w) * 30)})
    return rows, copies
