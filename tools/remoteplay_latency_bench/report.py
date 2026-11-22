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
ALLOWED = re.compile(r"\[(INFO |WARN |ERROR|TRACE)\] \[(push-(latency|quality|benchmark|mode|hardware|profile|adaptive-fg)|remoteplay(-[a-z0-9-]+)?|slipstream-(governor|gpu-budget|watch|config|latency|latency-test|press|delivery|sr|fg|autorun|smartlink|swapchain|present|pipeline|nr|clean|quality|input|input-poll|scanout|cadence|net|smooth|capture)|capture-(driver-age|buffer)|nr-runtime-status|frame-(flow|rate)|pacing|present-deviation|present-contract|qml-window|ui-(dialog|fullscreen|window|resume)|fg-(validity|drops)|display-(vrr|refresh|latency|query)|gpu-(failure|reconnect)|reset-lifecycle|engine-(hang|test)|crash|overlay-hooks|log-backpressure|qml\] \[update-check)\] ")
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
    (re.compile(r"(?<![Vv]ersion=)\b10\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"), "10.x.x.x"),
    (re.compile(r"(?<![Vv]ersion=)\b192\.168\.\d{1,3}\.\d{1,3}\b"), "192.168.x.x"),
    (re.compile(r"(?<![Vv]ersion=)\b172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}\b"), r"172.\1.x.x"),
    (re.compile(r"(?<![Vv]ersion=)\b169\.254\.\d{1,3}\.\d{1,3}\b"), "169.254.x.x"),
    (re.compile(r"(?<![Vv]ersion=)\b127\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"), "127.x.x.x"),
    (re.compile(r"(?<![Vv]ersion=)\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"), "x.x.x.x"),
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
            if not (name.endswith((".json", ".txt")) or name in ("markers.csv", "camera.csv", "reliability.csv") + TRACE_FILES) or name.startswith("report."): continue
            path = os.path.join(root, name)
            if name in TRACE_FILES:
                if trace_problem(path): found.append((os.path.relpath(path, folder), 0, "trace with a field that is not a number"))
                continue
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

FLOW_COUNTERS = ("captureReceived", "sourceAccepted", "realPresented", "generatedPresented", "generatedExpiredAfterEval", "cancelledBeforePresent", "mailboxOverwritten", "fgEvaluated", "fgInvalid")

def parse_log(lines):
    """Everything the report uses from the whitelisted log lines of one run."""
    out = {"presses": [], "summaries": [], "no_answer": 0, "windows": [], "progress": [], "keyframe_requests": [], "reconnects": [],
           "recovery_stopped": 0, "errors": 0, "warnings": 0, "lost_packets_max": 0, "session_start": None, "timing": "", "settings": "",
           "streams": [], "sessions": [], "decoders": [], "health": [], "console_units": None, "route": "", "cadence": [], "flow": {k: 0 for k in FLOW_COUNTERS}, "flow_lines": 0,
           "frame_rate": [], "vrr": [], "display_latency": [], "display_events": [], "pacing": "", "build": "", "gpu_failures": [], "gpu_reconnects": [],
           "crashes": [], "hangs": [], "engine_test": [], "log_dropped": 0, "kbm": [], "quality": [], "starts": [], "echo": [], "ingress": [],
           "fg_backend": "", "previous_run": [], "overlays": [], "first": None, "last": None, "recovery_events": [], "recovery_stopped_at": [], "resume_started": [], "resume_refused": [],
           "config": {}, "config_line": {}, "config_changed": set(), "config_settle": None, "slip_press": [], "slip_tests": [], "slip_latency": [], "window_facts": [], "slip_delivery": [], "slip_sr": [], "slip_fg": [],
           "audio": [], "smartlink": [], "autorun": [], "swapchain": {}, "present": {}, "sr_holds": [], "autorun_finished": None, "frame_rate_fps": [],
           "swapchain_lines": [], "measure_end": None}
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
        # Settle time from the LAST 'session started' before the first presented frame ([frame-rate]): a connection that was
        # retried (the console still held the previous session) is start-up, not a change of setting (kit 5, BENCH2 bug 3).
        if component == "remoteplay" and text.startswith("session started") and not out["frame_rate"] and when is not None:
            out["config_settle"] = when + CONFIG_SETTLE_S; out["config_changed"] = set()
        if out.get("config_settle") is None and when is not None: out["config_settle"] = when + CONFIG_SETTLE_S
        if component not in CONFIG_COMPONENTS: before = None
        else: before = {k: normal_value(k, v) for k, v in out["config"].items()}
        infer_config(out["config"], component, text)
        if before is None: after = None
        elif component == "slipstream-config":
            before.update({k: normal_value(k, v) for k, v in out["config_line"].items()})
            after = dict(before); after.update({k: normal_value(k, v) for k, v in words(text).items()})
        else: after = {k: normal_value(k, v) for k, v in out["config"].items()}
        if before is not None and when is not None and out.get("config_settle") is not None and when > out["config_settle"]:
            for key in TRACKED_KEYS:
                if before.get(key) is not None and after.get(key) is not None and before[key] != after[key]: out["config_changed"].add(key)
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
                if when is not None: out["last_test_done"] = when
                open_window = None
            if "no answer seen" in text: out["no_answer"] += 1
            if text.startswith("cancelled"): open_window = None
        elif component == "slipstream-config": out["config_line"].update(words(text))
        elif component == "slipstream-press": out["slip_press"].append(numbers(text))
        elif component == "slipstream-latency-test": out["slip_tests"].append(numbers(text))
        elif component == "slipstream-latency": out["slip_latency"].append(dict(numbers(text), t=when))
        elif component == "slipstream-delivery": out["slip_delivery"].append(dict(numbers(text), t=when))
        elif component == "slipstream-smartlink":
            entry = numbers(text); w = words(text)
            for key in ("profile", "regime", "mode", "reasons", "wifiStreamingMode"): entry[key] = w.get(key, "")
            entry["t"] = when; out["smartlink"].append(entry)
        elif component == "slipstream-sr":
            if text.startswith("event=hold-end"):
                entry = numbers(text); entry["label"] = words(text).get("label", "?"); out["sr_holds"].append(entry)
            elif not text.startswith("event="):
                entry = numbers(text); entry.update({k: v for k, v in words(text).items() if k in ("in", "out")}); out["slip_sr"].append(entry)
        elif component == "slipstream-swapchain": out["swapchain_lines"].append((when, words(text)))
        elif component == "slipstream-present":
            out["present"].update(words(text))
            if text.startswith("window event="): out["window_facts"].append(words(text))
        elif component == "slipstream-fg":
            entry = numbers(text); entry["reason"] = words(text).get("reason", ""); out["slip_fg"].append(entry)
        elif component == "slipstream-autorun":
            if text.startswith("finished"): out["autorun_finished"] = numbers(text).get("ok"); out["measure_end"] = out["measure_end"] or when
            else: out["autorun"].append(words(text))
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
            # "causes ..." (stream sync: what opened each wait for a keyframe) is a count, not a recovery event
            if "retry allowance renewed" not in text and not text.startswith("causes "): out["recovery_events"].append(when)
            m4 = re.match(r"device-loss resume attempt=(\d+) started", text)
            if m4: out["resume_started"].append((when, int(m4.group(1))))
            m4 = re.match(r"device-loss not resumed reason=([a-z-]+)", text)
            if m4: out["resume_refused"].append((when, m4.group(1)))
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
            elif text.startswith("consoleUnits "):
                # once per session (ConsoleQuality.h takeLogLine): "consoleUnits rtt=ms|s|undecided rate=bit/s|kbit/s|Mbit/s|undecided ..."
                m = re.match(r"consoleUnits rtt=(\S+) rate=(\S+)", text)
                if m: out["console_units"] = {"rtt": m.group(1), "rate": m.group(2)}
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
        elif component == "remoteplay-audio":
            if "underruns=" in text or "pops=" in text: out["audio"].append(dict(numbers(text), t=when))
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
            fps = numbers(text).get("presentSubmitFps")
            if fps is not None: out["frame_rate_fps"].append(fps)
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
    out["swapchain"] = swapchain_at_measurement(out)
    return out

def swapchain_at_measurement(log):
    """[slipstream-swapchain] words as they stood during the measurement (kit 5, BENCH2 bug 2): every line up to the end of
    the measurement merged in order, later lines ignored (the app writes one at exit, windowed again). End of the
    measurement: the last '[push-latency] done', else '[slipstream-autorun] finished', else the end of the log."""
    end = log.get("last_test_done") or log.get("measure_end")
    state = {}
    for when, w in log["swapchain_lines"]:
        if end is not None and when is not None and when > end: continue
        state.update(w)
    return state

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
        purpose = meta.get("purpose") or next((p for p in ("soak", "impairment", "camera", "quality", "probe", "network", "measure") + RELIABILITY_PURPOSES if label.startswith(p + "-")), "latency")
        arm = meta.get("arm") or (label[len(purpose) + 1:] if purpose != "latency" else label)
        raw = read_lines(path)
        log = parse_log(raw)
        run = {"name": name, "arm": arm, "purpose": purpose, "meta": meta, "log": log, "folder": run_folder, "lines": raw,
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
            "refresh": sorted({str(int(round(state["refresh"]))) for l in logs for _t, state in l["vrr"][-1:] if state["refresh"]}),
            "config": arm_config(runs)[0], "config_differ": arm_config(runs)[1], "allowed": (),
            "profiled": bool(runs) and all(r["meta"].get("testProfile") for r in runs),
            "method": test_method(runs)}

def test_method(runs):
    """The plan of the latency test: presses per test (run.json trialsAsked); '' when not recorded. Started by the automation
    or from the menu it is the same in-app test, so only the plan counts (run-tests.ps1 the same)."""
    kinds = sorted({f"{r['meta'].get('trialsAsked')} presses per test" for r in runs if r["meta"].get("trialsAsked")})
    return " / ".join(kinds)

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
    # What the arm is expected to change against its pair (arms.csv column differs): going full screen changes how
    # Windows presents the picture (presentMode) and may turn variable refresh on (vrrActive). run-tests.ps1 the same.
    allowed = tuple(facts.get("allowed") or ())
    if facts["streams"] and base["streams"]:
        if facts["streams"] != base["streams"]:
            return f"REFUSED: stream settings differ ({name}: {stream_text(facts['streams'])}; {base_name}: {stream_text(base['streams'])})", []
    else: unchecked.append("stream settings")
    if facts["mode"] and base["mode"]:
        if (facts["mode"], facts["tearing"]) != (base["mode"], base["tearing"]) and "presentMode" not in allowed:
            return f"REFUSED: present mode differs ({name}: {mode_text(facts)}; {base_name}: {mode_text(base)})", []
    else: unchecked.append("present mode")
    if facts["vrr"] and base["vrr"]:
        if facts["vrr"] != base["vrr"] and "vrrActive" not in allowed:
            return f"REFUSED: variable refresh state differs ({name}: active={facts['vrr'][0]}; {base_name}: active={base['vrr'][0]})", []
    else: unchecked.append("variable refresh")
    for who, f in ((name, facts), (base_name, base)):
        if f.get("config_differ"): return f"REFUSED: the runs of '{who}' differ in {', '.join(f['config_differ'])}", []
    profiled = bool(facts.get("profiled") and base.get("profiled"))
    mine_c, theirs_c, assumed = settings_that_matter(facts.get("config") or {}, base.get("config") or {}, profiled)
    differ, one_side = config_refusal(name, mine_c, base_name, theirs_c, tuple(facts.get("allowed") or ()))
    if differ:
        return f"REFUSED: the two arms ran with different settings: {'; '.join(differ)}. Fix: {config_fix(differ[0])}", []
    blind = [k for k in one_side if k in LATENCY_KEYS and not (profiled and k in PROFILE_KEYS)]
    if blind:
        return (f"REFUSED: not comparable: NOT CHECKED, logged by one side only: {', '.join(blind)}. These settings change the latency; "
                f"the build that does not log them cannot show that they were the same"), []
    if facts.get("method") and base.get("method") and facts["method"] != base["method"]:
        return f"REFUSED: the two arms used a different test method ({name}: {facts['method']}; {base_name}: {base['method']}): latency comparison refused", []
    return "", unchecked + [k for k in one_side if k not in LATENCY_KEYS] + [f"{k} assumed from the test profile" for k in assumed]

def same_conditions(unchecked):
    checked = [x for x in ("stream settings", "present mode", "variable refresh") if x not in unchecked]
    text = ("same " + ", ".join(checked)) if checked else ""
    if unchecked: text += ("; " if text else "") + "NOT CHECKED (not recorded on one side): " + ", ".join(unchecked)
    return text

# --- the configuration of a run, and which arms may be compared (sprint 4) ------------------------------------
# Every key a run's log shows. A build with the [slipstream-config] line gives its keys directly; older builds (the
# original and the stable candidate) give what can be inferred from existing lines. The kit (run-tests.ps1
# Get-RunConfig) reads the same lines into the same key names: change both together.
WIFI_LABEL = "Wi-Fi: diagnostic only, not a wired-LAN latency conclusion"
# Keys no build logs before [slipstream-config]: for an original or stable build these are not compared (named "not checked").
NOT_INFERRED = ("mode", "fgMethod", "fgMultiplier", "fgTier", "sr", "srQuality", "enhance", "nvLowLatency", "smoothPacing", "fullscreen", "window", "output", "hdr", "trialsMax")
# Keys that describe the network or the session's own measurement, never a setting: not compared.
CONFIG_IGNORED = ("route", "trialsMax")
CONFIG_FIX = {
    "lowQueue": "the low-latency queue differs: let the kit start every copy with its test profile (do not use -NoTestProfile), or set it the same in both copies",
    "displaySync": "display sync differs: use the kit's test profile, or set display sync the same in both copies",
    "vrrRequested": "the variable refresh request differs: use the kit's test profile, or set smooth frame pacing the same in both copies",
    "vrrActive": "variable refresh was active in one arm only: run both full screen on the same display (the test profile turns full screen on at connect)",
    "presentWaitable": "the present queue differs (DXGI waitable): use the kit's test profile in both copies",
    "presentMaxLatency": "the DXGI frame latency differs: use the kit's test profile in both copies",
    "presentVsync": "V-Sync differs: use the kit's test profile in both copies",
    "presentTearing": "tearing differs: use the kit's test profile in both copies",
    "stream": "the stream resolution or frame rate differs: choose the same quality in the PS5 connect dialog of both copies",
    "codec": "the codec differs: choose the same codec in the PS5 connect dialog of both copies",
    "bitrateKbps": "the bitrate differs: choose the same bitrate in the PS5 connect dialog of both copies",
    "decoder": "the decoder differs: choose the same decoder in the PS5 connect dialog of both copies",
    "refreshHz": "the refresh rate differs: keep the display at the same refresh rate for every run",
    "fg": "frame generation differs: turn it off (or on) in both copies before connecting",
    "sr": "super resolution differs: set it the same in both copies",
    "enhance": "picture enhancement differs: set it the same in both copies",
}

def _flag(value):
    v = str(value).strip().lower()
    return "1" if v in ("1", "true", "on", "yes") else "0" if v in ("0", "false", "off", "no") else v

def _codec(value):
    v = str(value).strip().lower()
    return {"h265": "hevc", "hevc": "hevc", "h265_hdr": "hevc", "h264": "h264", "avc": "h264"}.get(v, v)

def infer_config(config, component, text):
    """Updates the inferred configuration (last value of each key wins) from one log line."""
    if component == "pacing":
        m = re.match(r"revision=\d+ requested=(\w+)/(\d+)/(\d+)", text)
        if m:
            config["lowQueue"], config["pacingMode"], config["displaySync"] = _flag(m.group(1)), m.group(2), m.group(3)
            w = words(text)
            if "variableRefresh" in w: config["vrrRequested"] = _flag(w["variableRefresh"])
            if "displayQueuePaced" in w: config["displayQueuePaced"] = _flag(w["displayQueuePaced"])
        elif text.startswith("DXGI "):
            w = words(text)
            for key, name in (("enabled", "presentWaitable"), ("maximumLatency", "presentMaxLatency"), ("vsync", "presentVsync"), ("tearing", "presentTearing")):
                if key in w: config[name] = _flag(w[key]) if key != "maximumLatency" else w[key]
    elif component == "display-vrr":
        w = words(text)
        if "active" in w: config["vrrActive"] = _flag(w["active"])
        if "level" in w: config["vrrLevel"] = w["level"]
        hz = number(w.get("refreshHz"))
        if hz: config["refreshHz"] = str(int(round(hz)))
    elif component == "display-query" and text.startswith("result ") and "hdr" in words(text):
        # The monitor's HDR mode (every build logs it; the experimental build's hdr= is the stream's, a different thing).
        config["displayHdr"] = _flag(words(text)["hdr"])
    elif component == "display-refresh" and text.startswith("monitor "):
        hz = numbers(text).get("refresh")
        if hz: config["refreshHz"] = str(int(round(hz)))
    elif component == "remoteplay" and text.startswith("session started"):
        w = words(text)
        if w.get("profile"): config["stream"] = w["profile"]
        if w.get("codec"):
            config["codec"] = _codec(w["codec"])
            config["hdr"] = "1" if w["codec"].lower() == "h265_hdr" else "0"     # the stream's HDR request, as hdr= of [slipstream-config]
        if w.get("requestedBitrateKbps"): config["bitrateKbps"] = w["requestedBitrateKbps"]
    elif component == "remoteplay-decode" and text.startswith("actual="):
        config["decoder"] = "software" if words(text).get("fallback") == "true" else words(text).get("actual", "").lower()
    elif component == "ui-fullscreen" and text.startswith("enabled="):
        config["fullscreen"] = _flag(words(text)["enabled"])     # logged by the stable build's interface too: verifies the profile
    elif component == "frame-flow":
        generated = numbers(text).get("generatedPresented")
        if generated is not None and (generated > 0 or "fg" not in config): config["fg"] = "1" if generated > 0 else "0"
    elif component == "fg-validity":
        valid = numbers(text).get("valid")
        if valid: config["fg"] = "1"

HARDWARE_DECODERS = ("hardware", "d3d12va", "d3d11va", "dxva2", "dxva", "nvdec", "cuda", "qsv", "vulkan", "amf", "videotoolbox", "mediafoundation")
SOFTWARE_DECODERS = ("software", "software-fallback", "ffmpeg", "libavcodec", "cpu")

def normal_value(key, value):
    """One configuration value in the form both kinds of build agree on; None for unknown (-1, 'unknown', empty).
    The same rules as run-tests.ps1 ConvertTo-ConfigValue."""
    v = str(value).strip()
    if v.lower() in ("", "unknown", "-1", "-1.0", "-1.00", "n/a", "none") and key not in ("mode",): return None
    if key in ("fg", "enhance", "lowQueue", "smoothPacing", "vrrRequested", "vrrActive", "fullscreen", "hdr", "displayHdr", "presentWaitable", "presentVsync", "presentTearing", "displayQueuePaced"): return _flag(v)
    if key == "codec": return _codec(v)
    if key == "decoder":
        low = v.lower()
        return "hardware" if low in HARDWARE_DECODERS else "software" if low in SOFTWARE_DECODERS else low
    if key == "refreshHz":
        hz = number(v)
        return str(int(round(hz))) if hz and hz > 0 else None
    if key == "stream":
        m = re.match(r"^(\d+)x(\d+)@([\d.]+)$", v)
        if m:
            fps = float(m.group(3))
            return f"{m.group(1)}x{m.group(2)}@" + (str(int(round(fps))) if abs(fps - round(fps)) < 0.01 else f"{fps:.2f}")
    return v

