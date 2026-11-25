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
# The real frame's gain in a predicted pair: one output slot, 0.6-1.6. Above one slot on WARP because a predicted pair
# also waits for one generated frame less before B (WARP's generation is expensive; e2e-quick run 45565021111 measured
# 1.00-1.49 per pair); on his card the second interpolated frame costs about 2 ms, so the gain there is about one slot.
GAIN_MIN, GAIN_MAX = 0.6, 1.6
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
    warp: list = field(default_factory=list)            # [push-warp] (stream ownfg: PushWarp, the own I)
    raw_timing: list = field(default_factory=list)      # [capture-timing] messages as written (values with commas)
    capture_test: list = field(default_factory=list)
    errors: list = field(default_factory=list)          # [ERROR] lines
    dropped_log_lines: int = 0
    states: list = field(default_factory=list)          # rows of states.csv
    host_output: str = ""
    device_failures: list = field(default_factory=list)  # [gpu-failure] lines and removed-device reasons
    tail: list = field(default_factory=list)             # the last 40 lines of engine.log
    stalls_injected: list = field(default_factory=list)  # (startHost100ns, gapMs) of [e2e-stall]
    anchors: list = field(default_factory=list)          # dicts of [live-anchor]
    exit_totals: list = field(default_factory=list)      # (log seconds, {exit: session total}) from [capture-timing] lowDelayExits=
    fg_drops: list = field(default_factory=list)         # dicts of [fg-drops] (session totals of generated frames dropped)


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
                # A rebuilt graph (the row switched) numbers its batches again: the key is (epoch, revision, batch).
                key = (int(d.get("epoch", 0)), int(d.get("revision", 0)), int(d["batch"]))
                run.submits.append((key, int(d["subframe"]), int(d["pts100ns"]), int(d["host100ns"])))
            except (KeyError, ValueError):
                pass
        elif tag == "fg-sched":
            run.fg_sched.append(msg)
        elif tag == "capture-timing":
            run.timing.append(kv(msg))
            run.raw_timing.append(msg)
            ex = re.search(r"lowDelayExits=(\S+)", msg)
            if ex:
                run.exit_totals.append((seconds_of(m.group("ts")), {k: int(v) for k, v in re.findall(r"([a-z]+):(\d+)", ex.group(1))}))
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
        elif tag == "push-warp":
            run.warp.append(msg)
        elif tag == "fg-drops":
            run.fg_drops.append(kv(msg))
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
    one-predicted = (1 before, 1 after), causal = (0 before, 2 after: B, P, P; stream "ownfg" step 4).
    Generated/real from [pacing-sample] by pts; batches from [submit]."""
    generated_by_pts = {p.pts: p.generated for p in run.presents}
    if not run.submits:
        return {"batches": 0, "predicted": 0, "predictedShare": 0.0}
    start = run.submits[0][3] + int(warmup_s * 1e7)
    batches: dict[int, list] = {}
    for batch, _sub, pts, host in run.submits:
        if host < start or pts not in generated_by_pts:
            continue
        batches.setdefault(batch, []).append((host, pts, generated_by_pts[pts]))
    counts = {"interpolate": 0, "predicted": 0, "causal": 0, "other": 0, "noReal": 0, "ptsOutOfOrder": 0}
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
        elif (before, after) == (0, 2):
            counts["causal"] += 1
        else:
            counts["other"] += 1
    counts["batches"] = sum(v for k, v in counts.items() if k in ("interpolate", "predicted", "causal", "other", "noReal"))
    counts["predictedShare"] = counts["predicted"] / counts["batches"] if counts["batches"] else 0.0
    counts["causalShare"] = counts["causal"] / counts["batches"] if counts["batches"] else 0.0
    return counts


def pair_readouts(run: Run, warmup_s: float = WARMUP_S, from_s: float | None = None) -> dict:
    """The real frame's readout (capture arrival -> its Present returned) per pair pattern, in the same run: the evidence
    that a predicted pair (I, B, P) shows its real frame B one output slot earlier than an interpolated pair (I, I, B)."""
    by_pts = {p.pts: p for p in run.presents}
    if not run.submits:
        return {}
    start = run.submits[0][3] + int((warmup_s if from_s is None else max(warmup_s, from_s)) * 1e7)
    batches: dict[int, list] = {}
    for batch, _sub, pts, host in run.submits:
        if host >= start and pts in by_pts:
            batches.setdefault(batch, []).append((host, pts, by_pts[pts]))
    out: dict[str, list] = {"interpolate": [], "predicted": [], "causal": []}
    ids = sorted(batches)
    for b in ids[1:-1]:
        items = sorted(batches[b])
        reals = [i for i, it in enumerate(items) if not it[2].generated]
        if len(reals) != 1:
            continue
        r = reals[0]
        kind = {(2, 0): "interpolate", (1, 1): "predicted", (0, 2): "causal"}.get((r, len(items) - r - 1))
        p = items[r][2]
        if kind and p.arrival > 0:
            out[kind].append((p.end - p.arrival) / 1e4)
    res = {k: {"n": len(v), "meanMs": statistics.fmean(v) if v else None,
               "p50Ms": sorted(v)[len(v) // 2] if v else None} for k, v in out.items()}
    if out["interpolate"] and out["predicted"]:
        res["gainSlots"] = (res["interpolate"]["meanMs"] - res["predicted"]["meanMs"]) / SLOT_MS
    if out["predicted"] and out["causal"]:
        res["causalGainSlots"] = (res["predicted"]["meanMs"] - res["causal"]["meanMs"]) / SLOT_MS
    return res


def mean_multiplier(run: Run):
    """The mean multiplier in use after start-up (states.csv previewFgMultiplier), or None."""
    v = [number(r.get("previewFgMultiplier")) for r in last_states(run)]
    v = [x for x in v if x is not None and x >= 1]
    return statistics.fmean(v) if v else None


def state_timeline(run: Run) -> str:
    """rowState over the run, run-length encoded with the wall second each state started (states.csv)."""
    parts, last = [], None
    for r in run.states:
        st = r.get("rowState")
        if st != last:
            parts.append(f"{number(r.get('wall'), 0):.0f}s:{st}")
            last = st
    return " ".join(parts[:40])


def lowdelay_evidence(run: Run) -> str:
    """The schedule's own counters from its last [fg-sched] lowDelay line, and the session's generated frames dropped late."""
    lines = lowdelay_lines(run)
    d = kv(lines[-1]) if lines else {}
    drops = run.fg_drops[-1] if run.fg_drops else {}
    gen = sum(1 for p in run.presents if p.generated)
    skipped = sum(number(t.get("skippedPairs"), 0) for t in run.timing)
    return (f"exits={d.get('exits')} lastStop={d.get('lastStop')} entries={d.get('entries')} predictedLate={d.get('predictedLate')} "
            f"gridExits={d.get('gridExits')} stallRides={d.get('stallRides')} engineStalls={d.get('engineStalls')} "
            f"generatedDroppedDeadline={drops.get('deadline')} generatedShown={gen} skippedPairs={skipped:.0f} (admission skips, [capture-timing])")


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
    # The real predicted-frame path on WARP, end to end, with a real motion source (the float GPU DIS: comparison only; on
    # WARP it costs about 52-62 ms a pair, so the mode leaves late / dear): no timing assertions, only that the real GPU path
    # runs and fails safe (no device removal, the flow completed, the warp produced P, exits only late / dear, frame
    # generation goes on). The other scheduling scenarios run with the zero source (VEYRA_TEST_PREDICT_FLOW=zero).
    # VEYRA_TEST_PREDICT_ENTRY_GUARD=0: the entry guard (review 1 SHOULD 4) would keep the mode out on WARP's 88 ms flow.
    "predictedMechanics": ("card30", ["--low-delay", "--quality", "5"], dict(OWNER_ENV, VEYRA_TEST_PREDICT_FLOW="float", VEYRA_TEST_PREDICT_ENTRY_GUARD="0"), "main"),
    # The owner's capture stalls (a callback gap of 92-158 ms about every quarter hour, the device timestamps still stepping,
    # then a burst of late frames), injected every 6 s with a 120 ms gap. Measurement first: the baseline for riding through a
    # stall without buffering. Row off (today's schedule) and row on (the fixed branch: the mode neither leaves nor counts
    # several interruptions).
    "stallRide": ("card30", OWNER, dict(OWNER_ENV, VEYRA_TEST_REPLAY_STALL="6,120"), "main"),
    "stallRideOn": ("card30", OWNER + ["--low-delay"], dict(OWNER_ENV, VEYRA_TEST_REPLAY_STALL="6,120"), "main"),
    # 59.94 Hz twins (every picture twice: WARP keeps up there, 0-1 late P in about 235 against 3-4 % on the 30 Hz card,
    # e2e-quick run 45565021111): the scenarios that need the mode to STAY active carry their full assertions here
    # (docs/drafts/E2E_ENGINE_TEST.md, "Package 16 gate"); the 30.00 Hz runs above assert what does not depend on WARP's speed.
    "ownerOff60": ("card60", OWNER + ["--content-auto"], OWNER_ENV, "main"),
    "owner60": ("card60", OWNER + ["--content-auto", "--low-delay"], OWNER_ENV, "main"),
    "ownerVsync60": ("card60", ["--content-auto", "--hud-lock", "--smoothness", "--quality", "5", "--nv-low-latency", "--smooth-pacing",
                                "--low-delay", "--sync-auto-at", "10"], OWNER_ENV, "main"),
    "ownerToggle60": ("card60", OWNER + ["--content-auto", "--low-delay", "--toggle-at", "8", "--toggle-at", "14"], OWNER_ENV, "main"),
    "clean60": ("card60", ["--content-auto", "--low-delay", "--quality", "5"], OWNER_ENV, "main"),
    "skip2_60": ("card60", ["--content-auto", "--low-delay", "--quality", "5"], dict(OWNER_ENV, VEYRA_TEST_SKIP_PAIRS_PERMILLE="20"), "main"),
    "soak60": ("card60", ["--content-auto", "--low-delay", "--quality", "5"], OWNER_ENV, "soak"),
    "stallRide60": ("card60", OWNER + ["--content-auto"], dict(OWNER_ENV, VEYRA_TEST_REPLAY_STALL="6,120"), "main"),
    "stallRideOn60": ("card60", OWNER + ["--content-auto", "--low-delay"], dict(OWNER_ENV, VEYRA_TEST_REPLAY_STALL="6,120"), "main"),
    # The opt-in stall clock (PushStallClock.h, VEYRA_EXP_RP_STALL_CLOCK=1), measured against stallRide / stallRide60
    # (before / after; notes, not asserted: the model shows it is not never-later for every real frame).
    "stallRideClock": ("card30", OWNER, dict(OWNER_ENV, VEYRA_TEST_REPLAY_STALL="6,120", VEYRA_EXP_RP_STALL_CLOCK="1"), "main"),
    "stallRideClock60": ("card60", OWNER + ["--content-auto"], dict(OWNER_ENV, VEYRA_TEST_REPLAY_STALL="6,120", VEYRA_EXP_RP_STALL_CLOCK="1"), "main"),
    # Stream "ownfg" (pipeline/PushOwnFrame.h): PushWarp makes the predicted pair's I ONLY when the user chose it (review 1,
    # MUST 3). ownIAfterRefusalChosen: the user chose "PushFPS" and the fake VFG would refuse every FrameIndex-2-only
    # request (VEYRA_TEST_VFG_REJECT_ALONE): the mode runs with PushWarp's I and VFG is never asked (the first engine-level
    # run of PushWarp after a refusal). ownIChosen: the same choice with a VFG that would accept. On 59.94 Hz twins (WARP
    # keeps up there, as the base's *60 runs). VEYRA_TEST_OWNFRAME_COST_GUARD=0: WARP is a software rasteriser, so the
    # cost guard and the start-up budget (review 1, SHOULD 4 / 6) are lifted, as the flow's entry guard is in
    # predictedMechanics.
    "ownIAfterRefusalChosen": ("card60", ["--content-auto", "--low-delay", "--quality", "5", "--source", "pushfps"],
                               dict(OWNER_ENV, VEYRA_TEST_VFG_REJECT_ALONE="1", VEYRA_TEST_OWNFRAME_COST_GUARD="0"), "main"),
    "ownIChosen": ("card60", ["--content-auto", "--low-delay", "--quality", "5", "--source", "pushfps"], dict(OWNER_ENV, VEYRA_TEST_OWNFRAME_COST_GUARD="0"), "main"),
    # Step 4 (pipeline/PushCausal.h): the experimental fully causal schedule B, P(1/3), P(2/3), both predictions PushWarp's
    # from one field. Against clean60 (the same settings with one predicted frame): B about one more slot earlier, no wait.
    "causal": ("card60", ["--content-auto", "--low-delay", "--quality", "5", "--causal"], dict(OWNER_ENV, VEYRA_TEST_OWNFRAME_COST_GUARD="0"), "main"),
    "causalHud": ("card60", ["--content-auto", "--low-delay", "--quality", "5", "--hud-lock", "--causal"], dict(OWNER_ENV, VEYRA_TEST_OWNFRAME_COST_GUARD="0"), "main"),
    "causalStall": ("card60", ["--content-auto", "--low-delay", "--quality", "5", "--causal"],
                    dict(OWNER_ENV, VEYRA_TEST_OWNFRAME_COST_GUARD="0", VEYRA_TEST_REPLAY_STALL="6,120"), "main"),
    # Secondary: 59.94 Hz with every picture twice.
    "A": ("card60", ["--content-auto"], {}, "main"),
    "B": ("card60", ["--content-auto", "--low-delay"], {}, "main"),
    "L": ("card60", ["--content-auto"], {"VEYRA_TEST_RP_LEGACY_PACKAGE14": "1"}, "main"),
    # R: the default source (NVIDIA): a refusal leaves the mode unavailable, as before stream "ownfg"; PushWarp is not even
    # created (review 1, MUST 3 / SHOULD 9).
    "R": ("card60", ["--content-auto", "--low-delay"], {"VEYRA_TEST_VFG_REJECT_ALONE": "1"}, "main"),
    "T": ("card60", ["--content-auto", "--toggle-at", "8"], {}, "main"),
    "hud": ("card60", ["--content-auto", "--low-delay", "--hud-lock"], {}, "static"),
    "excl": ("card60", ["--content-auto", "--low-delay", "--exclusion"], {}, "static"),
    "smooth": ("card60", ["--content-auto", "--low-delay", "--smoothness"], {}, "static"),
    "m2": ("card60", ["--content-auto", "--low-delay", "--multiplier", "2"], {}, "static"),
    # m2's baseline: the same 2X with the row off (WARP shows about 0.73 generated frames per real frame at 2X on Package 15's
    # own code too: an open question, docs/drafts/E2E_ENGINE_TEST.md), so m2 is compared with it, not with a fixed bound.
    "m2off": ("card60", ["--content-auto", "--multiplier", "2"], {}, "static"),
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
                     "liveAnchorSnaps": number(run.anchors[-1].get("snaps")) if run.anchors else None,
                     "pairReadout": pair_readouts(run), "stateTimeline": state_timeline(run), "evidence": lowdelay_evidence(run),
                     "meanMultiplier": mean_multiplier(run)}
    checks.note(f"{name}: row state over time", summary[name]["stateTimeline"] or "no states.csv")
    checks.note(f"{name}: [push-predict] / [push-warp] start-up lines", " | ".join(m[:260] for m in (run.predict[:4] + run.warp[:4])) or "none")
    checks.note(f"{name}: lower-delay evidence", summary[name]["evidence"])
    pr = summary[name]["pairReadout"]
    if pr:
        checks.note(f"{name}: real frame's readout per pair pattern (same run)", json.dumps(pr))
        # The whole point of the mode: in a predicted pair the real frame B is shown one output slot earlier than in an
        # interpolated pair of the same run. Asserted where the run has both kinds (at least 20 pairs each), except in the
        # Vsync runs, whose interpolated pairs ran under another display sync (the 64 Hz runner's Vsync before 10 s).
        if pr["predicted"]["n"] >= 20 and pr["interpolate"]["n"] >= 20 and "Vsync" not in name:
            checks.check(GAIN_MIN <= pr["gainSlots"] <= GAIN_MAX, f"{name}: B one output slot earlier in predicted pairs than in interpolated pairs of the same run ({GAIN_MIN}-{GAIN_MAX} slots)",
                         f"interpolated={pr['interpolate']['meanMs']:.2f} ms (n={pr['interpolate']['n']}) predicted={pr['predicted']['meanMs']:.2f} ms "
                         f"(n={pr['predicted']['n']}) gain={pr['gainSlots']:.2f} slots of {SLOT_MS:.2f} ms")
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
    chosen = summary[name].get("meanMultiplier")
    if chosen and chosen < 2.9:   # Smoothness Mode at 2X part of the time (WARP): 90 % of (multiplier - 1)
        minimum = round(0.9 * (chosen - 1.0), 2)
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
        checks.check(GAIN_MIN <= gain <= GAIN_MAX, f"{name}: real frame's readout lower than {base}'s by about one output slot ({GAIN_MIN}-{GAIN_MAX} slots)",
                     f"{base}={summary[base]['readout']['meanMs']:.2f} ms {name}={r['meanMs']:.2f} ms gain={gain:.2f} slots of {SLOT_MS:.2f} ms")


def check_owner(name: str, run: Run, summary: dict, checks: Checks, expect: str, base: str | None = "ownerOff") -> None:
    p = summary[name]["patterns"]
    st = last_states(run)
    if expect == "fixed":
        # 30.00 Hz on WARP: Smoothness Mode may step down to 2X there; what does not depend on WARP's speed is asserted.
        checks.note(f"{name}: Smoothness Mode (target 90) 3X share", f"{share(st, 'smoothChosen', '3'):.2f} {summary[name]['smooth'][-1:]}")
        had3 = check_step_down(name, run, summary, checks)
        check_signal30(name, run, summary, checks, base, hud=True, asserted=had3)
        return
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
    if name == "stallRideOn60" and expect == "fixed" and not any(v for k, v in exit_counts(run).items() if k != "off"):
        # (with an exit, the twin rules of the gate's definition apply instead: check_stays_active)
        st = last_states(run)
        checks.check(share(st, "rowState", "active") >= 0.95, f"{name}: the mode does not leave in the stalls (active >= 95 %)",
                     f"active={share(st, 'rowState', 'active'):.2f}")
        exits = [m for m in lowdelay_lines(run) if re.search(r"exits=\S*(skipped|held|refused|late):[1-9]", m)]
        checks.check(not exits, f"{name}: no exits counted for the stalls", exits[-1][:220] if exits else "")


def check_own(name: str, run: Run, summary: dict, checks: Checks, use: str) -> None:
    """Stream "ownfg": the mode runs with PushWarp's I: active, I, B, P in order, PushWarp created after its self-check,
    and [capture-timing] labels I's source (fgSource=pushwarp ownI=<use>) on the predicted seconds."""
    check_runs_predicted(name, run, summary, checks, None)
    ready = [m for m in run.warp if m.startswith("ready ") or "PushWarp ready" in m]
    checks.check(bool(ready) and not any("UNAVAILABLE" in m for m in run.warp), f"{name}: PushWarp created after its start-up self-check matched the CPU reference",
                 (ready[:1] or run.warp[:2] or ["no [push-warp] line"])[0][:300])
    labelled = [t for t in run.timing[3:] if t.get("fgSource")]
    own = [t for t in labelled if t.get("fgSource") == "pushwarp" and t.get("ownI") == use]
    checks.check(bool(labelled) and len(own) >= 0.9 * len(labelled), f"{name}: [capture-timing] says I is PushWarp's ({use}) in >= 90 % of the seconds",
                 f"labelled={len(labelled)} pushwarp/{use}={len(own)} last={(labelled[-1] if labelled else {}).get('ownI')}")
    pairs = [number(t.get("ownIPairs"), 0) for t in labelled]
    checks.check(bool(pairs) and pairs[-1] > 0, f"{name}: PushWarp made I for predicted pairs (ownIPairs > 0)", f"ownIPairs={pairs[-1] if pairs else None}")
    notrec = [m for m in run.warp if "not recorded" in m]
    checks.check(not notrec, f"{name}: every PushWarp I was recorded", (notrec[:1] or [""])[0][:200])
    raw = [m for m in run.raw_timing if "ownIGpuMs=" in m] if hasattr(run, "raw_timing") else []
    gpu = raw[-1].split("ownIGpuMs=", 1)[1].split()[0] if raw else "none"
    checks.note(f"{name}: PushWarp's GPU time on WARP (software renderer, NOT a GPU measurement)", gpu + " (stage: P50/P95/P99 ms over 10 s)")


