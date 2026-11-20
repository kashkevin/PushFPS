#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The network session (-Session network, SmartLink) in report.py on made-up logs.
python3 -B tools/remoteplay_latency_bench/test_smartlink_session.py"""
import json, math, os, random, re, sys, tempfile, unittest
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report
from test_report import add_run, line

TABLE = ["name,exe,set,compare,switches,note,fg,differs",
         "n-observe,experimental,network,n-observe,VEYRA_EXP_RP_SMARTLINK=1,o,off,",
         "n-apply,experimental,network,n-observe,VEYRA_EXP_RP_SMARTLINK=apply,a,off,smartlink",
         "n-off,experimental,network,n-observe,VEYRA_EXP_RP_STATS=1,f,off,smartlink",
         "n-original,original,network-builds,n-observe,,b,off,"]

def config(mode, queue=1):
    return (f"mode=none fg=0 sr=0 enhance=0 lowQueue={queue} displaySync=1 vrrRequested=1 refreshHz=180.000 stream=1920x1080@60 codec=hevc bitrateKbps=15000 "
            f"decoder=D3D12VA route=wifi trialsMax=40 smartlink={mode}")

def network_log(rng, mode, stutter=2.0, latency=12.0, video=9.0, minutes=5, queue=1, smartlink=True):
    lines = [line(1, "remoteplay", "session started host=192.168.x.x profile=1920x1080@60 codec=hevc requestedBitrateKbps=15000")]
    if smartlink: lines.append(line(2, "slipstream-config", config(mode, queue)))
    pops = 0
    for i in range(minutes * 12):
        t = 10 + i * 5
        lines.append(line(t, "remoteplay-progress", f"state=3 received=300 decoded=300 inputFps=59.9 decodeFps=59.9 videoMbps={video + rng.uniform(-0.5, 0.5):.3f} receiveAgeMs=3.0 decodeAgeMs=2.0 decodeBusyMs=1.0 queue=0"))
        for s in range(5): lines.append(line(t + s, "frame-rate", "revision=1 gpuCompletedFps=60 completedReal=60 presentSubmitFps=59.90 windowMs=1000"))
        lines.append(line(t, "display-latency", "presentToScanoutP50Ms=9.1 presentToScanoutP95Ms=13.0 presentToScanoutMaxMs=20.0 samples=300 replacedAtLeast=0 compositionMode=-1 refreshHz=179.952"))
        if not smartlink: continue
        lp = latency + rng.uniform(-0.5, 0.5)
        lines.append(line(t, "slipstream-latency", f"frames=300 arrivalToScanoutP50={lp:.2f} arrivalToScanoutP95={lp + 4:.2f} arrivalToScanoutP99={lp + 7:.2f} arrivalToScanoutMax=40 scanoutKnown=290"))
        gaps = max(0, int(round(stutter + rng.uniform(-1, 1))))
        lines.append(line(t, "slipstream-delivery", f"arrived=300 shown=298 replaced=2 gapP50=16.6 gapP95={20 + gaps * 0.5:.1f} gapMax=45.0 clumps=2 longGaps={gaps} shownGapP95=17.0 shownGapMax=33.0 policy=newest"))
        pops += rng.choice((0, 0, 1))
        lines.append(line(t, "remoteplay-audio", f"queueMs=20.0 underruns={pops} resets=0 pops={pops} concealed=0"))
        reorder = 4 if (mode == "apply" and i % 3 == 0) else 0
        lines.append(line(t, "slipstream-smartlink", f"profile=unstable-wifi regime=periodic-spikes risk=0.40 rttMs={5 + i % 30} rttMaxMs=79 targetKbps=15000 measuredKbps=9000 gapP95Ms=21.0 gapMaxMs=45.0 late=1 lost=0 reorderMs={reorder} audioTargetMs=0 netLimited=1 headroom=0 signal=-1 mode={mode} reasons=spikes"))
    return lines

TRACE_HEADER = ("endMs,windowMs,route,packetsReceived,packetsLost,packetsReordered,packetsLate,packetGapMeanMs,packetGapMaxMs,"
                "framesArrived,framesIncomplete,framesFecRecovered,frameGapMaxMs,frameGapExcessMs,lateRescueMs,reorderWaitMs,"
                "rttMs,consoleTargetKbps,measuredKbps,requestedKbps,videoQueueDepth,audioBufferedMs,audioUnderruns,"
                "wifiSignalPercent,wifiRssiDbm,wifiLinkMbps,wifiBandGhz,frameLateMeanMs")
TRACE_ROW = "200,200,2,120,0,1,0,1.6,9,12,0,0,16.6,0,0,0,5,15000,9000,15000,0,20,0,80,-55,866,5,0.4"

def null_log(rng, mode, rho, state, windows=36):
    """The reviewer's identical-arms model (review-kit2 run/advnet.py): drifting Wi-Fi as AR(1) with rho for stutter and
    latency, the same process in every arm; 'state' carries the drift from one run to the next (the session order)."""
    lines = [line(1, "remoteplay", "session started host=x profile=1920x1080@60 codec=hevc requestedBitrateKbps=15000"), line(2, "slipstream-config", config(mode))]
    pops = 0
    for i in range(windows):
        t = 10 + i * 5
        state[0] = rho * state[0] + rng.gauss(0, 1.0); state[1] = rho * state[1] + rng.gauss(0, 0.6)
        lines.append(line(t, "remoteplay-progress", "state=3 received=300 decoded=300 inputFps=59.9 decodeFps=59.9 videoMbps=9.000 receiveAgeMs=3.0 decodeAgeMs=2.0 decodeBusyMs=1.0 queue=0"))
        lp = 12 + state[1]
        lines.append(line(t, "slipstream-latency", f"frames=300 arrivalToScanoutP50={lp:.2f} arrivalToScanoutP95={lp + 4:.2f} arrivalToScanoutP99={lp + 7:.2f} arrivalToScanoutMax=40 scanoutKnown=290"))
        g = max(0, int(round(3 + state[0])))
        lines.append(line(t, "slipstream-delivery", f"arrived=300 shown=298 replaced={max(0, int(2 + state[0]))} gapP50=16.6 gapP95={20 + g * 0.5:.1f} gapMax=45.0 clumps=2 longGaps={g} policy=newest"))
        pops += rng.choice((0, 0, 1)); lines.append(line(t, "remoteplay-audio", f"queueMs=20 underruns={pops} resets=0 pops={pops} concealed=0"))
        lines.append(line(t, "slipstream-smartlink", f"profile=unstable-wifi regime=stable rttMs=5 targetKbps=15000 measuredKbps=9000 reorderMs=0 netLimited=0 headroom=0 mode={mode} reasons=none"))
    return lines

class NetworkSession(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name; self.index = 0
        with open(os.path.join(self.folder, "arms.csv"), "w") as f: f.write("\n".join(TABLE) + "\n")
    def tearDown(self): self.tmp.cleanup()
    def add(self, arm, lines, trace=None):
        run = add_run(self.folder, arm, self.index, lines, purpose="network", presentmon=False, compare="n-observe"); self.index += 1
        with open(os.path.join(run, "run.json"), "w", encoding="utf-8-sig") as f: json.dump({"arm": arm, "purpose": "network", "compare": "n-observe", "switches": []}, f)
        if trace is not None:
            with open(os.path.join(run, report.TRACE_NAME), "w") as f: f.write(trace)
    def add_abba(self, a_lines, b_lines, a="n-observe", b="n-apply"):
        """Two runs per arm in the order A B B A (the session's default)."""
        for arm, lines in ((a, a_lines[0]), (b, b_lines[0]), (b, b_lines[1]), (a, a_lines[1])): self.add(arm, lines)
    def net(self): return report.network_tables(report.load(self.folder))
    def verdict(self, net, arm, metric): return next(v["verdict"] for v in net["arms"][arm]["verdicts"] if v["metric"] == metric)

    def test_fewer_stutters_called_and_latency_unchanged_not_invented(self):
        rng = random.Random(1)
        self.add("n-observe", network_log(rng, "observe", stutter=4.0), trace=TRACE_HEADER + "\n" + TRACE_ROW + "\n" + TRACE_ROW.replace("16.6", "-1") + "\n")
        self.add("n-apply", network_log(rng, "apply", stutter=1.0))
        self.add("n-apply", network_log(rng, "apply", stutter=1.0))
        self.add("n-observe", network_log(rng, "observe", stutter=4.0))
        net = self.net()
        self.assertEqual(net["route"], "wifi")
        self.assertIn("better than n-observe", self.verdict(net, "n-apply", "stutterPerMinute"))
        self.assertTrue(self.verdict(net, "n-apply", "stutterPerMinute").startswith(report.WIFI_LABEL))
        self.assertNotIn("better", self.verdict(net, "n-apply", "arrivalToScanoutP95")); self.assertNotIn("WORSE", self.verdict(net, "n-apply", "arrivalToScanoutP95"))
        f = net["arms"]["n-apply"]["facts"]
        self.assertEqual(f["profiles"], {"unstable-wifi": 100.0}); self.assertGreater(f["reorderChanges"], 10); self.assertAlmostEqual(f["addedWaitMs"], 4 * 20 / 60)
        self.assertEqual(self.verdict(net, "n-apply", "gapP95").split("; ")[-1], "descriptive (no verdict)")
        self.assertIn("Unstable Wi-Fi", net["arms"]["n-observe"]["connection"]); self.assertIn(report.WIFI_LABEL, net["arms"]["n-observe"]["connection"])
        self.assertIn("lowering graphics settings would not help", net["arms"]["n-observe"]["connection"])
        self.assertEqual(net["arms"]["n-observe"]["traces"][0]["summary"]["rows"], 2)
        sections = report.network_sections(net)
        self.assertEqual(sections[0][1][0], report.WIFI_LABEL)

    def test_cross_check_compares_verdict_words_and_motion(self):
        rng = random.Random(7)
        self.add_abba([network_log(rng, "observe", stutter=4.0) for _ in "ab"], [network_log(rng, "apply", stutter=1.0) for _ in "ab"])
        net = self.net()
        theirs = {"arms": {a: {"metrics": {k: {"n": m["n"], "mean": m["mean"]} for k, m in row["metrics"].items()},
                               "verdicts": [{"metric": v["metric"], "verdict": v["verdict"]} for v in row["verdicts"]],
                               "facts": {"motion": row["facts"]["motion"], "motionBasis": row["facts"]["motionBasis"]}} for a, row in net["arms"].items()}}
        def notes():
            with open(os.path.join(self.folder, "result.json"), "w", encoding="utf-8-sig") as f: json.dump({"networkSession": theirs}, f)
            return self.notes()
        self.assertTrue(any("network session and this report agree" in n for n in notes()))
        theirs["arms"]["n-apply"]["verdicts"][3]["verdict"] = "no difference shown"
        self.assertTrue(any("DISAGREE" in n and "verdict" in n for n in notes()))
        theirs["arms"]["n-apply"]["verdicts"][3]["verdict"] = net["arms"]["n-apply"]["verdicts"][3]["verdict"]
        theirs["arms"]["n-apply"]["facts"]["motion"] = False
        self.assertTrue(any("DISAGREE" in n and "motion" in n for n in notes()))
    def notes(self):
        from test_report import make
        return make(self.folder)[2]

    def test_latency_worse_is_reported_first(self):
        rng = random.Random(2)
        self.add_abba([network_log(rng, "observe", stutter=4.0, latency=10.0) for _ in "ab"], [network_log(rng, "apply", stutter=1.0, latency=14.0) for _ in "ab"])
        net = self.net()
        self.assertTrue(net["arms"]["n-apply"]["latencyWorse"])
        section = report.network_sections(net)[1]
        self.assertTrue(any(p.startswith("LATENCY WORSE") for p in section[1]))

    def test_still_scene_and_other_settings_are_not_comparable(self):
        rng = random.Random(3)
        self.add_abba([network_log(rng, "observe", stutter=4.0, video=2.0) for _ in "ab"], [network_log(rng, "apply", stutter=1.0) for _ in "ab"])
        net = self.net()
        self.assertIn("hardly moved", self.verdict(net, "n-apply", "stutterPerMinute"))
        self.assertIn(report.MOTION_ADVICE, report.network_sections(net)[1][1])
        self.assertEqual(net["arms"]["n-apply"]["metrics"]["stutterPerMinute"]["n"], 120)    # the numbers are still there
        self.tearDown(); self.setUp()
        self.add_abba([network_log(rng, "observe", stutter=4.0) for _ in "ab"], [network_log(rng, "apply", stutter=1.0, queue=0) for _ in "ab"])
        self.assertIn("not comparable: lowQueue", self.verdict(self.net(), "n-apply", "stutterPerMinute"))

    def test_original_build_only_on_shared_metrics_and_a_bad_trace_is_flagged(self):
        rng = random.Random(4)
        self.add("n-observe", network_log(rng, "observe"), trace=TRACE_HEADER + "\n" + TRACE_ROW.replace("16.6", "PlantedWord") + "\n")
        self.add("n-original", network_log(rng, "observe", smartlink=False))
        net = self.net()
        self.assertIn("n-original", net["builds"]); self.assertIn("n-observe", net["builds"])
        self.assertAlmostEqual(net["builds"]["n-original"]["presentToScanoutP50"], 9.1)
        self.assertIn("Not compared", net["notCompared"])
        self.assertTrue(net["arms"]["n-observe"]["traces"][0]["problem"])
        self.assertTrue(any("trace" in why for _f, _n, why in report.leak_findings(self.folder)))

    def test_the_smartlink_line_reaches_the_zip_and_names_do_not(self):
        kept = report.safe_lines([line(1, "slipstream-smartlink", "profile=strong-wifi regime=stable risk=0.1 rttMs=4 mode=observe reasons=none"),
                                  line(2, "slipstream-smartlink", "profile=strong-wifi ssid=PlantedNet")])
        self.assertEqual(len(kept), 1)

    def test_wifi_streaming_mode_refused_is_not_an_error_and_is_never_a_default_arm(self):
        rng = random.Random(5)
        lines = [l.replace("reasons=spikes", "reasons=spikes wifiStreamingMode=refused") for l in network_log(rng, "apply")]
        self.add("n-observe", lines)
        text = self.net()["arms"]["n-observe"]["connection"]
        self.assertIn("the adapter did not accept it (not an error)", text)
        table = report.read_arm_table(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts", "remoteplay", "testkit", "arms.csv"))
        self.assertEqual([n for n, r in table.items() if "WIFI_STREAMING_MODE" in r["switches"]], ["n-wifi-mode"])
        self.assertEqual(table["n-wifi-mode"]["set"], "network-wifi-mode")

    def test_rules(self):
        self.assertEqual(report.trace_problem.__doc__.count("Test-TraceNumbers"), 1)
        self.assertEqual(report.welch_interval([1.0, 1.0], [2.0, 2.0]), (-1.0, -1.0, -1.0))
        d, low, high = report.welch_interval([1.0, 3.0], [2.0, 2.0])     # df 1: t = 25.452, se 1
        self.assertAlmostEqual(d, 0.0); self.assertAlmostEqual(high, 25.452)
        self.assertTrue(all(v != v for v in report.welch_interval([1.0], [2.0, 3.0])))
        self.assertEqual(report.rate_per_minute([{"t": 0, "pops": 0}, {"t": 30, "pops": 2}, {"t": 60, "pops": 2}], "pops"), [4.0, 0.0])
        # per-window counts of the audio stream (underrunStarts=, tails=) are never taken as running totals
        self.assertEqual(report.rate_per_minute([{"t": 0, "underrunStarts": 1}, {"t": 10, "underrunStarts": 2}, {"t": 20, "underrunStarts": 3}], "underrunStarts", per_window=True), [12.0, 18.0])
        log = report.parse_log([line(t, "remoteplay-audio", f"pops={t // 10} clicks={t // 20} concealed=0 underrunStarts=1 hardResets=0 tails=0 queueP50=20 delayMs={40 + t // 10} underruns=0 resets=0") for t in range(10, 120, 10)])
        w, _f = report.network_windows([{"log": log}])
        self.assertTrue(w["audioDelayMs"]); self.assertAlmostEqual(w["underrunStartsPerMinute"][0], 6.0); self.assertTrue(w["clicksPerMinute"])
        names = [k for k, _l, _x in report.NETWORK_METRICS]
        self.assertLess(names.index("audioDelayMs"), names.index("popsPerMinute"))       # the cost before any gain in dropouts

KIT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts", "remoteplay", "testkit", "run-tests.ps1")

class TraceFiles(unittest.TestCase):
    """REVIEW_kit2 blocker 2 and should-fix 9: a run's traces are whole files written after it started; only the app's
    exact trace passes the privacy check (reviewer's cases from review-kit2/advtrace.py)."""
    def setUp(self): self.tmp = tempfile.TemporaryDirectory(); self.logs = self.tmp.name
    def tearDown(self): self.tmp.cleanup()
    def write(self, name, rows, when):
        path = os.path.join(self.logs, name)
        with open(path, "w", newline="\n") as f: f.write(TRACE_HEADER + "\n" + "".join(TRACE_ROW + "\n" for _ in range(rows)))
        os.utime(path, (when, when)); return path
    def rotate(self):   # openTrace: .2 removed, .1 -> .2, .csv -> .1 (renames keep the write time)
        p = lambda n: os.path.join(self.logs, n)
        if os.path.exists(p("smartlink-trace.2.csv")): os.remove(p("smartlink-trace.2.csv"))
        if os.path.exists(p("smartlink-trace.1.csv")): os.rename(p("smartlink-trace.1.csv"), p("smartlink-trace.2.csv"))
        if os.path.exists(p("smartlink-trace.csv")): os.rename(p("smartlink-trace.csv"), p("smartlink-trace.1.csv"))
    def rows(self, paths): return [report.trace_summary(x)["rows"] for x in paths]
    def test_shorter_new_file_is_taken_whole(self):
        self.write("smartlink-trace.csv", 900, 1000)          # run 1
        self.rotate(); self.write("smartlink-trace.csv", 300, 2100)   # run 2 started at 2000: a shorter file
        got = report.select_run_traces(self.logs, 2000 - 2)
        self.assertEqual([os.path.basename(x) for x in got], ["smartlink-trace.csv"]); self.assertEqual(self.rows(got), [300])
    def test_longer_new_file_is_taken_whole(self):
        self.write("smartlink-trace.csv", 100, 1000)
        self.rotate(); self.write("smartlink-trace.csv", 900, 2100)
        got = report.select_run_traces(self.logs, 1998)
        self.assertEqual(self.rows(got), [900])
        self.assertEqual(report.trace_problem(got[0]), "")
    def test_reconnect_inside_a_run_keeps_both_parts(self):
        self.write("smartlink-trace.csv", 50, 1000)            # an older run
        self.rotate(); self.write("smartlink-trace.csv", 200, 2100)   # this run, first stream
        self.rotate(); self.write("smartlink-trace.csv", 400, 2300)   # reconnect inside the same run
        got = report.select_run_traces(self.logs, 1998)
        self.assertEqual([os.path.basename(x) for x in got], ["smartlink-trace.1.csv", "smartlink-trace.csv"]); self.assertEqual(self.rows(got), [200, 400])
    def test_no_new_file_takes_nothing(self):
        self.write("smartlink-trace.csv", 50, 1000)
        self.assertEqual(report.select_run_traces(self.logs, 1998), [])
    def test_the_kit_copies_whole_files_by_write_time(self):
        text = open(KIT, encoding="utf-8-sig").read()
        body = text[text.index("function Save-RunTrace"):text.index("# --- one run of one arm")]
        self.assertIn("LastWriteTimeUtc -lt $SinceUtc", body); self.assertNotIn("Seek", body); self.assertIn("CopyTo", body)
        self.assertIn("$traceFiles = @('smartlink-trace.2.csv', 'smartlink-trace.1.csv', 'smartlink-trace.csv')", text)
        self.assertEqual(tuple(report.TRACE_FILES), ("smartlink-trace.2.csv", "smartlink-trace.1.csv", "smartlink-trace.csv"))
    def test_only_the_apps_exact_trace_passes(self):
        ok = TRACE_HEADER + "\n" + TRACE_ROW + "\n"
        bad = {"other-header": "endMs,MyHomeWifi_5G\n1,2\n", "short-header": "a,b\n1,2.5\n",
               "arabic-digit": ok + TRACE_ROW.replace("200", "\u0663", 1) + "\n", "fullwidth": ok + TRACE_ROW.replace("200", "\uff13", 1) + "\n",
               "hex": ok + TRACE_ROW.replace("200", "0x1F", 1) + "\n", "exp": ok + TRACE_ROW.replace("200", "1e5", 1) + "\n",
               "quoted": ok + TRACE_ROW.replace("200", '"1"', 1) + "\n", "second-header": ok + TRACE_HEADER + "\n",
               "long": ok + TRACE_ROW + "," + "1" * 1000 + "\n", "infinity": ok + TRACE_ROW.replace("200", "infinity", 1) + "\n",
               "nan": ok + TRACE_ROW.replace("200", "nan", 1) + "\n", "underscore": ok + TRACE_ROW.replace("200", "1_000", 1) + "\n",
               "dot": ok + TRACE_ROW.replace("200", ".", 1) + "\n", "minus": ok + TRACE_ROW.replace("200", "-", 1) + "\n",
               "nbsp": ok + TRACE_ROW.replace("200", "\u00a01", 1) + "\n", "tab": ok + TRACE_ROW.replace(",200", ",\t200", 1) + "\n",
               "bom": "\ufeff" + ok, "header-with-name": TRACE_HEADER + ",MyHomeWifi\n" + TRACE_ROW + "\n"}
        path = os.path.join(self.logs, "t.csv")
        with open(path, "w", encoding="utf-8", newline="\n") as f: f.write(ok)
        self.assertEqual(report.trace_problem(path), "")
        for name, text in bad.items():
            with open(path, "w", encoding="utf-8", newline="\n") as f: f.write(text)
            self.assertTrue(report.trace_problem(path), name)
    def test_kit_and_report_use_the_same_header_and_limit(self):
        text = open(KIT, encoding="utf-8-sig").read()
        start = text.index("$traceHeader = "); block = text[start:text.index("$traceMaxLine", start)]
        self.assertEqual("".join(re.findall(r"'([^']*)'", block)), report.TRACE_HEADER)
        self.assertIn("$traceMaxLine = 28 * 22 + 2", text); self.assertEqual(report.TRACE_MAX_LINE, 618)
        self.assertIn("-cnotmatch '^-?[0-9]+(\\.[0-9]+)?$'", text); self.assertNotIn("\\d", report.TRACE_FIELD.pattern)

class IdenticalArms(unittest.TestCase):
    """Acceptance test of the review (REVIEW_kit2 blocker 1): arms from the same process must give 'better' or 'WORSE' in
    no more than about 5 % of sessions. 200 simulated sessions of A B B A, 3-minute runs."""
    SESSIONS = 200
    def rate(self, rho, arms=(("n-observe", "observe"), ("n-apply", "apply")), seed=100):
        hits, latency = 0, 0
        with tempfile.TemporaryDirectory() as root:
            for k in range(self.SESSIONS):
                rng = random.Random(seed + k); state = [0.0, 0.0]
                d = os.path.join(root, str(k)); os.makedirs(d)
                with open(os.path.join(d, "arms.csv"), "w") as f: f.write("\n".join(TABLE) + "\n")
                (a, ma), (b, mb) = arms
                for i, (arm, mode) in enumerate(((a, ma), (b, mb), (b, mb), (a, ma))):
                    run = add_run(d, arm, i, null_log(rng, mode, rho, state), purpose="network", presentmon=False, compare="n-observe")
                    with open(os.path.join(run, "run.json"), "w") as f: json.dump({"arm": arm, "purpose": "network", "compare": "n-observe", "switches": []}, f)
                net = report.network_tables(report.load(d))
                vs = [v["verdict"] for v in net["arms"][b]["verdicts"]]
                hits += any(("better" in v) or ("WORSE" in v) for v in vs)
                latency += any(v["metric"].startswith("arrivalToScanout") and report.verdict_word(v["verdict"]) not in ("no difference shown", "cannot tell") for v in net["arms"][b]["verdicts"] if v["primary"])
        return hits / self.SESSIONS, latency / self.SESSIONS
    def test_drifting_wifi(self):
        rate, _l = self.rate(0.9); print(f"identical arms, rho 0.9: verdict in {rate * 100:.1f} % of {self.SESSIONS} sessions")
        self.assertLessEqual(rate, 0.06)
    def test_no_drift(self):
        rate, _l = self.rate(0.0, seed=900); print(f"identical arms, rho 0: verdict in {rate * 100:.1f} % of {self.SESSIONS} sessions")
        self.assertLessEqual(rate, 0.06)
    def test_observe_against_off(self):
        rate, latency = self.rate(0.9, arms=(("n-observe", "observe"), ("n-off", "off")), seed=5000)
        self.assertLessEqual(latency, 0.04)      # "no difference shown" or "cannot tell" for latency in (nearly) every identical session
    def test_one_run_per_arm_is_indication_only(self):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "arms.csv"), "w") as f: f.write("\n".join(TABLE) + "\n")
            rng = random.Random(1); state = [0.0, 0.0]
            for i, (arm, mode) in enumerate((("n-observe", "observe"), ("n-apply", "apply"))):
                run = add_run(d, arm, i, null_log(rng, mode, 0.9, state), purpose="network", presentmon=False, compare="n-observe")
                with open(os.path.join(run, "run.json"), "w") as f: json.dump({"arm": arm, "purpose": "network", "compare": "n-observe", "switches": []}, f)
            net = report.network_tables(report.load(d))
            for v in net["arms"]["n-apply"]["verdicts"]:
                if v["primary"]: self.assertIn("indication only: needs more runs (1 and 1 runs", v["verdict"])

