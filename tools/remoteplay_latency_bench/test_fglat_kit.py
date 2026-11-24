#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Frame generation latency, part by part (the kit's fglat set; docs/drafts/FG_LATENCY.md section 6).

Checks that the fglat arms are the comparison they claim to be: FG off, VFG 2x as installed, each part of
VEYRA_EXP_RP_FG_ULL alone, all parts, all parts with the presentation queue at high priority and all parts with VFG Low.
Every arm differs from its pair in exactly one thing, the part names are ones the engine understands, the set runs
inside the smoothness session with that session's preferences and settings folder, and the log lines the comparison
needs pass the kit's allow-list. Standard library only.
"""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
KIT = os.path.join(ROOT, "scripts", "remoteplay", "testkit")
sys.path.insert(0, HERE)
import report  # noqa: E402

# Since Package 14 every part of VEYRA_EXP_RP_FG_ULL is inert: its arms were removed (they could only have read as "the
# experiment did not help"); the set keeps frame generation off against 2x as installed.
ARMS = ["flt-off", "flt-fg2"]
PAIRS = {"flt-off": "flt-off", "flt-fg2": "flt-off"}
# What each arm adds to its pair: (pipeline keys added, pipeline keys removed, other switches added)
ADDS = {
    "flt-fg2": (["fg=vfg", "fgMultiplier=2"], ["fg=off"], set()),
}


def parts(table, name):
    """(pipeline keys as a list, other switches as a set) of an arm."""
    pipeline, other = [], set()
    for item in filter(None, table[name]["switches"].split(";")):
        if item.startswith("pipeline:"):
            pipeline = item[len("pipeline:"):].split("|")
        else:
            other.add(item)
    return pipeline, other


class FgLatKit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table = report.read_arm_table(os.path.join(KIT, "arms.csv"))
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="utf-8") as f:
            cls.kit = f.read()
        with open(os.path.join(KIT, "arms.csv"), encoding="ascii") as f:
            cls.csv = f.read()
        with open(os.path.join(ROOT, "include", "veyra", "engine", "PushFgLatency.h"), encoding="utf-8") as f:
            cls.header = f.read()

    def arms(self):
        return {n: r for n, r in self.table.items() if r["set"] == "fglat"}

    def test_arms_exist_with_their_pairs(self):
        mine = self.arms()
        self.assertEqual(sorted(mine), sorted(ARMS))
        for name, row in mine.items():
            self.assertEqual(row["compare"], PAIRS[name], name)
            self.assertEqual(self.table[row["compare"]]["set"], "fglat", name)   # the pair runs in the same command
            self.assertEqual(row["exe"], "experimental", name)
            pipeline, other = parts(self.table, name)
            self.assertEqual(pipeline[:2], ["profile=original", "hdr=0"], name)  # no arm inherits another's settings
            self.assertIn("VEYRA_EXP_PRESENT_WINDOW_FACTS=1", other, name)
            self.assertEqual(row["fg"] == "on", any(p.startswith("fg=") and p != "fg=off" for p in pipeline), name)
            self.assertFalse(any(p.startswith("sr=") or p.startswith("nr=") for p in pipeline), name)   # latency of FG only

    def test_each_arm_differs_from_its_pair_in_one_thing(self):
        for name, row in self.arms().items():
            if row["compare"] == name:
                continue
            mine, mine_other = parts(self.table, name)
            theirs, theirs_other = parts(self.table, row["compare"])
            added = sorted(p for p in mine if p not in theirs)
            removed = sorted(p for p in theirs if p not in mine)
            self.assertFalse(theirs_other - mine_other, name)
            want_added, want_removed, want_extra = ADDS[name]
            self.assertEqual((added, removed, mine_other - theirs_other), (sorted(want_added), sorted(want_removed), want_extra), name)
            self.assertEqual(row["differs"], "", name)

    def test_part_names_are_the_engines(self):
        # pushUllParse turns an unknown part into "nothing on": a typo would make an arm a copy of its pair
        for part in ("hold", "phase", "grace", "headroom"):
            self.assertIn('part == L"' + part + '"', self.header, part)
        self.assertIn('value == L"1"', self.header)                                # flt-fg2-ull's "1" means every part
        with open(os.path.join(ROOT, "src", "engine", "VideoPresenter.cpp"), encoding="utf-8") as f:
            self.assertIn('GetEnvironmentVariableW(L"VEYRA_TEST_PRESENT_QUEUE_PRIORITY",requestedPriority,16)&&std::wstring_view(requestedPriority)==L"high"', f.read())

    def test_runs_inside_the_smoothness_session(self):
        self.assertIn("if (@('smoothness', 'nrperf', 'fglat', 'd90') -contains [string]$Arm.set) { $table['fgAdaptive'] = $false; $table['pacingRepair'] = $false; $table['pushSmoothTarget'] = 0 }", self.kit)
        self.assertIn("if (@('smoothness', 'nrperf', 'fglat', 'd90') -contains [string]$Arm.set) { $profileKind += '-smoothness' }", self.kit)
        # the command in arms.csv names exactly this set, in the order of the table
        m = re.search(r"-Session smoothness -Pass 120 -Arms (flt-[a-z0-9,-]+)", self.csv)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1).split(","), ARMS)
        self.assertEqual([n for n in self.table if n.startswith("flt-")], ARMS)

    def test_how_long_it_takes(self):
        found = re.findall(r"    '(\w+)' = @\{ plan = '([^']+)'; rounds = (\d+);", self.kit)
        plans = {s: p for s, p, _r in found}
        rounds = {s: int(r) for s, _p, r in found}
        self.assertEqual(report.about_minutes(rounds["smoothness"] * len(ARMS) * report.plan_seconds(plans["smoothness"])), 6)

    def test_the_lines_it_compares_are_kept(self):
        lines = [
            "[INFO ] [fg-ull] experiment: Ultra Low Latency Frame Generation parts=phase understood=true (VEYRA_EXP_RP_FG_ULL)",
            "[INFO ] [remoteplay-timing] experiment: adaptive pair phase for the PS5 stream (VEYRA_EXP_RP_FG_ULL part phase)",
            "[INFO ] [present-priority] requested=HIGH supported=true queryHr=0x0 selected=1",
            "[INFO ] [slipstream-smooth] window=5.0 realAgeP50Ms=20.1 addedEstMs=12.3 presentLateP99Ms=0.8 deadPhaseFps=0.0",
            "[INFO ] [slipstream-cadence] window=5.0 realFps=59.9 generatedFps=59.9 intervalP99Ms=9.1",
        ]
        for line in lines:
            stamped = "2026-11-22T08:00:00.000Z t=1 " + line
            self.assertEqual(report.safe_lines([stamped]), [stamped], line)
        for name in ("run-tests.ps1", "collect-diagnostics.ps1"):
            with open(os.path.join(KIT, name), encoding="ascii") as f:
                self.assertIn("present-contract|present-priority|", f.read(), name)


if __name__ == "__main__":
    unittest.main()