def check_causal(name: str, run: Run, summary: dict, checks: Checks, base: str | None, stall: bool = False) -> None:
    """Step 4: the fully causal schedule B, P, P: the order, no wait before B, the readout about one slot lower than the
    one-predicted run (base), the evidence labelled schedule=causal with P1 / P2 apart, no device removal (common)."""
    p, r = summary[name]["patterns"], summary[name]["readout"]
    ready = [m for m in run.warp if m.startswith("ready ") or "PushWarp ready" in m]
    checks.check(bool(ready), f"{name}: PushWarp created after its start-up self-check (play size included)", (ready[:1] or run.warp[:2] or ["no [push-warp] line"])[0][:300])
    share_needed = 0.6 if stall else 0.9
    checks.check(p.get("causalShare", 0) >= share_needed, f"{name}: >= {share_needed:.0%} of pairs shown B, P, P (real frame first)", json.dumps(p))
    checks.check(p.get("ptsOutOfOrder", 0) == 0, f"{name}: presents of every pair in content order", json.dumps(p))
    wait = summary[name]["delayWaitMs"]
    if not stall:
        checks.check(wait is not None and wait <= 0.35 * SLOT_MS, f"{name}: [capture-timing] wait part about zero slots (nothing generated precedes B)",
                     f"delayWaitMs={wait} slot={SLOT_MS:.2f}")
    else:
        checks.note(f"{name}: wait part (stalls injected: not asserted)", f"delayWaitMs={wait}")
    if base and base in summary and summary[base]["readout"].get("n") and r.get("n"):
        gain = (summary[base]["readout"]["meanMs"] - r["meanMs"]) / SLOT_MS
        summary[name]["gainSlotsVsOnePredicted"] = gain
        if stall:
            checks.note(f"{name}: readout against {base} (stalls injected: not asserted)", f"gain={gain:.2f} slots")
        else:
            checks.check(GAIN_MIN <= gain <= GAIN_MAX, f"{name}: real frame's readout lower than {base}'s (one predicted frame) by about one output slot ({GAIN_MIN}-{GAIN_MAX} slots)",
                         f"{base}={summary[base]['readout']['meanMs']:.2f} ms {name}={r['meanMs']:.2f} ms gain={gain:.2f} slots of {SLOT_MS:.2f} ms")
    raw = [m for m in run.raw_timing if " schedule=" in m] if hasattr(run, "raw_timing") else []
    labelled = [m for m in raw if " schedule=causal " in m]
    checks.check(bool(raw) and len(labelled) >= 0.8 * len(raw), f"{name}: [capture-timing] labelled schedule=causal (>= 80 % of the labelled seconds)",
                 f"labelled={len(raw)} causal={len(labelled)}")
    last = (labelled or raw or [""])[-1]
    fields = {k: last.split(k + "=", 1)[1].split()[0] for k in ("causalP1Unusable", "causalP2Unusable", "causalP1GpuMs", "causalP2GpuMs", "causalLevel",
                                                                "causalExitsUnusable", "causalExitsLate", "causalExitsDear") if k + "=" in last}
    checks.check(all(k in fields for k in ("causalP1Unusable", "causalP2Unusable", "causalP1GpuMs", "causalP2GpuMs")),
                 f"{name}: the evidence gives P1's and P2's unusable shares and GPU times apart", json.dumps(fields))
    checks.note(f"{name}: P1 / P2 GPU time on WARP (software renderer, NOT a GPU measurement)", json.dumps(fields))
    if not stall:
        checks.check(fields.get("causalLevel") == "causal" and fields.get("causalExitsUnusable") == "0",
                     f"{name}: the causal level held (no unusable exit on zero-motion pictures)", json.dumps(fields))
    else:
        checks.note(f"{name}: exits with stalls", json.dumps(fields))
    left = [m for m in run.fg_sched if "causal schedule" in m]
    checks.note(f"{name}: causal ladder changes", "; ".join(x[:160] for x in left[:3]) or "none")


