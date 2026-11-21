#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Kit version 5 (Package 8): the MEASURED VALUES section, its rules and the measured sessions.

  python3 -B tools/remoteplay_latency_bench/test_kit5.py

Every rule of report.py that run-tests.ps1 mirrors (Get-MeasuredTables, Get-MeasureCell, Get-SpreadVerdict,
Get-VisualVerdict, Get-PlanSeconds) is tested here; the texts both write are held equal by reading the script. The
dry run of the kit cannot run here (no PowerShell): `dry_run_folder` writes the same made-up log lines Add-DryRunLog
writes for the measured sessions, and the texts ci/check-dry-run.ps1 expects in RESULT.txt are checked against this
report's section, which the Windows dry run then compares with the kit's own (DISAGREE).
"""
import csv, json, os, re, shutil, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import report

KIT = os.path.join(HERE, "..", "..", "scripts", "remoteplay", "testkit")
SPEC = os.path.join(KIT, "measures.csv")


def stamp(seconds):
    whole = int(seconds); ms = int(round((seconds - whole) * 1000))
    if ms == 1000: whole += 1; ms = 0
    return f"2026-11-20T12:{whole // 60:02d}:{whole % 60:02d}.{ms:03d}Z"

def line(seconds, component, text): return f"{stamp(seconds)} t=1 [INFO ] [{component}] {text}"

def run_of(lines, purpose="measure", samples=None):
    return {"name": "x@1", "arm": "x", "purpose": purpose, "meta": {}, "lines": lines, "samples": samples or [], "log": report.parse_log(lines)}

def row(component, field, stat, **more):
    base = {"group": "g", "sessions": "*", "label": "l", "component": component, "filter": "", "phase": "all", "field": field, "stat": stat,
            "digits": "2", "skipFirst": "0", "skipLast": "0", "unit": "", "compare": "0"}
    base.update(more)
    return base


class Spec(unittest.TestCase):
    def test_the_table_is_valid_and_every_value_is_unique(self):
        spec = report.read_measure_spec(SPEC)
        self.assertGreater(len(spec), 100)
        self.assertEqual(len({r["key"] for r in spec}), len(spec))
        known = set(report.MEASURE_SESSIONS) | {"*"}
        for r in spec:
            self.assertTrue(set(r["sessions"].split("|")) <= known, r["key"])
            self.assertIn(r["compare"], ("0", "1"), r["key"])
            self.assertNotIn(",", r["label"])
            if r["stat"] == "word": self.assertEqual(r["compare"], "0", r["key"])          # a word has no spread
        # every session of kit 5 shows at least the screen and the audio groups
        for session in report.MEASURE_SESSIONS:
            groups = {r["group"] for r in spec if r["sessions"] == "*" or session in r["sessions"].split("|")}
            self.assertTrue({"screen first 3 s", "screen steady", "audio", "in-app latency", "frame pacing", "frames", "gpu"} <= groups, session)
        self.assertIn("mouse and keyboard", {r["group"] for r in spec if "input" in r["sessions"].split("|")})

    def test_the_file_passes_the_kits_own_leak_check(self):
        """measures.csv is copied into the evidence folder and checked like arms.csv (Test-TableLeaks)."""
        with open(SPEC, encoding="ascii") as f:
            for text in f.read().splitlines():
                self.assertEqual(report.redact_line(text), text, text); self.assertIsNone(report.PRIVATE_WORD.search(text), text)

    def test_a_bad_table_is_refused(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "measures.csv")
            head = "group,sessions,label,component,filter,phase,field,stat,digits,skipFirst,skipLast,unit,compare\n"
            for bad in ("g,*,l,c,,all,f,mean,1,0,0,,0\n", "g,*,l,c,,later,f,median,1,0,0,,0\n", "g,*,l,samples,,all,f,rate-s,1,0,0,,0\n",
                        "g,*,l,c,,all,f,median,1,0,0,,0\ng,*,other,c,,all,f,median,1,0,0,,0\n"):
                with open(path, "w") as f: f.write(head + bad)
                with self.assertRaises(ValueError): report.read_measure_spec(path)


class Cells(unittest.TestCase):
    def lines(self):
        return [line(0.0, "frame-rate", "presentSubmitFps=60.0"),
                line(1.0, "c", "a=1.0 w=one k=x"), line(2.0, "c", "a=3.0 w=two k=y"), line(3.0, "c", "a=5.0 w=two k=x"),
                line(3.5, "c", "a=-1 w=two k=x"), line(5.0, "c", "a=9.0 w=one k=y"), line(61.0, "c", "a=4.0 w=one k=x")]

    def cell(self, **spec):
        return report.measure_cell(row("c", spec.pop("field", "a"), spec.pop("stat", "median"), **spec), [run_of(self.lines())])

    def test_statistics(self):
        self.assertEqual(self.cell()["value"], 4.0); self.assertEqual(self.cell()["n"], 5)            # -1 is 'unknown' and skipped
        self.assertEqual(self.cell(stat="max")["value"], 9.0)
        self.assertAlmostEqual(self.cell(stat="p01")["value"], 1.08)
        self.assertEqual(self.cell(stat="last")["value"], 4.0); self.assertEqual(self.cell(stat="last")["n"], 1)
        self.assertEqual(self.cell(stat="share-ge:4")["value"], 60.0); self.assertEqual(self.cell(stat="share-lt:4")["value"], 40.0)
        self.assertEqual(self.cell(stat="word", field="w")["text"], "one")                           # first seen wins a tie
        last = report.measure_cell(row("c", "w", "last-word"), [run_of(self.lines()), run_of(self.lines()[:3]), run_of(self.lines()[:4]), run_of([])])
        self.assertEqual((last["text"], last["n"], last["value"]), ("two", 3, None))                # each run's last line: one, two, two
        # a running counter: 1 -> 3 -> 5 -> 9 rises 8, then falls to 4 (a restart: 4 more) in 60 s
        self.assertAlmostEqual(self.cell(stat="rate-min")["value"], 12.0); self.assertAlmostEqual(self.cell(stat="rate-s")["value"], 0.2)
        self.assertAlmostEqual(self.cell(stat="slope-min")["value"], 3.0)
        self.assertEqual(self.cell(stat="median", unit="ms", digits="1")["text"], "4.0 ms")
        # first-lt:X: seconds from the first presented frame (0.0 s) to the first value under X
        self.assertEqual(self.cell(stat="first-lt:6", digits="1", unit="s")["text"], "1.0 s (in 1 of 1 runs)")
        self.assertEqual(self.cell(stat="first-lt:1", digits="1", unit="s")["text"], "never (0 of 1 runs)")
        self.assertEqual(self.cell(stat="first-lt:4.5", filter="k=y", digits="1")["value"], 2.0)

    def test_phase_filter_and_skips(self):
        # 'start' = within 3 s of the component's first line (1.0 s): lines at 1, 2, 3 and 3.5 s
        self.assertEqual(self.cell(phase="start")["n"], 3); self.assertEqual(self.cell(phase="start")["value"], 3.0)
        self.assertEqual(self.cell(phase="steady")["n"], 2); self.assertEqual(self.cell(phase="steady")["value"], 6.5)
        self.assertEqual(self.cell(filter="k=x")["n"], 3); self.assertEqual(self.cell(filter="k=x&w=two")["value"], 5.0)
        self.assertEqual(self.cell(skipFirst="1", skipLast="1")["value"], 5.0)
        self.assertEqual(self.cell(skipFirst="9")["text"], "n/a")
        # a line exactly 3.000 s after the first is 'start' in both readers (the difference is rounded to a millisecond)
        exact = [line(10.2, "c", "a=1"), line(13.2, "c", "a=2"), line(13.4, "c", "a=3")]
        self.assertEqual(report.measure_cell(row("c", "a", "median", phase="start"), [run_of(exact)])["n"], 2)

    def test_first_seconds_after_the_first_presented_frame(self):
        lines = [line(0.0, "x", "start"), line(2.0, "frame-rate", "presentSubmitFps=60.0"), line(3.0, "display-vrr", "requested=false active=false"),
                 line(6.5, "display-vrr", "requested=true active=true"), line(9.0, "display-vrr", "requested=true active=true")]
        spec = row("display-vrr", "", "first-s", filter="requested=true&active=true", digits="1", unit="s")
        self.assertEqual(report.measure_cell(spec, [run_of(lines)])["text"], "4.5 s (in 1 of 1 runs)")
        self.assertEqual(report.measure_cell(spec, [run_of(lines), run_of(lines[:3])])["text"], "4.5 s (in 1 of 2 runs)")
        never = report.measure_cell(spec, [run_of(lines[:3])])
        self.assertEqual((never["text"], never["n"], never["value"]), ("never (0 of 1 runs)", 0, None))

    def test_samples_and_words_with_commas(self):
        samples = [(1.0, {"gpuPercent": 40.0}), (2.0, {"gpuPercent": -1}), (3.0, {"gpuPercent": None}), (4.0, {"gpuPercent": 60.0})]
        cell = report.measure_cell(row("samples", "gpuPercent", "median", digits="0", unit="%"), [run_of([], samples=samples)])
        self.assertEqual((cell["text"], cell["n"]), ("50 %", 2))
        text = "applied=3 refused=1 detail=sr:ok,nr:refused:runtime-missing,hdr:ok"
        self.assertEqual(report.log_word(text, "detail"), "sr:ok,nr:refused:runtime-missing,hdr:ok")
        self.assertEqual(report.log_number("x mouseHz=1000 Hz=5", "Hz"), 5.0)                          # not after a letter


class SpreadRule(unittest.TestCase):
    def test_words(self):
        v = report.spread_verdict
        self.assertEqual(v([1.0, 1.2], [1.1, 1.5]), "same within run-to-run spread")
        self.assertEqual(v([1.0, 1.2], [1.2, 1.5]), "same within run-to-run spread")                  # touching ranges overlap
        self.assertEqual(v([1.0, 1.2], [3.0, 3.3]), "differs")                                        # gap 1.8 > 2 x 0.3
        self.assertEqual(v([1.0, 1.2], [1.5, 1.8]), "cannot tell")                                    # gap 0.3 is not more than 2 x 0.3
        self.assertEqual(v([30.0, 30.0], [60.0, 60.0]), "differs")
        self.assertEqual(v([30.0, 30.0], [30.0, 30.0]), "same within run-to-run spread")
        for few in (([1.0], [5.0, 6.0]), ([1.0, None], [5.0, 6.0]), ([], [])): self.assertEqual(v(*few), "cannot tell")
        self.assertIn("more than twice the larger range", report.SPREAD_RULE); self.assertIn("fewer than 2 runs", report.SPREAD_RULE)
        self.assertNotRegex(" ".join(report.SPREAD_WORDS), "better|worse")

    def test_the_chance_figure_is_a_model_and_about_right(self):
        """'about 1 value in 25' for identical arms with 2 runs each: reproduced here with normal noise (a model, not a measurement)."""
        import random
        rng = random.Random(5); hits = 0; rounds = 20000
        for _ in range(rounds):
            if report.spread_verdict([rng.gauss(0, 1), rng.gauss(0, 1)], [rng.gauss(0, 1), rng.gauss(0, 1)]) == "differs": hits += 1
        self.assertTrue(0.03 < hits / rounds < 0.05, hits / rounds)
        self.assertIn("a model of independent normal noise, not a measurement", report.SPREAD_RULE)


class PictureRule(unittest.TestCase):
    def test_hdr_and_sdr_are_never_compared(self):
        self.assertEqual(report.picture_of(["SDR", "SDR"]), "SDR"); self.assertEqual(report.picture_of(["HDR"]), "HDR")
        self.assertEqual(report.picture_of(["SDR", "HDR"]), "mixed"); self.assertEqual(report.picture_of([]), "not logged"); self.assertEqual(report.picture_of(["not logged", "not logged"]), "not logged")
        def measured(session, a, b): return {"session": session, "arms": {"x": {"picture": a}, "y": {"picture": b}}}
        self.assertEqual(report.picture_blocks(measured("visual", ["SDR", "SDR"], ["SDR", "SDR"]), "x", "y"), "")
        self.assertEqual(report.picture_blocks(measured("neural", ["HDR", "HDR"], ["HDR"]), "x", "y"), "")
        for a, b in ((["HDR", "HDR"], ["SDR", "SDR"]), (["SDR", "HDR"], ["SDR", "SDR"]), (["not logged"], ["not logged"]), (["SDR"], ["not logged"])):
            self.assertIn("never mixed", report.picture_blocks(measured("visual", a, b), "x", "y"))
        self.assertEqual(report.picture_blocks(measured("builds", ["not logged"], ["SDR"]), "x", "y"), "")          # only the picture sessions
        lines = [line(1.0, "slipstream-config", "mode=x hdr=0 route=wired"), line(9.0, "slipstream-config", "mode=x hdr=1 route=wired")]
        self.assertEqual(report.picture_states([run_of(lines), run_of(lines[:1]), run_of([])]), ["HDR", "SDR", "not logged"])
        # the stream's own HDR (streamHdr= of [slipstream-pipeline]) wins: hdr= of the config line also counts the HDR conversion
        pipe = lambda stream, video: line(5.0, "slipstream-pipeline", f"applied=1 refused=0 detail=hdr:ok streamHdr={stream} videoHdr={video}")
        self.assertEqual(report.picture_states([run_of(lines + [pipe(0, 1)]), run_of(lines[:1] + [pipe(1, 0)]), run_of([pipe("x", 0)] + lines[:1])]), ["SDR", "HDR", "SDR"])

    def test_the_reference_says_when_its_stream_was_not_hdr(self):
        def data(session, *words):
            runs = []
            for n, w in enumerate(words):
                lines = [line(1.0, "slipstream-config", "mode=x hdr=1 route=wired")] + ([line(2.0, "slipstream-pipeline", f"applied=1 refused=0 detail=hdr:ok streamHdr={w} videoHdr=1")] if w is not None else [])
                run = run_of(lines); run.update(name=f"measure-v-reference@{n}", arm="v-reference"); runs.append(run)
            return {"kit": {"session": session}, "runs": runs, "table": {"v-reference": {"compare": "v-original"}}}
        self.assertEqual(report.reference_lines(data("visual", "1", "1")), {})
        self.assertEqual(report.reference_lines(data("visual", "0", "0"))["v-reference"], report.REFERENCE_SDR + report.REFERENCE_HOW)
        self.assertTrue(report.reference_lines(data("visual", "1", "0"))["v-reference"].startswith("reference could not be reproduced: stream was SDR in 1 of 2 runs (the reference needs"))
        self.assertTrue(report.reference_lines(data("visual", "1", None))["v-reference"].startswith("reference could not be reproduced: the stream's HDR was not logged in 1 of 2 runs"))
        self.assertEqual(report.reference_lines(data("neural", "0")), {})

    def test_review_8a_hud_delta_and_blockiness_across_output_sizes(self):
        v = report.visual_verdict
        base = {"blockiness": (0.20, 2), "banding": (0.10, 2), "ringing": (0.05, 2), "flicker": (0.02, 2), "detail": (0.50, 2), "hudDelta": (0.0, 2)}
        arm = dict(base, hudDelta=(0.02, 2))
        # (i) HUD delta is about 0 in v-original: a processed arm must not be "worse (hudDelta higher)"
        self.assertNotIn("hudDelta", report.VISUAL_DEFECTS)
        self.assertTrue(v(arm, base, 5.0).startswith("cannot tell: the picture metrics differ by less than 10 %"))
        # (ii) a 1080 -> 1440 scaler blurs the 8-pixel grid: lower blockiness at another output size is no improvement
        upscaled = dict(arm, blockiness=(0.12, 2))
        self.assertEqual(v(upscaled, base, 5.0, ("2560x1440", "1920x1080")),
                         "cannot tell: the picture metrics differ by less than 10 % or in both directions; blockiness not comparable: different output size (2560x1440 / 1920x1080)")
        self.assertTrue(v(upscaled, base, 5.0, ("1920x1080", "1920x1080")).startswith("better on these software picture metrics (blockiness lower;"))
        self.assertIn("not comparable: different output size (n/a / 1920x1080)", v(upscaled, base, 5.0, ("n/a", "1920x1080")))
        self.assertFalse(v(dict(upscaled, blockiness=(0.40, 2)), base, 5.0, ("2560x1440", "1920x1080")).startswith("worse"))     # nor a worsening
        self.assertTrue(v(dict(upscaled, banding=(0.08, 2)), base, 5.0, ("2560x1440", "1920x1080")).startswith("better on these software picture metrics (banding lower;"))

    def test_a_neural_layer_that_did_not_run(self):
        for text in ("not installed: skipped (...)", "not usable on this PC: skipped (...)", "refused by the app: skipped (nr:refused:x)", "cannot tell whether the neural layer ran: ..."):
            self.assertTrue(report.neural_skipped(text), text)
        for text in ("neural runtime available: selected=nr-original", "neural layer applied per [slipstream-pipeline]; runtime state not logged"): self.assertFalse(report.neural_skipped(text), text)


class VisualRule(unittest.TestCase):
    BASE = {"blockiness": (0.2, 2), "banding": (0.1, 2), "ringing": (0.05, 2), "flicker": (0.02, 2), "hudDelta": (0.01, 2), "detail": (0.5, 2)}
    def arm(self, **changed):
        out = dict(self.BASE); out.update({k: (v, 2) for k, v in changed.items()})
        return out
    def test_words(self):
        v = report.visual_verdict
        self.assertTrue(v(self.arm(blockiness=0.12), self.BASE, 5.0).startswith("better on these software picture metrics (blockiness lower; processing P95 5.00 ms, within 8.33 ms)"))
        self.assertTrue(v(self.arm(blockiness=0.12), self.BASE, 9.5).startswith("cannot tell: cleaner on the picture metrics (blockiness lower) but processing P95 9.50 ms is over 8.33 ms"))
        self.assertTrue(v(self.arm(blockiness=0.12, ringing=0.08), self.BASE, 5.0).startswith("worse on these software picture metrics (ringing higher"))
        self.assertTrue(v(self.arm(blockiness=0.12, detail=0.44), self.BASE, 5.0).startswith("worse on these software picture metrics (detail lower"))
        self.assertTrue(v(self.arm(blockiness=0.12, detail=0.46), self.BASE, 5.0).startswith("cannot tell"))       # cleaner, but detail under 95 %
        self.assertTrue(v(self.arm(blockiness=0.19), self.BASE, 5.0).startswith("cannot tell"))                     # under 10 %
        self.assertTrue(v(self.arm(flicker=0.03), self.BASE, 5.0).startswith("worse"))
        self.assertTrue(v(self.arm(blockiness=0.12), self.BASE, None).startswith("no verdict: no processing time"))
        one_probe = dict(self.BASE); one_probe["banding"] = (0.1, 1)
        self.assertTrue(v(one_probe, self.BASE, 5.0).startswith("no verdict: fewer than 2 probes"))
        self.assertTrue(v({}, self.BASE, 5.0).startswith("no verdict: fewer than 2 probes"))
        for text in ("at least 10 % lower", "95 % or more", "8.33 ms", "below 90 %", "fewer than 2 probes"): self.assertIn(text, report.VISUAL_RULE)
        self.assertAlmostEqual(report.PROCESSING_BUDGET_MS, 8.3333, places=3)


# --- the made-up logs of the kit's dry run for the measured sessions (run-tests.ps1 Add-DryRunLog) ----------------------------
def dry_lines(arm, exe, switches, wifi=False):
    made = [("x", "filler")] * 19
    made += [("display-vrr", "level=2 known=true monitorSupportsVrr=true currentlyCapable=true trueGsync=false capsKnown=true possible=true requested=true active=true displayInVrrMode=true indicator=false simulated=false status=0 refreshHz=180.000 (made up)"),
             ("display-latency", "presentToScanoutP50Ms=2.100 presentToScanoutP95Ms=4.800 presentToScanoutMaxMs=6.000 samples=180 replacedAtLeast=0 compositionMode=2 refreshHz=180.000 (made up)"),
             ("frame-rate", "revision=1 gpuCompletedFps=59.90 completedReal=60 capture60To30=false rateSkipped=0 presentSubmitFps=59.90 windowMs=1000 vramMiB=1200 budgetMiB=11000 (not scanout FPS)")]
    flow = ("frame-flow", "state=active session=1 revision=1 epoch=1 source=600 batch=600 captureReceived=600 mailboxOverwritten=0 sourceAccepted=600 realSubmitted=600 fgEvaluated=0 fgReadyValid=0 realPresented=600 generatedPresented=0 generatedExpiredAfterEval=0 cancelledBeforePresent=0 presentSubmitFps=59.90")
    made += [flow, ("remoteplay-health", "route=direct-lan link=" + ("wifi" if wifi else "ethernet") + " linkMbps=1000 (the side of this PC only)")] + [("x", "filler")] * 7
    made += [("pacing", "revision=2 requested=true/0/1 effective=true/0/1 displayQueuePaced=true variableRefresh=true vrrLevel=2 vrrSync=false"), ("pacing", "DXGI enabled=true maximumLatency=1 vsync=true tearing=false hr=0x0 staleLatencyCounts=0")]
    made += [("frame-rate", "revision=1 gpuCompletedFps=59.90 completedReal=60 capture60To30=false rateSkipped=0 presentSubmitFps=59.90 windowMs=1000 vramMiB=1200 budgetMiB=11000 (not scanout FPS)")] * 12
    if exe in ("experimental", "previous"):
        made += [("slipstream-config", "mode=ultra-low-latency fg=0 fgMethod=0 fgMultiplier=2 fgTier=1 sr=0 srQuality=0 enhance=0 lowQueue=1 nvLowLatency=1 displaySync=1 smoothPacing=1 vrrRequested=1 vrrLevel=2 fullscreen="
                  + ("0 window=1600x900" if "pref:pushAutoFullscreen=false" in switches else "1 window=2560x1440") + " output=2560x1440 refreshHz=180.00 hdr=" + ("1" if "|hdr=1" in switches else "0")
                  + " stream=1920x1080@60.00 codec=hevc bitrateKbps=30000 decoder=hardware route=" + ("wifi" if wifi else "wired") + " trialsMax=500 modeSource=ini fgActive=0 srActive=0 smartlink=off nrRuntime=3 nrSize=1080 srTarget=" + ("2560x1440" if "|srTarget=qhd" in switches else "3840x2160" if "|srTarget=4k" in switches else "off")
                  + " srModel=" + (re.search(r"\|srModel=([A-Za-z0-9]+)", switches).group(1) if "|srModel=" in switches else "none") + " fgBackend=none clean=" + (re.search(r"\|clean=([0-9]+)", switches).group(1) if "|clean=" in switches else "0") + " profile=none"), ("slipstream-autorun", "step=0 name=[name] state=start reason=none"), ("slipstream-autorun", "step=0 name=[name] state=done reason=none")]
        made += [("slipstream-press", f"n={i} sendToArrivalMs=40.00 arrivalToScanoutMs=9.00 totalMs=60.00 phaseMs=3.10") for i in range(20)]
        made += [("slipstream-latency-test", "trials=20 answered=20 button=ps consoleP50=43.0 consoleP95=46.5 pcP50=10.5 pcP95=12.0 totalP50=60.0 totalP95=63.0")]
        delay = "60.0" if arm == "s-audio-all" else "30.0"
        for i in range(3):
            made += [("slipstream-latency", "frames=300 arrivalToDecodedP50=1.2 arrivalToDecodedP95=1.9 decodedToPickupP50=0.4 decodedToPickupP95=0.9 pickupToPresentCallP50=0.3 pickupToPresentCallP95=0.6 presentCallP50=0.2 presentCallP95=0.5 presentToScanoutP50=6.1 presentToScanoutP95=9.0 arrivalToScanoutP50=8.4 arrivalToScanoutP95=11.6 arrivalToScanoutP99=13.0 arrivalToScanoutMax=15.0 scanoutKnown=290 syncInterval=1 tearing=0 queueLatency=1 waitable=1 flip=independent"),
                     ("slipstream-delivery", "arrived=300 shown=298 replaced=2 gapP50=16.6 gapP95=21.0 gapMax=40.0 clumps=3 longGaps=1 shownGapP95=17.0 shownGapMax=33.0 policy=newest"),
                     ("remoteplay-audio", f"queueMs=20.0 endpointMs=10.0 delayMs={delay} targetMs=30.0 errorMs=0.0 driftPpm=0 ratioPpm=0 underruns={i} resets=0 deviceChanges=0 pops={i} concealed=0 hardResets=0 clicks=0 underrunStarts=0 queueP95=24.0"
                      + (f" ownClock=1 ownTargetMs=28.0 ownMedianMs=6.0 ownFrames={(0 if arm == 's-audio-own-fast' else 300) * (i + 1)} ownRejected=0" if "VEYRA_EXP_RP_AUDIO_OWN_CLOCK=1" in switches else ""))]
        made += [("remoteplay-clock", "wireIndex=600 ptsMs=11241.417 arrivalMinusPtsMs=58.931 nominalFps=60 (arrival-disciplined local estimate, not PS5 native PTS)"),
                 ("remoteplay-clock", "wireIndex=1200 ptsMs=21291.397 arrivalMinusPtsMs=165.675 nominalFps=60 (arrival-disciplined local estimate, not PS5 native PTS)")]
        if exe == "experimental":
            if arm == "v-neural": made.append(("nr-runtime-status", "original=absent originalVersion=none originalSigned=0 sf=absent selected=nr-original selectedAvailable=0 gpuSupportsOriginal=1 reason=runtime-missing originalTested=0 resolvedDefault=0"))
            else: made.append(("nr-runtime-status", "original=present originalVersion=x.x.x.x originalSigned=1 sf=absent selected=nr-original selectedAvailable=1 gpuSupportsOriginal=1 reason=none originalTested=1 resolvedDefault=0"))
            made += [("slipstream-scanout", "presents=300 samples=300 failed=0 disjoint=0 noSync=0 unchanged=250 named=50 exact=40 gapRejected=0 passedVsync=250 passedSync0=0 refreshShort=3 unsettled=0 stepP50=6 stepMax=9 statsAgeMsP50=30.0 statsAgeMsMax=45.0 namedPct=16.7 syncInterval=1")] * 3
            if "VEYRA_EXP_PRESENT_WINDOW_FACTS=1" in switches:
                region = ("2", "1", "1") if "VEYRA_EXP_PRESENT_RECT_REGION=1" in switches else ("3", "56", "0")
                made.append(("slipstream-present", f"window event=change known=1 host=[name],296,1280x720 monitor=0,0,2560x1440 coversMonitor=0 regionKind={region[0]} regionRects={region[1]} overlay=1 ownOverlaps=1 rootPopup=1 rootCaption=0 rootMaximized=0 rootTopmost=0 rootLayered=0 hostLayered=0 hostVisible=1 foreground=0 flipBlockers=0xF"))
                made.append(("slipstream-present", f"covers count=2 inPicture=1 rectRegion={region[2]} items=PushModeRow/modeRow,PushLatencyBadge/latencyBadge"))
            pipeline = next((p[9:] for p in switches.split(";") if p.startswith("pipeline:")), "")
            keys = [item.split("=")[0] for item in pipeline.split("|")] if pipeline else []
            if keys:
                detail = [k + (":refused:runtime-missing" if arm == "v-neural" and k in ("nr", "nrRuntime", "nrLook") else ":ok") for k in keys]
                refused = sum(1 for d in detail if ":refused" in d)
                made.append(("slipstream-pipeline", f"applied={len(keys) - refused} refused={refused} detail={','.join(detail)} streamHdr=0 videoHdr=" + ("1" if "|hdr=1" in switches else "0")))
                graph = "9.50" if arm == "v-clean-nis" else "5.00"
                if arm != "v-neural":
                    made += [("slipstream-nr", f"frames=300 layers=1 internal=1920x1080 output=2560x1440 nrMsP50=3.10 nrMsP95=4.20 srMsP50=1.20 srMsP95=1.80 fgMsP50=-1 fgMsP95=-1 graphMsP50=4.60 graphMsP95={graph} copyCount=1 vramMiB=3100 budgetMiB=11000 runtime=nr-original version=x.x.x.x")] * 3
                if "clean" in keys: made += [("slipstream-clean", "frames=300 strength=50 size=1920x1080 gpuMsP50=0.40 gpuMsP95=0.60 hudProtect=1 hdr=0")] * 2
                if arm.startswith("v-"):
                    block = "0.120" if arm in ("v-clean", "v-clean-dlss", "v-clean-nis") else "0.200"
                    band = "0.080" if arm in ("v-clean", "v-clean-dlss", "v-clean-nis") else "0.100"
                    ring = "0.080" if arm == "v-clean-neural-dlss" else "0.050"
                    size, hud = ("1920x1080", "0.000") if arm in ("v-original", "v-neural") else ("1920x1080", "0.004") if arm == "v-clean" else ("2560x1440", "0.012")
                    made.append(("slipstream-quality", "frames=20 size=1920x1080 blockiness=0.200 banding=0.100 ringing=0.050 detail=0.500 hudDelta=0.010 flicker=0.020 meanLuma=0.400 hdr=0 stage=raw"))
                    made.append(("slipstream-quality", f"frames=20 size={size} blockiness={block} banding={band} ringing={ring} detail=0.500 hudDelta={hud} flicker=0.020 meanLuma=0.400 hdr=0 stage=output"))
                if re.search(r"\|fg=(vfg|dlss|pushflow|fsr)", pipeline):
                    made.append((flow[0], flow[1].replace("generatedPresented=0", "generatedPresented=600")))
                    made.append((flow[0], flow[1].replace("realPresented=600", "realPresented=1200").replace("generatedPresented=0", "generatedPresented=1200")))
                    made.append(("slipstream-fg", "method=7 multiplier=2 tier=1 sourceFps=59.9 outputFps=119.8 generatedShown=1190 generatedExpired=4 skippedPairs=2 costP50=2.1 costP95=3.0 shownGapP95=8.9 shownGapMax=17.0 pairDelayMs=16.8"))
            kbm = " context=walk profile=builtin mouseEvents=5000 mouseHz=1000 rawToTranslateUsP50=12.0 rawToTranslateUsP95=30.0 rawToTranslateUsP99=55.0 translateUsP50=2.0 translateUsP95=4.0 translateUsP99=6.0 stateToSendMsP50=2.00 stateToSendMsP95=3.90 stateToSendMsP99=4.20 eventToSendMsP50=2.10 eventToSendMsP95=4.00 eventToSendMsP99=4.40 eventToSendMsMax=6.00 queueAgeMsP95=0.20 queueDepthMax=1 reports=1250 sendHz=250 sendGapMsP50=4.00 sendGapMsP95=4.30 sendGapMsMax=7.00 smoothingMs=0.0 saturatedPct=1.5 curve=linear switches=0 stuckReleases=0 heldAtLoss=0 fallback=none"
            pad = " context=none profile=builtin mouseEvents=0 mouseHz=-1 rawToTranslateUsP50=-1 rawToTranslateUsP95=-1 rawToTranslateUsP99=-1 translateUsP50=-1 translateUsP95=-1 translateUsP99=-1 stateToSendMsP50=1.90 stateToSendMsP95=3.80 stateToSendMsP99=4.10 eventToSendMsP50=2.00 eventToSendMsP95=3.90 eventToSendMsP99=4.30 eventToSendMsMax=5.50 queueAgeMsP95=0.10 queueDepthMax=1 reports=1250 sendHz=250 sendGapMsP50=4.00 sendGapMsP95=4.20 sendGapMsMax=6.50 smoothingMs=0.0 saturatedPct=0.0 curve=none switches=0 stuckReleases=0 heldAtLoss=0 fallback=none"
            m = re.search(r"VEYRA_EXP_RP_KBM_INPUT_PROFILE=([a-z-]+)", switches)
            if m: made += [("slipstream-input", "source=kbm inputProfile=" + m.group(1) + kbm)] * 2 + [("slipstream-input", "source=pad inputProfile=" + m.group(1) + pad)] * 2
            if m: made += [("slipstream-input-poll", "polls=1250 pollMs=4 pollGapMsP50=4.000 pollGapMsP95=4.400 pollGapMsP99=5.100 pollGapMsMax=9.000 latePolls=2 rawBacklogMax=3")] * 4
            else: made.append(("slipstream-input", "source=none inputProfile=legacy" + re.sub(r"=[0-9.]+ ", "=-1 ", pad)))
        made.append(("slipstream-autorun", "finished ok=1 steps=4"))
    if arm.startswith("sm-"):       # the smoothness session's cadence line, as run-tests.ps1 Add-DryRunLog makes it
        vrr, off, h1, h2, h3, uneven, sub, gen = "1", "0.5", "0", "0", "300", "0.0", "59.90", "0.00"
        if arm != "sm-native60": sub, gen, h2, h3, off = "119.80", "59.90", "600", "0", "1.0"
        if arm == "sm-fg2-novrr": vrr, h1, h2, uneven = "0", "300", "300", "50.0"
        if arm == "sm-fg3": sub, gen, h1, h2, off = "179.70", "119.80", "900", "0", "0.8"
        if arm == "sm-fg2-repair": off = "5.0"
        if arm == "sm-adaptive":        # VRR changes once during the runs (the app's [display-vrr] says so)
            vrr = "-1"; made.append(("display-vrr", "level=2 known=true monitorSupportsVrr=true currentlyCapable=true trueGsync=false capsKnown=true possible=true requested=true active=false displayInVrrMode=false indicator=false simulated=false status=0 refreshHz=180.000 (made up)"))
        if "pref:vrr=false" in switches: made = [(c, t.replace(" requested=true active=true ", " requested=false active=false ") if c == "display-vrr" else t) for c, t in made]
        made += [("slipstream-cadence", f"window=5 submittedFps={sub} realFps=59.90 generatedFps={gen} intervals=300 intervalP50Ms=8.33 intervalP95Ms=9.10 intervalP99Ms=10.20 intervalMaxMs=16.90 offPct={off} refreshHz=179.95 vrr={vrr} fixedHold1={h1} fixedHold2={h2} fixedHold3={h3} fixedUnevenPct={uneven} fixedShownFps=119.80 evenFps=89.98 evenDivisor=2 overflow=0")] * 3
    made += [("display-latency", "presentToScanoutP50Ms=1.200 presentToScanoutP95Ms=3.900 presentToScanoutMaxMs=5.100 samples=9 replacedAtLeast=0 compositionMode=2 refreshHz=180.000 (made up)")] * 2
    return [line(0.2 * n, component, text) for n, (component, text) in enumerate(made) if component != "x"]

ALSO = {"audiofix": ["s-base"], "pacing": ["p-window"]}      # run-tests.ps1 $measureSessionTable 'also': the pair that joins from another set

def dry_run_folder(folder, session, wifi=False, drop=(), nr_unknown=False):
    """An evidence folder as 'run-tests.ps1 -DryRun -Session <session>' leaves it (bench logs, run.json, kit.json, arms.csv)."""
    table = report.read_arm_table(os.path.join(KIT, "arms.csv"))
    also = ALSO.get(session, [])
    arms = [n for n, r in table.items() if r["set"] == session or n in also]
    os.makedirs(os.path.join(folder, "bench")); shutil.copy(os.path.join(KIT, "arms.csv"), folder); shutil.copy(SPEC, folder)
    with open(os.path.join(folder, "kit.json"), "w") as f: json.dump({"session": session, "kitVersion": "5", "dryRun": True}, f)
    for round_ in (1, 2):
        for arm in arms:
            name = f"measure-{arm}@20261120-12000{round_}"
            os.makedirs(os.path.join(folder, "runs", name))
            with open(os.path.join(folder, "runs", name, "run.json"), "w") as f: json.dump({"arm": arm, "purpose": "measure", "exe": table[arm]["exe"], "completed": True}, f)
            made = [one for one in dry_lines(arm, table[arm]["exe"], table[arm]["switches"], wifi) if not any("] [" + c + "] " in one for c in drop)]
            if nr_unknown:      # a build without the neural layer (8A / 8B): its keys are unknown, no runtime line
                made = [re.sub(r"\b(nr|nrRuntime|nrLook|nrSize):(ok|refused:runtime-missing)", r"\1:refused:unknown-key", one) for one in made if "] [nr-runtime-status] " not in one]
            with open(os.path.join(folder, "bench", name + ".txt"), "w", encoding="utf-8") as f: f.write("\n".join(made) + "\n")
    return arms


class DryRunSessions(unittest.TestCase):
    """What the Windows dry run must find in RESULT.txt of every measured session (ci/check-dry-run.ps1 $kitFiveText)."""
    @classmethod
    def setUpClass(cls):
        with open(os.path.join(KIT, "ci", "check-dry-run.ps1"), encoding="ascii") as f: check = f.read()
        block = check[check.index("$kitFiveText = @{"):check.index("foreach ($item in $sessionRuns) {")]
        cls.expected = {}
        for m in re.finditer(r"(?s)'(\w+)' = @\((.*?)\)\n", block):
            cls.expected[m.group(1)] = [t.replace("''", "'") for t in re.findall(r"'((?:[^']|'')*)'", m.group(2))]

    def section(self, session, wifi=False, **more):
        d = tempfile.mkdtemp()
        try:
            arms = dry_run_folder(d, session, wifi, **more)
            data = report.load(d)
            measured = report.measured_tables(data)
            return arms, "\n".join(report.measured_paragraphs(data, measured)), data, measured
        finally: shutil.rmtree(d)

    def test_every_session_has_the_texts_the_dry_run_check_expects(self):
        self.assertEqual(sorted(self.expected), sorted(report.MEASURE_SESSIONS))
        for session in report.MEASURE_SESSIONS:
            with self.subTest(session=session):
                arms, text, _data, _measured = self.section(session, wifi=(session == "pacing"))
                self.assertGreaterEqual(len(arms), 1)
                for arm in arms: self.assertIn(f"  {arm} (2 runs)", text)
                for wanted in self.expected[session]: self.assertIn(wanted, text, session)
                for bad in report.REFUSED_WORDS.split("|"): self.assertNotRegex(text, bad)
                for one in text.splitlines(): self.assertEqual(report.redact_line(one), one, one)      # RESULT.txt would be withheld

    def test_a_build_without_the_feature_says_so_and_the_session_finishes(self):
        # 8A has no input core: every input arm says "not in this build", nothing is compared
        arms, text, data, measured = self.section("input", drop=("slipstream-input", "slipstream-input-poll"))
        for arm in arms: self.assertIn(f"  {arm} (2 runs)", text)
        self.assertEqual(text.count("    " + report.INPUT_MISSING), len(arms))
        # a build without VEYRA_TEST_RP_PIPELINE (Package 7, original, stable): no verdict, no comparison, said per arm
        arms, text, data, measured = self.section("visual", drop=("slipstream-pipeline",))
        self.assertEqual(text.count("    " + report.PIPELINE_MISSING), len(arms))
        self.assertEqual(report.measured_compared(data, measured), {})
        verdicts = report.visual_verdicts(measured, report.neural_runtime_lines(data), report.feature_lines(data))
        for arm in arms:
            if arm not in ("v-original", "v-reference"): self.assertTrue(verdicts[arm].startswith("no verdict: the pipeline keys were not applied"), arm)
        self.assertIn("not compared with v-original: pipeline keys not applied in this arm", text)
        # 8A / 8B: the neural keys are unknown to the build
        arms, text, data, measured = self.section("neural", nr_unknown=True)
        runtime = report.neural_runtime_lines(data)
        self.assertEqual(set(runtime), set(arms))
        for arm in arms: self.assertTrue(runtime[arm].startswith("not in this build: skipped (the build does not know the neural keys: nr:refused:unknown-key"), runtime[arm])
        self.assertIn("WITHOUT the neural layer", text)

    def test_visual_every_arm_has_a_verdict_or_says_why_not(self):
        arms, text, _data, measured = self.section("visual")
        verdicts = report.visual_verdicts(measured, report.neural_runtime_lines(_data), report.feature_lines(_data))
        self.assertEqual(report.feature_lines(_data), {})
        self.assertEqual(set(verdicts), set(arms))
        # the neural runtime is not installed in v-neural: said, the arm still reported, no verdict from numbers without the layer
        self.assertTrue(report.neural_runtime_lines(_data)["v-neural"].startswith("not installed: skipped (the app reports selected=nr-original selectedAvailable=0 reason=runtime-missing"))
        self.assertTrue(verdicts["v-neural"].startswith("no verdict: the neural layer did not run in this arm"))
        self.assertIn("  v-neural (2 runs)", text); self.assertIn("WITHOUT the neural layer", text)
        # HDR or SDR: every run says which (the stream's own HDR); the made-up stream is SDR, so the reference (an HDR stream)
        # is named as not reproduced, and its HDR conversion (hdr=1) does not make the stream HDR
        self.assertIn("    picture: SDR, SDR (per run)", text); self.assertNotIn("    picture: HDR", text)
        self.assertIn("  v-reference (2 runs)\n    picture: SDR, SDR (per run)\n    " + report.REFERENCE_SDR + report.REFERENCE_HOW, text)
        self.assertEqual(report.measured_json(_data, measured)["reference"], {"v-reference": report.REFERENCE_SDR + report.REFERENCE_HOW})
        self.assertIn("v-reference", report.measured_compared(_data, measured)); self.assertIn("v-dlss-k", report.measured_compared(_data, measured))
        self.assertIn("    against v-dlss: ", text)                                                    # model K beside model M
        self.assertEqual(verdicts["v-clean"], "better on these software picture metrics (blockiness lower, banding lower; processing P95 5.00 ms, within 8.33 ms)")
        self.assertEqual({a: report.verdict_word5(v) for a, v in verdicts.items()},
                         {"v-original": "reference", "v-dlss": "cannot tell", "v-dlss-k": "cannot tell", "v-clean": "better", "v-clean-dlss": "better", "v-neural": "no verdict", "v-clean-neural-dlss": "worse",
                          "v-clean-nis": "cannot tell", "v-reference": "the owner's reference"})
        self.assertNotRegex(text.replace(report.VISUAL_RULE, "").replace(report.SPREAD_RULE, ""), r"(?i)\b(nicer|beautiful|prettier)\b")
        # a better / worse word only on a verdict line, never from the spread rule
        for one in text.splitlines():
            if re.search(r"\b(better|worse)\b", one): self.assertTrue(one.startswith("    verdict: ") or one.startswith("  Verdict rule") or one.startswith("  Comparison rule"), one)

    def test_neural_layer_line_is_never_blank(self):
        table = report.read_arm_table(os.path.join(KIT, "arms.csv"))
        for session in ("visual", "neural"):
            arms, text, data, _measured = self.section(session)
            lines = report.neural_runtime_lines(data)
            wanted = [a for a in arms if "nr=1" in report.arm_pipeline(table, a).split(";")]
            self.assertEqual(sorted(lines), sorted(wanted)); self.assertGreaterEqual(len(wanted), 2)
            for a in wanted: self.assertTrue(lines[a])
        # no line at all (a build without the pipeline switch): said, not blank
        d = tempfile.mkdtemp()
        try:
            dry_run_folder(d, "visual")
            for name in os.listdir(os.path.join(d, "bench")):
                path = os.path.join(d, "bench", name)
                with open(path) as f: kept = [l for l in f.read().splitlines() if "[nr-runtime-status]" not in l and "[slipstream-pipeline]" not in l]
                with open(path, "w") as f: f.write("\n".join(kept) + "\n")
            self.assertTrue(report.neural_runtime_lines(report.load(d))["v-neural"].startswith("not in this build: skipped (no [slipstream-pipeline] and no [nr-runtime-status] line"))
        finally: shutil.rmtree(d)

    def test_an_own_clock_arm_in_a_build_without_the_switch_is_not_a_result(self):
        d = tempfile.mkdtemp()
        try:
            dry_run_folder(d, "audiofix")
            for name in os.listdir(os.path.join(d, "bench")):
                path = os.path.join(d, "bench", name)
                with open(path) as f: text = re.sub(r" ownClock=1 ownTargetMs=\S+ ownMedianMs=\S+ ownFrames=\S+ ownRejected=\S+", "", f.read())
                with open(path, "w") as f: f.write(text)
            data = report.load(d)
            lines = report.own_clock_lines(data)
            self.assertEqual(sorted(lines), ["s-audio-own", "s-audio-own-fast"])
            for text in lines.values(): self.assertTrue(text.startswith("own clock: NOT A RESULT, no [remoteplay-audio] line says ownClock=1"), text)
            measured = report.measured_tables(data)
            self.assertEqual(sorted(report.measured_compared(data, measured)), ["s-audio-all"])            # the two own-clock arms are not compared
            self.assertEqual(sorted(report.measured_not_compared(data, measured)), ["s-audio-own", "s-audio-own-fast"])
        finally: shutil.rmtree(d)

    def test_the_whole_report_is_made_from_every_session_and_passes_its_privacy_check(self):
        """What ci/check-dry-run.ps1 does with every session's zip: report.py --strict (exit code 0, no DISAGREE)."""
        import contextlib, io
        for session in report.MEASURE_SESSIONS:
            d = tempfile.mkdtemp()
            try:
                dry_run_folder(d, session)
                with open(os.path.join(d, "result.json"), "w") as f: json.dump({"arms": [], "measured": report.measured_json(report.load(d))}, f)
                with contextlib.redirect_stdout(io.StringIO()) as said:
                    try: code = report.main([d, "--out", os.path.join(d, "out"), "--strict"])
                    except SystemExit as stop: code = stop.code
                self.assertIn(code, (0, None), session + ": " + said.getvalue()[-600:])
                with open(os.path.join(d, "out", "report.md"), encoding="utf-8") as f: text = f.read()
                self.assertNotIn("DISAGREE", text, session); self.assertIn("The kit's MEASURED VALUES and this report agree", text, session)
                self.assertIn("## Measured values (kit version 5)", text, session)
            finally: shutil.rmtree(d)

    def test_the_report_agrees_with_itself_and_sees_a_changed_cell(self):
        _arms, _text, data, measured = self.section("audiofix")
        mine = report.measured_json(data, measured)
        theirs = json.loads(json.dumps(mine))
        self.assertEqual(report.measured_disagreement(mine, theirs), [])
        self.assertEqual(mine["compared"]["s-audio-all"]["audio|remoteplay-audio||delayMs|median"], "differs")
        # the own clock: an arm whose ownFrames grew is compared, one whose arrival lookup failed is a failed arm and is not
        self.assertEqual(mine["ownClock"]["s-audio-own"], "own clock ran: ownFrames reached 900"); self.assertIn("s-audio-own", mine["compared"])
        self.assertTrue(mine["ownClock"]["s-audio-own-fast"].startswith("own clock: FAILED ARM, not a result")); self.assertNotIn("s-audio-own-fast", mine["compared"])
        self.assertTrue(mine["notCompared"]["s-audio-own-fast"].startswith("not compared with s-base: own clock: FAILED ARM"))
        self.assertNotIn("s-base", mine["ownClock"])
        key = "audio|remoteplay-audio||delayMs|median"
        theirs["arms"]["s-base"]["cells"][key]["value"] = 31.0
        theirs["compared"]["s-audio-all"][key] = "cannot tell"
        del theirs["arms"]["s-audio-all"]["cells"]["audio|remoteplay-audio||pops|rate-min"]
        differ = report.measured_disagreement(mine, theirs)
        self.assertEqual(len(differ), 3, differ)
        theirs = json.loads(json.dumps(mine)); del theirs["arms"]["s-base"]
        self.assertTrue(any("not in the kit's result" in d for d in report.measured_disagreement(mine, theirs)))


