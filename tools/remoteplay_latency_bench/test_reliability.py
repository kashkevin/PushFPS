#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Tests of reliability.py (pass/fail lines of the kit's reliability checks) on made-up runs.

  python3 -B tools/remoteplay_latency_bench/test_reliability.py

timeline() below is a copy of the made-up log that run-tests.ps1 -DryRun -Reliability writes (Add-DryRunTimeline):
the verdicts expected here are the ones the checks workflow expects from the PowerShell rules on Windows.
"""
import io, json, os, re, sys, tempfile, unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import reliability, report
from test_report import add_run, line, make, stamp, table

KIT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts", "remoteplay", "testkit", "run-tests.ps1")
STARTED = "slipstream.ini not read (VEYRA_TEST_RP_NO_SETTINGS_FILE is set)"
SESSION = "session started host=192.168.x.x profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000"
EXPECTED = {"cable-out-1s": "PASS", "cable-out-5s": "FAIL", "display-mode-change": "PASS", "display-reconnect": "NOT-EXERCISED", "focus-loss-keys-held": "SKIPPED", "crash-recovery": "PASS"}

def progress(second, fps=59.9): return line(second, "remoteplay-progress", f"state=3 received=600 decoded=600 inputFps={fps} decodeFps={fps} videoMbps=28.100 receiveAgeMs=7.0 decodeAgeMs=5.0 decodeBusyMs=1.2 queue=0 waitingIdr=false idrRequests=0 errors=0")
def frame_rate(second, fps="59.90"): return line(second, "frame-rate", f"revision=1 gpuCompletedFps=59.90 completedReal=60 capture60To30=false rateSkipped=0 presentSubmitFps={fps} windowMs=1000 vramMiB=1200 budgetMiB=11000 (not scanout FPS)")

EVENTS = {
    "reliability": [(43.5, "remoteplay-recovery", "request keyframe; received=900 decoded=880 videoCallbacks=900 rejected=0 packetReceived=4000 packetLost=37", "WARN "),
                    (85.5, "remoteplay-recovery", "old session joined; reconnect=1 using existing in-memory pairing", "WARN "),
                    (150.5, "remoteplay-recovery", "reconnected attempt=1 first decoded frame; reset video history and audio clock", "INFO "),
                    (195.5, "display-query", "invalidate reason=display-change generation=2 (stored states are stale; each monitor in use is queried once more)", "INFO "),
                    (196.5, "display-refresh", "monitor refresh=120.000 Hz (was 180.000; window moved or display mode changed)", "INFO "),
                    (205.5, "display-refresh", "monitor refresh=180.000 Hz (was 120.000; window moved or display mode changed)", "INFO ")],
    "gpuloss": [(20.25, "engine-test", "simulating a GPU frame-ready timeout; device removal after 500 ms", "WARN "), (20.75, "engine-test", "device removed", "WARN "),
                (21.25, "gpu-failure", "operation=frame-ready alive=false removedReason=0x887A0006 requestedFence=10 completedFence=9 sentinel=false adapter=MadeUpAdapter device=0x1 luid=1 driver=1", "ERROR"),
                (21.5, "gpu-reconnect", "session=1 kind=ps5 automatic=false", "WARN ")],
}

def timeline(purpose):
    """The lines of Add-DryRunTimeline, as the kit's redaction leaves them."""
    items = [(1.25, line(1.25, "remoteplay-settings", STARTED)), (2.25, line(2.25, "remoteplay", SESSION)), (2.5, line(2.5, "remoteplay-control", "start host=[name] restart=false background=false"))]
    if purpose == "reliability":
        for i in range(3, 500):
            if 371 <= i <= 391: continue
            items.append((i, progress(i, 0.0 if 41 <= i <= 43 or 81 <= i <= 150 else 59.9)))
            if i % 5 == 0: items.append((i + 0.1, frame_rate(i + 0.1)))
        items += [(375.5, line(375.5, "remoteplay-settings", STARTED)), (390.5, line(390.5, "remoteplay", SESSION))]
    else:
        for i in range(3, 170):
            if 21 <= i <= 42: continue
            items += [(i, progress(i)), (i + 0.1, frame_rate(i + 0.1))]
        items.append((41.5, line(41.5, "remoteplay", SESSION)))
    items += [(at, line(at, component, text, level)) for at, component, text, level in EVENTS[purpose]]
    return [text for _at, text in sorted(items, key=lambda item: item[0])]

