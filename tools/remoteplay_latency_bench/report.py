#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Report of one evidence folder (or zip) made by scripts/remoteplay/testkit/run-tests.ps1.

  python report.py <evidence folder or .zip> [--out DIR] [--baseline ARM] [--strict]

Writes report.md and report.html (into the folder, or into --out). Python standard library only.

What the report says, and what it refuses to say:
  - Software latency is the built-in test's clock: the press handed to the session until the console's
    answer is in a presented picture. Per arm: presses, median, P95, P99, mean, and the difference of
    MEANS to the arm's baseline with a bootstrap 95 % interval. A difference is called lower or higher
    only when the interval excludes zero AND both arms have at least 120 presses.
  - The original build logs one summary per test and no single presses. Such an arm gets its pooled mean
    only, and its comparison is a bootstrap over whole test runs (labelled "by run").
  - Displayed frame times, dropped frames and the present mode come from PresentMon, limited to the time
    the latency test ran when the log gives that time.
  - Pictures decoded from the console (real) and displayed frames are reported side by side; PresentMon
    cannot tell PushFPS's generated frames from real ones.
  - Camera measurements come from camera.csv, which the user fills in. Only that table is input to photon.
  - Every evidence file the kit wrote is checked again for addresses and private words (--strict: exit 2).
