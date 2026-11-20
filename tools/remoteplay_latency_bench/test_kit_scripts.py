#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Static checks of the PowerShell scripts of the hardware test kit (scripts/remoteplay/testkit/*.ps1).

  python3 -B tools/remoteplay_latency_bench/test_kit_scripts.py

There is no PowerShell where these scripts are written, so this is what can be checked without one: brackets
and quotes balance, every command is a function of the script or a cmdlet on a short list, every variable is
assigned somewhere, nothing needs PowerShell 7, the file is plain ASCII (Windows PowerShell 5.1 reads a file
without a byte order mark in the local code page). It is NOT a parser and proves nothing about behaviour: the
checks workflow parses and runs the scripts on Windows.
"""
import glob, math, os, re, sys, unittest

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.join(HERE, "..", "..", "scripts", "remoteplay", "testkit")
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))

# Cmdlets the kit may call (all exist in Windows PowerShell 5.1).
CMDLETS = set("""Add-Content Compress-Archive ConvertFrom-Csv ConvertFrom-Json ConvertTo-Json Copy-Item Expand-Archive ForEach-Object Get-ChildItem
Get-CimInstance Get-Command Get-Content Get-Date Get-FileHash Get-Item Get-ItemProperty Get-Location Get-Process Join-Path Move-Item New-Item
New-Object Out-Null Read-Host Remove-Item Resolve-Path Select-Object Select-String Set-Content Set-Item Sort-Object Split-Path Start-Process
Start-Sleep Stop-Process Test-Path Where-Object Write-Host Get-NetAdapter Get-NetRoute Test-Connection Measure-Object Out-File Out-String
Get-Counter Write-Output Get-Variable Add-Type Test-NetConnection Get-NetIPConfiguration Get-NetAdapterAdvancedProperty Get-NetIPAddress
Get-AuthenticodeSignature Rename-Item Add-Member""".split())
AUTOMATIC = set("_ true false null args matches lastexitcode psversiontable myinvocation erroractionpreference input this psscriptroot host pid error "
                "pscmdlet psboundparameters pwd home".split())


def scan(text):
    """Splits a script into code with strings and comments blanked out, and the list of double-quoted strings.
    Returns (code, strings, problems)."""
    out, strings, problems = [], [], []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n": line += 1
        if text.startswith("<#", i):
            end = text.find("#>", i + 2)
            if end < 0: problems.append(f"line {line}: block comment is not closed"); break
            block = text[i:end + 2]; line += block.count("\n"); out.append(re.sub(r"[^\n]", " ", block)); i = end + 2; continue
        if c == "#" and (i == 0 or text[i - 1] in " \t\n;({"):
            end = text.find("\n", i); end = n if end < 0 else end
            out.append(" " * (end - i)); i = end; continue
        if c == "@" and i + 1 < n and text[i + 1] in "'\"" and text[i + 2:i + 3] == "\n":
            problems.append(f"line {line}: here-strings are not used in the kit (this check cannot read them)"); break
        if c == "'":
            j = i + 1
            while j < n:
                if text[j] == "'":
                    if text[j + 1:j + 2] == "'": j += 2; continue
                    break
                if text[j] == "\n": problems.append(f"line {line}: single-quoted string runs over the end of the line"); break
                j += 1
            else: problems.append(f"line {line}: single-quoted string is not closed")
            out.append("'" + " " * (j - i - 1) + "'"); i = j + 1; continue
        if c == '"':
            j, depth = i + 1, 0
            while j < n:
                d = text[j]
                if d == "`": j += 2; continue
                if d == "$" and text[j + 1:j + 2] == "(": depth += 1; j += 2; continue
                if depth and d == "(": depth += 1
                elif depth and d == ")": depth -= 1
                elif d == '"' and not depth:
                    if text[j + 1:j + 2] == '"': j += 2; continue
                    break
                elif d == "\n" and not depth: problems.append(f"line {line}: double-quoted string runs over the end of the line"); break
                j += 1
            else: problems.append(f"line {line}: double-quoted string is not closed")
            strings.append((line, text[i + 1:j])); line += text[i:j].count("\n")
            out.append('"' + re.sub(r"[^\n]", " ", text[i + 1:j]) + '"'); i = j + 1; continue
        out.append(c); i += 1
    return "".join(out), strings, problems


def brackets(code):
    pairs, stack, problems = {")": "(", "}": "{", "]": "["}, [], []
    line = 1
    for c in code:
        if c == "\n": line += 1
        elif c in "({[": stack.append((c, line))
        elif c in ")}]":
            if not stack or stack[-1][0] != pairs[c]:
                problems.append(f"line {line}: '{c}' closes nothing" if not stack else f"line {line}: '{c}' closes the '{stack[-1][0]}' of line {stack[-1][1]}")
                return problems
            stack.pop()
    return problems + [f"line {l}: '{c}' is never closed" for c, l in stack]


def check_script(path):
    with open(path, "rb") as handle: raw = handle.read()
    problems = []
    # A Windows checkout (core.autocrlf) turns every line end into CR LF, which PowerShell reads the same: only a
    # carriage return that is not part of a line end is a mistake.
    raw = raw.replace(b"\r\n", b"\n")
    bad = [i for i, b in enumerate(raw) if b > 126 or (b < 32 and b not in (9, 10))]
    if bad: problems.append(f"byte {bad[0]} (line {raw[:bad[0]].count(10) + 1}) is not plain ASCII or is a lone carriage return")
    text = raw.decode("ascii", errors="replace")
    code, strings, found = scan(text)
    problems += found + brackets(code)
    defined = {m.group(1).lower() for m in re.finditer(r"(?im)^\s*function\s+([A-Za-z]+-[A-Za-z0-9]+)", code)}
    for m in re.finditer(r"(?<![\w$:.\-\[])([A-Z][a-z]+-[A-Z][A-Za-z0-9]+)\b(?!-)", code):
        name = m.group(1)
        if name.lower() not in defined and name not in CMDLETS:
            problems.append(f"line {code[:m.start()].count(chr(10)) + 1}: '{name}' is neither a function of this script nor a cmdlet the kit may use")
    for token in ("&&", "||", "??", "?."):
        at = code.find(token)
        if at >= 0: problems.append(f"line {code[:at].count(chr(10)) + 1}: '{token}' needs PowerShell 7")
    # variables: assigned somewhere (also as a parameter, a loop variable or by [ref]) before being read anywhere
    variable = re.compile(r"\$(?:(script|global|local|env):)?([A-Za-z_][A-Za-z0-9_]*)")
    assigned = set()
    for m in re.finditer(r"\$(?:(?:script|global|local):)?([A-Za-z_][A-Za-z0-9_]*)\s*(?:=(?!=)|\+=|-=|\*=|/=|\+\+|--)", code): assigned.add(m.group(1).lower())
    for m in re.finditer(r"(?i)foreach\s*\(\s*\$([A-Za-z_][A-Za-z0-9_]*)\s+in\b", code): assigned.add(m.group(1).lower())
    for m in re.finditer(r"(?i)\[ref\]\s*\$([A-Za-z_][A-Za-z0-9_]*)", code): assigned.add(m.group(1).lower())
    for m in re.finditer(r"(?is)\bparam\s*\((.*?)\n\)", code):
        for v in re.finditer(r"\]\s*\$([A-Za-z_][A-Za-z0-9_]*)", m.group(1)): assigned.add(v.group(1).lower())
    for m in re.finditer(r"(?im)^\s*function\s+[A-Za-z]+-[A-Za-z0-9]+\s*\(([^{]*)\)\s*\{", code):
        for v in variable.finditer(m.group(1)): assigned.add(v.group(2).lower())
    for m in re.finditer(r"(?is)\bparam\s*\(([^{}]*?)\)", code):
        for v in variable.finditer(m.group(1)): assigned.add(v.group(2).lower())
    uses = [(code[:m.start()].count("\n") + 1, m) for m in variable.finditer(code)]
    for line, body in strings: uses += [(line, m) for m in variable.finditer(body)]
    for line, m in uses:
        scope, name = m.group(1), m.group(2).lower()
        if scope == "env" or name in AUTOMATIC or name in assigned: continue
        problems.append(f"line {line}: ${m.group(2)} is read but never assigned in this script")
    return sorted(set(problems), key=lambda p: int(re.match(r"line (\d+)", p).group(1)) if p.startswith("line ") else 0)


class Checker(unittest.TestCase):
    """The checker itself: it must find the mistakes it is there for."""
    def problems(self, text):
        import tempfile
        with tempfile.NamedTemporaryFile("wb", suffix=".ps1", delete=False) as f: f.write(text.encode("latin-1"))
        try: return check_script(f.name)
        finally: os.unlink(f.name)

    def test_clean_script(self):
        self.assertEqual(self.problems("param(\n    [string]$Name = 'a',\n    [switch]$Quick\n)\nfunction Get-It([string]$What) {\n    $x = @{}\n    foreach ($item in @(1, 2)) { $x[$item] = \"$What $($item) it's\" }   # a comment with ' and (\n    return $x\n}\n"
                                       "if ($Quick -and ($Name -eq 'don''t')) { Write-Host (Get-It $Name) }\ntry { $null = Get-Item $env:TEMP } catch { Write-Host $_.Exception.Message }\n"), [])

    def test_mistakes_are_found(self):
        self.assertTrue(any("never closed" in p for p in self.problems("function Get-It {\n    if ($true) {\n}\n")))
        self.assertTrue(any("closes the" in p for p in self.problems("$a = @(1, 2]\n")))
        self.assertTrue(any("'Get-Itt'" in p for p in self.problems("function Get-It { return 1 }\n$a = Get-Itt\n")))
        self.assertTrue(any("$nmae" in p for p in self.problems("$name = 1\nWrite-Host $nmae\n")))
        self.assertTrue(any("$nmae" in p for p in self.problems("$name = 1\nWrite-Host \"value $($nmae)\"\n")))
        self.assertTrue(any("PowerShell 7" in p for p in self.problems("$a = 1\n$b = $a ?? 2\n")))
        self.assertTrue(any("not closed" in p or "end of the line" in p for p in self.problems("$a = 'open\n$b = 1\n")))
        self.assertTrue(any("ASCII" in p for p in self.problems("$a = 'caf\xe9'\n")))
        self.assertTrue(any("ASCII" in p for p in self.problems("$a = 1\rWrite-Host $a\n")))
        self.assertEqual(self.problems("$a = 1\r\nWrite-Host $a\r\n"), [])                     # a Windows checkout


class KitScripts(unittest.TestCase):
    def test_every_kit_script_passes_the_static_checks(self):
        # The install / first-test / uninstall scripts share functions through slipstream-common.ps1 (dot-sourced), which
        # this single-file checker cannot follow; they are run for real by the package workflow's Windows verify job.
        shared = {"install-slipstream.ps1", "run-first-test.ps1", "uninstall-slipstream.ps1", "slipstream-common.ps1"}
        scripts = sorted(p for p in glob.glob(os.path.join(KIT, "*.ps1")) + glob.glob(os.path.join(KIT, "ci", "*.ps1"))
                         if os.path.basename(p) not in shared)
        self.assertEqual(sorted(os.path.basename(p) for p in scripts), ["check-dry-run.ps1", "collect-diagnostics.ps1", "run-tests.ps1"])
        for path in scripts:
            with self.subTest(script=os.path.basename(path)):
                self.assertEqual(check_script(path), [], os.path.basename(path))


    def test_scripts_that_dot_source_the_common_file_pass_with_it(self):
        """install / first-test / uninstall read their shared functions from slipstream-common.ps1: checked together."""
        import tempfile
        with open(os.path.join(KIT, "slipstream-common.ps1"), "rb") as handle: common = handle.read()
        for name in ("run-first-test.ps1", "install-slipstream.ps1", "uninstall-slipstream.ps1"):
            with open(os.path.join(KIT, name), "rb") as handle: own = handle.read()
            with tempfile.NamedTemporaryFile("wb", suffix=".ps1", delete=False) as f: f.write(common + b"\n" + own)
            try:
                with self.subTest(script=name): self.assertEqual(check_script(f.name), [], name)
            finally: os.unlink(f.name)

    def test_first_test_never_elevates_and_names_its_checks(self):
        with open(os.path.join(KIT, "run-first-test.ps1"), encoding="ascii") as handle: script = handle.read()
        self.assertNotIn("RunAs", script)
        for wanted in ("'Not found: the '", "Check only: nothing started.", "-Session", "'-Resume'", "'-Full'", "'-Versus'", "'-DryRun'", "streamed from a PS5"):
            self.assertIn(wanted, script)
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="ascii") as handle: kit = handle.read()
        self.assertIn("-Verb RunAs", kit)                                                    # only PresentMon is elevated
        self.assertEqual(kit.count("-Verb RunAs"), 1)
        self.assertIn("Get-AuthenticodeSignature -FilePath $presentMonExe", kit)
        self.assertIn("Intel Corporation", kit)


class DryRunLog(unittest.TestCase):
    """The made-up log lines of run-tests.ps1 -DryRun, taken from the script and put through report.py's copy of the
    kit's rules (the test in test_report.py holds the two copies equal): what the workflow will find in the zip."""
    PLANTED = ("Planted", "PLANTED", "0123456789abcdef", "192.168.1.77", "203.0.113.9", "AABBCCDDEEFF", "00:1A:2B", "fe80", "00112233445566778899",
               "example.com", "001A2B3C4D5E", "001a.2b3c", "0F8FAD5B", "1A2B3C4D5E6F7A8", "%12")

    def made_up_lines(self):
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="ascii") as handle: script = handle.read()
        script = script[script.index("function Add-DryRunLog("):script.index("# --- checking and packing")]
        lines = [m.group(1).replace("''", "'") for m in re.finditer(r"'(\[(?:INFO |WARN |ERROR)\] \[[^'\n]*(?:''[^'\n]*)*)'", script)]
        self.assertGreater(len(lines), 20)
        # the session line's bitrate is appended from a variable (-DryRunMismatchArm): the default one
        lines = [text + "30000" if text.endswith("requestedBitrateKbps=") else text for text in lines]
        return ["2026-11-18T12:00:00.000Z t=1234 " + text for text in lines]

    def test_nothing_planted_survives_and_kept_lines_are_stable(self):
        sys.path.insert(0, HERE)
        import report
        lines = self.made_up_lines()
        self.assertTrue(any(value in "\n".join(lines) for value in self.PLANTED))
        kept = report.safe_lines(lines)
        text = "\n".join(kept)
        for value in self.PLANTED: self.assertNotIn(value, text)
        for wanted in ("host=192.168.x.x", "host=[name]", "host=x.x.x.x", "[ipv6]", "[mac]", "[hex]", "path=[path]", "[update-check]", "[frame-flow]", "[display-vrr]", "[remoteplay-health] grade=",
                       "[email]", "[guid]", "server=[name]", "computer=[name]", "device=[name]", "network=[name]", "[slipstream-governor]", "[push-hardware]"):
            self.assertIn(wanted, text)
        for kept_line in kept:
            self.assertEqual(report.redact_line(kept_line), kept_line)                 # the kit's final check would otherwise withhold the file
            self.assertIsNone(report.PRIVATE_WORD.search(kept_line), kept_line)
        log = report.parse_log(kept)
        self.assertEqual(log["streams"], [("1920x1080@60", "HEVC", "30000")]); self.assertEqual(log["decoders"], ["D3D12VA"]); self.assertEqual(len(log["health"]), 1)
        self.assertIn("3.2.1", log["build"]); self.assertEqual(log["vrr"][-1][1]["active"], "true"); self.assertEqual(log["flow"]["realPresented"], 600)


