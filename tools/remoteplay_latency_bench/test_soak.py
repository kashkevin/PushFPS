#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Tests of soak.py and of the soak sections of report.py on made-up runs.  python3 -B tools/remoteplay_latency_bench/test_soak.py"""
import io, json, math, os, random, sys, tempfile, unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report, soak
from test_report import add_run, evidence, line, make, presentmon_v2, presses, stamp, table

RULE = {rule.key: rule for rule in soak.RULES}

def series(hours, value, every=1.0, noise=0.0, seed=1):
    """[(seconds, value(hours since start))] every `every` seconds."""
    rng = random.Random(seed)
    return [(t * every, value(t * every / 3600.0) + (rng.gauss(0, noise) if noise else 0.0)) for t in range(int(hours * 3600 / every))]

def soak_run(folder, hours=2.0, private=lambda h: 800.0, working=lambda h: 900.0, handles=lambda h: 1500.0, threads=lambda h: 60.0, gpu_mb=lambda h: 1100.0, to_decoded=lambda h: 2.0, fps=lambda h: 59.94,
             vram=lambda h: 1200.0, extra=(), meta=None, windows=None, index=90, noise=1.0, arm="mod-all"):
    rng = random.Random(index)
    lines = [line(1, "remoteplay", "session started host=192.168.x.x profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000")]
    for second in range(2, int(hours * 3600), 10):
        h = second / 3600.0
        lines.append(line(second, "remoteplay-progress", f"state=3 received=600 decoded=600 inputFps=59.9 decodeFps={fps(h):.2f} videoMbps=28.100 receiveAgeMs={5.0 + to_decoded(h) + rng.gauss(0, 0.05):.2f} decodeAgeMs=5.0 decodeBusyMs=1.2 queue=0 waitingIdr=false idrRequests=0 errors=0 (x)"))
        lines.append(line(second, "frame-rate", f"revision=1 gpuCompletedFps=59.90 completedReal=60 capture60To30=false rateSkipped=0 presentSubmitFps={fps(h):.2f} windowMs=1000 vramMiB={vram(h):.0f} budgetMiB=11000 (not scanout FPS)"))
    run = add_run(folder, arm, index, lines + list(extra), purpose="soak", presentmon=False)
    with open(os.path.join(run, "samples.csv"), "w", encoding="ascii") as f:
        f.write("utc,cpuTotalPercent,appCpuPercent,appWorkingSetMb,appPrivateMb,appThreads,appHandles,appGpu3dPercent,appGpuDecodePercent,appGpuDedicatedMb,appGpuSharedMb\n")
        for second in range(0, int(hours * 3600), 2):
            h = second / 3600.0
            f.write(f"{stamp(second)},12,3.5,{working(h) + rng.gauss(0, noise):.1f},{private(h) + rng.gauss(0, noise):.1f},{threads(h):.0f},{handles(h) + rng.gauss(0, noise):.0f},20,8,{gpu_mb(h):.1f},64\n")
    record = {"arm": arm, "purpose": "soak", "compare": "mod-legacy", "presentMonStartUtc": "", "switches": [], "kitSwitches": ["VEYRA_EXP_RP_HEALTH=1"], "limitSeconds": int(hours * 3600), "limitReached": True, "stopReason": "time", "restarts": 0}
    record.update(meta or {})
    with open(os.path.join(run, "run.json"), "w") as f: json.dump(record, f)
    if windows:
        with open(os.path.join(run, "presentmon-windows.csv"), "w") as f:
            f.write("file,startUtc\n")
            for k, options in enumerate(windows):
                name = "presentmon.csv" if k == 0 else f"presentmon-{k + 1:03d}.csv"
                presentmon_v2(os.path.join(run, name), seconds=20, **options)
                f.write(f"{name},{stamp(k * 900)}\n")
    return run

def result_of(folder):
    run = next(r for r in report.load(folder)["runs"] if r["purpose"] == "soak")
    return report.soak_result(run)


