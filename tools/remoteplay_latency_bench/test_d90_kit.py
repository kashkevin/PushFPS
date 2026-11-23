#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The delay of a 30 FPS game at 90 FPS at 4K (the kit's d90 and d90-capture sets; docs/drafts/DELAY_90.md).

Checks that every arm is the comparison it claims to be: the reference without frame generation, 3x at 4K as installed,
each delay90 switch alone against 3x, all of them together, the backends at 3x ("choose by eye"), and the DIAGNOSTIC
references (3x at the screen's size, 2x), which must say so. Every arm differs from its pair in exactly one thing, the
switch names are the ones the engine reads, the sets run inside the smoothness and capture sessions, the lines the
comparison needs pass the kit's allow-list, and the session length is what the kit's own arithmetic says. Standard
library only.
"""
import csv
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
KIT = os.path.join(ROOT, "scripts", "remoteplay", "testkit")
sys.path.insert(0, HERE)
import report  # noqa: E402

ULL = "VEYRA_EXP_RP_FG_ULL=hold,phase,grace"
GRAPH = "VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY=high"
PRESENT = "VEYRA_TEST_PRESENT_QUEUE_PRIORITY=high"
EARLY = "VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE=1"
FIT = "VEYRA_EXP_RP_FIT_DISPLAY=1"
SETS = {
    "d90": ["d90-off", "d90-fg3", "d90-fg3-ull", "d90-fg3-prio", "d90-fg3-pprio", "d90-fg3-all", "d90-fg3-low", "d90-fg3-dlss",
            "d90-fg3-flow", "d90-fg3-fit", "d90-fg2"],
    "d90-capture": ["d90c-off", "d90c-fg3", "d90c-fg3-ull", "d90c-fg3-early", "d90c-fg3-prio", "d90c-fg3-pprio", "d90c-fg3-all",
                    "d90c-fg3-low", "d90c-fg3-dlss", "d90c-fg3-flow", "d90c-fg2"],
}
# What each arm adds to its pair: (pipeline keys added, pipeline keys removed, other switches added, differs)
ADDS = {
    "fg3": (["fg=vfg", "fgMultiplier=3", "fgTier=1"], ["fg=off"], set(), ""),
    "fg3-ull": ([], [], {ULL}, ""),
    "fg3-early": ([], [], {EARLY}, ""),
    "fg3-prio": ([], [], {GRAPH}, ""),
    "fg3-pprio": ([], [], {PRESENT}, ""),
    "fg3-all": ([], [], None, ""),
    "fg3-low": (["fgTier=0"], ["fgTier=1"], set(), "fgTier"),
    "fg3-dlss": (["fg=dlss"], ["fg=vfg", "fgTier=1"], set(), "fgMethod;fgTier"),
    "fg3-flow": (["fg=pushflow"], ["fg=vfg"], set(), "fgMethod"),
    "fg3-fit": ([], [], {FIT}, "srTarget"),
    "fg2": (["fgMultiplier=2"], ["fgMultiplier=3"], set(), "fgMultiplier"),
}


def parts(table, name):
    pipeline, other = [], set()
    for item in filter(None, table[name]["switches"].split(";")):
        if item.startswith("pipeline:"):
            pipeline = item[len("pipeline:"):].split("|")
        else:
            other.add(item)
    return pipeline, other


def kind(name):
    return name.split("-", 1)[1] if "-" in name else ""


class D90Kit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table = report.read_arm_table(os.path.join(KIT, "arms.csv"))
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="utf-8") as f:
            cls.kit = f.read()
        with open(os.path.join(KIT, "arms.csv"), encoding="ascii") as f:
            cls.csv = f.read()

    def test_arms_exist_with_their_pairs(self):
        for set_name, arms in SETS.items():
            mine = [n for n, r in self.table.items() if r["set"] == set_name]
            self.assertEqual(mine, arms, set_name)
            off, three = arms[0], arms[1]
            for name in arms:
                row = self.table[name]
                want = name if name == off else off if name == three else three
                self.assertEqual(row["compare"], want, name)
                self.assertEqual(row["exe"], "experimental", name)
                pipeline, other = parts(self.table, name)
                self.assertEqual(pipeline[:2], ["profile=original", "hdr=0"], name)
                self.assertIn("VEYRA_EXP_PRESENT_WINDOW_FACTS=1", other, name)
                self.assertEqual(row["fg"] == "on", any(p.startswith("fg=") and p != "fg=off" for p in pipeline), name)
                if set_name == "d90":
                    # Remote Play: DLSS upscaling to 4K, and the stream's duplicate removal (30 FPS pairs 33.4 ms apart)
                    self.assertTrue({"sr=dlss", "srTarget=4k", "dedup=auto"} <= set(pipeline), name)
                    self.assertIn("VEYRA_EXP_RP_STREAM_DEDUP=1", other, name)
                else:
                    self.assertTrue({"dedup=auto", "hudLock=0"} <= set(pipeline), name)
                    self.assertFalse(any(p.startswith("sr=") for p in pipeline), name)   # the 4K capture itself

    def test_each_arm_differs_from_its_pair_in_one_thing(self):
        for arms in SETS.values():
            for name in arms[1:]:
                row = self.table[name]
                mine, mine_other = parts(self.table, name)
                theirs, theirs_other = parts(self.table, row["compare"])
                added = sorted(p for p in mine if p not in theirs)
                removed = sorted(p for p in theirs if p not in mine)
                self.assertFalse(theirs_other - mine_other, name)
                want_added, want_removed, want_extra, differs = ADDS[kind(name)]
                if want_extra is None:   # every switch of the set that applies to it, together
                    singles = {s for n in arms for s in parts(self.table, n)[1] - theirs_other if kind(n) in ("fg3-ull", "fg3-early", "fg3-prio", "fg3-pprio")}
                    want_extra = singles
                self.assertEqual((added, removed, mine_other - theirs_other), (sorted(want_added), sorted(want_removed), want_extra), name)
                self.assertEqual(row["differs"], differs, name)

    def test_lower_size_and_lower_rate_arms_say_diagnostic(self):
        # The owner's rule (23 Nov 2026): 4K stays; a lower size or 2x may appear only as a labelled diagnostic arm.
        for name, row in self.table.items():
            if row["set"] not in SETS:
                continue
            lower = FIT in row["switches"] or "fgMultiplier=2" in row["switches"]
            self.assertEqual(lower, "DIAGNOSTIC" in row["note"], name)
            if lower:
                self.assertIn("not a", row["note"], name)   # "not a recommendation" / "not a solution"
            self.assertNotIn("srTarget=qhd", row["switches"], name)
            if kind(name) in ("fg3-low", "fg3-dlss", "fg3-flow"):
                self.assertIn("CHOOSE BY EYE", row["note"], name)
            # the headroom part may step 3x down to 2x: never in these arms
            self.assertNotIn("VEYRA_EXP_RP_FG_ULL=1", row["switches"], name)
            self.assertNotIn("headroom", row["switches"], name)

    def test_switch_names_are_the_engines(self):
        def read(*path):
            with open(os.path.join(ROOT, *path), encoding="utf-8") as f:
                return f.read()
        self.assertIn('L"VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY"', read("src", "gfx", "D3D12DeviceContext.cpp"))
        self.assertIn('L"VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE"', read("src", "pipeline", "EnhanceGraph.cpp"))
        self.assertIn('L"VEYRA_EXP_RP_FIT_DISPLAY"', read("include", "veyra", "engine", "PushSmoothHook.h"))
        self.assertIn('L"VEYRA_EXP_RP_STREAM_DEDUP"', read("src", "engine", "EngineController.cpp"))
        header = read("include", "veyra", "engine", "PushFgLatency.h")
        for part in ("hold", "phase", "grace"):
            self.assertIn('part == L"' + part + '"', header, part)
        self.assertIn("value.find(L',', start)", header)   # the parts are separated by commas (the arm's quoted field)

    def test_runs_inside_the_smoothness_and_capture_sessions(self):
        self.assertIn("if (@('smoothness', 'nrperf', 'fglat', 'd90') -contains [string]$Arm.set) { $profileKind += '-smoothness' }", self.kit)
        self.assertIn("if (@('capture', 'nrperf-capture', 'd90-capture') -contains [string]$Arm.set) { $startSwitch = 'VEYRA_TEST_RP_CAPTURE' }", self.kit)
        self.assertIn("if (@('capture', 'nrperf-capture', 'd90-capture') -contains [string]$Arm.set) { $profileKind = [string]$Arm.exe + '-capture' }", self.kit)
        for session, set_name in (("smoothness", "d90"), ("capture", "d90-capture")):
            m = re.search(r"-Session " + session + r" -Pass 180 -Arms (d90[a-z0-9,-]+)", self.csv)
            self.assertIsNotNone(m, session)
            self.assertEqual(m.group(1).split(","), SETS[set_name], session)

    def test_how_long_it_takes(self):
        found = re.findall(r"    '(\w+)' = @\{ plan = '([^']+)'; rounds = (\d+);", self.kit)
        plans = {s: p for s, p, _r in found}
        rounds = {s: int(r) for s, _p, r in found}
        for session, set_name in (("smoothness", "d90"), ("capture", "d90-capture")):
            minutes = report.about_minutes(rounds[session] * len(SETS[set_name]) * report.plan_seconds(plans[session]))
            self.assertEqual(minutes, 30, session)
        with open(os.path.join(ROOT, "docs", "drafts", "DELAY_90.md"), encoding="utf-8") as f:
            self.assertIn("about 30 minutes", f.read())

    def test_the_lines_it_reads_are_kept_and_shown(self):
        lines = [
            "[INFO ] [slipstream-sizes] source=3840x2160 srInput=3840x2160 srEvaluated=3840x2160 base=3840x2160 nr=- flow=3840x2160 fg=3840x2160 output=3840x2160 fit=0 fitReason=source-larger",
            "[INFO ] [graph-priority] requested=HIGH supported=1 queryHr=0x0 selected=1 (VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY)",
            "[INFO ] [slipstream-smooth] window=5.0 gpuFg1P50Ms=6.4 uploadCpuCopyFps=0.00 pickupWaitP50Ms=0.6 interpWaitP50Ms=22.2 partsSumP50Ms=32.9",
        ]
        for line in lines:
            stamped = "2026-11-23T08:00:00.000Z t=1 " + line
            self.assertEqual(report.safe_lines([stamped]), [stamped], line)
        for name in ("run-tests.ps1", "collect-diagnostics.ps1"):
            with open(os.path.join(KIT, name), encoding="utf-8") as f:
                self.assertIn("'" + report.ALLOWED.pattern + "'", f.read(), name)
        with open(os.path.join(KIT, "measures.csv"), encoding="utf-8") as f:
            rows = [r for r in csv.reader(l for l in f.read().splitlines() if l and not l.startswith("#"))][1:]
        groups = {r[0] for r in rows}
        self.assertIn("where the delay goes", groups)
        self.assertIn("processing sizes", groups)
        fields = {r[6] for r in rows if r[0] == "where the delay goes"}
        self.assertEqual(fields, {"pickupWaitP50Ms", "baseObsP50Ms", "fg1ObsP50Ms", "interpWaitP50Ms", "marginP50Ms", "realLateP50Ms",
                                  "partsSumP50Ms", "delayToCallP50Ms", "partsFps"})
        for r in rows:
            if r[0] in ("where the delay goes", "processing sizes"):
                self.assertEqual(set(r[1].split("|")), {"smoothness", "capture"}, r)


if __name__ == "__main__":
    unittest.main()
