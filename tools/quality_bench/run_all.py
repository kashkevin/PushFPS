#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Runs every experiment of the offline quality bench and writes the tables (markdown) and the raw rows (JSON).

  python3 -B tools/quality_bench/run_all.py --quick [--out /tmp/quality_bench_report.md]

SIMULATION OF A MODEL CHAIN on synthetic 4K pictures (CPU only): mechanisms and their relative sizes, not a measurement of
the owner's screen, capture card, graphics card or NVIDIA's generator. Deterministic (fixed seeds). Needs numpy, scipy, g++.
Temporary files (job files, built tools) go to $QB_BUILD or /tmp/quality_bench_build and are small (a few MB).
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qb import display, downscale as ds, experiments as E, native, observer as ob, scenes, tf  # noqa: E402


def table(rows, cols, fmt=None):
    fmt = fmt or {}
    out = ["| " + " | ".join(h for h, _ in cols) + " |", "|" + "---|" * len(cols)]
    for r in rows:
        cells = []
        for _, k in cols:
            v = r(k) if callable(r) else (k(r) if callable(k) else r.get(k, ""))
            if isinstance(v, (float, np.floating)):
                v = format(float(v), fmt.get(k if isinstance(k, str) else "", ".2f"))
            cells.append(str(v))
        out.append("| " + " | ".join(cells) + " |")
    return "\n".join(out)


