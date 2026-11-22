#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The neural-layer four-way comparison of the kit (stream nrperf, docs/drafts/NR_STUTTER.md section 7).

Checks that the nrperf / nrperf-capture arms are the comparison they claim to be: every arm differs from its pair in
exactly one thing (the neural layer, frame generation, the 4K target or the adaptive switch), they run inside the
smoothness / capture sessions with those sessions' preferences, settings folders and capture start, and the log lines
their page reads pass the kit's allow-list. Standard library only.
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

NR = ["nr=1", "nrRuntime=3", "nrLook=conservative", "nrSize=1080"]


def parts(table, name):
    """(pipeline keys as a list, other switches as a set) of an arm."""
    pipeline, other = [], set()
    for item in filter(None, table[name]["switches"].split(";")):
        if item.startswith("pipeline:"):
            pipeline = item[len("pipeline:"):].split("|")
        else:
            other.add(item)
    return pipeline, other


class NrPerfKit(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.table = report.read_arm_table(os.path.join(KIT, "arms.csv"))
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="utf-8") as f:
            cls.kit = f.read()

    def arms(self, sets):
        return {n: r for n, r in self.table.items() if r["set"] in sets}

    def test_four_way_arms_exist_with_their_pairs(self):
        rp = self.arms(("nrperf",))
        self.assertEqual(sorted(rp), sorted(["nrp-base", "nrp-nr", "nrp-fg", "nrp-nr-fg", "nrp-nr-fg-adapt",
                                             "nrp-sr4k-fg", "nrp-nr-sr4k-fg", "nrp-nr-sr4k-fg-adapt"]))
        cap = self.arms(("nrperf-capture",))
        self.assertEqual(sorted(cap), sorted(["nrpc-base", "nrpc-nr", "nrpc-fg", "nrpc-nr-fg", "nrpc-nr-fg-adapt"]))
        pairs = {"nrp-base": "nrp-base", "nrp-nr": "nrp-base", "nrp-fg": "nrp-base", "nrp-nr-fg": "nrp-fg",
                 "nrp-nr-fg-adapt": "nrp-nr-fg", "nrp-sr4k-fg": "nrp-fg", "nrp-nr-sr4k-fg": "nrp-sr4k-fg",
                 "nrp-nr-sr4k-fg-adapt": "nrp-nr-sr4k-fg", "nrpc-base": "nrpc-base", "nrpc-nr": "nrpc-base",
                 "nrpc-fg": "nrpc-base", "nrpc-nr-fg": "nrpc-fg", "nrpc-nr-fg-adapt": "nrpc-nr-fg"}
        for name, row in list(rp.items()) + list(cap.items()):
            self.assertEqual(row["compare"], pairs[name], name)
            self.assertEqual(self.table[row["compare"]]["set"], row["set"], name)   # the pair runs in the same command
            self.assertEqual(row["exe"], "experimental", name)
            pipeline, _ = parts(self.table, name)
            self.assertEqual(pipeline[:2], ["profile=original", "hdr=0"], name)      # no arm inherits another's settings
            self.assertIn("sr=dlss", pipeline, name)
            self.assertIn("srModel=M", pipeline, name)
            self.assertEqual(row["fg"] == "on", any(p.startswith("fg=") and p != "fg=off" for p in pipeline), name)
            if any(p == "nr=1" for p in pipeline):
                for key in NR: self.assertIn(key, pipeline, name)                   # the NVIDIA-signed original only, 1080 lines
            if row["set"] == "nrperf-capture": self.assertIn("dedup=auto", pipeline, name)

    def test_each_arm_differs_from_its_pair_in_one_thing(self):
        for name, row in self.arms(("nrperf", "nrperf-capture")).items():
            if row["compare"] == name: continue
            mine, mine_other = parts(self.table, name)
            theirs, theirs_other = parts(self.table, row["compare"])
            added = [p for p in mine if p not in theirs]
            removed = [p for p in theirs if p not in mine]
            extra = mine_other - theirs_other
            self.assertFalse(theirs_other - mine_other, name)
            if name.endswith("-adapt"):
                self.assertEqual((added, removed), ([], []), name)
                self.assertEqual(extra, {"VEYRA_EXP_RP_NR_ADAPT=1"}, name)
                self.assertEqual(row["differs"], "", name)
            elif "-nr" in name:
                self.assertEqual((sorted(added), removed, extra), (sorted(NR), [], set()), name)
                self.assertEqual(row["differs"], "enhance;nrRuntime;nrSize", name)
            elif "sr4k" in name:
                self.assertEqual((added, removed, extra), (["srTarget=4k"], ["srTarget=qhd"], set()), name)
                self.assertEqual(row["differs"], "srTarget", name)
            else:   # frame generation on against off
                self.assertTrue(all(p.startswith("fg") or p in ("dedup=auto", "hudLock=0") for p in added + removed), name)
                self.assertEqual(extra, set(), name)
                self.assertEqual(row["fg"], "on", name)
                self.assertEqual(self.table[row["compare"]]["fg"], "off", name)

    def test_sets_are_treated_like_their_sessions(self):
        # preferences, settings folder and (capture) the start of the remembered card: the same as the session's own arms
        self.assertIn("if (@('smoothness', 'nrperf') -contains [string]$Arm.set) { $table['fgAdaptive'] = $false; $table['pacingRepair'] = $false; $table['pushSmoothTarget'] = 0 }", self.kit)
        self.assertIn("if (@('capture', 'nrperf-capture') -contains [string]$Arm.set) { $table['captureBufferMode'] = 0;", self.kit)
        self.assertIn("if (@('smoothness', 'nrperf') -contains [string]$Arm.set) { $profileKind += '-smoothness' }", self.kit)
        self.assertIn("if (@('capture', 'nrperf-capture') -contains [string]$Arm.set) { $profileKind = [string]$Arm.exe + '-capture' }", self.kit)
        self.assertIn("if (@('capture', 'nrperf-capture') -contains [string]$Arm.set) { $startSwitch = 'VEYRA_TEST_RP_CAPTURE' }", self.kit)
        self.assertNotIn("[string]$Arm.set -eq 'smoothness'", self.kit)
        self.assertNotIn("[string]$Arm.set -eq 'capture'", self.kit)

    def test_page_reads_lines_the_kit_keeps(self):
        with open(os.path.join(KIT, "measures.csv"), encoding="utf-8") as f:
            rows = [l.split(",") for l in f.read().splitlines() if l and not l.startswith("#") and not l.startswith("group,")]
        mine = [r for r in rows if r[0] in ("neural layer cost", "lost before the engine")]
        self.assertGreaterEqual(len(mine), 10)
        for r in mine:
            self.assertEqual(len(r), 13, r)
            line = "[INFO ] [" + r[3] + "] x=1"
            self.assertTrue(report.ALLOWED.search(line), r[3])
            self.assertTrue(set(r[1].split("|")) <= {"smoothness", "capture"}, r)
        for component in ("nr-adapt", "nr-reuse", "nr-model"):
            self.assertTrue(report.ALLOWED.search("[INFO ] [" + component + "] x=1"), component)
        # the runtime's selection line names a folder: never kept
        self.assertFalse(report.ALLOWED.search("[INFO ] [nr-runtime] selected=original requestedId=3 path=x"))
        for name in ("collect-diagnostics.ps1", "run-tests.ps1"):
            with open(os.path.join(KIT, name), encoding="utf-8") as f:
                self.assertIn("'" + report.ALLOWED.pattern + "'", f.read(), name)


if __name__ == "__main__":
    unittest.main()