class Trend(unittest.TestCase):
    def verdict(self, key, points, **kw): return soak.trend(points, RULE[key], 0.0, **kw)

    def test_a_leak_is_flagged_with_its_rate(self):
        t = self.verdict("private", series(2.0, lambda h: 800.0 + 120.0 * h, noise=2.0))
        self.assertEqual(t["verdict"], "GROWTH"); self.assertTrue(t["flag"]); self.assertAlmostEqual(t["per_hour"], 120.0, delta=3.0)
        self.assertAlmostEqual(t["end"] - t["start"], 120.0 * (2.0 - 5 / 60.0), delta=8.0)
        self.assertEqual(self.verdict("handles", series(2.0, lambda h: 1500 + 200 * h))["verdict"], "GROWTH")
        self.assertEqual(self.verdict("threads", series(2.0, lambda h: 60 + round(6 * h)))["verdict"], "GROWTH")

    def test_noise_spikes_and_small_slopes_are_steady(self):
        self.assertEqual(self.verdict("private", series(2.0, lambda h: 800.0, noise=6.0))["verdict"], "steady")
        spikes = [(t, v + (400.0 if int(t) % 900 < 20 else 0.0)) for t, v in series(2.0, lambda h: 800.0, noise=1.0)]       # a 20 s spike every quarter of an hour
        self.assertEqual(self.verdict("private", spikes)["verdict"], "steady")
        self.assertEqual(self.verdict("private", series(2.0, lambda h: 800.0 + 8.0 * h, noise=1.0))["verdict"], "steady")      # 8 MB an hour: under 20 MB and under 2 %
        self.assertEqual(self.verdict("private", series(2.0, lambda h: 4000.0 + 60.0 * h, noise=1.0))["verdict"], "steady")    # 60 MB an hour of 4 GB: under 2 %
        self.assertEqual(self.verdict("private", series(2.0, lambda h: 4000.0 + 130.0 * h, noise=1.0))["verdict"], "GROWTH")

    def test_a_level_that_rose_early_and_stayed_is_not_growth(self):
        t = self.verdict("private", series(3.0, lambda h: 800.0 + 300.0 * min(1.0, h / 0.6), noise=1.0))                     # fills a cache for 36 minutes
        self.assertEqual(t["verdict"], "changed early, then level"); self.assertFalse(t["flag"])
        sawtooth = self.verdict("private", series(3.0, lambda h: 800.0 + 200.0 * abs(math.sin(h * 4.0)), noise=1.0))
        self.assertFalse(sawtooth["flag"])

    def test_falling_rates_are_a_decline_and_falling_memory_is_not(self):
        self.assertEqual(self.verdict("present_fps", series(2.0, lambda h: 59.94 - 2.0 * h, noise=0.05))["verdict"], "DECLINE")
        self.assertEqual(self.verdict("present_fps", series(2.0, lambda h: 59.94, noise=0.05))["verdict"], "steady")
        self.assertEqual(self.verdict("private", series(2.0, lambda h: 900.0 - 100.0 * h, noise=1.0))["verdict"], "steady")

    def test_too_short_and_missing(self):
        self.assertIn("too short to judge", self.verdict("private", series(0.4, lambda h: 800.0 + 500.0 * h))["verdict"])
        self.assertEqual(self.verdict("private", [])["verdict"], "not recorded")
        self.assertEqual(self.verdict("private", series(0.4, lambda h: 800.0 + 500.0 * h), minimum_minutes=10.0)["verdict"], "GROWTH")
        self.assertIn("6 points", self.verdict("frame_p99", [(k * 900.0, 17.0 + k) for k in range(5)])["verdict"])
        self.assertEqual(self.verdict("frame_p99", [(k * 900.0, 17.0 + 0.5 * k) for k in range(8)])["verdict"], "GROWTH")       # 2 ms an hour over 8 windows
        self.assertEqual(self.verdict("frame_p99", [(k * 900.0, 17.0 + 0.01 * k) for k in range(8)])["verdict"], "steady")

    def test_the_fit_ignores_outliers(self):
        points = [(x / 10.0, 5.0 + 2.0 * x / 10.0) for x in range(100)]
        points[10] = (1.0, 500.0); points[70] = (7.0, -300.0)
        slope, intercept = soak.theil_sen(points)
        self.assertAlmostEqual(slope, 2.0, delta=0.05); self.assertAlmostEqual(intercept, 5.0, delta=0.3)
        self.assertAlmostEqual(soak.theil_sen([(x, 3.0 * x) for x in range(2000)])[0], 3.0, places=6)                          # thinned beyond 300 points


