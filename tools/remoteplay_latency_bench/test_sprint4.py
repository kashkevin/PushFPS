#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Sprint 4 kit rules in report.py: comparable configurations, Wi-Fi labels, precision tables.
python3 -B tools/remoteplay_latency_bench/test_sprint4.py"""
import json, math, os, random, sys, tempfile, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import report
from test_report import add_run, line, presses, run_log, stamp

EVIDENCE = "/home/claude/evidence/e1/x"   # the first hardware run (not in the repository; the test is skipped without it)
GOLDEN = os.path.join(HERE, "..", "..", "scripts", "remoteplay", "testkit", "ci", "bootstrap-golden.json")

# The real lines of the first hardware run (19 Nov 2026): the original copy ran with the low-latency queue off.
PACING_ORIGINAL = ["revision=2 requested=false/0/1 effective=false/0/1 displayQueuePaced=true variableRefresh=true vrrLevel=4 vrrSync=false",
                   "DXGI enabled=true maximumLatency=1 vsync=true tearing=false hr=0x0 staleLatencyCounts=0"]
PACING_STABLE = ["revision=3 requested=true/0/1 effective=true/0/1 displayQueuePaced=true variableRefresh=true vrrLevel=4 vrrSync=false",
                 "DXGI enabled=true maximumLatency=1 vsync=true tearing=false hr=0x0 staleLatencyCounts=3"]
VRR = "level=4 known=true monitorSupportsVrr=true currentlyCapable=true trueGsync=false capsKnown=true possible=true requested=false active=false displayInVrrMode=true indicator=false simulated=false status=0 refreshHz=179.952 (state)"
CONFIG = ("mode=none fg=0 fgMethod=0 fgMultiplier=1 fgTier=0 sr=0 srQuality=0 enhance=0 lowQueue=1 nvLowLatency=0 displaySync=1 smoothPacing=1 vrrRequested=1 vrrLevel=4 "
          "fullscreen=1 window=2560x1440 output=2560x1440 refreshHz=179.952 hdr=0 stream=1920x1080@60 codec=hevc bitrateKbps=30000 decoder=D3D12VA route={route} trialsMax=40")

def arm_lines(values, pacing, at=0.0, extra=()):
    lines = run_log(values, at=at)
    lines += [line(at + 3, "pacing", p) for p in pacing] + [line(at + 3, "display-vrr", VRR), line(at + 3, "remoteplay-decode", "actual=D3D12VA pixelFormat=227 1920x1080 fallback=false")]
    return lines + list(extra)

def meta(run, arm, compare):
    with open(os.path.join(run, "run.json"), "w", encoding="utf-8-sig") as f:
        json.dump({"arm": arm, "purpose": "latency", "compare": compare, "presentMonStartUtc": "", "switches": [], "testProfile": True, "automatic": False, "trialsAsked": 12}, f)

class Folder(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name; self.index = 0
        self.hardware()
    def tearDown(self): self.tmp.cleanup()
    def hardware(self, media="802.3", description="Ethernet adapter"):
        with open(os.path.join(self.folder, "hardware.json"), "w", encoding="utf-8-sig") as f:
            json.dump({"computer": "(removed)", "network": [{"name": "(removed)", "description": description, "media": media, "defaultRoute": True}]}, f)
    def table(self, rows):
        with open(os.path.join(self.folder, "arms.csv"), "w", encoding="ascii") as f:
            f.write("name,exe,set,compare,switches,note,fg,differs\n" + "\n".join(rows) + "\n")
    def add(self, arm, lines, compare):
        add_run(self.folder, arm, self.index, lines, presentmon=False, compare=compare); self.index += 1
    def summary(self): return {r["name"]: r for r in report.result_summary(report.load(self.folder))["arms"]}

class Configuration(Folder):
    def test_inferred_keys_from_existing_lines(self):
        log = report.parse_log(arm_lines([500.0] * 20, PACING_ORIGINAL))
        config = report.normal_config(log)
        self.assertEqual(config["lowQueue"], "0"); self.assertEqual(config["displaySync"], "1"); self.assertEqual(config["vrrRequested"], "1")
        self.assertEqual(config["presentMaxLatency"], "1"); self.assertEqual(config["presentVsync"], "1"); self.assertEqual(config["presentTearing"], "0")
        self.assertEqual(config["vrrActive"], "0"); self.assertEqual(config["refreshHz"], "180"); self.assertEqual(config["codec"], "hevc")
        self.assertEqual(config["stream"], "1920x1080@60"); self.assertEqual(config["decoder"], "hardware")

    def test_low_queue_difference_refuses_with_the_key_and_a_fix(self):
        self.table(["original,original,quick,mod-legacy,,,off,", "mod-legacy,modified,quick,mod-legacy,VEYRA_TEST_RP_LEGACY_TIMING=1,,off,", "mod-all,modified,quick,mod-legacy,,,off,"])
        rng = random.Random(3)
        for _ in range(3):
            self.add("original", arm_lines(presses(rng, 550, 40), PACING_ORIGINAL), "mod-legacy")
            self.add("mod-legacy", arm_lines(presses(rng, 550, 40), PACING_STABLE), "mod-legacy")
            self.add("mod-all", arm_lines(presses(rng, 550, 40), PACING_STABLE), "mod-legacy")
        s = self.summary()
        self.assertTrue(s["original"]["refused"]); self.assertIn("lowQueue 0 in original, 1 in mod-legacy", s["original"]["verdict"]); self.assertIn("test profile", s["original"]["verdict"])
        self.assertFalse(s["mod-all"]["refused"], s["mod-all"]["verdict"])

    def presentation_session(self, full_differs, queue_differs):
        """p-window (windowed, variable refresh not active), p-fullscreen (full screen, active) and p-lowqueue-off, as the app logs them."""
        self.table(["p-window,experimental,switches,p-window,,,off,", "p-fullscreen,experimental,switches,p-window,,,off," + full_differs,
                    "p-lowqueue-off,experimental,switches,p-window,,,off," + queue_differs])
        full = CONFIG.format(route="wired")
        windowed = full.replace("fullscreen=1 window=2560x1440", "fullscreen=0 window=1600x900")
        rng = random.Random(11)
        for _ in range(3):
            self.add("p-window", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", windowed)]), "p-window")
            self.add("p-fullscreen", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", full), line(5, "display-vrr", VRR.replace("active=false", "active=true"))]), "p-window")
            self.add("p-lowqueue-off", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", windowed.replace("lowQueue=1", "lowQueue=0"))]), "p-window")
        return self.summary()

    def test_presentation_arms_are_compared_when_what_they_change_is_declared(self):
        s = self.presentation_session("fullscreen;window;vrrActive;vrrLevel;presentMode", "lowQueue;nvLowLatency;presentWaitable;presentMaxLatency;displayQueuePaced")
        for name in ("p-window", "p-fullscreen", "p-lowqueue-off"): self.assertFalse(s[name]["refused"], s[name]["verdict"])

    def test_presentation_arms_are_refused_for_what_is_not_declared(self):
        s = self.presentation_session("fullscreen", "nvLowLatency")
        self.assertTrue(s["p-fullscreen"]["refused"]); self.assertIn("variable refresh state differs", s["p-fullscreen"]["verdict"])
        self.assertTrue(s["p-lowqueue-off"]["refused"]); self.assertIn("lowQueue 0 in p-lowqueue-off, 1 in p-window", s["p-lowqueue-off"]["verdict"])

    def test_the_presentation_arms_of_arms_csv_declare_what_going_full_screen_changes(self):
        rows = {}
        with open(os.path.join(HERE, "..", "..", "scripts", "remoteplay", "testkit", "arms.csv"), encoding="utf-8") as f:
            for row in f:
                if row.startswith("p-"): rows[row.split(",")[0]] = row.strip().split(",")[-1].split(";")
        for key in ("fullscreen", "window", "vrrActive", "vrrLevel", "presentMode"): self.assertIn(key, rows["p-fullscreen"])
        for key in ("lowQueue", "nvLowLatency"): self.assertIn(key, rows["p-lowqueue-off"])
        self.assertEqual(rows["p-window"], [""])
        for key in ("presentMode", "vrrActive", "vrrLevel"): self.assertIn(key, rows["p-test-uncovered"])   # the arm exists to change how Windows presents the window

    def test_config_line_overlays_and_a_mixed_pair_compares_on_shared_keys(self):
        self.table(["stable,modified,versus,stable,,,off,", "x-base,experimental,versus,stable,,,off,", "x-sr,experimental,versus,x-base,,,off,"])
        rng = random.Random(4)
        for _ in range(3):
            self.add("stable", arm_lines(presses(rng, 550, 40), PACING_STABLE), "stable")
            self.add("x-base", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", CONFIG.format(route="wired"))]), "stable")
            self.add("x-sr", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", CONFIG.format(route="wired").replace("sr=0", "sr=2"))]), "x-base")
        s = self.summary()
        # the stable build cannot show sr, enhance, fullscreen ...: not comparable, named (review of 16a2bfec, item 4)
        self.assertTrue(s["x-base"]["refused"]); self.assertIn("NOT CHECKED, logged by one side only", s["x-base"]["verdict"]); self.assertIn("sr", s["x-base"]["verdict"])
        self.assertTrue(s["x-sr"]["refused"]); self.assertIn("sr 2 in x-sr, 0 in x-base", s["x-sr"]["verdict"])

    def test_versus_can_give_a_verdict_with_profile_values_named(self):
        """REVIEW_kit2 should-fix 6: fg details do not count with fg off on both sides, srQuality not with sr off; HDR from
        the session line's codec; fullscreen verified from [ui-fullscreen]; sr / enhance assumed from the test profile."""
        self.table(["stable,modified,versus,stable,,,off,", "x-base,experimental,versus,stable,,,off,"])
        rng = random.Random(9)
        frames = [line(5, "frame-flow", "captureReceived=600 sourceAccepted=600 realPresented=600 generatedPresented=0")]
        for _ in range(3):
            run = add_run(self.folder, "stable", self.index, arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=frames + [line(4, "ui-fullscreen", "enabled=true")]), presentmon=False, compare="stable"); self.index += 1
            meta(run, "stable", "stable")
            cfg = CONFIG.format(route="wired").replace("nvLowLatency=0 ", "")
            run = add_run(self.folder, "x-base", self.index, arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=frames + [line(2, "slipstream-config", cfg)]), presentmon=False, compare="stable"); self.index += 1
            meta(run, "x-base", "stable")
        s = self.summary()["x-base"]
        self.assertFalse(s["refused"], s["verdict"])
        self.assertIn("sr, enhance assumed from the test profile", s["verdict"].replace("enhance, sr", "sr, enhance"))
        self.assertNotIn("fullscreen", s["assumed"])            # verified from the stable build's own line
        self.assertNotIn("REFUSED: REFUSED", s["verdict"])
        log = report.parse_log([line(1, "remoteplay", "session started host=x profile=1920x1080@60 codec=H265_HDR requestedBitrateKbps=30000")])
        c = report.normal_config(log); self.assertEqual((c["codec"], c["hdr"]), ("hevc", "1"))

    def test_the_switch_under_test_may_change_its_own_key(self):
        self.table(["x-base,experimental,switches,x-base,,,off,", "x-sr,experimental,switches,x-base,,,off,sr"])
        rng = random.Random(5)
        for _ in range(3):
            self.add("x-base", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", CONFIG.format(route="wired"))]), "x-base")
            self.add("x-sr", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", CONFIG.format(route="wired").replace("sr=0", "sr=2"))]), "x-base")
        self.assertFalse(self.summary()["x-sr"]["refused"])

    @unittest.skipUnless(os.path.isdir(os.path.join(EVIDENCE, "bench")), "the first hardware run's evidence is not on this machine")
    def test_the_real_first_run_is_rescored(self):
        data = report.load(EVIDENCE)
        s = {r["name"]: r for r in report.result_summary(data)["arms"]}
        self.assertTrue(s["original"]["refused"]); self.assertIn("lowQueue", s["original"]["verdict"])
        self.assertFalse(s["mod-all"]["refused"], s["mod-all"]["verdict"])
        self.assertEqual(report.network_route(data), "wifi")
        tables = report.precision_tables(data)
        self.assertIn("FEWER short seconds", tables["delivery"]["mod-all"]["verdict"], tables["delivery"]["mod-all"]["verdict"])

class WiFi(Folder):
    def test_wifi_default_route_labels_every_table_and_the_json(self):
        self.hardware(media="Native 802.11", description="Wireless adapter")
        self.table(["mod-legacy,modified,quick,mod-legacy,,,off,", "mod-all,modified,quick,mod-legacy,,,off,"])
        rng = random.Random(6)
        for _ in range(3):
            self.add("mod-legacy", arm_lines(presses(rng, 550, 40), PACING_STABLE), "mod-legacy"); self.add("mod-all", arm_lines(presses(rng, 550, 40), PACING_STABLE), "mod-legacy")
        data = report.load(self.folder)
        self.assertEqual(report.network_route(data), "wifi")
        sections, notes, _ = report.build(data)
        self.assertTrue(notes[0].startswith(report.WIFI_LABEL))
        for title in ("PC pipeline (arrival to scanout), software", "Frame delivery", "Audio"):
            section = next(s for s in sections if s[0] == title)
            self.assertEqual(section[1][0], report.WIFI_LABEL)
        # every section and every verdict cell (review of 16a2bfec, item 6), the software latency table included
        for heading, paragraphs, header, rows in sections:
            self.assertEqual(paragraphs[0], report.WIFI_LABEL, heading)
            for i, h in enumerate(header):
                if "verdict" in h.lower():
                    for row in rows: self.assertTrue(not row[i] or row[i].startswith(report.WIFI_LABEL), (heading, row[i]))
        out = os.path.join(self.folder, "out"); report.main([self.folder, "--out", out])
        with open(os.path.join(out, "report-result.json")) as f: result = json.load(f)
        self.assertEqual(result["network"]["route"], "wifi")
        self.assertTrue(result["tables"]["total"]["mod-all"]["verdict"].startswith(report.WIFI_LABEL))
        self.assertTrue(all(a["verdict"].startswith(report.WIFI_LABEL) for a in result["arms"]))

    def test_config_line_route_wins(self):
        log_lines = arm_lines([500.0] * 20, PACING_STABLE, extra=[line(2, "slipstream-config", CONFIG.format(route="wifi"))])
        self.add("mod-all", log_lines, "mod-all")
        self.assertEqual(report.network_route(report.load(self.folder)), "wifi")

class Precision(Folder):
    def test_bootstrap_matches_the_golden_file_the_kit_is_checked_against(self):
        with open(GOLDEN) as f: golden = json.load(f)
        got = report.bootstrap_quantiles(golden["values"], rounds=golden["rounds"], seed=golden["seed"])
        for key, (low, high) in got.items():
            self.assertAlmostEqual(low, golden[key][0], places=6); self.assertAlmostEqual(high, golden[key][1], places=6)

    def test_new_lines_fill_the_tables_and_a_build_without_them_says_so(self):
        self.table(["x-base,experimental,switches,x-base,,,off,", "x-fast,experimental,switches,x-base,,,off,", "stable,modified,versus,stable,,,off,"])
        rng = random.Random(8)
        for run in range(3):
            for arm, pc in (("x-base", 14.0), ("x-fast", 9.0)):
                extra = [line(20 + i, "slipstream-press", f"n={i} sendToArrivalMs={40 + rng.uniform(0, 8):.2f} arrivalToScanoutMs={pc + rng.uniform(0, 2):.2f} totalMs=60.0 phaseMs=3.1") for i in range(45)]
                extra += [line(10 + i * 5, "slipstream-delivery", "arrived=300 shown=298 replaced=2 gapP50=16.6 gapP95=21.0 gapMax=40.0 clumps=3 longGaps=1 shownGapP95=17.0 shownGapMax=33.0 policy=newest") for i in range(20)]
                extra += [line(10 + i * 5, "slipstream-latency", f"frames=300 arrivalToScanoutP50={pc} arrivalToScanoutP95={pc + 4} arrivalToScanoutP99={pc + 6} arrivalToScanoutMax=30 scanoutKnown=290 syncInterval=1 tearing=0 queueLatency=1 waitable=1 flip=independent") for i in range(20)]
                extra += [line(10 + i * 5, "remoteplay-audio", f"queueMs=20.0 underruns={i} resets=0 pops={i} concealed=0") for i in range(10)]
                extra += [line(9, "slipstream-autorun", "step=2 name=sr state=failed reason=unsupported")]
                self.add(arm, arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=extra), "x-base")
            self.add("stable", arm_lines(presses(rng, 550, 40), PACING_STABLE), "stable")
        tables = report.precision_tables(report.load(self.folder))
        self.assertEqual(tables["pipeline"]["x-fast"]["n"], 135)
        self.assertTrue(tables["pipeline"]["x-fast"]["verdict"].startswith("LOWER"), tables["pipeline"]["x-fast"]["verdict"])
        self.assertTrue(tables["console"]["x-fast"]["verdict"].startswith("no difference shown"), tables["console"]["x-fast"]["verdict"])
        self.assertGreater(tables["console"]["x-fast"]["mdd"], 0)
        self.assertEqual(tables["pipeline"]["stable"]["n"], 0)
        self.assertEqual(tables["audio"]["x-base"]["pops"], 27)   # running totals 0..9, last value per run
        self.assertAlmostEqual(tables["delivery"]["x-base"]["replacedPercent"], 100.0 * 2 / 300)
        self.assertTrue(tables["sr"]["unsupported"])
        sections = report.precision_sections(tables)
        self.assertIn("not available in this build", next(s for s in sections if s[0] == "Super resolution cost")[1][0])


# The [slipstream-config] line exactly as the app writes it (rc4 src/ui/bridge/PushAutorun.cpp, format string at line 207).
REAL_CONFIG = ("mode=ultra-low-latency fg=0 fgMethod=0 fgMultiplier=2 fgTier=1 sr=0 srQuality=0 enhance=0 lowQueue=1 nvLowLatency=1 displaySync=1 "
               "smoothPacing=1 vrrRequested=1 vrrLevel=4 fullscreen=1 window=2560x1440 output=2560x1440 refreshHz=179.95 hdr=0 stream=1920x1080@60.00 "
               "codec=hevc bitrateKbps=15000 decoder=hardware route=wifi trialsMax=500 modeSource=ini fgActive=0 srActive=0")

class Review16a2bfec(Folder):
    """Regression tests for the independent review of commit 16a2bfec (adversarial fixtures from /home/claude/build/review-kit)."""
    def test_real_line_normalises_stream_rate_and_decoder_kind(self):
        log = report.parse_log([line(1, "remoteplay", "session started host=192.168.x.x profile=1920x1080@60 codec=H265 requestedBitrateKbps=15000"),
                                line(2, "remoteplay-decode", "actual=D3D12VA pixelFormat=227 1920x1080 fallback=false")])
        old = report.normal_config(log)
        new = report.normal_config(report.parse_log([line(2, "slipstream-config", REAL_CONFIG)]))
        for key in ("stream", "decoder", "codec"): self.assertEqual(old[key], new[key], key)
        self.assertEqual(new["refreshHz"], "180")
        self.assertIsNone(report.normal_value("refreshHz", "-1")); self.assertIsNone(report.normal_value("decoder", "unknown"))
        self.assertEqual(report.normal_value("decoder", "software-fallback"), "software")

    def test_two_experimental_arms_with_the_real_line_compare(self):
        self.table(["x-base,experimental,switches,x-base,,,off,", "x-b,experimental,switches,x-base,,,off,"])
        rng = random.Random(9)
        for _ in range(3):
            for arm in ("x-base", "x-b"): self.add(arm, arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", REAL_CONFIG)]), "x-base")
        self.assertFalse(self.summary()["x-b"]["refused"], self.summary()["x-b"]["verdict"])

    def test_a_setting_changed_during_a_run_is_refused_and_start_up_is_not(self):
        self.table(["x-base,experimental,switches,x-base,,,off,", "x-b,experimental,switches,x-base,,,off,"])
        rng = random.Random(10)
        changed = REAL_CONFIG.replace("lowQueue=1", "lowQueue=0")
        for _ in range(3):
            self.add("x-base", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", REAL_CONFIG)]), "x-base")
            self.add("x-b", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", changed), line(400, "slipstream-config", REAL_CONFIG)]), "x-base")
        v = self.summary()["x-b"]
        self.assertTrue(v["refused"]); self.assertIn("lowQueue (changed during run", v["verdict"])
        # the real start-up sequence (DXGI off, then on, within a second of the session start) is not a change
        log = report.parse_log([line(1, "remoteplay", "session started profile=1920x1080@60 codec=H265 requestedBitrateKbps=15000"),
                                line(2, "pacing", "DXGI enabled=false maximumLatency=3 vsync=false tearing=false hr=0x0 staleLatencyCounts=0"),
                                line(2.1, "pacing", "DXGI enabled=true maximumLatency=1 vsync=true tearing=false hr=0x0 staleLatencyCounts=3")])
        self.assertEqual(log["config_changed"], set())

    def test_one_sided_latency_settings_are_not_comparable(self):
        # reviewer fixture adv.py case 1: SR on, full screen off, NVIDIA low latency, HDR in the experimental arm only
        bad = CONFIG.format(route="wired").replace("sr=0", "sr=2").replace("fullscreen=1", "fullscreen=0").replace("nvLowLatency=0", "nvLowLatency=1").replace("hdr=0", "hdr=1")
        self.table(["stable,modified,versus,stable,,,off,", "x-base,experimental,versus,stable,,,off,"])
        rng = random.Random(11)
        for _ in range(3):
            self.add("stable", arm_lines(presses(rng, 550, 40), PACING_STABLE), "stable")
            self.add("x-base", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", bad)]), "stable")
        v = self.summary()["x-base"]
        self.assertTrue(v["refused"])
        # HDR is known for both builds (the codec of the session line), so it is the difference that is named.
        self.assertIn("hdr", v["verdict"])

    def test_a_setting_logged_by_one_side_only_is_never_passed_silently(self):
        # Super resolution, full screen and enhancement are logged by the experimental build only: on their own they
        # must end as REFUSED or as a named "NOT CHECKED" / "assumed" item in the verdict, never as a clean comparison.
        for key, was, now in (("sr", "sr=0", "sr=2"), ("fullscreen", "fullscreen=1", "fullscreen=0"), ("enhance", "enhance=0", "enhance=1")):
            with self.subTest(key=key):
                self.tearDown(); self.setUp()
                bad = CONFIG.format(route="wired").replace(was, now)
                self.assertNotEqual(bad, CONFIG.format(route="wired"))
                self.table(["stable,modified,versus,stable,,,off,", "x-base,experimental,versus,stable,,,off,"])
                rng = random.Random(11)
                for _ in range(3):
                    self.add("stable", arm_lines(presses(rng, 550, 40), PACING_STABLE), "stable")
                    self.add("x-base", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=[line(2, "slipstream-config", bad)]), "stable")
                v = self.summary()["x-base"]
                self.assertIn(key, v["verdict"])
                self.assertTrue(v["refused"] or "NOT CHECKED" in v["verdict"] or "assumed" in v["verdict"], v["verdict"])

class Review16a2bfecStatistics(unittest.TestCase):
    def test_mdd_is_what_the_overlap_rule_needs_and_delivery_is_blocked_by_run(self):
        self.assertEqual(report.mdd(10.0, 20.0), 15.0)
        self.assertTrue(all(x != x for x in report.run_block_interval([(1, 100), (2, 100)])))      # fewer than 3 runs
        low, high = report.run_block_interval([(10, 100), (30, 100), (20, 100)])
        self.assertLessEqual(10.0, low); self.assertLessEqual(high, 30.0); self.assertLess(low, high)
        self.assertIn("cannot resolve a few milliseconds", report.BUTTON_TEST_NOTE)

class PresentationArms(Folder):
    """docs/PRESENTATION_LATENCY.md section 6 and docs/SUPER_RESOLUTION.md (rc4): composed or independent flip, the
    tearing arm scored only with PresentMon, super resolution holds reported as measured values."""
    def test_presentation_table_tearing_without_presentmon_and_sr_holds(self):
        self.table(["p-fullscreen,experimental,switches,p-fullscreen,,,off,fullscreen", "p-tearing,experimental,switches,p-fullscreen,VEYRA_EXP_PRESENT_TEARING=1,,off,presentTearing;presentVsync;presentMaxLatency;presentWaitable"])
        rng = random.Random(12)
        extra = [line(5, "slipstream-swapchain", "flip=flip-discard buffers=3 format=28 waitable=1 tearingSupported=1 syncInterval=1 flags=0x840 fullscreen=1 windowCoversMonitor=1 reason=created"),
                 line(6, "slipstream-latency", "frames=300 presentToScanoutP50=3.1 presentToScanoutP95=5.0 arrivalToScanoutP50=9.0 arrivalToScanoutP95=12.0"),
                 line(7, "slipstream-sr", "event=hold-end index=0 label=nis-4k valid=1 seconds=15.0 fps=59.90 presents=900 gaps=0 latencyP50Ms=10.500 latencyP95Ms=13.000 latencySamples=900"),
                 line(8, "slipstream-sr", "choice=6 quality=0 in=1920x1080 out=3840x2160 gpuP50=1.200 gpuP95=1.600 frames=900 generatedToo=0 gpuMax=2.000 work=3840x2160 hdr=0 dual=0 levelSource=user budgetMs=5.00")]
        for _ in range(3):
            self.add("p-fullscreen", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=extra), "p-fullscreen")
            self.add("p-tearing", arm_lines(presses(rng, 550, 40), PACING_STABLE, extra=extra), "p-fullscreen")
        tables = report.precision_tables(report.load(self.folder))
        self.assertTrue(tables["total"]["p-tearing"]["verdict"].startswith("not scored: the tearing arm needs PresentMon"))
        row = tables["presentation"]["p-fullscreen"]
        self.assertEqual((row["flip"], row["fullscreen"], row["windowCoversMonitor"]), ("flip-discard", "1", "1"))
        self.assertAlmostEqual(row["appPresentToScanoutP50"], 3.1)
        self.assertEqual(tables["sr"]["rows"][0]["out"], "3840x2160")
        self.assertIn("nis-4k: in-app latency P50 10.50 ms", " ".join(sum((s[1] for s in report.precision_sections(tables)), [])))


class AlternativeButtons(unittest.TestCase):
    """The alternative latency buttons (VEYRA_EXP_RP_LATENCY_BUTTONS=1, PushLatencyTest.cpp) log refusals, sham presses and
    'trials invalid=1': none of these is ever a press or a test, in report.py or in any press pattern of run-tests.ps1."""
    LINES = ["refused reason=buttons-experimental button=options", "v2 button=options randomGapMs=0..33.3 split=1",
             "shams=6 (nothing pressed, judged like a press)", "sham 1 outcome=quiet", "sham 2 outcome=answered ms=80.0",
             "not valid reason=picture-changes-by-itself answered=8/8", "trials invalid=1 answerMs=[90.0,91.5,88.0]",
             "shams=6 shamAnswered=1 shamMoving=0 allowed=0 valid=0"]
    def test_never_counted_in_report(self):
        log = report.parse_log([line(10 + i, "push-latency", t) for i, t in enumerate(self.LINES)])
        self.assertEqual(log["presses"], []); self.assertEqual(log["summaries"], []); self.assertEqual(log["no_answer"], 0)
    def test_never_counted_by_the_kit(self):
        import re
        kit = open(os.path.join(HERE, "..", "..", "scripts", "remoteplay", "testkit", "run-tests.ps1"), encoding="ascii").read()
        patterns = {p for p in re.findall(r"'([^']*push-latency[^']*)'", kit) if ("trials answerMs" in p or "done answered" in p) and "\\[push-latency\\]" in p} | {r"^trials answerMs=\[([^\]]*)\]"}
        self.assertGreaterEqual(len(patterns), 5)
        for t in self.LINES:
            full = line(10, "push-latency", t)
            for p in patterns:
                self.assertIsNone(re.search(p, full if not p.startswith("^") else t), (p, t))
        self.assertIn("VEYRA_EXP_RP_LATENCY_BUTTONS is never used by the kit", kit)
        self.assertNotIn("VEYRA_EXP_RP_LATENCY_BUTTONS=", open(os.path.join(HERE, "..", "..", "scripts", "remoteplay", "testkit", "arms.csv")).read())
        self.assertIn("($item.Name -like 'VEYRA_EXP_RP_*')", kit)      # cleared when inherited from the environment
        self.assertNotRegex(kit, r"latency:\d+:(options|dpad)")       # the autorun plans name no other button


def latency_line(t, p50, p95, under, one_two, over):
    """[slipstream-latency] from PushPresentTiming.h formatSlipstreamLatency."""
    return line(t, "slipstream-latency", f"frames=300 arrivalToDecodedP50=2.000 arrivalToDecodedP95=3.000 decodedToPickupP50=0.500 decodedToPickupP95=1.000 "
        f"pickupToPresentCallP50=0.200 pickupToPresentCallP95=0.400 presentCallP50=0.100 presentCallP95=0.300 presentToScanoutP50={p50:.3f} presentToScanoutP95={p95:.3f} "
        f"arrivalToScanoutP50=12.000 arrivalToScanoutP95=16.000 arrivalToScanoutP99=18.000 arrivalToScanoutMax=30.000 scanoutKnown=290 syncInterval=1 tearing=0 queueLatency=1 "
        f"waitable=1 flip=composed scanoutUnder1Refresh={under} scanout1To2Refresh={one_two} scanoutOver2Refresh={over} refreshMs=5.557")

def window_line(t, covers, region, overlaps, blockers):
    """[slipstream-present] window ... from formatSlipstreamWindow (only with VEYRA_EXP_PRESENT_WINDOW_FACTS=1)."""
    return line(t, "slipstream-present", f"window event=periodic known=1 host=0,0,2560x1440 monitor=0,0,2560x1440 coversMonitor={covers} regionKind={region} regionRects=0 "
        f"overlay=0 ownOverlaps={overlaps} rootPopup=1 rootCaption=0 rootMaximized=0 rootTopmost=0 rootLayered=0 hostLayered=0 hostVisible=1 foreground=1 flipBlockers=0x{blockers:X}")

class ShortSessions(unittest.TestCase):
    def test_presentation_paragraph_reads_the_apps_lines(self):
        with tempfile.TemporaryDirectory() as d:
            for i, arm in enumerate(report.PRESENTATION_ARMS):
                lines = [latency_line(10 + 5 * k, 8.0 + i, 12.0 + i, 280, 15, 5) for k in range(6)]
                lines += [window_line(10 + 5 * k, 1 if arm == "p-fullscreen" else 0, 0, 1 if (arm == "p-window" and k < 2) else 0, 0 if arm == "p-fullscreen" else 4) for k in range(4)]
                add_run(d, arm, i, lines, presentmon=False, compare="p-window")
            with open(os.path.join(d, "kit.json"), "w") as f: json.dump({"session": "presentation"}, f)
            data = report.load(d)
            out = report.short_session_paragraphs(data, "presentation")
            self.assertTrue(out[0].startswith("SHORT SESSION: PRESENTATION (measured values only, no verdict word"))
            text = "\n".join(out)
            self.assertIn("p-window (1 runs): present to vblank P50 8.00 ms, P95 12.00 ms (dated presents only, DXGI statistics, not screen latency; medians of 6 windows of [slipstream-latency]); dated presents within 1 refresh 93.3 %, 1 to 2 refreshes 5.0 %, over 2 refreshes 1.7 %", text)
            self.assertIn("p-fullscreen (1 runs): present to vblank P50 9.00 ms", text)
            self.assertIn("Window facts: regionKind 0 in 100 % of 4 lines, coversMonitor 0 in 100 % of 4 lines, ownOverlaps 1 in 50 % of 4 lines, flipBlockers 0x4 in 100 % of 4 lines.", text)
            self.assertIn("coversMonitor 1 in 100 % of 4 lines", text)
            for word in ("better", "WORSE", "no difference"): self.assertNotIn(word, text)
            self.assertTrue(any(t.startswith("Short session: presentation") for t, *_r in report.build(data)[0]))
    def test_audio_pair_counts_clicks_per_minute(self):
        with tempfile.TemporaryDirectory() as d:
            for i, (arm, step) in enumerate((("s-base", 2), ("s-audio-smooth", 0))):
                add_run(d, arm, i, [line(10 + 30 * k, "remoteplay-audio", f"queueMs=20 underruns=0 resets=0 pops=0 concealed=0 clicks={step * k}") for k in range(5)], presentmon=False, compare="s-base")
            out = report.short_session_paragraphs(report.load(d), "audio")
            self.assertIn("  s-base (1 runs): clicks= rose by 8 in 2.0 min (4.00 per minute)", out)
            self.assertIn("  s-audio-smooth (1 runs): clicks= rose by 0 in 2.0 min (0.00 per minute)", out)

class PressVerdictWording(unittest.TestCase):
    """REVIEW_final2 blocker 2, applied to the per-press tables: overlapping P50 intervals say 'no difference shown' only
    when their MDD is below the smallest effect worth finding."""
    def row(self, n, p50, half): return {"n": n, "p50": p50, "p50Low": p50 - half, "p50High": p50 + half}
    def test_wording(self):
        wide = report.latency_verdict(self.row(120, 100.0, 8.0), self.row(120, 101.0, 8.0), "base", key="total")
        # MDD = the sum of the half-widths = 16 ms; 120 x (16 / 8.3)^2 = 446, in whole tests of 12: 456
        self.assertEqual(wide, "cannot tell with 120 presses per arm: this session would need about 456 presses per arm to show a difference of 8.3 ms (vs base)")
        wider = report.latency_verdict(self.row(120, 10.0, 1.5), self.row(120, 10.5, 1.5), "base", key="pipeline")
        self.assertIn("would need about 1080 presses per arm to show a difference of 1.0 ms", wider)
        huge = report.latency_verdict(self.row(120, 10.0, 9.0), self.row(120, 10.5, 9.0), "base", key="pipeline")
        self.assertIn("would need more than 2000 presses per arm", huge)
        narrow = report.latency_verdict(self.row(120, 10.0, 0.4), self.row(120, 10.2, 0.4), "base", key="pipeline")
        self.assertTrue(narrow.startswith("no difference shown: the 95 % intervals of the P50 overlap and could have shown 0.80 ms, below 1.0 ms"), narrow)
        self.assertEqual(report.verdict_word(wide), "cannot tell")
    def test_kit_has_the_same_numbers(self):
        kit = open(os.path.join(HERE, "..", "..", "scripts", "remoteplay", "testkit", "run-tests.ps1"), encoding="ascii").read()
        self.assertIn("$latencyEffect = @{ 'pipeline' = 1.0; 'console' = 2.0; 'total' = 8.3 }", kit)
        self.assertEqual(report.LATENCY_EFFECT, {"pipeline": 1.0, "console": 2.0, "total": 8.3})
        self.assertIn("$pressesCap = " + str(report.PRESSES_CAP), kit)
        self.assertIn("$m = [int]([math]::Ceiling($n * ($need / $effect) * ($need / $effect) / 12.0)) * 12", kit)
        self.assertIn("$row.verdict = Get-LatencyVerdict $row $other0 $entry.compare $key", kit)

if __name__ == "__main__":
    unittest.main()
