#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Named baselines per subsystem, and regression verdicts from interleaved A/B runs.

  python baseline.py save NAME EVIDENCE [--arm ARM] [--store DIR] [--replace]
  python baseline.py compare NAME EVIDENCE --candidate ARM [--reference ARM] [--store DIR]
                     [--tolerance METRIC=VALUE ...] [--markdown FILE] [--require-verdict]
  python baseline.py show NAME [--store DIR]

EVIDENCE is a folder or zip made by scripts/remoteplay/testkit/run-tests.ps1. Python standard library only.

save      stores, for one arm of the evidence, median / P95 / P99 / mean and the sample counts of every metric
          of every subsystem (input latency, network stability, decode latency, frame pacing, GPU utilisation,
          memory, image quality when the folder has a quality test), with the hardware, the stream settings and
          the present mode they were recorded under.
compare   judges a CANDIDATE arm against the REFERENCE arm of the same evidence folder. The two must have been
          run as interleaved pairs (the kit's rounds: A B, B A, A B ...): run k of the reference is paired with
          run k of the candidate, the difference is taken per pair, and the 95 % interval of the mean paired
          difference is a Student t interval over the pairs. A metric is a REGRESSION only when
            - there are at least 3 pairs, the arms were interleaved and ran under the same conditions,
            - the interval excludes zero on the worse side, and
            - the size of the difference exceeds the metric's tolerance.
          The stored baseline is the yardstick the reference is checked against: when today's reference runs
          do not reproduce the stored numbers the table says so (the rig drifted), and that is never called a
          regression of the candidate. A stored number from another day is never compared with the candidate
          directly: the console, the network and the room change from day to day.

What the words mean here: "software latency" is the built-in test's clock (press handed to the session until
the console's answer is in a presented picture). It is not input to photon; only a camera measures that, and
this tool has no camera metric. "Decode latency" values are processing time inside the program, not latency
the player feels.

Exit code: 0 no regression, 1 at least one REGRESSION, 2 cannot run (no baseline, no evidence, bad arguments),
3 with --require-verdict when no metric reached a verdict.
"""
import argparse, datetime, json, math, os, re, statistics, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report

MIN_PAIRS = 3
T_95 = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
        2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042]

def t_critical(degrees):
    if degrees < 1: return float("nan")
    if degrees <= len(T_95): return T_95[degrees - 1]
    return 2.021 if degrees <= 40 else 2.000 if degrees <= 60 else 1.980 if degrees <= 120 else 1.960

def nan(): return float("nan")
def known(value): return value is not None and value == value

# --- what is measured -----------------------------------------------------------------------------------------
def windows_of(run): return run["log"]["windows"]

def sample_column(run, *names):
    """Values of the first of these sample columns that has any, while the latency test ran (all of the run otherwise)."""
    for name in names:
        values = report.column_values(run["samples"], name, windows_of(run) or None)
        if values: return values
    return []

def frame_times(run): return run["presentmon"]["shown_values"] if run.get("presentmon") else []

def stutter_percent(values):
    if len(values) < 10: return nan()
    middle = statistics.median(values)
    return 100.0 * sum(1 for v in values if v >= 1.5 * middle) / len(values)

def p95(values): return report.percentile(values, .95) if len(values) >= 20 else nan()
def p99(values): return report.percentile(values, .99) if len(values) >= 100 else nan()
def middle(values): return statistics.median(values) if values else nan()

def run_hours(run):
    log = run["log"]
    if log["first"] is None or log["last"] is None or log["last"] - log["first"] < 30: return nan()
    return (log["last"] - log["first"]) / 3600.0

def recovery_rate(run):
    hours = run_hours(run)
    return [(len(run["log"]["keyframe_requests"]) + len(run["log"]["reconnects"])) / hours] if known(hours) else []

def dropped_percent(run): return [run["presentmon"]["dropped_percent"]] if run.get("presentmon") and run["presentmon"]["frames"] else []

def video_memory(run):
    log = run["log"]
    own = [f["vramMiB"] for t, f in log["frame_rate"] if f.get("vramMiB") is not None and (not log["windows"] or report.in_windows(t, log["windows"]))]
    return own or sample_column(run, "appGpuDedicatedMb")

class Metric:
    """samples(run) gives the raw values of one run; per_run() makes the one number of a run that the verdict is about.
    The stored median / P95 / P99 are of all raw values, or of the per-run numbers when stored == "runs"."""
    def __init__(self, key, subsystem, label, unit, samples, tolerance, relative=0.0, worse="higher", per_run=report.mean, minimum_pooled=0, digits=2, stored="samples"):
        self.key, self.subsystem, self.label, self.unit, self.samples, self.tolerance, self.relative = key, subsystem, label, unit, samples, tolerance, relative
        self.worse, self.per_run, self.minimum_pooled, self.digits, self.stored = worse, per_run, minimum_pooled, digits, stored

METRICS = [
    Metric("software_latency_mean_ms", "input latency", "software latency of all presses; verdict on the mean of each run (built-in test clock; not input to photon)", "ms", lambda r: r["log"]["presses"], 2.0, minimum_pooled=report.MIN_PRESSES, digits=1),
    Metric("software_latency_p95_ms", "input latency", "software latency of all presses; verdict on the P95 of each run (built-in test clock; not input to photon)", "ms", lambda r: r["log"]["presses"], 8.4, per_run=p95, minimum_pooled=report.MIN_PRESSES, digits=1),
    Metric("packet_loss_percent", "network stability", "packet loss, mean of the 5 s intervals", "%", lambda r: [e["lossPercent"] for e in r["log"]["health"] if e.get("lossPercent") is not None], 0.05, digits=3),
    Metric("frame_jitter_ms", "network stability", "arrival jitter of whole pictures, mean", "ms", lambda r: [e["frameJitterMs"] for e in r["log"]["health"] if e.get("frameJitterMs") is not None], 0.5),
    Metric("recovery_events_per_hour", "network stability", "keyframe requests and reconnects per hour", "/h", recovery_rate, 2.0, digits=1),
    Metric("receive_to_decoded_ms", "decode latency", "picture received until decoded; verdict on the median of each run (derived from the progress line; processing time)", "ms",
           lambda r: report.receive_to_decoded(r["log"]["progress"], windows_of(r) or None), 0.5, per_run=middle),
    Metric("pushfps_own_ms", "decode latency", "the program's own processing time during the latency test (processing time, not latency)", "ms",
           lambda r: [s["pushfps"] for s in r["log"]["summaries"] if s["pushfps"] > 0], 0.5),
    Metric("displayed_frame_time_p99_ms", "frame pacing", "time between displayed frames; verdict on the P99 of each run (PresentMon)", "ms", frame_times, 1.0, per_run=p99),
    Metric("displayed_stutter_percent", "frame pacing", "share of displayed frames that took 1.5 x the median or longer, per run (PresentMon)", "%", frame_times, 0.5, per_run=stutter_percent, stored="runs"),
    Metric("dropped_frames_percent", "frame pacing", "presented frames never displayed (PresentMon)", "%", dropped_percent, 0.2),
    Metric("gpu_percent", "GPU utilisation", "GPU load: the PushFPS process when Windows counts it, else the whole first GPU", "%", lambda r: sample_column(r, "appGpu3dPercent", "gpuPercent"), 5.0, digits=1),
    Metric("video_decode_percent", "GPU utilisation", "video decoder load: the process when Windows counts it, else the whole first GPU", "%", lambda r: sample_column(r, "appGpuDecodePercent", "gpuDecoderPercent"), 3.0, digits=1),
    Metric("process_memory_mb", "memory", "private memory of the PushFPS process", "MB", lambda r: sample_column(r, "appPrivateMb"), 50.0, relative=0.05, digits=0),
    Metric("video_memory_mb", "memory", "video memory of the PushFPS process (its own log, else Windows' counter)", "MB", video_memory, 100.0, relative=0.05, digits=0),
    Metric("quality_score", "image quality", "frame quality test score (built-in ground-truth test)", "", lambda r: [q["score"] for q in r["log"]["quality"] if q.get("score") is not None], 1.0, worse="lower", digits=1),
    Metric("generated_psnr_db", "image quality", "generated frames against withheld real frames, PSNR", "dB", lambda r: [q["psnr"] for q in r["log"]["quality"] if q.get("psnr") is not None], 0.5, worse="lower"),
]
SUBSYSTEMS = []
for _metric in METRICS:
    if _metric.subsystem not in SUBSYSTEMS: SUBSYSTEMS.append(_metric.subsystem)

# --- runs of an arm --------------------------------------------------------------------------------------------
def run_time(run):
    """Start of a run: run.json, else the time in the run's name."""
    start = report.utc(run["meta"].get("startUtc"))
    if start is not None: return start
    m = re.search(r"@(\d{8})-(\d{6})$", run["name"])
    if not m: return 0.0
    return datetime.datetime.strptime(m.group(1) + m.group(2), "%Y%m%d%H%M%S").replace(tzinfo=datetime.timezone.utc).timestamp()

def arm_runs(data, arm): return sorted([r for r in data["runs"] if r["purpose"] == "latency" and r["arm"] == arm], key=run_time)

def arms_of(data):
    names = []
    for run in sorted((r for r in data["runs"] if r["purpose"] == "latency"), key=run_time):
        if run["arm"] not in names: names.append(run["arm"])
    return names

def summarise(runs):
    """{metric key: counts, pooled median/P95/P99/mean, and the per-run values}."""
    out = {}
    for metric in METRICS:
        per_run, pooled = [], []
        for run in runs:
            values = [v for v in metric.samples(run) if known(v)]
            pooled += values
            value = metric.per_run(values) if values else nan()
            per_run.append(value if known(value) else None)
        usable = [v for v in per_run if v is not None]
        if not pooled: continue
        count = len(pooled)
        if metric.stored == "runs": pooled = usable or [nan()]
        out[metric.key] = {"runs": len(usable), "samples": count, "median": statistics.median(pooled), "p95": report.percentile(pooled, .95), "p99": report.percentile(pooled, .99),
                           "mean": report.mean(pooled), "mean_of_runs": report.mean(usable) if usable else None, "per_run": per_run}
    return out

def conditions(data, runs):
    facts = report.arm_facts(runs)
    hw = report.as_dict(data["hardware"])
    adapters = [report.as_dict(a) for a in report.as_list(hw.get("adapters"))]
    return {"streams": [list(s) for s in facts["streams"]], "present_mode": report.mode_text(facts), "variable_refresh": facts["vrr"],
            "gpu": "; ".join(str(a.get("name")) for a in adapters), "refresh_hz": [a.get("refreshHz") for a in adapters if a.get("refreshHz")],
            "cpu": str(report.as_dict(hw.get("cpu")).get("name") or ""), "builds": facts["builds"], "dry_run": bool(report.as_dict(data["kit"]).get("dryRun"))}

# --- the store -------------------------------------------------------------------------------------------------
def default_store(): return os.path.join(os.path.expanduser("~"), "slipstream-baselines")

def baseline_path(store, name):
    if not re.match(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$", name): raise ValueError("a baseline name is letters, digits, '.', '_' and '-' (at most 64)")
    return os.path.join(store, name + ".json")

def save(name, data, arm, store, replace=False):
    runs = arm_runs(data, arm)
    if not runs: raise ValueError(f"the evidence has no latency runs of arm '{arm}' (arms: {', '.join(arms_of(data)) or 'none'})")
    path = baseline_path(store, name)
    if os.path.exists(path) and not replace: raise ValueError(f"baseline '{name}' exists already ({path}); --replace overwrites it")
    record = {"format": 1, "name": name, "arm": arm, "saved": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"), "evidence": os.path.basename(os.path.normpath(data["folder"])),
              "run_count": len(runs), "conditions": conditions(data, runs), "metrics": summarise(runs),
              "labels": {m.key: {"subsystem": m.subsystem, "label": m.label, "unit": m.unit, "worse": m.worse} for m in METRICS}}
    record = finite(record)
    os.makedirs(store, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle: json.dump(record, handle, indent=1, allow_nan=False)
    return path, record

def finite(value):
    """The same structure with every number that is not finite replaced by None (JSON has no NaN)."""
    if isinstance(value, dict): return {k: finite(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)): return [finite(v) for v in value]
    if isinstance(value, float) and (value != value or abs(value) == float("inf")): return None
    return value

def load_baseline(store, name):
    with open(baseline_path(store, name), encoding="utf-8") as handle: record = json.load(handle)
    if record.get("format") != 1 or not isinstance(record.get("metrics"), dict): raise ValueError("not a baseline file of this tool")
    return record

def number_text(value, digits, unit=""): return "n/a" if not known(value) else f"{value:.{digits}f}{(' ' + unit) if unit else ''}"

def show(record):
    lines = [f"baseline '{record['name']}': arm '{record['arm']}', {record['run_count']} runs, saved {record['saved']} from {record['evidence']}",
             f"conditions: {record['conditions']}" + ("   DRY RUN: made-up data" if record["conditions"].get("dry_run") else ""), ""]
    rows = [["subsystem", "metric", "runs", "samples", "median", "P95", "P99", "mean"]]
    for metric in METRICS:
        s = record["metrics"].get(metric.key)
        if not s: rows.append([metric.subsystem, metric.label, "0", "0", "not recorded", "", "", ""]); continue
        rows.append([metric.subsystem, metric.label, str(s["runs"]), str(s["samples"])] + [number_text(s[k], metric.digits, metric.unit) for k in ("median", "p95", "p99", "mean")])
    return "\n".join(lines) + "\n" + text_table(rows)

def text_table(rows):
    widths = [max(len(str(row[i])) for row in rows) for i in range(len(rows[0]))]
    out = []
    for index, row in enumerate(rows):
        out.append("  ".join(str(cell).ljust(widths[i]) for i, cell in enumerate(row)).rstrip())
        if index == 0: out.append("  ".join("-" * w for w in widths))
    return "\n".join(out)

# --- comparing ---------------------------------------------------------------------------------------------------
def paired_interval(differences):
    """(mean, low, high) of the paired differences: Student t, 95 %."""
    n = len(differences); centre = report.mean(differences)
    if n < 2: return centre, nan(), nan()
    half = t_critical(n - 1) * statistics.stdev(differences) / math.sqrt(n)
    return centre, centre - half, centre + half

def interleaved(reference_runs, candidate_runs):
    """False when one arm was run as a block before the other: drift over the session would then look like a difference."""
    a, b = [run_time(r) for r in reference_runs], [run_time(r) for r in candidate_runs]
    if min(len(a), len(b)) < 2: return True
    return not (max(a) < min(b) or max(b) < min(a))

def judge(metric, reference_values, candidate_values, reference_pooled, candidate_pooled, tolerance=None):
    """One row of the verdict: the paired difference candidate - reference and what may be said about it."""
    pairs = [(r, c) for r, c in zip(reference_values, candidate_values) if r is not None and c is not None]
    out = {"pairs": len(pairs), "reference": report.mean([r for r, _c in pairs]), "candidate": report.mean([c for _r, c in pairs]), "difference": nan(), "low": nan(), "high": nan(), "regression": False, "decided": False}
    out["tolerance"] = max(metric.tolerance if tolerance is None else tolerance, metric.relative * abs(out["reference"]) if known(out["reference"]) and tolerance is None else 0.0)
    if not pairs: out["verdict"] = "no verdict: not recorded in both arms"; return out
    out["difference"], out["low"], out["high"] = paired_interval([c - r for r, c in pairs])
    if len(pairs) < MIN_PAIRS: out["verdict"] = f"no verdict: {len(pairs)} pair(s), {MIN_PAIRS} needed"; return out
    if metric.minimum_pooled and min(reference_pooled, candidate_pooled) < metric.minimum_pooled:
        out["verdict"] = f"no verdict: fewer than {metric.minimum_pooled} presses in an arm ({reference_pooled} and {candidate_pooled})"; return out
    out["decided"] = True
    worse = out["difference"] > 0 if metric.worse == "higher" else out["difference"] < 0
    if out["low"] <= 0 <= out["high"]: out["verdict"] = "not resolved: the interval includes zero"
    elif abs(out["difference"]) <= out["tolerance"]: out["verdict"] = "within tolerance (a real difference, smaller than the tolerance)"
    elif worse: out["verdict"] = "REGRESSION"; out["regression"] = True
    else: out["verdict"] = "improved"
    return out

def compare(record, data, candidate, reference=None, tolerances=None):
    """(rows for the verdict table, notes, regression found, any metric decided)."""
    tolerances = tolerances or {}
    reference = reference or record["arm"]
    notes = []
    reference_runs, candidate_runs = arm_runs(data, reference), arm_runs(data, candidate)
    if not candidate_runs: raise ValueError(f"the evidence has no latency runs of the candidate arm '{candidate}' (arms: {', '.join(arms_of(data)) or 'none'})")
    blocked = ""
    if reference == candidate: blocked = "no verdict: the candidate is the reference arm"
    elif not reference_runs:
        blocked = f"no verdict: the evidence has no runs of the reference arm '{reference}'"
        notes.append(f"No interleaved runs of '{reference}' in this evidence: a difference to numbers stored on another day cannot be told from drift of the console, the network or the PC. Run the kit with both arms.")
    else:
        why, unchecked = report.comparability(candidate, report.arm_facts(candidate_runs), reference, report.arm_facts(reference_runs))
        if why: blocked = why
        elif not interleaved(reference_runs, candidate_runs): blocked = "no verdict: the arms were not interleaved (one was run as a block before the other)"
        if unchecked: notes.append("Not checked, because one arm did not record it: " + ", ".join(unchecked) + ".")
    if len(reference_runs) != len(candidate_runs) and reference_runs:
        notes.append(f"'{reference}' has {len(reference_runs)} runs and '{candidate}' {len(candidate_runs)}: only the first {min(len(reference_runs), len(candidate_runs))} of each are paired.")
    today_reference, today_candidate = summarise(reference_runs), summarise(candidate_runs)
    stored_conditions, now_conditions = record["conditions"], conditions(data, reference_runs or candidate_runs)
    changed = [key for key in ("streams", "present_mode", "gpu", "refresh_hz") if stored_conditions.get(key) and now_conditions.get(key) and stored_conditions[key] != now_conditions[key]]
    if changed: notes.append("The stored baseline was recorded under other conditions (" + ", ".join(f"{k}: {stored_conditions[k]} then, {now_conditions[k]} now" for k in changed) + "): its numbers are shown but say nothing about today's runs.")
    rows, regression, decided = [], False, False
    for metric in METRICS:
        stored, ref, cand = record["metrics"].get(metric.key), today_reference.get(metric.key), today_candidate.get(metric.key)
        if not stored and not ref and not cand: continue
        def text(value): return number_text(value, metric.digits, metric.unit)
        stored_text = f"{text(stored['median'])} / {text(stored['p95'])} / {text(stored['p99'])} (n={stored['samples']}, {stored['runs']} runs)" if stored else "not stored"
        result = judge(metric, ref["per_run"] if ref else [], cand["per_run"] if cand else [], ref["samples"] if ref else 0, cand["samples"] if cand else 0, tolerances.get(metric.key))
        if blocked: result.update({"verdict": blocked, "regression": False, "decided": False})
        drift = ""
        if stored and ref and stored.get("mean_of_runs") is not None and ref.get("mean_of_runs") is not None and not changed:
            moved = ref["mean_of_runs"] - stored["mean_of_runs"]
            drift = f"{moved:+.{metric.digits}f}" + (" DRIFTED" if abs(moved) > 2 * result["tolerance"] else "")
        regression = regression or result["regression"]; decided = decided or result["decided"]
        show_numbers = not blocked or blocked.startswith("no verdict: the arms were not interleaved")
        rows.append([metric.subsystem, metric.label, stored_text, text(ref["mean_of_runs"]) if ref and ref["mean_of_runs"] is not None else "n/a", drift or "n/a",
                     text(cand["mean_of_runs"]) if cand and cand["mean_of_runs"] is not None else "n/a",
                     f"{result['difference']:+.{metric.digits}f}" if known(result["difference"]) and show_numbers else "n/a",
                     f"[{result['low']:+.{metric.digits}f}, {result['high']:+.{metric.digits}f}]" if known(result["low"]) and show_numbers else "n/a",
                     str(result["pairs"]), f"{result['tolerance']:.{metric.digits}f}" + (" (lower is worse)" if metric.worse == "lower" else ""), result["verdict"]])
    if any("DRIFTED" in row[4] for row in rows):
        notes.append(f"Today's runs of '{reference}' do not reproduce the stored baseline in the rows marked DRIFTED (more than twice the tolerance away). That is the rig or the day, not the candidate; save a new baseline if the change is meant.")
    if report.as_dict(data["kit"]).get("dryRun") or stored_conditions.get("dry_run"): notes.append("DRY RUN data (made up by run-tests.ps1 -DryRun) is involved: not a measurement.")
    notes.append("Software latency is the built-in test's clock, not input to photon. Decode latency rows are processing time. Nothing here was measured with a camera.")
    header = ["subsystem", "metric", "stored baseline: median / P95 / P99", f"reference today ({reference})", "reference vs stored", f"candidate ({candidate})", "difference (candidate - reference)", "95 % interval",
              "pairs", "tolerance", "verdict"]
    return [header] + rows, notes, regression, decided

def markdown_table(rows):
    def cell(text): return str(text).replace("|", "/")
    return "\n".join(["| " + " | ".join(cell(c) for c in rows[0]) + " |", "|" + "---|" * len(rows[0])] + ["| " + " | ".join(cell(c) for c in row) + " |" for row in rows[1:]])

# --- command line -----------------------------------------------------------------------------------------------
def open_evidence(path, temporary):
    if os.path.isfile(path): path = report.unpack(path, temporary)
    if not os.path.isdir(path): raise ValueError(f"not a folder or zip: {path}")
    data = report.load(path)
    if not data["runs"]: raise ValueError(f"no runs in {path}")
    return data

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command")
    for name in ("save", "compare", "show"):
        p = sub.add_parser(name)
        p.add_argument("name", help="name of the baseline")
        p.add_argument("--store", default=default_store(), help="folder the baselines are kept in (default: slipstream-baselines in your home folder)")
        if name != "show": p.add_argument("evidence", help="evidence folder or zip made by run-tests.ps1")
        if name == "save":
            p.add_argument("--arm", help="the arm to store (default: the arm table's baseline arm, mod-legacy, when the evidence has it)")
            p.add_argument("--replace", action="store_true", help="overwrite a baseline of the same name")
        if name == "compare":
            p.add_argument("--candidate", required=True, help="the arm to judge")
            p.add_argument("--reference", help="the arm it is paired with (default: the arm the baseline was saved from)")
            p.add_argument("--tolerance", action="append", default=[], metavar="METRIC=VALUE", help="replace a metric's tolerance, e.g. software_latency_mean_ms=1.0")
            p.add_argument("--markdown", metavar="FILE", help="also write the table and the notes as Markdown")
            p.add_argument("--require-verdict", action="store_true", help="exit 3 when no metric reached a verdict")
    args = ap.parse_args(argv)
    if not args.command: ap.print_help(); return 2
    try:
        if args.command == "show":
            print(show(load_baseline(args.store, args.name))); return 0
        with tempfile.TemporaryDirectory() as temporary:
            data = open_evidence(args.evidence, temporary)
            if args.command == "save":
                arm = args.arm or ("mod-legacy" if "mod-legacy" in arms_of(data) else None)
                if not arm: raise ValueError(f"--arm is needed (arms in the evidence: {', '.join(arms_of(data)) or 'none'})")
                path, record = save(args.name, data, arm, args.store, args.replace)
                print(show(record)); print(f"\nsaved: {path}")
                presses = record["metrics"].get("software_latency_mean_ms", {}).get("samples", 0)
                if record["run_count"] < MIN_PAIRS or presses < report.MIN_PRESSES:
                    print(f"note: {record['run_count']} runs and {presses} presses; a baseline wants at least {MIN_PAIRS} runs and {report.MIN_PRESSES} presses.")
                return 0
            tolerances = {}
            for item in args.tolerance:
                key, _, value = item.partition("=")
                if key not in {m.key for m in METRICS} or report.number(value) is None or report.number(value) < 0: raise ValueError(f"--tolerance {item}: use METRIC=VALUE with one of " + ", ".join(m.key for m in METRICS))
                tolerances[key] = report.number(value)
            record = load_baseline(args.store, args.name)
            rows, notes, regression, decided = compare(record, data, args.candidate, args.reference, tolerances)
            print(f"baseline '{record['name']}' (arm '{record['arm']}', saved {record['saved']}); evidence {os.path.basename(os.path.normpath(args.evidence))}\n")
            print(text_table(rows)); print()
            for note in notes: print("note:", note)
            print("\nRESULT: " + ("REGRESSION" if regression else "no regression" if decided else "no verdict"))
            if args.markdown:
                with open(args.markdown, "w", encoding="utf-8") as handle:
                    handle.write(f"# Regression check against baseline '{record['name']}'\n\n" + markdown_table(rows) + "\n\n" + "\n".join(f"- {n}" for n in notes) + "\n\nResult: " + ("REGRESSION" if regression else "no regression" if decided else "no verdict") + "\n")
            if regression: return 1
            return 3 if (args.require_verdict and not decided) else 0
    except (OSError, ValueError) as problem:
        print(f"baseline.py: {problem}", file=sys.stderr); return 2

if __name__ == "__main__":
    sys.exit(main())