def normal_config(log):
    """One run: inferred keys, overlaid by the [slipstream-config] line's own known keys, all normalised (unknown values are skipped)."""
    config = {}
    for source in (log.get("config") or {}, log.get("config_line") or {}):
        for key, value in source.items():
            v = normal_value(key, value)
            if v is not None: config[key] = v     # an explicit unknown (refreshHz=-1) never erases a value inferred from other lines
    return config

def arm_config(runs):
    """{key: value} of an arm, and the keys whose value differs between its runs."""
    merged, differ = {}, set()
    for run in runs:
        for key in run["log"].get("config_changed") or ():
            differ.add(key + " (changed during run " + run["name"] + ")")
        for key, value in normal_config(run["log"]).items():
            if key in merged and merged[key] != value: differ.add(key)
            merged.setdefault(key, value)
    return merged, sorted(differ)

CONFIG_COMPONENTS = ("pacing", "display-query", "display-vrr", "display-refresh", "remoteplay", "remoteplay-decode", "frame-flow", "fg-validity", "slipstream-config", "ui-fullscreen")
CONFIG_SETTLE_S = 10.0   # changes in the first 10 s after the session starts are its start-up, not a change of setting
# Settings that change what the latency or the picture timing can be: logged by one side only -> not comparable.
LATENCY_KEYS = ("lowQueue", "nvLowLatency", "displaySync", "smoothPacing", "vrrRequested", "fullscreen", "fg", "fgMethod", "fgMultiplier", "sr", "srQuality",
                "enhance", "hdr", "presentWaitable", "presentMaxLatency", "presentVsync", "presentTearing", "stream", "decoder")
TRACKED_KEYS = LATENCY_KEYS + ("codec", "bitrateKbps")
# Set by the kit's test profile in every copy (Write-TestProfile): equal when both arms ran with it.
PROFILE_KEYS = ("nvLowLatency", "smoothPacing")
# Settings a build may not log, taken as the test profile sets them when both arms ran with it and the side that logs
# them shows that value: full screen on connect (written by the kit), super resolution and enhancement off (a fresh
# settings folder starts with both off). Named "assumed from the test profile" in the verdict. run-tests.ps1 $assumedKeys.
ASSUMED_KEYS = {"fullscreen": "1", "sr": "0", "enhance": "0"}
FG_DETAIL_KEYS = ("fgMethod", "fgMultiplier", "fgTier")      # cannot matter when frame generation is off on both sides

def settings_that_matter(mine, theirs, profiled):
    """(mine, theirs, assumed keys): fills ASSUMED_KEYS on the silent side (profiled only), drops the frame generation
    details when fg is 0 on both sides and srQuality when sr is 0 on both sides."""
    mine, theirs, assumed = dict(mine), dict(theirs), []
    if profiled:
        for key, value in ASSUMED_KEYS.items():
            for a, b in ((mine, theirs), (theirs, mine)):
                if key not in a and b.get(key) == value: a[key] = value; assumed.append(key)
    fg_off, sr_off = mine.get("fg") == "0" and theirs.get("fg") == "0", mine.get("sr") == "0" and theirs.get("sr") == "0"
    for a in (mine, theirs):
        if fg_off:
            for key in FG_DETAIL_KEYS: a.pop(key, None)
        if sr_off: a.pop("srQuality", None)
    return mine, theirs, sorted(set(assumed))

def config_refusal(name, mine, base_name, theirs, allowed=()):
    """(differing keys with their values, keys known on one side only) between two arms; allowed = keys the switch
    under test is expected to change (arms.csv column differs)."""
    differ, unchecked = [], []
    for key in sorted(set(mine) | set(theirs)):
        if key in CONFIG_IGNORED or key in allowed: continue
        if key in mine and key in theirs:
            if mine[key] != theirs[key]: differ.append(f"{key} {mine[key]} in {name}, {theirs[key]} in {base_name}")
        else: unchecked.append(key)
    return differ, unchecked

def config_fix(differ_text):
    key = differ_text.split(" ", 1)[0]
    return CONFIG_FIX.get(key, f"{key} differs: set it the same in both arms")

def network_route(data):
    """'wifi', 'wired' or 'unknown': [slipstream-config] route=, the network health or hardware lines, then hardware.json's
    default-route adapter."""
    for run in data["runs"]:
        route = (run["log"].get("config_line") or {}).get("route")
        if route in ("wifi", "wired"): return route
    for run in data["runs"]:
        for entry in run["log"].get("health") or []:
            if entry.get("link") == "wifi": return "wifi"
            if entry.get("link") == "ethernet": return "wired"
    for adapter in [as_dict(x) for x in as_list(as_dict(data.get("hardware")).get("network"))]:
        if adapter.get("defaultRoute"):
            text = f"{adapter.get('media') or ''} {adapter.get('description') or ''}"
            return "wifi" if re.search(r"802\.11|wi-?fi|wireless|wlan", text, re.I) else "wired"
    return "unknown"

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
# Plain words for the app's stop reasons: the same table as run-tests.ps1 $stopReasonText.
STOP_REASONS = {
    "no-console": "PushFPS found no console to connect to (is the PS5 switched on or in rest mode, and paired with this copy?)",
    "stream-lost": "the stream ended while the plan ran (a disconnect, a reconnect or Stop)",
    "cancelled": "the latency test was cancelled in PushFPS",
    "invalid-plan": "this build did not understand the kit's plan (a kit and build mismatch: send the zip as it is)",
    "picture-moving": "the picture moved too much for the PS-button test: stand still in a calm place, or open the PS5 menu, and run it again",
    "no-capture": "PushFPS found no capture card remembered in the kit's capture settings folder (the kit asks you to choose it once before the capture session)",
}

VERDICT_WORD = re.compile(r"baseline, REFUSED|baseline|REFUSED|LOWER|HIGHER|better|WORSE|no difference shown|cannot tell|indication only|not comparable|no verdict|descriptive")

def verdict_word(text):
    """The deciding word of a verdict (for the cross-check of the kit's RESULT with this report)."""
    m = VERDICT_WORD.search(str(text or "").replace(WIFI_LABEL, ""))
    return m.group(0) if m else ""

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
    for a in arms: facts[a]["allowed"] = tuple(x for x in ((data["table"].get(a) or {}).get("differs") or "").split(";") if x)
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
            elif "verdict" in theirs and verdict_word(theirs.get("verdict")) != verdict_word(r.get("verdict")): differ.append(f"{r['name']} verdict '{verdict_word(theirs.get('verdict'))}' / '{verdict_word(r.get('verdict'))}'")
            elif "refused" in theirs and bool(theirs.get("refused")) != bool(r.get("refused")): differ.append(f"{r['name']} refused {theirs.get('refused')} / {r.get('refused')}")
        if differ: notes.append("RESULT.txt of the kit and this report DISAGREE on: " + "; ".join(differ) + ". Trust neither until run-tests.ps1 Write-ResultFiles and report.py result_summary are compared.")
        else: notes.append("RESULT.txt of the kit and this report agree on every arm's presses, median, verdict word and refusal.")
    tables = precision_tables(data, summary)
    sections += precision_sections(tables)
    net = network_tables(data)
    sections += network_sections(net)
    kind = str(data["kit"].get("session") or "")            # kit.json: the session the kit ran
    if kind in ("presentation", "audio"):
        sections.append((f"Short session: {kind}", short_session_paragraphs(data, kind)[1:], [], []))
    measured = measured_tables(data)
    sections.append(("Measured values (kit version 5)", measured_paragraphs(data, measured)[1:], [], []))
    kit_measured = as_dict(kit_result.get("measured"))
    if kit_measured:
        differ = measured_disagreement(measured_json(data, measured), kit_measured)
        if differ: notes.append("The kit's MEASURED VALUES and this report DISAGREE on: " + "; ".join(differ[:8]) + ". Trust neither until run-tests.ps1 Get-MeasuredTables and report.py measured_tables are compared.")
        else: notes.append("The kit's MEASURED VALUES and this report agree (every value, its count, the visual verdict words, the neural runtime lines and the button notes).")
    kit_net = as_dict(kit_result.get("networkSession"))
    if net and kit_net:
        differ = []
        for name, theirs in as_dict(kit_net.get("arms")).items():
            mine = net["arms"].get(name)
            if mine is None: differ.append(f"network {name} (not in this report)"); continue
            for key, value in as_dict(as_dict(theirs).get("metrics")).items():
                m = mine["metrics"].get(key)
                value = as_dict(value)
                if m is None: continue
                if int(value.get("n") or 0) != m["n"]: differ.append(f"network {name} {key} n {value.get('n')} / {m['n']}")
                elif value.get("mean") is not None and m["mean"] == m["mean"] and abs(float(value["mean"]) - m["mean"]) > 0.011: differ.append(f"network {name} {key} mean {value['mean']} / {m['mean']:.2f}")
            mine_v = {v["metric"]: verdict_word(v["verdict"]) for v in mine["verdicts"]}
            for v in [as_dict(x) for x in as_list(as_dict(theirs).get("verdicts"))]:
                if v.get("metric") in mine_v and verdict_word(v.get("verdict")) != mine_v[v["metric"]]: differ.append(f"network {name} {v['metric']} verdict '{verdict_word(v.get('verdict'))}' / '{mine_v[v['metric']]}'")
            tf = as_dict(as_dict(theirs).get("facts"))
            for key in ("motion", "motionBasis"):
                if key in tf and tf.get(key) != mine["facts"].get(key): differ.append(f"network {name} {key} {tf.get(key)} / {mine['facts'].get(key)}")
        if differ: notes.append("The kit's network session and this report DISAGREE on: " + "; ".join(differ[:8]) + ".")
        else: notes.append("The kit's network session and this report agree (windows, means and verdict words of every metric, motion).")
    import presentmon
    rows = []
    for a in [r["name"] for r in summary["arms"]]:
        captures = [presentmon.summary(path) for r in data["runs"] if r["purpose"] == "latency" and r["arm"] == a for path in sorted(glob.glob(os.path.join(r["folder"], "presentmon*.csv"))) if not path.endswith("presentmon-windows.csv")]
        m = presentmon.merge(captures)
        if m: rows.append([a, str(m["captures"]), str(m["frames"]), fmt(m["independentPercent"], 1), fmt(m["composedPercent"], 1), fmt(m["tearingPercent"], 1), fmt(m["droppedPercent"], 2),
                           f"{fmt(m['frameP50'], 2)} / {fmt(m['frameP95'], 2)} / {fmt(m['frameP99'], 2)}", f"{fmt(m['shownP95'], 2)} / {fmt(m['shownP99'], 2)}", f"{fmt(m['untilP50'], 2)} / {fmt(m['untilP95'], 2)}", f"{fmt(m['frameToDisplayP50'], 2)} / {fmt(m['frameToDisplayP95'], 2)}"])
        else: rows.append([a, "0", "n/a", "n/a", "n/a", "n/a", "n/a", "n/a", "n/a", "n/a", "no PresentMon capture (PresentMon-*-x64.exe beside the kit, see START-HERE)"])
    sections.append(("PresentMon per arm (presentmon.py)", ([WIFI_LABEL] if tables["route"] == "wifi" else []) + [
        "From PresentMon's own CSV (GameTechDev/PresentMon, MIT licence; not shipped with the kit), the busiest swap chain of PushFPS, whole run. Independent = hardware independent flip, hardware composed independent flip or legacy flip; composed = DWM composition. Frame time = MsBetweenPresents, displayed interval = MsBetweenDisplayChange, present to display = MsUntilDisplayed; DisplayLatency (frame start to display) is shown apart (2.x names; 1.x names are read too)."],
        ["Arm", "Captures", "Frames", "Independent flip %", "Composed %", "Tearing %", "Dropped %", "Frame time P50 / P95 / P99 ms", "Displayed interval P95 / P99 ms", "Present to display P50 / P95 ms (MsUntilDisplayed)", "Frame start to display P50 / P95 ms (DisplayLatency)"], rows))
    if tables["route"] == "wifi": notes.insert(0, WIFI_LABEL + ": the default route of this PC was Wi-Fi in this session; every table and verdict is about that network.")
    kit_tables = as_dict(kit_result.get("tables"))
    if kit_tables:
        differ = []
        for key in ("pipeline", "console", "total"):
            for name, theirs in as_dict(kit_tables.get(key)).items():
                mine = tables.get(key, {}).get(name)
                theirs = as_dict(theirs)
                if mine is None: differ.append(f"{key} {name} (not in this report)"); continue
                if int(theirs.get("n") or 0) != mine["n"]: differ.append(f"{key} {name} n {theirs.get('n')} / {mine['n']}")
                for q in ("p50", "p50Low", "p50High"):
                    if theirs.get(q) is not None and mine[q] == mine[q] and abs(float(theirs[q]) - mine[q]) > 0.011: differ.append(f"{key} {name} {q} {theirs[q]} / {mine[q]:.2f}")
        if differ: notes.append("The kit's precision tables and this report DISAGREE on: " + "; ".join(differ[:8]) + ".")
        else: notes.append("The kit's precision tables and this report agree (values, P50 and its bootstrap interval).")
    aside = glob.glob(os.path.join(data["folder"], "setaside", "runs", "*", "run.json"))
    if aside: notes.append(f"{len(aside)} run(s) were set aside during the session because their settings differed (folder setaside): they are not in any table.")
    sections.append(("Missing app log fields", ["Numbers that no log line gives today, with the line that should exist. The program was not changed for this; the kit reports what it can without them."],
                     ["Field and the line that should exist"], [[x] for x in MISSING_APP_FIELDS]))

    # Wi-Fi: every table carries the label, and so does every verdict cell (review of 16a2bfec, item 6).
    if network_route(data) == "wifi":
        labelled = []
        for heading, paragraphs, header, rows in sections:
            if not paragraphs or paragraphs[0] != WIFI_LABEL: paragraphs = [WIFI_LABEL] + list(paragraphs)
            verdict_columns = [i for i, h in enumerate(header) if "verdict" in str(h).lower()]
            rows = [[(WIFI_LABEL + "; " + str(c)) if i in verdict_columns and str(c) and not str(c).startswith(WIFI_LABEL) else c for i, c in enumerate(row)] for row in rows]
            labelled.append((heading, paragraphs, header, rows))
        sections = labelled
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
BUTTON_TEST_NOTE = ("The PS-button test times the console's own menu answer (about half a second, about 26 ms apart from press to press): it cannot "
                    "resolve a few milliseconds. The in-app split ([slipstream-latency], the PC pipeline table) and PresentMon can.")
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
    for a in names: facts[a]["allowed"] = tuple(x for x in ((data["table"].get(a) or {}).get("differs") or "").split(";") if x)
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
            if why: r["reasons"].append(why[len("REFUSED: "):] if why.startswith("REFUSED: ") else why)
        r["refused"] = bool(r["reasons"])
        r["assumed"] = []
        if base is not None and base is not r:
            f, b = facts[r["name"]], facts[base["name"]]
            r["assumed"] = settings_that_matter(f.get("config") or {}, b.get("config") or {}, bool(f.get("profiled") and b.get("profiled")))[2]
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
        if not r["refused"] and r["assumed"]: r["verdict"] += "; compared between the builds with " + ", ".join(r["assumed"]) + " assumed from the test profile (not logged by one build)"
    route = network_route(data)
    if route == "wifi":
        for r in arms: r["verdict"] = WIFI_LABEL + "; " + r["verdict"]
    return {"arms": arms, "rule": RESULT_RULE, "minimum": RESULT_MINIMUM, "route": route}

# --- precision tables (sprint 4): the same definitions as run-tests.ps1 Write-ResultFiles -------------------------
# Quantiles: linear interpolation between ranks (percentile). Bootstrap: the values sorted ascending, BOOT_ROUNDS
# resamples drawn with the Park-Miller generator x = 16807 x mod (2^31 - 1) from BOOT_SEED (index = x mod n), so that
# PowerShell and Python give the same intervals to the last digit; the 95 % interval is the 2.5 and 97.5 percentiles
# of the resampled quantile. Smallest detectable difference = what the verdict rule needs: the two intervals must not
# overlap, so the sum of the two half-widths.
BOOT_SEED, BOOT_ROUNDS, PM_MULT, PM_MOD = 807, 1000, 16807, 2147483647
TABLE_MINIMUM = 120          # values per arm before a verdict in a latency table
DELIVERY_MINIMUM = 300       # seconds per arm before a frame delivery verdict
QUANTILES = (("p50", .5), ("p95", .95), ("p99", .99))

def bootstrap_quantiles(values, rounds=BOOT_ROUNDS, seed=BOOT_SEED):
    """{p50: (low, high), p95: ..., p99: ...} of the values (nan when fewer than 2)."""
    v = sorted(values); n = len(v)
    if n < 2: return {k: (float("nan"), float("nan")) for k, _p in QUANTILES}
    x, draws = seed, {k: [] for k, _p in QUANTILES}
    for _ in range(rounds):
        sample = []
        for _i in range(n):
            x = (x * PM_MULT) % PM_MOD
            sample.append(v[x % n])
        sample.sort()
        for k, p in QUANTILES: draws[k].append(percentile(sample, p))
    return {k: (percentile(draws[k], .025), percentile(draws[k], .975)) for k, _p in QUANTILES}

def latency_row(values):
    v = sorted(values)
    row = {"n": len(v)}
    intervals = bootstrap_quantiles(v)
    for k, p in QUANTILES:
        row[k] = percentile(v, p); row[k + "Low"], row[k + "High"] = intervals[k]
    return row

def mdd(width_a, width_b):
    """Smallest difference the verdict rule can call: the two 95 % intervals must not overlap, so the difference must
    exceed the sum of the two half-widths (review of 16a2bfec, item 8)."""
    if width_a != width_a or width_b != width_b: return float("nan")
    return (width_a + width_b) / 2.0

# The smallest P50 difference worth finding per press table (ms; run-tests.ps1 $latencyEffect, the procedure's table):
# in-app pipeline 1 ms (as the network session's P95), console + network 2 ms, PS-button total 8.3 ms (half a console
# frame at 60 FPS: the test's answers come in 16.7 ms steps). Overlapping intervals whose MDD is larger say "cannot tell".
LATENCY_EFFECT = {"pipeline": 1.0, "console": 2.0, "total": 8.3}
PRESSES_CAP = 2000

