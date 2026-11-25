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
        self.assertNotIn("Lower delay", text, "nothing about Package 15 when it was off")

    def test_low_delay_fields(self):
        # Package 15: the fields appended to [capture-timing] while "Lower delay" is on.
        def timing(t, shown, pairs, switches, unusable, state="active"):
            return line(t, "capture-timing", "revision=2 received=60 processed=30 gridIntervals=89 onGrid1msPct=99.80 onGridHalfPct=100.00 shownPct=99.90 "
                        "gridLongestMs=22.30 gridOffRunMax=1 robustAdmits=0 gridEarly=0 skippedPairs=0 (not HDMI-to-display latency) lowDelay=%s "
                        "predictedShown=%d predictedPairs=%d lowDelaySwitches=%d lowDelayUnusable=%d lowDelayExits=off:0,ineligible:0,refused:1,unusable:2,skipped:0,held:0 "
                        "(Package 15, this window)" % (state, shown, pairs, switches, unusable))
        on = session_lines(0, True) + [timing(4 + k, 30, 30, 1 if k == 5 else 0, 1 if k == 5 else 0) for k in range(20)]
        text = triage.triage([("log-x.txt", on)])
        self.assertIn("Lower delay (one predicted frame per real frame) was on: last state active; predicted frames shown 600 (30.0/s), "
                      "pairs run predicted 600, schedule switches 1 (3.0 a minute), predicted frames found mostly held 1; "
                      "fallbacks by reason (session): refused 1, unusable 2", text)
        self.assertIn("steady: 99.8 % on grid, 99.9 % shown", text)
        self.assertIn("delay split: not in this log", text.replace(triage.NOT_LOGGED, "not in this log"))
        # Package 16 (review 1 SHOULD 7): the [fg-sched] lowDelay line's VFG mode, motion source and stalls ridden through.
        sched = line(30, "fg-sched", "lowDelay state=active refusal=ok paused=none predictedPairs=600 lastPredictP95Ms=0.41 stallRides=2 "
                     "flowSource=fidelityfx aloneMode=timestep (one predicted frame per real frame: PushLowDelay.h)")
        text = triage.triage([("log-x.txt", on + [sched])])
        self.assertIn("Lower delay details (last [fg-sched] line): VFG FrameIndex-2-only mode timestep; motion source fidelityfx; "
                      "capture stalls ridden through 2", text)

    def test_delay_split_fields(self):
        # Package 15 TASK 3: the readout and its four parts, always on in [capture-timing].
        def timing(t, total, picture, first, wait, margin):
            return line(t, "capture-timing", "revision=2 received=60 processed=30 gridIntervals=89 onGrid1msPct=99.80 onGridHalfPct=100.00 shownPct=99.90 "
                        "gridLongestMs=22.30 gridOffRunMax=1 robustAdmits=0 gridEarly=0 skippedPairs=0 (not HDMI-to-display latency) "
                        "delayMs=%.2f delayPictureMs=%.2f delayFirstGeneratedMs=%.2f delayWaitMs=%.2f delayMarginMs=%.2f delaySplitPct=100 "
                        "(the readout: capture callback -> real frame's Present returned, 1 s mean)" % (total, picture, first, wait, margin))
        lines = session_lines(0, True) + [timing(4 + k, 31.0, 2.0, 7.0, 22.2, -0.2 if k % 2 else 0.2) for k in range(10)]
        text = triage.triage([("log-x.txt", lines)])
        self.assertIn("delay readout 31.0 ms = 2.0 picture + 7.0 first generated frame + 22.2 waiting for the frames shown before the real one "
                      "+ 0.0 margin (session mean of 10 one-second readings; 100 % of real frames split", text)

    def test_fg_health_and_present_path(self):
        # Stream "present": [fg-health] every 10 s and [present-path] on change.
        def health(t, p99, overruns=0):
            return line(t, "fg-health", "windowS=10.0 realFrames=300 realMeanMs=30.10 realP50Ms=30.00 realP95Ms=31.00 realP99Ms=%.2f realMaxMs=40.00 presents=900 "
                        "generatedPresents=600 intervalP50Ms=11.11 intervalP95Ms=11.30 intervalP99Ms=11.60 intervalMaxMs=12.10 onGrid1msPct=99.50 shownPct=100.00 "
                        "gridLongestMs=12.10 gridOffRunMax=1 dropDeadline=1 dropCapacity=0 dropCap=0 suppressed=0 mailboxDropped=2 duplicates=300 "
                        "gpu_colorP50Ms=0.400 gpu_colorP95Ms=0.450 gpu_colorP99Ms=0.500 gpu_fg1P50Ms=6.500 gpu_fg1P95Ms=7.200 gpu_fg1P99Ms=7.900 "
                        "gpu_fg2P50Ms=-1 gpu_fg2P95Ms=-1 gpu_fg2P99Ms=-1 gpu_fgBatchP50Ms=13.000 gpu_fgBatchP95Ms=14.000 gpu_fgBatchP99Ms=15.000 "
                        "gpu_blitP50Ms=0.110 gpu_blitP95Ms=0.120 gpu_blitP99Ms=0.130 reflex=1 reflexSleeps=600 reflexSleepMeanMs=0.300 reflexSleepMaxMs=2.000 "
                        "reflexOverrunMeanMs=0.010 reflexOverrunP95Ms=0.000 reflexOverrunP99Ms=0.400 reflexOverrunMaxMs=1.500 reflexOverruns=%d (software timing, not photons)" % (p99, overruns))
        def path(t, region, items):
            return line(t, "present-path", "region=%s coversInPicture=1 cineBand=0 items=%s inPictureOverlay=0 overlayRollback=0 swapEffect=4 buffers=3 format=28 "
                        "chainFlags=0xA40 waitable=1 maxLatency=1 syncInterval=1 tearingFlag=0 tearingSupported=1 vrrSync=1 vrrLevel=4 vrrRequested=1 vrrActive=1 "
                        "displayInVrrMode=1 cadenceVrr=1 hwCompositionFlags=7 window=2560x1440 buffer=2560x1440 coversMonitor=1 foreground=1 xess=0 hdrCopy=0 "
                        "reflexActive=1 reflexEnded=0 reflexGuard=on driverStatus=0 driverLowLatency=1 driverFsVrr=1 driverFullscreenIFlip=1 driverGameSleep=1 "
                        "driverCplVsync=0 (on change)" % (region, items))
        lines = session_lines(0, True) + [path(4, "complex", "PushLatencyBadge/pushfps-latency-badge"), path(5, "full", "none")] + \
            [health(14 + 10 * k, 33.0 + k, overruns=k) for k in range(3)]
        text = triage.triage([("log-x.txt", lines)])
        self.assertIn("real frame's delay over 3 windows of 10 s (median of the windows): mean 30.10 ms, P50 30.00 ms, P95 31.00 ms, P99 34.00 ms; worst 40.00 ms", text)
        self.assertIn("dropped: deadline 3, capacity 0, output cap 0, suppressed 0, capture mailbox 6; duplicates removed 900", text)
        self.assertIn("GPU: first generated frame P50 6.50 ms / P95 7.20 ms / P99 7.90 ms", text)
        self.assertIn("ran past the sample's arrival 3 times", text)
        self.assertIn("display path (last of 2 [present-path] lines): region full, items over the picture none", text)
        self.assertIn("driver fullscreen/iFlip 1", text)
        self.assertIn("region was COMPLEX at some point (cut out for: PushLatencyBadge/pushfps-latency-badge)", text)
        trip = lines + [line(20.5, "reflex", "sleep overrun: in two consecutive blocks of the last 120 measured sleeps at least 4 per block ended more than 0.5 ms after the capture sample had arrived", "WARN"),
                        line(21.5, "present-overlay", "blended pipeline format=28 hr=0x80070057", "ERROR")]
        text = triage.triage([("log-x.txt", trip)])
        self.assertIn("NVIDIA low latency ENDED by the overrun guard: sleep overrun", text)
        self.assertIn("in-picture readouts FAILED 1 time(s)", text)
        old = triage.triage([("log-x.txt", session_lines(0, True))])
        self.assertIn("[fg-health]: not logged by this build or run", old)
        self.assertIn("[present-path]: not logged by this build or run", old)

    def test_fg_health_display_path_fields(self):
        # Stream "s10-display": the to-screen figure, the phase bins, late Presents and the completion polls.
        def health(t):
            return line(t, "fg-health", "windowS=10.0 realFrames=300 realMeanMs=30.10 realP50Ms=30.00 realP95Ms=31.00 realP99Ms=32.00 realMaxMs=40.00 "
                        "intervalP50Ms=11.11 intervalP99Ms=12.80 onGrid1msPct=95.20 shownPct=99.90 reflexSleeps=0 "
                        "toScreenSamples=600 toScreenP50Ms=9.40 toScreenP95Ms=14.50 toScreenP99Ms=17.00 toScreenMaxMs=21.00 "
                        "toScreenRefreshesByMsToVblank=0-0.5:2.000/10,0.5-1:1.000/10,1-1.5:-1/0,1.5-2:-1/0,2-2.5:-1/0,2.5-3:-1/0,3-3.5:-1/0,3.5+:1.000/20 "
                        "presentLateGenP50Ms=0.000 presentLateGenP99Ms=1.600 presentLateGenMaxMs=2.700 presentLateRealP50Ms=0.000 presentLateRealP99Ms=0.100 "
                        "presentLateRealMaxMs=0.300 presentsLateOver1ms=20 readyWakesNearDeadline=25 readyWakesHeld=0 presentsAfterReadyWake=25 "
                        "lateOver1msAfterReadyWake=18 lateOver1msOtherwise=2 polls=2000 pollP50Ms=0.200 pollP99Ms=1.900 pollMaxMs=3.100 pollsPastDeadline=19 "
                        "pollOverrunP99Ms=2.000 pollOverrunMaxMs=2.600 (software timing, not photons)")
        lines = session_lines(0, True) + [health(14), health(24)]
        text = triage.triage([("log-x.txt", lines)])
        self.assertIn("to screen (Present call -> the vblank that showed it, DXGI frame statistics; NOT in the delay above): P50 9.40 ms, P95 14.50 ms", text)
        self.assertIn("0-0.5 ms 2.00 (20), 0.5-1 ms 1.00 (20)", text)
        self.assertIn("late Presents (> 1 ms after the deadline): 40; generated P99 1.60 ms / worst 2.70 ms", text)
        self.assertIn("completion polls: 4000, P50 0.200 ms, P99 1.900 ms, worst 3.100 ms; 38 ran past a due deadline (worst overrun 2.600 ms); late Presents after a ready wake 36 of 50, otherwise 4", text)

    def test_allow_lists_take_the_new_lines(self):
        import report
        for component in ("fg-health", "present-path", "reflex", "present-overlay", "vrr-profile", "scheduler", "capture-stall"):
            self.assertTrue(report.ALLOWED.search("[INFO ] [" + component + "] x=1"), component)
        self.assertTrue(report.ALLOWED.search("[WARN ] [reflex] sleep overrun: in two consecutive blocks"))
        self.assertFalse(report.ALLOWED.search("[INFO ] [reflex-marker] source=1 type=0 status=0"))
        for name in ("scripts/remoteplay/testkit/run-tests.ps1", "scripts/remoteplay/testkit/collect-diagnostics.ps1"):
            with open(os.path.join(os.path.dirname(__file__), "..", "..", name), encoding="utf-8") as f:
                self.assertIn("'" + report.ALLOWED.pattern + "'", f.read(), name)


if __name__ == "__main__":
    unittest.main()