def check_mechanics(name: str, run: Run, summary: dict, checks: Checks) -> None:
    """The real predicted-frame path on WARP with the float GPU DIS: runs and fails safe; no timing is asserted."""
    samples = 0
    for t in run.timing:
        m = re.match(r"samples:(\d+)", str(t.get("predictGpuMs", "")))
        if m:
            samples = max(samples, int(m.group(1)))
    checks.check(samples > 0, f"{name}: the flow completed for predicted pairs (predictGpuMs samples > 0)", f"samples={samples}")
    flow = [m for m in run.predict if m.startswith("flow source=")]
    checks.check(any("source=float" in m for m in flow), f"{name}: P's motion came from the float GPU DIS (comparison source)", (flow[-1] if flow else "no flow line")[:200])
    predicted = sum(number(t.get("predictedPairs"), 0) for t in run.timing)
    whole = sum(number(t.get("lowDelayWholeHeld"), 0) for t in run.timing)
    # Not asserted on WARP (decision of 25 Nov 2026): the float flow costs 40-48 ms there, so P is late or held as a whole;
    # the warp's correctness is covered by the WARP predict* tests and the end-to-end self-check test.
    checks.note(f"{name}: P produced by the warp (not asserted on WARP)", f"predictedPairs={predicted:.0f} wholeHeld={whole:.0f}")
    last = lowdelay_lines(run)[-1:] or [""]
    other = re.findall(r"(off|ineligible|refused|unusable|skipped|held):([1-9]\d*)", last[0].split("exits=")[-1].split(" ")[0])
    checks.check(not other, f"{name}: the mode left only late or dear (expected on WARP)", last[0][:260])
    p = summary[name]["patterns"]
    checks.check(p.get("interpolate", 0) > 0 and p.get("ptsOutOfOrder", 0) == 0, f"{name}: frame generation goes on as I, I, B after the exits, in content order", json.dumps(p))