Nothing here is a measurement by itself: the numbers are only as good as the runs in the folder.
"""
import argparse, csv, datetime, glob, html, json, os, random, re, statistics, sys, tempfile, zipfile

MIN_PRESSES = 120          # software latency: presses per arm before any verdict
MIN_CAMERA_PRESSES = 30    # camera: 4.2 ms per frame at 240 fps (pushfps-latency-measurement skill)
BOOTSTRAP_ROUNDS = 4000

# --- redaction: the same rules as Protect-Text / Select-SafeLines in run-tests.ps1 -------------------
ALLOWED = re.compile(r"\[(push-latency|remoteplay(-[a-z0-9-]+)?)\]")
REFUSED_WORDS = r"registration|regist_?key|rp_?key|account|psn|token|secret|passw|ssid|morning|nickname"
REFUSED = re.compile(r"\[remoteplay-discovery\]|" + REFUSED_WORDS, re.I)
PRIVATE_WORD = re.compile(REFUSED_WORDS, re.I)
_MASKS = [
    (re.compile(r"\b[0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}\b"), "[mac]"),
    (re.compile(r"\b10\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"), "10.x.x.x"),
    (re.compile(r"\b192\.168\.\d{1,3}\.\d{1,3}\b"), "192.168.x.x"),
    (re.compile(r"\b172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}\b"), r"172.\1.x.x"),
    (re.compile(r"\b169\.254\.\d{1,3}\.\d{1,3}\b"), "169.254.x.x"),
    (re.compile(r"\b127\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"), "127.x.x.x"),
    (re.compile(r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"), "x.x.x.x"),
    (re.compile(r"(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){3,7}[0-9A-Fa-f]{1,4}(?![0-9A-Za-z:])"), "[ipv6]"),
    (re.compile(r"(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){1,6}:(?:[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4}){0,5})?(?![0-9A-Za-z:])"), "[ipv6]"),
    (re.compile(r"(?:0[xX])?[0-9A-Fa-f]{16,}"), "[hex]"),
    (re.compile(r"\b(host|name)=(?!\[name\]|(?:\d{1,3}|x)(?:\.(?:\d{1,3}|x)){3}(?![0-9A-Za-z.]))[^\s,;)]+", re.I), r"\1=[name]"),
]

def redact_line(line):
    for pattern, replacement in _MASKS: line = pattern.sub(replacement, line)
    return line

def safe_lines(lines):
    """The lines the kit may copy, masked."""
    return [redact_line(l) for l in lines if l and ALLOWED.search(l) and not REFUSED.search(l)]

def leak_findings(folder):
    """(file, line number, reason) for every line in a kit-written text file that the rules would change or refuse."""
    found = []
    for root, _dirs, files in os.walk(folder):
        for name in sorted(files):
            if not (name.endswith((".json", ".txt")) or name in ("markers.csv", "camera.csv")) or name.startswith("report."): continue
            path = os.path.join(root, name)
            for number, line in enumerate(read_lines(path), 1):
                if PRIVATE_WORD.search(line): found.append((os.path.relpath(path, folder), number, "private word"))
                elif redact_line(line) != line: found.append((os.path.relpath(path, folder), number, "address, long hex value or name"))
    return found

# --- small helpers -----------------------------------------------------------------------------------
def read_lines(path):
    with open(path, encoding="utf-8-sig", errors="replace") as handle: return handle.read().splitlines()

def read_json(path):
    try:
        with open(path, encoding="utf-8-sig", errors="replace") as handle: return json.load(handle)
    except (OSError, ValueError): return {}

def number(text):
    try:
        value = float(text)
    except (TypeError, ValueError): return None
    return value if value == value and abs(value) != float("inf") else None

def percentile(values, p):
    if not values: return float("nan")
    s = sorted(values); k = (len(s) - 1) * p; lo = int(k); hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)

def mean(values): return statistics.fmean(values) if values else float("nan")

def utc(text):
    try: return datetime.datetime.strptime(text, "%Y-%m-%dT%H:%M:%S.%fZ").replace(tzinfo=datetime.timezone.utc).timestamp()
    except (TypeError, ValueError): return None

def fmt(value, digits=1, unit=""):
    if value is None or value != value: return "n/a"
    return f"{value:.{digits}f}{unit}"

def in_windows(t, windows): return any(a <= t <= b for a, b in windows)

def as_list(value):
    """JSON written by PowerShell can hold one object where a list was meant."""
    if isinstance(value, list): return value
    return [] if value in (None, "") else [value]

def as_dict(value): return value if isinstance(value, dict) else {}

# --- the log of one run --------------------------------------------------------------------------------
LINE = re.compile(r"^(\S+Z) t=\d+ \[(\w+)\s*\] \[([^\]]+)\] (.*)$")
FIELD = re.compile(r"(\w+)=(-?[\d.]+)")

def parse_log(lines):
    """Everything the report uses from the whitelisted log lines of one run."""
    out = {"presses": [], "summaries": [], "no_answer": 0, "windows": [], "progress": [], "keyframe_requests": [], "reconnects": [],
           "recovery_stopped": 0, "errors": 0, "warnings": 0, "lost_packets_max": 0, "session_start": None, "timing": "", "settings": ""}
    open_window = None
    for raw in lines:
        m = LINE.match(raw)
        when, level, component, text = (utc(m.group(1)), m.group(2), m.group(3), m.group(4)) if m else (None, "", "", raw)
        if not m:
            m2 = re.search(r"\[([a-z-]+)\] (.*)$", raw)
            if m2: component, text = m2.group(1), m2.group(2)
        if level == "ERROR": out["errors"] += 1
        if level == "WARN": out["warnings"] += 1
        if component == "push-latency":
            if text.startswith("remote play latency test started"): open_window = when
            m3 = re.match(r"trials answerMs=\[([^\]]*)\]", text)
            if m3: out["presses"] += [v for v in (number(x) for x in m3.group(1).split(",")) if v is not None]
            m3 = re.match(r"done answered=(\d+)/(\d+) meanMs=([\d.]+) medianMs=([\d.]+).*pushfpsMs=([\d.]+)", text)
            if m3:
                out["summaries"].append({"answered": int(m3.group(1)), "trials": int(m3.group(2)), "mean": float(m3.group(3)),
                                         "median": float(m3.group(4)), "pushfps": float(m3.group(5))})
                if open_window is not None and when is not None: out["windows"].append((open_window, when))
                open_window = None
            if "no answer seen" in text: out["no_answer"] += 1
            if text.startswith("cancelled"): open_window = None
        elif component == "remoteplay-progress":
            fields = {k: number(v) for k, v in FIELD.findall(text)}
            if when is not None and fields.get("decodeFps") is not None: out["progress"].append((when, fields))
        elif component == "remoteplay-transport":
            lost = dict(FIELD.findall(text)).get("packetWindowLost")
            if number(lost): out["lost_packets_max"] = max(out["lost_packets_max"], int(number(lost)))
        elif component == "remoteplay-recovery":
            if text.startswith("request keyframe"): out["keyframe_requests"].append(when)
            if text.startswith("reconnected"): out["reconnects"].append(when)
            if "recovery stopped" in text: out["recovery_stopped"] += 1
        elif component == "remoteplay" and text.startswith("session started") and out["session_start"] is None: out["session_start"] = when
        elif component == "remoteplay-timing" and text.startswith("ownerWait"): out["timing"] = text.split(" (")[0]
        elif component == "remoteplay-settings": out["settings"] = text[:200]
    return out

# --- PresentMon ----------------------------------------------------------------------------------------
def pick(row, *names):
    """First of these columns that has a value. Rows are keyed in lower case: 1.x files write msBetweenPresents."""
    for n in names:
        value = row.get(n.lower())
        if value not in ("", None): return value
    return None

def presentmon_stats(path, windows=None, process="pushfps"):
    """Frame statistics of the busiest swap chain. windows: (from, to) in seconds of the capture's own clock."""
    chains = {}
    try:
        handle = open(path, newline="", encoding="utf-8-sig", errors="replace")
    except OSError: return None
    with handle:
        for raw in csv.DictReader(handle):
            row = {(k or "").strip().lower(): v for k, v in raw.items()}
            if process not in (pick(row, "Application", "ProcessName") or "").lower(): continue
            t = number(pick(row, "CPUStartTime", "TimeInSeconds"))
            chains.setdefault(pick(row, "SwapChainAddress") or "?", []).append((t, row))
    if not chains: return None
    result = {"swap_chains": len(chains), "window": "whole capture"}
    rows = max(chains.values(), key=len)
    if windows:
        inside = [(t, r) for t, r in rows if t is not None and in_windows(t, windows)]
        if len(inside) >= 10: rows, result["window"] = inside, "while the latency test ran"
        else: result["window"] = "whole capture (the test time was not found in it)"
    shown, present, until, modes, tearing, types = [], [], [], {}, 0, {}
    dropped = video = frame_total = 0
    for _t, r in rows:
        display = pick(r, "MsBetweenDisplayChange", "DisplayedTime")
        if "dropped" in r: is_dropped = str(r["dropped"]).strip() == "1"
        elif "displayedtime" in r: is_dropped = number(r["displayedtime"]) is None
        else: is_dropped = not number(display)
        if is_dropped: dropped += 1
        else:
            value = number(display)
            if value and value > 0: shown.append(value)
            u = number(pick(r, "MsUntilDisplayed", "DisplayLatency"))
            if u is not None: until.append(u)
        p = number(pick(r, "MsBetweenPresents", "FrameTime"))
        if p and p > 0: present.append(p); frame_total += p
        v = number(pick(r, "VideoBusy", "MsVideoBusy"))
        if v: video += v
        mode = pick(r, "PresentMode") or "?"
        modes[mode] = modes.get(mode, 0) + 1
        if str(pick(r, "AllowsTearing") or "").strip() == "1": tearing += 1
        if "frametype" in r: types[r["frametype"] or "?"] = types.get(r["frametype"] or "?", 0) + 1
    total = len(rows)
    top = max(modes, key=modes.get)
    result.update({
        "frames": total, "dropped": dropped, "dropped_percent": 100.0 * dropped / total if total else float("nan"),
        "shown_median": statistics.median(shown) if shown else float("nan"), "shown_p95": percentile(shown, .95), "shown_p99": percentile(shown, .99),
        "shown_max": max(shown) if shown else float("nan"),
        "shown_fps": 1000.0 * len(shown) / sum(shown) if shown else float("nan"),
        "present_median": statistics.median(present) if present else float("nan"),
        "until_displayed_median": statistics.median(until) if until else float("nan"),
        "mode": top, "mode_percent": 100.0 * modes[top] / total, "tearing_percent": 100.0 * tearing / total,
        "composed": top.startswith("Composed"), "video_busy_percent": 100.0 * video / frame_total if frame_total and video else float("nan"),
        "frame_types": types, "modes": modes, "shown_values": shown, "rows": rows})
    return result