def latency_verdict(row, base, base_name, minimum=TABLE_MINIMUM, key="total"):
    if not row["n"] or not base["n"]: return "no verdict: no values in one of the two arms"
    if row["n"] < minimum or base["n"] < minimum: return f"no verdict: fewer than {minimum} values ({row['n']} and {base['n']})"
    if any(x != x for x in (row["p50Low"], row["p50High"], base["p50Low"], base["p50High"])): return "no verdict: no interval"
    if row["p50High"] < base["p50Low"]: return f"LOWER than {base_name} by {base['p50'] - row['p50']:.2f} ms (P50)"
    if row["p50Low"] > base["p50High"]: return f"HIGHER than {base_name} by {row['p50'] - base['p50']:.2f} ms (P50)"
    effect = LATENCY_EFFECT[key]
    need = mdd(row["p50High"] - row["p50Low"], base["p50High"] - base["p50Low"])
    if need <= effect: return f"no difference shown: the 95 % intervals of the P50 overlap and could have shown {need:.2f} ms, below {effect:.1f} ms (vs {base_name})"
    n = min(row["n"], base["n"])
    m = int(math.ceil(n * (need / effect) ** 2 / 12.0)) * 12          # the MDD shrinks with the square root of the presses
    more = f"more than {PRESSES_CAP}" if m > PRESSES_CAP else f"about {m}"
    return f"cannot tell with {n} presses per arm: this session would need {more} presses per arm to show a difference of {effect:.1f} ms (vs {base_name})"

def wilson(k, n):
    if not n: return float("nan"), float("nan")
    z, p = 1.96, k / n
    centre = (p + z * z / (2 * n)) / (1 + z * z / n); half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / (1 + z * z / n)
    return 100.0 * (centre - half), 100.0 * (centre + half)

def counter_total(values):
    """A counter logged per window or as a running total: the last value when the values never fall, else the sum."""
    values = [v for v in values if v is not None]
    if not values: return None
    return values[-1] if all(b >= a for a, b in zip(values, values[1:])) and len(values) > 1 else sum(values)

DELIVERY_MINIMUM_RUNS = 3

def run_block_interval(blocks, rounds=BOOT_ROUNDS, seed=BOOT_SEED):
    """95 % interval of the pooled percentage sum(k) / sum(n) when whole runs are resampled (consecutive seconds of one
    run are not independent). blocks: [(k, n)] in run order, sorted by (k, n) first. nan with fewer than 3 runs."""
    b = sorted(blocks)
    if len(b) < DELIVERY_MINIMUM_RUNS: return float("nan"), float("nan")
    x, draws = seed, []
    for _ in range(rounds):
        k = n = 0
        for _i in range(len(b)):
            x = (x * PM_MULT) % PM_MOD; k += b[x % len(b)][0]; n += b[x % len(b)][1]
        draws.append(100.0 * k / n if n else float("nan"))
    draws = [d for d in draws if d == d]
    return percentile(draws, .025), percentile(draws, .975)

def delivery_row(runs):
    seconds = under = 0
    blocks = []
    for run in runs:
        fps = run["log"]["frame_rate_fps"][3:-2]
        seconds += len(fps); under += sum(1 for f in fps if f < 57)
        if fps: blocks.append((sum(1 for f in fps if f < 57), len(fps)))
    delivery = [d for r in runs for d in r["log"]["slip_delivery"]]
    arrived = sum(int(d.get("arrived") or 0) for d in delivery); replaced = sum(int(d.get("replaced") or 0) for d in delivery)
    if arrived: replaced_percent, replaced_from = 100.0 * replaced / arrived, "slipstream-delivery"
    else:
        flow = {k: sum(r["log"]["flow"][k] for r in runs) for k in ("captureReceived", "mailboxOverwritten")}
        replaced_percent = 100.0 * flow["mailboxOverwritten"] / flow["captureReceived"] if flow["captureReceived"] else float("nan")
        replaced_from = "frame-flow" if flow["captureReceived"] else ""
    gaps = [d["gapP95"] for d in delivery if d.get("gapP95") is not None]
    low, high = run_block_interval(blocks)
    return {"seconds": seconds, "under57": under, "under57Percent": 100.0 * under / seconds if seconds else float("nan"), "under57Low": low, "under57High": high,
            "replacedPercent": replaced_percent, "replacedFrom": replaced_from, "gapP95": statistics.median(gaps) if gaps else float("nan"),
            "clumps": sum(int(d.get("clumps") or 0) for d in delivery) if delivery else None, "longGaps": sum(int(d.get("longGaps") or 0) for d in delivery) if delivery else None,
            "runs": len(blocks)}

def delivery_verdict(row, base, base_name):
    if row["seconds"] < DELIVERY_MINIMUM or base["seconds"] < DELIVERY_MINIMUM: return f"no verdict: fewer than {DELIVERY_MINIMUM} seconds ({row['seconds']} and {base['seconds']})"
    if row["runs"] < DELIVERY_MINIMUM_RUNS or base["runs"] < DELIVERY_MINIMUM_RUNS: return f"no verdict: fewer than {DELIVERY_MINIMUM_RUNS} runs ({row['runs']} and {base['runs']})"
    if row["under57High"] < base["under57Low"]: return f"FEWER short seconds than {base_name} ({row['under57Percent']:.1f} % vs {base['under57Percent']:.1f} %)"
    if row["under57Low"] > base["under57High"]: return f"MORE short seconds than {base_name} ({row['under57Percent']:.1f} % vs {base['under57Percent']:.1f} %)"
    return f"no difference shown: the 95 % intervals overlap (vs {base_name})"

def delivery_mdd(row, base):
    return mdd(row["under57High"] - row["under57Low"], base["under57High"] - base["under57Low"])

def audio_row(runs):
    pops = underruns = concealed = None; minutes = 0.0
    for run in runs:
        lines = run["log"]["audio"]
        if run["log"]["first"] is not None and run["log"]["last"] is not None: minutes += (run["log"]["last"] - run["log"]["first"]) / 60.0
        for key in ("pops", "underruns", "concealed"):
            total = counter_total([l.get(key) for l in lines])
            if total is not None:
                if key == "pops": pops = (pops or 0) + total
                elif key == "underruns": underruns = (underruns or 0) + total
                else: concealed = (concealed or 0) + total
    return {"lines": sum(len(r["log"]["audio"]) for r in runs), "pops": pops, "underruns": underruns, "concealed": concealed, "minutes": minutes}

def sr_rows(runs):
    by = {}
    for run in runs:
        for e in run["log"]["slip_sr"]: by.setdefault((int(e.get("choice") or 0), int(e.get("quality") or 0), e.get("out", "?")), []).append(e)
    return [{"choice": c[0], "quality": int(es[-1].get("quality") or 0), "in": es[-1].get("in", "?"), "out": es[-1].get("out", "?"),
             "gpuP50": statistics.median([e["gpuP50"] for e in es if e.get("gpuP50") is not None] or [float("nan")]),
             "gpuP95": statistics.median([e["gpuP95"] for e in es if e.get("gpuP95") is not None] or [float("nan")]),
             "frames": sum(int(e.get("frames") or 0) for e in es)} for c, es in sorted(by.items())]

def fg_rows(runs):
    by = {}
    for run in runs:
        for e in run["log"]["slip_fg"]: by.setdefault((int(e.get("method") or 0), int(e.get("multiplier") or 0)), []).append(e)
    def med(es, key):
        values = [e[key] for e in es if e.get(key) is not None]
        return statistics.median(values) if values else float("nan")
    return [{"method": m, "multiplier": x, "outputFps": med(es, "outputFps"), "generatedShown": sum(int(e.get("generatedShown") or 0) for e in es),
             "generatedExpired": sum(int(e.get("generatedExpired") or 0) for e in es), "skippedPairs": sum(int(e.get("skippedPairs") or 0) for e in es),
             "costP50": med(es, "costP50"), "costP95": med(es, "costP95"), "shownGapP95": med(es, "shownGapP95"), "shownGapMax": max([e["shownGapMax"] for e in es if e.get("shownGapMax") is not None] or [float("nan")]),
             "pairDelayMs": med(es, "pairDelayMs")} for (m, x), es in sorted(by.items())]

def hook_unsupported(runs, step):
    """True when an autorun hook step failed with reason=unsupported (the build has no handler for it)."""
    return any(e.get("state") == "failed" and e.get("reason") == "unsupported" and (e.get("name") in (step, "[name]")) for r in runs for e in r["log"]["autorun"])

def precision_tables(data, summary=None):
    """The tables of RESULT.txt beyond the PS-button result: per latency arm and its pair."""
    summary = summary or result_summary(data)
    latency_runs = [r for r in data["runs"] if r["purpose"] == "latency"]
    names = [r["name"] for r in summary["arms"]]
    by_arm = {a: [r for r in latency_runs if r["arm"] == a] for a in names}
    route = network_route(data)
    def known(runs, key): return [p[key] for r in runs for p in r["log"]["slip_press"] if p.get(key) is not None and p[key] >= 0]
    sources = {"pipeline": lambda runs: known(runs, "arrivalToScanoutMs"), "console": lambda runs: known(runs, "sendToArrivalMs"),
               "total": lambda runs: [v for r in runs for v in r["log"]["presses"]]}
    tables = {"route": route, "label": WIFI_LABEL if route == "wifi" else "", "seed": BOOT_SEED, "rounds": BOOT_ROUNDS}
    refused = {r["name"]: r["refused"] for r in summary["arms"]}
    pair = {r["name"]: r["compare"] for r in summary["arms"]}
    for key, source in sources.items():
        rows = {a: latency_row(source(by_arm[a])) for a in names}
        for a in names:
            base = pair[a]; row = rows[a]
            if base == a: row["verdict"] = "baseline"
            elif base not in rows: row["verdict"] = f"no verdict: its pair '{base}' was not run"
            elif refused.get(a) or refused.get(base): row["verdict"] = "REFUSED: the two arms ran with different settings (see the result above)"
            else:
                row["verdict"] = latency_verdict(row, rows[base], base, key=key)
                row["mdd"] = mdd(row["p50High"] - row["p50Low"], rows[base]["p50High"] - rows[base]["p50Low"])
            if key == "pipeline":
                windows = [w["arrivalToScanoutP95"] for r in by_arm[a] for w in r["log"]["slip_latency"] if w.get("arrivalToScanoutP95") is not None and w["arrivalToScanoutP95"] >= 0]
                row["windowP95"] = statistics.median(windows) if windows else float("nan")
        tables[key] = rows
    rows = {a: delivery_row(by_arm[a]) for a in names}
    for a in names:
        base = pair[a]; row = rows[a]
        if base == a: row["verdict"] = "baseline"
        elif base not in rows: row["verdict"] = f"no verdict: its pair '{base}' was not run"
        elif refused.get(a) or refused.get(base): row["verdict"] = "REFUSED: the two arms ran with different settings (see the result above)"
        else: row["verdict"] = delivery_verdict(row, rows[base], base); row["mdd"] = delivery_mdd(row, rows[base])
    tables["delivery"] = rows
    if route == "wifi":
        for key in ("pipeline", "console", "total", "delivery"):
            for row in tables[key].values(): row["verdict"] = WIFI_LABEL + "; " + row["verdict"]
    tables["audio"] = {a: audio_row(by_arm[a]) for a in names}
    measured = measured_tables(data)
    for a in names:
        if a in measured["arms"] and "audio" in measured["groups"]: tables["audio"][a]["measured"] = measured_group_text(measured, a, "audio")
    every = [r for r in data["runs"]]
    tables["sr"] = {"rows": sr_rows(every), "unsupported": hook_unsupported(every, "sr"),
                    "holds": [{"label": h["label"], "seconds": h.get("seconds"), "fps": h.get("fps"), "latencyP50Ms": h.get("latencyP50Ms"), "latencyP95Ms": h.get("latencyP95Ms"), "valid": h.get("valid")}
                              for r in every for h in r["log"]["sr_holds"]]}
    # The tearing arm (VEYRA_EXP_PRESENT_TEARING): torn frames get no scan-out time from the app, so without PresentMon
    # its tables are not scored.
    for a in names:
        switches = (data["table"].get(a) or {}).get("switches") or ""
        if "VEYRA_EXP_PRESENT_TEARING" in switches and not any(r.get("presentmon") for r in by_arm[a]):
            for key in ("pipeline", "console", "total", "delivery"):
                if a in tables[key]: tables[key][a]["verdict"] = "not scored: the tearing arm needs PresentMon (torn frames get no scan-out time from the app)"
    tables["presentation"] = {a: presentation_row(by_arm[a]) for a in names}
    tables["fg"] = {"rows": fg_rows(every), "unsupported": hook_unsupported(every, "fg")}
    tables["dedup"] = dedup_row([r for r in every if r["purpose"] == "probe"])
    return tables

DEDUP_KEYS = ("repeatsFound", "repeatsRemoved", "exactVetoes", "suspendedKeeps", "keptUnsure", "unmeasured")

def dedup_row(runs):
    """Repeated pictures seen by duplicate detection in the -Session dedup-probe runs (diagnostic; [remoteplay-cadence] per 10 s)."""
    windows = [c for r in runs for c in r["log"]["cadence"]]
    row = {"runs": len(runs), "windows": len(windows)}
    for key in DEDUP_KEYS: row[key] = int(sum(c.get(key) or 0 for c in windows))
    found, inexact = row["repeatsFound"], row["exactVetoes"]
    if not runs: row["sentence"] = ""
    elif not windows: row["sentence"] = "no [remoteplay-cadence] line: the probe did not run (needs the experimental build and a PS5 stream)"
    elif not found: row["sentence"] = "exact repeats seen: no repeated pictures were found at all (was the game running at 30 FPS?)"
    else:
        exact = max(0, found - inexact)
        row["sentence"] = (f"exact repeats seen: yes, {exact} of {found} repeated pictures ({100.0 * exact / found:.0f} %) decoded exactly" if exact
                           else f"exact repeats seen: no, none of the {found} repeated pictures decoded exactly")
    return row

def presentation_row(runs):
    """Swap chain facts ([slipstream-swapchain]), the app's present-to-scanout and PresentMon's MsUntilDisplayed side by side."""
    import presentmon
    chain = {}
    for r in runs: chain.update(r["log"]["swapchain"])
    present = {}
    for r in runs: present.update(r["log"]["present"])
    app = [w["presentToScanoutP50"] for r in runs for w in r["log"]["slip_latency"] if w.get("presentToScanoutP50") is not None and w["presentToScanoutP50"] >= 0]
    caps = [presentmon.summary(p) for r in runs for p in sorted(glob.glob(os.path.join(r["folder"], "presentmon*.csv"))) if not p.endswith("presentmon-windows.csv")]
    pm = presentmon.merge(caps)
    app50 = statistics.median(app) if app else float("nan")
    until = pm["untilP50"] if pm else float("nan")
    refresh = number(next((r["log"]["config"].get("refreshHz") for r in runs if r["log"]["config"].get("refreshHz")), None))
    agree = ""
    if app50 == app50 and until == until and refresh:
        agree = "agree within one refresh" if abs(app50 - until) <= 1000.0 / refresh else "DISAGREE by more than one refresh: fix the statistics first"
    return {"flip": chain.get("flip", ""), "fullscreen": chain.get("fullscreen", ""), "windowCoversMonitor": chain.get("windowCoversMonitor", ""),
            "tearingSupported": chain.get("tearingSupported", ""), "compositionMode": present.get("compositionMode", ""), "tearingActive": present.get("active", ""),
            "appPresentToScanoutP50": app50, "presentMonUntilDisplayedP50": until, "presentMonIndependentPercent": pm["independentPercent"] if pm else float("nan"),
            "presentMonComposedPercent": pm["composedPercent"] if pm else float("nan"), "agreement": agree}

def precision_sections(tables):
    label = tables["label"]
    head = [label] if label else []
    out = []
    titles = {"pipeline": "PC pipeline (arrival to scanout), software", "console": "Console + network (send to arrival), software",
              "total": "Total (PS-button style), software"}
    for key in ("pipeline", "console", "total"):
        rows = [[a, str(r["n"])] + [f"{fmt(r[k], 2)} [{fmt(r[k + 'Low'], 2)}, {fmt(r[k + 'High'], 2)}]" for k, _p in QUANTILES] +
                [fmt(r.get("mdd", float("nan")), 2), r["verdict"]] + ([fmt(r.get("windowP95", float("nan")), 2)] if key == "pipeline" else [])
                for a, r in tables[key].items()]
        out.append((titles[key], head + [f"Per press; bootstrap 95 % intervals ({BOOT_ROUNDS} resamples, seed {BOOT_SEED}). Verdict: at least {TABLE_MINIMUM} values per arm and the P50 intervals do not overlap. MDD = the smallest P50 difference that rule can call (the sum of the two half-widths). Software counters only; never mixed with camera or light sensor data."]
                    + ([BUTTON_TEST_NOTE] if key == "total" else []),
                    ["Arm", "Values", "P50 ms", "P95 ms", "P99 ms", "MDD ms", "Verdict"] + (["5 s window P95 ms (median)"] if key == "pipeline" else []), rows))
    rows = [[a, str(r["seconds"]), f"{fmt(r['under57Percent'], 1)} [{fmt(r['under57Low'], 1)}, {fmt(r['under57High'], 1)}]", fmt(r["replacedPercent"], 2) + (f" ({r['replacedFrom']})" if r["replacedFrom"] else ""),
             fmt(r["gapP95"], 2), "n/a" if r["clumps"] is None else str(r["clumps"]), fmt(r.get("mdd", float("nan")), 1), r["verdict"]] for a, r in tables["delivery"].items()]
    out.append(("Frame delivery", head + [f"Seconds with fewer than 57 presents submitted ([frame-rate], first 3 and last 2 lines of each run left out); 95 % intervals by resampling whole runs (seconds of one run are not independent); replaced = frames overwritten before the engine took them. Verdict: at least {DELIVERY_MINIMUM} seconds and {DELIVERY_MINIMUM_RUNS} runs per arm and the intervals do not overlap. MDD in percentage points (the sum of the two half-widths)."],
                ["Arm", "Seconds", "Under 57 % [95 %]", "Replaced %", "Arrival gap P95 ms", "Clumps", "MDD pp", "Verdict"], rows))
    rows = [[a, str(r["lines"]), "n/a" if r["pops"] is None else f"{r['pops']:.0f}", "n/a" if r["underruns"] is None else f"{r['underruns']:.0f}", "n/a" if r["concealed"] is None else f"{r['concealed']:.0f}", fmt(r["minutes"], 1), r.get("measured", "")]
            for a, r in tables["audio"].items()]
    out.append(("Audio", head + ["Counts from [remoteplay-audio] (pops = underrun starts + hard resets). Counts only, no verdict. Delay, target, hard resets and pops per minute from [remoteplay-audio], beside the video clock lag of [remoteplay-clock] arrivalMinusPtsMs (measures.csv group audio)."], ["Arm", "Lines", "Pops", "Underruns", "Concealed", "Minutes logged", "Delay, target, per minute, clock lag"], rows))
    rows = [[a, r["flip"] or "n/a", r["fullscreen"] or "n/a", r["windowCoversMonitor"] or "n/a", r["compositionMode"] or "n/a", fmt(r["appPresentToScanoutP50"], 2),
             fmt(r["presentMonUntilDisplayedP50"], 2), fmt(r["presentMonIndependentPercent"], 0), fmt(r["presentMonComposedPercent"], 0), r["agreement"] or "not both measured"]
            for a, r in tables["presentation"].items()]
    out.append(("Presentation: composed or independent flip", head + ["[slipstream-swapchain] facts, the app's present-to-scanout P50 ([slipstream-latency]) and PresentMon's MsUntilDisplayed P50 for the same arm. 'Composed: Flip' means the desktop compositor adds about a refresh; independent flip does not."],
                ["Arm", "Flip", "Fullscreen", "Window covers monitor", "Composition mode", "App present to scanout P50 ms", "PresentMon MsUntilDisplayed P50 ms", "Independent flip %", "Composed %", "Agreement"], rows))
    sr = tables["sr"]
    holds = "; ".join(f"{h['label']}: in-app latency P50 {fmt(h['latencyP50Ms'] if h['latencyP50Ms'] is not None and h['latencyP50Ms'] >= 0 else float('nan'), 2)} ms, P95 {fmt(h['latencyP95Ms'] if h['latencyP95Ms'] is not None and h['latencyP95Ms'] >= 0 else float('nan'), 2)} ms over {fmt(h['seconds'] if h['seconds'] is not None else float('nan'), 0)} s" for h in sr["holds"])
    out.append(("Super resolution cost", ["not available in this build" if sr["unsupported"] and not sr["rows"] else "GPU time per choice and output size ([slipstream-sr], medians of 5 s windows).",
                                          "Holds (measured values only, no recommendation): " + (holds or "none logged")],
                ["Choice", "Quality", "In", "Out", "GPU P50 ms", "GPU P95 ms", "Frames"], [[str(r["choice"]), str(r["quality"]), r["in"], r["out"], fmt(r["gpuP50"], 2), fmt(r["gpuP95"], 2), str(r["frames"])] for r in sr["rows"]]))
    dd = tables["dedup"]
    if dd["runs"]:
        out.append(("Repeat pictures in this stream (diagnostic)", ["Diagnostic only, never a latency comparison: duplicate detection measured every picture and removed nothing. " + dd["sentence"] + "."],
                    ["Runs", "10 s windows"] + list(DEDUP_KEYS), [[str(dd["runs"]), str(dd["windows"])] + [str(dd[k]) for k in DEDUP_KEYS]]))
    fg = tables["fg"]
    out.append(("Frame generation", ["not available in this build" if fg["unsupported"] and not fg["rows"] else "Per method and multiplier ([slipstream-fg], medians of 5 s windows)."],
                ["Method", "Multiplier", "Output FPS", "Generated shown", "Generated expired", "Skipped pairs", "Cost P50 ms", "Cost P95 ms", "Shown gap P95 ms", "Shown gap max ms", "Pair delay ms"],
                [[str(r["method"]), str(r["multiplier"]), fmt(r["outputFps"], 1), str(r["generatedShown"]), str(r["generatedExpired"]), str(r["skippedPairs"]), fmt(r["costP50"], 2), fmt(r["costP95"], 2),
                  fmt(r["shownGapP95"], 2), fmt(r["shownGapMax"], 2), fmt(r["pairDelayMs"], 2)] for r in fg["rows"]]))
    return out