class VerdictTexts(unittest.TestCase):
    def test_the_kits_own_verdict_lines_pass_its_final_check(self):
        """reliability.csv is checked like every text of the zip: a verdict text with a private word or an address-like
        value would get the file withheld."""
        sys.path.insert(0, HERE)
        import report
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="ascii") as handle: script = handle.read()
        body = script[script.index("function Get-StreamVerdict("):script.index("# --- one run of one arm")]
        texts = [m.group(2) for m in re.finditer(r"@\('(PASS|FAIL|NOT-EXERCISED|SKIPPED)', \(?'([^']+)'", body)]
        self.assertGreater(len(texts), 30)
        for text in texts:
            self.assertIsNone(report.PRIVATE_WORD.search(text), text); self.assertEqual(report.redact_line(text), text, text)
            self.assertNotIn('"', text); self.assertNotIn("latency", text.lower())


class ResultTexts(unittest.TestCase):
    def test_result_and_live_texts_pass_the_final_check(self):
        """RESULT.txt is checked like every text of the zip: a line the masks would change gets the file withheld."""
        sys.path.insert(0, HERE)
        import report
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="ascii") as handle: script = handle.read()
        body = script[script.index("# --- live log reading, the settings of a run"):script.index("# --- checking and packing")]
        checked = 0
        for m in re.finditer(r"'([^'\n]{12,})'", body):
            text = m.group(1)
            if "\\]" in text or "\\[" in text or text.startswith(("^", "(?", "[^")) or "\\s" in text or "\\d" in text: continue      # patterns
            checked += 1
            self.assertIsNone(report.PRIVATE_WORD.search(text), text); self.assertEqual(report.redact_line(text), text, text)
        self.assertGreater(checked, 40)
        self.assertIn("SOFTWARE LATENCY (the built-in test: software, not input-to-photon)", body)
        physical = body[body.index("PHYSICAL MEASUREMENTS"):body.index("MISSING APP LOG FIELDS")]
        self.assertNotIn("median", physical.lower())                                                    # never in one table