# --- samples -------------------------------------------------------------------------------------------
def read_samples(path):
    rows = []
    try:
        with open(path, newline="", encoding="utf-8-sig", errors="replace") as handle:
            for row in csv.DictReader(handle):
                t = utc(row.get("utc"))
                if t is not None: rows.append((t, {k: number(v) for k, v in row.items() if k != "utc"}))
    except OSError: pass
    return rows

def column_values(samples, name, windows=None):
    return [r[name] for t, r in samples if r.get(name) is not None and (not windows or in_windows(t, windows))]

def slope_per_hour(samples, name):
    points = [(t, r[name]) for t, r in samples if r.get(name) is not None]
    if len(points) < 10: return float("nan")
    t0 = points[0][0]; xs = [(t - t0) / 3600.0 for t, _ in points]; ys = [v for _, v in points]
    mx, my = mean(xs), mean(ys); sxx = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if sxx > 0 else float("nan")

# --- comparing arms -------------------------------------------------------------------------------------
def bootstrap_interval(statistic, a, b, rounds=BOOTSTRAP_ROUNDS, seed=807):
    rng = random.Random(seed); diffs = []
    for _ in range(rounds): diffs.append(statistic(rng.choices(a, k=len(a))) - statistic(rng.choices(b, k=len(b))))
    return percentile(diffs, .025), percentile(diffs, .975)

def pooled(runs):
    weight = sum(n for _m, n in runs)
    return sum(m * n for m, n in runs) / weight if weight else float("nan")

def compare(arm, base, minimum=MIN_PRESSES):
    """Difference of means arm - base with a 95 % interval, and the only wording the report may use for it."""
    n_arm, n_base = arm["count"], base["count"]
    out = {"difference": float("nan"), "low": float("nan"), "high": float("nan"), "method": "", "real": False}
    if not n_arm or not n_base: out["verdict"] = "no verdict: no presses"; return out
    if arm["presses"] and base["presses"]:
        out["method"] = "by press"
        out["difference"] = mean(arm["presses"]) - mean(base["presses"])
        out["low"], out["high"] = bootstrap_interval(mean, arm["presses"], base["presses"])
    else:
        out["method"] = "by run"
        out["difference"] = pooled(arm["runs"]) - pooled(base["runs"])
        if len(arm["runs"]) < 3 or len(base["runs"]) < 3:
            out["verdict"] = "no verdict: fewer than 3 test runs for a by-run interval"; return out
        out["low"], out["high"] = bootstrap_interval(pooled, arm["runs"], base["runs"])
    if n_arm < minimum or n_base < minimum: out["verdict"] = f"no verdict: fewer than {minimum} presses ({n_arm} and {n_base})"
    elif not (out["low"] == out["low"] and out["high"] == out["high"]): out["verdict"] = "no verdict: no interval"
    elif out["low"] <= 0 <= out["high"]: out["verdict"] = "not resolved: the interval includes zero"
    else: out["verdict"] = "lower than the baseline" if out["high"] < 0 else "higher than the baseline"; out["real"] = True
    return out

