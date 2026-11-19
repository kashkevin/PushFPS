#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Pass/fail lines for the reliability checks of the hardware test kit (run-tests.ps1 -Reliability).

  python reliability.py <evidence folder or .zip>     exit 1 when a check FAILED, 3 when the evidence has no check

The kit writes its own verdict of every check into reliability.csv while it runs; this file judges the same
evidence again by the same rules (docs/RELIABILITY.md, section 4) and report.py shows both side by side.
Python standard library only.

A check is a pair of markers in markers.csv (<check> and <check>-end, written by the kit) and the log lines of the
program between them. Results:
  PASS            exercised, and the program did what the rule asks
  FAIL            exercised, and it did not
  NOT-EXERCISED   the log does not show that the thing was done (cable not pulled, display not changed ...)
  SKIPPED         the check does not apply to this run (keyboard and mouse mode off)
Nothing here measures latency.
"""
import os, statistics, sys

STREAM_CHECKS = {"cable-out-1s": 10.0, "cable-out-5s": 60.0}     # seconds allowed until the stream is back
DISPLAY_CHECKS = ("display-mode-change", "display-reconnect")
ORDER = ("cable-out-1s", "cable-out-5s", "display-mode-change", "display-reconnect", "focus-loss-keys-held", "crash-recovery", "gpu-device-loss", "safe-start")
LIVE_FPS = 5.0        # decoded pictures per second that count as "a stream"

def between(times, start, end): return [t for t in times if t is not None and start <= t < end]

def normal_rate(progress, start):
    """Median decoded rate in the 30 s before a step, or None when there was no steady stream."""
    before = [f["decodeFps"] for t, f in progress if start - 30.0 <= t < start and f.get("decodeFps") is not None]
    if len(before) < 3: return None
    rate = statistics.median(before)
    return rate if rate >= LIVE_FPS else None

def fatal(log, start, end):
    """Why the program did not survive the window, or ''."""
    if between(log["crashes"], start, end): return "a crash line"
    if between(log["hangs"], start, end): return "an engine hang line"
    if between(log["starts"], start, end): return "the program started again (it had ended)"
    return ""

def stream_check(log, start, end, limit):
    normal = normal_rate(log["progress"], start)
    if normal is None: return "NOT-EXERCISED", "no steady stream in the 30 s before the step"
    window = [(t, f["decodeFps"]) for t, f in log["progress"] if start <= t < end and f.get("decodeFps") is not None]
    low = [t for t, fps in window if fps < 0.9 * normal]
    events = between(log["recovery_events"], start, end)
    if not low and not events: return "NOT-EXERCISED", "the log shows no disturbance of the stream: was the cable pulled?"
    if between(log["recovery_stopped_at"], start, end): return "FAIL", "the program gave up reconnecting"
    why = fatal(log, start, end)
    if why: return "FAIL", why
    last_bad = max(low + events)
    back = next((t for t, fps in window if t > last_bad and fps >= 0.9 * normal), None)
    if back is None: return "FAIL", f"the stream was not back to {0.9 * normal:.0f} pictures per second when the step ended"
    seconds = round(back - start)          # whole seconds, as the kit's own line counts them
    reconnects = len(between(log["reconnects"], start, end))
    how = f"back after {seconds:.0f} s (limit {limit:.0f} s), {len(between(log['keyframe_requests'], start, end))} keyframe request(s), {reconnects} reconnect(s)"
    return ("PASS" if seconds <= limit else "FAIL"), how

def display_check(log, start, end):
    changed = [t for t, component, text in log["display_events"] if t is not None and start <= t < end and (component == "display-refresh" or text.startswith("invalidate"))]
    if not changed: return "NOT-EXERCISED", "the log shows no display change in the step"
    why = fatal(log, start, end)
    if why: return "FAIL", why
    if between(log["gpu_failures"], start, end): return "FAIL", "a GPU failure line"
    normal = normal_rate(log["progress"], start)
    window = [(t, f["decodeFps"]) for t, f in log["progress"] if start <= t < end and f.get("decodeFps") is not None]
    presented = [(t, f.get("presentSubmitFps")) for t, f in log["frame_rate"] if start <= t < end and f.get("presentSubmitFps") is not None]
    if normal is not None and (not window or window[-1][1] < 0.9 * normal): return "FAIL", "the stream was not back when the step ended"
    if presented and presented[-1][1] < 1.0: return "FAIL", "nothing was being presented when the step ended"
    return "PASS", f"{len(changed)} display change line(s); the program kept running" + (", the stream was at its rate again" if normal is not None else "; no stream was running, so only survival was checked")

def focus_check(log, start, end, answer):
    captured = [t for t, text in log["kbm"] if t is not None and t < end and text.startswith("keyboard and mouse captured")]
    if not captured: return "SKIPPED", "keyboard and mouse mode was not on in this run (kit parameter -ReliabilityKbm)"
    released = [t for t, text in log["kbm"] if t is not None and start <= t < end and text.startswith("keyboard and mouse released (focus lost)")]
    if released and answer == "no": return "FAIL", "the log shows every key released on focus loss, but you reported input still held on the console"
    if released: return "PASS", "every key was released when the window lost focus" + ("" if answer == "yes" else " (log only: no answer about the game recorded)")
    if answer == "no": return "FAIL", "no release on focus loss in the log, and you reported input still held"
    return "NOT-EXERCISED", "the log shows no loss of focus while keyboard and mouse were captured"

def crash_recovery_check(log, start, end):
    restarts = between(log["starts"], start, end)
    if not restarts: return "FAIL", "PushFPS did not start again after it was ended"
    again = restarts[0]
    if between(log["crashes"], again, end): return "FAIL", "a crash line after the new start"
    sessions = between(log["sessions"], again, end)
    if not sessions: return "NOT-EXERCISED", "PushFPS started again; no connection to the console was made in the step"
    live = next((t for t, f in log["progress"] if sessions[0] <= t < end and (f.get("decodeFps") or 0) >= LIVE_FPS), None)
    if live is None: return "FAIL", "PushFPS started again and connected, but no pictures were decoded in the step"
    return "PASS", f"started again {again - start:.0f} s after it was ended, pictures decoded {live - start:.0f} s after"

def gpu_loss_check(log, start, end):
    failures = between(log["gpu_failures"], start, end) + [t for t, text in log["engine_test"] if t is not None and start <= t < end and "GPU" in text]
    if not failures: return "NOT-EXERCISED", "the log shows no GPU device loss in the step"
    lost = min(failures)
    if between(log["crashes"], lost, end): return "FAIL", "a crash line after the device was lost"
    if between(log["starts"], lost, end): return "FAIL", "the program ended and started again after the device was lost"
    automatic = any("automatic=true" in text for t, text in log["gpu_reconnects"] if t is not None and lost <= t < end)
    shown = [(t, f.get("presentSubmitFps")) for t, f in log["frame_rate"] if lost + 2.0 <= t < end and f.get("presentSubmitFps") is not None]
    back = next((t for t, fps in shown if fps >= 1.0), None)
    if back is None and not log["frame_rate"]:
        back = next((t for t, f in log["progress"] if lost + 2.0 <= t < end and (f.get("decodeFps") or 0) >= LIVE_FPS), None)
    if back is None: return "FAIL", "the program survived the device loss, but no picture came back in the step (a PS5 session is not reconnected automatically)"
    return "PASS", f"picture back {back - lost:.0f} s after the device was lost (" + ("reconnected by the program" if automatic else "after a reconnect by hand: the program does not reconnect a PS5 session by itself") + ")"

def safe_start_check(log, meta):
    if not log["starts"]: return "FAIL", "no start line of the program in the log"
    if log["crashes"] or log["hangs"]: return "FAIL", "a crash or engine hang line"
    experiments = [e for e in log["echo"] if "experiment" in e.lower()]
    if experiments: return "FAIL", "an experiment was active in a start that should have none: " + experiments[0][:80]
    if meta.get("forcedStop"): return "FAIL", "the program did not close when asked"
    if not meta.get("limitReached"): return "FAIL", "the program ended before the kit asked it to close"
    return "PASS", f"started without slipstream.ini and without switches, stayed up {int(meta.get('limitSeconds') or 0)} s, closed when asked"

def evaluate(run):
    """[{"check", "result", "detail"}] for one reliability, gpuloss or safestart run."""
    log, meta = run["log"], run["meta"]
    marks = {}
    for when, label in run["markers"]: marks.setdefault(label, when)
    out = []
    def add(check, verdict): out.append({"check": check, "result": verdict[0], "detail": verdict[1]})
    if run["purpose"] == "safestart": add("safe-start", safe_start_check(log, meta)); return out
    for check in ORDER:
        start, end = marks.get(check), marks.get(check + "-end")
        if start is None: continue
        if end is None: add(check, ("NOT-EXERCISED", "the step was not finished (no end marker)")); continue
        answer = "yes" if (check + "-answer-yes") in marks else "no" if (check + "-answer-no") in marks else ""
        if check in STREAM_CHECKS: add(check, stream_check(log, start, end, STREAM_CHECKS[check]))
        elif check in DISPLAY_CHECKS: add(check, display_check(log, start, end))
        elif check == "focus-loss-keys-held": add(check, focus_check(log, start, end, answer))
        elif check == "crash-recovery": add(check, crash_recovery_check(log, start, end))
        elif check == "gpu-device-loss": add(check, gpu_loss_check(log, start, end))
    return out

def read_kit_verdicts(folder):
    """{check: (result, detail)} from the reliability.csv the kit wrote into a run folder."""
    import csv
    out = {}
    try:
        with open(os.path.join(folder, "reliability.csv"), newline="", encoding="utf-8-sig", errors="replace") as handle:
            for row in csv.DictReader(handle):
                if row.get("check"): out[row["check"].strip()] = ((row.get("result") or "").strip(), (row.get("detail") or "").strip())
    except OSError: pass
    return out

HEADER = ["Arm", "Check", "Result", "Why", "The kit's own line", "Agree"]

def rows_of(run):
    kit = read_kit_verdicts(run["folder"])
    rows = []
    for item in evaluate(run):
        theirs = kit.get(item["check"])
        rows.append([run["arm"], item["check"], item["result"], item["detail"], f"{theirs[0]}: {theirs[1]}" if theirs else "none", "n/a" if not theirs else ("yes" if theirs[0] == item["result"] else "NO")])
    return rows

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
        rows = [row for run in report.load(folder)["runs"] if run["purpose"] in report.RELIABILITY_PURPOSES for row in rows_of(run)]
        if not rows: print("no reliability check in this evidence"); return 3
        for row in rows: print(f"{row[2]:14s} {row[1]:22s} {row[3]}" + ("" if row[5] != "NO" else f"   (the kit said: {row[4]})"))
        return 1 if any(row[2] == "FAIL" for row in rows) else 0

if __name__ == "__main__":
    sys.exit(main())
