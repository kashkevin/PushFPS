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
  - Camera measurements come from camera.csv, light sensor measurements from sensor.csv, both filled in by the user.
    Only those two tables are input to photon; each has its own table, and neither is ever put into a table or a
    verdict with the software clock.
  - "One-page result" (and report-result.json): per arm median, P95 and P99 with order-statistic 95 % intervals and
    the conservative verdict rule of the kit's RESULT.txt; when the evidence holds the kit's result.json, the two are
    compared and any difference is named (DISAGREE).
  - A comparison is also refused when both sides logged a refresh rate or a frame generation state and they differ,
    and an arm is refused when its frame generation is not what arms.csv (column fg) asks for.
  - A comparison is REFUSED when the two arms ran with different stream settings (resolution, frame rate,
    codec, requested bitrate), a different present mode or tearing setting, or a different variable refresh
    state; when one side did not record one of these, the verdict says that it was not checked.
  - "What this evidence holds" lists, per arm, every value a comparison needs, where it comes from and
    whether this folder has it.
  - Every evidence file the kit wrote is checked again for addresses and private words (--strict: exit 2).
Nothing here is a measurement by itself: the numbers are only as good as the runs in the folder.
"""
import argparse, csv, datetime, glob, html, json, math, os, random, re, statistics, sys, tempfile, zipfile

MIN_PRESSES = 120          # software latency: presses per arm before any verdict
MIN_CAMERA_PRESSES = 30    # camera: 4.2 ms per frame at 240 fps
BOOTSTRAP_ROUNDS = 4000

# --- redaction: the same rules as Protect-Text / Select-SafeLines in run-tests.ps1 -------------------
ALLOWED = re.compile(r"\[(INFO |WARN |ERROR|TRACE)\] \[(push-(latency|quality|benchmark|mode|hardware)|remoteplay(-[a-z0-9-]+)?|slipstream-(governor|gpu-budget|watch)|frame-(flow|rate)|pacing|present-deviation|fg-(validity|drops)|display-(vrr|refresh|latency|query)|gpu-(failure|reconnect)|reset-lifecycle|engine-(hang|test)|crash|overlay-hooks|log-backpressure|qml\] \[update-check)\] ")
REFUSED_WORDS = r"registration|regist[_ -]?key|rp[_ -]?key|account|psn(?!r=)|token|secret|passw|ssid|morning|nickname"
REFUSED = re.compile(r"\[remoteplay-discovery\]|" + REFUSED_WORDS, re.I)
PRIVATE_WORD = re.compile(REFUSED_WORDS, re.I)
_MASKS = [
    (re.compile(r'(?i)[A-Za-z]:\\+Users\\+[^\\\r\n"]*'), "[path]"),
    (re.compile(r'[A-Za-z]:\\(?:[^\\\r\n"<>|]*\\)*[^\s",;)<>|]*'), "[path]"),
    (re.compile(r'\\{2,}[^\\\s"]+\\+[^\s",;)<>|]*'), "[path]"),
    (re.compile(r'(?i)(?<![A-Za-z])[A-Za-z]:/+Users/+[^/\r\n"]*'), "[path]"),
    (re.compile(r'(?<![A-Za-z])[A-Za-z]:/(?:[^/\r\n"<>|]*/)*[^\s",;)<>|]*'), "[path]"),
    (re.compile(r"(?<![/\\\w.%+-])[A-Za-z0-9._%+-]+@(?:[A-Za-z0-9-]*[A-Za-z][A-Za-z0-9-]*\.)+[A-Za-z]{2,}\b"), "[email]"),
    (re.compile(r"\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\b"), "[guid]"),
    (re.compile(r"\b[0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}\b"), "[mac]"),
    (re.compile(r"\b[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\b"), "[mac]"),
    (re.compile(r"\b10\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"), "10.x.x.x"),
    (re.compile(r"\b192\.168\.\d{1,3}\.\d{1,3}\b"), "192.168.x.x"),
    (re.compile(r"\b172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}\b"), r"172.\1.x.x"),
    (re.compile(r"\b169\.254\.\d{1,3}\.\d{1,3}\b"), "169.254.x.x"),
    (re.compile(r"\b127\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"), "127.x.x.x"),
    (re.compile(r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"), "x.x.x.x"),
    (re.compile(r"(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){3,7}[0-9A-Fa-f]{1,4}(?![0-9A-Za-z:])"), "[ipv6]"),
    (re.compile(r"(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){1,6}:(?:[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4}){0,5})?(?![0-9A-Za-z:])"), "[ipv6]"),
    (re.compile(r"\[ipv6\]%[0-9A-Za-z]+"), "[ipv6]"),
    (re.compile(r"(?:0[xX])?[0-9A-Fa-f]{16,}"), "[hex]"),
    (re.compile(r"\b(?=[0-9A-Fa-f]*[A-Fa-f])[0-9A-Fa-f]{12}\b"), "[mac]"),
    (re.compile(r"\b(?=[0-9A-Fa-f]*[A-Fa-f])[0-9A-Fa-f]{13,15}\b"), "[hex]"),
    (re.compile(r"\b(?-i:(host|name|server|computer|device|network|Host|Name|Server|Computer|Device|Network))=(?!\[name\]|(?:\d{1,3}|x)(?:\.(?:\d{1,3}|x)){3}(?![0-9A-Za-z.]))[^\s,;)]+"), r"\1=[name]"),
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
            if not (name.endswith((".json", ".txt")) or name in ("markers.csv", "camera.csv", "reliability.csv")) or name.startswith("report."): continue
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

def build_hash(build):
    """The start of a build's SHA-256 as kit.json holds it (10 digits since kit 3; 12 before)."""
    return build.get("sha256First10") or build.get("sha256First12") or ""

def as_dict(value): return value if isinstance(value, dict) else {}

# --- the log of one run --------------------------------------------------------------------------------
LINE = re.compile(r"^(\S+Z) t=\d+ \[(\w+)\s*\] \[([^\]]+)\] (.*)$")
FIELD = re.compile(r"(\w+)=(-?[\d.]+)")
WORD = re.compile(r"(\w+)=([^\s,;()]+)")

def numbers(text):
    """key=number fields of a log line (rtt=0.45ms gives 0.45)."""
    return {k: number(v.rstrip(".")) for k, v in FIELD.findall(text)}

def words(text): return dict(WORD.findall(text))

FLOW_COUNTERS = ("sourceAccepted", "realPresented", "generatedPresented", "generatedExpiredAfterEval", "cancelledBeforePresent", "mailboxOverwritten", "fgEvaluated", "fgInvalid")

