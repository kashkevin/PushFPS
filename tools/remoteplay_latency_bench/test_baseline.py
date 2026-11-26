#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Tests of baseline.py on made-up evidence folders.  python3 -B tools/remoteplay_latency_bench/test_baseline.py"""
import io, json, os, random, sys, tempfile, unittest
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import baseline, report
from test_report import add_run, evidence, full_extra, health_line, line, presses, run_log, samples, stamp


def quality_line(seconds, score, psnr): return line(seconds, "push-quality", f"done samples=60 psnr={psnr:.2f} blendPsnr=27.10 gain=3.00 realPsnr=44.00 realSamples=60 hudPsnr=40.00 hudSamples=4 meanError=0.0100 bad=0.0100 onTime=58/60 fitGain=1.000 fitOffset=0.000 withheld=60 score={score:.1f}")

def interleave(folder, arms, pairs, seed=1, per_run=40, order=None):
    """arms: {name: {"mean": ms, "extra": callable(k) -> lines, "samples": dict, "pm": dict}}. Runs go A B, B A, A B ... like the kit's rounds."""
    rng = random.Random(seed)
    names = list(arms)
    index = 0
    sequence = order or [name for k in range(pairs) for name in (names if k % 2 == 0 else names[::-1])]
    count = {name: 0 for name in names}
    for name in sequence:
        spec = arms[name]; k = count[name]; count[name] += 1
        mean = spec["mean"](k) if callable(spec["mean"]) else spec["mean"]
        extra = spec["extra"](k) if spec.get("extra") else []
        stream = spec.get("stream")
        lines = run_log(presses(rng, mean, per_run), extra=extra)
        if stream: lines = [l.replace("requestedBitrateKbps=30000", stream) for l in lines]
        run = add_run(folder, name, index, lines, **spec.get("pm", {})); index += 1
        if spec.get("samples"): samples(os.path.join(run, "samples.csv"), **spec["samples"](k))
    for name in ("kit.json", "hardware.json"):
        if not os.path.exists(os.path.join(folder, name)):
            with open(os.path.join(folder, name), "w") as f:
                json.dump({"presentMon": True} if name == "kit.json" else {"cpu": {"name": "Test CPU"}, "adapters": [{"name": "Test GPU", "refreshHz": 180}]}, f)

def run(*argv):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err): code = baseline.main(list(argv))
    return code, out.getvalue() + err.getvalue()

def verdicts(text):
    """{metric label: verdict} from the printed table."""
    out = {}
    for row in text.splitlines():
        cells = [c.strip() for c in row.split("  ") if c.strip()]
        for metric in baseline.METRICS:
            if metric.label in row: out[metric.key] = cells[-1]
    return out


class Statistics(unittest.TestCase):
    def test_t_interval_of_paired_differences(self):
        centre, low, high = baseline.paired_interval([5.0, 6.0, 4.0])
        self.assertAlmostEqual(centre, 5.0); self.assertAlmostEqual(low, 5.0 - 4.303 / 3 ** 0.5, places=3); self.assertAlmostEqual(high, 5.0 + 4.303 / 3 ** 0.5, places=3)
        self.assertTrue(all(v != v for v in baseline.paired_interval([1.0])[1:]))
        self.assertAlmostEqual(baseline.t_critical(1), 12.706); self.assertAlmostEqual(baseline.t_critical(30), 2.042); self.assertAlmostEqual(baseline.t_critical(500), 1.96)
        self.assertTrue(all(baseline.t_critical(d) >= baseline.t_critical(d + 1) for d in range(1, 200)))

    def test_the_rule_for_one_metric(self):
        metric = next(m for m in baseline.METRICS if m.key == "frame_jitter_ms")            # tolerance 0.5, higher is worse
        j = lambda ref, cand, **kw: baseline.judge(metric, ref, cand, 99, 99, **kw)
        self.assertEqual(j([1.0, 1.1, 0.9, 1.0], [2.0, 2.2, 1.9, 2.1])["verdict"], "REGRESSION")
        self.assertEqual(j([2.0, 2.2, 1.9, 2.1], [1.0, 1.1, 0.9, 1.0])["verdict"], "improved")
        self.assertEqual(j([1.0, 1.1, 0.9, 1.0], [1.2, 1.31, 1.1, 1.19])["verdict"], "within tolerance (a real difference, smaller than the tolerance)")
        self.assertEqual(j([1.0, 1.1, 0.9, 1.0], [3.0, 0.2, 0.1, 2.5])["verdict"], "not resolved: the interval includes zero")       # large and inconsistent: not a regression
        self.assertEqual(j([1.0, 1.1], [2.0, 2.2])["verdict"], "no verdict: 2 pair(s), 3 needed")
        self.assertEqual(j([1.0, None, 0.9, 1.0], [2.0, 2.2, None, 2.1])["verdict"], "no verdict: 2 pair(s), 3 needed")              # only pairs with both values count
        self.assertEqual(j([], [])["verdict"], "no verdict: not recorded in both arms")
        self.assertEqual(j([1.0, 1.1, 0.9, 1.0], [2.0, 2.2, 1.9, 2.1], tolerance=5.0)["verdict"], "within tolerance (a real difference, smaller than the tolerance)")
        lower = next(m for m in baseline.METRICS if m.key == "quality_score")
        self.assertTrue(baseline.judge(lower, [80.0, 81.0, 79.0], [70.0, 71.0, 69.5], 3, 3)["regression"])
        self.assertEqual(baseline.judge(lower, [70.0, 71.0, 69.5], [80.0, 81.0, 79.0], 3, 3)["verdict"], "improved")
        presses_metric = next(m for m in baseline.METRICS if m.key == "software_latency_mean_ms")
        self.assertIn("fewer than 120 presses", baseline.judge(presses_metric, [100.0, 101.0, 99.0], [110.0, 111.0, 109.0], 119, 300)["verdict"])
        relative = next(m for m in baseline.METRICS if m.key == "process_memory_mb")          # 50 MB or 5 %, whichever is larger
        self.assertAlmostEqual(baseline.judge(relative, [4000.0] * 3, [4100.0, 4101.0, 4099.0], 9, 9)["tolerance"], 200.0)
        self.assertAlmostEqual(baseline.judge(relative, [400.0] * 3, [410.0] * 3, 9, 9)["tolerance"], 50.0)

    def test_interleaving(self):
        r = lambda t: {"meta": {}, "name": f"x@20261118-12{t:02d}00"}
        self.assertTrue(baseline.interleaved([r(0), r(3), r(4)], [r(1), r(2), r(5)]))
        self.assertFalse(baseline.interleaved([r(0), r(1), r(2)], [r(3), r(4), r(5)]))
        self.assertFalse(baseline.interleaved([r(3), r(4)], [r(0), r(1)]))


