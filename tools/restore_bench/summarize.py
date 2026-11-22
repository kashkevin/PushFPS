#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""results.csv of restore_bench -> Markdown tables (docs/RESTORATION.md).

usage: summarize.py results.csv [--headline SETTING] [--full]

Every number: MODEL content, SOFTWARE encoder, CPU reference; not the PS5's encoder, not a GPU.
"off" is the picture as decoded (Package 7's behaviour); the other rows show the change against it,
so a loss reads as plainly as a gain.
"""
import csv
import sys

PSNR = ("psnr_y", "psnr_gradient", "psnr_dark", "psnr_texture", "psnr_hud", "psnr_lines")


def num(row, key):
    return float(row[key])


def signed(value, digits=2):
    return f"{value:+.{digits}f}"


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    headline = "all-50"
    full = "--full" in args
    if "--headline" in args:
        headline = args[args.index("--headline") + 1]
    rows = list(csv.DictReader(open(args[0], encoding="utf-8")))
    if not rows:
        print("no results")
        return 1
    streams, settings = [], []
    for r in rows:
        if r["stream"] not in streams:
            streams.append(r["stream"])
        if r["setting"] not in settings:
            settings.append(r["setting"])
    by = {(r["stream"], r["setting"]): r for r in rows}
    off = {s: by[(s, "off")] for s in streams}

    print("Model content, software encoder, CPU reference, 8-bit SDR. PSNR in dB of luma against the pristine")
    print("picture; every \"change\" is against \"off\" (the picture as decoded), so a loss is a minus sign.")
    print("Blockiness: 1 = no 8-pixel grid. Contours: band edges (a code step with flat runs on both sides) per")
    print("1000 pixels of the sky and dark regions after the final 8-bit write, plainly rounded / with the")
    print("ordered dither. HUD edge shift: mean distance in pixels between the HUD's strong edges and where the")
    print("pristine picture has them. HUD flicker: mean change of luma between two frames inside the HUD, which")
    print("does not move, in 8-bit code values.\n")

    if (streams[0], headline) in by:
        print(f"#### Setting `{headline}` against off, per stream\n")
        print("| stream | PSNR off | change: all | gradient | dark | texture | HUD | lines | SSIM | blockiness off -> on | "
              "contours rounded off -> on | on, dithered | HUD edge shift off -> on | HUD flicker off -> on |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for s in streams:
            o, r = off[s], by[(s, headline)]
            d = [signed(num(r, k) - num(o, k)) for k in PSNR]
            print(f"| {s} | {num(o, 'psnr_y'):.2f} | {d[0]} | {d[1]} | {d[2]} | {d[3]} | {d[4]} | {d[5]} | "
                  f"{signed(num(r, 'ssim') - num(o, 'ssim'), 4)} | {num(o, 'blockiness'):.3f} -> {num(r, 'blockiness'):.3f} | "
                  f"{num(o, 'contours_round'):.1f} -> {num(r, 'contours_round'):.1f} | {num(r, 'contours_dither'):.1f} | "
                  f"{num(o, 'hud_edge_shift_px'):.3f} -> {num(r, 'hud_edge_shift_px'):.3f} | "
                  f"{num(o, 'hud_flicker_codes'):.3f} -> {num(r, 'hud_flicker_codes'):.3f} |")
        print()

    print(f"#### Every setting: mean over the {len(streams)} streams, and the worst stream\n")
    print("| setting | strength | parts | PSNR all: mean (worst) | texture: mean (worst) | HUD: mean (worst) | dark: mean | "
          "contours rounded, mean change % | with dither % | blockiness, mean change | HUD flicker, mean change | pixels changed % |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for setting in settings:
        if setting == "off":
            continue
        rs = [(by[(s, setting)], off[s]) for s in streams if (s, setting) in by]
        def stat(key, digits=2):
            deltas = [num(r, key) - num(o, key) for r, o in rs]
            return f"{signed(sum(deltas) / len(deltas), digits)} ({signed(min(deltas), digits)})"
        mean = lambda values: sum(values) / len(values)
        contours = mean([100.0 * (num(r, "contours_round") / num(o, "contours_round") - 1.0) for r, o in rs if num(o, "contours_round") > 0])
        dithered = mean([100.0 * (num(r, "contours_dither") / num(o, "contours_round") - 1.0) for r, o in rs if num(o, "contours_round") > 0])
        r0 = rs[0][0]
        print(f"| {setting} | {r0['strength']} | {int(r0['parts']) & 31} | {stat('psnr_y')} | {stat('psnr_texture')} | {stat('psnr_hud')} | "
              f"{signed(mean([num(r, 'psnr_dark') - num(o, 'psnr_dark') for r, o in rs]))} | {contours:+.0f} | {dithered:+.0f} | "
              f"{signed(mean([num(r, 'blockiness') - num(o, 'blockiness') for r, o in rs]), 3)} | "
              f"{signed(mean([num(r, 'hud_flicker_codes') - num(o, 'hud_flicker_codes') for r, o in rs]), 3)} | "
              f"{mean([num(r, 'changed_pct') for r, o in rs]):.0f} |")
    print()
    o0 = off[streams[0]]
    if "contours_pristine_dither" in o0:
        print(f"Contours of the pristine picture itself: {num(o0, 'contours_pristine_round'):.1f} when only rounded to 8 bits (an ideal "
              f"gradient is fully banded then), {num(o0, 'contours_pristine_dither'):.1f} with the ordered dither.\n")
    if not full:
        return 0
    for stream in streams:
        o = off[stream]
        print(f"#### {stream}: every setting (frames measured: {o['frames']})\n")
        print("| setting | PSNR all | gradient | dark | texture | HUD | lines | SSIM | blockiness | contours round / dither | "
              "HUD contrast | HUD edge shift px | HUD flicker codes | pixels changed % |")
        print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
        for setting in settings:
            r = by.get((stream, setting))
            if not r:
                continue
            is_off = setting == "off"
            def cell(key, digits=2):
                return f"{num(r, key):.{digits}f}" if is_off else signed(num(r, key) - num(o, key), digits)
            print(f"| {setting} | {cell('psnr_y')} | {cell('psnr_gradient')} | {cell('psnr_dark')} | {cell('psnr_texture')} | "
                  f"{cell('psnr_hud')} | {cell('psnr_lines')} | {cell('ssim', 4)} | {cell('blockiness', 3)} | "
                  f"{num(r, 'contours_round'):.2f} / {num(r, 'contours_dither'):.2f} | {num(r, 'hud_contrast'):.4f} | "
                  f"{num(r, 'hud_edge_shift_px'):.3f} | {num(r, 'hud_flicker_codes'):.3f} | {num(r, 'changed_pct'):.1f} |")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
