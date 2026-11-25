#!/usr/bin/env python3
"""End-to-end engine test of "Lower delay": runs veyra_lowdelay_e2e_tests in several configurations and asserts from the
app's own outputs (engine.log: [pacing-sample], [submit], [fg-sched], [capture-timing], [fg-health], [vfg],
[engine-stall], [slipstream-smooth]; states.csv: the row state the interface reads). docs/drafts/E2E_ENGINE_TEST.md.

  python lowdelay_e2e.py --exe <veyra_lowdelay_e2e_tests.exe> --out <dir> --expect p15|fixed [--runs ...] [--uhd-seconds N]

--expect p15:   Package 15's code. The OWNER SCENARIO (his settings, from his log of 24 Nov 2026) must REPRODUCE his log:
                the mode does not run (state not-here, refusal smoothness-mode, predictedPairs 0), and 2 % skipped pairs
                collapse the predicted share. These are asserted as the baseline (expected until fixed).
--expect fixed: the branch that makes the mode run with Smoothness Mode and Live HUD Lock: the same scenario must run
                predicted (>= 90 % of pairs, I, B, P, wait part about one slot, readout about one slot lower than the row
                off), and 2 % skipped pairs keep >= 90 %.
The secondary scenario (59.94 Hz with every picture twice, a clean configuration) is asserted the same in both phases,
except that HUD Lock and Smoothness Mode must no longer refuse the mode in the fixed phase.

Exit code 0 only if every assertion passed; <out>/summary.json holds the numbers. Without --exe it analyses existing run
folders under --out. Standard library only.
"""
from __future__ import annotations

import argparse
import csv
import datetime
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

# A 30 FPS game: content interval 33.37 ms (59.94 Hz with repeats) or 33.33 ms (30.00 Hz card). 3X: one slot is T/3.
SLOT_MS = 2 * 1001 / 60000 * 1000.0 / 3.0     # 11.12 ms; the 30 Hz card's slot (11.11 ms) differs by 0.01 ms
WARMUP_S = 4.0          # start-up: graph creation, the learners, the calm run of 8 pairs (and its log) are excluded
# The owner's VFG costs at 4K HDR (his [fg-health]: gpu_fg1 P50 2.0-2.15 ms, fg2 P50 4.2-4.5 ms after the pair starts).
OWNER_COSTS = "2.0,2.2"

LINE = re.compile(r"^(?P<ts>\S+)\s+t=\d+\s+\[(?P<level>[A-Z ]+)\]\s+\[(?P<tag>[\w.-]+)\]\s?(?P<msg>.*)$")
KV = re.compile(r"(\w+)=([^\s,()]+)")


def kv(message: str) -> dict:
    return {k: v for k, v in KV.findall(message)}


def number(v, default=None):
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def truthy(v) -> bool:
    return str(v).lower() in ("1", "true")


def seconds_of(ts: str):
    try:
        return datetime.datetime.strptime(ts.rstrip("Z"), "%Y-%m-%dT%H:%M:%S.%f").timestamp()
    except ValueError:
        return None


@dataclass
class Present:
    pts: int
    end: int          # host 100 ns, Present returned
    arrival: int      # host 100 ns, capture arrival of the item's source
    generated: bool


@dataclass
class Run:
    name: str
    presents: list = field(default_factory=list)        # Present, in log order
    submits: list = field(default_factory=list)         # (batch, subframe, pts, host)
    fg_sched: list = field(default_factory=list)        # messages of [fg-sched]
    timing: list = field(default_factory=list)          # dicts of [capture-timing]
    health: list = field(default_factory=list)          # dicts of [fg-health]
    smooth: list = field(default_factory=list)          # messages of [slipstream-smooth]
    stalls: list = field(default_factory=list)          # (log seconds, totalMs) of [engine-stall]
    toggles: list = field(default_factory=list)         # (log seconds, wall, on) of [e2e-host] toggle
    vfg: list = field(default_factory=list)             # messages of [vfg]
    predict: list = field(default_factory=list)         # messages of [push-predict]
    capture_test: list = field(default_factory=list)
    errors: list = field(default_factory=list)          # [ERROR] lines
    dropped_log_lines: int = 0
    states: list = field(default_factory=list)          # rows of states.csv
    host_output: str = ""
    device_failures: list = field(default_factory=list)  # [gpu-failure] lines and removed-device reasons
    tail: list = field(default_factory=list)             # the last 40 lines of engine.log
    stalls_injected: list = field(default_factory=list)  # (startHost100ns, gapMs) of [e2e-stall]
    anchors: list = field(default_factory=list)          # dicts of [live-anchor]


def parse_log(text: str, run: Run) -> None:
    lines = text.splitlines()
    run.tail = lines[-40:]
    for raw in lines:
        if "[gpu-failure]" in raw or "removedReason=0x887A" in raw or "DXGI_ERROR_DEVICE_REMOVED" in raw:
            run.device_failures.append(raw)
        m = LINE.search(raw)
        if not m:
            continue
        tag, msg = m.group("tag"), m.group("msg")
        if m.group("level").strip() == "ERROR":
            run.errors.append(f"[{tag}] {msg}")
        if tag == "pacing-sample":
            d = kv(msg)
            try:
                run.presents.append(Present(int(d["pts"]), int(d["end"]), int(d.get("arrival", 0)), truthy(d.get("generated"))))
            except (KeyError, ValueError):
                pass
        elif tag == "submit":
            d = kv(msg)
            try:
                run.submits.append((int(d["batch"]), int(d["subframe"]), int(d["pts100ns"]), int(d["host100ns"])))
            except (KeyError, ValueError):
                pass
        elif tag == "fg-sched":
            run.fg_sched.append(msg)
        elif tag == "capture-timing":
            run.timing.append(kv(msg))
        elif tag == "fg-health":
            run.health.append(kv(msg))
        elif tag == "slipstream-smooth":
            run.smooth.append(msg)
        elif tag == "engine-stall":
            run.stalls.append((seconds_of(m.group("ts")), number(kv(msg).get("totalMs"), 0.0)))
        elif tag == "e2e-host" and msg.startswith("toggle"):
            d = kv(msg)
            run.toggles.append((seconds_of(m.group("ts")), number(d.get("wall"), 0.0), truthy(d.get("lowDelay"))))
        elif tag == "vfg":
            run.vfg.append(msg)
        elif tag == "e2e-stall":
            d = kv(msg)
            run.stalls_injected.append((int(number(d.get("startHost100ns"), 0)), number(d.get("gapMs"), 0.0)))
        elif tag == "live-anchor":
            run.anchors.append(kv(msg))
        elif tag == "push-predict":
            run.predict.append(msg)
        elif tag == "capture-test":
            run.capture_test.append(msg)
        elif tag == "log-backpressure":
            run.dropped_log_lines += int(number(kv(msg).get("dropped"), 0))