class KitAuthoredFiles(unittest.TestCase):
    def test_files_the_kit_copies_or_writes_pass_its_own_leak_check_unchanged(self):
        """Windows run #24 withheld arms.csv: every file the kit puts into the zip itself must come out of the masks
        unchanged, also with Windows line ends (a checkout with core.autocrlf)."""
        sys.path.insert(0, HERE)
        import report
        for name in ("arms.csv", "measures.csv", "camera-template.csv", "sensor-template.csv"):
            with open(os.path.join(KIT, name), "rb") as handle: raw = handle.read()
            for text in (raw.replace(b"\r\n", b"\n"), raw.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")):
                for line in re.split(r"\r?\n", text.decode("ascii")):
                    with self.subTest(file=name, line=line):
                        self.assertIsNone(report.PRIVATE_WORD.search(line)); self.assertEqual(report.redact_line(line), line)
        made = ['  "log": "bench/mod-all@20261119-110200.txt",', '  "configText": "1920x1080@60 HEVC 30000 kbit/s, refresh 180 Hz, variable refresh active, frame generation off, decoder D3D12VA",',
                '  "from": "run \'mod-all@20261119-110200\'",', '  "switches": [ "VEYRA_TEST_RP_LEGACY_OWNER_WAIT=1", "VEYRA_EXP_RP_GPU_BUDGET=1" ],', '  "sha256First10": "A1B2C3D4E5",',
                '  "sceneNote": "harbour pier, noon, camera north",', 'mod-all,1,29,240,', 'utc,cpuTotalPercent,appCpuPercent,appWorkingSetMb,appPrivateMb,appThreads,appHandles,appGpu3dPercent,appGpuDecodePercent,appGpuDedicatedMb,appGpuSharedMb,sampling',
                '2026-11-19T11:02:00.000Z,12,3.5,900.0,800.00,60,1500,10.5,3.1,1200.0,80.0,full', '  mod-all (modified build; no switch): 1920x1080@60 HEVC 30000 kbit/s, refresh 180 Hz']
        for line in made:
            with self.subTest(line=line): self.assertEqual(report.redact_line(line), line)


class Diagnostics(unittest.TestCase):
    """collect-diagnostics.ps1: the kit's rules, word for word, and a made-up log that must come out clean."""
    def script(self):
        with open(os.path.join(KIT, "collect-diagnostics.ps1"), encoding="ascii") as handle: return handle.read()

    def test_it_uses_the_rules_of_the_kit_and_of_the_report(self):
        sys.path.insert(0, HERE)
        import report
        script = self.script()
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="ascii") as handle: kit = handle.read()
        rule = re.compile(r"\$t = \$t -replace '([^']*)', '([^']*)'")
        self.assertEqual(rule.findall(script), rule.findall(kit))
        self.assertEqual(len(rule.findall(script)), len(report._MASKS))
        for name in ("$allowedLine", "$refusedLine"):
            line = next(l for l in kit.splitlines() if l.startswith(name + " = "))
            self.assertIn(line, script.splitlines())
        self.assertIn("'" + report.ALLOWED.pattern + "'", script)
        self.assertEqual(script.count(report.REFUSED_WORDS), 2)                      # the line filter and the final file check
        for function in ("Protect-Text", "Select-SafeLines"):
            body = lambda text: text[text.index("function " + function + "("):].split("\n}\n", 1)[0]
            self.assertEqual(body(script), body(kit), function)

    def test_made_up_log_of_the_dry_run_comes_out_clean(self):
        sys.path.insert(0, HERE)
        import report
        script = self.script()
        body = script[script.index("function New-DryRunCopy("):script.index("# --- start ---")]
        lines = ["2026-11-18T12:00:00.000Z t=1234 " + m.group(1) for m in re.finditer(r"'(\[(?:INFO |WARN |ERROR)\] \[[^'\n]*)'", body)]
        self.assertGreaterEqual(len(lines), 15)
        kept = report.safe_lines(lines)
        text = "\n".join(kept)
        for value in DryRunLog.PLANTED: self.assertIn(value, "\n".join(lines)); self.assertNotIn(value, text)
        for wanted in ("host=192.168.x.x", "host=[name]", "host=x.x.x.x", "[ipv6]", "[mac]", "[hex]", "path=[path]", "module=[path]", "[update-check]", "[remoteplay-health] grade=unsteady", "[crash]", "[email]", "[guid]", "computer=[name]"):
            self.assertIn(wanted, text)
        for kept_line in kept:
            self.assertEqual(report.redact_line(kept_line), kept_line); self.assertIsNone(report.PRIVATE_WORD.search(kept_line), kept_line)

    def test_it_says_what_is_not_collected_in_the_script_and_in_the_summary_it_writes(self):
        script = self.script()
        header, summary = script[:script.index("\nparam(")], script[script.index("'WHAT IS NOT IN THE ZIP'"):script.index("# --- final check and the zip")]
        for text in (" ".join(header.lower().split()), summary.lower()):
            for wanted in ("credentials", "console registration keys", "psn account ids", "tokens", "wi-fi network names", "your files", "crash dump files", "computer name", "user name", "settings store", "full log"):
                self.assertIn(wanted, text, wanted)
        self.assertIn("'WHAT THIS IS'", script); self.assertIn("'THIS PC IN SHORT'", script); self.assertIn("'THINGS THAT MAY CAUSE PROBLEMS'", script); self.assertIn("'WHAT IS IN THE ZIP'", script)
        self.assertIn("not the delay from button to screen", script)                       # the built-in test is named for what it is
        self.assertNotIn("Compress-Archive", script); self.assertNotIn("Get-FileHash", script)      # module cmdlets that fail under a foreign PSModulePath
        with open(os.path.join(KIT, "collect-diagnostics.cmd"), encoding="ascii") as handle: wrapper = handle.read()
        self.assertIn('-File "%~dp0collect-diagnostics.ps1"', wrapper); self.assertIn("pause", wrapper)

    def test_static_text_of_the_zip_passes_its_own_final_check(self):
        """Lines the tool writes itself (summary, headings) must not hold a private word or anything the masks change:
        the final check would leave the file out."""
        sys.path.insert(0, HERE)
        import report
        script = self.script()
        tail = script[script.index("# --- reading the log ---"):script.index("function New-DryRunCopy(")] + script[script.index("# --- start ---"):]
        own_list = tail[tail.index("'WHAT IS NOT IN THE ZIP'"):tail.index("Set-Content -LiteralPath (Join-Path $stage 'summary.txt')")]
        self.assertIn("Test-FileLeaks $file.FullName ($file.Name -ne 'summary.txt')", tail)    # only the summary may name what is not collected
        checked = 0
        for m in re.finditer(r"'([^'\n]{12,})'", tail):
            text = m.group(1)
            if "\\]" in text or "\\[" in text or text.startswith(("^", "(?")): continue      # patterns, not output
            checked += 1
            if m.group(0) not in own_list: self.assertIsNone(report.PRIVATE_WORD.search(text), text)
            self.assertEqual(report.redact_line(text), text, text)
        self.assertGreater(checked, 60)


