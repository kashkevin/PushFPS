#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of triage.py on made-up log lines (the line format of PushFPS's own log; nothing here is a measurement)."""
import io
import os
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import triage  # noqa: E402


def line(seconds, component, text, level="INFO "):
    m, s = divmod(seconds, 60)
    return "2026-11-23T20:%02d:%06.3fZ t=1 [%s] [%s] %s" % (m, s, level, component, text)


def flow(seconds, session, real, gen, expired=0, skipped=0, overwritten=0):
    return line(seconds, "frame-flow", "state=active session=%d revision=2 epoch=1 captureReceived=%d mailboxOverwritten=%d sourceAccepted=%d "
                "fgSkippedBeforeEval=%d fgEvaluated=%d realPresented=%d generatedPresented=%d generatedExpiredAfterEval=%d cancelledBeforePresent=0"
                % (session, real * 2, overwritten, real, skipped, real, real, gen, expired))


def session_lines(start, build70, late_per_s=0, span=12.0, ini="switches=[VEYRA_EXP_RP_STATS=1 VEYRA_EXP_RP_FG_ULL=hold,phase,grace]"):
    out = [line(start, "remoteplay-settings", "slipstream.ini mode=none lan=0 %s off=[] unknown=[] applied=2 alreadyInEnvironment=[]" % ini),
           line(start + 0.5, "nr-runtime-status", "original=present selected=original selectedAvailable=1 reason=none"),
           line(start + 1, "slipstream-config", "fg=1 fgMethod=0 fgMultiplier=3 fgTier=1 sr=0 fullscreen=1 window=2560x1440 refreshHz=180.00 hdr=0 nrRuntime=3"),
           line(start + 1.2, "fg-ull", "experiment: Ultra Low Latency Frame Generation parts=hold,phase,grace understood=1")]
    if build70:
        out.append(line(start + 2, "slipstream-sizes", "source=3840x2160 base=3840x2160 fg=3840x2160 output=3840x2160 fit=0 fitReason=source-size"))
    for k in range(0, 31):   # 30 s of counters: 30 real and 60 generated frames a second, `late_per_s` expired
        out.append(flow(start + 3 + k, 1, 30 * k, 60 * k - late_per_s * k, expired=late_per_s * k))
    out.append(line(start + 20, "slipstream-smooth", "window=5.0 chosen=3X contentFps=29.97 gpuSpanP95Ms=%.1f pairBudgetMs=33.37 realAgeP50Ms=-1.000 phaseMs=30.1 firstReadyP50Ms=8.0" % span))
    out.append(line(start + 20, "slipstream-capture", "window=5.0s realLatP50Ms=30.40 realLatP95Ms=32.00 newFps=29.97 missedNewFps=0.00"))
    out.append(line(start + 20, "slipstream-cadence", "window=5.0s submittedFps=89.90 realFps=29.97 generatedFps=59.93 intervalP50Ms=11.11 intervalP95Ms=11.50 intervalP99Ms=12.00 intervalMaxMs=14.00 offPct=0.5 vrr=1 refreshHz=179.95"))
    return out