def load_run(folder: Path, name: str) -> Run:
    run = Run(name)
    log = folder / "engine.log"
    if log.exists():
        parse_log(log.read_text(encoding="utf-8", errors="replace"), run)
    states = folder / "states.csv"
    if states.exists():
        with states.open(newline="", encoding="utf-8") as f:
            run.states = list(csv.DictReader(f))
    out = folder / "host-output.txt"
    if out.exists():
        run.host_output = out.read_text(encoding="utf-8", errors="replace")
    return run


def pair_patterns(run: Run, warmup_s: float = WARMUP_S) -> dict:
    """Per pair (batch), the order of real and generated presents: interpolate = (2 before, 0 after),
    one-predicted = (1 before, 1 after). Generated/real from [pacing-sample] by pts; batches from [submit]."""
    generated_by_pts = {p.pts: p.generated for p in run.presents}
    if not run.submits:
        return {"batches": 0, "predicted": 0, "predictedShare": 0.0}
    start = run.submits[0][3] + int(warmup_s * 1e7)
    batches: dict[int, list] = {}
    for batch, _sub, pts, host in run.submits:
        if host < start or pts not in generated_by_pts:
            continue
        batches.setdefault(batch, []).append((host, pts, generated_by_pts[pts]))
    counts = {"interpolate": 0, "predicted": 0, "other": 0, "noReal": 0, "ptsOutOfOrder": 0}
    ids = sorted(batches)
    for b in ids[1:-1]:                       # the first and last may be cut by the window
        items = sorted(batches[b])
        reals = [i for i, it in enumerate(items) if not it[2]]
        if len(reals) != 1:
            counts["noReal"] += 1
            continue
        r = reals[0]
        before, after = r, len(items) - r - 1
        if any(items[i][1] >= items[i + 1][1] for i in range(len(items) - 1)):
            counts["ptsOutOfOrder"] += 1
        if (before, after) == (2, 0):
            counts["interpolate"] += 1
        elif (before, after) == (1, 1):
            counts["predicted"] += 1
        else:
            counts["other"] += 1
    counts["batches"] = sum(v for k, v in counts.items() if k in ("interpolate", "predicted", "other", "noReal"))
    counts["predictedShare"] = counts["predicted"] / counts["batches"] if counts["batches"] else 0.0
    return counts


def readout(run: Run, warmup_s: float = WARMUP_S) -> dict:
    """Capture arrival -> the REAL frame's Present returned (the delay badge's definition), after start-up."""
    reals = [p for p in run.presents if not p.generated and p.arrival > 0]
    if not reals:
        return {"n": 0}
    start = reals[0].end + int(warmup_s * 1e7)
    ms = [(p.end - p.arrival) / 1e4 for p in reals if p.end >= start]
    gen = sum(1 for p in run.presents if p.generated and p.end >= start)
    if not ms:
        return {"n": 0}
    ms_sorted = sorted(ms)
    return {"n": len(ms), "meanMs": statistics.fmean(ms), "p50Ms": ms_sorted[len(ms) // 2],
            "p95Ms": ms_sorted[min(len(ms) - 1, int(0.95 * len(ms)))], "generatedPerReal": gen / len(ms)}


def timing_field(run: Run, key: str, skip_first: int = 3):
    """Median of a [capture-timing] field over the 1 s windows after start-up (the first `skip_first` lines dropped)."""
    values = [number(t.get(key)) for t in run.timing[skip_first:]]
    values = [v for v in values if v is not None and v >= 0]
    return statistics.median(values) if values else None


def health_field(run: Run, key: str):
    values = [number(h.get(key)) for h in run.health]
    values = [v for v in values if v is not None and v >= 0]
    return statistics.median(values) if values else None


def last_states(run: Run, warmup_s: float = WARMUP_S) -> list:
    return [s for s in run.states if number(s.get("wall"), 0) >= warmup_s]


def share(rows: list, key: str, value: str) -> float:
    return sum(1 for r in rows if r.get(key) == value) / len(rows) if rows else 0.0


def lowdelay_lines(run: Run) -> list:
    return [m for m in run.fg_sched if m.startswith("lowDelay state=")]


class Checks:
    def __init__(self):
        self.items = []

    def check(self, ok: bool, name: str, detail: str = "") -> bool:
        self.items.append({"ok": bool(ok), "name": name, "detail": detail})
        print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail else ""), flush=True)
        return bool(ok)

    def note(self, name: str, detail: str) -> None:
        self.items.append({"ok": True, "name": "NOTE " + name, "detail": detail})
        print("NOTE " + name + f"  ({detail})", flush=True)

    @property
    def failed(self) -> int:
        return sum(1 for i in self.items if not i["ok"])