class SoakRuns(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name
        rng = random.Random(30)
        evidence(self.folder, {"mod-legacy": [presses(rng, 100.0, 40)]})
    def tearDown(self): self.tmp.cleanup()

    def test_a_clean_two_hour_run_passes(self):
        soak_run(self.folder)
        result = result_of(self.folder)
        self.assertTrue(result["verdict"].startswith("PASS"), result["verdict"]); self.assertFalse(result["failed"])
        self.assertAlmostEqual(result["minutes"], 120.0, delta=0.2); self.assertEqual(result["stop"], "time")
        for key in ("working_set", "private", "gpu_memory", "own_vram", "handles", "threads", "to_decoded", "present_fps", "decode_fps"):
            self.assertEqual(result["trends"][key]["verdict"], "steady", key)
        self.assertEqual(result["trends"]["loss"]["verdict"], "not recorded")

    def test_memory_gpu_memory_handles_and_latency_drift_are_each_flagged(self):
        soak_run(self.folder, working=lambda h: 900.0 + 90.0 * h, gpu_mb=lambda h: 1100.0 + 150.0 * h, handles=lambda h: 1500.0 + 400.0 * h, to_decoded=lambda h: 2.0 + 1.5 * h, vram=lambda h: 1200.0 + 150.0 * h)
        result = result_of(self.folder)
        self.assertTrue(result["failed"]); self.assertTrue(result["verdict"].startswith("FAIL: trend flagged: "))
        flagged = sorted(key for key, t in result["trends"].items() if t["flag"])
        self.assertEqual(flagged, ["gpu_memory", "handles", "own_vram", "to_decoded", "working_set"])
        self.assertAlmostEqual(result["trends"]["working_set"]["per_hour"], 90.0, delta=3.0); self.assertAlmostEqual(result["trends"]["to_decoded"]["per_hour"], 1.5, delta=0.1)
        self.assertEqual(result["trends"]["private"]["verdict"], "steady")

    def test_frame_pacing_drift_over_presentmon_windows(self):
        soak_run(self.folder, windows=[{"fps": 60.0, "drop_every": 0 if k < 2 else max(2, 60 - 8 * k)} for k in range(8)])
        result = result_of(self.folder)
        self.assertEqual(result["trends"]["dropped"]["points"], 8); self.assertEqual(result["trends"]["dropped"]["verdict"], "GROWTH")
        self.assertEqual(result["trends"]["frame_p99"]["verdict"], "steady")
        self.assertIn("Presented frames never displayed", result["verdict"])

    def test_events_fail_the_run_and_are_counted(self):
        extra = [line(600, "remoteplay-recovery", "reconnected attempt=1 first decoded frame; reset video history and audio clock"), line(500, "remoteplay-recovery", "request keyframe; received=1 decoded=1", "WARN "),
                 line(3000, "gpu-failure", "operation=present alive=false removedReason=0x887A0006", "ERROR"), line(3100, "remoteplay-recovery", "recovery stopped attempts=3 quitReason=1 error=5", "ERROR")]
        soak_run(self.folder, extra=extra)
        result = result_of(self.folder)
        self.assertTrue(result["failed"]); self.assertIn("1 GPU failure line(s)", result["verdict"]); self.assertIn("1 recovery gave up line(s)", result["verdict"])
        self.assertEqual(result["counts"]["reconnects"], 1); self.assertEqual(result["counts"]["keyframe_requests"], 1); self.assertEqual(result["counts"]["error_lines"], 2)

    def test_the_kill_switch_is_incomplete_not_a_failure_and_keeps_the_evidence(self):
        soak_run(self.folder, hours=0.75, meta={"limitSeconds": 7200, "limitReached": False, "stopReason": "kill-switch (key)"})
        result = result_of(self.folder)
        self.assertFalse(result["failed"]); self.assertTrue(result["verdict"].startswith("INCOMPLETE: stopped by the kill-switch after 45 of 120 minutes"), result["verdict"])
        self.assertEqual(result["trends"]["private"]["verdict"], "steady")                                  # 40 minutes after the warm-up: enough to judge

    def test_a_run_that_ended_by_itself_or_had_to_be_ended_fails(self):
        soak_run(self.folder, hours=0.5, meta={"limitSeconds": 7200, "limitReached": False, "stopReason": "closed"})
        self.assertIn("ended by itself or was closed after 30 of 120 minutes", result_of(self.folder)["verdict"])
        other = tempfile.TemporaryDirectory()
        try:
            soak_run(other.name, meta={"forcedStop": True})
            self.assertIn("did not close when asked", result_of(other.name)["verdict"])
        finally: other.cleanup()

    def test_report_sections_and_command_line(self):
        soak_run(self.folder, private=lambda h: 800.0 + 120.0 * h)
        md, _h, notes, _l = make(self.folder)
        result = table(md, "Soak result")["mod-all"]
        self.assertEqual(result["Minutes run"], "120"); self.assertEqual(result["Minutes planned"], "120"); self.assertEqual(result["Ended by"], "time")
        self.assertTrue(result["Result"].startswith("FAIL: trend flagged: Private memory of the process"))
        part = md.split("## Soak trends\n", 1)[1].split("\n## ", 1)[0]
        row = next(l for l in part.splitlines() if "Private memory of the process" in l).split("|")
        self.assertEqual(row[-2].strip(), "GROWTH"); self.assertAlmostEqual(float(row[-4]), 120.0, delta=3.0)
        self.assertTrue(any(n.startswith("Soak of 'mod-all': FAIL") for n in notes))
        self.assertIn("not input to photon", part)
        self.assertAlmostEqual(float(table(md, "Soak")["mod-all"]["Memory MB per hour"]), 120.0, delta=2.0)     # the older summary table is still there
        out = io.StringIO()
        with redirect_stdout(out): code = soak.main([self.folder])
        self.assertEqual(code, 1); self.assertIn("soak of 'mod-all': FAIL", out.getvalue()); self.assertIn("GROWTH", out.getvalue())
        clean = tempfile.TemporaryDirectory()
        try:
            soak_run(clean.name)
            with redirect_stdout(io.StringIO()): self.assertEqual(soak.main([clean.name]), 0)
        finally: clean.cleanup()
        empty = tempfile.TemporaryDirectory()
        try:
            with redirect_stdout(io.StringIO()): self.assertEqual(soak.main([empty.name]), 3)
        finally: empty.cleanup()


if __name__ == "__main__":
    unittest.main(verbosity=1)