# --- loading an evidence folder ---------------------------------------------------------------------------
def read_arm_table(path):
    try: lines = [l for l in read_lines(path) if l.strip() and not l.lstrip().startswith("#")]
    except OSError: return {}
    return {row["name"]: row for row in csv.DictReader(lines) if row.get("name")}

def load(folder):
    here = os.path.dirname(os.path.abspath(__file__))
    table = read_arm_table(os.path.join(folder, "arms.csv")) or read_arm_table(os.path.join(here, "..", "..", "scripts", "remoteplay", "testkit", "arms.csv"))
    data = {"folder": folder, "kit": as_dict(read_json(os.path.join(folder, "kit.json"))), "hardware": as_dict(read_json(os.path.join(folder, "hardware.json"))),
            "table": table, "runs": []}
    for path in sorted(glob.glob(os.path.join(folder, "bench", "*@*.txt"))):
        name = os.path.basename(path)[:-4]
        run_folder = os.path.join(folder, "runs", name)
        meta = as_dict(read_json(os.path.join(run_folder, "run.json")))
        label = name.split("@")[0]
        purpose = meta.get("purpose") or next((p for p in ("soak", "impairment", "camera") if label.startswith(p + "-")), "latency")
        arm = meta.get("arm") or (label[len(purpose) + 1:] if purpose != "latency" else label)
        log = parse_log(read_lines(path))
        run = {"name": name, "arm": arm, "purpose": purpose, "meta": meta, "log": log, "folder": run_folder,
               "samples": read_samples(os.path.join(run_folder, "samples.csv")), "markers": []}
        offset = utc(meta.get("presentMonStartUtc"))
        windows = [(a - offset, b - offset) for a, b in log["windows"]] if offset is not None else []
        run["presentmon"] = presentmon_stats(os.path.join(run_folder, "presentmon.csv"), windows)
        run["presentmon_offset"] = offset
        marker_path = os.path.join(run_folder, "markers.csv")
        if os.path.exists(marker_path):
            for line in read_lines(marker_path)[1:]:
                when, _, label2 = line.partition(",")
                if utc(when) is not None: run["markers"].append((utc(when), label2.strip()))
        data["runs"].append(run)
    return data

def arm_latency(runs):
    presses, per_run, own, no_answer = [], [], [], 0
    for run in runs:
        log = run["log"]; presses += log["presses"]; no_answer += log["no_answer"]
        per_run += [(s["mean"], s["answered"]) for s in log["summaries"] if s["answered"] > 0]
        own += [s["pushfps"] for s in log["summaries"] if s["pushfps"] > 0]
    count = len(presses) if presses else sum(n for _m, n in per_run)
    medians = [s["median"] for run in runs for s in run["log"]["summaries"] if s["answered"] > 0]
    return {"presses": presses, "runs": per_run, "count": count, "own": own, "no_answer": no_answer, "run_medians": medians, "starts": len(runs)}