# The owner's settings, from his Package 15 log: 3840x2160 card at 30.00 Hz (no repeats), VFG 3X quality 5, Smoothness
# Mode target 90, Live HUD Lock, low queue, NVIDIA low latency requested, windowed, a 179.952 Hz display. Display sync
# Automatic (he switched smooth pacing off at 22:58 and played on so): CI's runner has a 64 Hz display, so Vsync caps the
# presents at 64 a second there; with Automatic, Present(0,0) does not wait for the display. The engine is told his refresh
# by the existing test switch VEYRA_TEST_DISPLAY_HZ (read where Smoothness Mode and the output hint take the refresh).
OWNER = ["--hud-lock", "--smoothness", "--quality", "5", "--nv-low-latency", "--sync", "auto"]
OWNER_ENV = {"VEYRA_TEST_FAKE_VFG_COST_MS": OWNER_COSTS}
OWNER_HZ = "179.952"

# name -> (clip kind, host arguments, extra environment, length kind)
RUNS = {
    # Primary: the owner scenario.
    "ownerOff": ("card30", OWNER, OWNER_ENV, "main"),
    "owner": ("card30", OWNER + ["--low-delay"], OWNER_ENV, "main"),
    # His start: smooth pacing (Vsync + VRR + cap), switched off at 10 s. On the 64 Hz runner Vsync limits the presents, so
    # only the mode's state (the reproduction / the fix) is asserted here, not pacing or delay.
    "ownerVsync": ("card30", ["--hud-lock", "--smoothness", "--quality", "5", "--nv-low-latency", "--smooth-pacing", "--low-delay",
                              "--sync-auto-at", "10"], OWNER_ENV, "main"),
    "ownerToggle": ("card30", OWNER + ["--low-delay", "--toggle-at", "8", "--toggle-at", "14"], OWNER_ENV, "main"),
    # The forensic report's regression case: a configuration the mode runs in on Package 15, with 2 % skipped pairs.
    "clean30": ("card30", ["--low-delay", "--quality", "5"], OWNER_ENV, "main"),
    "skip2": ("card30", ["--low-delay", "--quality", "5"], dict(OWNER_ENV, VEYRA_TEST_SKIP_PAIRS_PERMILLE="20"), "main"),
    # 60 s of predicted pairs: a device removal or an exit that is not immediate (fixed branch; Package 15 removes the device
    # at the first predicted pair on WARP, run 45530162944).
    "predictedSoak": ("card30", ["--low-delay", "--quality", "5"], OWNER_ENV, "soak"),
    # The owner's capture stalls (a callback gap of 92-158 ms about every quarter hour, the device timestamps still stepping,
    # then a burst of late frames), injected every 6 s with a 120 ms gap. Measurement first: the baseline for riding through a
    # stall without buffering. Row off (today's schedule) and row on (the fixed branch: the mode neither leaves nor counts
    # several interruptions).
    "stallRide": ("card30", OWNER, dict(OWNER_ENV, VEYRA_TEST_REPLAY_STALL="6,120"), "main"),
    "stallRideOn": ("card30", OWNER + ["--low-delay"], dict(OWNER_ENV, VEYRA_TEST_REPLAY_STALL="6,120"), "main"),
    # Secondary: 59.94 Hz with every picture twice.
    "A": ("card60", ["--content-auto"], {}, "main"),
    "B": ("card60", ["--content-auto", "--low-delay"], {}, "main"),
    "L": ("card60", ["--content-auto"], {"VEYRA_TEST_RP_LEGACY_PACKAGE14": "1"}, "main"),
    "R": ("card60", ["--content-auto", "--low-delay"], {"VEYRA_TEST_VFG_REJECT_ALONE": "1"}, "main"),
    "T": ("card60", ["--content-auto", "--toggle-at", "8"], {}, "main"),
    "hud": ("card60", ["--content-auto", "--low-delay", "--hud-lock"], {}, "static"),
    "excl": ("card60", ["--content-auto", "--low-delay", "--exclusion"], {}, "static"),
    "smooth": ("card60", ["--content-auto", "--low-delay", "--smoothness"], {}, "static"),
    "m2": ("card60", ["--content-auto", "--low-delay", "--multiplier", "2"], {}, "static"),
    "m4": ("card60", ["--content-auto", "--low-delay", "--multiplier", "4"], {}, "static"),
    "adaptive": ("card60", ["--content-auto", "--low-delay", "--adaptive"], {}, "static"),
}
STATIC_EXPECTED = {"hud": "hud-lock-or-exclusion", "excl": "hud-lock-or-exclusion", "smooth": "smoothness-mode",
                   "m2": "not-3x", "m4": "not-3x", "adaptive": "adaptive"}
# Optional runs that need ffmpeg on the runner: 4K correctness and the owner's HDR10 path (a 10-bit PQ clip).
OPTIONAL = {
    "ownerUhd": ("uhd", OWNER + ["--low-delay"], OWNER_ENV, "uhd"),
    "ownerHdr": ("hdr", OWNER + ["--low-delay"], dict(OWNER_ENV, VEYRA_TEST_ASSUME_HDR_DISPLAY="1"), "main"),
}
# Diagnostic variants of the row-off owner run (no assertions beyond the common ones; a NOTE with the numbers): which of
# his settings makes WARP fall behind (build 45517046834: ownerOff generatedPerReal 0.51, Smoothness Mode stepping to 2X,
# while the 59.94 Hz runs kept up with Smoothness Mode on).
DIAG = {
    "diagTearing": ("card30", ["--hud-lock", "--smoothness", "--quality", "5", "--nv-low-latency", "--sync", "tearing"], OWNER_ENV, "main"),
    "diagNoHud": ("card30", ["--smoothness", "--quality", "5", "--nv-low-latency", "--sync", "auto"], OWNER_ENV, "main"),
    "diagNoNvll": ("card30", ["--hud-lock", "--smoothness", "--quality", "5", "--sync", "auto"], OWNER_ENV, "main"),
    "diagPlain": ("card30", ["--quality", "5", "--sync", "tearing"], OWNER_ENV, "main"),
}
P15_OWNER_REFUSAL = "smoothness-mode"   # his log: lowDelay state=not-here refusal=smoothness-mode predictedPairs=0


