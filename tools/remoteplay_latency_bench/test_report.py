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
        # a four-part version after version= / originalVersion= is not an address (8A review): kept readable
        self.assertEqual(r("runtime=nr-original version=310.8.0.0 originalVersion=1.2.3.4 host=10.1.2.3"), "runtime=nr-original version=310.8.0.0 originalVersion=1.2.3.4 host=10.x.x.x")
        self.assertEqual(r("ProductVersion=192.168.0.1 sub=192.168.0.1"), "ProductVersion=192.168.0.1 sub=192.168.x.x")
        self.assertEqual(r("versions 8.8.8.8 aversion=8.8.8.8x"), "versions x.x.x.x aversion=8.8.8.8x")
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
        self.assertEqual(r("device name=PLANTED-PC,"), "device name=[name],")
        self.assertEqual(r("2026-11-18T12:34:56.789Z t=1234 [INFO ] [remoteplay] decodeFps=59.9 queue=0"), "2026-11-18T12:34:56.789Z t=1234 [INFO ] [remoteplay] decodeFps=59.9 queue=0")

    def test_identifiers_found_by_the_security_audit(self):
        r = report.redact_line
        self.assertEqual(r(r"profile=\\PLANTEDSRV\share\kbm.txt end"), "profile=[path] end")                  # UNC path
        self.assertEqual(r(r'"p": "\\\\PLANTEDSRV\\share\\x"'), '"p": "[path]"')                             # as JSON writes it
        self.assertEqual(r("file C:/Users/Planted User/AppData/x.ini y"), "file [path]/AppData/x.ini y")            # as Qt prints a path
        self.assertEqual(r("dump D:/Games/PlantedUser/x.log"), "dump [path]")
        self.assertEqual(r("see https://github.com/x/y"), "see https://github.com/x/y")                           # a web address is not a drive path
        self.assertEqual(r("mail planted.user@example.com end"), "mail [email] end")
        self.assertEqual(r("profile=1920x1080@60 codec=hevc"), "profile=1920x1080@60 codec=hevc")
        self.assertEqual(r("mac 001A2B3C4D5E dotted 001a.2b3c.4d5e"), "mac [mac] dotted [mac]")
        self.assertEqual(r("guid 0F8FAD5B-D9CB-469F-A165-70867728950E end"), "guid [guid] end")
        self.assertEqual(r("id 1A2B3C4D5E6F7A8 and 1a2b3c4d5e6f7"), "id [hex] and [hex]")
        self.assertEqual(r("received=123456789012 removedReason=0x887A0006"), "received=123456789012 removedReason=0x887A0006")   # counters stay
        self.assertEqual(r("server=PlantedServer computer=PLANTED-PC device=PlantedDevice network=PlantedNet networkKnown=true"),
                         "server=[name] computer=[name] device=[name] network=[name] networkKnown=true")
        self.assertEqual(r("peer fe80::1ff:fe23:4567:890b%12 end"), "peer [ipv6] end")
        self.assertTrue(report.PRIVATE_WORD.search("regist key=1")); self.assertTrue(report.PRIVATE_WORD.search("rp-key 1"))
        self.assertEqual(r("sha256 A1B2C3D4E5"), "sha256 A1B2C3D4E5")                                             # the kit's 10-digit build hash
        for text in ["profile=\\\\S\\x", "C:/Users/A B/c", "a@b.cd", "001A2B3C4D5E", "fe80::1%12", "device=x"]:
            once = r(text); self.assertEqual(r(once), once, text)

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
        self.assertEqual(script.count(report.REFUSED_WORDS), 3)        # the line filter, the final file check and the table check


