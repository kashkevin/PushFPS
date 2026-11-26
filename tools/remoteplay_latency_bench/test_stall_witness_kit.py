# SPDX-License-Identifier: GPL-3.0-or-later
# Stream "s10-stall": the kit's side of the capture-stall witness and the stall evidence collector.
#  - the nameless summary [capture-stall-cause] is on every allow-list, the private [stall-witness] on none;
#  - collect-diagnostics.ps1 copies [stall-witness] lines only with -IncludeProcessNames, into their own file, and reads
#    only PushFPS's own log files (a stall-evidence file next to them never reaches the zip);
#  - Collect-Stall-Evidence.ps1 is read-only: none of the commands that change a Windows PC appear in it.
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
KIT = os.path.join(ROOT, "scripts", "remoteplay", "testkit")
sys.path.insert(0, HERE)


def read(path):
    with open(path, encoding="ascii") as handle:
        return handle.read()


class StallWitnessAllowLists(unittest.TestCase):
    def test_summary_allowed_private_lines_not(self):
        import report
        self.assertTrue(report.ALLOWED.search("[INFO ] [capture-stall-cause] stall=1 kind=gap gapMs=137.2"))
        self.assertTrue(report.ALLOWED.search("[INFO ] [capture-stall] kind=gap gapMs=137.2"))
        self.assertFalse(report.ALLOWED.search("[INFO ] [stall-witness] stall=1 dt=+0.5s top=tool.exe:61.0"))
        self.assertFalse(report.ALLOWED.search("[INFO ] [capture-stall-causes] x=1"))
        lines = ["2026-11-25T00:00:00.000Z t=1 [INFO ] [stall-witness] stall=1 top=tool.exe:61.0",
                 "2026-11-25T00:00:00.000Z t=1 [INFO ] [capture-stall-cause] stall=1 kind=gap"]
        kept = report.safe_lines(lines) if hasattr(report, "safe_lines") else [l for l in lines if report.ALLOWED.search(l)]
        self.assertEqual(len(kept), 1)
        self.assertIn("capture-stall-cause", kept[0])

    def test_three_copies_of_the_allow_list_agree(self):
        import report
        for name in ("run-tests.ps1", "collect-diagnostics.ps1"):
            self.assertIn("'" + report.ALLOWED.pattern + "'", read(os.path.join(KIT, name)), name)
        self.assertIn("stall|stall-cause", report.ALLOWED.pattern)


class Diagnostics(unittest.TestCase):
    def test_process_names_only_on_request_and_only_from_the_log(self):
        script = read(os.path.join(KIT, "collect-diagnostics.ps1"))
        self.assertIn("[switch]$IncludeProcessNames", script)
        self.assertIn("if ($IncludeProcessNames) {", script)
        self.assertIn("'\\[(INFO |WARN |ERROR|TRACE)\\] \\[stall-witness\\] '", script)
        self.assertIn("('stall-witness-' + $label + '.txt')", script)
        # Only PushFPS's own log files are read for lines; nothing named stall-evidence / stall-watch.
        self.assertEqual(sorted(set(re.findall(r"'(veyra-qml[^']*)'", script))), ["veyra-qml.log", "veyra-qml.old.log"])
        self.assertNotIn("stall-evidence", script)
        self.assertNotIn("stall-watch", script)

    def test_dry_run_checks_both_ways(self):
        check = read(os.path.join(KIT, "ci", "check-dry-run.ps1"))
        self.assertIn("-DryRun -NoPing -IncludeProcessNames", check)
        self.assertIn("a [stall-witness] process name reached the default zip", check)
        self.assertIn("Collect-Stall-Evidence.ps1') -DryRun -OutFile", check)
        self.assertIn("-Mode watch -WatchSeconds 6", check)
        self.assertIn("fit: 2 of 3 stalls", check)


def evidence_fingerprint(text):
    """SHA-256 (first 12 hex) of Collect-Stall-Evidence.ps1 with LF line ends and the fingerprint value as twelve zeros."""
    import hashlib
    body = re.sub(r"\$toolFingerprint = '[0-9a-f]{12}'", "$toolFingerprint = '000000000000'", text.replace("\r\n", "\n"))
    return hashlib.sha256(body.encode("ascii")).hexdigest()[:12]