def check_common(name: str, run: Run, summary: dict, checks: Checks) -> None:
    summary[name] = {"patterns": pair_patterns(run), "readout": readout(run), "droppedLogLines": run.dropped_log_lines,
                     "finalState": (run.states[-1].get("rowState"), run.states[-1].get("rowRefusal")) if run.states else None,
                     "delayWaitMs": timing_field(run, "delayWaitMs"), "delayMs": timing_field(run, "delayMs"),
                     "fg1P50Ms": health_field(run, "gpu_fg1P50Ms"), "fg2P50Ms": health_field(run, "gpu_fg2P50Ms"),
                     "fgBatchP50Ms": health_field(run, "gpu_fgBatchP50Ms"),
                     "lowDelayLines": lowdelay_lines(run)[-4:], "smooth": run.smooth[-3:], "errors": run.errors[:10],
                     "stallsMs": [round(t, 1) for _, t in run.stalls],
                     "liveAnchorSnaps": number(run.anchors[-1].get("snaps")) if run.anchors else None}
    checks.check(any("TEST-ONLY fake VFG" in m for m in run.vfg), f"{name}: the fake VFG ran (named in the log)",
                 next((m[:240] for m in run.vfg if "TEST-ONLY fake VFG" in m), "no [vfg] TEST-ONLY line; errors: " + "; ".join(run.errors[:3])))
    checks.check("OK 0" in run.host_output or not run.host_output, f"{name}: host checks passed", run.host_output.strip()[-300:])
    # The device must survive every scenario: a removed device is a failure whatever else the run shows.
    if not checks.check(not run.device_failures, f"{name}: the D3D12 device was never removed (no [gpu-failure])",
                        run.device_failures[0][:300] if run.device_failures else ""):
        print(f"----- {name}: last 40 lines of engine.log -----")
        for line in run.tail:
            print("  " + line[:400])
        print("----- end -----", flush=True)
    if run.dropped_log_lines:
        checks.check(run.dropped_log_lines < 50, f"{name}: few verbose log lines dropped", str(run.dropped_log_lines))
    s = summary[name]
    fake = [kv(m) for m in run.vfg if "TEST-ONLY fake VFG timing" in m]
    q = [number(f.get("queuedToProducerMeanMs")) for f in fake if number(f.get("queuedToProducerMeanMs")) is not None]
    c = [number(f.get("costWaitedMeanMs")) for f in fake if number(f.get("costWaitedMeanMs")) is not None]
    s["fakeQueuedToProducerMs"] = statistics.median(q) if q else None
    s["fakeCostWaitedMs"] = statistics.median(c) if c else None
    checks.note(f"{name}: the fake's time", f"queuedToProducer={s['fakeQueuedToProducerMs']} costWaited={s['fakeCostWaitedMs']} (median of 2 s windows)")
    checks.note(f"{name}: costs on WARP (software renderer, NOT a GPU measurement)",
                f"fg1P50={s['fg1P50Ms']} fg2P50={s['fg2P50Ms']} (fg2 = the predicted frame P on predicted pairs) "
                f"fgBatchP50={s['fgBatchP50Ms']} delayWait={s['delayWaitMs']} delay={s['delayMs']}")


def keeps_up(name: str, summary: dict, checks: Checks, minimum: float = 1.8) -> bool:
    r = summary[name]["readout"]
    return checks.check(r.get("generatedPerReal", 0) >= minimum,
                        f"{name}: the pipeline keeps up (>= {minimum} generated frames shown per real frame)", json.dumps(r))


def check_runs_predicted(name: str, run: Run, summary: dict, checks: Checks, base: str | None) -> None:
    p, r = summary[name]["patterns"], summary[name]["readout"]
    st = last_states(run)
    checks.check(share(st, "rowState", "active") >= 0.9, f"{name}: the row reports active (>= 90 % of samples after start-up)",
                 f"active={share(st, 'rowState', 'active'):.2f} final={summary[name]['finalState']}")
    checks.check(p.get("predictedShare", 0) >= 0.9, f"{name}: >= 90 % of pairs shown I, B, P (predictedPairs > 0)", json.dumps(p))
    checks.check(p.get("ptsOutOfOrder", 0) == 0, f"{name}: presents of every pair in content order", json.dumps(p))
    keeps_up(name, summary, checks)
    wait = summary[name]["delayWaitMs"]
    checks.check(wait is not None and abs(wait - SLOT_MS) <= 0.35 * SLOT_MS, f"{name}: [capture-timing] wait part about one slot",
                 f"delayWaitMs={wait} slot={SLOT_MS:.2f}")
    if base and base in summary and summary[base]["readout"].get("n") and r.get("n"):
        gain = (summary[base]["readout"]["meanMs"] - r["meanMs"]) / SLOT_MS
        summary[name]["gainSlots"] = gain
        checks.check(0.6 <= gain <= 1.4, f"{name}: real frame's readout lower than {base}'s by about one output slot (0.6-1.4 slots)",
                     f"{base}={summary[base]['readout']['meanMs']:.2f} ms {name}={r['meanMs']:.2f} ms gain={gain:.2f} slots of {SLOT_MS:.2f} ms")