class PrintedCommands(unittest.TestCase):
    """REVIEW_kit2 should-fix 3: every command line printed for the owner uses only parameters its script declares."""
    SCRIPTS = {"run-first-test.cmd": "run-first-test.ps1", "run-full-test.cmd": "run-first-test.ps1", "run-first-test": "run-first-test.ps1",
               "run-tests.ps1": "run-tests.ps1", "install-slipstream.cmd": "install-slipstream.ps1", "collect-diagnostics.cmd": "collect-diagnostics.ps1",
               "uninstall-slipstream.cmd": "uninstall-slipstream.ps1"}
    # Package 8: -NeuralRuntime Ask|Yes|No is added to install-slipstream.ps1 by the stream that owns that script (nrrun);
    # START-HERE names it. Until the two branches are merged the installer here does not declare it yet.
    PROMISED = {"install-slipstream.ps1": {"neuralruntime"}}
    def declared(self, script):
        text = open(os.path.join(ROOT, "scripts", "remoteplay", "testkit", script), encoding="utf-8-sig").read()
        start = re.search(r"(?m)^param\(", text).start()
        depth, end, opened = 0, start, False
        for end in range(start, len(text)):
            depth += {"(": 1, ")": -1}.get(text[end], 0); opened = opened or depth > 0
            if opened and depth == 0: break
        return {n.lower() for n in re.findall(r"\$(\w+)", text[start:end])} | self.PROMISED.get(script, set())
    def test_printed_commands(self):
        checked = 0
        for doc in (os.path.join(ROOT, "packaging", "slipstream", "START-HERE.template.txt"), os.path.join(ROOT, "docs", "HARDWARE_TEST_PROCEDURE.md")):
            for number, line in enumerate(open(doc, encoding="utf-8").read().splitlines(), 1):
                for m in re.finditer(r"(?i)\b(run-first-test\.cmd|run-full-test\.cmd|run-tests\.ps1|install-slipstream\.cmd|collect-diagnostics\.cmd|uninstall-slipstream\.cmd)((?:\s+(?:-\w+|[^\s`-][^\s`]*))*)", line):
                    names = re.findall(r"(?:^|\s)-([A-Za-z]\w*)", m.group(2))
                    if not names: continue
                    declared = self.declared(self.SCRIPTS[m.group(1).lower()])
                    for n in names:
                        if n.lower() in ("executionpolicy", "file", "noprofile"): continue
                        self.assertIn(n.lower(), declared, f"{os.path.basename(doc)}:{number}: {m.group(1)} -{n} is not a parameter of {self.SCRIPTS[m.group(1).lower()]}")
                        checked += 1
        self.assertGreater(checked, 10)
    def test_network_options_reach_the_kit(self):
        text = open(os.path.join(ROOT, "scripts", "remoteplay", "testkit", "run-first-test.ps1"), encoding="utf-8-sig").read()
        for n in ("Minutes", "Rounds", "NetworkWithBuilds", "WifiStreamingMode", "PresentMonPrompt"):
            self.assertIn(f"$arguments += @('-{n}'", text, n)