def exit_counts(run: Run, warmup_s: float = WARMUP_S) -> dict:
    """The schedule's exits by reason AFTER start-up: the per-second session totals of [capture-timing] lowDelayExits=
    (the [fg-sched] lowDelay line is written at most once a second on a change and every 10 s, so an exit near the end
    of a run may not be in it), the totals at the end minus those at warmup_s after the first window. Without those
    lines: the last [fg-sched] lowDelay line."""
    if run.exit_totals and run.exit_totals[0][0] is not None:
        t0 = run.exit_totals[0][0]
        before = next((d for t, d in reversed(run.exit_totals) if t is not None and t <= t0 + warmup_s - 1.0), {})
        end = run.exit_totals[-1][1]
        return {k: v - before.get(k, 0) for k, v in end.items()}
    lines = lowdelay_lines(run)
    if not lines:
        return {}
    text = lines[-1].split("exits=")[-1].split(" ")[0]
    return {k: int(v) for k, v in re.findall(r"([a-z]+):(\d+)", text)}


TWIN_RUNS = ("owner60", "ownerVsync60", "ownerToggle60", "clean60", "skip2_60", "stallRideOn60", "B", "hud", "smooth", "excl")
TWIN_EXIT_RUNS_MAX = 2       # the gate's definition (docs/drafts/E2E_ENGINE_TEST.md): at most two twin runs with an exit
TWIN_LATE_SHARE_MAX = 0.05   # predictedLate in a twin run with an exit