class TriageTests(unittest.TestCase):
    def test_a_smooth_session_says_so_and_names_what_was_on(self):
        text = triage.triage([("log-PushFPS-slipstream-exp.txt", session_lines(0, True))])
        self.assertIn("Program: build #70 or later", text)
        self.assertIn("VEYRA_EXP_RP_FG_ULL=hold,phase,grace", text)                     # the ini as the program accepted it
        self.assertIn("[fg-ull] experiment: Ultra Low Latency", text)                    # the switch as the program logged it
        self.assertIn("Neural runtime: original=present", text)
        self.assertIn("frames shown: real 30.0/s, generated 60.0/s (expected 60.0 generated for 3X)", text)
        self.assertIn("GPU finishes a pair in time: span P95 12.0 ms of a 33.4 ms pair", text)
        self.assertIn("presented evenly: 89.9 FPS submitted", text)
        self.assertIn("[slipstream-capture] realLatP50Ms): 30.40 ms", text)
        self.assertIn("realAgeP50Ms): not logged by this build or run", text)               # -1 is unknown, never a value
        self.assertNotIn("too late", text)

    def test_switches_in_force_and_ignored_ini_lines_are_shown(self):
        # Stream "lagfix": the start-up line of every switch in force, and what the program ignored from slipstream.ini.
        lines = session_lines(0, True, ini="switches=[VEYRA_EXP_RP_KBM_SENS=2]")
        lines.insert(1, line(0.1, "remoteplay-settings", "slipstream.ini ignored=[VEYRA_EXP_RP_FG_ULL=hold,phase,grace VEYRA_EXP_RP_STATS=1] (frame-path ...)"))
        lines.insert(2, line(0.2, "experiment-switches", "in force at start: VEYRA_EXP_RP_KBM_SENS=2(ini) (ini: set from slipstream.ini at this start; env: ...)"))
        text = triage.triage([("log-x.txt", lines)])
        self.assertIn("ignored=[VEYRA_EXP_RP_FG_ULL=hold,phase,grace VEYRA_EXP_RP_STATS=1]", text)
        self.assertIn("[experiment-switches] in force at start: VEYRA_EXP_RP_KBM_SENS=2(ini)", text)
        self.assertNotIn("(ini: set from", text)
        self.assertEqual(len(triage.split_starts(triage.parse(lines))), 1)   # one start, not two

    def test_late_frames_and_a_slow_gpu_are_named(self):
        text = triage.triage([("log-x.txt", session_lines(0, True, late_per_s=6, span=35.0))])
        self.assertIn("generated frames arrive too late: 6.0 per second dropped at their deadline", text)
        self.assertIn("generated frames MISSING: 54.0/s of 60.0/s expected were shown", text)
        self.assertIn("GPU does not finish a pair in time: span P95 35.0 ms of a 33.4 ms pair", text)

    def test_starts_of_an_earlier_build_are_listed_apart(self):
        lines = session_lines(0, False, ini="switches=[VEYRA_EXP_RP_STATS=1]") + session_lines(600, True)
        text = triage.triage([("log-PushFPS-slipstream-exp.txt", lines)])
        early = text.index("Sessions of an earlier build (no [slipstream-sizes] line: build #66 or older):")
        late = text.index("Sessions of build #70 or later")
        self.assertLess(early, late)                                                    # grouped by build, in the order first seen
        self.assertIn("program start 1, playback session 1", text[early:late])
        self.assertIn("program start 2, playback session 1", text[late:])

    def test_without_statistics_every_window_value_is_unknown_not_zero(self):
        lines = [l for l in session_lines(0, True) if "[slipstream-" not in l or "[slipstream-config]" in l or "[slipstream-sizes]" in l]
        text = triage.triage([("log-x.txt", lines)])
        self.assertIn("present intervals: not logged by this build or run (needs VEYRA_EXP_RP_STATS=1)", text)
        self.assertIn("GPU time per pair: not logged by this build or run", text)
        self.assertIn("frames shown: real 30.0/s", text)                                 # the always-on counters still answer

    def test_reads_a_diagnostics_zip_and_its_folder(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "pushfps-diagnostics.zip")
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("log-PushFPS-slipstream-exp.txt", "\n".join(session_lines(0, True)))
                archive.writestr("summary.txt", "not a log")
            inputs = triage.read_inputs(path)
            self.assertEqual([name for name, _lines in inputs], ["log-PushFPS-slipstream-exp.txt"])
            out = os.path.join(folder, "triage.md")
            self.assertEqual(triage.main([path, "--out", out]), 0)
            with open(out, encoding="utf-8") as handle: self.assertIn("Is it smooth, and if not, why:", handle.read())


    def test_steadiness_verdict_in_words(self):
        # Package 14's always-on [capture-timing] grid fields: one window a second.
        def timing(t, on, half, shown, robust=0, early=0, skipped=0, n=89):
            return line(t, "capture-timing", "revision=2 received=60 processed=30 gridIntervals=%d onGrid1msPct=%.2f onGridHalfPct=%.2f shownPct=%.2f "
                        "gridLongestMs=22.30 gridOffRunMax=2 robustAdmits=%d gridEarly=%d skippedPairs=%d (not HDMI-to-display latency)" % (n, on, half, shown, robust, early, skipped))
        steady = session_lines(0, True) + [timing(4 + k, 99.8, 100.0, 99.9, robust=1) for k in range(20)]
        text = triage.triage([("log-x.txt", steady)])
        self.assertIn("steady: 99.8 % on grid, 99.9 % shown", text)
        self.assertIn("Package 14 parts that acted: robustAdmit 20 pairs, gridEarly 0 pairs; pairs skipped 0", text)
        late = session_lines(0, True) + [timing(4 + k, 97.5, 99.0, 99.6) for k in range(20)]
        text = triage.triage([("log-x.txt", late)])
        self.assertIn("not steady (97.5 % on grid, 99.6 % shown), because frames were shown but 2.5 % of intervals were more than 1 ms off the grid", text)
        skipping = session_lines(0, True) + [timing(4 + k, 80.0, 85.0, 75.0, early=5, skipped=10) for k in range(20)]
        text = triage.triage([("log-x.txt", skipping)])
        self.assertIn("generation skipped 10.0 pairs a second", text)
        self.assertIn("only 75.0 % of the expected frames were shown", text)
        old = triage.triage([("log-x.txt", session_lines(0, True))])
        self.assertIn("steadiness (on the 11.1 ms grid): not logged by this build or run", old)


if __name__ == "__main__":
    unittest.main()