class Evidence(unittest.TestCase):
    def script(self):
        return read(os.path.join(KIT, "Collect-Stall-Evidence.ps1"))

    def test_it_is_read_only(self):
        # Help text (the comment block) may name what the script does NOT do; the code may not do it.
        code = self.script().split("#>", 1)[1]
        code = "\n".join(l for l in code.splitlines() if not l.lstrip().startswith("#"))
        forbidden = [r"Set-ItemProperty", r"New-ItemProperty", r"Remove-ItemProperty", r"\breg(\.exe)? (add|delete|import)", r"Set-Service",
                     r"Stop-Service", r"Start-Service", r"Stop-Process", r"Register-ScheduledTask", r"Unregister-ScheduledTask",
                     r"Set-ScheduledTask", r"Enable-ScheduledTask", r"Disable-ScheduledTask", r"Start-ScheduledTask", r"schtasks(\.exe)? /(create|change|delete|run|end)",
                     r"wevtutil", r"Limit-EventLog", r"Clear-EventLog", r"powercfg(\.exe)? /(set|change|setactive|import|delete|h|hibernate|duplicatescheme)",
                     r"Disable-PnpDevice", r"Enable-PnpDevice", r"Set-NetAdapter", r"Set-CimInstance", r"Invoke-CimMethod", r"Start-Process",
                     r"\bbcdedit\b", r"Set-ExecutionPolicy", r"\bwpr\b", r"\blogman\b", r"RunAs", r"Set-Content -LiteralPath \(Join-Path \$LogFolder 'veyra"]
        for pattern in forbidden:
            self.assertIsNone(re.search(pattern, code, re.IGNORECASE), pattern)
        # It writes only its own output file (and, in a dry run, its made-up log in a temporary folder).
        writes = re.findall(r"(Set-Content -LiteralPath (?:\(Join-Path )?\S+|AppendAllText\(\$\w+)", code)
        self.assertTrue(all(("$OutFile" in w) or ("dryFolder" in w) for w in writes), writes)
        self.assertIn("powercfg.exe /query SCHEME_CURRENT", code)
        self.assertIn("powercfg.exe /getactivescheme", code)

    def test_it_lists_its_commands_and_masks_names(self):
        script = self.script()
        for needed in ("$commandsReport = @(", "$commandsWatch = @(", "READ-ONLY. Nothing was changed on this PC. Commands this run uses:", "function Hide-Private", "function Hide-Serial",
                       "$env:COMPUTERNAME, $env:USERNAME", "S-1-5-21-", "[user-folder]", "It was NOT switched on"):
            self.assertIn(needed, script, needed)

    def test_watch_is_bounded_and_measures_itself(self):
        script = self.script()
        self.assertIn("[math]::Min($WatchMinutes, 20) * 60", script)
        self.assertIn("[math]::Min($WatchSeconds, 1200)", script)
        self.assertIn("+ 90, 1200)", script)
        self.assertIn("selfCpuMs=", script)
        self.assertIn("the process list is dropped from here on", script)
        self.assertIn("stopped by itself: two seconds over the budget", script)
        self.assertIn("[int]$SampleBudgetMs = 30", script)

    def test_cmd_wrapper_has_crlf_line_ends(self):
        with open(os.path.join(KIT, "Collect-Stall-Evidence.cmd"), "rb") as handle:
            raw = handle.read()
        text = raw.decode("ascii")
        self.assertIn('-File "%~dp0Collect-Stall-Evidence.ps1" %*', text)
        self.assertIn("READ-ONLY", text)
        self.assertIn("AFTER playing", text)
        self.assertEqual(raw.count(b"\n"), raw.count(b"\r\n"), "every line of the .cmd ends in CR LF")
        with open(os.path.join(ROOT, ".gitattributes"), encoding="ascii") as handle:
            self.assertIn("scripts/remoteplay/testkit/*.cmd text eol=crlf", handle.read().splitlines())

    def test_fingerprint_is_current(self):
        text = self.script()
        stated = re.search(r"\$toolFingerprint = '([0-9a-f]{12})'", text).group(1)
        self.assertEqual(stated, evidence_fingerprint(text), "run: python3 -B tools/remoteplay_latency_bench/test_stall_witness_kit.py --fix-fingerprint")

    def test_masks_addresses_and_names(self):
        script = self.script()
        for needed in ("'[ip]'", "'[ipv6]'", "'[mac]'", "'[sid]'", "'[user-folder]'", "'[email]'", "'[removed]'"):
            self.assertIn(needed, script, needed)
        # The same patterns in Python, on the kinds of text the file holds: addresses go, versions and times stay.
        ipv4 = re.compile(r"(?<![\d.])(?:(?:25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(?:25[0-5]|2[0-4]\d|1?\d?\d)(?![\d.])")
        self.assertTrue(ipv4.search("dhcp lease 192.168.1.77 renewed"))
        self.assertTrue(ipv4.search("from 203.0.113.9:443"))
        self.assertIsNone(ipv4.search("driver 32.0.15.6094 2025-01-01"))
        self.assertIsNone(ipv4.search("2026-11-25 04:16:32.227Z"))
        ipv6 = re.compile(r"(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){3,7}[0-9A-Fa-f]{1,4}(?![0-9A-Za-z:])")
        self.assertTrue(ipv6.search("peer fe80:0:0:0:1ff:fe23:4567:890b"))
        self.assertIsNone(ipv6.search("04:16:32.227Z quarter+5.45 s"))

    def test_finds_package15_stalls_and_fits_the_model(self):
        script = self.script()
        for needed in ("[capture-timing] ", "[capture-callback] ", "[live-anchor] ", "[capture-stall] kind=gap", "veyra-qml.old.log", "128MB",
                       "[System.IO.FileShare]'ReadWrite,Delete'", "$modelFirst = @{ 0 = 11.05; 1 = 16.75; 2 = 5.45 }", "$modelSecond = 17.0",
                       "predicted next stalls", "the pattern IS still there", "read ' + $found.lines + ' lines", "PriorityClass = 'BelowNormal'",
                       "Best run AFTER playing", "It stays on this PC unless you send it", "-Last 24", "-FilterXPath $xpath", "Id = @(100, 129, 200)",
                       "within 20 s of ' + $hits + ' of '", "# done: this run took"):
            self.assertIn(needed, script, needed)

    def test_model_matches_the_research(self):
        # The research's per-phase means (QUARTER_HOUR_STALLS.md part C) and the owner's logs: 22:15:05.31 (n mod 3 = 2),
        # 21:45:11.14 (0), 22:45:16.60 (1) are first stalls; 22:15:22.51 is a second one.
        first = {0: 11.05, 1: 16.75, 2: 5.45}
        def fit(h, m, s):
            sec = h * 3600 + m * 60 + s
            q = sec // 900
            off = sec - q * 900
            f = first[q % 3]
            return min(abs(off - f), abs(off - f - 17.0))
        for t in ((22, 15, 5.31), (21, 45, 11.14), (22, 45, 16.60), (22, 15, 22.51), (2, 30, 16.95), (0, 0, 10.99)):
            self.assertLessEqual(fit(*t), 1.5, t)
        self.assertGreater(fit(23, 22, 29.79), 1.5)   # an off-pattern stall