def late_share(run: Run):
    lines = lowdelay_lines(run)
    m = re.match(r"(\d+)/(\d+)", kv(lines[-1]).get("predictedLate", "") if lines else "")
    return (int(m.group(1)) / int(m.group(2))) if m and int(m.group(2)) > 0 else None


def wait_while_active(run: Run):
    """Median [capture-timing] delayWaitMs over the windows the mode ran throughout (lowDelay=active, >= 20 predicted pairs,
    no switch), and how many such windows there were."""
    w = [number(t.get("delayWaitMs")) for t in run.timing
         if t.get("lowDelay") == "active" and number(t.get("predictedPairs"), 0) >= 20 and number(t.get("lowDelaySwitches"), 1) == 0]
    w = [x for x in w if x is not None and x >= 0]
    return (statistics.median(w) if w else None), len(w)


def check_structure(name: str, run: Run, summary: dict, checks: Checks, base: str | None = None, hud: bool = False,
                    from_s: float | None = None, say=None) -> None:
    """STRUCTURAL assertions, strict in every run (the gate's definition): the mode enters; predicted pairs run; order
    I, B, P; per pair the real frame 0.6-1.6 slots earlier; the wait part about one slot while active; frame generation
    continues as I, I, B after an exit; HUD mismatch 0; predictedLate=, gridExits= and skippedPairs logged.
    ([gpu-failure] and the row's states are asserted in check_common / check_step_down.)"""
    say = say or checks.check
    st = last_states(run)
    lines = lowdelay_lines(run)
    last = kv(lines[-1]) if lines else {}
    p = summary[name]["patterns"]
    say(number(last.get("entries"), 0) >= 1 or any(r.get("rowState") == "active" for r in st), f"{name}: the mode enters with these settings",
        lines[-1][:200] if lines else "no lowDelay line")
    say(p.get("predicted", 0) > 0, f"{name}: predicted pairs run (I, B, P presented)", json.dumps(p))
    say(p.get("ptsOutOfOrder", 0) == 0, f"{name}: presents of every pair in content order", json.dumps(p))
    pr = pair_readouts(run, from_s=from_s) if from_s is not None else (summary[name].get("pairReadout") or {})
    n_p, n_i = (pr.get("predicted") or {}).get("n", 0), (pr.get("interpolate") or {}).get("n", 0)
    if n_p >= 10 and n_i >= 10:
        g, how = pr["gainSlots"], "same run"
    elif n_p >= 10 and base and summary.get(base, {}).get("readout", {}).get("n"):
        g, how = (summary[base]["readout"]["meanMs"] - pr["predicted"]["meanMs"]) / SLOT_MS, f"against {base}"
    else:
        g, how = None, f"too few pairs (predicted {n_p}, interpolated {n_i})"
    say(g is not None and GAIN_MIN <= g <= GAIN_MAX, f"{name}: per pair, the real frame one output slot earlier in predicted pairs ({GAIN_MIN}-{GAIN_MAX} slots)",
        f"gain={g if g is None else round(g, 2)} ({how}) {json.dumps(pr)}")
    wait, windows = wait_while_active(run)
    say(wait is not None and abs(wait - SLOT_MS) <= 0.35 * SLOT_MS, f"{name}: wait part about one slot while active",
        f"delayWaitMs={wait} over {windows} whole active windows, slot={SLOT_MS:.2f}")
    ex = exit_counts(run)
    if sum(v for k, v in ex.items() if k != "off"):
        say(p.get("interpolate", 0) > 0, f"{name}: frame generation continues as I, I, B after the exits", json.dumps(p))
        # Generated frames per real frame: 90 % of (multiplier - 1) on the pairs admission did not skip (a skipped pair
        # makes no generated frame by design; WARP's skips are timing), after from_s in the Vsync runs (the runner's 64 Hz
        # Vsync before it caps the presents).
        r = readout(run, from_s if from_s is not None else WARMUP_S)
        mult = summary[name].get("meanMultiplier") or 3.0
        skipped = sum(number(t.get("skippedPairs"), 0) for t in run.timing)
        skip_share = min(1.0, skipped / r["n"]) if r.get("n") else 0.0
        need = round(0.9 * (mult - 1.0) * (1.0 - skip_share), 2)
        say(r.get("generatedPerReal", 0) >= need, f"{name}: the pipeline keeps up after the exits (>= {need} generated frames per real frame: "
            f"90 % of {mult:.2f} - 1 on the {1 - skip_share:.0%} of pairs not skipped)", json.dumps(r))
    say(bool(lines) and "predictedLate" in last and "gridExits" in last and any("skippedPairs" in t for t in run.timing),
        f"{name}: predictedLate=, gridExits= and skippedPairs are logged", (lines[-1][-200:] if lines else "no lowDelay line"))
    if hud:
        h = [kv(x) for x in run.predict if "hudLockMismatch" in x]
        say(bool(h) and all(number(x.get("hudLockMismatch"), 1) == 0 for x in h),
            f"{name}: HUD-locked pixels of P identical to B's ([push-predict] hudLockMismatch=0)", str(h[-1]) if h else "no [push-predict] hudLockMismatch= line")


def exit_text(name: str, run: Run) -> str:
    lines = lowdelay_lines(run)
    d = kv(lines[-1]) if lines else {}
    skipped = sum(number(t.get("skippedPairs"), 0) for t in run.timing)
    ex = ",".join(f"{k}:{v}" for k, v in exit_counts(run).items() if v)
    return f"exits after start-up=[{ex or 'none'}] gridExits={d.get('gridExits')} predictedLate={d.get('predictedLate')} skippedPairs={skipped:.0f}"


