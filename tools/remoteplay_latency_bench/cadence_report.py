#!/usr/bin/env python3
"""Scores the numbering of hardware-decoded pictures (veyra::engine::PushStreamDedup with
veyra::remoteplay::StreamCadence, then the engine's ContentDedup) on the stream sets of
cadence_streams.py and writes cadence_results.json, each set on its own.

  python3 cadence_report.py --eval <cadence_eval binary> --sets <folder with legacy/ tune/ holdout/ adversarial/>
                            [--out cadence_results.json] [--frozen "text"] [--only tune,adversarial] [--sweep]

The sets (see cadence_streams.py): `legacy` is the first study's synthetic scene, `tune` is what the
thresholds in StreamCadence.h were chosen on, `adversarial` is what a block mean cannot see (the
scenes of the independent review; also used for tuning: its bar is zero dropped new pictures),
`holdout` was generated without printing a single statistic and scored after the thresholds were
frozen. Only `holdout` says anything about streams the classifier has not seen; none is a PS5 stream.

Four scorings per set:
  all         what the engine gives the classifier: block means and every sample; it must find scene cuts itself
  cutsTold    the same, with the scene cuts given to it
  blocksOnly  without the per-sample evidence (what the pass measured before the review), same classifier
  maxOnly     the largest block difference alone; NOT what the engine uses

--sweep (tuning only, never on the hold-out set): one threshold at a time moved off its default, on
the tune and adversarial sets.
"""
import argparse, collections, json, pathlib, subprocess, sys

MODES = {"all": [], "cutsTold": ["--cuts"], "blocksOnly": ["--no-pixels"], "maxOnly": ["--max-only"]}
SWEEP = {"pixelCeiling": [8, 12, 24, 32, 1e9], "pixelRatio": [0.1, 0.25, 0.6, 1e9], "rhythmFloor": [0, 0.6, 2.4],
         "ratio": [0.3, 0.35, 0.45, 0.5], "ceiling": [1.5, 2.0, 3.5, 5.0], "meanRatio": [0.1, 0.15, 0.35, 0.5, 1e9],
         "meanVeto": [2.0, 3.0, 6.0, 1e9], "shareVeto": [0.5, 0.7, 2.0], "cutFactor": [2.0, 4.0, 1e9],
         "confidenceFloor": [0.0, 0.3, 0.7, 0.9], "lockSteps": [4, 8, 10], "marginGain": [1.0, 4.0]}
PIXEL_STEPS = [1, 2, 8, 16]
KEEP = ("label", "content", "source", "cadence", "motion", "grain", "halfRateObjects", "kind", "contrast", "lead", "encoder", "width", "height", "frames", "cuts",
        "newMedian", "repeatMedian", "repeatMaxBlockDiff", "newMinBlockDiff", "repeatMaxMeanDiff", "newMinMeanDiff", "repeatMaxPixelDiff", "newMinPixelDiff")
COUNTS = ("new", "repeat", "droppedNew", "droppedNewIdentical", "falseRepeat", "missedRepeat", "newWhileLocked", "fallbacks", "pixelVetoes")


def run_eval(binary, files, extra):
    out = subprocess.run([binary, *extra, *map(str, files)], capture_output=True, text=True)
    if out.returncode not in (0, 1): sys.exit(f"cadence_eval failed: {out.stderr[:400]}")
    rows = [json.loads(line) for line in out.stdout.splitlines() if line.startswith("{")]
    return [r for r in rows if not r.get("total")], next(r for r in rows if r.get("total"))


