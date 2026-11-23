#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Tests of presentmon.py on made-up captures of both column generations.  python3 -B tools/remoteplay_latency_bench/test_presentmon.py"""
import os, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import presentmon

def write(folder, name, header, rows):
    path = os.path.join(folder, name)
    with open(path, "w", encoding="utf-8", newline="") as f: f.write(",".join(header) + "\n" + "\n".join(",".join(r) for r in rows) + "\n")
    return path

class PresentMon(unittest.TestCase):
    def setUp(self): self.tmp = tempfile.TemporaryDirectory()
    def tearDown(self): self.tmp.cleanup()

    def test_current_columns(self):
        header = ["Application", "ProcessID", "SwapChainAddress", "PresentRuntime", "SyncInterval", "PresentFlags", "AllowsTearing", "PresentMode", "FrameType", "CPUStartTime",
                  "MsBetweenSimulationStart", "MsBetweenPresents", "MsBetweenDisplayChange", "MsInPresentAPI", "MsRenderPresentLatency", "MsUntilDisplayed", "MsPCLatency"]
        rows = [["PushFPS.exe", "1", "0x1", "DXGI", "1", "0", "0", "Composed: Flip", "Application", "0.1", "NA", "16.0", "16.0", "0.1", "1.0", "20.0", "NA"]]   # the interface's chain
        for i in range(200):
            mode = "Hardware: Independent Flip" if i < 150 else "Composed: Flip"
            rows.append(["PushFPS.exe", "1", "0x2", "DXGI", "1", "0", "0", mode, "Application", f"{i / 60:.4f}", "NA", f"{16.6 + (i % 5) * 0.1:.2f}",
                         "NA" if i % 50 == 49 else f"{16.6 + (i % 3):.2f}", "0.20", "1.0", "NA" if i % 50 == 49 else f"{5 + i % 4}", "NA"])
        rows.append(["other.exe", "2", "0x3", "DXGI", "1", "0", "1", "Composed: Flip", "Application", "0.1", "NA", "7.0", "7.0", "0.1", "1.0", "9.0", "NA"])
        s = presentmon.summary(write(self.tmp.name, "a.csv", header, rows))
        self.assertEqual(s["frames"], 200)
        self.assertAlmostEqual(s["independentPercent"], 75.0); self.assertAlmostEqual(s["composedPercent"], 25.0)
        self.assertAlmostEqual(s["tearingPercent"], 0.0); self.assertAlmostEqual(s["droppedPercent"], 2.0)
        self.assertAlmostEqual(s["frameP50"], 16.8, places=6); self.assertGreater(s["shownP99"], s["shownP95"] - 1e-9)
        self.assertEqual(s["untilP50"], 6.5); self.assertAlmostEqual(s["inPresentP50"], 0.2)
        self.assertTrue(all(v for k, v in s["columns"].items() if k != "frameToDisplay"))

    def test_v1_and_older_v2_names_lower_case_and_dropped(self):
        header = ["Application", "ProcessID", "SwapChainAddress", "Runtime", "SyncInterval", "PresentFlags", "AllowsTearing", "PresentMode", "Dropped", "TimeInSeconds",
                  "msBetweenPresents", "msBetweenDisplayChange", "msInPresentAPI", "msUntilRenderComplete", "msUntilDisplayed"]
        rows = [["pushfps.exe", "1", "0x2", "DXGI", "0", "512", "1", "Hardware: Legacy Flip", "1" if i % 10 == 0 else "0", f"{i / 120:.4f}", "8.33", "8.33", "0.1", "1.0", "3.0"] for i in range(100)]
        s = presentmon.summary(write(self.tmp.name, "b.csv", header, rows))
        self.assertAlmostEqual(s["droppedPercent"], 10.0); self.assertAlmostEqual(s["tearingPercent"], 100.0); self.assertAlmostEqual(s["independentPercent"], 100.0)
        header2 = ["Application", "ProcessID", "SwapChainAddress", "PresentMode", "AllowsTearing", "CPUStartTime", "FrameTime", "DisplayedTime", "DisplayLatency"]
        rows2 = [["PushFPS.exe", "1", "0x2", "Composed: Flip", "0", "0", "16.6", "NA" if i == 0 else "16.6", "NA" if i == 0 else "30"] for i in range(10)]
        s2 = presentmon.summary(write(self.tmp.name, "c.csv", header2, rows2))
        self.assertAlmostEqual(s2["droppedPercent"], 10.0); self.assertEqual(s2["frameToDisplayP95"], 30.0); self.assertNotEqual(s2["untilP95"], s2["untilP95"])   # DisplayLatency is not present-to-display
        self.assertEqual(s2["composedPercent"], 100.0)
        merged = presentmon.merge([s, s2, None])
        self.assertEqual(merged["captures"], 2); self.assertEqual(merged["frames"], 110)

    def test_no_rows_and_missing_file(self):
        self.assertIsNone(presentmon.summary(os.path.join(self.tmp.name, "missing.csv")))
        self.assertIsNone(presentmon.summary(write(self.tmp.name, "d.csv", ["Application", "PresentMode"], [["other.exe", "Composed: Flip"]])))
        self.assertIsNone(presentmon.merge([None]))

if __name__ == "__main__":
    unittest.main()