def check_stays_active(name: str, run: Run, summary: dict, checks: Checks, base: str | None, hud: bool = False,
                       from_s: float | None = None, full=None, had3: bool = True) -> bool:
    """A 59.94 Hz twin. Structure strict; exits only `late` with predictedLate <= 5 %; a twin WITHOUT an exit keeps the
    full timing assertions (active >= 90 %, >= 90 % of pairs I, B, P, readout gain against the row-off twin); soak60 must
    have no exit at all. Returns whether the run had an exit (the round allows at most TWIN_EXIT_RUNS_MAX)."""
    if not had3:   # Smoothness Mode kept 2X on WARP for nearly the whole run: the row's state is asserted (check_step_down)
        say = lambda ok, n, d="": checks.note(n + (" [ok]" if ok else " [NOT MET; not asserted: 2X on WARP]"), d)
        check_structure(name, run, summary, checks, base, hud, from_s, say)
        summary[name]["twinExit"] = False
        return False
    check_structure(name, run, summary, checks, base, hud, from_s)
    ex = {k: v for k, v in exit_counts(run).items() if k != "off" and v}
    had_exit = bool(ex)
    if name == "soak60":
        checks.check(bool(lowdelay_lines(run)) and not had_exit, f"{name}: no exits in 60 s", exit_text(name, run))
    else:
        checks.check(set(ex) <= {"late"}, f"{name}: exits only of kind late", exit_text(name, run))
        if had_exit:
            share_late = late_share(run)
            checks.check(share_late is not None and share_late <= TWIN_LATE_SHARE_MAX, f"{name}: predictedLate at most {TWIN_LATE_SHARE_MAX:.0%} (a run with an exit)",
                         exit_text(name, run))
            checks.note(f"{name}: an exit on WARP (timing; the round allows {TWIN_EXIT_RUNS_MAX} twin runs with one)", exit_text(name, run))
    if not had_exit and full is not None:
        full()   # the run's own timing assertions (ownerVsync60: active after smooth pacing is off; ownerToggle60: re-entry)
    elif not had_exit:
        p, st = summary[name]["patterns"], last_states(run)
        checks.check(share(st, "rowState", "active") >= 0.9, f"{name}: the row reports active (>= 90 % of samples after start-up)",
                     f"active={share(st, 'rowState', 'active'):.2f} final={summary[name]['finalState']}")
        checks.check(p.get("predictedShare", 0) >= 0.9, f"{name}: >= 90 % of pairs shown I, B, P", json.dumps(p))
        keeps_up(name, summary, checks)
        r = summary[name]["readout"]
        if base and base in summary and summary[base]["readout"].get("n") and r.get("n"):
            gain = (summary[base]["readout"]["meanMs"] - r["meanMs"]) / SLOT_MS
            summary[name]["gainSlots"] = gain
            checks.check(GAIN_MIN <= gain <= GAIN_MAX, f"{name}: real frame's readout lower than {base}'s by about one output slot ({GAIN_MIN}-{GAIN_MAX} slots)",
                         f"{base}={summary[base]['readout']['meanMs']:.2f} ms {name}={r['meanMs']:.2f} ms gain={gain:.2f} slots")
    summary[name]["twinExit"] = had_exit
    return had_exit


def check_hud_mismatch(name: str, run: Run, checks: Checks) -> None:
    hud = [kv(m) for m in run.predict if "hudLockMismatch" in m]
    checks.check(bool(hud) and all(number(h.get("hudLockMismatch"), 1) == 0 for h in hud),
                 f"{name}: HUD-locked pixels of P identical to B's ([push-predict] hudLockMismatch=0)",
                 str(hud[-1]) if hud else "no [push-predict] hudLockMismatch= line")


def check_signal30(name: str, run: Run, summary: dict, checks: Checks, base: str | None = None, hud: bool = False,
                   asserted: bool = True, from_s: float | None = None) -> None:
    """A 30.00 Hz run (the owner's signal). Structure strict. Exits of kind late, skipped and grid are NOTES on WARP; any
    other kind fails. asserted=False (Smoothness Mode kept 2X on WARP for nearly the whole run, so the mode cannot enter;
    check_step_down asserts the row's state instead) turns the structural checks into notes."""
    say = None if asserted else (lambda ok, n, d="": checks.note(n + (" [ok]" if ok else " [NOT MET; not asserted: 2X on WARP]"), d))
    st = last_states(run)
    p = summary[name]["patterns"]
    checks.note(f"{name}: active share (timing; not asserted at 30.00 Hz on WARP)", f"active={share(st, 'rowState', 'active'):.2f} predictedShare={p.get('predictedShare', 0):.2f}")
    check_structure(name, run, summary, checks, base, hud, from_s, say)
    ex = {k: v for k, v in exit_counts(run).items() if k != "off" and v}
    other = {k: v for k, v in ex.items() if k not in ("late", "skipped")}
    checks.check(not other, f"{name}: no exit of a kind other than late, skipped or grid", exit_text(name, run))
    if ex:
        checks.note(f"{name}: exits on WARP (timing: late / skipped / grid are notes at 30.00 Hz)",
                    exit_text(name, run) + f" lateShare={late_share(run)}")


def check_step_down(name: str, run: Run, summary: dict, checks: Checks, from_s: float = 0.0) -> bool:
    """Where Smoothness Mode steps down to 2X on WARP: the row names it (not-here / not-3x, resumes by itself), and when 3X
    is back for 2 s it is no longer not-3x. Samples within 1.5 s of a change of the chosen multiplier are left out.
    Returns whether the run had 3 s or more at 3X after from_s."""
    st = [r for r in last_states(run) if number(r.get("wall"), 0) >= from_s]
    changes = [number(st[i].get("wall"), 0) for i in range(1, len(st)) if st[i].get("smoothChosen") != st[i - 1].get("smoothChosen")]
    changes += [w for _, w, _ in run.toggles]   # a switch of the row rebuilds the graph: its state settles after it
    settled = [r for r in st if all(abs(number(r.get("wall"), 0) - c) > 1.5 for c in changes)]
    at2 = [r for r in settled if r.get("smoothChosen") == "2"]
    at3 = [r for r in settled if r.get("smoothChosen") == "3"]
    checks.note(f"{name}: Smoothness Mode's choice on WARP", f"2X samples={len(at2)} 3X samples={len(at3)} changes at {changes[:6]} {summary[name]['smooth'][-1:]}")
    checks.check(all(r.get("rowState") == "not-here" and r.get("rowRefusal") == "not-3x" for r in at2),
                 f"{name}: at 2X the row says not-here / not-3x (it resumes by itself)",
                 str(next((r for r in at2 if not (r.get("rowState") == "not-here" and r.get("rowRefusal") == "not-3x")), "ok")))
    checks.check(all(r.get("rowRefusal") != "not-3x" for r in at3), f"{name}: at 3X the row is no longer paused for not-3x",
                 str(next((r for r in at3 if r.get("rowRefusal") == "not-3x"), "ok")))
    return len({int(number(r.get("wall"), 0)) for r in at3}) >= 3   # seconds with settled 3X samples