class Power(unittest.TestCase):
    """REVIEW_final2 blocker 2 (reviewer's power.py): with 2 runs per arm the Welch 97.5 % interval almost never finds even
    a 3 SD effect; with 6 runs per arm it nearly always does. The wording says which case a session is in."""
    def power(self, n, effect, sims=1500, seed=1):
        rng = random.Random(seed); hit = 0
        for _ in range(sims):
            a = [rng.gauss(0, 1) for _ in range(n)]; b = [rng.gauss(effect, 1) for _ in range(n)]
            _d, low, high = report.welch_interval(b, a)
            hit += low > 0
        return hit / sims
    def test_two_runs_have_no_power_and_six_runs_have_high_power(self):
        self.assertLess(self.power(2, 3.0), 0.05)
        self.assertLess(self.power(2, 1.0), 0.02)
        self.assertGreater(self.power(4, 3.0), 0.6)
        self.assertGreater(self.power(6, 3.0), 0.93)
    def test_runs_needed_from_the_spread(self):
        a, b = [0.0, 2.0], [0.0, 2.0]           # SD sqrt(2)
        sd = math.sqrt(2.0)
        self.assertEqual(report.runs_needed(a, b, 3 * sd), 4)
        self.assertEqual(report.runs_needed(a, b, 0.1 * sd), report.NETWORK_RUNS_CAP + 1)
        self.assertEqual(report.runs_needed([5.0, 5.0], [5.0, 5.0], 1.0), 2)
    def test_wording_for_each_case(self):
        key = "arrivalToScanoutP95"
        wide = report.no_difference_text(key, [15.6, 16.4], [15.7, 16.3], *report.welch_interval([15.6, 16.4], [15.7, 16.3])[1:])
        self.assertTrue(wide.startswith("cannot tell with 2 runs per arm: this session would need about "), wide)
        self.assertIn("runs per arm to show a difference of 1.00 ms", wide); self.assertNotIn("no difference shown", wide)
        n = int(re.search(r"about (\d+) runs", wide).group(1)); self.assertGreater(n, 2)
        huge = report.no_difference_text(key, [10.0, 30.0], [10.0, 30.0], *report.welch_interval([10.0, 30.0], [10.0, 30.0])[1:])
        self.assertIn("would need more than 20 runs per arm", huge)
        ra, rb = [16.0, 16.1, 16.05, 15.95, 16.02, 16.0], [16.0, 16.05, 15.98, 16.03, 16.0, 15.97]
        narrow = report.no_difference_text(key, ra, rb, *report.welch_interval(ra, rb)[1:])
        self.assertTrue(narrow.startswith("no difference shown (interval +-"), narrow); self.assertIn("narrower than 1.00 ms", narrow)
        # stutter: 30 % of the reference arm's mean, at least 0.5 per minute
        self.assertAlmostEqual(report.effect_size("stutterPerMinute", [10.0, 20.0]), 4.5)
        self.assertAlmostEqual(report.effect_size("stutterPerMinute", [0.0, 0.2]), 0.5)
        self.assertEqual(report.verdict_word(wide), "cannot tell"); self.assertEqual(report.verdict_word(narrow), "no difference shown")
    def test_kit_uses_the_same_numbers_and_words(self):
        text = open(KIT, encoding="utf-8-sig").read()
        self.assertIn("$networkEffect = @{ 'stutterPerMinute' = @('relative', 0.30, 0.5); 'arrivalToScanoutP95' = @('absolute', 1.0, 0.0) }", text)
        self.assertEqual(report.NETWORK_EFFECT, {"stutterPerMinute": ("relative", 0.30, 0.5), "arrivalToScanoutP95": ("absolute", 1.0, 0.0)})
        self.assertIn("$networkRunsCap = " + str(report.NETWORK_RUNS_CAP), text); self.assertIn("$powerZ = " + str(report.POWER_Z), text)
        for words in ("'cannot tell with ' + $n + ' runs per arm: this session would need ' + $more + ' runs per arm to show a difference of ' + $what",
                      "'no difference shown (interval +-' + (Format-Number $half 2) + ', narrower than ' + $what + ')'", "$m = [math]::Max((Get-RunsNeeded $A $B $effect), $n + 1)",
                      "'stutterPerMinute' = 'stutter events per minute'; 'arrivalToScanoutP95' = 'ms'"):
            self.assertIn(words, text)
        self.assertNotIn("$verdict = 'no difference shown'", text)
        proc = open(os.path.join(os.path.dirname(KIT), "..", "..", "..", "docs", "HARDWARE_TEST_PROCEDURE.md"), encoding="utf-8").read()
        self.assertIn("| stutter events per minute | 30 % of the reference arm's mean, at least 0.5 per minute |", proc)
        self.assertIn("| in-app latency P95 | 1 ms |", proc)

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")