def parse_log(lines):
    """Everything the report uses from the whitelisted log lines of one run."""
    out = {"presses": [], "summaries": [], "no_answer": 0, "windows": [], "progress": [], "keyframe_requests": [], "reconnects": [],
           "recovery_stopped": 0, "errors": 0, "warnings": 0, "lost_packets_max": 0, "session_start": None, "timing": "", "settings": "",
           "streams": [], "sessions": [], "decoders": [], "health": [], "route": "", "cadence": [], "flow": {k: 0 for k in FLOW_COUNTERS}, "flow_lines": 0,
           "frame_rate": [], "vrr": [], "display_latency": [], "display_events": [], "pacing": "", "build": "", "gpu_failures": [], "gpu_reconnects": [],
           "crashes": [], "hangs": [], "engine_test": [], "log_dropped": 0, "kbm": [], "quality": [], "starts": [], "echo": [], "ingress": [],
           "fg_backend": "", "previous_run": [], "overlays": [], "first": None, "last": None, "recovery_events": [], "recovery_stopped_at": []}
    open_window = None
    flow_previous = None
    def flow_close():
        if flow_previous:
            for key in FLOW_COUNTERS: out["flow"][key] += int(flow_previous.get(key) or 0)
    for raw in lines:
        m = LINE.match(raw)
        when, level, component, text = (utc(m.group(1)), m.group(2), m.group(3), m.group(4)) if m else (None, "", "", raw)
        if not m:
            m2 = re.search(r"\[([a-z-]+)\] (.*)$", raw)
            if m2: component, text = m2.group(1), m2.group(2)
        if when is not None:
            if out["first"] is None: out["first"] = when
            out["last"] = when
        if level == "ERROR": out["errors"] += 1
        if level == "WARN": out["warnings"] += 1
        if component == "push-latency":
            if text.startswith("remote play latency test started"): open_window = when
            m3 = re.match(r"trials answerMs=\[([^\]]*)\]", text)
            if m3: out["presses"] += [v for v in (number(x) for x in m3.group(1).split(",")) if v is not None]
            m3 = re.match(r"done answered=(\d+)/(\d+) meanMs=([\d.]+) medianMs=([\d.]+).*pushfpsMs=([\d.]+)", text)
            if m3:
                out["summaries"].append({"answered": int(m3.group(1)), "trials": int(m3.group(2)), "mean": float(m3.group(3)),
                                         "median": float(m3.group(4)), "pushfps": float(m3.group(5)), "when": when})
                if open_window is not None and when is not None: out["windows"].append((open_window, when))
                open_window = None
            if "no answer seen" in text: out["no_answer"] += 1
            if text.startswith("cancelled"): open_window = None
        elif component == "push-quality":
            if text.startswith("done "): out["quality"].append(numbers(text))
        elif component == "remoteplay-progress":
            fields = {k: number(v) for k, v in FIELD.findall(text)}
            if when is not None and fields.get("decodeFps") is not None: out["progress"].append((when, fields))
        elif component == "remoteplay-transport":
            lost = dict(FIELD.findall(text)).get("packetWindowLost")
            if number(lost): out["lost_packets_max"] = max(out["lost_packets_max"], int(number(lost)))
        elif component == "remoteplay-recovery":
            if text.startswith("request keyframe"): out["keyframe_requests"].append(when)
            if text.startswith("reconnected"): out["reconnects"].append(when)
            if "recovery stopped" in text: out["recovery_stopped"] += 1; out["recovery_stopped_at"].append(when)
            if "retry allowance renewed" not in text: out["recovery_events"].append(when)
        elif component == "remoteplay" and text.startswith("session started"):
            if out["session_start"] is None: out["session_start"] = when
            w = words(text)
            out["sessions"].append(when)
            if w.get("profile"): out["streams"].append((w["profile"], (w.get("codec") or "?").upper(), w.get("requestedBitrateKbps") or "?"))
        elif component == "remoteplay-decode":
            if text.startswith("actual="): out["decoders"].append(words(text).get("actual", "?") + (" (after a fallback)" if words(text).get("fallback") == "true" else ""))
        elif component == "remoteplay-timing":
            if text.startswith("ownerWait"): out["timing"] = text.split(" (")[0]
            else: out["echo"].append(text.split(" (")[0][:160])
        elif component == "remoteplay-settings":
            out["settings"] = text[:200]
            if "slipstream.ini" in text: out["starts"].append(when)
        elif component == "remoteplay-health":
            if text.startswith("grade="):
                entry = numbers(text); w = words(text)
                entry.update({"t": when, "grade": w.get("grade", "unknown"), "interval": w.get("interval", "unknown"), "route": w.get("route", ""), "link": w.get("link", ""),
                              "rtt": numbers(text).get("rtt") if "not-measured" not in text else None})
                out["health"].append(entry)
            elif text.startswith("route="):
                w = words(text); out["route"] = f"{w.get('route', '?')}, {w.get('link', '?')}, {w.get('linkMbps', '?')} Mbit/s"
        elif component == "remoteplay-cadence":
            if text.startswith("state="):
                entry = numbers(text); entry["state"] = words(text).get("state", "?"); entry["t"] = when
                entry["removal"] = text.rsplit("duplicate removal ", 1)[1].rstrip(")")[:60] if "duplicate removal " in text else ""
                out["cadence"].append(entry)
            else: out["echo"].append(text[:160])
        elif component == "remoteplay-lan":
            if " ingress " in text: out["ingress"].append(numbers(text))
            else: out["echo"].append(text.split(" (")[0][:200])
        elif component == "remoteplay-kbm":
            out["kbm"].append((when, text))
            if text.startswith("experimental keyboard and mouse"): out["echo"].append(text[:120])
        elif component == "frame-flow":
            entry = numbers(text); entry["state"] = words(text).get("state", "")
            restarted = flow_previous is not None and (entry.get("session") != flow_previous.get("session") or any((entry.get(k) or 0) < (flow_previous.get(k) or 0) for k in FLOW_COUNTERS))
            if restarted: flow_close()
            flow_previous = entry; out["flow_lines"] += 1
            if entry["state"] == "closed": flow_close(); flow_previous = None
        elif component == "frame-rate":
            if when is not None: out["frame_rate"].append((when, numbers(text)))
        elif component == "fg-validity": out["fg_backend"] = words(text).get("backend", out["fg_backend"])
        elif component == "pacing": out["pacing"] = text[:200]
        elif component == "display-vrr":
            w = words(text); out["vrr"].append((when, {"level": w.get("level", "?"), "active": w.get("active", "?"), "in_vrr": w.get("displayInVrrMode", "?"), "requested": w.get("requested", "?"), "refresh": number(w.get("refreshHz"))}))
        elif component == "display-latency":
            if when is not None: out["display_latency"].append((when, numbers(text)))
        elif component in ("display-refresh", "display-query"): out["display_events"].append((when, component, text[:100]))
        elif component == "qml":
            if text.startswith("[update-check]"):
                w = words(text)
                if w.get("build") or w.get("installed"): out["build"] = f"{w.get('build') or '?'} (installed {w.get('installed') or '?'})"
        elif component == "gpu-failure":
            if text.startswith("operation="): out["gpu_failures"].append(when)
        elif component == "gpu-reconnect": out["gpu_reconnects"].append((when, text[:120]))
        elif component == "crash":
            if "unhandled exception" in text: out["crashes"].append(when)
        elif component == "engine-hang":
            if text.startswith("owner="): out["hangs"].append(when)
        elif component == "engine-test": out["engine_test"].append((when, text[:120]))
        elif component == "log-backpressure": out["log_dropped"] += int(numbers(text).get("dropped") or 0)
        elif component == "overlay-hooks":
            if text.startswith("previous run:"): out["previous_run"].append((when, text[:160]))
            else: out["overlays"].append(text.split(" (")[0][:120])
    flow_close()
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
RELIABILITY_PURPOSES = ("reliability", "gpuloss", "safestart")

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
        purpose = meta.get("purpose") or next((p for p in ("soak", "impairment", "camera") + RELIABILITY_PURPOSES if label.startswith(p + "-")), "latency")
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

