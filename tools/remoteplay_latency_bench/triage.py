#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Is it smooth, and if not, why: a triage of PushFPS log lines, per program start and per playback session.

    python3 -B tools/remoteplay_latency_bench/triage.py <diagnostics zip | unpacked folder | log file> [--out triage.md]

Reads the log-<copy>.txt files of a Collect-Diagnostics zip (or any PushFPS log or kit bench log), splits them into
program starts and playback sessions, and writes for each session: what was on (the slipstream.ini line as the program
accepted it, the switches the program itself logged, the neural runtime status, the [slipstream-config] settings, the
working sizes), which build it was as far as the log can tell, and "is it smooth, and if not, why" from every value the
log holds (frames shown per second, generated frames dropped late, pairs skipped, mailbox replacements, GPU time per pair
against the pair interval, present intervals, resets). A value the log does not hold is written "not logged by this
build or run" (most per-window values need VEYRA_EXP_RP_STATS=1 in slipstream.ini). Sessions of an earlier build in
the same log (no [slipstream-sizes] line: build #66 or older) are listed separately: a comparison from normal play.

Standard library only; used by report.py for the kit's A/B sets as well. Software values from the program's own log:
never input-to-photon, never a camera measurement. Nothing here changes anything.
"""
import argparse
import io
import os
import re
import statistics
import sys
import zipfile

LINE = re.compile(r"^\ufeff?(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z) t=\S+ \[(\w+) ?\] \[([a-z0-9-]+)\] (.*)$")
NUMBER = re.compile(r"(?<![A-Za-z])(\w+)=(-?[0-9]+(?:\.[0-9]+)?)(?![0-9A-Za-z.])")
WORD = re.compile(r"(?<![A-Za-z])(\w+)=(\S+)")
# Markers of a program start: each is written once per start (the settings file, the neural runtime check, the
# update check). Markers less than START_GAP_S apart belong to the same start.
START_MARKERS = (("remoteplay-settings", "slipstream.ini"), ("nr-runtime-status", ""), ("qml", "[update-check]"))
START_GAP_S = 60.0
NOT_LOGGED = "not logged by this build or run"

FLOW_COUNTERS = ("captureReceived", "mailboxOverwritten", "sourceAccepted", "sourceSkippedBeforeGraph", "realSubmitted", "fgCandidate",
                 "fgSkippedBeforeEval", "fgEvaluated", "fgSkippedForReset", "fgReduced", "fgReadyValid", "fgInvalid", "realPresented",
                 "generatedPresented", "generatedExpiredAfterEval", "cancelledBeforePresent", "slotWaitCount")


def utc_seconds(stamp):
    """Seconds of a log time stamp (only differences are used)."""
    m = re.match(r"(\d{4})-(\d\d)-(\d\d)T(\d\d):(\d\d):(\d\d(?:\.\d+)?)Z", stamp)
    if not m: return None
    y, mo, d, h, mi = (int(m.group(i)) for i in range(1, 6))
    days = (y - 2000) * 372 + mo * 31 + d   # monotonic within a log; never shown
    return days * 86400.0 + h * 3600 + mi * 60 + float(m.group(6))


def parse(lines):
    """[(seconds, level, component, text)] of the stamped lines."""
    out = []
    for raw in lines:
        m = LINE.match(raw.rstrip("\r\n"))
        if not m: continue
        t = utc_seconds(m.group(1))
        if t is not None: out.append((t, m.group(2), m.group(3), m.group(4)))
    return out


def numbers(text):
    return {k: float(v) for k, v in NUMBER.findall(text)}


def words(text):
    return dict(WORD.findall(text))


def is_start_marker(component, text):
    return any(component == c and text.startswith(p) if p else component == c for c, p in START_MARKERS)


def split_starts(entries):
    """Program starts: a new start where a start marker follows the previous marker by more than START_GAP_S."""
    starts, current, last_marker = [], [], None
    for entry in entries:
        t, _level, component, text = entry
        if is_start_marker(component, text):
            if last_marker is not None and t - last_marker > START_GAP_S and current:
                starts.append(current); current = []
            last_marker = t
        current.append(entry)
    if current: starts.append(current)
    return starts


def split_sessions(start):
    """Playback sessions of one program start by the [frame-flow] session= number. Lines between the first and the last
    [frame-flow] line of a session (plus 6 s, one 5 s window) belong to it; lines before the first session go to the
    start's own facts."""
    spans = {}
    for t, _l, c, text in start:
        if c != "frame-flow": continue
        w = words(text)
        if w.get("state") != "active" or "session" not in w: continue
        a, b = spans.get(w["session"], (t, t))
        spans[w["session"]] = (min(a, t), max(b, t))
    sessions = []
    for name, (a, b) in sorted(spans.items(), key=lambda item: item[1][0]):
        sessions.append({"session": name, "from": a, "to": b, "lines": [e for e in start if a - 1.0 <= e[0] <= b + 6.0]})
    return sessions


def median_of(values):
    values = [v for v in values if v is not None and v == v and v != -1.0]
    return statistics.median(values) if values else None


def most_common(values):
    values = [v for v in values if v not in (None, "")]
    if not values: return None
    return max(set(values), key=lambda v: (values.count(v), -values.index(v)))


def field_median(lines, component, field):
    return median_of([numbers(text).get(field) for _t, _l, c, text in lines if c == component])


def field_max(lines, component, field):
    values = [numbers(text).get(field) for _t, _l, c, text in lines if c == component]
    values = [v for v in values if v is not None and v != -1.0]
    return max(values) if values else None


def field_word(lines, component, field):
    return most_common([words(text).get(field) for _t, _l, c, text in lines if c == component])


def last_text(lines, component, prefix=""):
    found = [text for _t, _l, c, text in lines if c == component and text.startswith(prefix)]
    return found[-1] if found else None


def flow_rates(lines):
    """Per second of the [frame-flow] counters (cumulative within a session; a fall is a restart)."""
    points = [(t, numbers(text)) for t, _l, c, text in lines if c == "frame-flow" and words(text).get("state") == "active"]
    if len(points) < 2: return {}, 0.0
    seconds = points[-1][0] - points[0][0]
    if seconds <= 0: return {}, 0.0
    rates = {}
    for key in FLOW_COUNTERS:
        rise = 0.0
        for (_a, x), (_b, y) in zip(points, points[1:]):
            if key not in x or key not in y: continue
            rise += (y[key] - x[key]) if y[key] >= x[key] else y[key]
        rates[key] = rise / seconds
    return rates, seconds


def facts(start_lines, session_lines):
    """Everything the triage shows for one session (start_lines: the whole program start, for its one-off lines)."""
    f = {}
    settings = [text for _t, _l, c, text in start_lines if c == "remoteplay-settings"]
    f["settings_file"] = settings or None
    f["nr_runtime"] = last_text(start_lines, "nr-runtime-status")
    f["config"] = words(last_text(session_lines, "slipstream-config") or last_text(start_lines, "slipstream-config") or "")
    f["sizes"] = last_text(session_lines, "slipstream-sizes") or last_text(start_lines, "slipstream-sizes")
    f["build"] = ("build #70 or later (it logs [slipstream-sizes])" if any(c == "slipstream-sizes" for _t, _l, c, _x in start_lines)
                  else "an earlier build (no [slipstream-sizes] line: build #66 or older)" if any(c == "slipstream-config" for _t, _l, c, _x in start_lines)
                  else "unknown (no [slipstream-config] or [slipstream-sizes] line)")
    evidence = []
    for _t, _l, c, text in start_lines:
        if c == "experiment-switches": evidence.append("[experiment-switches] " + text.split(" (ini: ")[0][:400])
        elif c == "fg-ull" and "experiment" in text: evidence.append("[fg-ull] " + text[:200])
        elif c == "graph-priority": evidence.append("[graph-priority] " + text[:200])
        elif c == "remoteplay-timing" and "adaptive pair phase" in text: evidence.append("[remoteplay-timing] " + text[:140])
        elif c == "remoteplay-cadence" and text.startswith("experiment"): evidence.append("[remoteplay-cadence] " + text[:140])
        elif c == "slipstream-smooth" and text.startswith("switches"): evidence.append("[slipstream-smooth] " + text[:140])
        elif c == "slipstream-sizes" and text.startswith("event=switch"): evidence.append("[slipstream-sizes] " + text[:140])
    f["switch_lines"] = list(dict.fromkeys(evidence))
    rates, seconds = flow_rates(session_lines)
    f["flow"], f["seconds"] = rates, seconds
    L = session_lines
    f["smooth"] = {k: field_median(L, "slipstream-smooth", k) for k in (
        "contentFps", "generatedFps", "rejectedPairsFps", "dropDeadlineFps", "dropCapacityFps", "dropCapFps", "gpuSpanP95Ms", "gpuFgP95Ms",
        "gpuFg1P95Ms", "gpuFg1P50Ms", "pairBudgetMs", "deadPhaseFps", "fallbackFps", "realAgeP50Ms", "realAgeP95Ms", "firstReadyP50Ms", "phaseMs",
        "presentLateP99Ms", "realLateP50Ms", "marginP50Ms", "partsSumP50Ms", "delayToCallP50Ms", "gpuNrP95Ms", "gpuSrP95Ms", "realHoldP95Ms")}
    f["smooth"]["holds"] = field_max(L, "slipstream-smooth", "holds")
    f["smooth"]["chosen"] = field_word(L, "slipstream-smooth", "chosen")
    f["smooth"]["work"] = field_word(L, "slipstream-smooth", "work")
    f["cadence"] = {k: field_median(L, "slipstream-cadence", k) for k in ("submittedFps", "realFps", "generatedFps", "intervalP50Ms", "intervalP95Ms", "intervalP99Ms", "offPct", "refreshHz")}
    f["cadence"]["intervalMaxMs"] = field_max(L, "slipstream-cadence", "intervalMaxMs")
    f["cadence"]["vrr"] = field_word(L, "slipstream-cadence", "vrr")
    f["capture"] = {k: field_median(L, "slipstream-capture", k) for k in ("realLatP50Ms", "realLatP95Ms", "newFps", "repeatFps", "missedNewFps", "mailboxDropFps", "genFps", "stillGenFps")}
    f["timing"] = {k: field_median(L, "capture-timing", k) for k in ("callbackToPresentReturnP95Ms", "readAgeMs", "gpuFgBatchP95Ms")}
    f["timing"]["expiredGenerated"] = field_max(L, "capture-timing", "expiredGenerated")
    f["steady"] = steadiness(L)
    f["health"] = fg_health(L)
    f["path"] = present_path(L)
    f["reflex_trip"] = [text for _t, _l, c, text in L if c == "reflex" and text.startswith("sleep overrun")]
    f["overlay_errors"] = [text for _t, level, c, text in L if c == "present-overlay" and level in ("WARN", "ERROR")]
    f["stream"] = {"arrivalToPresentMs": field_median(L, "remoteplay-cadence", "arrivalToPresentMs"),
                   "sourceFps": field_median(L, "remoteplay-cadence", "sourceFps"),
                   "decodeFps": field_median(L, "remoteplay-progress", "decodeFps"),
                   "gapMaxMs": field_max(L, "slipstream-net", "gapMaxMs")}
    f["dedup_content_fps"] = field_median(L, "content-dedup", "contentFps")
    f["present_rate"] = field_median(L, "frame-rate", "presentSubmitFps")
    f["scanout_p50"] = field_median(L, "display-latency", "presentToScanoutP50Ms")
    f["pacing"] = last_text(start_lines, "pacing", "DXGI")
    f["resets"] = sum(1 for _t, _l, c, _x in L if c == "reset-lifecycle")
    f["failures"] = sum(1 for _t, level, c, _x in L if c in ("gpu-failure", "engine-hang", "crash", "gpu-reconnect") or level == "ERROR")
    return f


def steadiness(lines):
    """Package 14's always-on [capture-timing] grid fields over a session: None when the build does not log them."""
    seen, intervals, on, half, shown, seconds = False, 0.0, 0.0, 0.0, 0.0, 0
    longest, run_max, robust, grid_early, skipped = 0.0, 0, 0, 0, 0
    for _t, _l, c, text in lines:
        if c != "capture-timing": continue
        w = words(text)
        if "gridIntervals" not in w: continue
        seen = True
        seconds += 1
        try:
            n = float(w["gridIntervals"])
            robust += int(float(w.get("robustAdmits", "0"))); grid_early += int(float(w.get("gridEarly", "0"))); skipped += int(float(w.get("skippedPairs", "0")))
            if n <= 0: continue
            intervals += n
            on += n * float(w["onGrid1msPct"]); half += n * float(w["onGridHalfPct"]); shown += n * float(w["shownPct"])
            longest = max(longest, float(w.get("gridLongestMs", "0"))); run_max = max(run_max, int(float(w.get("gridOffRunMax", "0"))))
        except (KeyError, ValueError):
            continue
    if not seen: return None
    out = {"seconds": seconds, "intervals": intervals, "robustAdmits": robust, "gridEarly": grid_early, "skippedPairs": skipped,
           "longestMs": longest, "offRunMax": run_max}
    if intervals > 0:
        out.update(onGrid=on / intervals, onGridHalf=half / intervals, shown=shown / intervals)
    return out


def fg_health(lines):
    """Stream "present" [fg-health] (one line per 10 s, always on with a capture card): None when the build does not log it."""
    windows = []
    for _t, _l, c, text in lines:
        if c != "fg-health": continue
        n = numbers(text)
        if n: windows.append(n)
    if not windows: return None
    def med(key):
        values = [w[key] for w in windows if key in w and w[key] >= 0]
        return statistics.median(values) if values else None
    def top(key):
        values = [w[key] for w in windows if key in w and w[key] >= 0]
        return max(values) if values else None
    def total(key):
        return sum(w.get(key, 0) for w in windows)
    out = {"windows": len(windows)}
    for key in ("realMeanMs", "realP50Ms", "realP95Ms", "realP99Ms", "intervalP50Ms", "intervalP99Ms", "onGrid1msPct", "shownPct",
                "gpu_fg1P50Ms", "gpu_fg1P95Ms", "gpu_fg1P99Ms", "gpu_colorP95Ms", "gpu_blitP95Ms", "reflexSleepMeanMs", "reflexOverrunP95Ms", "reflexOverrunP99Ms"):
        out[key] = med(key)
    for key in ("realMaxMs", "intervalMaxMs", "reflexSleepMaxMs", "reflexOverrunMaxMs"):
        out[key] = top(key)
    for key in ("realFrames", "dropDeadline", "dropCapacity", "dropCap", "suppressed", "mailboxDropped", "duplicates", "reflexSleeps", "reflexOverruns"):
        out[key] = total(key)
    out["reflex"] = int(windows[-1].get("reflex", 0))
    return out


def present_path(lines):
    """Stream "present" [present-path] (on change): the last state, how often it changed, and the items that made the
    video window's region complex at any time. None when the build does not log it."""
    states = [words(text) for _t, _l, c, text in lines if c == "present-path" and text.startswith("region=")]
    if not states: return None
    complex_items = sorted({w.get("items", "") for w in states if w.get("region") == "complex" and w.get("items", "none") != "none"})
    return {"changes": len(states), "last": states[-1], "complex": any(w.get("region") == "complex" for w in states), "complexItems": complex_items}


def health_lines(h, p):
    """The [fg-health] and [present-path] facts in words."""
    out = []
    if h is None:
        out.append("[fg-health]: " + NOT_LOGGED + " (a build before stream \"present\")")
    else:
        out.append("real frame's delay over %d windows of 10 s (median of the windows): mean %s, P50 %s, P95 %s, P99 %s; worst %s" % (
            h["windows"], number_text(h["realMeanMs"], 2, " ms"), number_text(h["realP50Ms"], 2, " ms"), number_text(h["realP95Ms"], 2, " ms"),
            number_text(h["realP99Ms"], 2, " ms"), number_text(h["realMaxMs"], 2, " ms")))
        out.append("present spacing: P50 %s, P99 %s, longest %s; on the grid %s, shown %s" % (
            number_text(h["intervalP50Ms"], 2, " ms"), number_text(h["intervalP99Ms"], 2, " ms"), number_text(h["intervalMaxMs"], 2, " ms"),
            number_text(h["onGrid1msPct"], 1, " %"), number_text(h["shownPct"], 1, " %")))
        out.append("dropped: deadline %d, capacity %d, output cap %d, suppressed %d, capture mailbox %d; duplicates removed %d" % (
            h["dropDeadline"], h["dropCapacity"], h["dropCap"], h["suppressed"], h["mailboxDropped"], h["duplicates"]))
        out.append("GPU: first generated frame P50 %s / P95 %s / P99 %s (P95 - P50 is the clock-ramp sign); colour P95 %s; present blit P95 %s" % (
            number_text(h["gpu_fg1P50Ms"], 2, " ms"), number_text(h["gpu_fg1P95Ms"], 2, " ms"), number_text(h["gpu_fg1P99Ms"], 2, " ms"),
            number_text(h["gpu_colorP95Ms"], 2, " ms"), number_text(h["gpu_blitP95Ms"], 2, " ms")))
        if h["reflexSleeps"]:
            out.append("NVIDIA low latency's Sleep: %d sleeps, mean %s, longest %s; ran past the sample's arrival %d times (P95 %s, P99 %s, worst %s, added 1:1 to that real frame)%s" % (
                h["reflexSleeps"], number_text(h["reflexSleepMeanMs"], 2, " ms"), number_text(h["reflexSleepMaxMs"], 2, " ms"), h["reflexOverruns"],
                number_text(h["reflexOverrunP95Ms"], 2, " ms"), number_text(h["reflexOverrunP99Ms"], 2, " ms"), number_text(h["reflexOverrunMaxMs"], 2, " ms"),
                "; ended for the session by a guard" if h["reflex"] == 2 else ""))
        else:
            out.append("NVIDIA low latency's Sleep: not called in this session (low latency off, not an NVIDIA card, or ended)")
    if p is None:
        out.append("[present-path]: " + NOT_LOGGED + " (a build before stream \"present\")")
    else:
        w = p["last"]
        out.append("display path (last of %d [present-path] lines): region %s, items over the picture %s, in-picture readouts %s, sync interval %s, VRR requested %s / active %s, "
                   "driver fullscreen VRR %s, driver fullscreen/iFlip %s, hardware composition flags %s, window %s, buffer %s, covers its monitor %s" % (
                       p["changes"], w.get("region", "?"), w.get("items", "?"), w.get("inPictureOverlay", "?"), w.get("syncInterval", "?"), w.get("vrrRequested", "?"),
                       w.get("vrrActive", "?"), w.get("driverFsVrr", "?"), w.get("driverFullscreenIFlip", "?"), w.get("hwCompositionFlags", "?"), w.get("window", "?"),
                       w.get("buffer", "?"), w.get("coversMonitor", "?")))
        if p["complex"]:
            out.append("the video window's region was COMPLEX at some point (cut out for: %s): the slow, composed present path is likely then" % (
                ", ".join(p["complexItems"]) or "the cinema band"))
    return out


def steadiness_lines(st):
    """The 99 % verdict in words (the owner, 23 Nov 2026: "99 % steady")."""
    if st is None:
        return ["steadiness (on the 11.1 ms grid): " + NOT_LOGGED + " (a build before Package 14's [capture-timing] grid fields)"]
    out = ["Package 14 parts that acted: robustAdmit %d pairs, gridEarly %d pairs; pairs skipped %d (%.1f/s over %d s)" % (
        st["robustAdmits"], st["gridEarly"], st["skippedPairs"], st["skippedPairs"] / max(1, st["seconds"]), st["seconds"])]
    if "onGrid" not in st:
        return out + ["steadiness: no frame-generation output in this session"]
    on, half, shown = st["onGrid"], st["onGridHalf"], st["shown"]
    detail = "%.1f %% within half a slot, longest gap %.1f ms, longest run of off-grid intervals %d" % (half, st["longestMs"], st["offRunMax"])
    if on >= 99.0 and shown >= 99.0:
        return out + ["steady: %.1f %% on grid, %.1f %% shown (within 1 ms of content interval / M; %s)" % (on, shown, detail)]
    why = []
    rate = st["skippedPairs"] / max(1, st["seconds"])
    if rate > 0.5: why.append("generation skipped %.1f pairs a second (the GPU did not fit a pair's frames in time)" % rate)
    if shown < 99.0: why.append("only %.1f %% of the expected frames were shown" % shown)
    if on < 99.0 and shown >= 99.0: why.append("frames were shown but %.1f %% of intervals were more than 1 ms off the grid (late frames: GPU spikes or present lateness)" % (100.0 - on))
    elif on < 99.0: why.append("%.1f %% of intervals were more than 1 ms off the grid" % (100.0 - on))
    return out + ["not steady (%.1f %% on grid, %.1f %% shown), because %s (%s)" % (on, shown, "; ".join(why), detail)]


def number_text(value, digits=1, unit=""):
    return NOT_LOGGED if value is None else ("%." + str(digits) + "f%s") % (value, unit)


def verdicts(f):
    """Plain lines: what the log shows about smoothness, each with its numbers. Unknown values say so."""
    out = []
    config, smooth, cadence, flow = f["config"], f["smooth"], f["cadence"], f["flow"]
    asked = config.get("fgMultiplier")
    fg_on = config.get("fg") == "1" or (flow.get("generatedPresented") or 0) > 0.5
    chosen = smooth.get("chosen")
    if asked and chosen and fg_on and chosen.rstrip("X") != asked: out.append("multiplier: asked %sX, in force %s" % (asked, chosen))
    content = smooth.get("contentFps") or f.get("dedup_content_fps") or (f["stream"].get("sourceFps"))
    real, generated = flow.get("realPresented"), flow.get("generatedPresented")
    if real is None: out.append("frames shown: " + NOT_LOGGED + " (no [frame-flow] counters)")
    else:
        mult = None
        try: mult = int((chosen or "").rstrip("X") or asked or 0)
        except ValueError: mult = None
        expected_gen = real * (mult - 1) if (fg_on and mult and mult > 1) else 0.0
        out.append("frames shown: real %.1f/s, generated %.1f/s (%s)" % (real, generated or 0.0,
                   "expected %.1f generated for %dX" % (expected_gen, mult) if expected_gen else "frame generation off"))
        if expected_gen and (generated or 0.0) < 0.95 * expected_gen:
            out.append("generated frames MISSING: %.1f/s of %.1f/s expected were shown" % (generated or 0.0, expected_gen))
    if content: out.append("content rate the duplicate removal settled on: %.1f FPS" % content)
    late = flow.get("generatedExpiredAfterEval")
    if late is not None and late > 0.5: out.append("generated frames arrive too late: %.1f per second dropped at their deadline" % late)
    skipped = flow.get("fgSkippedBeforeEval")
    if skipped is not None and skipped > 0.5: out.append("pairs skipped before generation (admission or a cost hold): %.1f/s" % skipped)
    for key, text in (("dropDeadlineFps", "generated frames dropped at the deadline"), ("dropCapacityFps", "generated frames dropped waiting for display capacity"),
                      ("rejectedPairsFps", "pairs rejected by admission"), ("deadPhaseFps", "real frames held for a phase with no generated frame"),
                      ("fallbackFps", "real frames shown when ready because generation was held off")):
        value = smooth.get(key)
        if value is not None and value > 0.5: out.append("%s: %.1f/s" % (text, value))
    span, budget = smooth.get("gpuSpanP95Ms"), smooth.get("pairBudgetMs")
    if span is not None and budget:
        if span > 0.9 * budget: out.append("GPU does not finish a pair in time: span P95 %.1f ms of a %.1f ms pair" % (span, budget))
        else: out.append("GPU finishes a pair in time: span P95 %.1f ms of a %.1f ms pair" % (span, budget))
    else: out.append("GPU time per pair: " + NOT_LOGGED)
    replaced = flow.get("mailboxOverwritten")
    if replaced is not None and replaced > 1.0: out.append("capture or decoder frames replaced before the engine read them: %.1f/s (the engine was busy)" % replaced)
    missed = f["capture"].get("missedNewFps")
    if missed is not None and missed > 0.5: out.append("capture delivers uneven frames: %.1f new pictures/s missed" % missed)
    if cadence.get("offPct") is not None:
        target = (cadence.get("submittedFps") or 0.0)
        p99, worst = cadence.get("intervalP99Ms"), cadence.get("intervalMaxMs")
        even = cadence["offPct"] <= 5.0 and (p99 is None or target <= 0 or p99 <= 1.5 * 1000.0 / target)
        out.append("%s: %.1f FPS submitted, intervals P50 %s / P99 %s / longest %s, uneven %.1f %%" % (
            "presented evenly" if even else "presents UNEVEN", target, number_text(cadence.get("intervalP50Ms"), 2, " ms"),
            number_text(p99, 2, " ms"), number_text(worst, 2, " ms"), cadence["offPct"]))
    else: out.append("present intervals: " + NOT_LOGGED + " (needs VEYRA_EXP_RP_STATS=1)")
    late_p99 = smooth.get("presentLateP99Ms")
    if late_p99 is not None and late_p99 > 2.0: out.append("presents late: P99 %.1f ms after their deadline" % late_p99)
    if f["resets"] > 1: out.append("graph resets or rebuilds after the start: %d" % (f["resets"] - 1))
    if f["failures"]: out.append("errors, GPU failures, hangs or crashes: %d line(s)" % f["failures"])
    gap = f["stream"].get("gapMaxMs")
    if gap is not None and gap > 50: out.append("stream stall: longest gap between frames %.0f ms" % gap)
    return out


def delay_lines(f):
    """The delay values both builds log (the on-screen readout is the 1 s mean of a real frame's arrival to Present return)."""
    s, c, t, st = f["smooth"], f["capture"], f["timing"], f["stream"]
    return [
        "on-screen readout's measure, exact mean (Remote Play with the stream duplicate removal: [remoteplay-cadence] arrivalToPresentMs): " + number_text(st.get("arrivalToPresentMs"), 2, " ms"),
        "capture card, P50 of the same measure ([slipstream-capture] realLatP50Ms): " + number_text(c.get("realLatP50Ms"), 2, " ms"),
        "stream, P50 of the same measure to Present return ([slipstream-smooth] realAgeP50Ms): " + number_text(s.get("realAgeP50Ms"), 2, " ms"),
        "P95 of the same measure, always on ([capture-timing] callbackToPresentReturnP95Ms): " + number_text(t.get("callbackToPresentReturnP95Ms"), 2, " ms"),
        "pair phase ([slipstream-smooth] phaseMs): " + number_text(s.get("phaseMs"), 2, " ms") + "; first generated frame ready: " + number_text(s.get("firstReadyP50Ms"), 2, " ms"),
    ]


def describe_session(f, title):
    out = ["### " + title, "", "Program: " + f["build"]]
    out.append("slipstream.ini as the program accepted it: " + ("; ".join(f["settings_file"]) if f["settings_file"] else "no [remoteplay-settings] line (no slipstream.ini, or the line was not collected)"))
    out.append("Switches the program itself logged: " + ("; ".join(f["switch_lines"]) if f["switch_lines"] else "none"))
    out.extend(steadiness_lines(f.get("steady")))
    out.extend(health_lines(f.get("health"), f.get("path")))
    for text in f.get("reflex_trip", []):
        out.append("NVIDIA low latency ENDED by the overrun guard: " + text[:300])
    if f.get("overlay_errors"):
        out.append("in-picture readouts FAILED %d time(s) (they stay cut out of the video window): %s" % (len(f["overlay_errors"]), f["overlay_errors"][0][:200]))
    out.append("Neural runtime: " + (f["nr_runtime"] or NOT_LOGGED))
    keys = ("fg", "fgMethod", "fgMultiplier", "fgTier", "fgBackend", "sr", "srTarget", "enhance", "nrRuntime", "nrSize", "hdr", "fullscreen", "window", "refreshHz", "vrrLevel", "stream", "smoothPacing")
    out.append("Settings ([slipstream-config]): " + (" ".join("%s=%s" % (k, f["config"][k]) for k in keys if k in f["config"]) or NOT_LOGGED))
    out.append("Working sizes: " + (f["sizes"] or ("work=" + f["smooth"]["work"] if f["smooth"].get("work") else NOT_LOGGED)))
    out.append("Length: %.0f s of [frame-flow] counters" % f["seconds"])
    out += ["", "Is it smooth, and if not, why:"] + ["  - " + line for line in verdicts(f)]
    out += ["", "Delay values (software, not input-to-photon):"] + ["  - " + line for line in delay_lines(f)]
    return out + [""]


def read_inputs(path):
    """[(name, lines)] of the log files in a zip, a folder or one file."""
    found = []
    if os.path.isfile(path) and path.lower().endswith(".zip"):
        with zipfile.ZipFile(path) as archive:
            for name in sorted(archive.namelist()):
                base = os.path.basename(name.replace("\\", "/"))
                if re.match(r"(log-.*|.*@.*)\.txt$", base):
                    found.append((base, io.TextIOWrapper(archive.open(name), encoding="utf-8", errors="replace").read().splitlines()))
    elif os.path.isdir(path):
        for folder, _dirs, names in os.walk(path):
            for name in sorted(names):
                base = os.path.basename(name.replace("\\", "/"))
                if re.match(r"(log-.*|.*@.*)\.txt$", base):
                    with open(os.path.join(folder, name), encoding="utf-8", errors="replace") as handle: found.append((base, handle.read().splitlines()))
    else:
        with open(path, encoding="utf-8", errors="replace") as handle: found.append((os.path.basename(path), handle.read().splitlines()))
    return found


def triage(inputs):
    out = ["# Is it smooth, and if not, why (triage.py)", "",
           "From the program's own log lines only: software values, never input-to-photon. 'not logged by this build or run' means the",
           "log holds no line for it (most per-window values need VEYRA_EXP_RP_STATS=1 in slipstream.ini). Build #70 is Package 13's",
           "program; a start without [slipstream-sizes] is an earlier build (Package 12A's is #66).", ""]
    for name, lines in inputs:
        starts = split_starts(parse(lines))
        out += ["## " + name, ""]
        if not starts: out += ["No stamped log lines.", ""]; continue
        groups = {}
        for number, start in enumerate(starts, 1):
            sessions = split_sessions(start)
            for session in sessions:
                f = facts(start, session["lines"])
                groups.setdefault(f["build"], []).append((number, session, f))
            if not sessions:
                groups.setdefault(facts(start, start)["build"], []).append((number, None, None))
        for build, items in groups.items():
            out += ["Sessions of " + build + ":", ""]
            for number, session, f in items:
                if session is None: out += ["### program start %d: no playback session in the collected lines" % number, ""]; continue
                out += describe_session(f, "program start %d, playback session %s" % (number, session["session"]))
    return "\n".join(out) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("path", help="a Collect-Diagnostics zip, its unpacked folder, or one log file")
    parser.add_argument("--out", default="", help="write the text here instead of printing it")
    arguments = parser.parse_args(argv)
    text = triage(read_inputs(arguments.path))
    if arguments.out:
        with open(arguments.out, "w", encoding="utf-8") as handle: handle.write(text)
        print("written: " + arguments.out)
    else: sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
