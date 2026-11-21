#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Reads a PresentMon console capture (CSV) of PushFPS by its header names, any PresentMon version.

  python presentmon.py <presentmon.csv> [--process PushFPS.exe]

PresentMon (GameTechDev/PresentMon, MIT licence, Intel Corporation) is NOT shipped with the kit: the tester downloads
it. Columns are found case-insensitively; 2.x names first, then the 1.x (--v1_metrics) and older 2.x names:
  frame time            MsBetweenPresents | FrameTime
  displayed interval    MsBetweenDisplayChange | DisplayedTime
  present to display    MsUntilDisplayed (DisplayLatency is frame start to display, reported apart)
  time inside Present   MsInPresentAPI | MsInPresent
  present mode          PresentMode;  tearing  AllowsTearing;  dropped  Dropped (1.x) or an empty / NA displayed value
'NA' and empty cells count as missing. Only rows of the named process; of its swap chains the busiest one (the
interface's own chain presents a few frames only). Python standard library only.
"""
import argparse, csv, math, sys

FRAME = ("MsBetweenPresents", "FrameTime")
SHOWN = ("MsBetweenDisplayChange", "DisplayedTime")
UNTIL = ("MsUntilDisplayed",)
FRAME_TO_DISPLAY = ("DisplayLatency",)   # 2.x: from the START of the frame's CPU work to display, not from Present()
IN_PRESENT = ("MsInPresentAPI", "MsInPresent")
INDEPENDENT = ("Hardware: Independent Flip", "Hardware Composed: Independent Flip", "Hardware: Legacy Flip", "Hardware: Legacy Copy to front buffer")

def number(text):
    try: value = float(str(text).strip())
    except (TypeError, ValueError): return None
    return value if math.isfinite(value) else None

def percentile(values, p):
    if not values: return float("nan")
    s = sorted(values); k = (len(s) - 1) * p; lo = int(k); hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)

def pick(row, names):
    for name in names:
        value = row.get(name.lower())
        if value not in (None, "") and str(value).strip().upper() != "NA": return value
    return None

def has(header, names): return any(n.lower() in header for n in names)

def read(path, process="pushfps"):
    """Rows of the busiest swap chain of the process, keyed in lower case; (rows, header) or (None, header)."""
    chains, header = {}, []
    try: handle = open(path, newline="", encoding="utf-8-sig", errors="replace")
    except OSError: return None, header
    with handle:
        reader = csv.DictReader(handle)
        header = [(h or "").strip().lower() for h in (reader.fieldnames or [])]
        for raw in reader:
            row = {(k or "").strip().lower(): v for k, v in raw.items() if k is not None}
            name = (row.get("application") or row.get("processname") or "").lower()
            if process.lower() not in name: continue
            chains.setdefault(row.get("swapchainaddress") or "?", []).append(row)
    if not chains: return None, header
    return max(chains.values(), key=len), header

def summary(path, process="pushfps"):
    """Per capture: present mode shares, tearing, frame time and displayed interval quantiles; None without rows."""
    rows, header = read(path, process)
    if not rows: return None
    frame, shown, until, in_present, modes, frame_to_display = [], [], [], [], {}, []
    tearing = dropped = 0
    for r in rows:
        mode = (r.get("presentmode") or "?").strip() or "?"
        modes[mode] = modes.get(mode, 0) + 1
        if str(r.get("allowstearing") or "").strip() == "1": tearing += 1
        value = number(pick(r, FRAME))
        if value is not None and value > 0: frame.append(value)
        if "dropped" in r: is_dropped = str(r["dropped"]).strip() == "1"
        else: is_dropped = number(pick(r, SHOWN)) is None and has(header, SHOWN)
        if is_dropped: dropped += 1
        else:
            value = number(pick(r, SHOWN))
            if value is not None and value > 0: shown.append(value)
            value = number(pick(r, UNTIL))
            if value is not None and value >= 0: until.append(value)
            value = number(pick(r, FRAME_TO_DISPLAY))
            if value is not None and value >= 0: frame_to_display.append(value)
        value = number(pick(r, IN_PRESENT))
        if value is not None and value >= 0: in_present.append(value)
    total = len(rows)
    independent = sum(n for m, n in modes.items() if m in INDEPENDENT)
    composed = sum(n for m, n in modes.items() if m.lower().startswith("composed"))
    return {"frames": total, "modes": modes, "independentPercent": 100.0 * independent / total, "composedPercent": 100.0 * composed / total,
            "tearingPercent": 100.0 * tearing / total, "droppedPercent": 100.0 * dropped / total,
            "frameP50": percentile(frame, .5), "frameP95": percentile(frame, .95), "frameP99": percentile(frame, .99),
            "shownP95": percentile(shown, .95), "shownP99": percentile(shown, .99),
            "untilP50": percentile(until, .5), "untilP95": percentile(until, .95), "inPresentP50": percentile(in_present, .5),
            "frameToDisplayP50": percentile(frame_to_display, .5), "frameToDisplayP95": percentile(frame_to_display, .95),
            "columns": {"frame": has(header, FRAME), "shown": has(header, SHOWN), "until": has(header, UNTIL), "inPresent": has(header, IN_PRESENT), "frameToDisplay": has(header, FRAME_TO_DISPLAY)}}

def merge(summaries):
    """One arm: frame-weighted shares, medians of the per-capture quantiles."""
    items = [s for s in summaries if s]
    if not items: return None
    frames = sum(s["frames"] for s in items)
    out = {"captures": len(items), "frames": frames}
    for key in ("independentPercent", "composedPercent", "tearingPercent", "droppedPercent"):
        out[key] = sum(s[key] * s["frames"] for s in items) / frames
    for key in ("frameP50", "frameP95", "frameP99", "shownP95", "shownP99", "untilP50", "untilP95", "inPresentP50", "frameToDisplayP50", "frameToDisplayP95"):
        values = sorted(s[key] for s in items if s[key] == s[key])
        out[key] = percentile(values, .5) if values else float("nan")
    return out

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv"); ap.add_argument("--process", default="PushFPS")
    args = ap.parse_args(argv)
    s = summary(args.csv, args.process)
    if not s: print("no rows of that process"); return 1
    for key, value in s.items(): print(f"{key}: {value}")
    return 0

if __name__ == "__main__":
    sys.exit(main())