# --- the network session (-Session network, SmartLink; phase 2 of sprint 4) -----------------------------------------
# The same definitions as run-tests.ps1 (Get-NetworkTables). Every metric is a list of per-window values (5 s lines of
# the app); a difference between an arm and the observe arm is the difference of the means with a bootstrap interval
# (Park-Miller as above: each round draws the arm's resample, then the observe arm's). Lower is better for every metric.
NETWORK_MINIMUM_WINDOWS = 6           # 30 s of 5 s windows before a run counts for an arm
NETWORK_MINIMUM_RUNS = 2              # runs per arm before an interval; fewer: "indication only: needs more runs"
# Motion: the median video rate against the console's own target rate (targetKbps of [slipstream-smartlink] or the health
# line) when logged, else against the requested bitrate with a lower share. Both arms must see SIMILAR motion: their
# shares may differ by at most MOTION_TOLERANCE (absolute), and neither may be below its share.
MOTION_TARGET_SHARE = 0.40
MOTION_REQUESTED_SHARE = 0.20
MOTION_TOLERANCE = 0.25
MOTION_ADVICE = "The picture hardly moved in this test: play something with movement and run it again."
# metric key, label, latency metric. The unit of analysis is the RUN (the mean of its 5 s windows): windows of one run are
# strongly correlated, runs are not. Only the PRIMARY metrics get a verdict word; every other metric is descriptive.
NETWORK_METRICS = (
    ("arrivalToScanoutP50", "in-app latency P50 (arrival to scanout) ms", True),
    ("arrivalToScanoutP95", "in-app latency P95 ms", True),
    ("arrivalToScanoutP99", "in-app latency P99 ms", True),
    ("stutterPerMinute", "stutter events per minute (gaps over 25 ms, about 1.5 frame intervals)", False),
    ("gapP95", "frame gap P95 ms", False),
    ("replacedPercent", "frames replaced %", False),
    ("audioDelayMs", "audio delay ms (delayMs; a cost, shown before any gain in dropouts)", False),
    ("popsPerMinute", "audio pops per minute", False),
    ("clicksPerMinute", "audio clicks per minute (dropouts without a fade)", False),
    ("underrunsPerMinute", "audio underruns per minute", False),
    ("underrunStartsPerMinute", "audio underrun starts per minute (underrunStarts=, per window)", False),
    ("tailsPerMinute", "audio tails per minute (tails=, per window)", False),
    ("concealedPerMinute", "audio concealed per minute", False),
)
# One primary metric per question; Bonferroni over the two: each interval is 97.5 % (two-sided), so the chance of any
# verdict word for an arm that is really the same as the reference stays at or below 5 %.
NETWORK_PRIMARY = (("stutterPerMinute", "stability"), ("arrivalToScanoutP95", "cost"))
NETWORK_CONFIDENCE = "97.5 % (Bonferroni over 2 primary metrics)"
# Student t, 0.9875 quantile, df 1..30 (df rounded down, so the interval is never narrower than Welch's; above 30: 30).
T_9875 = (25.452, 6.205, 4.177, 3.495, 3.163, 2.969, 2.841, 2.752, 2.685, 2.634, 2.593, 2.560, 2.533, 2.510, 2.490,
          2.473, 2.458, 2.445, 2.433, 2.423, 2.414, 2.405, 2.398, 2.391, 2.385, 2.379, 2.373, 2.368, 2.364, 2.360)

def welch_interval(a, b):
    """(mean a - mean b, low, high): Welch interval at 97.5 % on per-run values; nan without 2 values on each side."""
    if len(a) < 2 or len(b) < 2: return float("nan"), float("nan"), float("nan")
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    va = sum((x - ma) ** 2 for x in a) / (len(a) - 1); vb = sum((x - mb) ** 2 for x in b) / (len(b) - 1)
    qa, qb = va / len(a), vb / len(b)
    diff = ma - mb
    if qa + qb <= 0: return diff, diff, diff
    den = (qa * qa / (len(a) - 1) if qa > 0 else 0.0) + (qb * qb / (len(b) - 1) if qb > 0 else 0.0)
    df = (qa + qb) ** 2 / den if den > 0 else float(len(a) + len(b) - 2)
    t = T_9875[min(30, max(1, int(math.floor(df)))) - 1]
    half = t * math.sqrt(qa + qb)
    return diff, diff - half, diff + half

# The smallest effect worth finding per verdict metric (one place; run-tests.ps1 $networkEffect holds the same numbers and
# docs/HARDWARE_TEST_PROCEDURE.md shows them in one table). ("relative", share of the reference arm's mean, floor) or
# ("absolute", value, 0). When the interval's half-width is larger than the effect the verdict is "cannot tell", never
# "no difference shown".
NETWORK_EFFECT = {"stutterPerMinute": ("relative", 0.30, 0.5), "arrivalToScanoutP95": ("absolute", 1.0, 0.0)}
EFFECT_UNIT = {"stutterPerMinute": "stutter events per minute", "arrivalToScanoutP95": "ms"}
NETWORK_RUNS_CAP = 20                 # "more than 20": the spread is too large for any session the kit plans
POWER_Z = 0.842                       # 80 % power (normal quantile 0.80)

def effect_size(key, reference_values):
    kind, value, floor = NETWORK_EFFECT[key]
    if kind == "absolute": return value
    m = sum(reference_values) / len(reference_values) if reference_values else 0.0
    return max(value * m, floor)

def runs_needed(a, b, effect):
    """Runs per arm for a 97.5 % interval to find `effect` with 80 % power, from the observed run-to-run spread (pooled
    SD s; equal runs per arm; t from T_9875 at df 2n-2): smallest n with (t + 0.842) * s * sqrt(2 / n) <= effect.
    NETWORK_RUNS_CAP + 1 = "more than the cap"."""
    if len(a) < 2 or len(b) < 2 or effect <= 0: return NETWORK_RUNS_CAP + 1
    ma, mb = sum(a) / len(a), sum(b) / len(b)
    va = sum((x - ma) ** 2 for x in a) / (len(a) - 1); vb = sum((x - mb) ** 2 for x in b) / (len(b) - 1)
    s = math.sqrt((va + vb) / 2.0)
    for n in range(2, NETWORK_RUNS_CAP + 1):
        t = T_9875[min(30, 2 * n - 2) - 1]
        if (t + POWER_Z) * s * math.sqrt(2.0 / n) <= effect: return n
    return NETWORK_RUNS_CAP + 1

def no_difference_text(key, ra, rb, low, high):
    """The verdict when the interval contains zero: "no difference shown" only when it is narrower than the effect."""
    effect = effect_size(key, rb)
    half = (high - low) / 2.0
    what = f"{effect:.2f} {EFFECT_UNIT[key]}"
    if half <= effect: return f"no difference shown (interval +-{half:.2f}, narrower than {what})"
    n = min(len(ra), len(rb))
    m = max(runs_needed(ra, rb, effect), n + 1)
    more = f"more than {NETWORK_RUNS_CAP}" if m > NETWORK_RUNS_CAP else f"about {m}"
    return f"cannot tell with {n} runs per arm: this session would need {more} runs per arm to show a difference of {what}"

def run_values(runs, key):
    """Per-run means of one metric (runs with at least NETWORK_MINIMUM_WINDOWS windows); 'addedWaitMs' from the facts."""
    out = []
    for r in runs:
        w, f = network_windows([r])
        if key == "addedWaitMs":
            if f["addedWaitMs"] == f["addedWaitMs"]: out.append(f["addedWaitMs"])
        elif len(w[key]) >= NETWORK_MINIMUM_WINDOWS: out.append(sum(w[key]) / len(w[key]))
    return out

def motion_gate(fa, fb):
    """'' when both arms saw similar motion, else the reason (the verdicts are withheld)."""
    if fa["motion"] is None or fb["motion"] is None: return "motion not known (no video rate, no console target and no requested bitrate)"
    if fa["motion"] is False or fb["motion"] is False: return "the picture hardly moved in one arm (" + MOTION_ADVICE + ")"
    if abs(fa["motionShare"] - fb["motionShare"]) > MOTION_TOLERANCE:
        return f"the arms saw different motion (video rate {fa['motionShare'] * 100:.0f} % and {fb['motionShare'] * 100:.0f} % of the reference rate; at most {MOTION_TOLERANCE * 100:.0f} points apart)"
    return ""

ADVICE = {
    "wired": "Wired link: SmartLink leaves every setting as it is.",
    "strong-wifi": "Strong Wi-Fi: fine for play. A cable still gives the lowest delay.",
    "unstable-wifi": "Unstable Wi-Fi: move the PC or the router closer, keep other devices from big downloads or videos while playing, or connect a cable.",
    "unknown": "The connection type was not recognised.",
}
REGIME_TEXT = {"periodic-spikes": "short delays that come back regularly (often another device or a background scan)", "congested": "the network was busy",
               "weak-signal": "the signal was weak", "interrupted": "the connection dropped out for a moment", "stable": "steady"}

def boot_mean_difference(a, b, rounds=BOOT_ROUNDS, seed=BOOT_SEED):
    """(mean a - mean b, low, high); a and b sorted first, resampled with the Park-Miller generator (a, then b, per round)."""
    a, b = sorted(a), sorted(b)
    if len(a) < 2 or len(b) < 2: return float("nan"), float("nan"), float("nan")
    x, diffs = seed, []
    for _ in range(rounds):
        total_a = 0.0
        for _i in range(len(a)):
            x = (x * PM_MULT) % PM_MOD; total_a += a[x % len(a)]
        total_b = 0.0
        for _i in range(len(b)):
            x = (x * PM_MULT) % PM_MOD; total_b += b[x % len(b)]
        diffs.append(total_a / len(a) - total_b / len(b))
    return sum(a) / len(a) - sum(b) / len(b), percentile(diffs, .025), percentile(diffs, .975)

def rate_per_minute(entries, key, per_window=False):
    """Per-window rate of a counter (running total or per window: increments are taken when the values never fall;
    per_window=True: the values are counts of their own window, as underrunStarts= and tails= of [remoteplay-audio])."""
    points = [(e["t"], e[key]) for e in entries if e.get(key) is not None and e.get("t") is not None]
    if len(points) < 2: return []
    rising = (not per_window) and all(b[1] >= a[1] for a, b in zip(points, points[1:]))
    out = []
    for (t0, v0), (t1, v1) in zip(points, points[1:]):
        dt = t1 - t0
        if dt <= 0: continue
        out.append(((v1 - v0) if rising else v1) * 60.0 / dt)
    return out

def network_windows(runs):
    """Per-window values of every network metric, and the facts of one arm."""
    w = {k: [] for k, _l, _x in NETWORK_METRICS}
    rtt, target, measured, profiles, regimes, reorder, netlimited, headroom, video, requested, wifi_mode = [], [], [], {}, {}, [], [], [], [], [], {}
    gap_max = float("nan")
    rtt_undecided = rate_undecided = False
    for run in runs:
        log = run["log"]
        for e in log["slip_latency"]:
            for key in ("arrivalToScanoutP50", "arrivalToScanoutP95", "arrivalToScanoutP99"):
                if e.get(key) is not None and e[key] >= 0: w[key].append(e[key])
        for e in log["slip_delivery"]:
            if e.get("longGaps") is not None: w["stutterPerMinute"].append(e["longGaps"] * 12.0)       # 5 s windows
            if e.get("gapP95") is not None: w["gapP95"].append(e["gapP95"])
            if e.get("arrived"): w["replacedPercent"].append(100.0 * (e.get("replaced") or 0) / e["arrived"])
            if e.get("gapMax") is not None: gap_max = e["gapMax"] if gap_max != gap_max else max(gap_max, e["gapMax"])
        w["popsPerMinute"] += rate_per_minute(log["audio"], "pops")
        w["underrunsPerMinute"] += rate_per_minute(log["audio"], "underruns")
        w["concealedPerMinute"] += rate_per_minute(log["audio"], "concealed")
        w["clicksPerMinute"] += rate_per_minute(log["audio"], "clicks")
        w["underrunStartsPerMinute"] += rate_per_minute(log["audio"], "underrunStarts", per_window=True)
        w["tailsPerMinute"] += rate_per_minute(log["audio"], "tails", per_window=True)
        w["audioDelayMs"] += [e["delayMs"] for e in log["audio"] if e.get("delayMs") is not None and e["delayMs"] >= 0]
        previous = None
        for e in log["smartlink"]:
            if e.get("rttMs") is not None and e["rttMs"] >= 0: rtt.append(e["rttMs"])
            if e.get("targetKbps") is not None and e["targetKbps"] >= 0: target.append(e["targetKbps"])
            if e.get("measuredKbps") is not None and e["measuredKbps"] >= 0: measured.append(e["measuredKbps"])
            profiles[e["profile"] or "unknown"] = profiles.get(e["profile"] or "unknown", 0) + 1
            regimes[e["regime"] or "unknown"] = regimes.get(e["regime"] or "unknown", 0) + 1
            if e.get("reorderMs") is not None: reorder.append(e["reorderMs"])
            if e.get("netLimited") is not None: netlimited.append(e["netLimited"])
            if e.get("headroom") is not None: headroom.append(e["headroom"])
            if e.get("wifiStreamingMode"): wifi_mode[e["wifiStreamingMode"]] = wifi_mode.get(e["wifiStreamingMode"], 0) + 1
        if not log["smartlink"]:
            # the health line's console fields (ConnectionHealth.h formatHealthLine; -1 = unknown)
            for e in log["health"]:
                if e.get("consoleRttMs") is not None and e["consoleRttMs"] >= 0: rtt.append(e["consoleRttMs"])
                if e.get("consoleTargetKbps") is not None and e["consoleTargetKbps"] >= 0: target.append(e["consoleTargetKbps"])
                if e.get("consoleMeasuredKbps") is not None and e["consoleMeasuredKbps"] >= 0: measured.append(e["consoleMeasuredKbps"])
        units = log.get("console_units")
        if units:
            if units["rtt"] == "undecided": rtt_undecided = True
            if units["rate"] == "undecided": rate_undecided = True
        video += [f["videoMbps"] for _t, f in log["progress"] if f.get("videoMbps") is not None and f["videoMbps"] > 0]
        requested += [number(s[2]) for s in log["streams"] if number(s[2])]
    # consoleUnits said the unit could not be decided: the round trip (and the console's rates) are not known in this
    # session, never numbers (a number in an undecided unit could be off by a factor of 1000).
    if rtt_undecided: rtt = []
    if rate_undecided: target, measured = [], []
    changes = sum(1 for a, b in zip(reorder, reorder[1:]) if a != b)
    at_value = {}
    for v in reorder: at_value[v] = at_value.get(v, 0) + 1
    total = len(reorder)
    facts = {"rtt": sorted(rtt), "targetKbps": statistics.median(target) if target else float("nan"), "measuredKbps": statistics.median(measured) if measured else float("nan"),
             "requestedKbps": statistics.median(requested) if requested else float("nan"), "gapMax": gap_max,
             "profiles": {k: 100.0 * n / sum(profiles.values()) for k, n in sorted(profiles.items())}, "regimes": {k: 100.0 * n / sum(regimes.values()) for k, n in sorted(regimes.items())},
             "reorderChanges": changes, "reorderShares": {str(int(k)) if k == int(k) else str(k): 100.0 * n / total for k, n in sorted(at_value.items())},
             "addedWaitMs": sum(k * n for k, n in at_value.items()) / total if total else float("nan"),
             "netLimitedPercent": 100.0 * sum(1 for v in netlimited if v > 0) / len(netlimited) if netlimited else float("nan"),
             "headroomPercent": 100.0 * sum(1 for v in headroom if v > 0) / len(headroom) if headroom else float("nan"),
             "videoMbps": statistics.median(video) if video else float("nan"), "smartlinkLines": sum(len(r["log"]["smartlink"]) for r in runs), "wifiStreamingMode": wifi_mode,
             "rttUndecided": rtt_undecided, "rateUndecided": rate_undecided}
    req, tgt, vid = facts["requestedKbps"], facts["targetKbps"], facts["videoMbps"]
    facts["motionBasis"], facts["motionShare"], facts["motion"] = "unknown", float("nan"), None
    if vid == vid:
        if tgt == tgt and tgt > 0: facts["motionBasis"], facts["motionShare"] = "console target", vid * 1000.0 / tgt; facts["motion"] = facts["motionShare"] >= MOTION_TARGET_SHARE
        elif req == req and req > 0: facts["motionBasis"], facts["motionShare"] = "requested", vid * 1000.0 / req; facts["motion"] = facts["motionShare"] >= MOTION_REQUESTED_SHARE
    return w, facts