# --- what an arm ran with, and whether two arms may be compared ---------------------------------------------
GRADES = ("unknown", "stable", "unsteady", "unstable")

def dominant(counts):
    """(most frequent key, its share in percent) or (None, nan)."""
    total = sum(counts.values())
    if not total: return None, float("nan")
    top = max(counts, key=counts.get)
    return top, 100.0 * counts[top] / total

def receive_to_decoded(progress, windows=None):
    """Once-a-second estimate from the two ages in the progress line: how long the newest decoded picture took
    from complete arrival to decoded. Only lines whose newest received picture was already decoded give a value."""
    values = []
    for t, f in progress:
        if windows and not in_windows(t, windows): continue
        a, b = f.get("receiveAgeMs"), f.get("decodeAgeMs")
        if a is not None and b is not None and 0 <= a - b < 1000 and (f.get("decodeFps") or 0) > 0: values.append(a - b)
    return values

def arm_facts(runs):
    """Stream settings, presentation, build and switches of one arm, as its runs recorded them."""
    logs = [r["log"] for r in runs]
    captures = [r["presentmon"] for r in runs if r.get("presentmon")]
    modes, tearing, frames = {}, 0.0, 0
    for c in captures:
        for k, n in c["modes"].items(): modes[k] = modes.get(k, 0) + n
        tearing += c["tearing_percent"] * c["frames"]; frames += c["frames"]
    mode, share = dominant(modes)
    flow = {k: sum(l["flow"][k] for l in logs) for k in FLOW_COUNTERS}
    if not any(l["flow_lines"] for l in logs): generation = "not logged"
    elif flow["generatedPresented"]: generation = f"on ({flow['generatedPresented']} generated frames presented)"
    else: generation = "off (no generated frame presented)"
    return {"streams": sorted({s for l in logs for s in l["streams"]}), "decoders": sorted({d for l in logs for d in l["decoders"]}),
            "mode": mode, "mode_share": share, "tearing": None if not frames else ("allowed" if tearing / frames >= 50 else "off"),
            "vrr": sorted({state["active"] for l in logs for _t, state in l["vrr"][-1:] if state["active"] in ("true", "false")}),
            "vrr_level": sorted({state["level"] for l in logs for _t, state in l["vrr"][-1:]}),
            "switch_sets": sorted({tuple(sorted(str(x) for x in as_list(r["meta"].get("switches")) + as_list(r["meta"].get("kitSwitches")))) for r in runs if "switches" in r["meta"]}),
            "builds": sorted({l["build"] for l in logs if l["build"]}), "echo": sorted({e for l in logs for e in [l["timing"]] + l["echo"] if e}),
            "flow": flow, "generation": generation, "backend": sorted({l["fg_backend"] for l in logs if l["fg_backend"]}),
            "fg_state": None if generation == "not logged" else ("on" if flow["generatedPresented"] else "off"),
            "refresh": sorted({str(int(round(state["refresh"]))) for l in logs for _t, state in l["vrr"][-1:] if state["refresh"]})}

def stream_text(streams): return "; ".join(f"{profile} {codec} {bitrate} kbit/s" for profile, codec, bitrate in streams) or "not logged"

def mode_text(facts):
    if not facts["mode"]: return "not recorded"
    return f"{facts['mode']}, tearing {facts['tearing']}"

def comparability(name, facts, base_name, base):
    """(why the comparison is refused or "", [what could not be checked because one side did not record it])."""
    for who, f in ((name, facts), (base_name, base)):
        if len(f["streams"]) > 1: return f"REFUSED: '{who}' ran with more than one stream setting ({stream_text(f['streams'])})", []
        if len(f["switch_sets"]) > 1: return f"REFUSED: the runs of '{who}' did not all have the same switches", []
        if len(f["vrr"]) > 1: return f"REFUSED: variable refresh was active in some runs of '{who}' and not in others", []
        if len(f.get("refresh") or []) > 1: return f"REFUSED: the runs of '{who}' ran at different refresh rates ({' / '.join(f['refresh'])} Hz)", []
    # Refresh rate and frame generation: refused when both sides logged them and they differ (not listed as unchecked otherwise).
    if facts.get("refresh") and base.get("refresh") and facts["refresh"] != base["refresh"]:
        return f"REFUSED: refresh rate differs ({name}: {'/'.join(facts['refresh'])} Hz; {base_name}: {'/'.join(base['refresh'])} Hz)", []
    if facts.get("fg_state") and base.get("fg_state") and facts["fg_state"] != base["fg_state"]:
        return f"REFUSED: frame generation differs ({name}: {facts['fg_state']}; {base_name}: {base['fg_state']})", []
    unchecked = []
    if facts["streams"] and base["streams"]:
        if facts["streams"] != base["streams"]:
            return f"REFUSED: stream settings differ ({name}: {stream_text(facts['streams'])}; {base_name}: {stream_text(base['streams'])})", []
    else: unchecked.append("stream settings")
    if facts["mode"] and base["mode"]:
        if (facts["mode"], facts["tearing"]) != (base["mode"], base["tearing"]):
            return f"REFUSED: present mode differs ({name}: {mode_text(facts)}; {base_name}: {mode_text(base)})", []
    else: unchecked.append("present mode")
    if facts["vrr"] and base["vrr"]:
        if facts["vrr"] != base["vrr"]:
            return f"REFUSED: variable refresh state differs ({name}: active={facts['vrr'][0]}; {base_name}: active={base['vrr'][0]})", []
    else: unchecked.append("variable refresh")
    return "", unchecked

def same_conditions(unchecked):
    checked = [x for x in ("stream settings", "present mode", "variable refresh") if x not in unchecked]
    text = ("same " + ", ".join(checked)) if checked else ""
    if unchecked: text += ("; " if text else "") + "NOT CHECKED (not recorded on one side): " + ", ".join(unchecked)
    return text