class KitFiles(unittest.TestCase):
    def test_the_network_line_is_copied_by_the_kit(self):
        # Stream network: [slipstream-net] must pass the kit's allowlist unchanged (the dry run's made-up line).
        line = ("2026-11-21T08:00:00.000Z t=1 [INFO ] [slipstream-net] window=5.0 codec=hevc resolution=1920x1080 missing=0 late=0 "
                "udpInErrors=0 nicDiscards=0 route=on-link routeClass=wired family=ipv4 bottleneck=none signs=none "
                "(software counters; udp*: whole PC; nic*: the console's adapter; -1 = not available)")
        self.assertEqual(report.safe_lines([line]), [line])
    kit = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts", "remoteplay", "testkit")
    def test_the_smoothness_line_is_copied_by_the_kit(self):
        # Stream smooth: [slipstream-smooth] must pass the kit's allowlist too (it was missing when the streams merged).
        line = "2026-11-21T08:00:00.000Z t=1 [INFO ] [slipstream-smooth] window=5.0 backend=vfg ceiling=3X chosen=3X"
        self.assertEqual(report.safe_lines([line]), [line])
        for name in ("run-tests.ps1", "collect-diagnostics.ps1"):
            with open(os.path.join(self.kit, name), encoding="ascii") as f: self.assertIn("|cadence|net|smooth|capture)", f.read())

    def test_the_ull_lines_are_copied_by_the_kit(self):
        # Stream fglat: the [fg-ull] lines FG_LATENCY.md tells the reader to look for must pass the kit's allowlist
        # (it was missing when Package 12 was merged). Both formats as EngineController.cpp writes them.
        lines = ["2026-11-22T08:00:00.000Z t=1 [INFO ] [fg-ull] experiment: Ultra Low Latency Frame Generation "
                 "parts=hold,phase understood=true (VEYRA_EXP_RP_FG_ULL)",
                 "2026-11-22T08:00:01.000Z t=2 [INFO ] [fg-ull] revision=3 held=true why=settling source=120 heldPairs=4 "
                 "heldProbes=1 entries=1 (real frames shown when ready instead of on the FG phase while no generated frame "
                 "of their pair is shown; 3 admitted pairs in a row resume)"]
        self.assertEqual(report.safe_lines(lines), lines)
        for name in ("run-tests.ps1", "collect-diagnostics.ps1"):
            with open(os.path.join(self.kit, name), encoding="ascii") as f: self.assertIn("fg-(validity|drops|ull)", f.read())

    def test_arm_table_is_well_formed(self):
        with open(os.path.join(self.kit, "run-tests.ps1"), encoding="ascii") as f: kit_text = f.read()
        # the kit's own switch rule (Get-ArmSwitches, also run for every arm in Read-ArmTable at start-up) is the one checked below
        self.assertIn("if ($name -notmatch '^VEYRA_(EXP|TEST)_(RP|PRESENT|SR|PIPE)_[A-Z0-9_]+$')", kit_text)
        self.assertIn("[void](Get-ArmSwitches $row)", kit_text); self.assertIn("[void](Get-ArmPrefs $row)", kit_text)
        table = report.read_arm_table(os.path.join(self.kit, "arms.csv"))
        self.assertTrue({"original", "mod-legacy", "mod-all", "mod-input", "mod-ownerwait", "mod-enginewake"} <= set(table))
        for name, row in table.items():
            self.assertRegex(name, r"^[A-Za-z0-9-]+$")
            self.assertEqual(set(row), {"name", "exe", "set", "compare", "switches", "note", "fg", "differs"}, name)       # a comma in a note would add a column
            self.assertIn(row["exe"], ("original", "modified", "experimental", "previous")); self.assertIn(row["set"], ("quick", "standard", "experimental", "versus", "switches", "quality", "probe", "network", "network-builds", "network-wifi-mode", "clarity", "netdiag", "nrperf", "nrperf-capture") + report.MEASURE_SESSIONS)
            if row["exe"] == "previous": self.assertEqual(row["set"], "builds", name)                    # Package 7's build only in the regression session
            if row["set"] in ("switches", "quality", "probe", "network", "network-wifi-mode", "netdiag"): self.assertEqual(row["exe"], "experimental", name)
            self.assertIn(row["fg"], ("off", "on"), name); self.assertEqual(row["fg"] == "on", "frame generation on" in row["note"].lower(), name)
            if row["set"] != "probe": self.assertNotIn("STREAM_DEDUP", row["switches"], name)          # duplicate detection only in the optional diagnostic
            self.assertIn(row["compare"], table); self.assertTrue(row["note"])
            for pair in filter(None, row["switches"].split(";")): self.assertRegex(pair, r"^(VEYRA_(EXP|TEST)_(RP|PRESENT|SR|PIPE)_[A-Z0-9_]+=[^=;]+|pref:(pushAutoFullscreen|vrr|fgAdaptive|pacingRepair)=(true|false)|pref:captureBufferMode=[012]|pipeline:[A-Za-z0-9]+=[A-Za-z0-9.-]+(\|[A-Za-z0-9]+=[A-Za-z0-9.-]+)*)$")
        self.assertEqual([n for n, r in table.items() if r["set"] == "quick"], ["original", "mod-legacy", "mod-all"])
        self.assertEqual(table["original"]["switches"], ""); self.assertEqual(table["mod-all"]["switches"], "")
        self.assertTrue(all(r["set"] in ("experimental", "versus", "switches", "probe", "network", "network-wifi-mode", "input", "audiofix", "pacing", "smoothness", "neural", "clarity", "netdiag", "retest", "capture", "nrperf", "nrperf-capture") for n, r in table.items() if "VEYRA_EXP_" in r["switches"]))   # experiments only when asked
        self.assertEqual([n for n, r in table.items() if "STREAM_DEDUP" in r["switches"]], ["p-dedup"])
        versus = {n: r for n, r in table.items() if r["set"] == "versus"}
        self.assertEqual(versus["stable"]["exe"], "modified"); self.assertEqual(versus["x-base"]["compare"], "stable")
        named = {r["switches"].split("=")[0] for n, r in versus.items() if r["switches"]}
        self.assertEqual(named, {"VEYRA_EXP_RP_LAN_PROFILE", "VEYRA_EXP_RP_REORDER_ADAPTIVE", "VEYRA_EXP_RP_HEALTH", "VEYRA_EXP_RP_BACKLOG", "VEYRA_EXP_RP_KBM",
                                 "VEYRA_EXP_RP_AUDIO_FAST_RECOVER", "VEYRA_EXP_RP_GOVERNOR", "VEYRA_EXP_RP_GPU_BUDGET"})
        self.assertTrue(all(r["exe"] == "experimental" and r["compare"] == "x-base" for r in versus.values() if r["switches"]))

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
        rows = table(md, "Software latency (built-in test; software, not input-to-photon)")
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
        rows = table(make(self.folder)[0], "Software latency (built-in test; software, not input-to-photon)")
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
        rows = table(make(self.folder)[0], "Software latency (built-in test; software, not input-to-photon)")
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
        rows = table(make(self.folder)[0], "Software latency (built-in test; software, not input-to-photon)")
        self.assertIn("vs mod-all", rows["exp-poll2"]["Mean difference"])
        self.assertEqual(rows["mod-all"]["Mean difference"], "baseline 'mod-legacy' was not run")
        rows = table(make(self.folder, baseline_override="exp-poll2")[0], "Software latency (built-in test; software, not input-to-photon)")
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
        self.assertEqual([h for h in headings if h.lower().startswith("## input to photon")], ["## Input to photon (camera)"])
        self.assertTrue(all("not input-to-photon" in h for h in headings if "photon" in h.lower() and not h.lower().startswith("## input to photon")))
        self.assertEqual(page.lower().count("<h2>input to photon"), 1)
        rows = table(md, "Input to photon (camera)")
        self.assertEqual(rows["direct-ps5 (camera only)"]["Mean difference"], "reference")
        self.assertEqual(rows["mod-all"]["Presses"], "40"); self.assertEqual(rows["mod-all"]["One camera frame ms"], "4.2")
        self.assertAlmostEqual(float(rows["mod-all"]["Median ms"]), 28 / 240 * 1000, delta=4.2)
        self.assertEqual(rows["mod-all"]["Verdict"], "higher than the baseline")
        self.assertIn("fewer than 30", rows["official-app (camera only)"]["Verdict"])
        self.assertTrue(any("row 91 skipped" in n for n in notes))
        software = table(md, "Software latency (built-in test; software, not input-to-photon)")
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
        software = table(make(self.folder)[0], "Software latency (built-in test; software, not input-to-photon)")
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