def kit_steps():
    """[(purpose, name, watch, dryAt, action)] as run-tests.ps1 defines them."""
    with open(KIT, encoding="ascii") as f: script = f.read()
    body = script[script.index("function Get-Steps("):script.index("function Add-Marker(")]
    steps, purpose = [], ""
    for row in body.splitlines():
        m = re.search(r"\$Purpose -eq '(\w+)'", row)
        if m: purpose = m.group(1)
        m = re.search(r"New-Step '([a-z0-9-]+)' '.*' (\d+) (\d+) '(\w*)' '.*'$", row.strip())
        if m: steps.append((purpose, m.group(1), int(m.group(2)), int(m.group(3)), m.group(4)))
    return steps

def run_with(folder, purpose, lines, markers, meta=None, kit=None, index=95, arm="mod-all"):
    run = add_run(folder, arm, index, lines, purpose=purpose, presentmon=False)
    with open(os.path.join(run, "markers.csv"), "w") as f: f.write("utc,marker\n" + "".join(f"{stamp(t)},{name}\n" for t, name in markers))
    record = {"arm": arm, "purpose": purpose, "compare": "", "presentMonStartUtc": "", "switches": [], "kitSwitches": [], "limitSeconds": 0, "limitReached": False}
    record.update(meta or {})
    with open(os.path.join(run, "run.json"), "w") as f: json.dump(record, f)
    if kit is not None:
        with open(os.path.join(run, "reliability.csv"), "w") as f: f.write("check,result,detail\n" + "".join(f'{c},{r},"{d}"\n' for c, r, d in kit))
    return run

def verdicts(folder, purpose="reliability"):
    run = next(r for r in report.load(folder)["runs"] if r["purpose"] == purpose)
    return {item["check"]: (item["result"], item["detail"]) for item in reliability.evaluate(run)}

def steady(until=300, low=(), gone=()):
    """A session with a progress and a frame-rate line every second; low: seconds with no pictures; gone: seconds with no line."""
    out = [line(1, "remoteplay-settings", STARTED), line(2, "remoteplay", SESSION)]
    for i in range(3, until):
        if i in gone: continue
        out += [progress(i, 0.0 if i in low else 59.9), frame_rate(i + 0.1, "0.00" if i in low else "59.90")]
    return out


