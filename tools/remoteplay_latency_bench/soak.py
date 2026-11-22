#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Trends over a long run (soak) of the hardware test kit: fits a line to each value, flags growth.

  python soak.py <evidence folder or .zip>        exit 1 when a soak run fails, 3 when the evidence has none

report.py shows the same tables. Python standard library only.

For every value the samples are reduced to one median per minute (a spike is not a trend), the first five
minutes are left out (caches and pools fill), and a Theil-Sen line (the median of the slopes between all pairs of
points) gives the change per hour. A value is flagged only when all of this holds:
  - the run is long enough to judge (30 minutes of points after the warm-up, or 6 points for values that exist
    only once per PresentMon window),
  - the change per hour exceeds the value's threshold (an absolute figure or a share of the starting level,
    whichever is larger),
  - each third of the run is worse than the one before, and both steps carry at least a fifth of the rise:
    a level that rose early and then stayed is reported as such, not as growth.
"Latency" rows here are processing times inside the program (picture received until decoded, present until
scanout by the program's own statistics, arrival until present) and the built-in test's software clock when the
test was run during the soak. None of them is input to photon.
"""
import csv, os, statistics, sys

WARM_UP_SECONDS = 300.0
MINIMUM_MINUTES = 30.0
MINIMUM_WINDOW_POINTS = 6

def mean(values): return statistics.fmean(values) if values else float("nan")
def known(value): return value is not None and value == value

def percentile(values, p):
    if not values: return float("nan")
    s = sorted(values); k = (len(s) - 1) * p; lo = int(k); hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)

def per_minute(points, start, warm_up=WARM_UP_SECONDS):
    """[(hours since start, median of that minute)] for the minutes after the warm-up."""
    buckets = {}
    for t, value in points:
        if not known(value) or t is None or t - start < warm_up: continue
        buckets.setdefault(int((t - start) // 60), []).append(value)
    return [((minute + 0.5) / 60.0, statistics.median(values)) for minute, values in sorted(buckets.items())]

def theil_sen(points):
    """Median of the slopes between pairs of points (every pair up to 300 points, evenly thinned beyond that)."""
    if len(points) > 300:
        step = len(points) / 300.0
        points = [points[int(i * step)] for i in range(300)]
    slopes = [(y2 - y1) / (x2 - x1) for i, (x1, y1) in enumerate(points) for x2, y2 in points[i + 1:] if x2 > x1]
    if not slopes: return float("nan"), float("nan")
    slope = statistics.median(slopes)
    return slope, statistics.median([y - slope * x for x, y in points])

class Rule:
    def __init__(self, key, label, unit, source, per_hour, relative=0.0, worse="up", windows=False, digits=1):
        self.key, self.label, self.unit, self.source, self.per_hour, self.relative, self.worse, self.windows, self.digits = key, label, unit, source, per_hour, relative, worse, windows, digits

RULES = [
    Rule("working_set", "Working set of the process", "MB", "samples.csv appWorkingSetMb", 20.0, 0.02),
    Rule("private", "Private memory of the process", "MB", "samples.csv appPrivateMb", 20.0, 0.02),
    Rule("gpu_memory", "Video memory of the process (Windows counter)", "MB", "samples.csv appGpuDedicatedMb", 20.0, 0.02),
    Rule("own_vram", "Video memory of the process (its own log)", "MiB", "[frame-rate] vramMiB", 20.0, 0.02),
    Rule("handles", "Handles", "", "samples.csv appHandles", 30.0, 0.02, digits=0),
    Rule("threads", "Threads", "", "samples.csv appThreads", 2.0, digits=0),
    Rule("to_decoded", "Picture received until decoded (processing time)", "ms", "[remoteplay-progress] ages", 0.5, digits=2),
    Rule("to_scanout", "Present until scanout, P50 (the program's own statistics)", "ms", "[display-latency]", 0.5, digits=2),
    Rule("arrival_to_present", "Arrival until present (processing time)", "ms", "[remoteplay-cadence] arrivalToPresentMs", 0.5, digits=2),
    Rule("software_latency", "Software latency of the built-in test, mean per test (not input to photon)", "ms", "[push-latency] done", 2.0, windows=True),
    Rule("present_fps", "Frames presented per second", "", "[frame-rate] presentSubmitFps", 0.5, 0.01, worse="down", digits=2),
    Rule("decode_fps", "Pictures decoded per second", "", "[remoteplay-progress] decodeFps", 0.5, 0.01, worse="down", digits=2),
    Rule("frame_p99", "Time between displayed frames, P99 per PresentMon window", "ms", "PresentMon windows", 1.0, windows=True, digits=2),
    Rule("dropped", "Presented frames never displayed, per PresentMon window", "%", "PresentMon windows", 0.2, windows=True, digits=2),
    Rule("loss", "Packet loss per 5 s interval", "%", "[remoteplay-health] lossPercent", 0.05, digits=3),
    Rule("jitter", "Arrival jitter of whole pictures", "ms", "[remoteplay-health] frameJitterMs", 0.5, digits=2),
]

def trend(points, rule, start, minimum_minutes=MINIMUM_MINUTES, warm_up=WARM_UP_SECONDS):
    """What one value did over the run: level at the start and the end of the fitted line, change per hour, verdict."""
    if rule.windows: series = sorted(((t - start) / 3600.0, v) for t, v in points if known(v) and t is not None)
    else: series = per_minute(points, start, warm_up)
    out = {"points": len(series), "start": float("nan"), "end": float("nan"), "per_hour": float("nan"), "flag": False}
    if not series: out["verdict"] = "not recorded"; return out
    span = series[-1][0] - series[0][0]
    out["start"], out["end"] = series[0][1], series[-1][1]
    too_short = len(series) < MINIMUM_WINDOW_POINTS or span <= 0 if rule.windows else (span * 60.0 < minimum_minutes or len(series) < 10)
    if too_short:
        need = f"{MINIMUM_WINDOW_POINTS} points" if rule.windows else f"{minimum_minutes:.0f} minutes after the first {warm_up / 60:.0f}"
        out["verdict"] = f"too short to judge ({need} needed)"; return out
    slope, intercept = theil_sen(series)
    out["per_hour"], out["start"], out["end"] = slope, intercept + slope * series[0][0], intercept + slope * series[-1][0]
    third = max(1, len(series) // 3)
    first, last = mean([v for _x, v in series[:third]]), mean([v for _x, v in series[-third:]])
    centre = mean([v for _x, v in series[third:len(series) - third]] or [v for _x, v in series])
    sign = 1.0 if rule.worse == "up" else -1.0
    threshold = max(rule.per_hour, rule.relative * abs(first))
    rise, step1, step2 = sign * (last - first), sign * (centre - first), sign * (last - centre)
    out["threshold"] = threshold
    early = rise > 0 and step2 <= 0.2 * rise
    if sign * slope <= threshold or sign * slope * span < threshold / 2:
        # The fitted line is flat. A level that moved by more than the threshold would allow over the run, all of it
        # early, is still worth a word: it is what a cache or a pool filling looks like.
        out["verdict"] = "changed early, then level" if early and rise > threshold * span else "steady"
    elif rise > 0 and step1 > 0.2 * rise and step2 > 0.2 * rise:
        out["verdict"] = "GROWTH" if rule.worse == "up" else "DECLINE"; out["flag"] = True
    elif early: out["verdict"] = "changed early, then level"
    else: out["verdict"] = "uneven: no steady trend"
    return out

def read_windows(folder):
    """[(file path, start as text)] of the PresentMon captures of one soak run (presentmon-windows.csv)."""
    rows = []
    try:
        with open(os.path.join(folder, "presentmon-windows.csv"), newline="", encoding="utf-8-sig", errors="replace") as handle:
            for row in csv.DictReader(handle):
                name = os.path.basename((row.get("file") or "").strip())
                if name: rows.append((os.path.join(folder, name), (row.get("startUtc") or "").strip()))
    except OSError: pass
    return rows

def series_of(run, window_stats):
    """{rule key: [(time, value)]} for one run. window_stats: [(start time, PresentMon statistics)]."""
    log, samples = run["log"], run["samples"]
    def column(name): return [(t, row.get(name)) for t, row in samples]
    progress = log["progress"]
    return {
        "working_set": column("appWorkingSetMb"), "private": column("appPrivateMb"), "gpu_memory": column("appGpuDedicatedMb"), "handles": column("appHandles"), "threads": column("appThreads"),
        "own_vram": [(t, f.get("vramMiB")) for t, f in log["frame_rate"]], "present_fps": [(t, f.get("presentSubmitFps")) for t, f in log["frame_rate"]],
        "to_decoded": [(t, f["receiveAgeMs"] - f["decodeAgeMs"]) for t, f in progress if f.get("receiveAgeMs") is not None and f.get("decodeAgeMs") is not None and 0 <= f["receiveAgeMs"] - f["decodeAgeMs"] < 1000 and (f.get("decodeFps") or 0) > 0],
        "decode_fps": [(t, f.get("decodeFps")) for t, f in progress],
        "to_scanout": [(t, f.get("presentToScanoutP50Ms")) for t, f in log["display_latency"]],
        "arrival_to_present": [(c.get("t"), c.get("arrivalToPresentMs")) for c in log["cadence"] if (c.get("arrivalToPresentMs") or 0) > 0],
        "software_latency": [(s.get("when"), s["mean"]) for s in log["summaries"] if s.get("when") is not None and s["answered"] > 0],
        "frame_p99": [(t, s["shown_p99"]) for t, s in window_stats if s], "dropped": [(t, s["dropped_percent"]) for t, s in window_stats if s],
        "loss": [(e.get("t"), e.get("lossPercent")) for e in log["health"]], "jitter": [(e.get("t"), e.get("frameJitterMs")) for e in log["health"]],
    }

def analyse(run, window_stats=(), minimum_minutes=MINIMUM_MINUTES, warm_up=WARM_UP_SECONDS):
    """Trends, counts and the verdict of one soak run."""
    log, samples, meta = run["log"], run["samples"], run["meta"]
    times = [t for t, _row in samples] or [t for t in (log["first"], log["last"]) if t is not None]
    start = min(times) if times else 0.0
    minutes = (max(times) - start) / 60.0 if len(times) > 1 else float("nan")
    series = series_of(run, list(window_stats))
    trends = {rule.key: trend(series[rule.key], rule, start, minimum_minutes, warm_up) for rule in RULES}
    health = log["health"]
    counts = {"reconnects": len(log["reconnects"]), "keyframe_requests": len(log["keyframe_requests"]), "recovery_gave_up": log["recovery_stopped"],
              "decoder_resets": sum(int(e.get("decoderResets") or 0) for e in health), "network_frame_drops": sum(int(e.get("frameDrops") or 0) for e in health),
              "generated_expired": log["flow"]["generatedExpiredAfterEval"], "cancelled_before_present": log["flow"]["cancelledBeforePresent"], "unread_overwritten": log["flow"]["mailboxOverwritten"],
              "crashes": len(log["crashes"]), "gpu_failures": len(log["gpu_failures"]), "engine_hangs": len(log["hangs"]), "error_lines": log["errors"], "log_lines_dropped": log["log_dropped"],
              "restarts": int(meta.get("restarts") or 0)}
    planned = float(meta.get("limitSeconds") or 0) / 60.0
    stop = str(meta.get("stopReason") or ("time" if meta.get("limitReached") else "unknown"))
    problems = []
    for key, label in (("crashes", "crash"), ("gpu_failures", "GPU failure"), ("engine_hangs", "engine hang"), ("recovery_gave_up", "recovery gave up")):
        if counts[key]: problems.append(f"{counts[key]} {label} line(s)")
    if meta.get("forcedStop"): problems.append("the program did not close when asked and was ended by the kit")
    if stop.startswith("closed") and planned and known(minutes) and minutes < 0.9 * planned: problems.append(f"the program ended by itself or was closed after {minutes:.0f} of {planned:.0f} minutes")
    flagged = [rule.label for rule in RULES if trends[rule.key]["flag"]]
    if flagged: problems.append("trend flagged: " + "; ".join(flagged))
    short = [rule.label for rule in RULES if trends[rule.key]["verdict"].startswith("too short")]
    if problems: verdict = "FAIL: " + "; ".join(problems)
    elif stop.startswith("kill-switch"): verdict = f"INCOMPLETE: stopped by the kill-switch after {minutes:.0f} of {planned:.0f} minutes; nothing flagged in that time"
    elif short and len(short) >= len([r for r in RULES if trends[r.key]["points"]]): verdict = "INCOMPLETE: too short to judge any trend"
    else: verdict = "PASS: no growth flagged, no crash, GPU failure, hang or abandoned recovery"
    return {"minutes": minutes, "planned": planned, "stop": stop, "trends": trends, "counts": counts, "verdict": verdict, "failed": bool(problems), "start": start}

def fmt(value, digits, unit=""):
    if not known(value): return "n/a"
    return f"{value:.{digits}f}" + (f" {unit}" if unit else "")

def trend_rows(arm, result):
    rows = []
    for rule in RULES:
        t = result["trends"][rule.key]
        if t["verdict"] == "not recorded": continue
        rows.append([arm, rule.label, rule.source, str(t["points"]), fmt(t["start"], rule.digits, rule.unit), fmt(t["end"], rule.digits, rule.unit),
                     (f"{t['per_hour']:+.{rule.digits + 1}f}" if known(t["per_hour"]) else "n/a"), fmt(t.get("threshold"), rule.digits + 1), t["verdict"]])
    return rows

TREND_HEADER = ["Arm", "Value", "Source", "Points", "Start of the fitted line", "End", "Change per hour", "Flagged beyond (per hour)", "Verdict"]
COUNT_HEADER = ["Arm", "Minutes run", "Minutes planned", "Ended by", "Reconnects", "Keyframe requests", "Recovery gave up", "Decoder resets", "Frames dropped (network)", "Generated frames expired",
                "Cancelled before present", "Unread pictures overwritten", "Crash lines", "GPU failure lines", "Engine hang lines", "Error lines", "Restarts", "Result"]

def count_row(arm, result):
    c = result["counts"]
    return [arm, fmt(result["minutes"], 0), fmt(result["planned"], 0) if result["planned"] else "n/a", result["stop"]] + [str(c[k]) for k in (
        "reconnects", "keyframe_requests", "recovery_gave_up", "decoder_resets", "network_frame_drops", "generated_expired", "cancelled_before_present", "unread_overwritten", "crashes", "gpu_failures",
        "engine_hangs", "error_lines", "restarts")] + [result["verdict"]]

def main(argv=None):
    import tempfile
    here = os.path.dirname(os.path.abspath(__file__))
    if here not in sys.path: sys.path.insert(0, here)
    import report
    args = sys.argv[1:] if argv is None else argv
    if len(args) != 1 or args[0].startswith("-"): print(__doc__); return 2
    with tempfile.TemporaryDirectory() as temporary:
        folder = report.unpack(args[0], temporary) if os.path.isfile(args[0]) else args[0]
        if not os.path.isdir(folder): print(f"not a folder or zip: {args[0]}"); return 2
        runs = [r for r in report.load(folder)["runs"] if r["purpose"] == "soak"]
        if not runs: print("no soak run in this evidence"); return 3
        failed = False
        for run in runs:
            result = report.soak_result(run)
            failed = failed or result["failed"]
            print(f"soak of '{run['arm']}': {result['verdict']}")
            for row in [COUNT_HEADER[1:17], count_row(run["arm"], result)[1:17]]: print("  " + " | ".join(row))
            for row in [TREND_HEADER[1:]] + [r[1:] for r in trend_rows(run["arm"], result)]: print("  " + " | ".join(row))
        return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