def connection_text(facts, route):
    profile = max(facts["profiles"], key=facts["profiles"].get) if facts["profiles"] else ("wired" if route == "wired" else "unknown")
    regime = max(facts["regimes"], key=facts["regimes"].get) if facts["regimes"] else "unknown"
    parts = [f"Detected connection: {profile} ({fmt(facts['profiles'].get(profile, float('nan')), 0)} % of the time)." if facts["profiles"] else "No SmartLink line: the connection type comes from the network route only.",
             ADVICE.get(profile, ADVICE["unknown"])]
    if regime in REGIME_TEXT and regime != "stable": parts.append(f"Most of the time: {REGIME_TEXT[regime]}.")
    if facts["netLimitedPercent"] == facts["netLimitedPercent"] and facts["netLimitedPercent"] > 50: parts.append("Stutter came from the connection, so lowering graphics settings would not help.")
    if facts["headroomPercent"] == facts["headroomPercent"] and facts["headroomPercent"] > 50: parts.append("The connection carried the requested quality with room to spare.")
    if "refused" in facts.get("wifiStreamingMode", {}): parts.append("Wi-Fi streaming mode: the adapter did not accept it (not an error).")
    elif "requested" in facts.get("wifiStreamingMode", {}): parts.append("Wi-Fi streaming mode: requested from the adapter for the stream.")
    if route == "wifi": parts.append(WIFI_LABEL + ".")
    return " ".join(parts)

def network_tables(data):
    """Per arm of the network session: metrics with intervals, the verdicts against the observe arm, the facts and the
    comparison with original and stable arms on what all of them log."""
    runs = [r for r in data["runs"] if r["purpose"] == "network"]
    if not runs: return None
    names = []
    for r in runs:
        if r["arm"] not in names: names.append(r["arm"])
    by_arm = {a: [r for r in runs if r["arm"] == a] for a in names}
    route = network_route(data)
    out = {"route": route, "label": WIFI_LABEL if route == "wifi" else "", "arms": {}, "builds": {}}
    windows, facts = {}, {}
    for a in names: windows[a], facts[a] = network_windows(by_arm[a])
    reference = next((a for a in names if (data["table"].get(a) or {}).get("compare") == a and (data["table"].get(a) or {}).get("exe", "experimental") == "experimental"), names[0])
    configs = {a: arm_config(by_arm[a]) for a in names}
    for a in names:
        row = {"metrics": {}, "facts": facts[a], "connection": connection_text(facts[a], route), "verdicts": [], "latencyWorse": [], "motionGate": "", "runs": len(by_arm[a])}
        for key, label, latency in NETWORK_METRICS:
            v = windows[a][key]
            row["metrics"][key] = {"n": len(v), "mean": mean(v), "p50": percentile(v, .5), "p95": percentile(v, .95), "p99": percentile(v, .99), "max": max(v) if v else float("nan")}
        table = data["table"].get(a) or {}
        exe = table.get("exe", "experimental")
        row["exe"] = exe
        if a != reference and exe == "experimental":
            allowed = tuple(x for x in (table.get("differs") or "").split(";") if x)
            differ, _u = config_refusal(a, configs[a][0], reference, configs[reference][0], allowed)
            if configs[a][1] or configs[reference][1]: differ = differ + ["the runs of one arm differ in " + ", ".join(configs[a][1] + configs[reference][1])]
            gate = motion_gate(facts[a], facts[reference])
            row["motionGate"] = gate
            for key, label, latency in NETWORK_METRICS:
                ra, rb = run_values(by_arm[a], key), run_values(by_arm[reference], key)
                diff, low, high = welch_interval(ra, rb)
                if diff != diff and ra and rb: diff = sum(ra) / len(ra) - sum(rb) / len(rb)
                primary = next((q for k, q in NETWORK_PRIMARY if k == key), "")
                if not primary: verdict = "descriptive (no verdict)"
                elif differ: verdict = "not comparable: " + "; ".join(differ[:3])
                elif gate: verdict = "not comparable: " + gate
                elif len(ra) < NETWORK_MINIMUM_RUNS or len(rb) < NETWORK_MINIMUM_RUNS: verdict = f"indication only: needs more runs ({len(ra)} and {len(rb)} runs; at least {NETWORK_MINIMUM_RUNS} per arm)"
                elif low != low: verdict = "indication only: no interval"
                elif high < 0: verdict = f"better than {reference} by {-diff:.2f}"
                elif low > 0:
                    verdict = f"WORSE than {reference} by {diff:.2f}"
                    if latency: row["latencyWorse"].append(f"{label}: {verdict}")
                else: verdict = no_difference_text(key, ra, rb, low, high)
                if route == "wifi": verdict = WIFI_LABEL + "; " + verdict
                row["verdicts"].append({"metric": key, "label": label, "primary": primary, "runs": [len(ra), len(rb)], "difference": diff, "low": low, "high": high, "verdict": verdict})
            ra, rb = run_values(by_arm[a], "addedWaitMs"), run_values(by_arm[reference], "addedWaitMs")
            row["addedWait"] = {"difference": (sum(ra) / len(ra) - sum(rb) / len(rb)) if ra and rb else float("nan"), "runs": [len(ra), len(rb)]}
        row["traces"] = []
        for r in by_arm[a]:
            for name in TRACE_FILES:
                path = os.path.join(r["folder"], name)
                if os.path.exists(path): row["traces"].append({"run": r["name"] + ("" if name == TRACE_NAME else " (" + name + ")"), "problem": trace_problem(path), "summary": trace_summary(path)})
        out["arms"][a] = row
    out["reference"] = reference
    # original and stable builds in the same session: only what every build logs
    for a in names:
        if out["arms"][a]["exe"] == "experimental": continue
        delivery = delivery_row(by_arm[a])
        display = [n for r in by_arm[a] for _t, n in r["log"]["display_latency"]]
        out["builds"][a] = {"under57Percent": delivery["under57Percent"], "seconds": delivery["seconds"], "replacedPercent": delivery["replacedPercent"],
                            "presentToScanoutP50": statistics.median([d["presentToScanoutP50Ms"] for d in display if d.get("presentToScanoutP50Ms") is not None] or [float("nan")]),
                            "presentToScanoutP95": statistics.median([d["presentToScanoutP95Ms"] for d in display if d.get("presentToScanoutP95Ms") is not None] or [float("nan")])}
    if out["builds"]:
        ref = by_arm[reference]
        delivery = delivery_row(ref)
        display = [n for r in ref for _t, n in r["log"]["display_latency"]]
        out["builds"][reference] = {"under57Percent": delivery["under57Percent"], "seconds": delivery["seconds"], "replacedPercent": delivery["replacedPercent"],
                                    "presentToScanoutP50": statistics.median([d["presentToScanoutP50Ms"] for d in display if d.get("presentToScanoutP50Ms") is not None] or [float("nan")]),
                                    "presentToScanoutP95": statistics.median([d["presentToScanoutP95Ms"] for d in display if d.get("presentToScanoutP95Ms") is not None] or [float("nan")])}
    out["notCompared"] = ("Not compared with the original and stable builds: in-app latency split, stutter events, frame gaps and audio counts per minute, "
                          "round trip, console target rate and SmartLink state (those builds write no [slipstream-latency], [slipstream-delivery], "
                          "[slipstream-smartlink] or audio count lines); the PS-button test only when it was run in every build.")
    return out

TRACE_NAME = "smartlink-trace.csv"
# The app's rotation (s4-net RemotePlaySmartLink.h openTrace): oldest first.
TRACE_FILES = ("smartlink-trace.2.csv", "smartlink-trace.1.csv", "smartlink-trace.csv")
# The app's exact header and longest line (rc4 include/veyra/remoteplay/SmartLinkTrace.h kHeader, kMaxLine).
TRACE_HEADER = ("endMs,windowMs,route,packetsReceived,packetsLost,packetsReordered,packetsLate,packetGapMeanMs,packetGapMaxMs,"
                "framesArrived,framesIncomplete,framesFecRecovered,frameGapMaxMs,frameGapExcessMs,lateRescueMs,reorderWaitMs,"
                "rttMs,consoleTargetKbps,measuredKbps,requestedKbps,videoQueueDepth,audioBufferedMs,audioUnderruns,"
                "wifiSignalPercent,wifiRssiDbm,wifiLinkMbps,wifiBandGhz,frameLateMeanMs")
TRACE_MAX_LINE = 28 * 22 + 2
TRACE_FIELD = re.compile(r"^-?[0-9]+(\.[0-9]+)?$")   # ASCII digits only; the app writes fixed decimals, -1 for unknown (never nan, inf or exponents)

def trace_problem(path):
    """'' when the file is the app's SmartLink trace: exactly its header, then plain decimal numbers only (ASCII digits),
    no line longer than kMaxLine; else why not. The same rule as run-tests.ps1 Test-TraceNumbers, which withholds such a
    file from the zip."""
    try:
        with open(path, "rb") as f: raw = f.read()
    except OSError: return "unreadable"
    if re.search(rb"[^\x20-\x7e\r\n]", raw): return "not plain printable ASCII"
    text = raw.decode("ascii")
    lines = re.split(r"\r?\n", text)
    if lines and lines[-1] == "": lines.pop()
    if not lines: return "empty"
    for number, line in enumerate(lines, 1):
        if len(line) > TRACE_MAX_LINE: return f"line {number} is longer than {TRACE_MAX_LINE} characters"
    if lines[0].rstrip() != TRACE_HEADER: return "line 1 is not the app's trace header"
    for number, line in enumerate(lines[1:], 2):
        if not line.strip(): continue
        for field in line.strip().split(","):
            if field.strip() and not TRACE_FIELD.match(field.strip()): return f"line {number} has a field that is not a number"
    return ""

def select_run_traces(log_folder, since):
    """The trace files of one run: every one of TRACE_FILES written at or after 'since' (seconds since the epoch), each
    taken whole. The same rule as run-tests.ps1 Save-RunTrace (which uses LastWriteTimeUtc, 2 s before the run started)."""
    out = []
    for name in TRACE_FILES:
        path = os.path.join(log_folder, name)
        if os.path.exists(path) and os.path.getmtime(path) >= since: out.append(path)
    return out

def trace_summary(path):
    """Rows, and median / P95 / max of every column of a numbers-only trace (at most 16 columns)."""
    if trace_problem(path): return None
    lines = [l for l in read_lines(path) if l.strip()]
    header = lines[0].strip().split(",")[:16]
    columns = {h: [] for h in header}
    for line in lines[1:]:
        for h, field in zip(header, line.strip().split(",")):
            v = number(field)
            if v is not None: columns[h].append(v)
    return {"rows": len(lines) - 1, "columns": {h: (percentile(v, .5), percentile(v, .95), max(v) if v else float("nan")) for h, v in columns.items()}}

NETWORK_RULE = (f"The unit is the RUN (the mean of its 5 s windows; runs with fewer than {NETWORK_MINIMUM_WINDOWS} windows do not count). Verdict words only for the primary "
                "metrics: stability = stutter events per minute, cost = in-app latency P95; difference of the run means against the reference with a Welch "
                f"{NETWORK_CONFIDENCE} interval. 'better' or 'WORSE' only when the interval excludes zero, the settings are comparable, both arms saw similar motion "
                f"and each arm has at least {NETWORK_MINIMUM_RUNS} runs; with fewer runs: 'indication only: needs more runs'. Every other metric is descriptive (no verdict).")

AUDIO_EXPECTATIONS = ("What the audio arms should show (expectations from the audio work, not results): base arm: clicks= rising when pops are heard, "
                      "with gapMax= at or above 25 ms and paddingMin=0; base + SMOOTH: clicks= flat; continuity arm (n-audio): fewer underrunStarts=, "
                      "playoutTargetMs= back to its starting value within a minute after trouble ends, and the delayMs= of quiet windows not growing. "
                      "Audio delay is a cost: the model predicts +13 to +16 ms on Wi-Fi while trouble lasts.")

UNITS_UNKNOWN = "not known in this session"

# The short sessions (run-tests.ps1 -Session presentation / audio; Add-ShortSessionSection writes the same text).
PRESENTATION_ARMS = ("p-window", "p-fullscreen", "p-lowqueue-off", "p-test-uncovered")
AUDIO_ARMS = ("s-base", "s-audio-smooth")
SHORT_SESSION_ROUNDS, SHORT_SESSION_PRESSES = 2, 12
WINDOW_FACT_KEYS = ("regionKind", "coversMonitor", "ownOverlaps", "flipBlockers")

def most_common_text(values):
    if not values: return "not logged"
    counts, order = {}, []
    for v in values:
        if v not in counts: counts[v] = 0; order.append(v)
        counts[v] += 1
    best = order[0]
    for v in order:
        if counts[v] > counts[best]: best = v
    return f"{best} in {fmt(100.0 * counts[best] / len(values), 0)} % of {len(values)} lines"

def short_session_paragraphs(data, session):
    """RESULT.txt's SHORT SESSION section for 'presentation' or 'audio': measured values per arm, no verdict word."""
    wanted = PRESENTATION_ARMS if session == "presentation" else AUDIO_ARMS if session == "audio" else ()
    if not wanted: return []
    out = [f"SHORT SESSION: {session.upper()} (measured values only, no verdict word: {SHORT_SESSION_ROUNDS} rounds of {SHORT_SESSION_PRESSES} presses are too few for one)"]
    if network_route(data) == "wifi": out.append("  " + WIFI_LABEL)
    for name in wanted:
        runs = [r for r in data["runs"] if r["arm"] == name]
        if not runs: out.append(f"  {name}: no run"); continue
        p50, p95, under, one_two, over = [], [], 0.0, 0.0, 0.0
        facts = {k: [] for k in WINDOW_FACT_KEYS}
        clicks = seconds = 0.0
        for r in runs:
            log = r["log"]
            for e in log["slip_latency"]:
                if e.get("presentToScanoutP50") is not None and e["presentToScanoutP50"] >= 0: p50.append(e["presentToScanoutP50"])
                if e.get("presentToScanoutP95") is not None and e["presentToScanoutP95"] >= 0: p95.append(e["presentToScanoutP95"])
                under += e.get("scanoutUnder1Refresh") or 0.0; one_two += e.get("scanout1To2Refresh") or 0.0; over += e.get("scanoutOver2Refresh") or 0.0
            for w in log["window_facts"]:
                for k in WINDOW_FACT_KEYS:
                    if k in w: facts[k].append(w[k])
            points = [(e["t"], e["clicks"]) for e in log["audio"] if e.get("clicks") is not None and e.get("t") is not None]
            if len(points) >= 2 and points[-1][0] > points[0][0]:
                clicks += max(0.0, points[-1][1] - points[0][1]); seconds += points[-1][0] - points[0][0]
        if session == "presentation":
            med = lambda v: statistics.median(v) if v else float("nan")
            text = f"  {name} ({len(runs)} runs): present to vblank P50 {fmt(med(p50), 2)} ms, P95 {fmt(med(p95), 2)} ms (dated presents only, DXGI statistics, not screen latency; medians of {len(p50)} windows of [slipstream-latency])"
            total = under + one_two + over
            if total > 0: text += f"; dated presents within 1 refresh {fmt(100.0 * under / total, 1)} %, 1 to 2 refreshes {fmt(100.0 * one_two / total, 1)} %, over 2 refreshes {fmt(100.0 * over / total, 1)} %"
            else: text += "; refresh buckets not logged"
            out.append(text + ". Window facts: " + ", ".join(f"{k} {most_common_text(facts[k])}" for k in WINDOW_FACT_KEYS) + ".")
        elif seconds > 0: out.append(f"  {name} ({len(runs)} runs): clicks= rose by {int(clicks)} in {fmt(seconds / 60.0, 1)} min ({fmt(clicks * 60.0 / seconds, 2)} per minute)")
        else: out.append(f"  {name} ({len(runs)} runs): no [remoteplay-audio] clicks= line")
    if session == "presentation":
        out.append("  regionKind 0 = no window region; coversMonitor 1 = the video window covers the whole monitor; ownOverlaps = PushFPS windows over the picture; flipBlockers 0x0 = nothing the app knows of stops an independent flip. Whether Windows really composed the window: PresentMon's present mode in report.md (presentmon.py), only when PresentMon ran.")
    return out