def check_owner(name: str, run: Run, summary: dict, checks: Checks, expect: str, base: str | None = "ownerOff") -> None:
    p = summary[name]["patterns"]
    st = last_states(run)
    checks.check(share(st, "smoothChosen", "3") >= 0.9, f"{name}: Smoothness Mode (target 90) chooses 3X",
                 f"3X share={share(st, 'smoothChosen', '3'):.2f} {summary[name]['smooth'][-1:]}")
    if expect == "p15":
        lines = lowdelay_lines(run)
        checks.check(any(f"state=not-here refusal={P15_OWNER_REFUSAL}" in m for m in lines) and
                     share(st, "rowRefusal", P15_OWNER_REFUSAL) >= 0.9 and share(st, "rowState", "not-here") >= 0.9,
                     f"{name}: REPRODUCES the owner's log (expected until fixed): state=not-here refusal={P15_OWNER_REFUSAL}",
                     (lines[-1][:200] if lines else "no [fg-sched] lowDelay line") + f" final={summary[name]['finalState']}")
        checks.check(p.get("predicted", 0) == 0 and not any(re.search(r"predictedPairs=[1-9]", m) for m in lines),
                     f"{name}: REPRODUCES predictedPairs=0 (expected until fixed)", json.dumps(p))
        keeps_up(name, summary, checks)
        wait = summary[name]["delayWaitMs"]
        checks.check(wait is not None and abs(wait - 2 * SLOT_MS) <= 0.35 * SLOT_MS,
                     f"{name}: wait part about two slots, as his 22.1 ms (expected until fixed)", f"delayWaitMs={wait}")
        if base and base in summary and summary[base]["readout"].get("n") and summary[name]["readout"].get("n"):
            d = abs(summary[base]["readout"]["meanMs"] - summary[name]["readout"]["meanMs"]) / SLOT_MS
            checks.check(d <= 0.3, f"{name}: readout as the row-off run's (within 0.3 slot): no gain, as he saw (expected until fixed)",
                         f"{base}={summary[base]['readout']['meanMs']:.2f} {name}={summary[name]['readout']['meanMs']:.2f}")
    else:
        check_runs_predicted(name, run, summary, checks, base)
        # Live HUD Lock in P: the fixed branch must log how many HUD-locked pixels of P differ from B (a GPU check).
        hud = [kv(m) for m in run.predict if "hudLockMismatch" in m]
        checks.check(bool(hud) and all(number(h.get("hudLockMismatch"), 1) == 0 for h in hud),
                     f"{name}: HUD-locked pixels of P identical to B's ([push-predict] hudLockMismatch=0)",
                     str(hud[-1]) if hud else "no [push-predict] hudLockMismatch= line: the fixed branch must log this GPU check")


def stall_effects(run: Run, window_s: float = 3.0) -> list:
    """Per injected stall: real frames shown late (readout > pre-stall median + 5 ms) in 3 s from the stall, the same count in
    the 3 s before it (WARP's own jitter), the worst readout, generated frames shown per real frame in the window, and the
    time from the end of the gap until 10 real frames in a row are no longer late (pacing back)."""
    reals = [p for p in run.presents if not p.generated and p.arrival > 0]
    out = []
    for start, gap in run.stalls_injected:
        before = [(p.end - p.arrival) / 1e4 for p in reals if start - 3e7 <= p.end < start]
        if len(before) < 10:
            continue
        median = statistics.median(before)
        end_gap = start + int(gap * 1e4)
        inside = [p for p in reals if start <= p.end < start + int(window_s * 1e7)]
        ms = [(p.end - p.arrival) / 1e4 for p in inside]
        late = sum(1 for v in ms if v > median + 5)
        gen = sum(1 for p in run.presents if p.generated and start <= p.end < start + int(window_s * 1e7))
        late_before = sum(1 for v in before if v > median + 5)
        back = None
        streak = 0
        for p in reals:
            if p.end < end_gap:
                continue
            if (p.end - p.arrival) / 1e4 <= median + 5:
                streak += 1
                if streak == 10:
                    back = (p.end - end_gap) / 1e4
                    break
            else:
                streak = 0
        out.append({"gapMs": gap, "medianBeforeMs": round(median, 2), "lateReal": late, "lateRealIn3sBefore": late_before,
                    "worstMs": round(max(ms), 2) if ms else None,
                    "generatedPerRealInWindow": round(gen / len(inside), 2) if inside else None, "pacingBackMs": back})
    return out


def check_stall(name: str, run: Run, summary: dict, checks: Checks, expect: str) -> None:
    effects = stall_effects(run)
    summary[name]["stalls"] = effects
    snaps = [number(a.get("snaps")) for a in run.anchors if number(a.get("snaps")) is not None]
    summary[name]["anchorSnaps"] = (snaps[0], snaps[-1]) if snaps else None
    checks.check(len(run.stalls_injected) >= 2, f"{name}: stalls injected", str(run.stalls_injected))
    checks.note(f"{name}: what the schedule does in a stall (WARP; the baseline for riding through it without buffering)",
                json.dumps({"stalls": effects, "liveAnchorSnapsFirstLast": summary[name]["anchorSnaps"]}))
    if name == "stallRideOn" and expect == "fixed":
        st = last_states(run)
        checks.check(share(st, "rowState", "active") >= 0.95, f"{name}: the mode does not leave in the stalls (active >= 95 %)",
                     f"active={share(st, 'rowState', 'active'):.2f}")
        exits = [m for m in lowdelay_lines(run) if re.search(r"exits=\S*(skipped|held|refused|late):[1-9]", m)]
        checks.check(not exits, f"{name}: no exits counted for the stalls", exits[-1][:220] if exits else "")


def check_owner_state_only(name: str, run: Run, summary: dict, checks: Checks, expect: str) -> None:
    st = last_states(run)
    checks.check(share(st, "smoothChosen", "3") >= 0.9, f"{name}: Smoothness Mode (target 90) chooses 3X",
                 f"3X share={share(st, 'smoothChosen', '3'):.2f} {summary[name]['smooth'][-1:]}")
    if expect == "p15":
        checks.check(share(st, "rowRefusal", P15_OWNER_REFUSAL) >= 0.9 and share(st, "rowState", "not-here") >= 0.9,
                     f"{name}: REPRODUCES not-here / {P15_OWNER_REFUSAL} with smooth pacing on and after it is switched off (expected until fixed)",
                     str(summary[name]["finalState"]))
    else:
        after = [r for r in st if number(r.get("wall"), 0) >= 10 + WARMUP_S]
        checks.check(share(after, "rowState", "active") >= 0.9, f"{name}: active after smooth pacing is switched off (fixed branch)",
                     f"active={share(after, 'rowState', 'active'):.2f} final={summary[name]['finalState']}")
    checks.note(f"{name}: pacing and delay not asserted", "the runner's display is 64 Hz: Vsync limits presents there")