def health_summary(logs):
    entries = [e for l in logs for e in l["health"]]
    if not entries: return None
    def total(key): return sum(int(e.get(key) or 0) for e in entries)
    def values(key): return [e[key] for e in entries if e.get(key) is not None]
    rtt = next((e["rtt"] for e in entries if e.get("rtt") is not None), None)
    return {"intervals": len(entries), "loss_mean": mean(values("lossPercent")), "loss_max": max(values("lossPercent") or [float("nan")]), "lost": total("lost"),
            "jitter_mean": mean(values("frameJitterMs")), "jitter_peak": max(values("frameJitterPeakMs") or [float("nan")]), "late": total("late"),
            "reorder_timeouts": total("reorderTimeouts"), "fec_recovered": total("fecRecovered"), "fec_failed": total("fecFailed"),
            "keyframe_requests": total("keyframeRequests"), "decoder_resets": total("decoderResets"), "frame_drops": total("frameDrops"),
            "mbps": mean(values("mbps")), "rtt": rtt, "worst": max((e["interval"] if e["interval"] in GRADES else "unknown" for e in entries), key=GRADES.index),
            "route": "; ".join(sorted({l["route"] for l in logs if l["route"]} or {f"{e['route']}, {e['link']}" for e in entries if e.get("route")}))}