def network_sections(net):
    if not net: return []
    head = [net["label"]] if net["label"] else []
    rows = []
    for a, row in net["arms"].items():
        f = row["facts"]; r = f["rtt"]
        unknown = UNITS_UNKNOWN
        rtt_cells = [unknown] * 4 if f.get("rttUndecided") else [fmt(r[0] if r else float("nan"), 1), fmt(percentile(r, .5), 1), fmt(percentile(r, .95), 1), fmt(r[-1] if r else float("nan"), 1)]
        rows.append([a] + rtt_cells + [
                     f"{unknown} / {unknown} / {fmt(f['requestedKbps'], 0)}" if f.get("rateUndecided") else f"{fmt(f['targetKbps'], 0)} / {fmt(f['measuredKbps'], 0)} / {fmt(f['requestedKbps'], 0)}", fmt(f["videoMbps"], 2), {True: "yes", False: "LITTLE", None: "unknown"}[f["motion"]] + (f" ({fmt(f['motionShare'] * 100, 0)} % of {f['motionBasis']})" if f["motion"] is not None else ""),
                     "; ".join(f"{k} {fmt(v, 0)} %" for k, v in f["profiles"].items()) or "n/a", "; ".join(f"{k} {fmt(v, 0)} %" for k, v in f["regimes"].items()) or "n/a",
                     str(f["reorderChanges"]), "; ".join(f"{k} ms {fmt(v, 0)} %" for k, v in f["reorderShares"].items()) or "n/a", fmt(f["addedWaitMs"], 2)])
    out = [("Network session: connection (SmartLink)", head + [net["arms"][net["reference"]]["connection"],
            f"Round trip from [slipstream-smartlink] (else [remoteplay-health] consoleRttMs; '{UNITS_UNKNOWN}' when the session's consoleUnits line says the unit could not be decided). Rates in kbit/s: the console's target / measured / requested. Motion: the median video rate against the console's target rate ({MOTION_TARGET_SHARE * 100:.0f} %) or, without one, the requested bitrate ({MOTION_REQUESTED_SHARE * 100:.0f} %); both arms must reach it and be at most {MOTION_TOLERANCE * 100:.0f} points apart, or the verdicts say 'not comparable'. Added wait = time at each reorder wait x its size, in ms on average: the latency-cost indicator of SmartLink's reorder decisions."],
            ["Arm", "RTT min", "RTT median", "RTT P95", "RTT max", "Target / measured / requested", "Video Mbit/s", "Motion", "Profile time", "Regime time", "Reorder changes", "Time at each reorder wait", "Added wait ms"], rows)]
    rows = []
    for a, row in net["arms"].items():
        for key, label, _lat in NETWORK_METRICS:
            m = row["metrics"][key]
            v = next((x for x in row["verdicts"] if x["metric"] == key), None)
            rows.append([a, label, str(m["n"]), fmt(m["mean"], 2), fmt(m["p95"], 2), fmt(m["p99"], 2), fmt(m["max"], 2),
                         "" if not v else (f"{fmt(v['difference'], 2)} [{fmt(v['low'], 2)}, {fmt(v['high'], 2)}]" if v["primary"] else f"{fmt(v['difference'], 2)}"), str(row["runs"]),
                         "reference" if a == net["reference"] else (v["verdict"] if v else "")])
    worse = [f"{a}: {w}" for a, row in net["arms"].items() for w in row["latencyWorse"]]
    gates = sorted({row["motionGate"] for row in net["arms"].values() if row.get("motionGate")})
    waits = [f"{a} {fmt(row['addedWait']['difference'], 2)} ms" for a, row in net["arms"].items() if row.get("addedWait")]
    out.append(("Network session: per arm against " + net["reference"], head + (["LATENCY WORSE (read this first): " + "; ".join(worse)] if worse else []) +
        ([MOTION_ADVICE] if any("hardly moved" in g for g in gates) else []) + [
        NETWORK_RULE, AUDIO_EXPECTATIONS, "Reorder added wait against " + net["reference"] + " (latency-cost indicator, descriptive): " + ("; ".join(waits) or "n/a") + ". Software counters only."],
        ["Arm", "Metric", "Windows", "Mean", "P95", "P99", "Max", "Difference of run means [interval]", "Runs", "Verdict"], rows))
    rows = []
    for a, row in net["arms"].items():
        for t in row.get("traces", []):
            if t["summary"] is None: rows.append([a, t["run"], "not used: " + t["problem"], ""]); continue
            rows.append([a, t["run"], str(t["summary"]["rows"]), "; ".join(f"{h} {fmt(m, 2)}/{fmt(p, 2)}/{fmt(x, 2)}" for h, (m, p, x) in t["summary"]["columns"].items())])
    if rows: out.append(("Network session: SmartLink trace (200 ms windows)", head + ["smartlink-trace.csv of each run: per column median / P95 / max. A file with anything but numbers is not used (the kit withholds it from the zip)."],
                         ["Arm", "Run", "Rows", "Columns: median / P95 / max"], rows))
    if net["builds"]:
        rows = [[a, str(b["seconds"]), fmt(b["under57Percent"], 1), fmt(b["replacedPercent"], 2), fmt(b["presentToScanoutP50"], 2), fmt(b["presentToScanoutP95"], 2)] for a, b in net["builds"].items()]
        out.append(("Network session: against the original and stable builds", head + ["Only what every build logs: seconds with fewer than 57 presents, frames replaced, present to scanout ([display-latency]). No verdict: these builds ran once each.", net["notCompared"]],
                    ["Arm", "Seconds", "Under 57 %", "Replaced %", "Present to scanout P50 ms", "P95 ms"], rows))
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

# --- kit version 5: MEASURED VALUES (scripts/remoteplay/testkit/measures.csv), the same rules as run-tests.ps1 -------------
# Get-MeasuredTables / Add-MeasuredSection. One table read by both, so the kit and this report cannot list different
# things; result.json 'measured' is compared cell by cell (DISAGREE). Software counters only.
MEASURED_PURPOSES = ("latency", "measure", "network", "quality", "probe")
MEASURE_PHASE_S = 3.0          # 'start' = within 3 s of the run's first line of the component (BENCH2: the slow regime lasted 1-3 s)
MEASURE_NUMBER = {}
MEASURE_STATS = ("median", "p01", "max", "last", "word", "rate-min", "rate-s", "slope-min", "first-s", "last-word")
MEASURE_SAMPLE_STATS = ("median", "p01", "max")          # samples.csv columns: no time-based statistic
# Sessions of kit 5 that are made of 'measure' runs (arms.csv set = the session's name), and what each page says first.
MEASURE_SESSIONS = ("input", "visual", "neural", "framegen", "audiofix", "pacing", "builds", "smoothness", "retest", "capture")
SESSION_FIRST_LINE = {
    "input": "These are times inside the PC (raw input to the pad report handed to the network), not input-to-photon; how fast the camera turns on screen needs a camera.",
    "visual": "Every picture path against v-original, the original stream untouched (each arm's 'against' line, v-dlss-k against v-dlss, and a verdict against v-original only by the rule below): software numbers, processing times of the app's own counters and no-reference picture metrics of the probe; not a judgement of how it looks on your screen and not input-to-photon.",
    "neural": "GPU cost of the neural layer at each internal size, with and without the upscaler to the display: the app's own counters, not input-to-photon.",
    "framegen": "Submitted FPS counts generated frames too and is NOT motion: 'real frames' are the stream's frames (a 30 FPS game repeats each picture inside the 60 FPS stream; no line here detects that). The real / generated split comes from the app's own counters ([frame-flow], [slipstream-fg]), not from an outside tool: PresentMon marks generated frames only in an application instrumented for it.",
    "audiofix": "Audio delay is a cost: a lower pop count bought with a longer delay is not a gain. Software estimates of the audio owner, not a measurement at the speaker.",
    "pacing": "One presentation switch per arm against p-window (windowed, the interface visible): software counters of presentation and frame pacing, not input-to-photon.",
    "capture": "Capture card: the game's own pictures (new pictures/s) against what was shown; generated frames between two identical pictures are copies and never count as motion; latency is a real frame's capture callback to its Present return (software, not photons). With these counters on, the card's callback compares every picture before handing it over, in every arm (about 0.3 ms at 1440p, more at 4K; estimate), so the passthrough arm's latency reads slightly high and the added delay of frame generation slightly low. Generated frames are never native PS5 frames.",
    "builds": "Regression proof: every build with the SAME test profile. A build that does not log a value shows n/a and is 'cannot tell' for it.",
    "smoothness": "The result is how EVENLY frames were PRESENTED ([slipstream-cadence]: the spacing of the Present calls, and with VRR off the fixed-refresh model of what the panel holds), not the submitted FPS the counter shows. What your eye sees only you can judge: watch every run.",
    "retest": "The same moving scene at 50 and 100 Mbit/s, three runs each, interleaved, with stream sync's audio and clock A/B arms at each bitrate: per run the delivered bitrate, the audio delay and the video clock lag (software estimates), the frames the console sent per second, resets by cause, and the probe's detail of the raw and the output picture; the matched pictures are in a separate zip, not in this one (tools/clarity_frames/compare.py). Software numbers, not input-to-photon.",
}

def measure_spec_path(folder):
    here = os.path.dirname(os.path.abspath(__file__))
    own = os.path.join(folder, "measures.csv")
    return own if os.path.exists(own) else os.path.join(here, "..", "..", "scripts", "remoteplay", "testkit", "measures.csv")

def share_stat(stat):
    """('ge' | 'lt', limit) of a share-ge:X / share-lt:X statistic, else None."""
    m = re.match(r"^share-(ge|lt):(-?[0-9]+(?:\.[0-9]+)?)$", stat)
    return (m.group(1), float(m.group(2))) if m else None

def first_lt_stat(stat):
    """The limit of a first-lt:X statistic (seconds until the first value under X), else None."""
    m = re.match(r"^first-lt:(-?[0-9]+(?:\.[0-9]+)?)$", stat)
    return float(m.group(1)) if m else None

def read_measure_spec(path):
    try: lines = [l for l in read_lines(path) if l.strip() and not l.lstrip().startswith("#")]
    except OSError: return []
    rows, seen = [], set()
    for raw in csv.DictReader(lines):
        row = {k: (v or "").strip() for k, v in raw.items() if k}
        stat = row.get("stat") or ""
        if stat not in MEASURE_STATS and not share_stat(stat) and first_lt_stat(stat) is None: raise ValueError(f"measures.csv: unknown stat '{stat}'")
        if row["component"] == "samples" and stat not in MEASURE_SAMPLE_STATS: raise ValueError(f"measures.csv: samples rows take median, p01 or max, not '{stat}'")
        if row.get("phase") not in ("all", "start", "steady"): raise ValueError(f"measures.csv: unknown phase '{row.get('phase')}'")
        row["key"] = row["group"] + "|" + row["component"] + "|" + (row.get("filter") or "") + "|" + (row.get("field") or "") + "|" + stat
        if row["key"] in seen: raise ValueError(f"measures.csv: two rows are the same value: {row['key']}")
        seen.add(row["key"])
        rows.append(row)
    return rows

def log_number(text, name):
    """The number after name= (Get-LogNumber: case-insensitive, not after a letter)."""
    m = MEASURE_NUMBER.get(name)
    if m is None: m = MEASURE_NUMBER[name] = re.compile(r"(?<![A-Za-z])" + re.escape(name) + r"=(-?[0-9]+(?:\.[0-9]+)?)", re.I)
    found = m.search(text)
    return float(found.group(1)) if found else None

def log_word(text, name):
    """The text after name= up to the next blank (Get-LogWord): keeps commas, so detail=a:ok,b:refused:why stays whole."""
    m = re.search(r"(?<![A-Za-z])" + re.escape(name) + r"=(\S+)", text)
    return m.group(1) if m else None

def lin_quantile(sorted_values, p):
    """Get-Quantile: linear interpolation between ranks."""
    n = len(sorted_values)
    if not n: return float("nan")
    k = (n - 1) * p; lo = int(math.floor(k)); hi = min(lo + 1, n - 1)
    return sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * (k - lo)

def most_common(values):
    counts, order = {}, []
    for v in values:
        if v not in counts: counts[v] = 0; order.append(v)
        counts[v] += 1
    best = order[0]
    for v in order:
        if counts[v] > counts[best]: best = v
    return best

def measure_lines(run):
    """(seconds, component, text) of every stamped line of a run's bench log."""
    cached = run.get("measure_lines")
    if cached is not None: return cached
    out = []
    for raw in run.get("lines") or []:
        m = LINE.match(raw.lstrip("﻿"))
        if m and utc(m.group(1)) is not None: out.append((utc(m.group(1)), m.group(3), m.group(4)))
    run["measure_lines"] = out
    return out

def measure_filter_ok(text, flt):
    if not flt: return True
    for pair in flt.split("&"):
        k, _, v = pair.partition("=")
        if log_word(text, k) != v: return False
    return True

def counter_rise(points):
    """Rises of a running counter: a fall is a restart (the new value counts from zero)."""
    rise = 0.0
    for (_t0, a), (_t1, b) in zip(points, points[1:]): rise += (b - a) if b >= a else b
    return rise

def span_seconds(points):
    """Seconds from the first to the last point, to the millisecond of the log's stamps (Get-SpanSeconds): the two readers
    count time from different days, and a rate over a short span must not depend on that."""
    return round(points[-1][0] - points[0][0], 3)

def measure_cell(spec, runs):
    """One value of measures.csv over the runs of one arm: {value, n, text}."""
    stat, field, comp = spec["stat"], spec.get("field") or "", spec["component"]
    skip_first, skip_last = int(spec.get("skipFirst") or 0), int(spec.get("skipLast") or 0)
    phase = spec.get("phase") or "all"
    first_lt = first_lt_stat(stat)
    per_run = []
    for run in runs:
        points = []
        if comp == "samples":
            for t, row in run.get("samples") or []:
                v = row.get(field)
                if v is not None and v != -1: points.append((t, v))
        else:
            lines = measure_lines(run)
            t_first = next((t for t, c, _x in lines if c == comp), None)
            for t, c, text in lines:
                if c != comp or not measure_filter_ok(text, spec.get("filter") or ""): continue
                since = round(t - t_first, 3)          # to the millisecond of the log's stamps: a line exactly 3.000 s later is 'start' in both readers
                if phase == "start" and since > MEASURE_PHASE_S: continue
                if phase == "steady" and since <= MEASURE_PHASE_S: continue
                if stat == "first-s": points.append((t, 0.0)); continue
                if stat in ("word", "last-word"):
                    v = log_word(text, field)
                    if v is not None: points.append((t, v))
                    continue
                v = log_number(text, field)
                if v is not None and v != -1: points.append((t, v))
            if stat == "first-s" or first_lt is not None:
                if first_lt is not None: points = [(t, v) for t, v in points if v < first_lt]
                frames = next((t for t, c, _x in lines if c == "frame-rate"), None)
                origin = frames if frames is not None else (lines[0][0] if lines else None)
                per_run.append([] if not points or origin is None else [(points[0][0], round(points[0][0] - origin, 3))])
                continue
        if skip_first: points = points[skip_first:]
        if skip_last: points = points[:max(0, len(points) - skip_last)]
        per_run.append(points)
    pooled_values = [v for points in per_run for _t, v in points]
    out = {"value": None, "n": 0, "text": "n/a"}
    digits = int(spec.get("digits") or 0)
    unit = (" " + spec["unit"]) if spec.get("unit") else ""
    def number_cell(value, n):
        out.update(value=value, n=n, text=(fmt(value, digits) + unit) if value == value else "n/a")
    share = share_stat(stat)
    if stat == "word":
        if pooled_values: out.update(value=None, n=len(pooled_values), text=most_common(pooled_values))
    elif stat == "last-word":
        last_words = [points[-1][1] for points in per_run if points]
        if last_words: out.update(value=None, n=len(last_words), text=most_common(last_words))
    elif stat in ("median", "p01", "max") or share:
        if pooled_values:
            ordered = sorted(pooled_values)
            if stat == "median": value = lin_quantile(ordered, 0.5)
            elif stat == "p01": value = lin_quantile(ordered, 0.01)
            elif stat == "max": value = ordered[-1]
            elif share[0] == "ge": value = 100.0 * sum(1 for v in ordered if v >= share[1]) / len(ordered)
            else: value = 100.0 * sum(1 for v in ordered if v < share[1]) / len(ordered)
            number_cell(value, len(ordered))
    elif stat == "last":
        lasts = sorted(points[-1][1] for points in per_run if points)
        if lasts: number_cell(lin_quantile(lasts, 0.5), len(lasts))
    elif stat in ("rate-min", "rate-s"):
        rise = seconds = 0.0; used = 0
        for points in per_run:
            if len(points) >= 2 and points[-1][0] > points[0][0]:
                rise += counter_rise(points); seconds += span_seconds(points); used += 1
        if seconds > 0: number_cell(rise / seconds * (60.0 if stat == "rate-min" else 1.0), used)
    elif stat == "slope-min":
        slopes = sorted((points[-1][1] - points[0][1]) / (span_seconds(points) / 60.0) for points in per_run if len(points) >= 2 and points[-1][0] > points[0][0])
        if slopes: number_cell(lin_quantile(slopes, 0.5), len(slopes))
    elif stat == "first-s" or first_lt is not None:
        firsts = sorted(points[0][1] for points in per_run if points)
        if firsts:
            number_cell(lin_quantile(firsts, 0.5), len(firsts))
            out["text"] += f" (in {len(firsts)} of {len(runs)} runs)"
        else: out["text"] = f"never (0 of {len(runs)} runs)"
    return out

def measured_arms(data):
    """Arms with measured runs, in arms.csv order, then the others by name: [(arm, runs)]."""
    runs = [r for r in data["runs"] if r["purpose"] in MEASURED_PURPOSES and not as_dict(r.get("meta")).get("setAside")]
    names = [n for n in data["table"] if any(r["arm"] == n for r in runs)] + sorted({r["arm"] for r in runs} - set(data["table"]))
    return [(a, sorted([r for r in runs if r["arm"] == a], key=lambda r: r["name"])) for a in names]

# The retest session (stream clarity, round 2; run-tests.ps1 $perRunLines): one line per run with that run's own values.
PER_RUN_ROWS = (("stream rate", "delivered median"), ("stream rate", "delivered max"), ("stream rate", "frames received per second"), ("audio", "delay at end"), ("audio", "delay growth"), ("audio", "video clock lag at end"),
                ("audio", "video clock lag growth"), ("quality raw", "detail"), ("quality output", "detail"))
SYNC_NOT_CAUSES = ("episodes", "depthAtLast", "refusedWaiting", "staleClockTolerated", "callbackRejected")
SYNC_COUNTERS = (("remoteplay-health", "decoderResets", "decoder resets", "sum", 0, ""), ("remoteplay-health", "frameDrops", "frame drops", "sum", 0, ""),
                 ("slipstream-net", "recoveries", "recoveries", "sum", 0, ""), ("slipstream-net", "ingressDropped", "ingress dropped", "sum", 0, ""),
                 ("slipstream-net", "waitingIdrMs", "waiting for a keyframe max", "max", 0, " ms"), ("slipstream-net", "gapMaxMs", "frame gap max", "max", 1, " ms"),
                 ("remoteplay-transport", "callbackRejected", "callbacks rejected", "last", 0, ""), ("remoteplay-transport", "assemblyErrors", "assembly errors", "last", 0, ""))
LOG_NUMBER = r"=(-?[0-9]+(?:\.[0-9]+)?)"
def log_number(text, name):
    """Get-LogNumber: the number after name= (digits, optional sign and decimals), None without one."""
    m = re.search(r"(?<![A-Za-z])" + re.escape(name) + LOG_NUMBER, text)
    return float(m.group(1)) if m else None

def sync_run_parts(run):
    """run-tests.ps1 Get-SyncRunParts, the same words: stream sync's per-run facts in the retest session (the console's
    frame rate from [remoteplay-clock] wireFps without the -1 'unknown', clockRate, pictureTerm, resets and drops summed
    over the 5 s windows, the transport's counters at the end, and the last 'causes' line of [remoteplay-recovery] or
    [remoteplay-ingress])."""
    lines = measure_lines(run)
    wire, clock_rate, term = [], None, "not logged"
    for _t, c, text in lines:
        if c == "remoteplay-clock":
            v = log_number(text, "wireFps")
            if v is not None and v != -1: wire.append(v)
            w = log_word(text, "clockRate")
            if w is not None: clock_rate = w
        elif c == "remoteplay-audio":
            w = log_word(text, "pictureTerm")
            if w is not None: term = w
    parts = [f"console frames per second (wireFps) {fmt(lin_quantile(sorted(wire), 0.5), 2)}" if wire else "console frames per second (wireFps) not logged",
             "video clock follows the console " + (clock_rate if clock_rate is not None else "not logged"), "audio picture term " + term]
    for component, field, label, how, digits, unit in SYNC_COUNTERS:
        value = None
        for _t, c, text in lines:
            if c != component: continue
            v = log_number(text, field)
            if v is None or v == -1: continue
            if value is None: value = v
            elif how == "sum": value += v
            elif how == "max": value = max(value, v)
            else: value = v
        parts.append(f"{label} not logged" if value is None else f"{label} {fmt(value, digits)}{unit}")
    causes = None
    for t, c, text in lines:
        if c in ("remoteplay-recovery", "remoteplay-ingress") and text.startswith("causes "):
            if causes is None or t >= causes[0]: causes = (t, text)
    if causes is None: parts.append("resets by cause not logged")
    else:
        head = causes[1].split(" (", 1)[0]
        named = [f"{k}={v}" for k, v in re.findall(r"(?<![A-Za-z])([A-Za-z]+)=(-?[0-9]+)", head) if k not in SYNC_NOT_CAUSES and v != "0"]
        episodes = log_word(head, "episodes")
        parts.append(f"resets by cause {' '.join(named) if named else 'none'} (episodes {episodes if episodes is not None else 'n/a'})")
    return parts