def check_owner_state_only(name: str, run: Run, summary: dict, checks: Checks, expect: str) -> None:
    st = last_states(run)
    # Under Vsync the runner's 64 Hz display caps the presents, so Smoothness Mode may choose 2X there (run 45538445632);
    # his 3X is asserted after smooth pacing is switched off at 10 s.
    after_off = [r for r in st if number(r.get("wall"), 0) >= 10 + WARMUP_S]
    checks.check(share(after_off, "smoothChosen", "3") >= 0.9, f"{name}: Smoothness Mode (target 90) chooses 3X after smooth pacing is off",
                 f"3X share={share(after_off, 'smoothChosen', '3'):.2f} {summary[name]['smooth'][-1:]}")
    if expect == "p15":
        checks.check(share(st, "rowRefusal", P15_OWNER_REFUSAL) >= 0.9 and share(st, "rowState", "not-here") >= 0.9,
                     f"{name}: REPRODUCES not-here / {P15_OWNER_REFUSAL} with smooth pacing on and after it is switched off (expected until fixed)",
                     str(summary[name]["finalState"]))
    else:
        after = [r for r in st if number(r.get("wall"), 0) >= 10 + WARMUP_S]
        checks.check(share(after, "rowState", "active") >= 0.9, f"{name}: active after smooth pacing is switched off (fixed branch)",
                     f"active={share(after, 'rowState', 'active'):.2f} final={summary[name]['finalState']}")
    checks.note(f"{name}: pacing and delay not asserted", "the runner's display is 64 Hz: Vsync limits presents there")