def check_toggles(name: str, run: Run, summary: dict, checks: Checks, expect: str, owner: bool, within_pairs: int = 90) -> None:
    checks.check(len(run.toggles) >= 1, f"{name}: the row was switched during play", str(run.toggles))
    per_toggle = []
    for ts, wall, on in run.toggles:
        stall = max([ms for t, ms in run.stalls if t is not None and ts is not None and 0 <= t - ts <= 3.0] or [0.0])
        per_toggle.append({"wall": wall, "on": on, "stallMs": stall})
    summary[name]["toggleStalls"] = per_toggle
    checks.note(f"{name}: engine stall per toggle ([engine-stall], WARP; his PC: 656-1515 ms)", json.dumps(per_toggle))
    walls_on = [w for _, w, o in run.toggles if o]
    for _, wall, on in run.toggles:
        if on:
            after = [r for r in run.states if number(r.get("wall"), 0) >= wall]
            first_active = next((number(r["wall"]) for r in after if r.get("rowState") == "active"), None)
            if expect == "p15" and owner:
                checks.check(first_active is None and bool(after) and all(r.get("rowRefusal") == P15_OWNER_REFUSAL for r in after[-20:]),
                             f"{name}: after switching on at {wall:.1f} s the row says {P15_OWNER_REFUSAL} again (expected until fixed)",
                             str(after[-1] if after else None))
            else:
                pairs = (first_active - wall) * 30 if first_active is not None else None
                checks.check(pairs is not None and pairs <= within_pairs,
                             f"{name}: active again within {within_pairs} pairs after switching on at {wall:.1f} s", f"pairs={pairs}")
        else:
            nxt = min([w for w in walls_on if w > wall] or [1e9])
            later = [r for r in run.states if wall + 1.0 <= number(r.get("wall"), 0) < nxt - 0.2]
            checks.check(bool(later) and all(r.get("rowState") == "idle" for r in later), f"{name}: row off at {wall:.1f} s reads idle",
                         str(later[-1] if later else None))