class Bench2Fixes(unittest.TestCase):
    """The kit 4 defects of BENCH2.md that are rules of the report (the kit holds the same in Read-PrecisionLines / Get-RunConfig)."""
    def test_swap_chain_state_during_the_measurement_not_at_exit(self):
        lines = [line(1.0, "slipstream-swapchain", "flip=unknown fullscreen=0 windowCoversMonitor=0 reason=created"),
                 line(1.2, "slipstream-swapchain", "flip=unknown fullscreen=1 windowCoversMonitor=1 reason=resized"),
                 line(40.0, "push-latency", "done answered=12/12 meanMs=554.6 medianMs=551.1 minMs=492.8 maxMs=586.6 pushfpsMs=2.3"),
                 line(40.1, "slipstream-autorun", "finished ok=1 steps=3"),
                 line(40.5, "slipstream-swapchain", "flip=unknown fullscreen=0 windowCoversMonitor=0 reason=pacing")]
        self.assertEqual(report.parse_log(lines)["swapchain"]["fullscreen"], "1")
        self.assertEqual(report.parse_log([lines[0], lines[1], lines[3], lines[4]])["swapchain"]["fullscreen"], "1")     # no latency test: the plan's end
        self.assertEqual(report.parse_log([lines[0], lines[1], lines[4]])["swapchain"]["fullscreen"], "0")                # neither: the whole log

    def test_settle_time_counts_from_the_last_session_start_before_the_first_frame(self):
        def log(extra):
            return report.parse_log([line(0.0, "remoteplay", "session started host=x profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000"),
                                     line(0.5, "pacing", "DXGI enabled=true maximumLatency=1 vsync=true tearing=false hr=0x0 staleLatencyCounts=0")] + extra +
                                    [line(26.0, "frame-rate", "presentSubmitFps=59.9"), line(60.0, "pacing", "DXGI enabled=true maximumLatency=1 vsync=true tearing=false hr=0x0 staleLatencyCounts=0")])
        retried = [line(12.0, "remoteplay", "session started host=x profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000"),
                   line(14.0, "pacing", "DXGI enabled=true maximumLatency=3 vsync=false tearing=false hr=0x0 staleLatencyCounts=0"),
                   line(15.0, "pacing", "DXGI enabled=true maximumLatency=1 vsync=true tearing=false hr=0x0 staleLatencyCounts=0")]
        self.assertEqual(log(retried)["config_changed"], set())                       # the retried connection is start-up
        late = [line(40.0, "pacing", "DXGI enabled=true maximumLatency=3 vsync=false tearing=false hr=0x0 staleLatencyCounts=0")]
        self.assertTrue(log(late)["config_changed"])                                  # a change long after the start still counts

    def test_button_answer_time_note(self):
        def presses(values): return [line(1.0, "push-latency", "trials answerMs=[" + ",".join(str(v) for v in values) + "]")]
        def data(a, b, purpose="latency"):
            runs = []
            for i, values in enumerate((a, b)):
                r = run_of(presses(values), purpose); r["arm"] = "s-base"; r["name"] = f"s-base@{i}"; runs.append(r)
            return {"runs": runs, "table": {"s-base": {"compare": "s-base"}}, "kit": {}, "folder": ""}
        notes = report.button_spread_notes(data([560.0, 562.0, 561.0], [451.0, 452.0, 453.0]))
        self.assertIn("differs by 109 ms between its runs (run medians 561, 452 ms)", notes["s-base"])
        self.assertIn("cannot compare arms at this size", notes["s-base"])
        self.assertEqual(report.button_spread_notes(data([560.0, 562.0], [520.0, 530.0])), {})          # 36 ms: under the 50 ms limit
        self.assertEqual(report.button_spread_notes(data([560.0], [451.0], purpose="measure")), {})