def health_line(seconds, loss=0.0, jitter=0.4, late=0, grade="stable", mbps=28.1, lost=0, keyframes=0, resets=0, drops=0):
    return line(seconds, "remoteplay-health", f"grade={grade} interval={grade} seconds=5.0 lossPercent={loss:.3f} lost={lost} received=9000 frameJitterMs={jitter:.2f} frameJitterPeakMs={jitter * 3:.2f} "
                f"reordered=0 late={late} reorderTimeouts=0 skipped=0 fecRecovered=2 fecFailed=0 keyframeRequests={keyframes} decoderResets={resets} frameDrops={drops} ingressDepth=0 ingressPeak=1 mailboxOverwrites=0 "
                f"mbps={mbps:.2f} fps=59.9 rtt=0.45ms(at-connect) route=direct-lan link=ethernet linkMbps=1000")

def flow_line(seconds, real, generated, session=1, state="active", expired=0):
    return line(seconds, "frame-flow", f"state={state} session={session} revision=1 epoch=1 source={real} batch={real} captureReceived={real} mailboxOverwritten=0 sourceAccepted={real} realSubmitted={real} "
                f"fgEvaluated={generated} fgInvalid=0 realPresented={real} generatedPresented={generated} generatedExpiredAfterEval={expired} cancelledBeforePresent=0 presentSubmitFps=59.90")

def vrr_line(seconds, active="true", level=2):
    return line(seconds, "display-vrr", f"level={level} known=true monitorSupportsVrr=true currentlyCapable=true trueGsync=false capsKnown=true possible=true requested={active} active={active} displayInVrrMode={active} "
                "indicator=false simulated=false status=0 refreshHz=180.000 (NVIDIA G-SYNC / G-SYNC Compatible state of the window's monitor; read-only monitor query)")

def full_extra(at=0.0, vrr="true", generated=0):
    """The lines a run of kit 2 adds to what run_log() makes: every value 'What this evidence holds' asks for."""
    return [line(at + 1, "remoteplay-decode", "actual=D3D12VA pixelFormat=172 1920x1080 fallback=false"), line(at + 1, "qml", "[update-check] current installed=3.2.1 build=3.2.1 latest=3.2.1"),
            line(at + 1, "remoteplay-health", "route=direct-lan link=ethernet linkMbps=1000 (this PC's side only)"), vrr_line(at + 2, vrr),
            line(at + 25, "display-latency", "presentToScanoutP50Ms=2.100 presentToScanoutP95Ms=4.800 presentToScanoutMaxMs=6.000 samples=180 replacedAtLeast=0 compositionMode=2 refreshHz=180.000 (DXGI)"),
            line(at + 20, "frame-rate", "revision=1 gpuCompletedFps=59.90 completedReal=60 capture60To30=false rateSkipped=0 presentSubmitFps=59.90 windowMs=1000 vramMiB=1200 budgetMiB=11000 (not scanout FPS)"),
            line(at + 20, "remoteplay-cadence", "state=doubled windowS=10.0 streamFps=59.94 sourceFps=29.97 sourceFpsNow=29.97 realPresentedFps=29.97 generatedPresentedFps=59.94 presentedFps=89.91 repeatsFound=300 repeatsRemoved=299 "
                 "keptUnsure=1 unmeasured=0 lateUsed=0 restarts=0 cuts=0 locks=1 unlocks=0 unreadOverwritten=0 generatedExpired=0 cancelledBeforePresent=0 fgGpuBatchP95Ms=2.100 arrivalToPresentMs=9.500 (frames counted where they are presented, generated and real apart; duplicate removal on)"),
            line(at + 21, "remoteplay-progress", "state=3 received=600 decoded=600 inputFps=59.9 decodeFps=59.9 videoMbps=28.100 receiveAgeMs=7.0 decodeAgeMs=5.0 decodeBusyMs=1.2 queue=0 waitingIdr=false idrRequests=0 errors=0 (x)"),
            flow_line(at + 30, 1800, generated), flow_line(at + 100, 6000, generated * 3)] + [health_line(at + 5 * k, loss=0.01 * (k % 3), lost=k % 3) for k in range(1, 20)]