# --- the report ---------------------------------------------------------------------------------------------
def build(data, baseline_override=None):
    """A list of sections: (title, [paragraphs], [header], [[cells]])."""
    sections, notes = [], []
    latency_runs = [r for r in data["runs"] if r["purpose"] == "latency"]
    arms = []
    for run in latency_runs:
        if run["arm"] not in arms: arms.append(run["arm"])
    by_arm = {a: [r for r in latency_runs if r["arm"] == a] for a in arms}
    stats = {a: arm_latency(by_arm[a]) for a in arms}

    def baseline_of(arm):
        if baseline_override: return baseline_override
        named = (data["table"].get(arm) or {}).get("compare") or next((r["meta"].get("compare") for r in by_arm[arm] if r["meta"].get("compare")), "")
        return named or "mod-legacy"

    # hardware and kit
    hw, kit = as_dict(data["hardware"]), as_dict(data["kit"])
    rows = []
    for a in [as_dict(x) for x in as_list(hw.get("adapters"))]: rows.append(["GPU", f"{a.get('name')}, driver {a.get('nvidiaDriver') or a.get('driverVersion')}, {a.get('width')}x{a.get('height')} at {a.get('refreshHz')} Hz"])
    if hw.get("cpu"): rows.append(["CPU", str(as_dict(hw["cpu"]).get("name"))])
    for w in as_list(hw.get("warnings")): rows.append(["Warning from the hardware check", str(w)])
    for key in ("original", "modified"):
        build = as_dict(kit.get(key))
        if build: rows.append([f"{key} build", f"{build.get('file')} version {build.get('productVersion')} sha256 {build.get('sha256First12')}"])
    rows.append(["PresentMon / nvidia-smi / elevated", f"{kit.get('presentMon', 'n/a')} / {kit.get('nvidiaSmi', 'n/a')} / {kit.get('elevated', 'n/a')}"])
    if kit.get("dryRun"): rows.append(["DRY RUN", "made-up data from run-tests.ps1 -DryRun: not a measurement"])
    sections.append(("Where this was measured", ["Taken from hardware.json and kit.json in the evidence folder. The report cannot check that this is the intended PC."], ["Item", "Value"], rows))

    # software latency
    rows = []
    for a in arms:
        s, base_name = stats[a], baseline_of(a)
        v = sorted(s["presses"])
        if v: cells = [a, str(len(v)), fmt(statistics.median(v)), fmt(percentile(v, .95)), fmt(percentile(v, .99)), fmt(mean(v))]
        elif s["runs"]: cells = [a, f"{s['count']} (run summaries)", fmt(statistics.median(s["run_medians"])) + " (median of run medians)", "n/a", "n/a", fmt(pooled(s["runs"]))]
        else: cells = [a, "0", "n/a", "n/a", "n/a", "n/a"]
        cells += [fmt(mean(s["own"])), str(s["no_answer"])]
        if base_name == a: cells += ["baseline", "", ""]
        elif base_name not in stats: cells += [f"baseline '{base_name}' was not run", "", ""]
        else:
            c = compare(s, stats[base_name])
            cells += [f"{c['difference']:+.1f} ms vs {base_name}" if c["difference"] == c["difference"] else "n/a",
                      f"[{c['low']:+.1f}, {c['high']:+.1f}] {c['method']}" if c["low"] == c["low"] else "n/a", c["verdict"]]
        rows.append(cells)
    sections.append(("Software latency (built-in test)", [
        "Milliseconds from the press being handed to the session until the console's answer is in a presented picture. The controller poll, the display and the panel are outside this clock.",
        f"Difference of means with a bootstrap 95 % interval. A difference is called lower or higher only when the interval excludes zero and both arms have at least {MIN_PRESSES} presses. One press is quantised to the console's 16.7 ms frames, so the median often does not move when the mean does.",
        "'PushFPS own' is the application's own processing time sampled between presses: processing time, not latency."],
        ["Arm", "Presses", "Median ms", "P95 ms", "P99 ms", "Mean ms", "PushFPS own ms", "No answer", "Mean difference", "95 % interval", "Verdict"], rows))

    # displayed frames (PresentMon), merged over the runs of an arm
    rows, origin_rows, resource_rows, stream_rows = [], [], [], []
    modes_seen = set()
    for a in arms:
        captures = [r["presentmon"] for r in by_arm[a] if r["presentmon"]]
        shown = [v for c in captures for v in c["shown_values"]]
        frames = sum(c["frames"] for c in captures); dropped = sum(c["dropped"] for c in captures)
        decode = [f["decodeFps"] for r in by_arm[a] for t, f in r["log"]["progress"] if not r["log"]["windows"] or in_windows(t, r["log"]["windows"])]
        decode_fps = statistics.median(decode) if decode else float("nan")
        if captures:
            mode_counts = {}
            for c in captures:
                for k, n in c["modes"].items(): mode_counts[k] = mode_counts.get(k, 0) + n
            mode = max(mode_counts, key=mode_counts.get); modes_seen.add(mode)
            share = 100.0 * mode_counts[mode] / frames
            shown_fps = 1000.0 * len(shown) / sum(shown) if shown else float("nan")
            rows.append([a, str(frames), f"{dropped} ({fmt(100.0 * dropped / frames if frames else float('nan'), 2)} %)", fmt(statistics.median(shown) if shown else float("nan"), 2),
                         fmt(percentile(shown, .95), 2), fmt(percentile(shown, .99), 2), fmt(shown_fps, 2), f"{mode} ({fmt(share, 1)} %)" + (" COMPOSED" if mode.startswith("Composed") else ""),
                         fmt(mean([c["tearing_percent"] for c in captures]), 0) + " %", fmt(statistics.median([c["until_displayed_median"] for c in captures]), 2),
                         "; ".join(sorted({c["window"] for c in captures}))])
            types = {}
            for c in captures:
                for k, n in c["frame_types"].items(): types[k] = types.get(k, 0) + n
            extra = max(0.0, shown_fps - decode_fps) if shown_fps == shown_fps and decode_fps == decode_fps else float("nan")
            origin_rows.append([a, fmt(decode_fps), fmt(shown_fps), fmt(extra), ", ".join(f"{k}: {n}" for k, n in sorted(types.items())) or "not reported by PresentMon for this program"])
        else:
            rows.append([a, "no PresentMon capture"] + [""] * 9)
            origin_rows.append([a, fmt(decode_fps), "n/a", "n/a", "no PresentMon capture"])
        samples = [(t, s) for r in by_arm[a] for t, s in r["samples"] if not r["log"]["windows"] or in_windows(t, r["log"]["windows"])]
        def col(name, how=mean, rows_=samples):
            values = [s[name] for _t, s in rows_ if s.get(name) is not None]
            return fmt(how(values)) if values else "n/a"
        video = [c["video_busy_percent"] for c in captures if c["video_busy_percent"] == c["video_busy_percent"]]
        resource_rows.append([a, col("gpuPercent"), col("gpuPercent", max), col("gpuDecoderPercent"), fmt(mean(video)) if video else "n/a", col("vramUsedMb", max),
                              col("cpuTotalPercent"), col("cpuTotalPercent", max), col("appCpuPercent"), col("appPrivateMb", max), str(len(samples))])
        logs = [r["log"] for r in by_arm[a]]
        stream_rows.append([a, str(len(logs)), fmt(decode_fps), str(sum(len(l["keyframe_requests"]) for l in logs)), str(sum(len(l["reconnects"]) for l in logs)),
                            str(max([l["lost_packets_max"] for l in logs] or [0])), str(sum(l["errors"] for l in logs)), str(sum(l["warnings"] for l in logs)),
                            "; ".join(sorted({l["timing"] for l in logs if l["timing"]})) or "not logged (original build)"])
    if len(modes_seen) > 1: notes.append("The arms did NOT all run with the same present mode: frame-time comparisons between arms with different modes are not valid.")
    if any(m.startswith("Composed") for m in modes_seen): notes.append("At least one arm was composed by the desktop compositor (no independent flip): its display timing includes the compositor.")
    sections.append(("Displayed frames (PresentMon)", ["Time between frames actually shown, for the busiest swap chain of the program. Dropped: presented but never shown."],
                     ["Arm", "Frames", "Dropped", "Median ms", "P95 ms", "P99 ms", "Shown per second", "Present mode (share)", "Tearing allowed", "Present to display, median ms", "Part of the capture used"], rows))
    sections.append(("Real pictures and displayed frames", [
        "Decoded per second: pictures received from the console and decoded (real; a 30 fps game arrives in a 60 fps stream with every picture twice). Shown per second: every frame displayed.",
        "PushFPS presents its generated frames itself, so PresentMon cannot tell them from real ones. 'Beyond the stream' is shown minus decoded: generated or repeated frames together, never real game frames."],
        ["Arm", "Decoded per second (real)", "Shown per second (all)", "Beyond the stream per second (generated or repeated)", "Frame types by PresentMon"], origin_rows))
    sections.append(("GPU, CPU and memory", ["Once-a-second samples while the latency test ran (all samples of the run when the test time is unknown). nvidia-smi values are for the first GPU and the whole PC, not only PushFPS."],
                     ["Arm", "GPU % mean", "GPU % max", "Decoder % mean (nvidia-smi)", "Decoder busy % (PresentMon)", "Video memory MB max", "CPU % mean", "CPU % max", "PushFPS CPU % mean", "PushFPS memory MB max", "Samples"], resource_rows))
    sections.append(("Stream health during the runs", ["From the application's own log lines."],
                     ["Arm", "Starts", "Decoded per second", "Keyframe requests", "Reconnects", "Lost packets (largest window)", "Error lines", "Warning lines", "Timing in use"], stream_rows))

    # camera
    camera = read_camera(os.path.join(data["folder"], "camera.csv"))
    if camera["rows"]:
        rows = []
        groups = camera["rows"]
        reference = "direct-ps5" if "direct-ps5" in groups else ("original" if "original" in groups else next(iter(groups)))
        for a, values in groups.items():
            fps = camera["fps"][a]; v = sorted(values)
            cells = [a + ("" if a in stats else " (camera only)"), str(len(v)), fmt(statistics.median(v)), fmt(percentile(v, .95)) if len(v) >= 20 else "n/a", fmt(mean(v)), fmt(v[0]), fmt(v[-1]), fmt(1000.0 / fps, 1)]
            if a == reference: cells += ["reference", "", ""]
            else:
                c = compare({"presses": values, "runs": [], "count": len(values)}, {"presses": groups[reference], "runs": [], "count": len(groups[reference])}, MIN_CAMERA_PRESSES)
                cells += [f"{c['difference']:+.1f} ms vs {reference}", f"[{c['low']:+.1f}, {c['high']:+.1f}]" if c["low"] == c["low"] else "n/a", c["verdict"]]
            rows.append(cells)
        sections.append(("Input to photon (camera)", [
            "Counted by hand from high-speed video: frames from the button fully down to the first changed picture, divided by the camera's frame rate. This is the only table here that includes the controller, the display and the panel.",
            f"Arms marked camera only (Sony's own Remote Play program, the PS5 plugged straight into the monitor) have no other data in this report. A difference is called lower or higher only when the interval excludes zero and both arms have at least {MIN_CAMERA_PRESSES} filmed presses."],
            ["Arm", "Presses", "Median ms", "P95 ms", "Mean ms", "Fastest ms", "Slowest ms", "One camera frame ms", "Mean difference", "95 % interval", "Verdict"], rows))
    else: notes.append("No camera measurements (camera.csv has no rows): nothing in this report includes the controller, the display or the panel.")
    for problem in camera["problems"]: notes.append("camera.csv: " + problem)

    # soak
    rows = []
    for run in [r for r in data["runs"] if r["purpose"] == "soak"]:
        samples, log = run["samples"], run["log"]
        minutes = (samples[-1][0] - samples[0][0]) / 60.0 if len(samples) > 1 else float("nan")
        private = column_values(samples, "appPrivateMb"); tenth = max(1, len(private) // 10)
        pm = run["presentmon"]
        rows.append([run["arm"], fmt(minutes), str(len(log["reconnects"])), str(len(log["keyframe_requests"])), str(log["recovery_stopped"]), str(log["errors"]),
                     fmt(min([f["decodeFps"] for _t, f in log["progress"]] or [float("nan")])), f"{pm['dropped']} of {pm['frames']} (first part only)" if pm else "no PresentMon capture",
                     fmt(mean(private[:tenth])), fmt(mean(private[-tenth:])), fmt(slope_per_hour(samples, "appPrivateMb")), fmt(max(column_values(samples, "vramUsedMb") or [float("nan")]))])
    if rows: sections.append(("Soak", ["One long run. Memory: mean of the first and of the last tenth of the run, and the fitted growth per hour."],
                              ["Arm", "Minutes", "Reconnects", "Keyframe requests", "Recovery gave up", "Error lines", "Lowest decoded per second", "Dropped frames", "Memory MB start", "Memory MB end", "Memory MB per hour", "Video memory MB max"], rows))

    # impairment
    rows = []
    for run in [r for r in data["runs"] if r["purpose"] == "impairment"]:
        log, markers = run["log"], sorted(run["markers"])
        for i, (when, label) in enumerate(markers):
            until = markers[i + 1][0] if i + 1 < len(markers) else when + 120.0
            before = [f["decodeFps"] for t, f in log["progress"] if when - 30 <= t < when]
            normal = statistics.median(before) if before else float("nan")
            after = [(t, f["decodeFps"]) for t, f in log["progress"] if when <= t < until]
            lowest = min([v for _t, v in after] or [float("nan")])
            back = float("nan")
            if after and normal == normal:
                fallen = next((t for t, v in after if v < 0.9 * normal), None)
                if fallen is None: back = 0.0
                else: back = next((t - when for t, v in after if t > fallen and v >= 0.9 * normal), float("nan"))
            gap = float("nan")
            pm, offset = run["presentmon"], run["presentmon_offset"]
            if pm and offset is not None:
                gaps = [number(pick(r, "MsBetweenDisplayChange", "DisplayedTime")) for t, r in pm["rows"] if t is not None and when - offset <= t < until - offset]
                gap = max([g for g in gaps if g] or [float("nan")])
            rows.append([run["arm"], label, fmt(normal), fmt(lowest), fmt(back), fmt(gap, 0), str(sum(1 for t in log["keyframe_requests"] if t is not None and when <= t < until)),
                         str(sum(1 for t in log["reconnects"] if t is not None and when <= t < until))])
        if not markers: notes.append(f"Impairment run of '{run['arm']}' has no markers: Enter was not pressed in the kit's window.")
    if rows: sections.append(("Impairment and recovery", [
        "From each marker (Enter pressed in the kit's window just before the cable was pulled or the load started) to the next one. Back to normal: seconds until the decoded rate is again within 10 % of the rate before the marker, as fine as the progress lines allow. Longest gap: the longest time one picture stayed on screen, from PresentMon."],
        ["Arm", "Marker", "Decoded per second before", "Lowest after", "Back to normal after s", "Longest picture gap ms", "Keyframe requests", "Reconnects"], rows))

    leaks = leak_findings(data["folder"])
    if leaks:
        notes.append(f"PRIVACY CHECK FAILED: {len(leaks)} line(s) in the evidence still hold an address, a long hex value, a name or a private word. Do not pass this folder on. First: " + ", ".join(f"{f} line {n} ({why})" for f, n, why in leaks[:5]))
    else: notes.append("Privacy check: no address, long hex value, name or private word found in the files the kit wrote.")
    return sections, notes, leaks

def read_camera(path):
    out = {"rows": {}, "fps": {}, "problems": []}
    try: lines = [l for l in read_lines(path) if l.strip() and not l.lstrip().startswith("#")]
    except OSError: return out
    for index, row in enumerate(csv.DictReader(lines), 1):
        arm = (row.get("arm") or "").strip(); frames = number(row.get("frames")); fps = number(row.get("camera_fps"))
        if not arm or frames is None or fps is None or frames < 0 or not 30 <= fps <= 100000:
            out["problems"].append(f"row {index} skipped (arm, frames and camera_fps are needed)"); continue
        if out["fps"].setdefault(arm, fps) != fps: out["problems"].append(f"arm {arm}: more than one camera_fps; the first is used for the frame length shown")
        out["rows"].setdefault(arm, []).append(frames / fps * 1000.0)
    return out

def markdown(sections, notes, title):
    def cell(text): return str(text).replace("|", "/").replace("\n", " ")
    out = [f"# {title}", "", "Generated by tools/remoteplay_latency_bench/report.py from the evidence folder. Numbers are what the runs in that folder show, nothing more.", ""]
    out += ["## Read this first", ""] + [f"- {cell(n)}" for n in notes] + [""]
    for heading, paragraphs, header, rows in sections:
        out += [f"## {heading}", ""] + [p for p in paragraphs] + [""]
        if rows:
            out += ["| " + " | ".join(header) + " |", "|" + "---|" * len(header)] + ["| " + " | ".join(cell(c) for c in row) + " |" for row in rows] + [""]
        else: out += ["No data.", ""]
    return "\n".join(out)

def html_page(sections, notes, title):
    e = html.escape
    out = ["<!DOCTYPE html>", "<html lang=\"en\"><head><meta charset=\"utf-8\">", f"<title>{e(title)}</title>",
           "<style>body{font:14px/1.45 system-ui,sans-serif;margin:2em;max-width:1500px;color:#16191d}table{border-collapse:collapse;margin:.6em 0 1.6em}"
           "td,th{border:1px solid #c5cad1;padding:4px 8px;text-align:left;vertical-align:top}th{background:#eef0f3}h2{margin-top:1.8em}"
           ".notes li{margin:.3em 0}</style></head><body>", f"<h1>{e(title)}</h1>",
           "<p>Generated by tools/remoteplay_latency_bench/report.py from the evidence folder. Numbers are what the runs in that folder show, nothing more.</p>",
           "<h2>Read this first</h2><ul class=\"notes\">"] + [f"<li>{e(n)}</li>" for n in notes] + ["</ul>"]
    for heading, paragraphs, header, rows in sections:
        out.append(f"<h2>{e(heading)}</h2>")
        out += [f"<p>{e(p)}</p>" for p in paragraphs]
        if rows:
            out.append("<table><tr>" + "".join(f"<th>{e(h)}</th>" for h in header) + "</tr>")
            out += ["<tr>" + "".join(f"<td>{e(str(c))}</td>" for c in row) + "</tr>" for row in rows]
            out.append("</table>")
        else: out.append("<p>No data.</p>")
    out.append("</body></html>")
    return "\n".join(out)

def unpack(zip_path, target):
    """Extracts an evidence zip; entries that would leave the target folder are skipped."""
    root = os.path.realpath(target)
    with zipfile.ZipFile(zip_path) as archive:
        for info in archive.infolist():
            name = info.filename.replace("\\", "/")
            destination = os.path.realpath(os.path.join(root, name))
            if info.is_dir() or not destination.startswith(root + os.sep): continue
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            with archive.open(info) as source, open(destination, "wb") as sink: sink.write(source.read())
    return root

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("evidence", help="evidence folder or the zip made by run-tests.ps1")
    ap.add_argument("--out", help="folder for report.md and report.html (default: the evidence folder; beside the zip for a zip)")
    ap.add_argument("--baseline", help="compare every arm against this arm instead of the arm table's choice")
    ap.add_argument("--strict", action="store_true", help="exit with 2 when the privacy check finds anything")
    args = ap.parse_args(argv)
    with tempfile.TemporaryDirectory() as temporary:
        folder, out = args.evidence, args.out
        if os.path.isfile(folder):
            out = out or os.path.dirname(os.path.abspath(folder))
            folder = unpack(folder, temporary)
        if not os.path.isdir(folder): print(f"not a folder or zip: {args.evidence}"); return 1
        out = out or folder
        os.makedirs(out, exist_ok=True)
        data = load(folder)
        sections, notes, leaks = build(data, args.baseline)
        title = "Slipstream hardware test report" + (" (DRY RUN, made-up data)" if data["kit"].get("dryRun") else "")
        with open(os.path.join(out, "report.md"), "w", encoding="utf-8") as handle: handle.write(markdown(sections, notes, title) + "\n")
        with open(os.path.join(out, "report.html"), "w", encoding="utf-8") as handle: handle.write(html_page(sections, notes, title) + "\n")
        print(f"runs: {len(data['runs'])}  report: {os.path.join(out, 'report.md')} and report.html")
        for note in notes: print("note:", note)
        return 2 if (leaks and args.strict) else 0

if __name__ == "__main__":
    sys.exit(main())