def soak_result(run):
    """Trends and verdict of one soak run (soak.py), with its PresentMon windows read."""
    import soak
    windows = []
    for path, started in soak.read_windows(run["folder"]):
        if utc(started) is not None: windows.append((utc(started), presentmon_stats(path)))
    if not windows and run.get("presentmon") and run.get("presentmon_offset") is not None: windows.append((run["presentmon_offset"], run["presentmon"]))
    return soak.analyse(run, windows)

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
    if hw.get("cpu"): rows.append(["CPU", f"{as_dict(hw['cpu']).get('name')}, {as_dict(hw['cpu']).get('cores', '?')} cores, {as_dict(hw['cpu']).get('threads', '?')} threads; memory {hw.get('ramGb', '?')} GB"])
    if hw.get("windows"): rows.append(["Windows", str(hw["windows"])])
    for m in [as_dict(x) for x in as_list(hw.get("monitors"))]: rows.append(["Monitor", f"{m.get('manufacturer') or ''} {m.get('name') or ''}".strip() + ("" if m.get("active", True) else " (not active)")])
    for n in [as_dict(x) for x in as_list(hw.get("network"))]: rows.append(["Network adapter", f"{n.get('description')}, {n.get('linkSpeed')}, {n.get('media')}" + (" (default route)" if n.get("defaultRoute") else "")])
    ping = as_dict(hw.get("ps5Ping"))
    if ping: rows.append(["Ping to the console", f"median {ping.get('medianMs')} ms, worst {ping.get('maxMs')} ms, {ping.get('replies')} of {ping.get('sent')} answered"])
    settings = as_dict(hw.get("settings"))
    if settings: rows.append(["Windows settings", f"GPU scheduling {settings.get('hardwareGpuScheduling')}; Game Mode {settings.get('gameMode')}; DirectX user settings '{settings.get('directXUserGlobalSettings') or 'not set'}'; {settings.get('powerPlan') or 'power plan unknown'}"])
    for w in as_list(hw.get("warnings")): rows.append(["Warning from the hardware check", str(w)])
    for key in ("original", "modified"):
        build = as_dict(kit.get(key))
        if build: rows.append([f"{key} build", f"{build.get('file')} version {build.get('productVersion')} sha256 {build_hash(build)}"])
    rows.append(["PresentMon / nvidia-smi / elevated", f"{kit.get('presentMon', 'n/a')} / {kit.get('nvidiaSmi', 'n/a')} / {kit.get('elevated', 'n/a')}"])
    rows.append(["Kit version / network health line asked for", f"{kit.get('kitVersion', 'n/a')} / {kit.get('health', 'n/a')}"])
    if kit.get("dryRun"): rows.append(["DRY RUN", "made-up data from run-tests.ps1 -DryRun: not a measurement"])
    sections.append(("Where this was measured", ["Taken from hardware.json and kit.json in the evidence folder. The report cannot check that this is the intended PC."], ["Item", "Value"], rows))

    facts = {a: arm_facts(by_arm[a]) for a in arms}
    refused = {}

    # software latency
    rows = []
    for a in arms:
        s, base_name = stats[a], baseline_of(a)
        v = sorted(s["presses"])
        if v: cells = [a, str(len(v)), fmt(statistics.median(v)), fmt(percentile(v, .95)), fmt(percentile(v, .99)), fmt(mean(v))]
        elif s["runs"]: cells = [a, f"{s['count']} (run summaries)", fmt(statistics.median(s["run_medians"])) + " (median of run medians)", "n/a", "n/a", fmt(pooled(s["runs"]))]
        else: cells = [a, "0", "n/a", "n/a", "n/a", "n/a"]
        cells += [fmt(mean(s["own"])), str(s["no_answer"])]
        if base_name == a: cells += ["baseline", "", "", ""]
        elif base_name not in stats: cells += [f"baseline '{base_name}' was not run", "", "", ""]
        else:
            why, unchecked = comparability(a, facts[a], base_name, facts[base_name])
            if why:
                refused[a] = why
                cells += [f"refused (vs {base_name})", "", why, "different conditions"]
            else:
                c = compare(s, stats[base_name])
                cells += [f"{c['difference']:+.1f} ms vs {base_name}" if c["difference"] == c["difference"] else "n/a",
                          f"[{c['low']:+.1f}, {c['high']:+.1f}] {c['method']}" if c["low"] == c["low"] else "n/a", c["verdict"], same_conditions(unchecked)]
        rows.append(cells)
    for a, why in refused.items(): notes.append(f"Comparison of '{a}' with its baseline {why}. Repeat the runs with identical settings; no difference is reported for it.")
    sections.append(("Software latency (built-in test; software, not input-to-photon)", [
        "Milliseconds from the press being handed to the session until the console's answer is in a presented picture. The controller poll, the display and the panel are outside this clock.",
        f"Difference of means with a bootstrap 95 % interval. A difference is called lower or higher only when the interval excludes zero and both arms have at least {MIN_PRESSES} presses. One press is quantised to the console's 16.7 ms frames, so the median often does not move when the mean does.",
        "'PushFPS own' is the application's own processing time sampled between presses: processing time, not latency.",
        "A comparison is refused when the two arms did not run with the same stream settings, present mode and tearing setting, or variable refresh state. 'Conditions' says which of these were recorded on both sides and found equal."],
        ["Arm", "Presses", "Median ms", "P95 ms", "P99 ms", "Mean ms", "PushFPS own ms", "No answer", "Mean difference", "95 % interval", "Verdict", "Conditions"], rows))

    # displayed frames (PresentMon), merged over the runs of an arm
    rows, origin_rows, resource_rows, stream_rows, network_rows, flow_rows, ran_rows = [], [], [], [], [], [], []
    holds = {}
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
        logs = [r["log"] for r in by_arm[a]]
        own_vram = [f["vramMiB"] for l in logs for t, f in l["frame_rate"] if f.get("vramMiB") is not None and (not l["windows"] or in_windows(t, l["windows"]))]
        resource_rows.append([a, col("gpuPercent"), col("gpuPercent", max), col("gpuDecoderPercent"), fmt(mean(video)) if video else "n/a", col("vramUsedMb", max),
                              col("cpuTotalPercent"), col("cpuTotalPercent", max), col("appCpuPercent"), col("appPrivateMb", max), str(len(samples)),
                              col("appGpu3dPercent"), col("appGpuDecodePercent"), col("appGpuDedicatedMb", max), fmt(max(own_vram)) if own_vram else "n/a"])
        to_decoded = [v for l in logs for v in receive_to_decoded(l["progress"], l["windows"])]
        scanout = [f["presentToScanoutP50Ms"] for l in logs for t, f in l["display_latency"] if f.get("presentToScanoutP50Ms") is not None and (not l["windows"] or in_windows(t, l["windows"]))]
        stream_rows.append([a, str(len(logs)), fmt(decode_fps), str(sum(len(l["keyframe_requests"]) for l in logs)), str(sum(len(l["reconnects"]) for l in logs)),
                            str(max([l["lost_packets_max"] for l in logs] or [0])), str(sum(l["errors"] for l in logs)), str(sum(l["warnings"] for l in logs)),
                            "; ".join(sorted({l["timing"] for l in logs if l["timing"]})) or "not logged (original build)",
                            fmt(statistics.median(to_decoded), 2) if to_decoded else "n/a", fmt(statistics.median(scanout), 2) if scanout else "n/a"])

        # network, frames by origin, what the arm ran with, and what the folder holds for it
        f = facts[a]
        health = health_summary(logs)
        mbps = [p["videoMbps"] for l in logs for t, p in l["progress"] if p.get("videoMbps") is not None and (not l["windows"] or in_windows(t, l["windows"]))]
        if health:
            network_rows.append([a, health["route"] or "not logged", str(health["intervals"]), fmt(health["loss_mean"], 3), fmt(health["loss_max"], 3), str(health["lost"]), fmt(health["jitter_mean"], 2),
                                 fmt(health["jitter_peak"], 2), str(health["late"]), str(health["reorder_timeouts"]), f"{health['fec_recovered']} / {health['fec_failed']}", str(health["keyframe_requests"]),
                                 str(health["decoder_resets"]), str(health["frame_drops"]), fmt(health["mbps"], 2), fmt(health["rtt"], 2), health["worst"]])
        else:
            network_rows.append([a, "not logged", "0 (no health line: run the kit with -Health)", "n/a", "n/a", "n/a", "n/a", "n/a", "n/a", "n/a", "n/a", str(sum(len(l["keyframe_requests"]) for l in logs)),
                                 "n/a", "n/a", fmt(mean(mbps), 2) + " (progress line)" if mbps else "n/a", "n/a", "n/a"])
        cadence = [c for l in logs for c in l["cadence"]]
        flow = f["flow"]; presented = flow["realPresented"] + flow["generatedPresented"]
        def cadence_sum(key): return str(sum(int(c.get(key) or 0) for c in cadence)) if cadence else "n/a"
        flow_rows.append([a] + ([str(flow["realPresented"]), str(flow["generatedPresented"]), fmt(100.0 * flow["generatedPresented"] / presented if presented else float("nan"), 1),
                                 str(flow["generatedExpiredAfterEval"]), str(flow["cancelledBeforePresent"]), str(flow["mailboxOverwritten"])] if any(l["flow_lines"] for l in logs) else ["not logged"] + ["n/a"] * 5) +
                         [", ".join(sorted({c["state"] for c in cadence})) or "not logged", fmt(mean([c["streamFps"] for c in cadence if c.get("streamFps") is not None]), 2),
                          fmt(mean([c["sourceFps"] for c in cadence if c.get("sourceFps") is not None]), 2), cadence_sum("repeatsFound"), cadence_sum("repeatsRemoved"), cadence_sum("keptUnsure"),
                          "; ".join(sorted({c["removal"] for c in cadence if c["removal"]})) or "n/a"])
        kit_build = as_dict(kit.get(next((r["meta"].get("exe") for r in by_arm[a] if r["meta"].get("exe")), "") or ("original" if a == "original" else "modified")))
        ran_rows.append([a, f"sha256 {build_hash(kit_build) or 'n/a'}, file version {kit_build.get('productVersion', 'n/a')}; the program says: {'; '.join(f['builds']) or 'not logged'}",
                         " | ".join("; ".join(x) or "(none)" for x in f["switch_sets"]) or "not recorded", "; ".join(f["echo"]) or "not logged", stream_text(f["streams"]),
                         "; ".join(f["decoders"]) or "not logged", mode_text(f) + (f" ({fmt(f['mode_share'], 1)} % of frames)" if f["mode"] else ""),
                         ("active=" + "/".join(f["vrr"]) + " level " + "/".join(f["vrr_level"])) if f["vrr"] else "not logged", f["generation"] + (f", {'/'.join(f['backend'])}" if f["backend"] else "")])
        s_lat = stats[a]
        gpu_n = len([1 for _t, x in samples if x.get("gpuPercent") is not None]); own_n = len([1 for _t, x in samples if x.get("appGpu3dPercent") is not None])
        holds[a] = [
            f"{len(s_lat['presses'])} presses" if s_lat["presses"] else (f"{s_lat['count']} presses in run summaries only (no P95, no P99)" if s_lat["count"] else "MISSING"),
            f"{frames} frames in {len(captures)} capture(s)" if captures else "MISSING: no PresentMon capture (PresentMon in the kit folder, elevated PowerShell)",
            mode_text(f) if f["mode"] else "MISSING: needs PresentMon",
            ("active=" + "/".join(f["vrr"])) if f["vrr"] else "MISSING: no [display-vrr] line (kit 2 or later copies it)",
            (f"whole GPU: {gpu_n} samples; PushFPS process: {own_n} samples" if gpu_n or own_n else "MISSING: no nvidia-smi and no Windows GPU counters") + (f"; program's own video memory: {len(own_vram)} values" if own_vram else ""),
            "; ".join(x for x in [f"nvidia-smi: {len([1 for _t, x in samples if x.get('gpuDecoderPercent') is not None])} samples", f"PushFPS process: {len([1 for _t, x in samples if x.get('appGpuDecodePercent') is not None])} samples",
                                  "PresentMon video busy" if video else ""] if x and not x.endswith(": 0 samples")) or "MISSING",
            f"{len([1 for _t, x in samples if x.get('cpuTotalPercent') is not None])} samples" if samples else "MISSING",
            f"{health['intervals']} intervals of 5 s" if health else ("MISSING: the original build has no health line" if a == "original" else "MISSING: run the kit with -Health (VEYRA_EXP_RP_HEALTH=1)"),
            f"{sum(len(l['keyframe_requests']) for l in logs)} keyframe requests, {sum(len(l['reconnects']) for l in logs)} reconnects (every one is logged)",
            f"{len(cadence)} lines" if cadence else "MISSING: logged only with VEYRA_EXP_RP_STREAM_DEDUP=1",
            f"{sum(l['flow_lines'] for l in logs)} lines" if any(l["flow_lines"] for l in logs) else "MISSING: no [frame-flow] line (kit 2 or later copies it)",
            stream_text(f["streams"]) if f["streams"] else "MISSING: no 'session started' line",
            "; ".join(f["decoders"]) or "MISSING: no [remoteplay-decode] line",
            "yes" if hw else "MISSING: no hardware.json",
            (f"sha256 {build_hash(kit_build)}" if build_hash(kit_build) else "file hash MISSING") + ("; " + "; ".join(f["builds"]) if f["builds"] else "; the program's own version line is MISSING (no update check ran)"),
            ("recorded by the kit" if f["switch_sets"] else "MISSING: no run.json") + ("; echoed by the program" if f["echo"] else "; not echoed by the program"),
        ]
        problems = [(len(l["crashes"]), "crash line(s)") for l in logs] + [(len(l["gpu_failures"]), "GPU failure line(s)") for l in logs] + [(len(l["hangs"]), "engine hang line(s)") for l in logs] + \
                   [(l["log_dropped"], "log line(s) dropped by the program's own log queue") for l in logs]
        for label in sorted({label for _n, label in problems}):
            count = sum(n for n, lab in problems if lab == label)
            if count: notes.append(f"Arm '{a}': {count} {label} during its latency runs.")
    if len(modes_seen) > 1: notes.append("The arms did NOT all run with the same present mode: frame-time comparisons between arms with different modes are not valid.")
    if any(m.startswith("Composed") for m in modes_seen): notes.append("At least one arm was composed by the desktop compositor (no independent flip): its display timing includes the compositor.")
    sections.append(("Displayed frames (PresentMon)", ["Time between frames actually shown, for the busiest swap chain of the program. Dropped: presented but never shown."],
                     ["Arm", "Frames", "Dropped", "Median ms", "P95 ms", "P99 ms", "Shown per second", "Present mode (share)", "Tearing allowed", "Present to display, median ms", "Part of the capture used"], rows))
    sections.append(("Real pictures and displayed frames", [
        "Decoded per second: pictures received from the console and decoded (real; a 30 fps game arrives in a 60 fps stream with every picture twice). Shown per second: every frame displayed.",
        "PushFPS presents its generated frames itself, so PresentMon cannot tell them from real ones. 'Beyond the stream' is shown minus decoded: generated or repeated frames together, never real game frames."],
        ["Arm", "Decoded per second (real)", "Shown per second (all)", "Beyond the stream per second (generated or repeated)", "Frame types by PresentMon"], origin_rows))
    sections.append(("GPU, CPU and memory", ["Once-a-second samples while the latency test ran (all samples of the run when the test time is unknown). nvidia-smi values are for the first GPU and the whole PC, not only PushFPS."],
                     ["Arm", "GPU % mean", "GPU % max", "Decoder % mean (nvidia-smi)", "Decoder busy % (PresentMon)", "Video memory MB max", "CPU % mean", "CPU % max", "PushFPS CPU % mean", "PushFPS memory MB max", "Samples",
                      "PushFPS GPU 3D % mean", "PushFPS video decode % mean", "PushFPS video memory MB max (Windows counters)", "PushFPS video memory MiB max (its own log)"], resource_rows))
    sections.append(("Stream health during the runs", ["From the application's own log lines. 'Receive to decoded' is derived once a second from two ages in the progress line (complete picture received until decoded): processing time, not latency. 'Present to scanout' is the program's own DXGI statistics: not a photon measurement."],
                     ["Arm", "Starts", "Decoded per second", "Keyframe requests", "Reconnects", "Lost packets (largest window)", "Error lines", "Warning lines", "Timing in use",
                      "Receive to decoded ms, median (derived)", "Present to scanout ms, median of P50 (program)"], stream_rows))
    sections.append(("Network health", ["From the [remoteplay-health] line (every 5 s, only with VEYRA_EXP_RP_HEALTH=1: kit parameter -Health). This PC's side only. Without it only keyframe requests and the received bitrate are known."],
                     ["Arm", "Route and link", "Intervals", "Loss % mean", "Loss % worst interval", "Packets lost", "Frame jitter ms mean", "Frame jitter ms peak", "Late packets", "Reorder timeouts", "FEC recovered / failed",
                      "Keyframe requests", "Decoder resets", "Frames dropped (network)", "Received Mbit/s", "Round trip ms (at connect)", "Worst interval grade"], network_rows))
    sections.append(("Frames by origin, duplicates and cadence", [
        "Presented frames as the program counts them where it presents them ([frame-flow]): real and generated apart, over the whole of each run. Cadence and duplicate columns come from [remoteplay-cadence], which exists only with VEYRA_EXP_RP_STREAM_DEDUP=1."],
        ["Arm", "Real presented", "Generated presented", "Generated %", "Generated, expired unshown", "Cancelled before present", "Unread pictures overwritten", "Cadence state", "Stream pictures per second",
         "Source pictures per second", "Repeats found", "Repeats removed", "Kept (unsure)", "Duplicate removal"], flow_rows))
    sections.append(("What each arm ran with", ["Arms are only compared when stream settings, present mode and variable refresh state are the same. Switches: what the kit put into the environment of each start; echo: what the program itself logged about them."],
                     ["Arm", "Build", "Switches (kit)", "The program's own echo", "Stream", "Decoder", "Present mode", "Variable refresh", "Frame generation"], ran_rows))
    hold_labels = [("Software latency per press", "[push-latency] trials answerMs=[...]"), ("Displayed frame times, dropped frames", "PresentMon capture"), ("Present mode and tearing", "PresentMon: PresentMode, AllowsTearing"),
                   ("Variable refresh state", "[display-vrr]"), ("GPU load and video memory", "samples.csv: nvidia-smi (whole GPU), Windows GPU counters (the process); [frame-rate] vramMiB"),
                   ("Decoder load", "samples.csv: nvidia-smi decoder, Windows VideoDecode engine; PresentMon VideoBusy"), ("CPU load", "samples.csv"), ("Network health: loss, jitter, late packets, bitrate, route", "[remoteplay-health]"),
                   ("Recovery events", "[remoteplay-recovery]"), ("Duplicate and cadence statistics", "[remoteplay-cadence]"), ("Frames presented: real, generated", "[frame-flow]"),
                   ("Stream settings: resolution, frame rate, codec, requested bitrate", "[remoteplay] session started"), ("Decoder in use", "[remoteplay-decode] actual="), ("Hardware and Windows settings", "hardware.json"),
                   ("Build", "kit.json (file hash); [qml] [update-check] build="), ("Switches", "run.json; [remoteplay-timing], [remoteplay-lan], [remoteplay-settings]")]
    sections.append(("What this evidence holds", ["One row per value a comparison needs: where it comes from, and what this folder has of it for each arm. MISSING says how to get it."],
                     ["Value", "Comes from"] + arms, [[label, source] + [holds[a][i] for a in arms] for i, (label, source) in enumerate(hold_labels)] if arms else []))
    missing = {a: [hold_labels[i][0] for i, text in enumerate(holds[a]) if text.startswith("MISSING")] for a in arms}
    for a in arms:
        if missing[a]: notes.append(f"Arm '{a}' has no record of: " + "; ".join(missing[a]) + ". See 'What this evidence holds'.")

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
                why = comparability(a, facts[a], reference, facts[reference])[0] if a in facts and reference in facts else ""
                c = compare({"presses": values, "runs": [], "count": len(values)}, {"presses": groups[reference], "runs": [], "count": len(groups[reference])}, MIN_CAMERA_PRESSES)
                if why: cells += [f"refused (vs {reference})", "", why]
                else: cells += [f"{c['difference']:+.1f} ms vs {reference}", f"[{c['low']:+.1f}, {c['high']:+.1f}]" if c["low"] == c["low"] else "n/a", c["verdict"]]
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

    soak_runs = [r for r in data["runs"] if r["purpose"] == "soak"]
    if soak_runs:
        import soak
        results = [(run["arm"], soak_result(run)) for run in soak_runs]
        sections.append(("Soak result", ["Counts over the whole run and the verdict. FAIL: a crash, GPU failure, engine hang or abandoned recovery line, the program ending by itself, or a flagged trend. Stopped by the kill-switch is INCOMPLETE, not a failure."],
                         soak.COUNT_HEADER, [soak.count_row(arm, result) for arm, result in results]))
        sections.append(("Soak trends", [
            "One median per minute, the first five minutes left out, a Theil-Sen line through the rest. Flagged only when the change per hour exceeds the threshold and each third of the run is worse than the one before.",
            "The latency rows are processing times inside the program, or the built-in test's software clock when the test was run during the soak: not input to photon."],
            soak.TREND_HEADER, [row for arm, result in results for row in soak.trend_rows(arm, result)]))
        for arm, result in results: notes.append(f"Soak of '{arm}': {result['verdict']}")

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

    # reliability checks
    checked = [r for r in data["runs"] if r["purpose"] in RELIABILITY_PURPOSES]
    if checked:
        import reliability
        rows = [row for run in checked for row in reliability.rows_of(run)]
        sections.append(("Reliability checks", [
            "One line per check, judged from the markers the kit wrote and the program's log between them (rules: docs/RELIABILITY.md). 'The kit's own line' is what run-tests.ps1 printed on the PC for the same step; 'Agree' says whether the two verdicts are the same.",
            "NOT-EXERCISED: the log does not show that the thing was done. SKIPPED: the check does not apply to this run. Nothing here measures latency."],
            reliability.HEADER, rows))
        failed = [row[1] for row in rows if row[2] == "FAIL"]
        notes.append("Reliability checks: " + (f"{len(failed)} FAILED ({', '.join(failed)})" if failed else "none failed") + f"; {sum(1 for row in rows if row[2] == 'PASS')} passed, "
                     f"{sum(1 for row in rows if row[2] in ('NOT-EXERCISED', 'SKIPPED'))} not exercised or skipped.")
        if any(row[5] == "NO" for row in rows): notes.append("The kit's own verdict and this report DISAGREE on: " + ", ".join(row[1] for row in rows if row[5] == "NO") + ". Trust neither until the rules in run-tests.ps1 and reliability.py are compared.")

    # light sensor (input to photon): its own table, never with the software clock
    sensor = read_sensor(os.path.join(data["folder"], "sensor.csv"))
    if sensor["rows"]:
        rows = []
        for a, values in sensor["rows"].items():
            v = sorted(values)
            rows.append([a, str(len(v)), fmt(statistics.median(v)), fmt(percentile(v, .95)) if len(v) >= 20 else "n/a", fmt(mean(v)), fmt(v[0]), fmt(v[-1]), "; ".join(sorted(sensor["devices"].get(a, []))) or "not named"])
        sections.append(("Input to photon (light sensor)", [
            "From sensor.csv (a photodiode or latency tester): milliseconds from the button contact to the light change, as the device reports them. Physical measurements: never compared with the software clock above, and kept apart from the camera table."],
            ["Arm", "Presses", "Median ms", "P95 ms", "Mean ms", "Fastest ms", "Slowest ms", "Device"], rows))
    for problem in sensor["problems"]: notes.append("sensor.csv: " + problem)

    # the one-page result: the same rule as RESULT.txt of the kit (run-tests.ps1 Write-ResultFiles)
    summary = result_summary(data)
    rows = [[r["name"], str(r["presses"]), fmt(r["median"]), f"[{fmt(r['medianLow'])}, {fmt(r['medianHigh'])}]", fmt(r["p95"]), f"[{fmt(r['p95Low'])}, {fmt(r['p95High'])}]",
             fmt(r["p99"]), f"[{fmt(r['p99Low'])}, {fmt(r['p99High'])}]", str(r["starts"]), r["verdict"]] for r in summary["arms"]]
    sections.append(("One-page result (software, not input-to-photon)", [RESULT_RULE,
        "95 % intervals of the quantiles from order statistics (no assumption about the distribution); n/a where the presses are too few for one. The camera and light sensor tables are physical measurements and never enter this table or its verdicts."],
        ["Arm", "Presses", "Median ms", "95 % interval", "P95 ms", "95 % interval", "P99 ms", "95 % interval", "Starts", "Verdict"], rows))
    kit_result = as_dict(read_json(os.path.join(data["folder"], "result.json")))
    if kit_result.get("arms"):
        differ = []
        mine = {r["name"]: r for r in summary["arms"]}
        for theirs in [as_dict(x) for x in as_list(kit_result.get("arms"))]:
            r = mine.get(theirs.get("name"))
            if r is None: differ.append(f"{theirs.get('name')} (not in this report)"); continue
            if int(theirs.get("presses") or 0) != r["presses"]: differ.append(f"{r['name']} presses {theirs.get('presses')} / {r['presses']}")
            elif theirs.get("median") is not None and r["median"] == r["median"] and abs(float(theirs["median"]) - r["median"]) > 0.05: differ.append(f"{r['name']} median {theirs['median']} / {r['median']:.2f}")
        if differ: notes.append("RESULT.txt of the kit and this report DISAGREE on: " + "; ".join(differ) + ". Trust neither until run-tests.ps1 Write-ResultFiles and report.py result_summary are compared.")
        else: notes.append("RESULT.txt of the kit and this report agree on every arm's presses and median.")
    aside = glob.glob(os.path.join(data["folder"], "setaside", "runs", "*", "run.json"))
    if aside: notes.append(f"{len(aside)} run(s) were set aside during the session because their settings differed (folder setaside): they are not in any table.")
    sections.append(("Missing app log fields", ["Numbers that no log line gives today, with the line that should exist. The program was not changed for this; the kit reports what it can without them."],
                     ["Field and the line that should exist"], [[x] for x in MISSING_APP_FIELDS]))

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