class RoughEdges(unittest.TestCase):
    """REVIEW_kit2 rough edges: stop reasons in plain words, 'stopped reason=' ends the running test, unknown never erases."""
    def setUp(self):
        self.text = open(os.path.join(KIT, "run-tests.ps1"), encoding="utf-8-sig").read()
    def test_stop_reasons_match_report(self):
        sys.path.insert(0, HERE); import report
        block = self.text[self.text.index("$stopReasonText = @{"):self.text.index("function Get-StopReasonText")]
        found = dict(re.findall(r"'([a-z-]+)' = '((?:[^']|'')*)'", block))
        self.assertEqual({k: v.replace("''", "'") for k, v in found.items()}, report.STOP_REASONS)
    def test_stopped_clears_the_running_test(self):
        i = self.text.index("stopped reason=(\\S+)')")
        self.assertIn("$State.inTrial = $false", self.text[i:i + 200])
    def test_explicit_unknown_never_erases(self):
        sys.path.insert(0, HERE); import report
        log = {"config": {"refreshHz": "180"}, "config_line": {"refreshHz": "-1", "fg": "0"}}
        self.assertEqual(report.normal_config(log)["refreshHz"], "180")
        self.assertIn("if ($null -ne $v) { $keys[$k] = $v }", self.text)