def validation(md, raw):
    md.append("## V. Validation (sanity cases with known answers)\n")
    # static picture: zero hold blur, zero judder
    run = E.StripRun()
    p = E.q1_point(run, 0.0 + 1e-12, 3, "ideal", 0.0, "exact", "ideal")
    rows = [{"case": "static picture, 90 FPS, ideal holds", "expected": "blur 0 (a pixel-aligned edge stays a step); judder 0",
             "got": f"blur {p['blur_px']:.2f} px, judder {p['judder_rms']:.3f} px"}]
    for M in (1, 2, 3):
        q = E.q1_point(run, 960, M, "ideal", 0.0, "exact", "ideal")
        exp = 960 / (30 * M)
        rows.append({"case": f"hold blur, 960 px/s, {30 * M} FPS, ideal everything", "expected": f"sqrt({exp:.2f}^2 + static^2) = {np.hypot(exp, p['blur_px']):.2f} px",
                     "got": f"{q['blur_px']:.2f} px (10-90 % rise {q['rise_px']:.2f})"})
    for M in (2, 3):
        n, places = E.copy_count(M)
        rows.append({"case": f"crosshair images per real-frame period, block MODEL, {M}X", "expected": "3" if M == 2 else "5",
                     "got": f"{n} (x = {places})"})
    # downscale reproductions (S12_PACE_REVIEW2 tables)
    lines = E.q2_lines(E.HDR, 300.0, 10.0)
    for r in lines:
        if r["width"] == 1:
            exp = {"p16": "1.10 / 1.35 / 112", "area": "0.42 / 0.75 / 99", "exact": "0.000 / 0.555 / 131"}[r["filter"]]
            rows.append({"case": f"moving 1-px line 10->300 nits, quarter-pixel steps, {r['filter']}: energy pulse / peak pulse / mean peak",
                         "expected": exp + " (reviewer)", "got": f"{r['energy_pulse']:.2f} / {r['peak_pulse']:.2f} / {r['peak_mean']:.0f}"})
    for r in E.q2_strokes():
        exp = {("p16", 1): "44 / 1", ("p16", 2): "222 / 66", ("exact", 1): "150 / 100", ("exact", 2): "250 / 200"}.get((r["filter"], r["width"]))
        if exp:
            rows.append({"case": f"{r['width']}-px stroke 300 nits, column phases, {r['filter']}: peak mean / worst (nits)",
                         "expected": exp + " (reviewer)", "got": f"{r['peak_mean']:.0f} / {r['peak_worst']:.0f}"})
    shares = []
    for M in (1, 2, 3):
        ev = [display.hold_histogram(display.schedule(M, 30 * M * 10, "composed", seed=s)[0], M)["even_pct"] for s in range(4)]
        shares.append(f"{30 * M} FPS {np.mean(ev):.0f} %")
    rows.append({"case": "composed-path hold model: share of frames held exactly k refreshes", "expected": "67-68 % (s12-pace's fit)",
                 "got": ", ".join(shares)})
    md.append(table(rows, [("case", "case"), ("expected", "expected"), ("got (SIMULATED here)", "got")]))
    md.append("")
    raw["validation"] = rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--quick", action="store_true", help="fewer speeds, seeds and phases (about 4 minutes on 2 cores)")
    ap.add_argument("--out", type=Path, default=Path("/tmp/quality_bench_report.md"))
    ap.add_argument("--only", default="", help="comma list of sections: V,Q1,Q2,Q3,Q4,HDR")
    a = ap.parse_args()
    only = set(a.only.split(",")) if a.only else {"V", "Q1", "Q2", "Q3", "Q4", "HDR"}
    t0 = time.time()
    md = [f"# Quality bench tables ({'quick' if a.quick else 'full'} run) - SIMULATED, a model chain on synthetic pictures\n"]
    raw = {}
    for n in ("pushwarp_cli", "hud_guard_seq"):
        native.build(n)
    if "V" in only:
        validation(md, raw)
        print(f"[V] {time.time() - t0:.0f} s", flush=True)
    if "Q1" in only:
        rows, factors = E.q1(quick=a.quick)
        md.append("## Q1. Native 30 vs generated 60 vs 90\n\n### Q1a. Everything ideal (ideal generator, ideal holds, no source blur, exact downscale), HDR10\n")
        md.append(table(rows, [("speed px/s", "v_out"), ("FPS", "fps"), ("unique frames/s", "unique_fps"), ("step px", "step_px"),
                               ("blur width px", "blur_px"), ("10-90 rise px", "rise_px"), ("MTF 0.1", "mtf01"), ("MTF 0.2", "mtf02"),
                               ("MTF 0.3", "mtf03"), ("judder RMS px", "judder_rms")], {"mtf01": ".3f", "mtf02": ".3f", "mtf03": ".3f"}))
        md.append("\n### Q1b. One reason at a time, 960 output px/s (60 -> 90 gain kept)\n")
        frows = []
        g0 = None
        for name, pts in factors:
            b60, b90, r60, r90 = pts[2]["blur_px"], pts[3]["blur_px"], pts[2]["rise_px"], pts[3]["rise_px"]
            gain = b60 - b90
            g0 = g0 if g0 is not None else gain
            rg0 = factors[0][1][2]["rise_px"] - factors[0][1][3]["rise_px"]
            frows.append({"name": name, "b30": pts[1]["blur_px"], "b60": b60, "b90": b90, "gain": gain, "kept": 100 * gain / g0,
                          "r60": r60, "r90": r90, "rkept": 100 * (r60 - r90) / rg0,
                          "j60": pts[2]["judder_rms"], "j90": pts[3]["judder_rms"], "jw90": pts[3]["judder_worst"],
                          "jp60": 100 * pts[2]["judder_rms"] / pts[2]["step_px"], "jp90": 100 * pts[3]["judder_rms"] / pts[3]["step_px"],
                          "e90": pts[3]["even_pct"], "u90": pts[3]["unique_fps"]})
        md.append(table(frows, [("condition", "name"), ("blur 30", "b30"), ("blur 60", "b60"), ("blur 90", "b90"),
                                ("60->90 gain px", "gain"), ("gain kept %", "kept"), ("rise 60", "r60"), ("rise 90", "r90"),
                                ("rise gain kept %", "rkept"), ("judder RMS 60", "j60"), ("judder RMS 90", "j90"),
                                ("judder % of step 60", "jp60"), ("judder % of step 90", "jp90"), ("judder worst 90", "jw90"), ("even holds 90 %", "e90"), ("unique/s 90", "u90")], {"kept": ".0f", "rkept": ".0f", "e90": ".0f", "u90": ".1f", "jp60": ".0f", "jp90": ".0f"}))
        md.append("")
        raw["q1"] = {"ideal": rows, "factors": [(n, p) for n, p in factors]}
        print(f"[Q1] {time.time() - t0:.0f} s", flush=True)
    if "Q2" in only:
        md.append("## Q2. Current vs improved downscaling (HDR10)\n\n### Q2a. Thin lines 300 nits on 10 nits, all quarter-pixel phases\n")
        md.append(table(E.q2_lines(), [("4K width", "width"), ("filter", "filter"), ("peak mean nits", "peak_mean"), ("peak worst", "peak_worst"),
                                         ("energy error %", "energy_err_pct"), ("energy pulse", "energy_pulse"), ("peak pulse", "peak_pulse"),
                                         ("FWHM px", "fwhm_mean"), ("FWHM range", "fwhm_range")], {"peak_mean": ".0f", "peak_worst": ".0f", "energy_err_pct": ".0f"}))
        md.append("\n### Q2b. Pixel-aligned HUD strokes 300 nits on black (column phases)\n")
        md.append(table(E.q2_strokes(), [("4K width", "width"), ("filter", "filter"), ("peak mean nits", "peak_mean"), ("worst column", "peak_worst")], {"peak_mean": ".0f", "peak_worst": ".1f"}))
        md.append("\n### Q2c. Small text (strokes 2 px at 4K), static and drifting a quarter pixel a frame\n")
        md.append(table(E.q2_text(quick=a.quick), [("filter", "filter"), ("stroke contrast static, mean of 3 phases (nits)", "stroke_peak_static_mean"), ("static, worst phase", "stroke_peak_static_worst"),
                                                   ("energy vs exact %", "energy_vs_exact_pct"), ("energy pulse moving", "energy_pulse_moving"), ("stroke contrast pulse moving", "peak_pulse_moving")],
                        {"stroke_peak_static_mean": ".0f", "stroke_peak_static_worst": ".0f", "energy_vs_exact_pct": ".0f"}))
        md.append("\n### Q2d. Moire, shimmer and the price in sharpness (contrast kept at a period of N output px)\n")
        md.append(table(E.q2_moire_and_sharpness(), [("filter", "filter"), ("moire RMS % of amplitude", "moire_rms_pct"), ("shimmer % (moving)", "shimmer_pct"),
                                                     ("0.1 c/px", "contrast_0.1"), ("0.2", "contrast_0.2"), ("0.25 (4 px)", "contrast_0.25"),
                                                     ("0.333 (3 px)", "contrast_0.333"), ("0.4 (2.5 px)", "contrast_0.4")],
                        {"moire_rms_pct": ".1f", "shimmer_pct": ".1f", "contrast_0.1": ".3f", "contrast_0.2": ".3f", "contrast_0.25": ".3f", "contrast_0.333": ".3f", "contrast_0.4": ".3f"}))
        hl, big = E.q2_highlights()
        md.append("\n### Q2e. HDR highlights: thin 1000-nit lines on 50 nits (all phases) and a large 1000-nit patch\n")
        md.append(table(hl, [("4K width", "width"), ("filter", "filter"), ("peak mean nits", "peak_mean"), ("energy error %", "energy_err_pct"), ("energy pulse", "energy_pulse")], {"peak_mean": ".0f", "energy_err_pct": ".0f"}))
        md.append("")
        md.append(table(big, [("filter", "filter"), ("1000-nit patch shown as nits", "patch_nits"), ("PQ code error (of 1023)", "patch_pq_err_codes")], {"patch_nits": ".1f"}))
        md.append("\n### Q2f. Hue at saturated edges (1000-nit BT.2020 red stroke on a 100-nit green field)\n")
        md.append(table(E.hue_test(), [("4K width", "width"), ("filter", "filter"), ("dE ITP mean", "dEITP_mean"), ("dE ITP worst", "dEITP_worst"), ("hue shift worst deg", "hue_shift_deg_worst")], {"dEITP_mean": ".1f", "dEITP_worst": ".1f", "hue_shift_deg_worst": ".1f"}))
        md.append("")
        print(f"[Q2] {time.time() - t0:.0f} s", flush=True)
    if "Q3" in only:
        md.append("## Q3. Interaction: downscale x crosshair ghosting (block MODEL, circular camera move 48 4K px a frame = 960 output px/s, HDR10)\n")
        cases = [("crosshair 1 px, phase 0", dict(width=1, phase=0)), ("crosshair 1 px, phase 1 (the dark column)", dict(width=1, phase=1)),
                 ("crosshair 2 px", dict(width=2, phase=0))]
        if not a.quick:
            cases += [("crosshair 1 px, phase 2", dict(width=1, phase=2)), ("crosshair 2 px, phase 1", dict(width=2, phase=1)),
                      ("crosshair 2 px, 60 % opacity", dict(width=2, phase=0, opacity=0.6)), ("crosshair 3 px", dict(width=3, phase=0)),
                      ("text 2 px strokes", dict(kind="text", width=2, phase=0)), ("minimap ring 3 px", dict(kind="ring", width=3, phase=0))]
        q3 = []
        for name, kw in cases:
            for M in (2, 3):
                for guard in (False, True):
                    for filt in ds.FILTERS:
                        r = E.hud_case(M, guard, filt, **kw)
                        r["case"] = name
                        q3.append(r)
            print(f"[Q3] {name} {time.time() - t0:.0f} s", flush=True)
        cols = [("case", "case"), ("x", "M"), ("HUD Guard", lambda r: "on" if r["guard"] else "off"), ("downscale", "filter"),
                ("HUD window error (codes)", "hud_err"), ("crosshair light vs exact filter", "static_energy_vs_exact"), ("brightness mean (vs truth)", "bright_mean"), ("brightness min", "bright_min"),
                ("30 Hz flicker %", "flicker30_pct"), ("position error px", "pos_err_mean"), ("width SD px", "width_sd"),
                ("ghost px / frame", "ghost_px_per_frame"), ("copies / s", "copies_per_s"), ("ghost energy / s", "ghost_energy_per_s"),
                ("copy twinkle CV", "copy_bright_cv")]
        md.append(table(q3, cols, {"flicker30_pct": ".1f", "copies_per_s": ".0f", "ghost_energy_per_s": ".0f", "ghost_px_per_frame": ".0f"}))
        # interaction terms
        md.append("\n### Q3b. Interaction terms (P16 - exact) with HUD Guard off minus the same with it on; 0 = independent (additive)\n")
        inter = []
        for name, _ in cases:
            for M in (2, 3):
                g = {(r["guard"], r["filter"]): r for r in q3 if r["case"] == name and r["M"] == M}
                row = {"case": name, "M": M}
                for k in ("ghost_energy_per_s", "copies_per_s", "flicker30_pct", "bright_mean"):
                    off = g[(False, "p16")][k] - g[(False, "exact")][k]
                    on = g[(True, "p16")][k] - g[(True, "exact")][k]
                    row[k + "_off"] = off
                    row[k + "_on"] = on
                    row[k + "_int"] = off - on
                    base = abs(g[(False, "exact")][k] - g[(True, "exact")][k]) or 1.0
                    row[k + "_rel"] = 100 * (off - on) / base
                inter.append(row)
        md.append(table(inter, [("case", "case"), ("x", "M"), ("ghost energy: P16 effect, guard off", "ghost_energy_per_s_off"),
                                ("... guard on", "ghost_energy_per_s_on"), ("interaction % of the guard's effect", "ghost_energy_per_s_rel"),
                                ("copies/s: P16 effect off", "copies_per_s_off"), ("... on", "copies_per_s_on"),
                                ("30 Hz flicker: P16 effect off", "flicker30_pct_off"), ("... on", "flicker30_pct_on")],
                        {"ghost_energy_per_s_rel": ".0f"}))
        md.append("\n### Q3d. Combined gain vs the sum of the single gains (error in the crosshair's 80x80 output-px window against the ideal view: true frames through the exact filter, codes of 1023)\n")
        comb = []
        for name, _ in cases:
            for M in (2, 3):
                g = {(r["guard"], r["filter"]): r["hud_err"] for r in q3 if r["case"] == name and r["M"] == M}
                base = g[(False, "p16")]
                gg, gf, gc = base - g[(True, "p16")], base - g[(False, "exact")], base - g[(True, "exact")]
                comb.append({"case": name, "M": M, "base": base, "guard": gg, "filt": gf, "sum": gg + gf, "comb": gc,
                             "excess": gc - (gg + gf), "excess_pct": 100 * (gc - (gg + gf)) / (gg + gf) if gg + gf else 0.0})
        md.append(table(comb, [("case", "case"), ("x", "M"), ("error today (guard off, P16)", "base"), ("gain: guard only", "guard"),
                               ("gain: exact filter only", "filt"), ("sum of single gains", "sum"), ("gain: both", "comb"),
                               ("both - sum", "excess"), ("both - sum, % of sum", "excess_pct")], {"excess_pct": ".0f"}))
        md.append("")
        raw["q3"] = q3
        raw["q3_combined"] = comb
        # PushWarp on the same scene (overlay rules off / on)
        pw = []
        for M in (2, 3):
            for guard in (False, True):
                for filt in ("p16", "exact"):
                    r = E.hud_case(M, guard, filt, gen="pushwarp", width=2)
                    r["case"] = "crosshair 2 px, PushWarp"
                    pw.append(r)
        md.append("### Q3c. The same crosshair scene with PushWarp's CPU reference (HUD protection = its overlay rules)\n")
        md.append(table(pw, cols[1:], {"flicker30_pct": ".1f", "copies_per_s": ".0f", "ghost_energy_per_s": ".0f", "ghost_px_per_frame": ".0f"}))
        md.append("")
        raw["q3_pushwarp"] = pw
        print(f"[Q3] {time.time() - t0:.0f} s", flush=True)
    if "Q4" in only:
        q4 = E.q4(quick=a.quick)
        md.append("## Q4. Block MODEL vs PushWarp (CPU reference on the FidelityFX-like flow MODEL), against the ideal frame, exact downscale, HDR10\n")
        md.append(table(q4, [("scene", "scene"), ("x", "M"), ("generator", "gen"), ("mean error (PQ codes of 1023)", "mae_codes"),
                             ("P99 error", "p99_codes"), ("pixels > 32 codes %", "bad_px_pct"), ("thin object kept %", lambda r: f"{r['object_kept_pct']:.0f}" if "object_kept_pct" in r else ""),
                             ("whole-frame fallbacks", "whole_frame_fallbacks"), ("unique frames/s", "unique_fps")], {"unique_fps": ".1f"}))
        md.append("")
        raw["q4"] = q4
        print(f"[Q4] {time.time() - t0:.0f} s", flush=True)
    if "HDR" in only:
        rows, copies = E.hdr_generator()
        md.append("## HDR. Thin highlights through the generator (blend in PQ codes) and the brightness of a cross-faded copy\n")
        md.append(table(rows, [("x", "M"), ("generator", "gen"), ("energy in real frames %", "energy_real_pct"),
                               ("energy in generated frames %", "energy_generated_pct"), ("30 Hz flicker %", "flicker30_pct")], {"energy_real_pct": ".0f", "energy_generated_pct": ".0f", "flicker30_pct": ".1f"}))
        md.append("")
        md.append(table(copies, [("copy weight", "weight"), ("nits if blended in PQ codes (300-nit crosshair on 30 nits)", "nits_code_blend"), ("nits if blended in linear light", "nits_linear_blend")], {"weight": ".3f", "nits_code_blend": ".0f", "nits_linear_blend": ".0f"}))
        md.append("")
        raw["hdr"] = {"generator": rows, "copies": copies}
        print(f"[HDR] {time.time() - t0:.0f} s", flush=True)
    md.append(f"\n(run time {time.time() - t0:.0f} s, {'quick' if a.quick else 'full'}; SIMULATED)\n")
    a.out.write_text("\n".join(md))
    a.out.with_suffix(".json").write_text(json.dumps(raw, default=lambda o: float(o) if isinstance(o, np.floating) else str(o), indent=1))
    shutil.rmtree(native.BUILD / "tmp", ignore_errors=True)
    print(f"written {a.out} ({time.time() - t0:.0f} s)")


if __name__ == "__main__":
    main()