def tally(rows):
    t = collections.Counter()
    for r in rows:
        for k in COUNTS: t[k] += r[k]
    t = {k: t[k] for k in COUNTS}; t["streams"] = len(rows)
    t["worstDroppedChange"] = max((r["worstDroppedChange"] for r in rows), default=0)
    t["worstDroppedPixel"] = max((r["worstDroppedPixel"] for r in rows), default=0)
    t["streamsWithDroppedNew"] = sum(1 for r in rows if r["droppedNew"])
    t["missedRepeatShare"] = round(t["missedRepeat"] / t["repeat"], 4) if t["repeat"] else None
    return t


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--eval", required=True); parser.add_argument("--sets", required=True, type=pathlib.Path)
    parser.add_argument("--out", type=pathlib.Path); parser.add_argument("--frozen", default="")
    parser.add_argument("--only", default="legacy,tune,adversarial,holdout"); parser.add_argument("--sweep", action="store_true")
    parser.add_argument("--merge", type=pathlib.Path, help="an earlier result file: its sets are kept unless scored again now")
    args = parser.parse_args()
    if args.sweep:
        sets = {which: sorted((args.sets / which).glob("*.txt")) for which in ("tune", "adversarial")}
        def line(name, extra):
            parts = []
            for which, files in sets.items():
                _, t = run_eval(args.eval, files, extra)
                parts.append(f"{which}: dropped {t['droppedNew']:4d} (identical {t['droppedNewIdentical']}) missed {t['missedRepeat']:5d} of {t['repeat']} worst sample {t['worstDroppedPixel']:.0f}")
            print(f"{name:24s} " + " | ".join(parts), flush=True)
        line("default", [])
        for name, values in SWEEP.items():
            for value in values: line(f"{name}={value}", ["--set", f"{name}={value}"])
        for step in PIXEL_STEPS: line(f"pixel step {step}", ["--pixel-step", str(step)])
        return 0
    result = {"note": "Linux, libx264/libx265 through PyAV, 480 pictures per stream (adversarial: 600); NOT PS5 streams, not the PS5's encoder. "
                      "Each stream goes through PushStreamDedup and ContentDedup as in the engine. droppedNew: a new game picture the engine would drop "
                      "(the number that matters). droppedNewIdentical: of those, pictures whose decoded luma is the same as the picture before in every "
                      "sample (nothing visible lost). falseRepeat: a new picture the classifier called a repeat. missedRepeat: a repeat that is kept "
                      "(harmless). worstDroppedPixel: the largest luma sample difference of any dropped new picture. Block differences: 16x16 block "
                      "means against the previous decoded picture, 8-bit code values. Classifier: include/veyra/remoteplay/StreamCadence.h defaults.",
              "thresholds": args.frozen, "sets": {}}
    if args.merge and args.merge.exists():
        earlier = json.loads(args.merge.read_text()); result["sets"] = earlier.get("sets", {})
        if not args.frozen: result["thresholds"] = earlier.get("thresholds", "")
    for which in args.only.split(","):
        folder = args.sets / which
        files = sorted(folder.glob("*.txt"))
        if not files: continue
        index = {r["label"]: r for r in json.loads((folder / "index.json").read_text())}
        missing = [f.stem for f in files if f.stem not in index]
        if missing: sys.exit(f"{which}: no index entry for {missing[:3]} (a generator run was interrupted: delete those files and run it again)")
        scored = {}
        for mode, extra in MODES.items():
            rows, _ = run_eval(args.eval, files, extra)
            scored[mode] = {r["stream"][:-4]: r for r in rows}
        streams = []
        for label, r in scored["all"].items():
            info = {k: index[label].get(k) for k in KEEP}; info["label"] = label
            info.update({k: r[k] for k in COUNTS + ("worstDroppedChange", "worstDroppedPixel", "closestNewToLimit", "firstLockedAt", "locks", "fastLocks", "unlocks", "cutsFound", "sourceFpsSecondHalf")})
            for mode in ("cutsTold", "blocksOnly", "maxOnly"): info["droppedNew" + mode[0].upper() + mode[1:]] = scored[mode][label]["droppedNew"]
            streams.append(info)
        def group(key):
            groups = collections.defaultdict(list)
            for s in streams: groups[str(key(s))].append(scored["all"][s["label"]])
            return {k: tally(v) for k, v in sorted(groups.items())}
        entry = {"total": {mode: tally(list(rows.values())) for mode, rows in scored.items()},
                 "byHeight": group(lambda s: s["height"]), "bySource": group(lambda s: s["source"]), "byCadence": group(lambda s: s["cadence"]),
                 "byEncoder": group(lambda s: s["encoder"]), "streams": streams}
        if which == "adversarial":
            entry["byKind"] = group(lambda s: s["kind"]); entry["byMotion"] = group(lambda s: s["motion"])
        result["sets"][which] = entry
        t = entry["total"]
        print(f"{which:11s} {t['all']['streams']:3d} streams, {t['all']['new']} new, {t['all']['repeat']} repeats | all: dropped new {t['all']['droppedNew']} "
              f"(identical {t['all']['droppedNewIdentical']}, worst sample {t['all']['worstDroppedPixel']:.0f}) kept repeats {t['all']['missedRepeat']} | "
              f"cuts told: dropped {t['cutsTold']['droppedNew']} | blocks only: dropped {t['blocksOnly']['droppedNew']} | max only: dropped {t['maxOnly']['droppedNew']}")
        for name in ("byHeight", "bySource", "byCadence", "byEncoder", "byKind", "byMotion"):
            if name in entry:
                print("   " + name + ": " + "; ".join(f"{k}: dropped {v['droppedNew']} kept {v['missedRepeat']}/{v['repeat']}" for k, v in entry[name].items()))
    if args.out: args.out.write_text(json.dumps(result, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