def read_sensor(path):
    out = {"rows": {}, "devices": {}, "problems": []}
    try: lines = [l for l in read_lines(path) if l.strip() and not l.lstrip().startswith("#")]
    except OSError: return out
    for index, row in enumerate(csv.DictReader(lines), 1):
        arm = (row.get("arm") or "").strip(); ms = number(row.get("ms"))
        if not arm or ms is None or not 0 < ms < 2000: out["problems"].append(f"row {index} skipped (arm and ms between 0 and 2000 are needed)"); continue
        out["rows"].setdefault(arm, []).append(ms)
        if (row.get("device") or "").strip(): out["devices"].setdefault(arm, set()).add(row["device"].strip()[:40])
    return out

# --- the one-page result (RESULT.txt of the kit uses the same rule) -------------------------------------------
RESULT_MINIMUM = 120
RESULT_RULE = ("A difference is called only when both arms have at least 120 presses with per-press values, the two arms ran with the same settings, and the 95 % "
               "intervals of the two medians do not overlap. This is conservative: a real difference can stay not shown. The tables above (difference of means, "
               "bootstrap interval) can resolve smaller ones.")
MISSING_APP_FIELDS = (
    "performance mode in effect at session start (set in the interface or by slipstream.ini): [remoteplay-settings] mode=ultra-low-latency source=ui|ini|none",
    "frame generation setting at session start, also when it is off: [remoteplay-settings] frameGeneration=off multiplier=1 backend=none",
    "per-frame decode time percentiles without the health switch: [remoteplay-decode] window=5s frames=300 decodeMsP50=1.10 decodeMsP95=1.90 decodeMsMax=3.20",
    "present queue depth at the swap chain: [pacing] queueDepth=1 queueDepthMax=2 window=5s",
    "count of error and warning lines of every family since start (the kit copies only some families): [log-summary] errors=0 warnings=3 since=start",
    "refresh rate and variable refresh state from every GPU maker (display-vrr reads NVIDIA only): [display-refresh] at-start refresh=180.000 Hz vrr=active source=dxgi",
    "stream settings in the original build (3.2.1 logs no session line the kit reads): [remoteplay] session started profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000",
)