class DryRunTimeline(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name
    def tearDown(self): self.tmp.cleanup()

    def markers(self, purpose):
        out = []
        for p, name, watch, at, _action in kit_steps():
            if p == purpose: out += [(at, name), (at + watch, name + "-end")]
        return out

    def test_the_kit_defines_the_steps_this_file_expects(self):
        steps = kit_steps()
        self.assertEqual([name for p, name, *_ in steps if p == "reliability"], list(EXPECTED))
        self.assertEqual(sorted(set((name, watch, action) for p, name, watch, _at, action in steps if p == "gpuloss")), [("gpu-device-loss", 150, "auto")])   # two texts: with and without the resume switch
        self.assertEqual([name for p, name, *_ in steps if p == "impairment"], ["cable-out-1s", "cable-out-5s", "second-device-load-start", "second-device-load-end"])
        self.assertEqual([action for p, name, _w, _a, action in steps if name == "crash-recovery"], ["kill"])
        for _p, name, *_ in steps: self.assertIn(name, reliability.ORDER + ("second-device-load-start", "second-device-load-end"))

    def test_this_file_has_the_same_timeline_as_the_kit(self):
        with open(KIT, encoding="ascii") as f: script = f.read()
        body = script[script.index("function Add-DryRunTimeline("):script.index("function Add-DryRunLog(")]
        planted = sorted((float(m.group(1)), m.group(2)) for m in re.finditer(r"New-DryLine ([0-9.]+) '\[(?:INFO |WARN |ERROR)\] (\[[a-z-]+\] [^']*)'", body))
        mine = sorted((at, f"[{component}] {text}") for events in EVENTS.values() for at, component, text, _level in events)
        self.assertEqual([p for p in planted if p[0] > 3], mine)
        for wanted in ("for ($i = 3; $i -lt 500; $i++)", "if (($i -ge 371) -and ($i -le 391)) { continue }", "if ((($i -ge 41) -and ($i -le 43)) -or (($i -ge 81) -and ($i -le 150))) { $fps = '0.0' }",
                       "for ($i = 3; $i -lt 170; $i++)", "if (($i -ge 21) -and ($i -le 42)) { continue }", "New-DryLine 375.5 $started", "New-DryLine 390.5 $session", "New-DryLine 41.5 $session", "if (($i % 5) -eq 0)"):
            self.assertIn(wanted, body)

    def test_expected_verdicts_of_the_dry_run(self):
        run_with(self.folder, "reliability", timeline("reliability"), self.markers("reliability"))
        found = verdicts(self.folder)
        self.assertEqual({check: result for check, (result, _d) in found.items()}, EXPECTED)
        self.assertIn("back after 4 s (limit 10 s)", found["cable-out-1s"][1]); self.assertIn("back after 71 s (limit 60 s)", found["cable-out-5s"][1])
        self.assertIn("started again 6 s after it was ended, pictures decoded 22 s after", found["crash-recovery"][1])
        other = tempfile.TemporaryDirectory()
        try:
            run_with(other.name, "gpuloss", timeline("gpuloss"), self.markers("gpuloss"))
            result, detail = verdicts(other.name, "gpuloss")["gpu-device-loss"]
            self.assertEqual(result, "PASS"); self.assertIn("picture back 23 s after the device was lost", detail); self.assertIn("does not reconnect a PS5 session by itself", detail)
        finally: other.cleanup()


class Rules(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name
    def tearDown(self): self.tmp.cleanup()

    def one(self, lines, check="cable-out-1s", start=100, length=30, purpose="reliability", extra_markers=(), meta=None):
        run_with(self.folder, purpose, lines, [(start, check), (start + length, check + "-end")] + list(extra_markers), meta=meta)
        return verdicts(self.folder, purpose)[check]

    def test_stream_recovery_within_and_beyond_the_limit(self):
        self.assertEqual(self.one(steady(low=range(101, 104)))[0], "PASS")
    def test_stream_recovery_too_slow(self):
        result, detail = self.one(steady(low=range(101, 115)))
        self.assertEqual(result, "FAIL"); self.assertIn("back after 15 s (limit 10 s)", detail)
    def test_stream_never_back(self):
        self.assertEqual(self.one(steady(low=range(101, 300))), ("FAIL", "the stream was not back to 54 pictures per second when the step ended"))
    def test_stream_not_disturbed(self):
        self.assertEqual(self.one(steady())[0], "NOT-EXERCISED")
    def test_no_stream_before_the_step(self):
        self.assertEqual(self.one(steady(low=range(60, 300))), ("NOT-EXERCISED", "no steady stream in the 30 s before the step"))
    def test_recovery_given_up_or_program_gone(self):
        lines = steady(low=range(101, 104)) + [line(104.5, "remoteplay-recovery", "recovery stopped attempts=3 quitReason=1 error=5", "ERROR")]
        self.assertEqual(self.one(lines), ("FAIL", "the program gave up reconnecting"))
    def test_program_restart_or_crash_during_a_cable_step_fails(self):
        self.assertEqual(self.one(steady(low=range(101, 104)) + [line(110.5, "remoteplay-settings", STARTED)])[0], "FAIL")
    def test_crash_line_during_a_cable_step_fails(self):
        self.assertEqual(self.one(steady(low=range(101, 104)) + [line(102.5, "crash", "unhandled exception code=0xC0000005 address=0x1 module=[path] offset=0x1 thread=5", "ERROR")]), ("FAIL", "a crash line"))
    def test_a_keyframe_request_alone_counts_as_disturbance_and_a_renewal_does_not(self):
        self.assertEqual(self.one(steady() + [line(101.5, "remoteplay-recovery", "request keyframe; received=1 decoded=1", "WARN ")])[0], "PASS")
    def test_renewal_line_is_not_a_disturbance(self):
        self.assertEqual(self.one(steady() + [line(101.5, "remoteplay-recovery", "retry allowance renewed after 30s continuous decoded progress; lifetimeAttempts=1")])[0], "NOT-EXERCISED")
    def test_five_second_limit_is_sixty(self):
        result, detail = self.one(steady(low=range(101, 150)), check="cable-out-5s", length=75)
        self.assertEqual(result, "PASS"); self.assertIn("back after 50 s (limit 60 s)", detail)

    def test_display_change_survived(self):
        lines = steady() + [line(105.5, "display-refresh", "monitor refresh=120.000 Hz (was 180.000; window moved or display mode changed)")]
        self.assertEqual(self.one(lines, check="display-mode-change", length=60)[0], "PASS")
    def test_display_change_not_seen(self):
        lines = steady() + [line(105.5, "display-query", "query reason=session monitor=0x1 generation=2 force=false (background thread; nothing waits for it)")]
        self.assertEqual(self.one(lines, check="display-reconnect", length=60)[0], "NOT-EXERCISED")
    def test_display_change_with_gpu_failure_or_dead_stream_fails(self):
        base = [line(105.5, "display-query", "invalidate reason=display-change generation=2 (stored states are stale)")]
        self.assertEqual(self.one(steady() + base + [line(106.5, "gpu-failure", "operation=present alive=false removedReason=0x887A0006", "ERROR")], check="display-reconnect", length=60), ("FAIL", "a GPU failure line"))
    def test_display_change_after_which_the_stream_stays_down_fails(self):
        base = [line(105.5, "display-query", "invalidate reason=display-change generation=2 (stored states are stale)")]
        self.assertEqual(self.one(steady(low=range(106, 300)) + base, check="display-reconnect", length=60), ("FAIL", "the stream was not back when the step ended"))
    def test_display_change_without_a_stream_checks_survival_only(self):
        lines = [line(1, "remoteplay-settings", STARTED), line(105.5, "display-refresh", "monitor refresh=120.000 Hz (was 180.000)")]
        result, detail = self.one(lines, check="display-mode-change", length=60)
        self.assertEqual(result, "PASS"); self.assertIn("only survival was checked", detail)

    def kbm(self, released=True): return steady() + [line(50, "remoteplay-kbm", "keyboard and mouse captured (Ctrl+Alt+Shift+Z releases)")] + \
        ([line(110, "remoteplay-kbm", "keyboard and mouse released (focus lost); raw input: messages=10 reports=10 largestBacklog=1 bufferedReads=0 absoluteIgnored=0")] if released else [])
    def test_focus_loss_releases_every_key(self):
        self.assertEqual(self.one(self.kbm(), check="focus-loss-keys-held", extra_markers=[(131, "focus-loss-keys-held-answer-yes")]), ("PASS", "every key was released when the window lost focus"))
    def test_focus_loss_log_and_user_disagree(self):
        self.assertEqual(self.one(self.kbm(), check="focus-loss-keys-held", extra_markers=[(131, "focus-loss-keys-held-answer-no")])[0], "FAIL")
    def test_focus_not_lost(self):
        self.assertEqual(self.one(self.kbm(False), check="focus-loss-keys-held")[0], "NOT-EXERCISED")
    def test_focus_not_released_and_user_saw_stuck_input(self):
        self.assertEqual(self.one(self.kbm(False), check="focus-loss-keys-held", extra_markers=[(131, "focus-loss-keys-held-answer-no")])[0], "FAIL")
    def test_focus_check_without_keyboard_and_mouse_mode(self):
        self.assertEqual(self.one(steady(), check="focus-loss-keys-held")[0], "SKIPPED")

    def test_crash_recovery_needs_a_new_start_and_a_stream(self):
        gone = range(101, 130)
        self.assertEqual(self.one(steady(gone=gone) + [line(105.5, "remoteplay-settings", STARTED), line(120.5, "remoteplay", SESSION)], check="crash-recovery", length=120)[0], "PASS")
    def test_crash_recovery_without_a_new_start(self):
        self.assertEqual(self.one(steady(gone=range(101, 300)), check="crash-recovery", length=120), ("FAIL", "PushFPS did not start again after it was ended"))
    def test_crash_recovery_without_a_connection(self):
        self.assertEqual(self.one(steady(gone=range(101, 300)) + [line(105.5, "remoteplay-settings", STARTED)], check="crash-recovery", length=120)[0], "NOT-EXERCISED")
    def test_crash_recovery_with_a_crash_at_the_new_start(self):
        lines = steady(gone=range(101, 300)) + [line(105.5, "remoteplay-settings", STARTED), line(107.5, "crash", "unhandled exception code=0xC0000005", "ERROR")]
        self.assertEqual(self.one(lines, check="crash-recovery", length=120), ("FAIL", "a crash line after the new start"))
    def test_crash_recovery_connected_without_pictures(self):
        lines = steady(gone=range(101, 300)) + [line(105.5, "remoteplay-settings", STARTED), line(120.5, "remoteplay", SESSION)]
        self.assertEqual(self.one(lines, check="crash-recovery", length=120)[0], "FAIL")

    def test_gpu_loss_survived_without_a_picture_fails_and_says_why(self):
        lines = steady(gone=range(21, 300)) + [line(20.25, "engine-test", "simulating a GPU frame-ready timeout; device removal after 500 ms", "WARN "), line(21.5, "gpu-reconnect", "session=1 kind=ps5 automatic=false", "WARN ")]
        result, detail = self.one(lines, check="gpu-device-loss", start=0, length=150, purpose="gpuloss")
        self.assertEqual(result, "FAIL"); self.assertIn("not reconnected automatically", detail)
    def test_gpu_loss_not_triggered(self):
        self.assertEqual(self.one(steady(), check="gpu-device-loss", start=0, length=150, purpose="gpuloss")[0], "NOT-EXERCISED")
    def test_gpu_loss_followed_by_a_restart_of_the_program_fails(self):
        lines = steady() + [line(21.25, "gpu-failure", "operation=frame-ready alive=false", "ERROR"), line(30.5, "remoteplay-settings", STARTED)]
        self.assertEqual(self.one(lines, check="gpu-device-loss", start=0, length=150, purpose="gpuloss")[0], "FAIL")
    def test_gpu_loss_with_automatic_reconnect_is_named(self):
        lines = steady(gone=range(21, 30)) + [line(21.25, "gpu-failure", "operation=frame-ready alive=false", "ERROR"), line(21.5, "gpu-reconnect", "session=1 kind=capture automatic=true", "WARN ")]
        result, detail = self.one(lines, check="gpu-device-loss", start=0, length=150, purpose="gpuloss")
        self.assertEqual(result, "PASS"); self.assertIn("reconnected by the program", detail)

    def test_gpu_loss_resume_by_itself_with_the_hook_ending_at_the_budget_passes(self):
        # VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1 with the test hook: attempts 1 to 3, then reason=budget (the expected end)
        lines = steady(gone=range(21, 40)) + [line(21.25, "gpu-failure", "operation=frame-ready alive=false", "ERROR"),
                 line(22.0, "remoteplay-recovery", "device-loss resume attempt=1 waiting delayMs=500, then for a usable device (at most 3 in 60 s)", "WARN "),
                 line(24.0, "remoteplay-recovery", "device-loss resume attempt=1 started: same console and settings, new device and session", "WARN "),
                 line(120.0, "remoteplay-recovery", "device-loss not resumed reason=budget", "WARN ")]
        result, detail = self.one(lines, check="gpu-device-loss", start=0, length=150, purpose="gpuloss")
        self.assertEqual(result, "PASS"); self.assertIn("resumed by itself (attempt 1)", detail); self.assertIn("expected end with the test hook", detail)
    def test_gpu_loss_resume_cancelled_by_a_connection_by_hand_says_so(self):
        lines = steady(gone=range(21, 300)) + [line(21.25, "gpu-failure", "operation=frame-ready alive=false", "ERROR"),
                 line(30.0, "remoteplay-recovery", "device-loss not resumed reason=other-source", "WARN ")]
        result, detail = self.one(lines, check="gpu-device-loss", start=0, length=150, purpose="gpuloss")
        self.assertEqual(result, "FAIL"); self.assertIn("cancelled the automatic resume", detail)

    def test_safe_start(self):
        ok = {"limitSeconds": 20, "limitReached": True, "forcedStop": False}
        self.assertEqual(self.one([line(1, "remoteplay-settings", STARTED)], check="safe-start", purpose="safestart", meta=ok)[0], "PASS")
    def test_safe_start_that_ended_early_or_had_to_be_ended(self):
        self.assertEqual(self.one([line(1, "remoteplay-settings", STARTED)], check="safe-start", purpose="safestart", meta={"limitSeconds": 20, "limitReached": False})[0], "FAIL")
    def test_safe_start_forced(self):
        self.assertEqual(self.one([line(1, "remoteplay-settings", STARTED)], check="safe-start", purpose="safestart", meta={"limitSeconds": 20, "limitReached": True, "forcedStop": True})[0], "FAIL")
    def test_safe_start_with_an_experiment_or_without_a_start_line(self):
        lines = [line(1, "remoteplay-settings", STARTED), line(5, "remoteplay-lan", "experiment requested: reorder=2000us (off by default; unverified on a PS5)")]
        result, detail = self.one(lines, check="safe-start", purpose="safestart", meta={"limitSeconds": 20, "limitReached": True})
        self.assertEqual(result, "FAIL"); self.assertIn("experiment", detail)
    def test_safe_start_without_a_start_line(self):
        self.assertEqual(self.one([line(1, "remoteplay-timing", "ownerWait=event input=immediate engineWake=event")], check="safe-start", purpose="safestart", meta={"limitSeconds": 20, "limitReached": True}), ("FAIL", "no start line of the program in the log"))

    def test_a_step_without_its_end_marker(self):
        run_with(self.folder, "reliability", steady(), [(100, "cable-out-1s")])
        self.assertEqual(verdicts(self.folder)["cable-out-1s"][0], "NOT-EXERCISED")


class ReportAndCommandLine(unittest.TestCase):
    def test_report_section_agreement_with_the_kit_and_exit_codes(self):
        with tempfile.TemporaryDirectory() as folder:
            markers = [(100, "cable-out-1s"), (130, "cable-out-1s-end"), (140, "cable-out-5s"), (215, "cable-out-5s-end")]
            run_with(folder, "reliability", steady(low=list(range(101, 104)) + list(range(141, 215))), markers, kit=[("cable-out-1s", "PASS", "back after 4 s (limit 10 s)"), ("cable-out-5s", "PASS", "made up, to disagree")])
            run_with(folder, "safestart", [line(1, "remoteplay-settings", STARTED)], [], meta={"limitSeconds": 20, "limitReached": True}, kit=[("safe-start", "PASS", "x")], index=96, arm="safe-start")
            md, _html, notes, leaks = make(folder)
            part = md.split("## Reliability checks\n", 1)[1].split("\n## ", 1)[0]
            rows = {l.split("|")[2].strip(): [c.strip() for c in l.split("|")] for l in part.splitlines() if l.startswith("| ") and "Check" not in l}
            self.assertEqual(rows["cable-out-1s"][3], "PASS"); self.assertEqual(rows["cable-out-1s"][6], "yes")
            self.assertEqual(rows["cable-out-5s"][3], "FAIL"); self.assertEqual(rows["cable-out-5s"][6], "NO"); self.assertEqual(rows["safe-start"][3], "PASS")
            self.assertTrue(any(n.startswith("Reliability checks: 1 FAILED (cable-out-5s); 2 passed") for n in notes)); self.assertTrue(any("DISAGREE on: cable-out-5s" in n for n in notes))
            self.assertEqual(leaks, []); self.assertNotIn("latency", part.lower().replace("nothing here measures latency", ""))
            software = table(md, "Software latency (built-in test; software, not input-to-photon)")
            self.assertEqual(software, {})                                                  # reliability runs are not latency arms
            out = io.StringIO()
            with redirect_stdout(out): code = reliability.main([folder])
            self.assertEqual(code, 1); self.assertRegex(out.getvalue(), r"FAIL\s+cable-out-5s"); self.assertIn("the kit said: PASS", out.getvalue())
        with tempfile.TemporaryDirectory() as folder:
            run_with(folder, "safestart", [line(1, "remoteplay-settings", STARTED)], [], meta={"limitSeconds": 20, "limitReached": True}, arm="safe-start")
            with redirect_stdout(io.StringIO()): self.assertEqual(reliability.main([folder]), 0)
        with tempfile.TemporaryDirectory() as folder:
            with redirect_stdout(io.StringIO()): self.assertEqual(reliability.main([folder]), 3)


if __name__ == "__main__":
    unittest.main(verbosity=1)