class Durations(unittest.TestCase):
    """REVIEW_final2 stale text: every duration START-HERE, the procedure and run-first-test.ps1 print is the one run-tests.ps1
    computes. Kit 5 (BENCH2: the estimates were 4 times too high): Get-PlanSeconds, from the measured runs of 20 Nov (15 s to
    start, connect and close, 3 s per press, 2 s per test, every settle / wait step its own seconds; 60 s more for a start
    done by hand); report.py plan_seconds holds the same numbers. Network runs: -Minutes plus 2."""
    def setUp(self):
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="ascii") as f: self.kit = f.read()
        with open(os.path.join(KIT, "run-first-test.ps1"), encoding="ascii") as f: self.first = f.read()
        with open(os.path.join(ROOT, "packaging", "slipstream", "START-HERE.template.txt"), encoding="ascii") as f: self.start = f.read()
        with open(os.path.join(ROOT, "docs", "HARDWARE_TEST_PROCEDURE.md"), encoding="utf-8") as f: self.proc = f.read()
        self.assertIn("$perStartSeconds = Get-PlanSeconds (Get-AutorunPlan 'latency' $script:sessionTrials $plannedTests)", self.kit)
        self.assertIn("if (-not ($allExperimental -and ($Session -ne 'network') -and (-not $NoAutomation))) { $perStartSeconds += 60.0 }", self.kit)
        for text in ("    $total = 15.0\n", "        if (@('settle', 'wait', 'probe', 'sr', 'fg') -contains $name) { $total += $amount }",
                     "        elseif ($name -eq 'latency') { $total += ($amount * 3.0 + 2.0) }", "        elseif ($name -eq 'quality') { $total += 30.0 }",
                     "    return [int][math]::Max(1, [math]::Floor($Seconds / 60.0 + 0.5))"):
            self.assertIn(text, self.kit)
        self.assertIn("[int]$Minutes = 3,", self.kit); self.assertIn("$networkRounds = 2\n", self.kit)
        self.assertIn("[int]$Rounds = 3,", self.kit); self.assertIn("[int]$PressesPerArm = 120,", self.kit)
        self.assertIn("$shortSessionRounds = 2\n", self.kit); self.assertIn("$shortSessionPresses = 12\n", self.kit)
        self.assertIn("$presentationArms = @('p-window', 'p-fullscreen', 'p-lowqueue-off', 'p-test-uncovered')", self.kit)
        self.assertIn("$audioArms = @('s-base', 's-audio-smooth')", self.kit)
    @staticmethod
    def latency(arms, rounds, presses_per_arm, trials, by_hand):
        sys.path.insert(0, HERE); import report
        tests = max(1, math.ceil(presses_per_arm / (rounds * trials)))
        per_start = report.plan_seconds(report.latency_plan(trials, tests)) + (report.MANUAL_SECONDS if by_hand else 0.0)
        return report.about_minutes(per_start * arms * rounds)
    def test_the_estimate_matches_the_measured_runs(self):
        """The 12 runs of 20 Nov (/home/claude/evidence/p7-a and p7-b run.json start and end): 52.6 to 74.7 s, median 54.1 s."""
        sys.path.insert(0, HERE); import report
        measured = [59.0, 53.5, 53.3, 60.6, 74.7, 54.6, 61.2, 53.4, 53.1, 66.5, 53.6, 52.6]
        estimate = report.plan_seconds("settle:8;latency:12;exit")
        self.assertEqual(estimate, 61.0)
        self.assertLessEqual(min(measured), estimate); self.assertLessEqual(estimate, max(measured))
        self.assertEqual((report.about_minutes(estimate * 8), report.about_minutes(estimate * 4)), (8, 4))       # the two sessions took 8 and 4 minutes
        self.assertEqual(report.plan_seconds("settle:8;probe:20;wait:40;exit"), 83.0)
        self.assertEqual(report.plan_seconds("settle:8;sr:15;fg:15;quality;exit"), 83.0)
    @staticmethod
    def network(arms, rounds=2, minutes=3): return arms * rounds * (minutes + 2)
    def test_printed_durations_are_computed_ones(self):
        first = self.latency(3, 3, 120, 12, True)           # mixed builds: 12 presses per test, started by hand
        presentation, audio = self.latency(4, 2, 24, 12, False), self.latency(2, 2, 24, 12, False)
        full = first + (10 + 3) + 15 + 5                    # soak 10 min (+3), reliability 15, breaks 5
        versus, six = self.latency(10, 3, 120, 12, True), self.latency(6, 3, 120, 12, True)
        self.assertEqual((first, presentation, audio, full, versus, six), (35, 8, 4, 68, 118, 71))
        self.assertEqual((self.network(4), self.network(2, rounds=6), self.network(2)), (40, 60, 20))
        self.assertIn(f"3. MEASURE                                          about {first} minutes", self.start)
        self.assertIn(f"Run-First-Test.cmd -Session presentation      about {presentation} minutes", self.start)
        self.assertIn(f"Run-First-Test.cmd -Session audio      about {audio} minutes", self.start)
        self.assertIn(f"(about {full} minutes)", self.start); self.assertIn(f"(about {self.network(4)} minutes)", self.start)
        self.assertIn(f"(about {self.network(2, rounds=6)} minutes)", self.start)
        self.assertLess(self.start.index("-Session presentation"), self.start.index("-Session network"))
        for text in (f"| 1 | Check and main comparison (3 arms) | {first} min |", f"## 1. Check and main comparison ({first} min)", f"**about {presentation} minutes**",
                     f"**about {audio} minutes**", f"(about {full} minutes)", f"about {versus} minutes", f"(about {six} min)", f"about {self.network(4)} minutes.**",
                     f"about {self.network(2, rounds=6)} minutes)", f"about {self.network(2)} minutes more", "6 runs per arm"):
            self.assertIn(text, self.proc)
        for text in (f"about {first} minutes with you at the PC", f"about {full} minutes;", f"(about {self.network(4)} minutes)", f"about {presentation} minutes.", f"about {audio} minutes."):
            self.assertIn(text, self.first)
        self.assertNotIn("5 minutes each", self.first)
        self.assertNotIn("four presentation arms", self.proc)