def per_run_lines(data, measured):
    """{arm: ['run <name>: group label value, ..., matched pictures saved N']} in the retest session, runs in name (time) order."""
    if str(data["kit"].get("session") or "") != "retest": return {}
    out = {}
    for arm, runs in measured_arms(data):
        lines = []
        for run in runs:
            parts = []
            for group, label in PER_RUN_ROWS:
                for row in measured["spec"]:
                    if row["group"] == group and row["label"] == label: parts.append(f"{group} {label} {measure_cell(row, [run])['text']}")
            saved = sum(1 for _t, c, text in measure_lines(run) if c == "slipstream-quality" and text.startswith("event=frames-saved "))
            parts.append(f"matched pictures saved {saved}")
            parts += sync_run_parts(run)
            lines.append(f"run {run['name']}: " + ", ".join(parts))
        out[arm] = lines
    return out

# HDR or SDR (kit 5, research of 20 Nov: the neural layer is undocumented on HDR video). In the sessions that compare
# pictures every run says which it was, and an HDR arm is never compared with an SDR one.
PICTURE_SESSIONS = ("visual", "neural", "retest")
PICTURE_RULE = ("HDR or SDR per run: the stream's own picture, streamHdr= of the run's last [slipstream-pipeline] line (the codec chosen before connecting; "
                "the pipeline key hdr= only switches the RTX Video HDR conversion), or hdr= of [slipstream-config] in a run without that line. An arm is compared "
                "with its pair (and gets a verdict) only when every run of both was the same of the two; the neural layer is undocumented on HDR video, so HDR "
                "and SDR numbers are never mixed.")

def stream_hdr_words(run):
    """The run's last [slipstream-pipeline] streamHdr= ('0' | '1'), None when no such line says it."""
    word = None
    for _t, c, text in measure_lines(run):
        if c != "slipstream-pipeline": continue
        v = log_word(text, "streamHdr")
        if v in ("0", "1"): word = v
    return word

def picture_states(runs):
    """Per run 'HDR' | 'SDR' | 'not logged': the stream's HDR (streamHdr= of the last [slipstream-pipeline] line); a run
    without it: the last [slipstream-config] hdr= (older builds; that value also counts the HDR conversion)."""
    out = []
    for run in runs:
        stream = stream_hdr_words(run)
        if stream is not None: out.append("HDR" if stream == "1" else "SDR"); continue
        state = "not logged"
        for _t, c, text in measure_lines(run):
            if c != "slipstream-config": continue
            v = log_word(text, "hdr")
            if v == "1": state = "HDR"
            elif v == "0": state = "SDR"
        out.append(state)
    return out

# The owner's reference (v-reference) is an HDR stream: the stream's codec is chosen before connecting (saved with the PS5
# pairing; no key of VEYRA_TEST_RP_PIPELINE can change it). A run whose stream was SDR did not reproduce it: said, never silent.
REFERENCE_SDR = "reference could not be reproduced: stream was SDR"
REFERENCE_HOW = (" (the reference needs the H.265 HDR stream, chosen in the PS5 connect dialog before the session; the kit cannot change the codec "
                 "saved with the pairing); its values are those of an SDR run")

def reference_lines(data):
    """{v-reference: text} when the visual session's reference arm did not run on an HDR stream in every run ({} = it did)."""
    if str(data["kit"].get("session") or "") != "visual": return {}
    for arm, runs in measured_arms(data):
        if arm != VISUAL_REFERENCE: continue
        words = [stream_hdr_words(run) for run in runs]
        sdr = sum(1 for w in words if w == "0"); missing = sum(1 for w in words if w is None)
        if words and sdr == len(words): return {arm: REFERENCE_SDR + REFERENCE_HOW}
        if sdr: return {arm: f"{REFERENCE_SDR} in {sdr} of {len(words)} runs" + REFERENCE_HOW}
        if missing: return {arm: f"reference could not be reproduced: the stream's HDR was not logged in {missing} of {len(words)} runs (no streamHdr= in [slipstream-pipeline])"}
    return {}

def picture_of(states):
    """'HDR' | 'SDR' when every run was that, 'not logged' when none said, else 'mixed'."""
    kinds = set(states)
    if len(kinds) == 1: return states[0]
    return "not logged" if not kinds else "mixed"

def picture_blocks(measured, arm, base):
    """Why arm and base are not compared in a picture session ('' = they are)."""
    if measured["session"] not in PICTURE_SESSIONS: return ""
    a, b = picture_of(measured["arms"][arm]["picture"]), picture_of(measured["arms"][base]["picture"])
    if a == b and a in ("HDR", "SDR"): return ""
    return f"this arm: {a}, {base}: {b}; HDR and SDR runs are never mixed in one comparison, and runs that do not say which they were are not compared"

def measured_tables(data, spec=None):
    """result.json 'measured' of the kit: per arm the runs and every cell of the groups this session shows; for the rows
    marked compare=1 also the value of each run on its own (the run-to-run spread rule needs them)."""
    if spec is None: spec = read_measure_spec(measure_spec_path(data["folder"]))
    session = str(data["kit"].get("session") or "")
    shown = [row for row in spec if row["sessions"] == "*" or session in row["sessions"].split("|")]
    arms = {}
    for arm, runs in measured_arms(data):
        cells = {row["key"]: measure_cell(row, runs) for row in shown}
        per_run = {row["key"]: [measure_cell(row, [run])["value"] for run in runs] for row in shown if row.get("compare") == "1"}
        arms[arm] = {"runs": len(runs), "cells": cells, "perRun": per_run, "picture": picture_states(runs)}
    return {"session": session, "groups": list(dict.fromkeys(row["group"] for row in shown)), "spec": shown, "arms": arms}

def measured_group_text(measured, arm, group):
    """'label value, label value' of one group of one arm (Get-MeasuredGroupText); 'not logged' when no row has a value."""
    cells = measured["arms"][arm]["cells"]
    rows = [row for row in measured["spec"] if row["group"] == group]
    if not any(cells[row["key"]]["n"] for row in rows): return "not logged in this arm's runs"
    return ", ".join(row["label"] + " " + cells[row["key"]]["text"] for row in rows)

# The run-to-run spread rule (every A/B line of the section; fourth directive: "prove every improvement"): written out in
# SPREAD_RULE, the same in run-tests.ps1 Get-SpreadVerdict.
SPREAD_MIN_RUNS = 2
SPREAD_WORDS = ("differs", "same within run-to-run spread", "cannot tell")
SPREAD_GAP_FACTOR = 2.0
SPREAD_RULE = ("Comparison rule (per value, each arm against its pair; a run = one start): 'same within run-to-run spread' when the ranges of the per-run "
               "values of the two arms overlap; 'differs' when they do not overlap AND the gap between the two ranges is more than twice the larger "
               "range; 'cannot tell' otherwise, and whenever an arm has fewer than 2 runs with that value. With 2 runs per arm only large differences "
               "can show, and identical arms would still be called 'differs' for about 1 value in 25 by chance (a model of independent normal noise, "
               "not a measurement); 'same' is not proof of equality. No better / worse word comes from this rule.")

def spread_verdict(mine, base):
    mine = sorted(v for v in mine if v is not None and v == v); base = sorted(v for v in base if v is not None and v == v)
    if len(mine) < SPREAD_MIN_RUNS or len(base) < SPREAD_MIN_RUNS: return "cannot tell"
    if mine[0] <= base[-1] and base[0] <= mine[-1]: return "same within run-to-run spread"
    spread = max(mine[-1] - mine[0], base[-1] - base[0])
    gap = max(mine[0] - base[-1], base[0] - mine[-1])
    return "differs" if gap > SPREAD_GAP_FACTOR * spread else "cannot tell"

# The audio own clock (S5_PRESENT_API.md, comparison A): ownFrames= must grow. An arm whose arrival lookup failed ran the
# default rule: it is reported as a failed arm, not as a result, and is not compared.
OWN_CLOCK_SWITCH = "VEYRA_EXP_RP_AUDIO_OWN_CLOCK=1"
OWN_CLOCK_RAN = "own clock ran"
def own_clock_lines(data):
    out = {}
    for arm, runs in measured_arms(data):
        if OWN_CLOCK_SWITCH not in ((data["table"].get(arm) or {}).get("switches") or "").split(";"): continue
        seen = 0; frames = 0.0
        for run in runs:
            for _t, c, text in measure_lines(run):
                if c != "remoteplay-audio" or log_word(text, "ownClock") != "1": continue
                v = log_number(text, "ownFrames")
                if v is not None: seen += 1; frames = max(frames, v)
        if not seen: out[arm] = "own clock: NOT A RESULT, no [remoteplay-audio] line says ownClock=1 with ownFrames= (a build without the switch): the arm ran the default rule"
        elif frames <= 0: out[arm] = "own clock: FAILED ARM, not a result: ownFrames stayed 0 (the arrival of the shown frames was not found), so the arm ran the default rule"
        else: out[arm] = OWN_CLOCK_RAN + ": ownFrames reached " + fmt(frames, 0)
    return out

# A feature the build does not have (kit 5, rolling releases 8A / 8B / 8C): the arm still ran and says so, and it is
# neither compared nor judged. Pipeline arms without any [slipstream-pipeline] line ran the build's own settings (a build
# without VEYRA_TEST_RP_PIPELINE: Package 7, the original, the stable candidate); input arms without any
# [slipstream-input] line had no input core (a build before 8B) or the keyboard-and-mouse object did not start.
PIPELINE_MISSING = ("pipeline keys not applied: this build has no VEYRA_TEST_RP_PIPELINE (no [slipstream-pipeline] line in any run), "
                    "so this arm ran the build's own settings; it is neither compared nor judged")
OVERRIDE_MISSING = ("test override not applied: a run's [remoteplay] session started request is not the arm's VEYRA_TEST_RP_BITRATE_KBPS / "
                    "VEYRA_TEST_RP_CODEC (a build without the clarity switches, or a refused value), so this arm ran the saved stream settings; "
                    "it is neither compared nor judged")
CODEC_WORDS = {"h264": ("h264", "H264"), "hevc": ("hevc", "H265"), "hevc-hdr": ("hevc-hdr", "H265_HDR")}
def override_missing(table, arm, runs):
    """Stream clarity (run-tests.ps1 Test-OverrideMissing): a run of the arm logged a session request that is not the
    bitrate or codec its VEYRA_TEST_RP_BITRATE_KBPS / VEYRA_TEST_RP_CODEC switches ask for."""
    want_kbps = want_codec = ""
    for pair in ((table.get(arm) or {}).get("switches") or "").split(";"):
        m = re.match(r"^VEYRA_TEST_RP_BITRATE_KBPS=(\d+)$", pair)
        if m: want_kbps = m.group(1); continue
        m = re.match(r"^VEYRA_TEST_RP_CODEC=(\S+)$", pair)
        if m: want_codec = m.group(1)
    if not want_kbps and not want_codec: return False
    for run in runs:
        for _t, component, text in measure_lines(run):
            if component != "remoteplay": continue
            m = re.match(r"^session started .*codec=(\S+) requestedBitrateKbps=(\d+)", text)
            if not m: continue
            if want_kbps and m.group(2) != want_kbps: return True
            if want_codec in CODEC_WORDS and m.group(1) not in CODEC_WORDS[want_codec]: return True
    return False
INPUT_MISSING = ("input: not in this build (no [slipstream-input] line in any run: the build has no input core, or the keyboard-and-mouse "
                 "object did not start); the session went on")
def feature_lines(data):
    """{arm: text} for the arms whose build lacked the feature the arm measures."""
    out = {}
    session = str(data["kit"].get("session") or "")
    for arm, runs in measured_arms(data):
        components = {c for run in runs for _t, c, _x in measure_lines(run)}
        if arm_pipeline(data["table"], arm) and "slipstream-pipeline" not in components: out[arm] = PIPELINE_MISSING
        elif override_missing(data["table"], arm, runs): out[arm] = OVERRIDE_MISSING
        elif session == "input" and "slipstream-input" not in components: out[arm] = INPUT_MISSING
    return out

# The owner's report of 21 Nov (smooth at about 120 FPS only sometimes): per arm, how evenly frames were PRESENTED, from
# the present stream's 5 s [slipstream-cadence] line (S5_PRESENT_API.md). A word only by this rule; run-tests.ps1 the same.
CADENCE_EVEN_PCT = 2.0
CADENCE_UNEVEN_PCT = 10.0
CADENCE_RULE = ("Cadence rule (per arm, the 5 s [slipstream-cadence] windows of all its runs; VRR on or off from the app's own [display-vrr] "
                "active= lines of those runs, beside what the arm asked for): with VRR active throughout the share of uneven presents is offPct; "
                "with VRR off throughout it is the larger of offPct and fixedUnevenPct (the fixed-refresh model); 'even' when the median share "
                "is 2 % or less, 'uneven' when it is 10 % or more, 'cannot tell' otherwise, and whenever VRR changed during the runs, no "
                "[display-vrr] line said it, or no window was logged. A word about presenting only: not about what the panel or your eye showed.")
def cadence_lines(data):
    """{arm: 'presented cadence: even | uneven | cannot tell (...)'} in the smoothness session."""
    if str(data["kit"].get("session") or "") != "smoothness": return {}
    out = {}
    for arm, runs in measured_arms(data):
        modes, windows = set(), []
        for run in runs:
            for _t, c, text in measure_lines(run):
                if c == "display-vrr":
                    active = log_word(text, "active")
                    if active in ("true", "false"): modes.add(active)
                elif c == "slipstream-cadence": windows.append((log_number(text, "offPct"), log_number(text, "fixedUnevenPct")))
        asked = "no" if "pref:vrr=false" in ((data["table"].get(arm) or {}).get("switches") or "").split(";") else "yes"
        if not windows:
            out[arm] = "presented cadence: cannot tell (no [slipstream-cadence] line: a build without it, or the statistics switch was off)"; continue
        if not modes:
            out[arm] = "presented cadence: cannot tell (no [display-vrr] line said whether VRR was active)"; continue
        if len(modes) > 1:
            out[arm] = "presented cadence: cannot tell (VRR changed during the runs: active=" + "/".join(sorted(modes)) + "; asked for: " + asked + ")"; continue
        on = next(iter(modes)) == "true"
        shares = []
        for off, fixed in windows:
            if off is None or off == -1: continue
            shares.append(max(off, fixed) if not on and fixed is not None and fixed != -1 else off)
        if not shares:
            out[arm] = "presented cadence: cannot tell (no offPct in the [slipstream-cadence] lines)"; continue
        share = lin_quantile(sorted(shares), 0.5)
        word = "even" if share <= CADENCE_EVEN_PCT else "uneven" if share >= CADENCE_UNEVEN_PCT else "cannot tell"
        out[arm] = f"presented cadence: {word} (VRR {'on' if on else 'off'}, asked for: {asked}; uneven presents {fmt(share, 1)} % median of {len(shares)} windows)"
    return out

def compare_blocks(data, measured, arm, base):
    """Why arm is not compared with base ('' = it is): HDR against SDR in a picture session, or a failed own-clock arm."""
    features = feature_lines(data)
    if features.get(arm) == PIPELINE_MISSING: return "pipeline keys not applied in this arm (this build has no VEYRA_TEST_RP_PIPELINE)"
    if features.get(base) == PIPELINE_MISSING: return "pipeline keys not applied in " + base + " (that build has no VEYRA_TEST_RP_PIPELINE)"
    if features.get(arm) == OVERRIDE_MISSING: return "test override not applied in this arm (the request was not the arm's bitrate / codec)"
    if features.get(base) == OVERRIDE_MISSING: return "test override not applied in " + base + " (the request was not its bitrate / codec)"
    why = picture_blocks(measured, arm, base)
    if why: return why
    own = own_clock_lines(data).get(arm)
    if own is not None and not own.startswith(OWN_CLOCK_RAN): return own
    return ""

def measured_compared(data, measured):
    """{arm: {key: word}} for every arm whose pair (arms.csv compare) is another measured arm of the folder."""
    out = {}
    for arm, cell in measured["arms"].items():
        base = str((data["table"].get(arm) or {}).get("compare") or "")
        if not base or base == arm or base not in measured["arms"]: continue
        if compare_blocks(data, measured, arm, base): continue
        out[arm] = {key: spread_verdict(values, measured["arms"][base]["perRun"].get(key, [])) for key, values in cell["perRun"].items()}
    return out

def measured_not_compared(data, measured):
    """{arm: text} for the arms of a picture session whose pair ran in the other of HDR / SDR (or did not say)."""
    out = {}
    for arm in measured["arms"]:
        base = str((data["table"].get(arm) or {}).get("compare") or "")
        if not base or base == arm or base not in measured["arms"]: continue
        why = compare_blocks(data, measured, arm, base)
        if why: out[arm] = "not compared with " + base + ": " + why
    return out

def compared_lines(data, measured, compared):
    """'against <pair>: differs: ...; same within run-to-run spread: ...; cannot tell: ...' per arm (Add-MeasuredSection)."""
    out = {}
    for arm, words_of in compared.items():
        base = str(data["table"][arm]["compare"])
        parts = []
        for word in SPREAD_WORDS:
            names = []
            for row in measured["spec"]:
                if words_of.get(row["key"]) != word: continue
                name = row["group"] + " " + row["label"]
                if word == "differs": name += " (" + measured["arms"][arm]["cells"][row["key"]]["text"] + " / " + measured["arms"][base]["cells"][row["key"]]["text"] + ")"
                names.append(name)
            if names: parts.append(word + ": " + ", ".join(names))
        out[arm] = "against " + base + ": " + ("; ".join(parts) if parts else "no value marked for comparison in this session")
    return out