def assert_runs(runs: dict, checks: Checks, expect: str = "p15") -> dict:
    summary: dict = {"expect": expect, "slotMs": SLOT_MS}
    for name, run in runs.items():
        check_common(name, run, summary, checks)

    # ---- primary: the owner scenario ----
    if "ownerOff" in runs:
        p = summary["ownerOff"]["patterns"]
        checks.check(p.get("batches", 0) >= 60 and p.get("predicted", 0) == 0, "ownerOff: pairs presented, none predicted", json.dumps(p))
        keeps_up("ownerOff", summary, checks)
        wait = summary["ownerOff"]["delayWaitMs"]
        checks.check(wait is not None and abs(wait - 2 * SLOT_MS) <= 0.35 * SLOT_MS, "ownerOff: wait part about two slots (his 22.1 ms)",
                     f"delayWaitMs={wait}")
    for name in ("owner", "ownerHdr"):
        if name in runs:
            check_owner(name, runs[name], summary, checks, expect)
    if "ownerVsync" in runs:
        check_owner_state_only("ownerVsync", runs["ownerVsync"], summary, checks, expect)
    if "ownerHdr" in runs:
        checks.check(any("format=24" in m and "TEST-ONLY fake VFG" in m for m in runs["ownerHdr"].vfg),
                     "ownerHdr: frame generation runs in the HDR10 format (R10G10B10A2 = 24)", "; ".join(m[:160] for m in runs["ownerHdr"].vfg[:2]))
    if "ownerToggle" in runs:
        check_toggles("ownerToggle", runs["ownerToggle"], summary, checks, expect, owner=True)
    for name in ("stallRide", "stallRideOn"):
        if name in runs:
            check_stall(name, runs[name], summary, checks, expect)
    if "clean30" in runs:
        check_runs_predicted("clean30", runs["clean30"], summary, checks, None)
    if "predictedSoak" in runs:
        check_runs_predicted("predictedSoak", runs["predictedSoak"], summary, checks, None)
        exits = [m for m in lowdelay_lines(runs["predictedSoak"]) if "state=active" not in m]
        checks.note("predictedSoak: lowDelay lines that were not active", "; ".join(m[:160] for m in exits[-3:]) or "none")
    if "skip2" in runs:
        p = summary["skip2"]["patterns"]
        skipped = sum(number(t.get("skippedPairs"), 0) for t in runs["skip2"].timing)
        checks.note("skip2: injected skips (VEYRA_TEST_SKIP_PAIRS_PERMILLE=20)", f"skippedPairs={skipped:.0f} predictedShare={p.get('predictedShare', 0):.3f}")
        checks.check(skipped > 0, "skip2: skipped pairs were injected", f"skippedPairs={skipped:.0f}")
        if expect == "p15":
            checks.check(p.get("predictedShare", 1) <= 0.5,
                         "skip2: with 2 % skipped pairs Package 15's rule loses the mode (expected until fixed; scratch model 1 %)", json.dumps(p))
        else:
            checks.check(p.get("predictedShare", 0) >= 0.9, "skip2: with 2 % skipped pairs >= 90 % of pairs still run predicted", json.dumps(p))
    if "ownerUhd" in runs:
        u = runs["ownerUhd"]
        checks.check(any("size=3840x2160" in m and "TEST-ONLY fake VFG" in m for m in u.vfg), "ownerUhd: the fake VFG was created at 3840x2160",
                     "; ".join(m[:160] for m in u.vfg[:3]))
        missing = [m for m in u.predict + u.errors if "pass or flow not created" in m or "not created:" in m]
        text = "\n".join(lowdelay_lines(u))
        checks.check(not missing and "predictPass=true" in text, "ownerUhd: the predicted-frame pass exists at 4K (predictPass=true, no 'not created')",
                     (missing[:1] or [text[-300:] or "no [fg-sched] lowDelay line"])[0])

    for name in DIAG:
        if name in runs:
            s = summary[name]
            checks.note(f"{name}: diagnostic", f"generatedPerReal={s['readout'].get('generatedPerReal')} patterns={json.dumps(s['patterns'])} "
                        f"smooth={s['smooth'][-1:]} fg1P50={s['fg1P50Ms']} fgBatchP50={s['fgBatchP50Ms']} delayWait={s['delayWaitMs']}")

    # ---- secondary: 59.94 Hz, every picture twice ----
    a = runs.get("A")
    if a:
        p = summary["A"]["patterns"]
        checks.check(p.get("batches", 0) >= 60, "A: pairs presented after start-up", json.dumps(p))
        checks.check(p.get("predicted", 0) == 0, "A (row off): no pair ran the predicted schedule", json.dumps(p))
        checks.check(p.get("interpolate", 0) >= 0.9 * max(1, p.get("batches", 0)), "A: >= 90 % of pairs shown I, I, B", json.dumps(p))
        keeps_up("A", summary, checks)
    if "B" in runs:
        check_runs_predicted("B", runs["B"], summary, checks, "A" if a else None)
    rr = runs.get("R")
    if rr:
        p, r = summary["R"]["patterns"], summary["R"]["readout"]
        st = rr.states[-1] if rr.states else {}
        checks.check(any("injected rejection of a FrameIndex-2-only request" in m for m in rr.vfg), "R: the fake refused index-2-alone (log)")
        checks.check(st.get("rowRefusal") == "vfg-refused" and st.get("rowState") == "unavailable", "R: the row says unavailable / vfg-refused", str(st))
        checks.check(p.get("interpolate", 0) >= 0.9 * max(1, p.get("batches", 0)), "R: frame generation continues as I, I, B", json.dumps(p))
        keeps_up("R", summary, checks)
        if a and summary["A"]["readout"].get("n") and r.get("n"):
            d = abs(summary["A"]["readout"]["meanMs"] - r["meanMs"]) / SLOT_MS
            checks.check(d <= 0.3, "R: readout as A's (within 0.3 slot)", f"A={summary['A']['readout']['meanMs']:.2f} R={r['meanMs']:.2f}")
    leg = runs.get("L")
    if leg and a and summary["A"]["readout"].get("n") and summary["L"]["readout"].get("n"):
        ra, rl = summary["A"]["readout"], summary["L"]["readout"]
        checks.check(abs(ra["meanMs"] - rl["meanMs"]) <= 0.25 * SLOT_MS and abs(ra["p95Ms"] - rl["p95Ms"]) <= 0.5 * SLOT_MS,
                     "A equals Package 14's logic (LEGACY_PACKAGE14) within 0.25 slot mean, 0.5 slot P95",
                     f"A mean/p95={ra['meanMs']:.2f}/{ra['p95Ms']:.2f} L={rl['meanMs']:.2f}/{rl['p95Ms']:.2f}")
        pa, pl = summary["A"]["patterns"], summary["L"]["patterns"]
        checks.check(pl.get("predicted", 0) == 0 and abs(pa.get("interpolate", 0) / max(1, pa.get("batches", 1)) -
                     pl.get("interpolate", 0) / max(1, pl.get("batches", 1))) <= 0.05, "A and L show the same pair pattern", f"A={pa} L={pl}")
    if "T" in runs:
        check_toggles("T", runs["T"], summary, checks, expect, owner=False)
    for name, expected in STATIC_EXPECTED.items():
        s = runs.get(name)
        if not s:
            continue
        st = s.states[-1] if s.states else {}
        p = summary[name]["patterns"]
        fixed_runs = expect == "fixed" and name in ("hud", "smooth")
        checks.check(p.get("predicted", 0) == 0 or expected == "adaptive" or fixed_runs, f"{name}: no pair ran predicted", json.dumps(p))
        checks.check(summary[name]["readout"].get("generatedPerReal", 0) >= 0.8, f"{name}: frame generation continues", json.dumps(summary[name]["readout"]))
        if expected == "adaptive":
            mult = st.get("previewFgMultiplier")
            ok = (st.get("rowRefusal") == "not-3x" and mult != "3") or (mult == "3" and st.get("rowState") in ("active", "calm"))
            checks.check(ok, "adaptive: the row names not-3x when the pair runs below 3X (else it may run)", str(st))
        elif fixed_runs:
            checks.check(st.get("rowState") == "active", f"{name}: the mode runs with {expected.replace('-', ' ')} on (fixed branch)", str(st))
        else:
            checks.check(st.get("rowState") == "not-here" and st.get("rowRefusal") == expected, f"{name}: the row says not-here / {expected}", str(st))
    return summary


def run_host(exe: Path, clip: Path, folder: Path, seconds: int, args: list, env_extra: dict) -> None:
    env = dict(os.environ)
    env.update({"VEYRA_TEST_REPLAY_AS_CARD": "1", "VEYRA_TEST_FAKE_VFG": "1", "VEYRA_TEST_ALLOW_WARP": "1", "VEYRA_TEST_DISPLAY_HZ": OWNER_HZ})
    if os.environ.get("E2E_D3D12_DEBUG") == "1":
        env["VEYRA_TEST_D3D12_DEBUG"] = "1"
    env.update(env_extra)
    folder.mkdir(parents=True, exist_ok=True)
    cmd = [str(exe), "--run", str(clip), str(folder), str(seconds)] + args
    print("run:", " ".join(cmd), json.dumps(env_extra), flush=True)
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=seconds + 120)
        out = (proc.stdout or "") + (proc.stderr or "") + f"\nexit={proc.returncode}\n"
    except subprocess.TimeoutExpired as e:
        out = (e.stdout if isinstance(e.stdout, str) else "") + "\nTIMEOUT\n"
    (folder / "host-output.txt").write_text(out, encoding="utf-8")
    print(out[-1500:], flush=True)
    if "FAIL" in out or "TIMEOUT" in out:
        print_excerpt(folder / "engine.log")