class Redaction2(unittest.TestCase):
    def test_paths_are_masked_whole(self):
        r = report.redact_line
        self.assertEqual(r(r"module=C:\Users\John Smith\AppData\Local\Programs\PushFPS\PushFPS.exe offset=0x12"), r"module=[path]\AppData\Local\Programs\PushFPS\PushFPS.exe offset=0x12")
        self.assertEqual(r(r"data folder C:\Users\John Smith"), "data folder [path]")
        self.assertEqual(r(r"path=C:\Program Files (x86)\RivaTuner Statistics Server\RTSSHooks64.dll, next=1"), "path=[path], next=1")
        self.assertEqual(r(r"file=D:\My Games PlantedUser\dumps\veyra-crash.dmp bytes=5"), "file=[path] bytes=5")
        self.assertEqual(r(r'"log": "C:\\Users\\Planted User\\x.log",'), r'"log": "[path]\\x.log",')          # as JSON writes it
        self.assertEqual(r(r'"log": "D:\\Games\\PlantedUser\\x.log",'), '"log": "[path]",')
        self.assertEqual(r("ratio 16:9 at 12:30:00 t=1 codec=h265"), "ratio 16:9 at 12:30:00 t=1 codec=h265")
        for text in [r"C:\Users\John Smith\a b\c.dll x", r"e:\x", r"module=C:\Users\A\b.exe"]:
            once = r(text); self.assertEqual(r(once), once); self.assertNotIn("Users", once)

    def test_the_family_must_be_the_component_of_the_line(self):
        kept = report.safe_lines([line(0, "qml", "[update-check] current installed=3.2.1 build=3.2.1 latest=3.2.1"), line(0, "qml", "some text of the interface [crash] [remoteplay] x=1"),
                                  line(0, "app", "data folder [frame-flow] here"), line(0, "frame-flow", "state=active session=1 realPresented=5"), line(0, "display-vrr", "level=2 active=true"),
                                  line(0, "display-color", "hdrInput=false"), line(0, "crash", r"unhandled exception code=0xC0000005 module=C:\Users\Planted Name\PushFPS.exe offset=0x1", "ERROR"),
                                  line(0, "ui-live-failed", "remote=true status=x"), line(0, "push-quality", "done samples=60 psnr=31.20 score=78.5"), "[frame-flow] a line without time and level"])
        text = "\n".join(kept)
        self.assertEqual(len(kept), 5, text)
        for wanted in ("[update-check]", "[frame-flow] state=active", "[display-vrr]", "module=[path]", "[push-quality]"): self.assertIn(wanted, text)
        self.assertNotIn("Planted", text); self.assertNotIn("interface", text); self.assertNotIn("display-color", text)