class PresentationSession(unittest.TestCase):
    """-Session presentation / audio reach the kit from the double-click entry point; PresentMon never asks for elevation there."""
    def test_wiring(self):
        with open(os.path.join(KIT, "run-tests.ps1"), encoding="ascii") as f: kit = f.read()
        with open(os.path.join(KIT, "run-first-test.ps1"), encoding="ascii") as f: first = f.read()
        self.assertIn("'presentation', 'audio', 'input', 'visual', 'neural', 'framegen', 'audiofix', 'pacing', 'builds')][string]$Session = 'first'", first)
        self.assertIn("elseif ($kitFiveSessions -contains $Session) { $needed = @('experimental') }", first)
        self.assertIn("$kitFiveSessions = @('input', 'visual', 'neural', 'framegen', 'audiofix', 'pacing', 'builds')", first)
        self.assertIn("$measureSessions = @('input', 'visual', 'neural', 'framegen', 'audiofix', 'pacing', 'builds')", kit)
        sys.path.insert(0, HERE); import report as report5
        self.assertEqual(list(report5.MEASURE_SESSIONS), ['input', 'visual', 'neural', 'framegen', 'audiofix', 'pacing', 'builds'])
        self.assertIn("if (@('presentation', 'audio') -contains $Session) { $needed = @('experimental') }", first)
        self.assertIn("'PresentMon not used: this session then cannot tell whether Windows composes the video window; the app''s own lines are still collected.'", kit)
        self.assertIn("} elseif ($PresentMonPrompt -and ($Session -ne 'presentation')) {", kit)
        self.assertIn("if ($Session -and (@('switches', 'presentation', 'audio') -notcontains $Session)) { $SkipLatency = $true }", kit)
        self.assertIn("Add-ShortSessionSection $out $Folder $kept $route", kit)
        sys.path.insert(0, HERE); import report
        table = report.read_arm_table(os.path.join(KIT, "arms.csv"))
        for name in report.PRESENTATION_ARMS:
            self.assertIn("VEYRA_EXP_PRESENT_WINDOW_FACTS=1", table[name]["switches"]); self.assertEqual(table[name]["exe"], "experimental")
        self.assertIn("VEYRA_EXP_RP_AUDIO_SMOOTH=1", table["s-audio-smooth"]["switches"]); self.assertEqual(table["s-base"]["switches"], "")
        # the app's format strings carry every key the section reads
        with open(os.path.join(ROOT, "include", "veyra", "engine", "PushPresentTiming.h"), encoding="utf-8") as f: timing = f.read()
        for key in ("presentToScanoutP50=%.3f", "presentToScanoutP95=%.3f", "scanoutUnder1Refresh=%u", "scanout1To2Refresh=%u", "scanoutOver2Refresh=%u",
                    "\"window event=%s", "coversMonitor=%d", "regionKind=%d", "ownOverlaps=%u", "flipBlockers=0x%X"):
            self.assertIn(key, timing)
        self.assertIn("$windowFactKeys = @('" + "', '".join(report.WINDOW_FACT_KEYS) + "')", kit)

if __name__ == "__main__":
    unittest.main(verbosity=1)