class SaveAndCompare(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.base = self.tmp.name
        self.store = os.path.join(self.base, "store")
        self.first = os.path.join(self.base, "evidence-first"); os.makedirs(self.first)
        both = lambda k: full_extra() + [quality_line(110, 80.0 + k * 0.2, 31.0)]
        interleave(self.first, {"mod-legacy": {"mean": 100.0, "extra": both}, "mod-all": {"mean": 100.0, "extra": both}}, pairs=4, seed=3)
    def tearDown(self): self.tmp.cleanup()

    def new(self, name, arms, pairs=4, **kw):
        folder = os.path.join(self.base, name); os.makedirs(folder)
        interleave(folder, arms, pairs, **kw)
        return folder

    def test_save_stores_every_subsystem_with_percentiles_and_counts(self):
        code, text = run("save", "rig-a", self.first, "--store", self.store)
        self.assertEqual(code, 0, text)
        with open(os.path.join(self.store, "rig-a.json")) as f: record = json.load(f)
        self.assertEqual(record["arm"], "mod-legacy"); self.assertEqual(record["run_count"], 4)
        self.assertEqual({record["labels"][k]["subsystem"] for k in record["metrics"]}, set(baseline.SUBSYSTEMS))
        self.assertEqual(baseline.SUBSYSTEMS, ["input latency", "network stability", "decode latency", "frame pacing", "GPU utilisation", "memory", "image quality"])
        latency = record["metrics"]["software_latency_mean_ms"]
        self.assertEqual(latency["samples"], 160); self.assertEqual(latency["runs"], 4); self.assertEqual(len(latency["per_run"]), 4)
        for key in ("median", "p95", "p99", "mean"): self.assertTrue(80 < latency[key] < 130, (key, latency[key]))
        self.assertLessEqual(latency["median"], latency["p95"]); self.assertLessEqual(latency["p95"], latency["p99"])
        self.assertEqual(record["conditions"]["streams"], [["1920x1080@60", "HEVC", "30000"]]); self.assertIn("Independent Flip", record["conditions"]["present_mode"])
        self.assertEqual(record["metrics"]["quality_score"]["samples"], 4); self.assertAlmostEqual(record["metrics"]["video_memory_mb"]["median"], 1200.0)
        with open(os.path.join(self.store, "rig-a.json")) as f: self.assertNotIn("NaN", f.read())
        code, text = run("show", "rig-a", "--store", self.store)
        self.assertEqual(code, 0); self.assertIn("software latency of all presses; verdict on the mean of each run (built-in test clock; not input to photon)", text); self.assertIn("image quality", text)
        code, text = run("save", "rig-a", self.first, "--store", self.store)
        self.assertEqual(code, 2); self.assertIn("exists already", text)
        self.assertEqual(run("save", "rig-a", self.first, "--store", self.store, "--replace")[0], 0)
        self.assertEqual(run("save", "bad name", self.first, "--store", self.store)[0], 2)
        self.assertEqual(run("save", "x", self.first, "--store", self.store, "--arm", "nope")[0], 2)

    def test_no_change_is_not_a_regression(self):
        run("save", "rig-a", self.first, "--store", self.store)
        code, text = run("compare", "rig-a", self.first, "--candidate", "mod-all", "--store", self.store)
        self.assertEqual(code, 0, text); self.assertIn("RESULT: no regression", text)
        self.assertNotIn("REGRESSION", text.replace("RESULT: no regression", ""))
        found = verdicts(text)
        self.assertEqual(found["software_latency_mean_ms"], "not resolved: the interval includes zero")
        self.assertIn(found["displayed_frame_time_p99_ms"], ("not resolved: the interval includes zero", "within tolerance (a real difference, smaller than the tolerance)"))

    def test_a_latency_regression_is_flagged_and_exits_non_zero(self):
        run("save", "rig-a", self.first, "--store", self.store)
        slower = self.new("evidence-slow", {"mod-legacy": {"mean": 100.0}, "candidate": {"mean": 112.0}}, pairs=5, seed=4)
        markdown = os.path.join(self.base, "verdict.md")
        code, text = run("compare", "rig-a", slower, "--candidate", "candidate", "--store", self.store, "--markdown", markdown)
        self.assertEqual(code, 1, text); self.assertIn("RESULT: REGRESSION", text)
        found = verdicts(text)
        self.assertEqual(found["software_latency_mean_ms"], "REGRESSION")
        self.assertEqual([k for k, v in found.items() if v == "REGRESSION" and not k.startswith("software_latency")], [])        # nothing else moved
        with open(markdown) as f: written = f.read()
        self.assertIn("| input latency |", written); self.assertIn("Result: REGRESSION", written)
        faster = self.new("evidence-fast", {"mod-legacy": {"mean": 100.0}, "candidate": {"mean": 88.0}}, pairs=5, seed=5)
        code, text = run("compare", "rig-a", faster, "--candidate", "candidate", "--store", self.store)
        self.assertEqual(code, 0); self.assertEqual(verdicts(text)["software_latency_mean_ms"], "improved")

    def test_a_real_but_small_difference_stays_within_tolerance(self):
        run("save", "rig-a", self.first, "--store", self.store)
        folder = os.path.join(self.base, "evidence-memory"); os.makedirs(folder)
        interleave(folder, {"mod-legacy": {"mean": 100.0}, "candidate": {"mean": 100.0}}, pairs=4, seed=6)
        for name in sorted(os.listdir(os.path.join(folder, "runs"))):                                    # candidate uses 10 MB more, every run
            extra = 10.0 if name.startswith("candidate") else 0.0
            with open(os.path.join(folder, "runs", name, "samples.csv"), "w") as f:
                f.write("utc,cpuTotalPercent,appCpuPercent,appWorkingSetMb,appPrivateMb,appThreads,appHandles\n" + "".join(f"{stamp(s)},12,3.5,900.0,{800.0 + extra + (hash(name) % 7) * 0.01:.2f},60,1500\n" for s in range(130)))
        code, text = run("compare", "rig-a", folder, "--candidate", "candidate", "--store", self.store)
        self.assertEqual(code, 0, text)
        self.assertEqual(verdicts(text)["process_memory_mb"], "within tolerance (a real difference, smaller than the tolerance)")
        code, text = run("compare", "rig-a", folder, "--candidate", "candidate", "--store", self.store, "--tolerance", "process_memory_mb=5")
        self.assertEqual(code, 1); self.assertEqual(verdicts(text)["process_memory_mb"], "REGRESSION")
        self.assertEqual(run("compare", "rig-a", folder, "--candidate", "candidate", "--store", self.store, "--tolerance", "nonsense=5")[0], 2)

    def test_too_few_pairs_or_blocks_give_no_verdict(self):
        run("save", "rig-a", self.first, "--store", self.store)
        two = self.new("evidence-two", {"mod-legacy": {"mean": 100.0}, "candidate": {"mean": 140.0}}, pairs=2, per_run=80, seed=7)
        code, text = run("compare", "rig-a", two, "--candidate", "candidate", "--store", self.store)
        self.assertEqual(code, 0); self.assertIn("RESULT: no verdict", text); self.assertEqual(verdicts(text)["software_latency_mean_ms"], "no verdict: 2 pair(s), 3 needed")
        self.assertEqual(run("compare", "rig-a", two, "--candidate", "candidate", "--store", self.store, "--require-verdict")[0], 3)
        blocks = self.new("evidence-blocks", {"mod-legacy": {"mean": 100.0}, "candidate": {"mean": 140.0}}, pairs=4, seed=8, order=["mod-legacy"] * 4 + ["candidate"] * 4)
        code, text = run("compare", "rig-a", blocks, "--candidate", "candidate", "--store", self.store)
        self.assertEqual(code, 0); self.assertTrue(all(v.startswith("no verdict: the arms were not interleaved") for v in verdicts(text).values()), text)
        alone = self.new("evidence-alone", {"candidate": {"mean": 140.0}}, pairs=4, seed=9)
        code, text = run("compare", "rig-a", alone, "--candidate", "candidate", "--store", self.store)
        self.assertEqual(code, 0); self.assertIn("no runs of the reference arm 'mod-legacy'", text); self.assertIn("cannot be told from drift", text)

    def test_unlike_conditions_are_refused(self):
        run("save", "rig-a", self.first, "--store", self.store)
        other = self.new("evidence-bitrate", {"mod-legacy": {"mean": 100.0}, "candidate": {"mean": 130.0, "stream": "requestedBitrateKbps=10000"}}, pairs=4, seed=10)
        code, text = run("compare", "rig-a", other, "--candidate", "candidate", "--store", self.store)
        self.assertEqual(code, 0); self.assertIn("REFUSED: stream settings differ", verdicts(text)["software_latency_mean_ms"]); self.assertIn("RESULT: no verdict", text)

    def test_frame_pacing_and_image_quality_regressions(self):
        run("save", "rig-a", self.first, "--store", self.store)
        worse = lambda k: full_extra() + [quality_line(110, 70.0 + k * 0.2, 29.0)]
        good = lambda k: full_extra() + [quality_line(110, 80.0 + k * 0.2, 31.0)]
        folder = self.new("evidence-pacing", {"mod-legacy": {"mean": 100.0, "extra": good}, "candidate": {"mean": 100.0, "extra": worse, "pm": {"drop_every": 20}}}, pairs=4, seed=11)
        code, text = run("compare", "rig-a", folder, "--candidate", "candidate", "--store", self.store)
        found = verdicts(text)
        self.assertEqual(code, 1); self.assertEqual(found["dropped_frames_percent"], "REGRESSION"); self.assertEqual(found["quality_score"], "REGRESSION"); self.assertEqual(found["generated_psnr_db"], "REGRESSION")
        self.assertNotEqual(found["software_latency_mean_ms"], "REGRESSION")

    def test_network_regression_and_reference_drift(self):
        run("save", "rig-a", self.first, "--store", self.store)
        lossy = lambda k: [health_line(5 * i, loss=0.8 + 0.01 * k, jitter=3.0 + 0.1 * k, lost=70) for i in range(1, 20)]
        clean = lambda k: [health_line(5 * i, loss=0.0, jitter=0.4 + 0.01 * k) for i in range(1, 20)]
        folder = self.new("evidence-network", {"mod-legacy": {"mean": 120.0, "extra": clean}, "candidate": {"mean": 120.0, "extra": lossy}}, pairs=4, seed=12)
        code, text = run("compare", "rig-a", folder, "--candidate", "candidate", "--store", self.store)
        found = verdicts(text)
        self.assertEqual(code, 1); self.assertEqual(found["packet_loss_percent"], "REGRESSION"); self.assertEqual(found["frame_jitter_ms"], "REGRESSION")
        self.assertNotEqual(found["software_latency_mean_ms"], "REGRESSION")                               # both arms are 20 ms slower than the stored day: drift, not the candidate
        self.assertIn("DRIFTED", text); self.assertIn("do not reproduce the stored baseline", text)

    def test_software_time_is_never_called_input_to_photon(self):
        run("save", "rig-a", self.first, "--store", self.store)
        for argv in (["show", "rig-a", "--store", self.store], ["compare", "rig-a", self.first, "--candidate", "mod-all", "--store", self.store]):
            text = run(*argv)[1].lower()
            self.assertIn("not input to photon", text)
            self.assertNotIn("photon", text.replace("not input to photon", "")); self.assertNotIn("end-to-end", text); self.assertNotIn("button to screen", text)
        self.assertIn("is not input to photon", baseline.__doc__)

    def test_missing_baseline_or_evidence(self):
        self.assertEqual(run("compare", "none", self.first, "--candidate", "mod-all", "--store", self.store)[0], 2)
        run("save", "rig-a", self.first, "--store", self.store)
        self.assertEqual(run("compare", "rig-a", os.path.join(self.base, "nothing"), "--candidate", "mod-all", "--store", self.store)[0], 2)
        self.assertEqual(run("compare", "rig-a", self.first, "--candidate", "nope", "--store", self.store)[0], 2)
        self.assertEqual(run()[0], 2)


if __name__ == "__main__":
    unittest.main(verbosity=1)