class ResultPage(unittest.TestCase):
    """The one-page result: the same rule as RESULT.txt of the kit (run-tests.ps1 Write-ResultFiles)."""
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name; self.index = 0
    def tearDown(self): self.tmp.cleanup()

    def arm(self, name, mean, stream="profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000", runs=3, extra=()):
        rng = random.Random(len(name) * 11 + int(mean))
        for _ in range(runs):
            lines = [l.replace("profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000", stream) for l in run_log(presses(rng, mean, 40))] + list(extra)
            add_run(self.folder, name, self.index, lines, presentmon=False); self.index += 1

    def summary(self): return {r["name"]: r for r in report.result_summary(report.load(self.folder))["arms"]}

    def test_quantile_intervals_from_order_statistics(self):
        values = [float(i) for i in range(1, 121)]
        self.assertEqual(report.quantile_interval(values, .5), (49.0, 72.0))
        low, high = report.quantile_interval(values, .99)
        self.assertEqual(low, 116.0); self.assertTrue(high != high)                         # too few presses for its upper side
        low, high = report.quantile_interval([1.0, 2.0, 3.0], .5); self.assertTrue(low != low and high != high)
        self.assertTrue(all(x != x for x in report.quantile_interval([], .5)))

    def test_a_clear_saving_is_called_and_an_overlap_is_not(self):
        self.arm("mod-legacy", 100.0); self.arm("mod-all", 80.0); self.arm("mod-input", 99.0)
        rows = self.summary()
        self.assertEqual(rows["mod-legacy"]["verdict"], "baseline")
        self.assertEqual(rows["mod-all"]["presses"], 120)
        self.assertTrue(rows["mod-all"]["verdict"].startswith("LOWER than mod-legacy"), rows["mod-all"]["verdict"])
        self.assertTrue(rows["mod-all"]["medianHigh"] < rows["mod-legacy"]["medianLow"])
        self.assertTrue(rows["mod-input"]["verdict"].startswith("no difference shown"), rows["mod-input"]["verdict"])

    def test_too_few_presses_give_no_verdict(self):
        self.arm("mod-legacy", 100.0, runs=2); self.arm("mod-all", 80.0, runs=2)
        self.assertTrue(self.summary()["mod-all"]["verdict"].startswith("no verdict: fewer than 120 presses (80 and 80)"))

    def test_settings_that_differ_refuse_the_arm_and_name_the_difference(self):
        self.arm("mod-legacy", 100.0); self.arm("mod-all", 80.0, stream="profile=1920x1080@60 codec=hevc requestedBitrateKbps=15000")
        row = self.summary()["mod-all"]
        self.assertTrue(row["refused"]); self.assertTrue(row["verdict"].startswith("REFUSED")); self.assertIn("15000", row["verdict"])

    def test_frame_generation_on_in_an_arm_that_needs_it_off_is_refused(self):
        flow = [line(120, "frame-flow", "state=active session=1 revision=1 sourceAccepted=600 realPresented=600 generatedPresented=300 generatedExpiredAfterEval=0 cancelledBeforePresent=0 mailboxOverwritten=0 fgEvaluated=300 fgInvalid=0")]
        off = [line(120, "frame-flow", "state=active session=1 revision=1 sourceAccepted=600 realPresented=600 generatedPresented=0 generatedExpiredAfterEval=0 cancelledBeforePresent=0 mailboxOverwritten=0 fgEvaluated=0 fgInvalid=0")]
        self.arm("mod-legacy", 100.0, extra=off); self.arm("mod-all", 80.0, extra=flow)
        row = self.summary()["mod-all"]
        self.assertTrue(row["refused"]); self.assertIn("frame generation was on; this arm needs it off", row["verdict"])
        md = make(self.folder)[0]
        self.assertIn("frame generation differs", table(md, "Software latency (built-in test; software, not input-to-photon)")["mod-all"]["Verdict"])

    def test_refresh_rates_that_differ_are_refused(self):
        def vrr(hz): return [line(3, "display-vrr", f"level=2 known=true active=true displayInVrrMode=true requested=true refreshHz={hz:.3f} (x)")]
        self.arm("mod-legacy", 100.0, extra=vrr(180)); self.arm("mod-all", 80.0, extra=vrr(120))
        row = self.summary()["mod-all"]
        self.assertTrue(row["refused"]); self.assertIn("refresh rate differs", row["verdict"])

    def test_the_kits_result_is_compared_and_a_difference_is_named(self):
        self.arm("mod-legacy", 100.0); self.arm("mod-all", 80.0)
        rows = self.summary()
        mine = [{"name": n, "presses": r["presses"], "median": round(r["median"], 2)} for n, r in rows.items()]
        with open(os.path.join(self.folder, "result.json"), "w", encoding="utf-8-sig") as f: json.dump({"arms": mine}, f)
        notes = make(self.folder)[2]
        self.assertTrue(any("agree on every arm" in n for n in notes), notes)
        mine[1]["presses"] += 1
        with open(os.path.join(self.folder, "result.json"), "w", encoding="utf-8-sig") as f: json.dump({"arms": mine}, f)
        notes = make(self.folder)[2]
        self.assertTrue(any("DISAGREE" in n for n in notes), notes)
        # the same counts but another verdict word (REVIEW_kit2 should-fix 5)
        mine[1]["presses"] -= 1
        for m in mine: m["verdict"] = rows[m["name"]]["verdict"]; m["refused"] = rows[m["name"]]["refused"]
        with open(os.path.join(self.folder, "result.json"), "w", encoding="utf-8-sig") as f: json.dump({"arms": mine}, f)
        self.assertTrue(any("agree on every arm" in n for n in make(self.folder)[2]))
        mine[1]["verdict"] = "Wi-Fi: diagnostic only, not a wired-LAN latency conclusion; no difference shown"
        with open(os.path.join(self.folder, "result.json"), "w", encoding="utf-8-sig") as f: json.dump({"arms": mine}, f)
        notes = make(self.folder)[2]
        self.assertTrue(any("DISAGREE" in n and "verdict" in n for n in notes), notes)

    def test_light_sensor_rows_have_their_own_table_and_never_enter_the_result(self):
        self.arm("mod-legacy", 100.0); self.arm("mod-all", 80.0)
        with open(os.path.join(self.folder, "sensor.csv"), "w", encoding="utf-8") as f:
            f.write("# c\narm,press,ms,device,note\n" + "".join(f"mod-all,{i},{40 + i % 3},photodiode tester,\n" for i in range(1, 31)) + "mod-all,99,,x,\n")
        md, _p, notes, _l = make(self.folder)
        rows = table(md, "Input to photon (light sensor)")
        self.assertEqual(rows["mod-all"]["Presses"], "30"); self.assertEqual(rows["mod-all"]["Device"], "photodiode tester")
        page = table(md, "One-page result (software, not input-to-photon)")
        self.assertEqual(set(page), {"mod-legacy", "mod-all"}); self.assertEqual(page["mod-all"]["Presses"], "120")
        self.assertTrue(any("sensor.csv: row 31 skipped" in n for n in notes))

    def test_the_kit_uses_the_same_rule_and_lists_the_same_missing_fields(self):
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "scripts", "remoteplay", "testkit", "run-tests.ps1"), encoding="ascii") as f: script = f.read()
        self.assertIn("$resultMinimum = " + str(report.RESULT_MINIMUM), script)
        block = script[script.index("$missingAppFields = @("):script.index("# Sorted values: linear interpolation")]
        self.assertEqual(tuple(re.findall(r"'([^']+)'", block)), report.MISSING_APP_FIELDS)
        self.assertIn("1.96 * [math]::Sqrt($n * $P * (1 - $P))", script)
        self.assertIn("[int][math]::Ceiling($n * $P + $spread) + 1", script); self.assertIn("[int][math]::Floor($n * $P - $spread)", script)
        for text in report.MISSING_APP_FIELDS + (report.RESULT_RULE,):
            self.assertIsNone(report.PRIVATE_WORD.search(text), text); self.assertEqual(report.redact_line(text), text, text)