EXCERPT_TAGS = ("gpu-failure", "dred", "d3d12-debug", "push-predict", "gpu-dis", "predict", "capture-present", "gfx", "graph")


def print_excerpt(log: Path, before: int = 40, tagged: int = 80) -> None:
    """A failing run's evidence in the job log (artifact downloads may be unavailable): the lines before the first GPU
    failure, and the last lines of the tags that explain one."""
    if not log.exists():
        return
    lines = log.read_text(encoding="utf-8", errors="replace").splitlines()
    first = next((i for i, l in enumerate(lines) if "[gpu-failure]" in l or "[d3d12-debug]" in l), None)
    print(f"----- {log.parent.name}: engine.log excerpt -----", flush=True)
    if first is not None:
        for l in lines[max(0, first - before):first + 5]:
            print("  " + l[:400])
    picked = [l for l in lines if any(f"[{t}]" in l for t in EXCERPT_TAGS) or "TEST-ONLY" in l]
    for l in picked[-tagged:]:
        print("  # " + l[:400])
    print("----- end -----", flush=True)


def make_clips(exe: Path, out: Path, size: str, seconds: int, uhd_seconds: int, wanted_kinds: set) -> dict:
    w, h = (int(v) for v in size.lower().split("x"))
    clips = {}
    for kind, rate in (("card60", "59.94"), ("card30", "30")):
        if kind not in wanted_kinds:
            continue
        clip = out / f"{kind}-{w}x{h}.y4m"
        if not clip.exists():
            r = subprocess.run([str(exe), "--make-clip", str(clip), str(w), str(h), str(seconds + 10), rate])
            if r.returncode != 0:
                print(f"FAIL clip {kind} not written", flush=True)
                continue
        clips[kind] = clip
    ffmpeg = shutil.which("ffmpeg")
    if (("uhd" in wanted_kinds and uhd_seconds > 0) or "hdr" in wanted_kinds) and not ffmpeg:
        print("NOTE 4K and HDR10 runs skipped: no ffmpeg on this machine (a lossless 4K clip is about 12 MB per frame)", flush=True)
    if ffmpeg and "uhd" in wanted_kinds and uhd_seconds > 0:
        clip = out / "uhd-3840x2160-30.mp4"
        r = subprocess.run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=3840x2160:rate=30", "-t", str(uhd_seconds + 10),
                            "-c:v", "libx264", "-preset", "ultrafast", "-crf", "20", "-pix_fmt", "yuv420p", str(clip)])
        if r.returncode == 0:
            clips["uhd"] = clip
    if ffmpeg and "hdr" in wanted_kinds:
        # 10-bit, tagged PQ / BT.2020, so the source is an HDR path (the owner's card delivers HDR10).
        clip = out / f"hdr-{w}x{h}-30.mkv"
        r = subprocess.run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", f"testsrc2=size={w}x{h}:rate=30", "-t", str(seconds + 10),
                            "-vf", "format=yuv420p10le", "-c:v", "libx265", "-preset", "ultrafast", "-x265-params", "log-level=error",
                            "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc", str(clip)])
        if r.returncode == 0:
            clips["hdr"] = clip
        else:
            print("NOTE HDR10 run skipped: ffmpeg could not write a 10-bit PQ clip", flush=True)
    return clips


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):   # the runner's console is cp1252; the app's texts are UTF-8
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe")
    ap.add_argument("--out", required=True)
    ap.add_argument("--expect", choices=("p15", "fixed"), required=True)
    ap.add_argument("--seconds", type=int, default=24)
    ap.add_argument("--static-seconds", type=int, default=12)
    # 160x90: every pass still runs (fake VFG, HUD Lock, the predicted pass with its flow at 40x23), and WARP keeps up with
    # the owner's settings (e2e-quick run 45536414453: Smoothness Mode 90 keeps 3X, I, I, B, two slots); at 320x180 WARP's
    # own work before VFG (8-12 ms) made Smoothness Mode step down to 2X.
    ap.add_argument("--size", default="160x90")
    ap.add_argument("--runs", default=",".join(list(RUNS) + list(OPTIONAL) + list(DIAG)))
    ap.add_argument("--uhd-seconds", type=int, default=0, help="4K correctness run length (needs ffmpeg; 0 = skip)")
    a = ap.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    every = dict(RUNS, **OPTIONAL, **DIAG)
    names = [n for n in a.runs.split(",") if n in every]
    if a.exe:
        exe = Path(a.exe)
        clips = make_clips(exe, out, a.size, max(a.seconds, a.static_seconds), a.uhd_seconds, {every[n][0] for n in names})
        for n in names:
            kind, args, env_extra, length = every[n]
            if kind not in clips:
                continue
            seconds = a.static_seconds if length == "static" else a.uhd_seconds if length == "uhd" else 60 if length == "soak" else a.seconds
            run_host(exe, clips[kind], out / n, seconds, args, env_extra)
    runs = {n: load_run(out / n, n) for n in names if (out / n / "engine.log").exists()}
    checks = Checks()
    checks.check(bool(runs), "at least one run folder with output", str(out))
    summary = assert_runs(runs, checks, a.expect)
    summary["checks"] = checks.items
    (out / "summary.json").write_text(json.dumps(summary, indent=2, default=str), encoding="utf-8")
    print(f"expect={a.expect}: {len([c for c in checks.items if c['ok']])} passed, {checks.failed} failed", flush=True)
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
