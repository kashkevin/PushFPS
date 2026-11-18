#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Compares PS5 Remote Play latency runs made with scripts/remoteplay/latency-ab.ps1.

  python analyze.py <logs>/bench [--baseline original]
  python analyze.py <logs>/bench --presentmon new=new.csv original=orig.csv

Each result file is named <configuration>@<time>.txt and holds the log lines of one run of the
built-in latency test. All presses of a configuration are pooled; the comparison against the
baseline is the difference of MEANS with a bootstrap 95 % interval. The mean, not the median:
one press is quantised to the console's 16.7 ms frames, so a saving of a few milliseconds shows
as a few more presses landing one frame earlier, which moves the mean and leaves the median on
its frame. A difference whose interval contains zero is reported as not resolved, whatever its sign.

--presentmon reads PresentMon captures (1.x or 2.x column names) of the PushFPS process and
reports frame pacing: rate, frame time spread, the share of frames that took 1.5x the median
or longer, the present mode.
"""
import argparse, csv, glob, os, random, re, statistics, sys

def presses(path):
    values, summaries = [], []
    for line in open(path, encoding="utf-8-sig", errors="replace"):
        m = re.search(r"\[push-latency\] trials answerMs=\[([^\]]*)\]", line)
        if m: values += [float(v) for v in m.group(1).split(",") if v.strip()]
        m = re.search(r"\[push-latency\] done answered=(\d+)/(\d+) meanMs=([\d.]+) medianMs=([\d.]+).*pushfpsMs=([\d.]+)", line)
        if m: summaries.append({"answered": int(m.group(1)), "trials": int(m.group(2)), "median": float(m.group(4)), "pushfps": float(m.group(5))})
    return values, summaries

def percentile(sorted_values, p):
    if not sorted_values: return float("nan")
    k = (len(sorted_values) - 1) * p; lo = int(k); hi = min(lo + 1, len(sorted_values) - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (k - lo)

def bootstrap_mean_difference(a, b, rounds=4000, seed=807):
    rng = random.Random(seed); diffs = []
    for _ in range(rounds):
        diffs.append(statistics.fmean(rng.choices(a, k=len(a))) - statistics.fmean(rng.choices(b, k=len(b))))
    diffs.sort()
    return percentile(diffs, .025), percentile(diffs, .975)

def latency_report(folder, baseline):
    groups, software = {}, {}
    for path in sorted(glob.glob(os.path.join(folder, "*@*.txt"))):
        label = os.path.basename(path).split("@")[0]
        values, summaries = presses(path)
        groups.setdefault(label, []).extend(values)
        software.setdefault(label, []).extend(s["pushfps"] for s in summaries if s["pushfps"] > 0)
        if summaries and not values:
            print(f"note: {os.path.basename(path)} has a summary but no per-press values (older build); it is not used")
    if not groups: print(f"no result files in {folder}"); return 1
    if baseline not in groups:
        baseline = sorted(groups)[0]; print(f"baseline not found; using '{baseline}'")
    print(f"{'configuration':28s} {'presses':>7s} {'median':>8s} {'mean':>8s} {'p10':>7s} {'p90':>7s} {'PushFPS own':>12s}   vs {baseline} (difference of means, 95 % interval)")
    for label in sorted(groups, key=lambda l: (l != baseline, l)):
        v = sorted(groups[label])
        if not v: print(f"{label:28s} {'0':>7s}"); continue
        own = statistics.mean(software[label]) if software.get(label) else float("nan")
        row = f"{label:28s} {len(v):7d} {statistics.median(v):7.1f}  {statistics.mean(v):7.1f}  {percentile(v,.1):6.1f}  {percentile(v,.9):6.1f}  {own:10.1f}  "
        if label != baseline and groups[baseline]:
            lo, hi = bootstrap_mean_difference(v, groups[baseline])
            diff = statistics.fmean(v) - statistics.fmean(groups[baseline])
            verdict = "faster" if hi < 0 else "slower" if lo > 0 else "not resolved"
            few = "  (fewer than 120 presses on one side)" if min(len(v), len(groups[baseline])) < 120 else ""
            row += f"  {diff:+6.1f} ms  [{lo:+.1f}, {hi:+.1f}]  {verdict}{few}"
        print(row)
    print("\nAll times in ms: press handed to the session until the console's answer is in a presented picture."
          "\nThe pad poll, the display and the panel are outside this clock; use a 240 fps camera for those.")
    return 0

def column(row, *names):
    for n in names:
        if n in row and row[n] not in ("", "NA"): return row[n]
    return None

def presentmon_report(pairs):
    print(f"\n{'capture':24s} {'frames':>7s} {'fps':>7s} {'median ms':>10s} {'sd ms':>7s} {'p99 ms':>7s} {'>=1.5x median':>14s} {'to display ms':>14s}  present mode")
    for item in pairs:
        label, _, path = item.partition("=")
        frame, until, modes = [], [], {}
        with open(path, newline="", encoding="utf-8-sig", errors="replace") as handle:
            for row in csv.DictReader(handle):
                app = column(row, "Application", "ProcessName") or ""
                if "pushfps" not in app.lower(): continue
                ft = column(row, "MsBetweenDisplayChange", "DisplayedTime", "MsBetweenPresents", "FrameTime")
                if ft is None: continue
                try: ft = float(ft)
                except ValueError: continue
                if ft <= 0: continue
                frame.append(ft)
                u = column(row, "MsUntilDisplayed", "DisplayLatency")
                try:
                    if u is not None: until.append(float(u))
                except ValueError: pass
                mode = column(row, "PresentMode") or "?"; tearing = column(row, "AllowsTearing") or "?"
                modes[f"{mode}, tearing {tearing}"] = modes.get(f"{mode}, tearing {tearing}", 0) + 1
        if len(frame) < 10: print(f"{label:24s} no PushFPS frames in {path}"); continue
        med = statistics.median(frame); s = sorted(frame)
        long_share = sum(1 for f in frame if f >= 1.5 * med) / len(frame) * 100
        mode = max(modes, key=modes.get)
        to_display = f"{statistics.median(until):12.2f}" if until else f"{'n/a':>12s}"
        print(f"{label:24s} {len(frame):7d} {1000 / statistics.mean(frame):7.2f} {med:10.3f} {statistics.pstdev(frame):7.3f} {percentile(s, .99):7.3f} {long_share:13.2f}% {to_display}    {mode}")
    return 0

if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder", nargs="?", help="<PushFPS folder>/logs/bench")
    ap.add_argument("--baseline", default="original")
    ap.add_argument("--presentmon", nargs="*", default=[], metavar="LABEL=FILE.csv")
    args = ap.parse_args()
    rc = 0
    if args.folder: rc = latency_report(args.folder, args.baseline)
    if args.presentmon: rc = presentmon_report(args.presentmon) or rc
    if not args.folder and not args.presentmon: ap.print_help()
    sys.exit(rc)