def check_toggles(name: str, run: Run, summary: dict, checks: Checks, expect: str, owner: bool, within_pairs: int = 90,
                  timing: bool = True) -> None:
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
            elif timing:
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
        if expect == "fixed":
            had3 = check_step_down("ownerVsync", runs["ownerVsync"], summary, checks, from_s=10 + WARMUP_S)
            check_signal30("ownerVsync", runs["ownerVsync"], summary, checks, "ownerOff" if "ownerOff" in runs else None, hud=True, asserted=had3, from_s=10 + WARMUP_S)
        else:
            check_owner_state_only("ownerVsync", runs["ownerVsync"], summary, checks, expect)
    if "ownerHdr" in runs:
        checks.check(any("format=24" in m and "TEST-ONLY fake VFG" in m for m in runs["ownerHdr"].vfg),
                     "ownerHdr: frame generation runs in the HDR10 format (R10G10B10A2 = 24)", "; ".join(m[:160] for m in runs["ownerHdr"].vfg[:2]))
    if "ownerToggle" in runs:
        check_toggles("ownerToggle", runs["ownerToggle"], summary, checks, expect, owner=True, timing=expect != "fixed")
        if expect == "fixed":
            had3 = check_step_down("ownerToggle", runs["ownerToggle"], summary, checks)
            check_signal30("ownerToggle", runs["ownerToggle"], summary, checks, "ownerOff" if "ownerOff" in runs else None, hud=True, asserted=had3)
    for name in ("stallRide", "stallRideOn", "stallRide60", "stallRideOn60", "stallRideClock", "stallRideClock60"):
        if name in runs:
            check_stall(name, runs[name], summary, checks, expect)
    for before, after in (("stallRide", "stallRideClock"), ("stallRide60", "stallRideClock60")):
        if before in summary and after in summary:
            def agg(n):
                st = summary[n].get("stalls") or []
                back = [x["pacingBackMs"] for x in st if x.get("pacingBackMs") is not None]
                return {"stalls": len(st), "lateReal": sum(x["lateReal"] for x in st), "worstMs": max([x["worstMs"] or 0 for x in st] or [0]),
                        "pacingBackMedianMs": statistics.median(back) if back else None, "anchorSnaps": summary[n].get("anchorSnaps"),
                        "readoutMeanMs": summary[n]["readout"].get("meanMs")}
            checks.note(f"stall clock before / after ({before} -> {after}; WARP, not asserted)", json.dumps({"before": agg(before), "after": agg(after)}))
    if expect == "fixed":
        if "stallRideOn" in runs:
            check_signal30("stallRideOn", runs["stallRideOn"], summary, checks, "stallRide", hud=True)
        for name in ("clean30", "predictedSoak", "skip2"):
            if name in runs:
                check_signal30(name, runs[name], summary, checks, None)
        # The 59.94 Hz twins: the full assertions.
        if "ownerOff60" in runs:
            p = summary["ownerOff60"]["patterns"]
            checks.check(p.get("batches", 0) >= 60 and p.get("predicted", 0) == 0, "ownerOff60: pairs presented, none predicted", json.dumps(p))
            keeps_up("ownerOff60", summary, checks)
        off60 = "ownerOff60" if "ownerOff60" in runs else None
        if "owner60" in runs:
            had3 = check_step_down("owner60", runs["owner60"], summary, checks)
            check_stays_active("owner60", runs["owner60"], summary, checks, off60, hud=True, had3=had3)
        if "ownerVsync60" in runs:
            r = runs["ownerVsync60"]
            had3 = check_step_down("ownerVsync60", r, summary, checks, from_s=10 + WARMUP_S)
            check_stays_active("ownerVsync60", r, summary, checks, off60, hud=True, from_s=10 + WARMUP_S, had3=had3,
                               full=lambda: check_owner_state_only("ownerVsync60", r, summary, checks, expect))
        if "ownerToggle60" in runs:
            r = runs["ownerToggle60"]
            check_toggles("ownerToggle60", r, summary, checks, expect, owner=True, timing=False)
            had3 = check_step_down("ownerToggle60", r, summary, checks)
            check_stays_active("ownerToggle60", r, summary, checks, off60, hud=True, had3=had3,
                               full=lambda: check_toggles("ownerToggle60", r, summary, checks, expect, owner=True))
        for name in ("clean60", "soak60", "skip2_60"):
            if name in runs:
                check_stays_active(name, runs[name], summary, checks, "A" if "A" in runs else None)
        if "stallRideOn60" in runs:
            had3 = check_step_down("stallRideOn60", runs["stallRideOn60"], summary, checks)
            check_stays_active("stallRideOn60", runs["stallRideOn60"], summary, checks, "stallRide60" if "stallRide60" in runs else None, hud=True, had3=had3)
    if "clean30" in runs and expect != "fixed":
        check_runs_predicted("clean30", runs["clean30"], summary, checks, None)
    if "predictedSoak" in runs:
        if expect != "fixed":
            check_runs_predicted("predictedSoak", runs["predictedSoak"], summary, checks, None)
        exits = [m for m in lowdelay_lines(runs["predictedSoak"]) if "state=active" not in m]
        checks.note("predictedSoak: lowDelay lines that were not active", "; ".join(m[:160] for m in exits[-3:]) or "none")
    if "predictedMechanics" in runs:
        check_mechanics("predictedMechanics", runs["predictedMechanics"], summary, checks)
    if "ownIAfterRefusalChosen" in runs:
        run = runs["ownIAfterRefusalChosen"]
        check_own("ownIAfterRefusalChosen", run, summary, checks, "chosen")
        checks.check(not any("injected rejection" in m for m in run.vfg), "ownIAfterRefusalChosen: no FrameIndex-2-only request reached VFG (PushFPS chosen)")
        st = last_states(run)
        checks.check(share(st, "rowState", "unavailable") == 0, "ownIAfterRefusalChosen: the row never says unavailable",
                     f"unavailable={share(st, 'rowState', 'unavailable'):.2f} final={summary['ownIAfterRefusalChosen']['finalState']}")
    for name in ("causal", "causalHud", "causalStall"):
        if name in runs:
            check_causal(name, runs[name], summary, checks, "clean60" if name != "causalHud" else None, stall=name == "causalStall")
    if "ownIChosen" in runs:
        run = runs["ownIChosen"]
        check_own("ownIChosen", run, summary, checks, "chosen")
        checks.check(not any("injected rejection" in m for m in run.vfg), "ownIChosen: no FrameIndex-2-only request reached VFG")
    if "skip2" in runs:
        p = summary["skip2"]["patterns"]
        skipped = sum(number(t.get("skippedPairs"), 0) for t in runs["skip2"].timing)
        checks.note("skip2: injected skips (VEYRA_TEST_SKIP_PAIRS_PERMILLE=20)", f"skippedPairs={skipped:.0f} predictedShare={p.get('predictedShare', 0):.3f}")
        checks.check(skipped > 0, "skip2: skipped pairs were injected", f"skippedPairs={skipped:.0f}")
        if expect == "p15":
            checks.check(p.get("predictedShare", 1) <= 0.5,
                         "skip2: with 2 % skipped pairs Package 15's rule loses the mode (expected until fixed; scratch model 1 %)", json.dumps(p))
        # fixed: check_signal30 above; the 59.94 Hz twin skip2_60 asserts >= 90 % predicted.
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
        if expect == "fixed":
            check_stays_active("B", runs["B"], summary, checks, "A" if a else None)
        else:
            check_runs_predicted("B", runs["B"], summary, checks, "A" if a else None)
    rr = runs.get("R")
    if rr:
        p, r = summary["R"]["patterns"], summary["R"]["readout"]
        st = rr.states[-1] if rr.states else {}
        checks.check(any("injected rejection of a FrameIndex-2-only request" in m for m in rr.vfg), "R: the fake refused index-2-alone (log)")
        checks.check(st.get("rowRefusal") == "vfg-refused" and st.get("rowState") == "unavailable", "R: the row says unavailable / vfg-refused", str(st))
        checks.check(not any(m.startswith("ready ") or "PushWarp ready" in m for m in rr.warp) and not any("fgSource=pushwarp" in m for m in rr.raw_timing),
                     "R: with the default source PushWarp is neither created nor used (review 1, MUST 3 / SHOULD 9)", (rr.warp[:1] or ["no [push-warp] line"])[0][:200])
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
        # Package 16 (item 2): HUD Lock and exclusion boxes run predicted; Smoothness Mode only decides the multiplier.
        fixed_runs = expect == "fixed" and name in ("hud", "smooth", "excl")
        checks.check(p.get("predicted", 0) == 0 or expected == "adaptive" or fixed_runs, f"{name}: no pair ran predicted", json.dumps(p))
        if name == "m2" and "m2off" in summary:
            g, g0 = summary["m2"]["readout"].get("generatedPerReal", 0), summary["m2off"]["readout"].get("generatedPerReal", 0)
            checks.check(g0 > 0 and abs(g - g0) <= 0.1 * g0, "m2: frame generation continues as with the row off at 2X (within 10 %)",
                         f"m2={g:.3f} m2off={g0:.3f}")
            checks.note("m2: generated frames per real frame at 2X on WARP (open question: about 0.73 on Package 15 too)", f"m2={g:.3f} m2off={g0:.3f}")
        else:
            checks.check(summary[name]["readout"].get("generatedPerReal", 0) >= 0.8, f"{name}: frame generation continues", json.dumps(summary[name]["readout"]))
        if expected == "adaptive":
            mult = st.get("previewFgMultiplier")
            ok = (st.get("rowRefusal") == "not-3x" and mult != "3") or (mult == "3" and st.get("rowState") in ("active", "calm"))
            checks.check(ok, "adaptive: the row names not-3x when the pair runs below 3X (else it may run)", str(st))
        elif fixed_runs:
            check_stays_active(name, s, summary, checks, "A" if a else None, hud=name == "hud")
            if name == "excl":
                ex = [kv(m) for m in s.predict if "exclusionMismatch" in m]
                checks.check(bool(ex) and all(number(e.get("exclusionMismatch"), 1) == 0 for e in ex) and any(number(e.get("excluded"), 0) > 0 for e in ex),
                             "excl: excluded pixels of P identical to B's ([push-predict] exclusionMismatch=0, excluded > 0)",
                             str(ex[-1]) if ex else "no [push-predict] exclusionMismatch= line")
        else:
            checks.check(st.get("rowState") == "not-here" and st.get("rowRefusal") == expected, f"{name}: the row says not-here / {expected}", str(st))
    if expect == "fixed":
        # The gate's definition: at most TWIN_EXIT_RUNS_MAX twin runs of the round may contain an exit (WARP's timing).
        with_exit = [n for n in TWIN_RUNS if summary.get(n, {}).get("twinExit")]
        present = [n for n in TWIN_RUNS if n in summary and "twinExit" in summary[n]]
        checks.check(len(with_exit) <= TWIN_EXIT_RUNS_MAX, f"59.94 Hz twins: at most {TWIN_EXIT_RUNS_MAX} runs of the round with an exit",
                     f"with an exit: {with_exit or 'none'} of {len(present)} twin runs")
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
    ap.add_argument("--runs", default=",".join(list(RUNS) + list(OPTIONAL)), help="the DIAG runs are on request (name them here)")
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