class KitMirror(unittest.TestCase):
    """run-tests.ps1 holds the same texts and numbers (there is no PowerShell here: the strings are read from the script)."""
    @classmethod
    def setUpClass(cls):
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="ascii") as f: cls.kit = f.read()
    def quoted(self, text): return "'" + text.replace("'", "''") + "'"
    def test_texts(self):
        for text in (report.SPREAD_RULE, report.VISUAL_RULE, report.MEASURED_FOOT) + tuple(report.SESSION_FIRST_LINE.values()) + tuple(report.SPREAD_WORDS):
            self.assertIn(self.quoted(text), self.kit, text[:60])
        for session in report.MEASURE_SESSIONS: self.assertIn("    '" + session + "' = '", self.kit)
        self.assertIn("$measurePurposes = @('" + "', '".join(report.MEASURED_PURPOSES) + "')", self.kit)
        self.assertIn("$measureStats = @('" + "', '".join(report.MEASURE_STATS) + "')", self.kit)
        self.assertIn("$measurePhaseSeconds = 3.0 ", self.kit); self.assertEqual(report.MEASURE_PHASE_S, 3.0)
        self.assertIn("$visualDefects = @('" + "', '".join(report.VISUAL_DEFECTS) + "')", self.kit)
        self.assertIn("$visualCleaned = @('" + "', '".join(report.VISUAL_CLEANED) + "')", self.kit)
        self.assertIn("$neuralKeys = @('" + "', '".join(report.NEURAL_KEYS) + "')", self.kit)
        self.assertIn("$buttonSpreadMs = 50.0", self.kit); self.assertEqual(report.BUTTON_SPREAD_MS, 50.0)
        self.assertIn("if ($gap -gt (2.0 * $spread)) { return 'differs' }", self.kit); self.assertEqual(report.SPREAD_GAP_FACTOR, 2.0)
        self.assertIn("'quality output|slipstream-quality|stage=output|' + $k + '|median'", self.kit); self.assertEqual(report.VISUAL_QUALITY_KEY.format("x"), "quality output|slipstream-quality|stage=output|x|median")
        self.assertIn("'" + report.VISUAL_PROCESSING_KEY + "'", self.kit)
        self.assertIn(report.measured_heading("")[len("MEASURED VALUES"):].replace("'", "''"), self.kit)
        self.assertIn("$kitVersion = '5'", self.kit)
        for text in ("own clock: NOT A RESULT, no [remoteplay-audio] line says ownClock=1 with ownFrames= (a build without the switch): the arm ran the default rule",
                     "own clock: FAILED ARM, not a result: ownFrames stayed 0 (the arrival of the shown frames was not found), so the arm ran the default rule", "own clock ran: ownFrames reached "):
            self.assertIn(text, self.kit, text[:40])
        self.assertIn("-notcontains '" + report.OWN_CLOCK_SWITCH + "'", self.kit)
        for text in (report.PICTURE_RULE, report.NEURAL_WITHOUT, "not installed: skipped", "not usable on this PC: skipped", "refused by the app: skipped (",
                     "no verdict: the neural layer did not run in this arm (the line above says why); its values are those of the path without it",
                     "; HDR and SDR runs are never mixed in one comparison, and runs that do not say which they were are not compared",
                     report.REFERENCE_SDR, report.REFERENCE_HOW, "reference could not be reproduced: the stream's HDR was not logged in ",
                     report.PIPELINE_MISSING, report.INPUT_MISSING, "pipeline keys not applied in this arm (this build has no VEYRA_TEST_RP_PIPELINE)",
                     " (that build has no VEYRA_TEST_RP_PIPELINE)", "no verdict: the pipeline keys were not applied (this build has no VEYRA_TEST_RP_PIPELINE)",
                     "not in this build: skipped (the build does not know the neural keys: "):
            self.assertIn(text.replace("'", "''"), self.kit, text[:50])
        self.assertIn("$pictureSessions = @('" + "', '".join(report.PICTURE_SESSIONS) + "')", self.kit)
    def test_sessions_and_plans(self):
        table = report.read_arm_table(os.path.join(KIT, "arms.csv"))
        plans = dict(re.findall(r"    '(\w+)' = @\{ plan = '([^']+)'; rounds = 2;", self.kit))
        self.assertEqual(sorted(plans), sorted(report.MEASURE_SESSIONS))
        self.assertEqual(plans["visual"], "settle:8;probe:20;wait:40;exit"); self.assertEqual(plans["audiofix"], "settle:8;wait:120;exit")
        self.assertEqual(plans["builds"], "settle:8;wait:60;exit")
        with open(os.path.join(KIT, "run-first-test.ps1"), encoding="ascii") as f: first = f.read()
        def minutes(session, by_hand=0):
            also = ALSO.get(session, [])
            arms = [n for n, r in table.items() if r["set"] == session or n in also]
            return report.about_minutes(2 * (len(arms) * report.plan_seconds(plans[session]) + by_hand * report.MANUAL_SECONDS))
        got = {s: minutes(s, by_hand=2 if s == "builds" else 0) for s in report.MEASURE_SESSIONS}
        self.assertEqual({k: got[k] for k in ("input", "visual", "neural", "framegen", "audiofix", "builds")}, {"input": 21, "visual": 25, "neural": 20, "framegen": 20, "audiofix": 19, "builds": 15})
        self.assertEqual(got["smoothness"], 17); self.assertIn("About 17 minutes.", first[first.index("    smoothness "):first.index("    smoothness ") + 300])
        self.assertEqual(got["pacing"], 8); self.assertIn("About 8 minutes.", first[first.index("    pacing "):first.index("    pacing ") + 200])
        # the procedure and START-HERE print the same figures
        root = os.path.join(HERE, "..", "..")
        with open(os.path.join(root, "docs", "HARDWARE_TEST_PROCEDURE.md"), encoding="utf-8") as f: procedure = f.read()
        with open(os.path.join(root, "packaging", "slipstream", "START-HERE.template.txt"), encoding="ascii") as f: start = f.read()
        for session, minutes_ in got.items():
            self.assertIn(f"`Run-First-Test.cmd -Session {session}` | {minutes_} min |", procedure, session)
            self.assertRegex(start, r"Run-First-Test\.cmd -Session %s +about %d minutes" % (session, minutes_), session)
        self.assertIn("## Package 8 sessions (kit version 5)", procedure)
        for text in ("None of it is\ninput-to-photon", "not installed: skipped", "\"Differs\" is not \"better\"", "It does NOT tell you which picture looks best", "It does NOT prove the builds are equal"):
            self.assertIn(text, procedure)
        for session, text in (("input", "About 21 minutes."), ("visual", "About 25 minutes."), ("neural", "About 20 minutes."), ("framegen", "About 20 minutes."), ("audiofix", "About 19 minutes."), ("builds", "About 15 minutes.")):
            self.assertIn(text, first[first.index("    " + session + " "):first.index("    " + session + " ") + 420], session)
    def test_arms(self):
        table = report.read_arm_table(os.path.join(KIT, "arms.csv"))
        for session in report.MEASURE_SESSIONS:
            arms = {n: r for n, r in table.items() if r["set"] == session}
            self.assertGreaterEqual(len(arms), 1, session)
            for name, r in arms.items():
                self.assertIn(r["compare"], table, name)
                self.assertIn(table[r["compare"]]["set"], (session, "switches"), name)                       # its pair runs in the same session
                if session != "builds": self.assertEqual(r["exe"], "experimental", name)
                pipeline = report.arm_pipeline(table, name)
                if session in ("visual", "neural", "framegen", "smoothness"):
                    self.assertTrue(pipeline.startswith("profile=original;"), name)                          # no arm inherits another's settings
                    self.assertRegex(pipeline, r"^[A-Za-z0-9]+=[A-Za-z0-9.-]+(;[A-Za-z0-9]+=[A-Za-z0-9.-]+)*$")
                    for key in (p.split("=")[0] for p in pipeline.split(";")):
                        self.assertIn(key, ("sr", "srModel", "srTarget", "clean", "nr", "nrRuntime", "nrSize", "nrLook", "fg", "fgMultiplier", "hdr", "profile"), name)
                    if "nr=1" in pipeline.split(";"): self.assertIn("nrRuntime=3", pipeline.split(";"), name)      # the NVIDIA-signed original only
                    if "sr=dlss" in pipeline.split(";") and session == "visual": self.assertRegex(pipeline, r";srModel=[MK];", name)                 # the model is always named
                else: self.assertEqual(pipeline, "", name)
        self.assertEqual([n for n, r in table.items() if r["set"] == "visual"], ["v-original", "v-dlss", "v-dlss-k", "v-clean", "v-clean-dlss", "v-neural", "v-clean-neural-dlss", "v-clean-nis", "v-reference"])
        self.assertEqual(table["v-dlss-k"]["compare"], "v-dlss"); self.assertIn("srModel=K", report.arm_pipeline(table, "v-dlss-k")); self.assertIn("srModel=M", report.arm_pipeline(table, "v-dlss"))
        self.assertIn("not from an outside tool", report.SESSION_FIRST_LINE["framegen"]); self.assertIn("[frame-flow], [slipstream-fg]", report.SESSION_FIRST_LINE["framegen"])
        self.assertEqual(report.arm_pipeline(table, "v-reference"), "profile=original;sr=dlss;srModel=M;srTarget=4k;fg=vfg;fgMultiplier=2;hdr=1")
        # 8A review: clean-up alone at the stream's own size, so its blockiness is compared with v-original's
        self.assertEqual(report.arm_pipeline(table, "v-clean"), "profile=original;hdr=0;clean=50"); self.assertEqual(table["v-clean"]["compare"], "v-original")
        self.assertEqual(sorted(r["exe"] for r in table.values() if r["set"] == "builds"), ["experimental", "modified", "original", "previous"])
        self.assertEqual(table["p-rect-region"]["compare"], "p-window"); self.assertIn("VEYRA_EXP_PRESENT_RECT_REGION=1", table["p-rect-region"]["switches"])
        self.assertIn("pref:pushAutoFullscreen=false", table["p-rect-region"]["switches"]); self.assertIn("    'pacing' = @{ plan = 'settle:8;wait:60;exit'; rounds = 2; also = @('p-window');", self.kit)
        self.assertEqual(table["s-audio-own"]["switches"], "VEYRA_EXP_RP_AUDIO_OWN_CLOCK=1"); self.assertIn("    'audiofix' = @{ plan = 'settle:8;wait:120;exit'; rounds = 2; also = @('s-base');", self.kit)
        self.assertEqual(table["s-audio-all"]["switches"], "VEYRA_EXP_RP_AUDIO_FAST_RECOVER=1;VEYRA_EXP_RP_AUDIO_SMOOTH=1;VEYRA_EXP_RP_AUDIO_ADAPTIVE=1")
        self.assertEqual({r["switches"].split("INPUT_PROFILE=")[1].split(";")[0] for r in table.values() if r["set"] == "input"}, {"legacy", "ultra-low-latency", "adaptive-precision"})
        self.assertFalse(any("STREAM_DEDUP" in r["switches"] for r in table.values() if r["set"] in report.MEASURE_SESSIONS))


if __name__ == "__main__":
    unittest.main(verbosity=1)