def health_line(t, rtt=6.4, target=15000, measured=9000, reports=12, age=300):
    """[remoteplay-health] built from ConnectionHealth.h formatHealthLine's format strings (the parts the kit reads)."""
    return line(t, "remoteplay-health",
        "grade=good interval=good seconds=5.0 lossPercent=0.000 lost=0 received=1500 frameJitterMs=1.20 frameJitterPeakMs=4.00 "
        "reordered=0 late=0 reorderTimeouts=0 skipped=0 fecRecovered=0 fecFailed=0 keyframeRequests=0 decoderResets=0 "
        "frameDrops=0 ingressDepth=0 ingressPeak=1 mailboxOverwrites=0 mbps=9.00 fps=60.0 rtt=4.10ms(at-connect) route=direct-lan link=wifi linkMbps=866"
        " bottleneck=none signs=none"
        f" overshootMs=0.00 decodeP95Ms=2.10 lateArrivals=0 routeClass=wifi consoleRttMs={rtt:.1f} consoleTargetKbps={target:.0f} consoleMeasuredKbps={measured:.0f} consoleReports={reports} consoleAgeMs={age:.0f}")

def units_line(t, rtt="ms", rate="kbit/s"):
    """ConsoleQuality.h takeLogLine's format string."""
    return line(t, "remoteplay-health", f"consoleUnits rtt={rtt} rate={rate} basis=reports:20 rttVotes:3/0 rateVotes:0/3/0 connectRttMs:4.100 requestedKbps:15000")

