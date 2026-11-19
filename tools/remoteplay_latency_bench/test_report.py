#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Tests of report.py on made-up evidence folders.  python3 -B tools/remoteplay_latency_bench/test_report.py"""
import datetime, io, json, os, random, re, sys, tempfile, unittest, zipfile
from contextlib import redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report

T0 = datetime.datetime(2026, 11, 18, 12, 0, 0, tzinfo=datetime.timezone.utc)

def stamp(seconds): return (T0 + datetime.timedelta(seconds=seconds)).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"

def line(seconds, component, text, level="INFO "): return f"{stamp(seconds)} t=4242 [{level}] [{component}] {text}"

def presses(rng, true_mean, count):
    """Latency quantised to the console's 16.7 ms frames, as the built-in test sees it."""
    out = []
    for _ in range(count):
        raw = true_mean + rng.uniform(-8.35, 8.35) + rng.gauss(0, 2.0)
        out.append(round(round(raw / 16.7) * 16.7 + rng.uniform(0, 1.5), 1))
    return out

def run_log(values, at=0.0, per_press=True, own=6.0, extra=()):
    lines = [line(at + 1, "remoteplay", "session started host=192.168.x.x profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000"),
             line(at + 1, "remoteplay-timing", "ownerWait=event input=immediate engineWake=event (event-driven unless asked)")]
    for second in range(2, 130, 2):
        lines.append(line(at + second, "remoteplay-progress", "state=3 received=600 decoded=600 inputFps=59.9 decodeFps=59.9 videoMbps=28.100 receiveAgeMs=3.0 decodeAgeMs=5.0 decodeBusyMs=1.2 queue=0 waitingIdr=false idrRequests=0 errors=0 (x)"))
    for test, start in enumerate(range(0, len(values), 20)):
        chunk = values[start:start + 20]; begin = at + 10 + test * 60
        lines.append(line(begin, "push-latency", f"remote play latency test started trials={len(chunk)}"))
        mean = sum(chunk) / len(chunk); median = sorted(chunk)[len(chunk) // 2]
        lines.append(line(begin + 50, "push-latency", f"done answered={len(chunk)}/{len(chunk)} meanMs={mean:.1f} medianMs={median:.1f} minMs={min(chunk):.1f} maxMs={max(chunk):.1f} pushfpsMs={own:.1f}"))
        if per_press: lines.append(line(begin + 50, "push-latency", "trials answerMs=[" + ",".join(f"{v:.1f}" for v in chunk) + "]"))
    return lines + list(extra)

def presentmon_v2(path, seconds=130, fps=60.0, mode="Hardware: Independent Flip", drop_every=0, long_at=None, frame_type=None):
    header = ["Application", "ProcessID", "SwapChainAddress", "PresentMode", "AllowsTearing"] + (["FrameType"] if frame_type else []) + \
             ["CPUStartTime", "MsBetweenPresents", "MsBetweenDisplayChange", "MsUntilDisplayed", "DisplayedTime", "VideoBusy"]
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(",".join(header) + "\n")
        f.write("PushFPS.exe,1,0x1AA,Composed: Flip,0," + ("Application," if frame_type else "") + "0.5,16.0,16.0,9.0,16.0,0.0\n")   # the interface's own swap chain: one frame
        f.write("other.exe,2,0x2BB,Composed: Flip,0," + ("Application," if frame_type else "") + "0.5,7.0,7.0,9.0,7.0,0.0\n")
        frame = 1000.0 / fps
        for i in range(int(seconds * fps)):
            t = i / fps
            shown = frame * (30 if long_at is not None and abs(t - long_at) < 0.5 / fps else 1)
            dropped = drop_every and i % drop_every == drop_every - 1
            cells = ["PushFPS.exe", "1", "0x3CC", mode, "1"] + ([frame_type[i % len(frame_type)]] if frame_type else []) + \
                    [f"{t:.4f}", f"{frame:.3f}", "NA" if dropped else f"{shown:.3f}", "NA" if dropped else "4.200", "NA" if dropped else f"{shown:.3f}", "1.500"]
            f.write(",".join(cells) + "\n")

def samples(path, seconds=130, at=0.0, growth_mb_per_hour=0.0, gpu=True):
    with open(path, "w", encoding="ascii") as f:
        f.write("utc,cpuTotalPercent,appCpuPercent,appWorkingSetMb,appPrivateMb,appThreads,appHandles" + (",gpuPercent,gpuDecoderPercent,gpuEncoderPercent,vramUsedMb,vramTotalMb,gpuTempC,gpuPowerW" if gpu else "") + "\n")
        for s in range(seconds):
            row = f"{stamp(at + s)},12,3.5,900.0,{800.0 + growth_mb_per_hour * s / 3600.0:.2f},60,1500"
            f.write(row + (",35,11,0,2400,12282,55,[N/A]" if gpu else "") + "\n")

def add_run(folder, arm, index, lines, purpose="latency", presentmon=True, compare="mod-legacy", **pm):
    name = (arm if purpose == "latency" else f"{purpose}-{arm}") + f"@20261118-12{index:02d}00"
    os.makedirs(os.path.join(folder, "bench"), exist_ok=True)
    run = os.path.join(folder, "runs", name); os.makedirs(run, exist_ok=True)
    with open(os.path.join(folder, "bench", name + ".txt"), "w", encoding="utf-8-sig") as f: f.write("\n".join(lines) + "\n")
    with open(os.path.join(run, "run.json"), "w", encoding="utf-8-sig") as f:
        json.dump({"arm": arm, "purpose": purpose, "compare": compare, "presentMonStartUtc": stamp(0) if presentmon else "", "switches": []}, f)
    if presentmon: presentmon_v2(os.path.join(run, "presentmon.csv"), **pm)
    samples(os.path.join(run, "samples.csv"))
    return run

def evidence(folder, arms, kit=None):
    """arms: {name: [list of press lists, one per program start]}"""
    with open(os.path.join(folder, "kit.json"), "w", encoding="utf-8-sig") as f: json.dump(kit or {"presentMon": True, "nvidiaSmi": True, "elevated": True}, f)
    with open(os.path.join(folder, "hardware.json"), "w", encoding="utf-8-sig") as f:
        json.dump({"computer": "(removed)", "cpu": {"name": "Test CPU"}, "adapters": [{"name": "NVIDIA GeForce RTX 5070", "nvidiaDriver": "572.83", "width": 2560, "height": 1440, "refreshHz": 180}], "warnings": []}, f)
    index = 0
    for arm, starts in arms.items():
        for values in starts:
            add_run(folder, arm, index, run_log(values, per_press=arm != "original")); index += 1

def table(markdown, heading):
    """Rows of the table under '## heading', as lists of cells keyed by the first cell."""
    part = markdown.split(f"## {heading}\n", 1)[1].split("\n## ", 1)[0]
    rows = [[c.strip() for c in l.strip().strip("|").split("|")] for l in part.splitlines() if l.startswith("|") and not l.startswith("|---")]
    return {r[0]: dict(zip(rows[0], r)) for r in rows[1:]}

def make(folder, **kw):
    data = report.load(folder)
    sections, notes, leaks = report.build(data, **kw)
    return report.markdown(sections, notes, "t"), report.html_page(sections, notes, "t"), notes, leaks


class Redaction(unittest.TestCase):
    def test_private_range_prefix_is_kept_and_the_rest_masked(self):
        r = report.redact_line
        self.assertEqual(r("host=192.168.1.77 x"), "host=192.168.x.x x")
        self.assertEqual(r("to 10.20.30.40:9295"), "to 10.x.x.x:9295")
        self.assertEqual(r("a 172.16.5.4 b 172.31.255.1 c 172.32.0.1"), "a 172.16.x.x b 172.31.x.x c x.x.x.x")
        self.assertEqual(r("169.254.10.20 127.0.0.1"), "169.254.x.x 127.x.x.x")
        self.assertEqual(r("public 203.0.113.9 and 8.8.8.8"), "public x.x.x.x and x.x.x.x")
        self.assertEqual(r("driver 32.0.15.7283 Qt 6.8.3"), "driver 32.0.15.7283 Qt 6.8.3")          # versions are not addresses

    def test_other_identifiers(self):
        r = report.redact_line
        self.assertEqual(r("mac 00:1A:2B:3C:4D:5E end"), "mac [mac] end")
        self.assertEqual(r("mac 00-1a-2b-3c-4d-5e"), "mac [mac]")
        self.assertEqual(r("peer fe80::1ff:fe23:4567:890a x"), "peer [ipv6] x")
        self.assertEqual(r("peer 2001:db8:0:1:2:3:4:5"), "peer [ipv6]")
        self.assertEqual(r("key 00112233445566778899aabbccddeeff"), "key [hex]")
        self.assertEqual(r("pointer 0x00007FF6A1B2C3D4"), "pointer [hex]")
        self.assertEqual(r("start host=LivingRoomPS5 restart=false"), "start host=[name] restart=false")
        self.assertEqual(r("device name=Kevins-PC,"), "device name=[name],")
        self.assertEqual(r("2026-11-18T12:34:56.789Z t=1234 [INFO ] [remoteplay] decodeFps=59.9 queue=0"), "2026-11-18T12:34:56.789Z t=1234 [INFO ] [remoteplay] decodeFps=59.9 queue=0")

    def test_masking_is_stable(self):
        for text in ["host=192.168.1.77", "host=PlantedHostName", "fe80::1 00:1A:2B:3C:4D:5E 0123456789abcdef0123", "name=x host=203.0.113.9"]:
            once = report.redact_line(text)
            self.assertEqual(report.redact_line(once), once)

    def test_only_whitelisted_families_without_private_words(self):
        lines = [line(0, "app", "data folder C:\\Users\\PlantedUserName"), line(0, "remoteplay-discovery", "host-name=PlantedConsoleName host-id=AABBCCDDEEFF"),
                 line(0, "remoteplay-ui", "registration regist_key=0123456789abcdef0123"), line(0, "remoteplay", "account=PlantedAccountId="),
                 line(0, "wlan", "ssid=PlantedWifiName"), line(0, "remoteplay-control", "start host=192.168.1.77 restart=false"),
                 line(0, "remoteplay-kbm", "RegisterRawInputDevices failed; keyboard and mouse not captured"), line(0, "push-latency", "trials answerMs=[83.3,100.0]"),
                 line(0, "remoteplay", "psn token secret"), ""]
        kept = report.safe_lines(lines)
        self.assertEqual(len(kept), 3)
        text = "\n".join(kept)
        for planted in ("Planted", "0123456789abcdef", "192.168.1.77", "AABBCCDDEEFF"): self.assertNotIn(planted, text)
        self.assertIn("host=192.168.x.x", text); self.assertIn("RegisterRawInputDevices", text); self.assertIn("answerMs=[83.3,100.0]", text)

    def test_the_kit_script_uses_the_same_rules(self):
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts", "remoteplay", "testkit", "run-tests.ps1"), encoding="utf-8") as f: script = f.read()
        masks = re.findall(r"\$t = \$t -replace '([^']*)', '([^']*)'", script)
        self.assertEqual(len(masks), len(report._MASKS))
        for (pattern, replacement), (compiled, python_replacement) in zip(masks, report._MASKS):
            self.assertEqual(pattern, compiled.pattern)
            self.assertEqual(replacement.replace("$1", r"\1"), python_replacement)
        self.assertIn("'" + report.ALLOWED.pattern + "'", script)
        self.assertEqual(script.count(report.REFUSED_WORDS), 2)        # the line filter and the final file check


class KitFiles(unittest.TestCase):
    kit = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts", "remoteplay", "testkit")

    def test_arm_table_is_well_formed(self):
        table = report.read_arm_table(os.path.join(self.kit, "arms.csv"))
        self.assertTrue({"original", "mod-legacy", "mod-all", "mod-input", "mod-ownerwait", "mod-enginewake"} <= set(table))
        for name, row in table.items():
            self.assertRegex(name, r"^[A-Za-z0-9-]+$")
            self.assertEqual(set(row), {"name", "exe", "set", "compare", "switches", "note"}, name)       # a comma in a note would add a column
            self.assertIn(row["exe"], ("original", "modified")); self.assertIn(row["set"], ("quick", "standard", "experimental"))
            self.assertIn(row["compare"], table); self.assertTrue(row["note"])
            for pair in filter(None, row["switches"].split(";")): self.assertRegex(pair, r"^VEYRA_(EXP|TEST)_RP_[A-Z0-9_]+=[^=;]+$")
        self.assertEqual([n for n, r in table.items() if r["set"] == "quick"], ["original", "mod-legacy", "mod-all"])
        self.assertEqual(table["original"]["switches"], ""); self.assertEqual(table["mod-all"]["switches"], "")
        self.assertTrue(all(r["set"] == "experimental" for n, r in table.items() if "VEYRA_EXP_" in r["switches"]))   # experiments only when asked

    def test_camera_template_has_no_rows_and_passes_the_privacy_check(self):
        with tempfile.TemporaryDirectory() as folder:
            with open(os.path.join(self.kit, "camera-template.csv"), encoding="utf-8") as f: text = f.read()
            with open(os.path.join(folder, "camera.csv"), "w", encoding="utf-8") as f: f.write(text)
            camera = report.read_camera(os.path.join(folder, "camera.csv"))
            self.assertEqual(camera["rows"], {}); self.assertEqual(camera["problems"], [])
            self.assertEqual(report.leak_findings(folder), [])


class Latency(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name
    def tearDown(self): self.tmp.cleanup()

    def test_a_known_saving_is_found_and_no_difference_is_not_invented(self):
        rng = random.Random(1)
        evidence(self.folder, {"mod-legacy": [presses(rng, 100.0, 40) for _ in range(6)], "mod-all": [presses(rng, 94.0, 40) for _ in range(6)],
                               "mod-input": [presses(rng, 100.0, 40) for _ in range(6)]})
        md, _html, _notes, leaks = make(self.folder)
        rows = table(md, "Software latency (built-in test)")
        self.assertEqual(rows["mod-legacy"]["Mean difference"], "baseline")
        self.assertEqual(rows["mod-all"]["Presses"], "240")
        self.assertEqual(rows["mod-all"]["Verdict"], "lower than the baseline")
        difference = float(rows["mod-all"]["Mean difference"].split(" ms")[0])
        self.assertTrue(-9.0 < difference < -3.0, difference)
        low, high = [float(x) for x in re.match(r"\[([-+\d.]+), ([-+\d.]+)\]", rows["mod-all"]["95 % interval"]).groups()]
        self.assertTrue(low < difference < high < 0)
        self.assertEqual(rows["mod-input"]["Verdict"], "not resolved: the interval includes zero")
        self.assertEqual(leaks, [])
        for key in ("Median ms", "P95 ms", "P99 ms", "Mean ms"): self.assertRegex(rows["mod-all"][key], r"^\d+\.\d$")
        self.assertEqual(rows["mod-all"]["PushFPS own ms"], "6.0")

    def test_fewer_than_120_presses_never_gets_a_verdict(self):
        rng = random.Random(2)
        evidence(self.folder, {"mod-legacy": [presses(rng, 100.0, 20) for _ in range(6)], "mod-all": [presses(rng, 60.0, 20) for _ in range(5)]})   # 40 ms apart, but 100 presses
        rows = table(make(self.folder)[0], "Software latency (built-in test)")
        self.assertEqual(rows["mod-all"]["Presses"], "100")
        self.assertEqual(rows["mod-all"]["Verdict"], "no verdict: fewer than 120 presses (100 and 120)")
        self.assertNotIn("lower", rows["mod-all"]["Verdict"])

    def test_compare_rule_directly(self):
        a = {"presses": [100.0] * 60 + [116.7] * 60, "runs": [], "count": 120}
        b = {"presses": [116.7] * 120, "runs": [], "count": 120}
        result = report.compare(b, a)
        self.assertTrue(result["real"]); self.assertEqual(result["verdict"], "higher than the baseline")
        self.assertFalse(report.compare(a, a)["real"])
        short = dict(a, presses=a["presses"][:119], count=119)
        self.assertFalse(report.compare(short, b)["real"]); self.assertIn("fewer than 120", report.compare(short, b)["verdict"])
        self.assertIn("no presses", report.compare({"presses": [], "runs": [], "count": 0}, a)["verdict"])

    def test_original_build_with_run_summaries_only(self):
        rng = random.Random(3)
        evidence(self.folder, {"original": [presses(rng, 100.0, 40) for _ in range(3)], "mod-legacy": [presses(rng, 100.0, 40) for _ in range(3)]})
        rows = table(make(self.folder)[0], "Software latency (built-in test)")
        self.assertEqual(rows["original"]["Presses"], "120 (run summaries)")
        self.assertEqual(rows["original"]["P95 ms"], "n/a"); self.assertEqual(rows["original"]["P99 ms"], "n/a")
        self.assertIn("by run", rows["original"]["95 % interval"])
        self.assertIn(rows["original"]["Verdict"], ("not resolved: the interval includes zero", "lower than the baseline", "higher than the baseline"))
        streams = table(make(self.folder)[0], "Stream health during the runs")
        self.assertEqual(streams["mod-legacy"]["Timing in use"], "ownerWait=event input=immediate engineWake=event")

    def test_baseline_override_and_missing_baseline(self):
        rng = random.Random(4)
        evidence(self.folder, {"exp-poll2": [presses(rng, 100.0, 40) for _ in range(3)], "mod-all": [presses(rng, 100.0, 40) for _ in range(3)]})
        with open(os.path.join(self.folder, "arms.csv"), "w") as f: f.write("# c\nname,exe,set,compare,switches,note\nexp-poll2,modified,experimental,mod-all,X=1,n\nmod-all,modified,quick,mod-legacy,,n\n")
        rows = table(make(self.folder)[0], "Software latency (built-in test)")
        self.assertIn("vs mod-all", rows["exp-poll2"]["Mean difference"])
        self.assertEqual(rows["mod-all"]["Mean difference"], "baseline 'mod-legacy' was not run")
        rows = table(make(self.folder, baseline_override="exp-poll2")[0], "Software latency (built-in test)")
        self.assertEqual(rows["exp-poll2"]["Mean difference"], "baseline"); self.assertIn("vs exp-poll2", rows["mod-all"]["Mean difference"])


class Frames(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name
    def tearDown(self): self.tmp.cleanup()

    def test_presentmon_columns_dropped_frames_and_the_test_window(self):
        path = os.path.join(self.folder, "p.csv")
        presentmon_v2(path, seconds=100, fps=60.0, drop_every=100, long_at=5.0)
        whole = report.presentmon_stats(path)
        self.assertEqual(whole["swap_chains"], 2); self.assertEqual(whole["frames"], 6000); self.assertEqual(whole["dropped"], 60)
        self.assertAlmostEqual(whole["dropped_percent"], 1.0, places=6)
        self.assertAlmostEqual(whole["shown_median"], 16.667, places=2); self.assertGreater(whole["shown_max"], 400)
        self.assertEqual(whole["mode"], "Hardware: Independent Flip"); self.assertFalse(whole["composed"]); self.assertAlmostEqual(whole["tearing_percent"], 100.0)
        self.assertAlmostEqual(whole["until_displayed_median"], 4.2); self.assertAlmostEqual(whole["video_busy_percent"], 9.0, places=1)
        windowed = report.presentmon_stats(path, [(10.0, 60.0)])
        self.assertEqual(windowed["window"], "while the latency test ran")
        self.assertTrue(2990 <= windowed["frames"] <= 3002); self.assertLess(windowed["shown_max"], 20)          # the long frame at 5 s is outside
        self.assertIn("not found", report.presentmon_stats(path, [(5000.0, 5001.0)])["window"])
        self.assertIsNone(report.presentmon_stats(os.path.join(self.folder, "missing.csv")))

    def test_presentmon_one_x_columns(self):
        path = os.path.join(self.folder, "v1.csv")
        with open(path, "w") as f:
            f.write("Application,ProcessID,SwapChainAddress,Runtime,SyncInterval,PresentFlags,Dropped,TimeInSeconds,msInPresentAPI,msBetweenPresents,AllowsTearing,PresentMode,msUntilRenderComplete,msUntilDisplayed,msBetweenDisplayChange\n")
            for i in range(600):
                dropped = i % 50 == 49
                f.write(f"PushFPS.exe,1,0x3CC,DXGI,0,0,{int(dropped)},{i / 120:.4f},0.1,8.333,0,Hardware Composed: Independent Flip,1.0,{0 if dropped else 5.0},{0 if dropped else 8.333}\n")
        stats = report.presentmon_stats(path)
        self.assertEqual(stats["frames"], 600); self.assertEqual(stats["dropped"], 12)
        self.assertEqual(stats["mode"], "Hardware Composed: Independent Flip")
        self.assertAlmostEqual(stats["shown_median"], 8.333); self.assertAlmostEqual(stats["until_displayed_median"], 5.0)   # 1.x writes msBetween...: names are matched without case
        self.assertAlmostEqual(stats["shown_fps"], 120.0, delta=0.1); self.assertAlmostEqual(stats["tearing_percent"], 0.0)

    def test_generated_frames_are_kept_apart_from_real_pictures(self):
        rng = random.Random(5)
        evidence(self.folder, {"mod-legacy": [presses(rng, 100.0, 40) for _ in range(3)]})
        add_run(self.folder, "exp-fg", 50, run_log(presses(rng, 120.0, 40)), fps=180.0, compare="exp-fg")
        md = make(self.folder)[0]
        rows = table(md, "Real pictures and displayed frames")
        self.assertEqual(rows["mod-legacy"]["Decoded per second (real)"], "59.9")
        self.assertAlmostEqual(float(rows["mod-legacy"]["Shown per second (all)"]), 60.0, delta=0.2)
        self.assertLess(float(rows["mod-legacy"]["Beyond the stream per second (generated or repeated)"]), 0.5)
        self.assertEqual(rows["exp-fg"]["Decoded per second (real)"], "59.9")
        self.assertAlmostEqual(float(rows["exp-fg"]["Beyond the stream per second (generated or repeated)"]), 120.1, delta=0.5)
        self.assertIn("not reported", rows["exp-fg"]["Frame types by PresentMon"])
        add_run(self.folder, "exp-typed", 51, run_log(presses(rng, 120.0, 40)), fps=120.0, compare="exp-typed", frame_type=["Application", "Generated"])
        typed = table(make(self.folder)[0], "Real pictures and displayed frames")["exp-typed"]["Frame types by PresentMon"]
        self.assertRegex(typed, r"^Application: \d+, Generated: \d+$")
        shown = table(md, "Displayed frames (PresentMon)")
        self.assertEqual(shown["mod-legacy"]["Part of the capture used"], "while the latency test ran")
        self.assertRegex(shown["mod-legacy"]["Present mode (share)"], r"^Hardware: Independent Flip \(100\.0 %\)$")
        resources = table(md, "GPU, CPU and memory")
        self.assertEqual(resources["mod-legacy"]["GPU % mean"], "35.0"); self.assertEqual(resources["mod-legacy"]["Decoder % mean (nvidia-smi)"], "11.0")
        self.assertEqual(resources["mod-legacy"]["Video memory MB max"], "2400.0"); self.assertEqual(resources["mod-legacy"]["CPU % mean"], "12.0")

    def test_different_present_modes_are_called_out(self):
        rng = random.Random(6)
        evidence(self.folder, {"mod-legacy": [presses(rng, 100.0, 40)]})
        add_run(self.folder, "mod-all", 60, run_log(presses(rng, 100.0, 40)), mode="Composed: Flip")
        md, _h, notes, _l = make(self.folder)
        self.assertTrue(any("NOT all run with the same present mode" in n for n in notes)); self.assertTrue(any("composed by the desktop compositor" in n for n in notes))
        self.assertIn("COMPOSED", table(md, "Displayed frames (PresentMon)")["mod-all"]["Present mode (share)"])

    def test_a_run_without_presentmon_or_gpu_samples(self):
        rng = random.Random(7)
        os.makedirs(os.path.join(self.folder, "bench"))
        run = add_run(self.folder, "mod-legacy", 1, run_log(presses(rng, 100.0, 40)), presentmon=False)
        samples(os.path.join(run, "samples.csv"), gpu=False)
        md = make(self.folder)[0]
        self.assertEqual(table(md, "Displayed frames (PresentMon)")["mod-legacy"]["Frames"], "no PresentMon capture")
        self.assertEqual(table(md, "GPU, CPU and memory")["mod-legacy"]["GPU % mean"], "n/a")


class CameraSoakImpairment(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name
        rng = random.Random(8)
        evidence(self.folder, {"mod-legacy": [presses(rng, 100.0, 40)], "mod-all": [presses(rng, 100.0, 40)]})
    def tearDown(self): self.tmp.cleanup()

    def camera(self, rows):
        with open(os.path.join(self.folder, "camera.csv"), "w") as f:
            f.write("# how to film ...\narm,press,frames,camera_fps,note\n" + "".join(rows))

    def test_only_the_camera_table_is_input_to_photon(self):
        self.camera([])                                                       # the template as copied: no rows
        md, page, notes, _l = make(self.folder)
        self.assertNotIn("input to photon", md.lower().replace("is input to photon", "")); self.assertTrue(any("No camera measurements" in n for n in notes))
        rng = random.Random(9)
        rows = [f"direct-ps5,{i},{rng.choice([19, 20, 21])},240,\n" for i in range(1, 41)] + [f"mod-all,{i},{rng.choice([27, 28, 29])},240,\n" for i in range(1, 41)] + \
               [f"official-app,{i},{rng.choice([30, 31])},240,\n" for i in range(1, 11)] + ["mod-all,99,,240,\n"]
        self.camera(rows)
        md, page, notes, _l = make(self.folder)
        headings = [l for l in md.splitlines() if l.startswith("## ")]
        self.assertEqual([h for h in headings if "photon" in h.lower()], ["## Input to photon (camera)"])
        self.assertEqual(page.lower().count("<h2>input to photon"), 1)
        rows = table(md, "Input to photon (camera)")
        self.assertEqual(rows["direct-ps5 (camera only)"]["Mean difference"], "reference")
        self.assertEqual(rows["mod-all"]["Presses"], "40"); self.assertEqual(rows["mod-all"]["One camera frame ms"], "4.2")
        self.assertAlmostEqual(float(rows["mod-all"]["Median ms"]), 28 / 240 * 1000, delta=4.2)
        self.assertEqual(rows["mod-all"]["Verdict"], "higher than the baseline")
        self.assertIn("fewer than 30", rows["official-app (camera only)"]["Verdict"])
        self.assertTrue(any("row 91 skipped" in n for n in notes))
        software = table(md, "Software latency (built-in test)")
        self.assertNotIn("direct-ps5", software)                               # camera-only arms never enter the software tables

    def test_soak_reports_memory_growth_and_reconnects(self):
        extra = [line(600, "remoteplay-recovery", "reconnected attempt=1 first decoded frame; reset video history and audio clock"),
                 line(500, "remoteplay-recovery", "request keyframe; received=1 decoded=1", "WARN "), line(700, "remoteplay", "avcodec_send_packet failed code=-5", "ERROR")]
        run = add_run(self.folder, "mod-all", 70, run_log([], extra=extra), purpose="soak")
        samples(os.path.join(run, "samples.csv"), seconds=3600, growth_mb_per_hour=120.0)
        rows = table(make(self.folder)[0], "Soak")
        self.assertAlmostEqual(float(rows["mod-all"]["Minutes"]), 60.0, delta=0.1)
        self.assertEqual(rows["mod-all"]["Reconnects"], "1"); self.assertEqual(rows["mod-all"]["Keyframe requests"], "1"); self.assertEqual(rows["mod-all"]["Error lines"], "1")
        self.assertAlmostEqual(float(rows["mod-all"]["Memory MB per hour"]), 120.0, delta=1.0)
        self.assertLess(float(rows["mod-all"]["Memory MB start"]), float(rows["mod-all"]["Memory MB end"]))
        software = table(make(self.folder)[0], "Software latency (built-in test)")
        self.assertEqual(software["mod-all"]["Presses"], "40")                 # the soak run adds no presses to the arm

    def test_impairment_markers(self):
        lines = [line(1, "remoteplay", "session started host=10.x.x.x profile=1920x1080@60 codec=hevc")]
        for second in range(2, 200):
            fps = 0.0 if 61 <= second < 66 else 59.9                          # five seconds without pictures after the first marker
            lines.append(line(second, "remoteplay-progress", f"state=3 received=1 decoded=1 inputFps={fps} decodeFps={fps} queue=0 idrRequests=0 errors=0"))
        lines += [line(66, "remoteplay-recovery", "request keyframe; received=1 decoded=1", "WARN "), line(140, "remoteplay-recovery", "reconnected attempt=1 first decoded frame")]
        run = add_run(self.folder, "mod-all", 80, lines, purpose="impairment", seconds=200, long_at=62.0)
        with open(os.path.join(run, "markers.csv"), "w") as f: f.write(f"utc,marker\n{stamp(60)},cable-out-1s\n{stamp(120)},cable-out-5s\n")
        md = make(self.folder)[0]
        part = md.split("## Impairment and recovery\n", 1)[1]
        first = next(l for l in part.splitlines() if "cable-out-1s" in l).split("|")
        second = next(l for l in part.splitlines() if "cable-out-5s" in l).split("|")
        self.assertEqual(first[3].strip(), "59.9"); self.assertEqual(first[4].strip(), "0.0"); self.assertEqual(first[5].strip(), "6.0")
        self.assertEqual(first[6].strip(), "500"); self.assertEqual(first[7].strip(), "1"); self.assertEqual(first[8].strip(), "0")
        self.assertEqual(second[4].strip(), "59.9"); self.assertEqual(second[5].strip(), "0.0"); self.assertEqual(second[8].strip(), "1")


class Output(unittest.TestCase):
    def test_html_escapes_everything_from_the_evidence(self):
        with tempfile.TemporaryDirectory() as folder:
            rng = random.Random(10)
            evidence(folder, {"mod-legacy": [presses(rng, 100.0, 40)]}, kit={"modified": {"file": "<script>alert(1)</script>", "productVersion": "1", "sha256First12": "abc"}})
            md, page, _n, _l = make(folder)
            self.assertNotIn("<script>", page); self.assertIn("&lt;script&gt;", page)
            self.assertTrue(page.startswith("<!DOCTYPE html>")); self.assertIn("<table>", page)

    def test_privacy_check_finds_what_the_kit_should_have_removed(self):
        with tempfile.TemporaryDirectory() as folder:
            rng = random.Random(11)
            evidence(folder, {"mod-legacy": [presses(rng, 100.0, 40)]})
            self.assertEqual(report.leak_findings(folder), [])
            with open(os.path.join(folder, "bench", "leak@20261118-130000.txt"), "w") as f:
                f.write(line(0, "remoteplay-control", "start host=192.168.1.77") + "\n" + line(1, "remoteplay", "account id AQAAAAAAAAA=") + "\n")
            found = report.leak_findings(folder)
            self.assertEqual([(f.replace("\\", "/"), n) for f, n, _w in found], [("bench/leak@20261118-130000.txt", 1), ("bench/leak@20261118-130000.txt", 2)])
            _md, _page, notes, leaks = make(folder)
            self.assertTrue(any(n.startswith("PRIVACY CHECK FAILED") for n in notes)); self.assertEqual(len(leaks), 2)

    def test_command_line_on_a_folder_and_on_a_zip(self):
        with tempfile.TemporaryDirectory() as base:
            folder = os.path.join(base, "evidence-1"); os.makedirs(folder)
            rng = random.Random(12)
            evidence(folder, {"mod-legacy": [presses(rng, 100.0, 40) for _ in range(3)], "mod-all": [presses(rng, 100.0, 40) for _ in range(3)]})
            with redirect_stdout(io.StringIO()): self.assertEqual(report.main([folder, "--strict"]), 0)
            self.assertTrue(os.path.getsize(os.path.join(folder, "report.md")) > 1000); self.assertTrue(os.path.getsize(os.path.join(folder, "report.html")) > 1000)
            with redirect_stdout(io.StringIO()): self.assertEqual(report.main([folder, "--strict"]), 0)      # its own reports are not evidence
            archive = os.path.join(base, "evidence-1.zip")
            with zipfile.ZipFile(archive, "w") as z:
                for root, _d, files in os.walk(folder):
                    for name in files:
                        if not name.startswith("report."): z.write(os.path.join(root, name), os.path.relpath(os.path.join(root, name), folder).replace(os.sep, "\\"))
                z.writestr("../outside.txt", "must not be written outside")
            out = os.path.join(base, "out")
            with redirect_stdout(io.StringIO()): self.assertEqual(report.main([archive, "--out", out]), 0)
            with open(os.path.join(out, "report.md"), encoding="utf-8") as f: self.assertIn("mod-all", f.read())
            self.assertFalse(os.path.exists(os.path.join(base, "outside.txt")))
            with open(os.path.join(folder, "bench", "x@20261118-140000.txt"), "w") as f: f.write(line(0, "remoteplay", "peer 203.0.113.9") + "\n")
            with redirect_stdout(io.StringIO()): self.assertEqual(report.main([folder, "--strict"]), 2)
            with redirect_stdout(io.StringIO()): self.assertEqual(report.main([os.path.join(base, "nothing-here")]), 1)

    def test_json_shapes_powershell_can_write(self):
        # One adapter or one warning can arrive as a single object or string instead of a list, a file can be a bare list.
        with tempfile.TemporaryDirectory() as folder:
            rng = random.Random(13)
            evidence(folder, {"mod-legacy": [presses(rng, 100.0, 40)]})
            with open(os.path.join(folder, "hardware.json"), "w", encoding="utf-8-sig") as f:
                json.dump({"cpu": "odd", "adapters": {"name": "Single Adapter", "driverVersion": "1.2", "width": 1, "height": 2, "refreshHz": 60}, "warnings": "one warning"}, f)
            with open(os.path.join(folder, "kit.json"), "w", encoding="utf-8-sig") as f: json.dump([1, 2, 3], f)
            run = next(os.path.join(folder, "runs", n) for n in os.listdir(os.path.join(folder, "runs")))
            with open(os.path.join(run, "run.json"), "w", encoding="utf-8-sig") as f: json.dump("text", f)
            md = make(folder)[0]
            self.assertIn("Single Adapter", md); self.assertIn("one warning", md); self.assertIn("mod-legacy", md)

    def test_an_empty_folder_gives_a_report_without_numbers(self):
        with tempfile.TemporaryDirectory() as folder:
            md, page, notes, leaks = make(folder)
            self.assertIn("No data.", md); self.assertEqual(leaks, [])


if __name__ == "__main__":
    unittest.main(verbosity=1)