# Visual verdict rule (second directive: "measurable image-quality gains and acceptable P95/P99 processing latency"):
# against v-original, on the OUTPUT picture of the probe (PushImageMetrics.h, no-reference software numbers).
VISUAL_BASE = "v-original"
VISUAL_REFERENCE = "v-reference"
VISUAL_DEFECTS = ("blockiness", "banding", "ringing", "flicker")     # hudDelta: never against v-original (about 0 there)
VISUAL_CLEANED = ("blockiness", "banding", "ringing")
VISUAL_CHANGE = 0.10                 # a defect metric must move by at least 10 % to count
VISUAL_DETAIL_KEEP = 0.95            # detail must stay at least 95 % of the original's for "better"
VISUAL_DETAIL_LOSS = 0.90            # detail below 90 % of the original's counts as worse
VISUAL_MIN_PROBES = 2                # one probe per run, two rounds
PROCESSING_BUDGET_MS = 1000.0 / (2 * 60.0)   # half a stream frame interval at 60 FPS: 8.33 ms graph P95
VISUAL_QUALITY_KEY = "quality output|slipstream-quality|stage=output|{}|median"
VISUAL_PROCESSING_KEY = "processing|slipstream-nr||graphMsP95|median"
VISUAL_RULE = ("Verdict rule (software picture metrics of the shown picture, against v-original; no word of beauty): 'better' only when "
               "blockiness, banding or ringing is at least 10 % lower, no defect metric (blockiness, banding, ringing, flicker) "
               "is 10 % or more higher, detail stays at 95 % or more, AND the graph processing P95 is at most 8.33 ms (half a 60 FPS frame); "
               "'worse' when a defect metric is 10 % or more higher or detail falls below 90 % (processing time measured); "
               "'cannot tell' otherwise; 'no verdict' when an arm has fewer than 2 probes or no processing time. "
               "HUD delta is not compared with v-original (it measures the output against the raw picture, so it is about 0 there), and "
               "blockiness counts only when the arm's output size is v-original's (a 1080 to 1440 scaler blurs the 8-pixel grid it looks for).")
VISUAL_SIZE_KEY = "quality output|slipstream-quality|stage=output|size|word"
VISUAL_SIZE_NOTE = "; blockiness not comparable: different output size"

def visual_verdict(arm, base, processing_p95, sizes=None):
    """arm, base: {metric: (median, n)} of the output picture; processing_p95: graph P95 ms (None = not logged); sizes:
    (arm, v-original) output size texts: blockiness counts only when both are known and equal (None = equal)."""
    names = VISUAL_DEFECTS + ("detail",)
    if any(arm.get(k, (None, 0))[1] < VISUAL_MIN_PROBES or base.get(k, (None, 0))[1] < VISUAL_MIN_PROBES for k in names):
        return "no verdict: fewer than 2 probes with picture numbers in this arm or in v-original"
    if processing_p95 is None or processing_p95 != processing_p95: return "no verdict: no processing time ([slipstream-nr] graphMsP95) in this arm"
    same = sizes is None or (sizes[0] == sizes[1] and sizes[0] not in ("", "n/a"))
    note = "" if same else f"{VISUAL_SIZE_NOTE} ({sizes[0]} / {sizes[1]})"
    lower = [k for k in VISUAL_CLEANED if (same or k != "blockiness") and arm[k][0] <= base[k][0] * (1 - VISUAL_CHANGE) and arm[k][0] < base[k][0]]
    higher = [k for k in VISUAL_DEFECTS if (same or k != "blockiness") and arm[k][0] >= base[k][0] * (1 + VISUAL_CHANGE) and arm[k][0] > base[k][0]]
    detail = arm["detail"][0] / base["detail"][0] if base["detail"][0] > 0 else 1.0
    if higher or detail < VISUAL_DETAIL_LOSS:
        why = [k + " higher" for k in higher] + (["detail lower"] if detail < VISUAL_DETAIL_LOSS else [])
        return f"worse on these software picture metrics ({', '.join(why)}; processing P95 {fmt(processing_p95, 2)} ms{note})"
    if lower and detail >= VISUAL_DETAIL_KEEP:
        if processing_p95 <= PROCESSING_BUDGET_MS:
            return f"better on these software picture metrics ({', '.join(k + ' lower' for k in lower)}; processing P95 {fmt(processing_p95, 2)} ms, within 8.33 ms{note})"
        return f"cannot tell: cleaner on the picture metrics ({', '.join(k + ' lower' for k in lower)}) but processing P95 {fmt(processing_p95, 2)} ms is over 8.33 ms{note}"
    return "cannot tell: the picture metrics differ by less than 10 % or in both directions" + note

def visual_verdicts(measured, runtime=None, features=None):
    """runtime: neural_runtime_lines of the folder (an arm whose neural layer did not run gets no verdict); features:
    feature_lines of the folder (an arm whose pipeline keys were not applied gets no verdict)."""
    if measured["session"] != "visual" or VISUAL_BASE not in measured["arms"]: return {}
    def metrics(arm):
        cells = measured["arms"][arm]["cells"]
        return {k: (cells.get(VISUAL_QUALITY_KEY.format(k), {}).get("value"), cells.get(VISUAL_QUALITY_KEY.format(k), {}).get("n", 0)) for k in VISUAL_DEFECTS + ("detail",)}
    base = metrics(VISUAL_BASE)
    out = {}
    for arm in measured["arms"]:
        if arm == VISUAL_BASE: out[arm] = "reference of the comparison"; continue
        if arm == VISUAL_REFERENCE: out[arm] = "the owner's reference settings (4K output, HDR, frame generation): measured values only, no verdict"; continue
        if (features or {}).get(arm) == PIPELINE_MISSING or (features or {}).get(VISUAL_BASE) == PIPELINE_MISSING:
            out[arm] = "no verdict: the pipeline keys were not applied (this build has no VEYRA_TEST_RP_PIPELINE)"; continue
        if arm in (runtime or {}) and neural_skipped(runtime[arm]): out[arm] = "no verdict: the neural layer did not run in this arm (the line above says why); its values are those of the path without it"; continue
        if picture_blocks(measured, arm, VISUAL_BASE): out[arm] = "no verdict: " + picture_blocks(measured, arm, VISUAL_BASE); continue
        size = lambda a: str(measured["arms"][a]["cells"].get(VISUAL_SIZE_KEY, {}).get("text") or "n/a")
        out[arm] = visual_verdict(metrics(arm), base, measured["arms"][arm]["cells"].get(VISUAL_PROCESSING_KEY, {}).get("value"), (size(arm), size(VISUAL_BASE)))
    return out

def arm_pipeline(table, arm):
    """The VEYRA_TEST_RP_PIPELINE text of an arm (arms.csv 'pipeline:a=b|c=d' entry), '' without one."""
    for pair in ((table.get(arm) or {}).get("switches") or "").split(";"):
        if pair.startswith("pipeline:"): return pair[len("pipeline:"):].replace("|", ";")
    return ""

NEURAL_KEYS = ("nr", "nrRuntime", "nrSize", "nrLook")
NEURAL_RAN = ("neural runtime available", "neural layer applied")
NEURAL_WITHOUT = "; the session went on, and the values of this arm are WITHOUT the neural layer"
def neural_skipped(text):
    """True when the line of neural_runtime_lines says the neural layer did not run (or that nothing tells)."""
    return not str(text).startswith(NEURAL_RAN)

def neural_runtime_lines(data):
    """Per arm that asks for the neural layer (nr=1): what its runtime did, never a blank. A runtime that is not there is
    'not installed: skipped' (the installer keeps the NVIDIA file only on an explicit yes): the arm still ran, without it."""
    out = {}
    for arm, runs in measured_arms(data):
        if "nr=1" not in arm_pipeline(data["table"], arm).split(";"): continue
        status = detail = None
        for run in runs:
            for _t, c, text in measure_lines(run):
                if c == "nr-runtime-status": status = text
                elif c == "slipstream-pipeline": detail = log_word(text, "detail") or ""
        word = lambda name: (log_word(status, name) or "?") if status is not None else "?"
        refused = [d for d in (detail or "").split(",") if d.split(":")[0] in NEURAL_KEYS and ":refused" in d]
        unknown = [d for d in refused if "unknown-key" in d]
        if status is None and detail is None:
            out[arm] = "not in this build: skipped (no [slipstream-pipeline] and no [nr-runtime-status] line: a build without the test hook cannot be asked for the neural layer)" + NEURAL_WITHOUT
        elif unknown:
            out[arm] = "not in this build: skipped (the build does not know the neural keys: " + ", ".join(unknown) + ")" + NEURAL_WITHOUT
        elif status is not None and word("selectedAvailable") == "0":
            kind = "not installed: skipped" if word("reason") == "runtime-missing" or word("original") == "absent" else "not usable on this PC: skipped"
            out[arm] = (f"{kind} (the app reports selected={word('selected')} selectedAvailable=0 reason={word('reason')}, original={word('original')}, "
                        f"originalSigned={word('originalSigned')}, gpuSupportsOriginal={word('gpuSupportsOriginal')})" + NEURAL_WITHOUT)
        elif refused:
            out[arm] = "refused by the app: skipped (" + ", ".join(refused) + ")" + NEURAL_WITHOUT
        elif status is None:
            out[arm] = "neural layer applied per [slipstream-pipeline]; runtime state not logged (no [nr-runtime-status] line)"
        else:
            out[arm] = f"neural runtime available: selected={word('selected')} version={word('originalVersion')} signed={word('originalSigned')} reason={word('reason')}"
    return out

BUTTON_SPREAD_MS = 50.0
def button_spread_notes(data):
    """Arms whose PS-button answer time differs by more than 50 ms between runs (BENCH2: two console states, ~450 / ~550 ms)."""
    out = {}
    for arm, runs in measured_arms(data):
        medians = []
        for run in runs:
            if run["purpose"] != "latency": continue
            values = sorted(run["log"]["presses"]) or sorted(s["median"] for s in run["log"]["summaries"])
            if values: medians.append(lin_quantile(values, 0.5))
        if len(medians) >= 2 and max(medians) - min(medians) > BUTTON_SPREAD_MS:
            out[arm] = (f"the PS-button answer time of {arm} differs by {fmt(max(medians) - min(medians), 0)} ms between its runs (run medians "
                        + ", ".join(fmt(m, 0) for m in medians) + " ms): the console answers in more than one state (about 450 and 550 ms were seen), "
                        "so this test cannot compare arms at this size")
    return out

MEASURED_FOOT = ("vblank = DXGI frame statistics, software: the Present call to the vblank the statistics name for that present, and only for the presents they dated ('share of presents dated'; Package 9 steady state 1 to 4 %, the statistics 100 to 500 ms old when read). It is not screen latency (no panel processing, no pixel response) and not input-to-photon, and a sub-millisecond value is what that dated sample shows (perhaps the variable-refresh path: not established), not a proven screen delay: PresentMon (MsUntilDisplayed) or a camera decides. 'presents named' counts presents the statistics named, not dated. 'first 3 s' and 'steady' are apart because the first 1 to 3 seconds ran slower in every run of 20 Nov. audio = the audio owner's estimates beside the video clock lag ([remoteplay-clock] arrivalMinusPtsMs).")

def measured_heading(session):
    name = (": " + session.upper()) if session else ""
    return "MEASURED VALUES" + name + " (software counters per arm, medians of the app's lines unless named; never input-to-photon)"

def measured_paragraphs(data, measured=None):
    """RESULT.txt section MEASURED VALUES (Add-MeasuredSection writes the same lines)."""
    measured = measured or measured_tables(data)
    out = [measured_heading(measured["session"])]
    if network_route(data) == "wifi": out.append("  " + WIFI_LABEL)
    if not measured["arms"]: out.append("  no run"); return out
    if measured["session"] in SESSION_FIRST_LINE: out.append("  " + SESSION_FIRST_LINE[measured["session"]])
    runtime = neural_runtime_lines(data)
    features = feature_lines(data)
    verdicts = visual_verdicts(measured, runtime, features)
    compared = measured_compared(data, measured)
    against = compared_lines(data, measured, compared)
    apart = measured_not_compared(data, measured)
    own_clock = own_clock_lines(data)
    reference = reference_lines(data)
    cadence = cadence_lines(data)
    per_run = per_run_lines(data, measured)
    for arm, cell in measured["arms"].items():
        out.append(f"  {arm} ({cell['runs']} runs)")
        if measured["session"] in PICTURE_SESSIONS: out.append("    picture: " + ", ".join(cell["picture"]) + " (per run)")
        if arm in reference: out.append("    " + reference[arm])
        if arm in features: out.append("    " + features[arm])
        for group in measured["groups"]: out.append(f"    {group}: {measured_group_text(measured, arm, group)}")
        if arm in runtime: out.append("    neural layer: " + runtime[arm])
        if arm in own_clock: out.append("    " + own_clock[arm])
        if arm in cadence: out.append("    " + cadence[arm])
        for line in per_run.get(arm, []): out.append("    " + line)
        if arm in against: out.append("    " + against[arm])
        if arm in apart: out.append("    " + apart[arm])
        if arm in verdicts: out.append("    verdict: " + verdicts[arm])
    if measured["session"] in PICTURE_SESSIONS: out.append("  " + PICTURE_RULE)
    if compared: out.append("  " + SPREAD_RULE)
    if verdicts: out.append("  " + VISUAL_RULE)
    if cadence: out.append("  " + CADENCE_RULE)
    for note in button_spread_notes(data).values(): out.append("  NOTE: " + note)
    out.append("  " + MEASURED_FOOT)
    return out

def measured_json(data, measured=None):
    measured = measured or measured_tables(data)
    return {"session": measured["session"],
            "arms": {a: {"runs": c["runs"], "cells": {k: {"value": None if v["value"] is None or isinstance(v["value"], str) else round(v["value"], 2), "n": v["n"], "text": v["text"]}
                                                      for k, v in c["cells"].items()}} for a, c in measured["arms"].items()},
            "compared": measured_compared(data, measured), "notCompared": measured_not_compared(data, measured),
            "picture": {a: list(c["picture"]) for a, c in measured["arms"].items()}, "ownClock": own_clock_lines(data), "reference": reference_lines(data),
            "verdicts": visual_verdicts(measured, neural_runtime_lines(data), feature_lines(data)), "runtime": neural_runtime_lines(data), "features": feature_lines(data), "cadence": cadence_lines(data), "buttonSpread": sorted(button_spread_notes(data))}

def measured_disagreement(mine, theirs):
    """Differences between the kit's result.json 'measured' and this report's (empty list = agree)."""
    differ = []
    their_arms = as_dict(theirs.get("arms"))
    for arm in mine["arms"]:
        if arm not in their_arms: differ.append(f"measured {arm} (not in the kit's result)")
    for arm, t in their_arms.items():
        m = mine["arms"].get(arm)
        if m is None: differ.append(f"measured {arm} (not in this report)"); continue
        if int(as_dict(t).get("runs") or 0) != m["runs"]: differ.append(f"measured {arm} runs {t.get('runs')} / {m['runs']}")
        their_cells = as_dict(as_dict(t).get("cells"))
        for key in m["cells"]:
            if key not in their_cells: differ.append(f"measured {arm} {key} (not in the kit's result)")
        for key, cell in their_cells.items():
            mc = m["cells"].get(key); cell = as_dict(cell)
            if mc is None: differ.append(f"measured {arm} {key} (not in this report)"); continue
            if int(cell.get("n") or 0) != mc["n"]: differ.append(f"measured {arm} {key} n {cell.get('n')} / {mc['n']}")
            elif cell.get("value") is not None and mc["value"] is not None and abs(float(cell["value"]) - mc["value"]) > 0.011: differ.append(f"measured {arm} {key} {cell['value']} / {mc['value']}")
            elif (cell.get("value") is None) != (mc["value"] is None): differ.append(f"measured {arm} {key} {cell.get('value')} / {mc['value']}")
            elif mc["value"] is None and str(cell.get("text") or "n/a") != mc["text"]: differ.append(f"measured {arm} {key} '{cell.get('text')}' / '{mc['text']}'")
    their_compared = as_dict(theirs.get("compared"))
    for arm in set(their_compared) | set(mine["compared"]):
        t, m = as_dict(their_compared.get(arm)), mine["compared"].get(arm, {})
        for key in sorted(set(t) | set(m)):
            if t.get(key) != m.get(key): differ.append(f"compared {arm} {key} '{t.get(key)}' / '{m.get(key)}'")
    for key in ("verdicts", "runtime", "notCompared", "ownClock", "reference", "features", "cadence"):
        their_words = as_dict(theirs.get(key))
        for arm in sorted(set(their_words) | set(mine[key])):
            text, mt = their_words.get(arm), mine[key].get(arm)
            if key == "verdicts" and verdict_word5(text) != verdict_word5(mt): differ.append(f"visual verdict {arm} '{text}' / '{mt}'")
            if key != "verdicts" and str(text) != str(mt): differ.append(f"{key} {arm} '{text}' / '{mt}'")
    their_picture = as_dict(theirs.get("picture"))
    for arm in sorted(set(their_picture) | set(mine["picture"])):
        if as_list(their_picture.get(arm)) != mine["picture"].get(arm, []): differ.append(f"picture {arm} {as_list(their_picture.get(arm))} / {mine['picture'].get(arm)}")
    if sorted(as_list(theirs.get("buttonSpread"))) != mine["buttonSpread"]: differ.append(f"button spread {as_list(theirs.get('buttonSpread'))} / {mine['buttonSpread']}")
    return differ

def verdict_word5(text):
    m = re.match(r"(better|worse|cannot tell|no verdict|reference|the owner's reference)", str(text or ""))
    return m.group(1) if m else str(text)

# Time estimates (kit 5, BENCH2 bug 1): from the measured runs of 20 Nov (12 starts of 'settle:8;latency:12;exit' took
# 53-75 s, median 54 s). Start, connect and close: 15 s; a press 3 s; a latency test 2 s more; every settle / wait /
# probe / sr / fg step its own seconds; a quality step 30 s. A start done by hand (a build without automation): 60 s more.
START_SECONDS, PRESS_SECONDS, TEST_SECONDS, QUALITY_SECONDS, MANUAL_SECONDS = 15.0, 3.0, 2.0, 30.0, 60.0

def plan_seconds(plan):
    """About how long one start of an autorun plan takes, start to start (run-tests.ps1 Get-PlanSeconds)."""
    total = START_SECONDS
    for step in (plan or "").split(";"):
        name, _, value = step.partition(":")
        v = number(value) or 0.0
        if name in ("settle", "wait", "probe", "sr", "fg"): total += v
        elif name == "latency": total += v * PRESS_SECONDS + TEST_SECONDS
        elif name == "quality": total += QUALITY_SECONDS
    return total

def latency_plan(trials, tests):
    """The plan of a latency start (run-tests.ps1 Get-AutorunPlan)."""
    return ";".join(["settle:8"] + [f"latency:{trials}"] * tests + ["exit"])

def about_minutes(seconds):
    """'about N min' rounded to the nearest minute, at least 1 (run-tests.ps1 Get-AboutMinutes)."""
    return max(1, int(math.floor(seconds / 60.0 + 0.5)))

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
            tables = precision_tables(data, summary)
            def deep(value):
                if isinstance(value, dict): return {str(k): deep(v) for k, v in value.items()}
                if isinstance(value, list): return [deep(v) for v in value]
                value = clean(value)
                return round(value, 4) if isinstance(value, float) else value
            json.dump({"kind": "software latency (built-in test), not input-to-photon", "rule": summary["rule"], "minimumPresses": summary["minimum"],
                       "network": {"route": tables["route"], "label": tables["label"]},
                       "arms": [{k: clean(v) for k, v in r.items()} for r in summary["arms"]], "tables": deep(tables), "networkSession": deep(network_tables(data))}, handle, indent=1)
        print(f"runs: {len(data['runs'])}  report: {os.path.join(out, 'report.md')}, report.html and report-result.json")
        for note in notes: print("note:", note)
        return 2 if (leaks and args.strict) else 0

if __name__ == "__main__":
    sys.exit(main())