class ConsoleFallback(unittest.TestCase):
    """REVIEW_final2 should-fix: without SmartLink lines the round trip and the console's rates come from the health line's
    console* fields; an undecided unit gives 'not known in this session', never numbers."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name; self.index = 0
        with open(os.path.join(self.folder, "arms.csv"), "w") as f: f.write("\n".join(TABLE) + "\n")
    def tearDown(self): self.tmp.cleanup()
    def log(self, rng, rtt_unit="ms", rate_unit="kbit/s"):
        lines = network_log(rng, "observe", smartlink=False)
        lines += [health_line(10 + 5 * i, rtt=6.0 + i % 4) for i in range(30)]
        lines.append(units_line(40, rtt_unit, rate_unit))
        return lines
    def facts(self, **units):
        rng = random.Random(4)
        run = add_run(self.folder, "n-observe", 0, self.log(rng, **units), purpose="network", presentmon=False, compare="n-observe")
        with open(os.path.join(run, "run.json"), "w") as f: json.dump({"arm": "n-observe", "purpose": "network", "compare": "n-observe", "switches": []}, f)
        return report.network_tables(report.load(self.folder))
    def test_health_fields_fill_the_round_trip(self):
        net = self.facts(); f = net["arms"]["n-observe"]["facts"]
        self.assertEqual(f["rtt"][0], 6.0); self.assertEqual(f["rtt"][-1], 9.0); self.assertEqual(f["targetKbps"], 15000); self.assertEqual(f["measuredKbps"], 9000)
        self.assertEqual(f["motionBasis"], "console target")
        row = report.network_sections(net)[0][3][0]; self.assertEqual(row[1], "6.0")
    def test_undecided_units_are_not_known(self):
        net = self.facts(rtt_unit="undecided", rate_unit="undecided"); f = net["arms"]["n-observe"]["facts"]
        self.assertEqual(f["rtt"], []); self.assertTrue(f["rttUndecided"]); self.assertTrue(f["rateUndecided"]); self.assertNotEqual(f["motionBasis"], "console target")
        row = report.network_sections(net)[0][3][0]
        self.assertEqual(row[1:5], [report.UNITS_UNKNOWN] * 4); self.assertTrue(row[5].startswith(report.UNITS_UNKNOWN + " / " + report.UNITS_UNKNOWN))
    def test_fields_match_the_app_and_the_kit(self):
        health = open(os.path.join(ROOT, "include", "veyra", "remoteplay", "ConnectionHealth.h"), encoding="utf-8").read()
        for key in ("consoleRttMs=%.1f", "consoleTargetKbps=%.0f", "consoleMeasuredKbps=%.0f", "consoleReports=%llu", "consoleAgeMs=%.0f"): self.assertIn(key, health)
        quality = open(os.path.join(ROOT, "include", "veyra", "remoteplay", "ConsoleQuality.h"), encoding="utf-8").read()
        self.assertIn('"consoleUnits rtt=%s rate=%s basis=', quality); self.assertIn(':"undecided"', quality)
        kit = open(KIT, encoding="utf-8-sig").read()
        for words in ("Get-LogNumber $text 'consoleRttMs'", "Get-LogNumber $text 'consoleTargetKbps'", "Get-LogNumber $text 'consoleMeasuredKbps'",
                      "'^consoleUnits rtt=(\\S+) rate=(\\S+)'", "$unitsUnknown = '" + report.UNITS_UNKNOWN + "'", "if ($rttUndecided) { $rtt.Clear() }"):
            self.assertIn(words, kit)
        self.assertNotIn("Get-LogNumber $text 'rttMs'; if (($null -ne $v) -and ($v -ge 0)) { $healthRtt", kit)

if __name__ == "__main__":
    unittest.main()