def quantile_interval(sorted_values, p):
    """95 % interval of a quantile from order statistics: the values at the 1-based ranks floor(np - 1.96 sqrt(np(1-p)))
    and ceil(np + 1.96 sqrt(np(1-p))) + 1; nan on a side whose rank falls outside the data. As run-tests.ps1."""
    n = len(sorted_values)
    if not n: return float("nan"), float("nan")
    spread = 1.96 * math.sqrt(n * p * (1 - p))
    low_rank, high_rank = math.floor(n * p - spread), math.ceil(n * p + spread) + 1
    return (sorted_values[low_rank - 1] if low_rank >= 1 else float("nan"), sorted_values[high_rank - 1] if high_rank <= n else float("nan"))

def result_summary(data):
    """Per latency arm: presses, median / P95 / P99 with 95 % intervals, refusal and the verdict of RESULT_RULE."""
    latency_runs = [r for r in data["runs"] if r["purpose"] == "latency"]
    names = [n for n in data["table"] if any(r["arm"] == n for r in latency_runs)] + sorted({r["arm"] for r in latency_runs} - set(data["table"]))
    by_arm = {a: [r for r in latency_runs if r["arm"] == a] for a in names}
    facts = {a: arm_facts(by_arm[a]) for a in names}
    arms = []
    for a in names:
        v = sorted(p for r in by_arm[a] for p in r["log"]["presses"])
        row = {"name": a, "compare": (data["table"].get(a) or {}).get("compare") or a, "starts": len(by_arm[a]), "presses": len(v)}
        for key, p in (("median", .5), ("p95", .95), ("p99", .99)):
            row[key] = percentile(v, p); row[key + "Low"], row[key + "High"] = quantile_interval(v, p)
        expected = (data["table"].get(a) or {}).get("fg") or ""
        state = facts[a].get("fg_state")
        row["reasons"] = [f"frame generation was {state}; this arm needs it {expected}"] if expected in ("on", "off") and state and state != expected else []
        arms.append(row)
    by_name = {r["name"]: r for r in arms}
    for r in arms:
        base = by_name.get(r["compare"])
        if base is not None and base is not r:
            why = comparability(r["name"], facts[r["name"]], base["name"], facts[base["name"]])[0]
            if why: r["reasons"].append(why)
        r["refused"] = bool(r["reasons"])
    for r in arms:
        base = by_name.get(r["compare"])
        if base is r: r["verdict"] = "baseline" + (", REFUSED: " + "; ".join(r["reasons"]) if r["refused"] else "")
        elif base is None: r["verdict"] = f"no verdict: its pair '{r['compare']}' was not run"
        elif r["refused"]: r["verdict"] = "REFUSED: " + "; ".join(r["reasons"])
        elif base["refused"]: r["verdict"] = f"REFUSED: its pair '{base['name']}' is refused"
        elif not r["presses"] or not base["presses"]: r["verdict"] = "no verdict: per-press values are not logged by one of the two builds (only test summaries)"
        elif r["presses"] < RESULT_MINIMUM or base["presses"] < RESULT_MINIMUM: r["verdict"] = f"no verdict: fewer than {RESULT_MINIMUM} presses ({r['presses']} and {base['presses']})"
        elif any(x != x for x in (r["medianLow"], r["medianHigh"], base["medianLow"], base["medianHigh"])): r["verdict"] = "no verdict: no interval"
        elif r["medianHigh"] < base["medianLow"]: r["verdict"] = f"LOWER than {base['name']} by {base['median'] - r['median']:.1f} ms (median)"
        elif r["medianLow"] > base["medianHigh"]: r["verdict"] = f"HIGHER than {base['name']} by {r['median'] - base['median']:.1f} ms (median)"
        else: r["verdict"] = f"no difference shown: the 95 % intervals of the medians overlap (vs {base['name']})"
    return {"arms": arms, "rule": RESULT_RULE, "minimum": RESULT_MINIMUM}

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
        summary = result_summary(data)
        def clean(value): return None if isinstance(value, float) and value != value else value
        with open(os.path.join(out, "report-result.json"), "w", encoding="utf-8") as handle:
            json.dump({"kind": "software latency (built-in test), not input-to-photon", "rule": summary["rule"], "minimumPresses": summary["minimum"],
                       "arms": [{k: clean(v) for k, v in r.items()} for r in summary["arms"]]}, handle, indent=1)
        print(f"runs: {len(data['runs'])}  report: {os.path.join(out, 'report.md')}, report.html and report-result.json")
        for note in notes: print("note:", note)
        return 2 if (leaks and args.strict) else 0

if __name__ == "__main__":
    sys.exit(main())