class Trace(unittest.TestCase):
    def test_the_trace_draft_is_not_in_the_kit(self):
        # Review S10_STALL_REVIEW1 verdict C: held back with its TODO list.
        self.assertFalse(os.path.exists(os.path.join(KIT, "Record-Stall-Trace.ps1")))
        self.assertTrue(os.path.exists(os.path.join(ROOT, "docs", "drafts", "Record-Stall-Trace.ps1")))
        self.assertNotIn("Record-Stall-Trace", read(os.path.join(KIT, "ci", "check-dry-run.ps1")))
        todo = read(os.path.join(ROOT, "docs", "drafts", "STALL_TRACE_TODO.md"))
        for item in ("outside the `try`", "Closing the window with X", "Package 15 does not write", "no level or keyword limits", "16 MB"):
            self.assertIn(item, todo)


if __name__ == "__main__":
    if sys.argv[1:] == ["--fix-fingerprint"]:
        path = os.path.join(KIT, "Collect-Stall-Evidence.ps1")
        text = read(path)
        value = evidence_fingerprint(text)
        with open(path, "w", encoding="ascii", newline="") as handle:
            handle.write(re.sub(r"\$toolFingerprint = '[0-9a-f]{12}'", "$toolFingerprint = '" + value + "'", text))
        print(value)
    else:
        unittest.main()