class Conditions(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name; self.index = 0
    def tearDown(self): self.tmp.cleanup()

    def arm(self, name, mean, stream="profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000", runs=3, switches=(), extra=None, **pm):
        rng = random.Random(len(name) * 7 + int(mean))
        for _ in range(runs):
            lines = [l.replace("profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000", stream) for l in run_log(presses(rng, mean, 40))] + list(extra if extra is not None else [])
            run = add_run(self.folder, name, self.index, lines, **pm); self.index += 1
            with open(os.path.join(run, "run.json"), "w") as f: json.dump({"arm": name, "exe": "modified", "purpose": "latency", "compare": "mod-legacy", "presentMonStartUtc": stamp(0), "switches": list(switches), "kitSwitches": []}, f)

    def rows(self): return table(make(self.folder)[0], "Software latency (built-in test; software, not input-to-photon)")

    def test_same_conditions_give_a_verdict_and_say_what_was_checked(self):
        self.arm("mod-legacy", 100.0, extra=full_extra()); self.arm("mod-all", 94.0, extra=full_extra())
        row = self.rows()["mod-all"]
        self.assertEqual(row["Verdict"], "lower than the baseline")
        self.assertEqual(row["Conditions"], "same stream settings, present mode, variable refresh")

    def test_different_stream_settings_are_refused(self):
        self.arm("mod-legacy", 100.0); self.arm("mod-all", 80.0, stream="profile=1920x1080@60 codec=hevc requestedBitrateKbps=15000")
        md, _h, notes, _l = make(self.folder)
        row = table(md, "Software latency (built-in test; software, not input-to-photon)")["mod-all"]
        self.assertTrue(row["Verdict"].startswith("REFUSED: stream settings differ"), row["Verdict"])
        self.assertIn("15000", row["Verdict"]); self.assertIn("30000", row["Verdict"])
        self.assertEqual(row["Mean difference"], "refused (vs mod-legacy)"); self.assertEqual(row["95 % interval"], "")
        self.assertNotIn("lower", row["Verdict"]); self.assertNotRegex(row["Mean difference"], r"\d ms")
        self.assertTrue(any("REFUSED" in n and "'mod-all'" in n for n in notes))
        self.assertRegex(row["Mean ms"], r"^\d+\.\d$")                                    # the arm's own numbers stay

    def test_resolution_codec_and_frame_rate_count_as_stream_settings(self):
        self.arm("mod-legacy", 100.0)
        self.arm("mod-all", 100.0, stream="profile=1280x720@60 codec=hevc requestedBitrateKbps=30000")
        self.arm("mod-input", 100.0, stream="profile=1920x1080@60 codec=h264 requestedBitrateKbps=30000")
        self.arm("mod-ownerwait", 100.0, stream="profile=1920x1080@30 codec=hevc requestedBitrateKbps=30000")
        rows = self.rows()
        for name in ("mod-all", "mod-input", "mod-ownerwait"): self.assertTrue(rows[name]["Verdict"].startswith("REFUSED: stream settings differ"), name)

    def test_different_present_mode_or_tearing_is_refused(self):
        self.arm("mod-legacy", 100.0); self.arm("mod-all", 90.0, mode="Composed: Flip")
        row = self.rows()["mod-all"]
        self.assertTrue(row["Verdict"].startswith("REFUSED: present mode differs"), row["Verdict"]); self.assertIn("Composed: Flip", row["Verdict"])

    def test_different_variable_refresh_state_is_refused(self):
        self.arm("mod-legacy", 100.0, extra=[vrr_line(2, "true")]); self.arm("mod-all", 90.0, extra=[vrr_line(2, "false", 0)])
        self.assertTrue(self.rows()["mod-all"]["Verdict"].startswith("REFUSED: variable refresh state differs"))

    def test_an_arm_that_changed_settings_between_its_runs_is_refused(self):
        self.arm("mod-legacy", 100.0)
        self.arm("mod-all", 90.0, runs=2); self.arm("mod-all", 90.0, runs=1, stream="profile=1920x1080@60 codec=hevc requestedBitrateKbps=10000")
        self.assertIn("more than one stream setting", self.rows()["mod-all"]["Verdict"])

    def test_runs_of_one_arm_with_different_switches_are_refused(self):
        self.arm("mod-legacy", 100.0)
        self.arm("mod-all", 90.0, runs=2); self.arm("mod-all", 90.0, runs=1, switches=["VEYRA_EXP_RP_PAD_POLL_MS=1"])
        self.assertIn("did not all have the same switches", self.rows()["mod-all"]["Verdict"])

    def test_what_one_side_did_not_record_is_named_not_assumed_equal(self):
        self.arm("mod-legacy", 100.0, presentmon=False); self.arm("mod-all", 94.0)
        row = self.rows()["mod-all"]
        self.assertEqual(row["Verdict"], "lower than the baseline")
        self.assertEqual(row["Conditions"], "same stream settings; NOT CHECKED (not recorded on one side): present mode, variable refresh")

    def test_camera_comparison_between_kit_arms_follows_the_same_rule(self):
        self.arm("original", 100.0); self.arm("mod-all", 90.0, stream="profile=1920x1080@60 codec=hevc requestedBitrateKbps=15000")
        with open(os.path.join(self.folder, "camera.csv"), "w") as f:
            f.write("arm,press,frames,camera_fps,note\n" + "".join(f"original,{i},30,240,\n" for i in range(1, 41)) + "".join(f"mod-all,{i},27,240,\n" for i in range(1, 41)) +
                    "".join(f"official-app,{i},33,240,\n" for i in range(1, 41)))
        rows = table(make(self.folder)[0], "Input to photon (camera)")
        self.assertTrue(rows["mod-all"]["Verdict"].startswith("REFUSED: stream settings differ"))
        self.assertEqual(rows["official-app (camera only)"]["Verdict"], "higher than the baseline")      # nothing is known about a camera-only arm


class Completeness(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.folder = self.tmp.name
    def tearDown(self): self.tmp.cleanup()

    def test_every_value_is_listed_with_its_source_and_missing_ones_say_how_to_get_them(self):
        rng = random.Random(21)
        evidence(self.folder, {"mod-legacy": [presses(rng, 100.0, 40) for _ in range(3)]})
        add_run(self.folder, "mod-all", 40, run_log(presses(rng, 100.0, 40), extra=full_extra(generated=1000)))
        md, _h, notes, _l = make(self.folder)
        holds = table(md, "What this evidence holds")
        self.assertEqual(len(holds), 16)
        for label in ("Software latency per press", "Displayed frame times, dropped frames", "Present mode and tearing", "Variable refresh state", "GPU load and video memory", "Decoder load", "CPU load",
                      "Network health: loss, jitter, late packets, bitrate, route", "Recovery events", "Duplicate and cadence statistics", "Frames presented: real, generated",
                      "Stream settings: resolution, frame rate, codec, requested bitrate", "Decoder in use", "Hardware and Windows settings", "Build", "Switches"):
            self.assertIn(label, holds); self.assertTrue(holds[label]["Comes from"])
        full = {label: row["mod-all"] for label, row in holds.items()}
        self.assertEqual([label for label, text in full.items() if "MISSING" in text and label != "Build"], [])
        self.assertEqual(full["Variable refresh state"], "active=true"); self.assertEqual(full["Network health: loss, jitter, late packets, bitrate, route"], "19 intervals of 5 s")
        self.assertEqual(full["Decoder in use"], "D3D12VA"); self.assertIn("3.2.1", full["Build"]); self.assertIn("program's own video memory", full["GPU load and video memory"])
        bare = {label: row["mod-legacy"] for label, row in holds.items()}
        self.assertIn("-Health", bare["Network health: loss, jitter, late packets, bitrate, route"]); self.assertIn("VEYRA_EXP_RP_STREAM_DEDUP", bare["Duplicate and cadence statistics"])
        self.assertTrue(bare["Variable refresh state"].startswith("MISSING")); self.assertEqual(bare["Software latency per press"], "120 presses")
        self.assertTrue(any(n.startswith("Arm 'mod-legacy' has no record of: Variable refresh state; Network health") for n in notes))
        self.assertFalse(any(n.startswith("Arm 'mod-all' has no record of") for n in notes))

    def test_network_frames_and_conditions_tables(self):
        rng = random.Random(22)
        extra = full_extra(generated=1000) + [health_line(120, loss=1.5, jitter=6.0, late=7, grade="unstable", lost=140, keyframes=1, resets=1, drops=3)]
        add_run(self.folder, "mod-all", 41, run_log(presses(rng, 100.0, 40), extra=extra))
        md = make(self.folder)[0]
        net = table(md, "Network health")["mod-all"]
        self.assertEqual(net["Intervals"], "20"); self.assertEqual(net["Loss % worst interval"], "1.500"); self.assertEqual(net["Late packets"], "7")
        self.assertEqual(net["Worst interval grade"], "unstable"); self.assertEqual(net["Route and link"], "direct-lan, ethernet, 1000 Mbit/s")
        self.assertEqual(net["Round trip ms (at connect)"], "0.45"); self.assertEqual(net["FEC recovered / failed"], "40 / 0"); self.assertEqual(net["Frames dropped (network)"], "3")
        self.assertAlmostEqual(float(net["Frame jitter ms peak"]), 18.0); self.assertEqual(net["Packets lost"], str(sum(k % 3 for k in range(1, 20)) + 140))
        flow = table(md, "Frames by origin, duplicates and cadence")["mod-all"]
        self.assertEqual(flow["Real presented"], "6000"); self.assertEqual(flow["Generated presented"], "3000"); self.assertEqual(flow["Generated %"], "33.3")
        self.assertEqual(flow["Cadence state"], "doubled"); self.assertEqual(flow["Repeats removed"], "299"); self.assertEqual(flow["Duplicate removal"], "on")
        ran = table(md, "What each arm ran with")["mod-all"]
        self.assertEqual(ran["Stream"], "1920x1080@60 HEVC 30000 kbit/s"); self.assertEqual(ran["Decoder"], "D3D12VA"); self.assertIn("active=true level 2", ran["Variable refresh"])
        self.assertTrue(ran["Frame generation"].startswith("on (3000 generated frames presented)")); self.assertIn("3.2.1", ran["Build"])
        health = table(md, "Stream health during the runs")["mod-all"]
        self.assertEqual(health["Receive to decoded ms, median (derived)"], "2.00"); self.assertEqual(health["Present to scanout ms, median of P50 (program)"], "2.10")
        resources = table(md, "GPU, CPU and memory")["mod-all"]
        self.assertEqual(resources["PushFPS video memory MiB max (its own log)"], "1200.0"); self.assertEqual(resources["PushFPS GPU 3D % mean"], "n/a")

    def test_frame_counters_that_restart_are_added_not_lost(self):
        lines = [flow_line(10, 600, 0), flow_line(20, 1200, 0), flow_line(21, 1200, 0, state="closed"), flow_line(30, 300, 600, session=2), flow_line(40, 900, 1800, session=2),
                 flow_line(50, 60, 0, session=2), flow_line(60, 120, 10, session=2)]                       # a settings change restarts the window inside session 2
        log = report.parse_log(lines)
        self.assertEqual(log["flow"]["realPresented"], 1200 + 900 + 120); self.assertEqual(log["flow"]["generatedPresented"], 1800 + 10); self.assertEqual(log["flow_lines"], 7)

    def test_problem_lines_are_called_out(self):
        rng = random.Random(23)
        extra = [line(50, "crash", "unhandled exception code=0xC0000005 address=0x1 module=[path] offset=0x1 thread=5", "ERROR"), line(60, "gpu-failure", "operation=present alive=false removedReason=0x887A0006", "ERROR"),
                 line(70, "log-backpressure", "dropped=12 (bounded asynchronous output queue; realtime producers continued)", "WARN ")]
        add_run(self.folder, "mod-all", 42, run_log(presses(rng, 100.0, 40), extra=extra))
        notes = make(self.folder)[2]
        self.assertTrue(any("'mod-all': 1 crash line(s)" in n for n in notes)); self.assertTrue(any("1 GPU failure line(s)" in n for n in notes)); self.assertTrue(any("12 log line(s) dropped" in n for n in notes))


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


class PresentationTags(unittest.TestCase):
    """Tags the presentation investigation needs stay in the evidence, through the same masks as everything else."""
    def test_tags_kept_and_masked(self):
        lines = [line(1, "qml-window", "size=2560x1440 dpr=1.00 visibility=FullScreen"), line(2, "ui-dialog", "opened key=settings"),
                 line(3, "ui-fullscreen", "enabled=true"), line(4, "ui-window", "maximized=false"),
                 line(5, "present-contract", "backend=DXGI sync=1 flags=0x0 hr=0x0"), line(6, "slipstream-present", "flip=independent tearing=0"),
                 line(7, "slipstream-swapchain", "buffers=3 waitable=1 size=2560x1440"),
                 line(8, "ui-dialog", "opened key=connect host=PlantedHost 192.168.1.20"), line(9, "ui-window", "title account=PlantedAccount")]
        kept = report.safe_lines(lines)
        self.assertEqual(len(kept), 8)                       # the line with a private word is refused
        joined = "\n".join(kept)
        for tag in ("qml-window", "ui-dialog", "ui-fullscreen", "ui-window", "present-contract", "slipstream-present", "slipstream-swapchain"): self.assertIn("[" + tag + "]", joined)
        self.assertNotIn("PlantedHost", joined); self.assertNotIn("192.168.1.20", joined); self.assertNotIn("PlantedAccount", joined)

class RetestSyncFacts(unittest.TestCase):
    """Stream sync's per-run facts of the retest page (report.sync_run_parts = run-tests.ps1 Get-SyncRunParts), on the line
    formats of exp/s6-sync: wireFps (-1 = unknown, skipped), clockRate, pictureTerm, 5 s window counters summed, the
    transport's running counters at the end, the last causes line under either tag."""
    def test_parts_from_real_line_formats(self):
        stamp = lambda s, tag, text: f"2026-11-21T18:{s // 60:02d}:{s % 60:02d}.000Z t=1 [INFO ] [{tag}] {text}"
        lines = [stamp(1, "remoteplay-audio", "pictureTerm=pts (the automatic sync waits for the video PTS; VEYRA_TEST_RP_LEGACY_AUDIO_PICTURE=1 gives the PTS alone)"),
                 stamp(10, "remoteplay-clock", "wireIndex=600 ptsMs=10000.000 arrivalMinusPtsMs=5.000 nominalFps=60 wireFps=-1.000 clockRate=1 (arrival-disciplined local estimate, not PS5 native PTS; wireFps: frames the console sent per second)"),
                 stamp(20, "remoteplay-clock", "wireIndex=1200 ptsMs=20000.000 arrivalMinusPtsMs=6.000 nominalFps=60 wireFps=58.600 clockRate=1 (x)"),
                 stamp(30, "remoteplay-clock", "wireIndex=1800 ptsMs=30000.000 arrivalMinusPtsMs=7.000 nominalFps=60 wireFps=59.000 clockRate=1 (x)"),
                 stamp(12, "remoteplay-health", "grade=good interval=good seconds=5.0 lossPercent=0.000 decoderResets=1 frameDrops=0 consoleTargetKbps=-1"),
                 stamp(17, "remoteplay-health", "grade=good interval=good seconds=5.0 lossPercent=0.000 decoderResets=0 frameDrops=3 consoleTargetKbps=-1"),
                 stamp(12, "slipstream-net", "window=5.0 recoveries=1 ingressDropped=2 gapMaxMs=34.5 waitingIdrMs=120"),
                 stamp(17, "slipstream-net", "window=5.0 recoveries=0 ingressDropped=1 gapMaxMs=51.2 waitingIdrMs=0"),
                 stamp(18, "remoteplay-transport", "attempt=0 connected=true callbackRejected=4 assemblyErrors=7 (upstream)"),
                 stamp(28, "remoteplay-transport", "attempt=0 connected=true callbackRejected=9 assemblyErrors=29 (upstream)"),
                 stamp(13, "remoteplay-ingress", "causes episodes=1 malformed=0 gap=1 lost=0 decoder=0 depthAtLast=2 refusedWaiting=3 staleClockTolerated=0 callbackRejected=4 (since the stream started)"),
                 stamp(25, "remoteplay-recovery", "causes episodes=3 malformed=0 gap=1 lost=0 tooOld=0 staleClock=2 decoder=0 depthAtLast=2 refusedWaiting=9 staleClockTolerated=1 callbackRejected=9 (since the stream started)")]
        parts = report.sync_run_parts({"lines": lines})
        self.assertEqual(parts, ["console frames per second (wireFps) 58.80", "video clock follows the console 1", "audio picture term pts",
                                 "decoder resets 1", "frame drops 3", "recoveries 1", "ingress dropped 3", "waiting for a keyframe max 120 ms",
                                 "frame gap max 51.2 ms", "callbacks rejected 9", "assembly errors 29", "resets by cause gap=1 staleClock=2 (episodes 3)"])
        # nothing logged (a build without stream sync's lines): said, not guessed
        self.assertEqual(report.sync_run_parts({"lines": []}), ["console frames per second (wireFps) not logged", "video clock follows the console not logged",
                         "audio picture term not logged", "decoder resets not logged", "frame drops not logged", "recoveries not logged", "ingress dropped not logged",
                         "waiting for a keyframe max not logged", "frame gap max not logged", "callbacks rejected not logged", "assembly errors not logged",
                         "resets by cause not logged"])
        # a causes line under [remoteplay-recovery] is a count, not a recovery event
        log = report.parse_log([lines[-1]])
        self.assertEqual(log["recovery_events"], [])

if __name__ == "__main__":
    unittest.main(verbosity=1)
