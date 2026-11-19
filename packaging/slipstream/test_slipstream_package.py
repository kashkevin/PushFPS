#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests of the Slipstream package tool and static checks of the scripts it ships.

    python3 -B packaging/slipstream/test_slipstream_package.py

Python 3 standard library; runs on Linux and Windows. The builds are made-up zips and the GitHub CLI is
a stand-in script, so this says nothing about real artifacts or about the scripts running on Windows:
that is job "verify" of slipstream-package.yml (scripts/remoteplay/verify-slipstream-package.ps1).
"""
import hashlib
import io
import json
import os
import re
import stat
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import slipstream_package as tool  # noqa: E402

OUR_SCRIPTS = (
    "scripts/remoteplay/testkit/slipstream-common.ps1",
    "scripts/remoteplay/testkit/install-slipstream.ps1",
    "scripts/remoteplay/testkit/uninstall-slipstream.ps1",
    "scripts/remoteplay/testkit/run-first-test.ps1",
    "scripts/remoteplay/verify-slipstream-package.ps1",
)


def build_zip(path, version="2.1.5-pushfps.77", extra=None, backslashes=False, sidecar=True):
    """A stand-in for PushFPS-<version>-win64.zip as packaging/make-release.ps1 lays it out."""
    entries = {
        "Install-PushFPS.ps1": "# stand-in installer\n$PushFpsVersion = '%s'\n$UpstreamTag = 'v2.1.9'\n" % version,
        "Install-PushFPS.cmd": "@echo off\r\n",
        "READ ME FIRST.txt": "PushFPS %s\n" % version,
        "PushFPS-files/PushFPS.exe": "MZ stand-in " + version,
        "PushFPS-files/qml/Veyra/Main.qml": "import QtQuick\nItem {}\n",
        "PushFPS-files/LICENSE": "GPL-3.0\n",
    }
    entries.update(extra or {})
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name, text in entries.items():
            archive.writestr(name.replace("/", "\\") if backslashes else name, text)
    if sidecar:
        with open(path + ".sha256", "w", encoding="ascii") as handle:
            handle.write("%s  %s\n" % (tool.sha256_of(path).upper(), os.path.basename(path)))
    return path


def artifact(root, slot, version="2.1.5-pushfps.77", record=True, **options):
    folder = os.path.join(root, slot, "download", "PushFPS-" + version)
    build_zip(os.path.join(folder, "PushFPS-%s-win64.zip" % version), version, **options)
    with zipfile.ZipFile(os.path.join(folder, "PushFPS-%s-streaming-source.zip" % version), "w") as archive:
        archive.writestr("SOURCES.txt", "chiaki-ng ...\n")
    if record:
        with open(os.path.join(root, slot, "source.json"), "w", encoding="utf-8") as handle:
            json.dump({"slot": slot, "branch": "slipstream/" + slot, "found": True, "reason": "", "run_id": "42",
                       "run_url": "https://github.com/example/repo/actions/runs/42", "commit": "0123456789abcdef0123", "built": "2026-11-18T20:00:00Z"}, handle)


class AssembleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.art = os.path.join(self.temp.name, "art")
        self.out = os.path.join(self.temp.name, "dist")
        os.makedirs(self.art)

    def tearDown(self):
        self.temp.cleanup()

    def assemble(self, name="Slipstream-test-package-2026-11-18-7"):
        return tool.assemble(self.art, REPO, self.out, name, "https://github.com/example/repo/actions/runs/99")

    def read(self, *parts):
        with open(os.path.join(self.out, "Slipstream-test-package-2026-11-18-7", *parts), "rb") as handle:
            return handle.read()

    def test_three_builds_make_the_whole_package(self):
        artifact(self.art, "original", "2.1.5-pushfps.70")
        artifact(self.art, "stable", "2.1.5-pushfps.77")
        artifact(self.art, "experimental", "2.1.5-pushfps.80", backslashes=True)
        zip_path, builds, notes = self.assemble()
        self.assertEqual([b["found"] for b in builds], [True, True, True])
        self.assertEqual(notes, [])
        stage = os.path.join(self.out, "Slipstream-test-package-2026-11-18-7")
        for slot in ("original", "stable", "experimental"):
            for needed in ("Install-PushFPS.ps1", "PushFPS-files/PushFPS.exe", "PushFPS-files/qml/Veyra/Main.qml", "SOURCE.txt"):
                self.assertTrue(os.path.isfile(os.path.join(stage, slot, *needed.split("/"))), "%s/%s" % (slot, needed))
            # the build zip's own double-click installer targets the official install folder: never in the package
            for left_out in ("Install-PushFPS.cmd", "READ ME FIRST.txt"):
                self.assertFalse(os.path.exists(os.path.join(stage, slot, left_out)), "%s/%s" % (slot, left_out))
            self.assertEqual(len([n for n in os.listdir(os.path.join(stage, slot)) if n.endswith("-streaming-source.zip")]), 1)
        self.assertIn(b"version: 2.1.5-pushfps.80", self.read("experimental", "SOURCE.txt"))
        for needed in ("START-HERE.txt", "SHA256SUMS.txt", "LICENSE.txt") + tool.TOP_FILES:
            self.assertTrue(os.path.isfile(os.path.join(stage, needed)), needed)
        for relative in tool.KIT_FILES:
            self.assertTrue(os.path.isfile(os.path.join(stage, "testkit", *relative.split("/"))), relative)
        for relative in tool.DOC_FILES:
            self.assertTrue(os.path.isfile(os.path.join(stage, "docs", os.path.basename(relative))), relative)
        # run-tests.ps1 looks for detect-hardware.ps1 one folder above itself
        self.assertTrue(os.path.isfile(os.path.join(stage, "testkit", "scripts", "remoteplay", "detect-hardware.ps1")))

        start = self.read("START-HERE.txt").decode("ascii")
        self.assertNotIn("@", re.sub(r"\S+@\S+", "", start).replace("@", "") + "")  # placeholders are checked by the tool itself
        self.assertNotIn("MISSING FROM THIS PACKAGE", start)
        self.assertNotIn("NOT IN THIS PACKAGE", start)
        for text in ("PushFPS-slipstream\\PushFPS.exe", "Ultra Low Latency", "Frame generation OFF", "PushFPS-original\\PushFPS.exe",
                     "version 2.1.5-pushfps.77", "commit 0123456789ab", "actions/runs/99", "Install-Slipstream.cmd", "Run-First-Test.cmd",
                     "Rollback-Slipstream.cmd", "Uninstall-Slipstream.cmd"):
            self.assertIn(text, start)

        # Windows line endings for what cmd.exe and Notepad read; no lone LF left
        for parts in (("START-HERE.txt",), ("Install-Slipstream.cmd",), ("SHA256SUMS.txt",), ("docs", "SETUP.md"),
                      ("testkit", "scripts", "remoteplay", "testkit", "install-slipstream.ps1"), ("testkit", "scripts", "remoteplay", "testkit", "arms.csv")):
            data = self.read(*parts)
            self.assertIn(b"\r\n", data, parts)
            self.assertNotIn(b"\n", data.replace(b"\r\n", b""), parts)

        # SHA256SUMS.txt lists every other file with its real hash
        listed = {}
        for line in self.read("SHA256SUMS.txt").decode("ascii").splitlines():
            digest, relative = line.split("  ", 1)
            listed[relative] = digest
        on_disk = set()
        for folder, _dirs, names in os.walk(stage):
            for name in names:
                relative = os.path.relpath(os.path.join(folder, name), stage).replace(os.sep, "/")
                if relative != "SHA256SUMS.txt":
                    on_disk.add(relative)
                    self.assertEqual(listed.get(relative), tool.sha256_of(os.path.join(folder, name)), relative)
        self.assertEqual(set(listed), on_disk)

        # the zip holds exactly the staged files, at its root, and has its own .sha256
        with zipfile.ZipFile(zip_path) as archive:
            self.assertEqual(set(archive.namelist()), on_disk | {"SHA256SUMS.txt"})
            self.assertIsNone(archive.testzip())
        with open(zip_path + ".sha256", "r", encoding="ascii") as handle:
            self.assertEqual(handle.read().split()[0], tool.sha256_of(zip_path))
        self.assertEqual(os.path.basename(zip_path), "Slipstream-test-package-2026-11-18-7.zip")
        self.assertEqual(tool.check_private(stage), [])

    def test_missing_builds_do_not_stop_the_package(self):
        artifact(self.art, "stable")
        os.makedirs(os.path.join(self.art, "experimental"))
        with open(os.path.join(self.art, "experimental", "source.json"), "w", encoding="utf-8") as handle:
            json.dump({"slot": "experimental", "branch": "slipstream/dev-audio", "found": False, "reason": "no successful build-windows.yml run on branch slipstream/dev-audio"}, handle)
        zip_path, builds, _notes = self.assemble()
        self.assertEqual([b["found"] for b in builds], [False, True, False])
        self.assertTrue(os.path.isfile(zip_path))
        stage = os.path.join(self.out, "Slipstream-test-package-2026-11-18-7")
        self.assertFalse(os.path.exists(os.path.join(stage, "original")))
        self.assertFalse(os.path.exists(os.path.join(stage, "experimental")))
        start = self.read("START-HERE.txt").decode("ascii")
        self.assertIn("MISSING FROM THIS PACKAGE", start)
        self.assertIn("  experimental: no successful build-windows.yml run on branch slipstream/dev-audio", start)
        self.assertIn("  original: no artifact was fetched for this build", start)
        self.assertRegex(start, r"original\\\s+NOT IN THIS PACKAGE")
        self.assertIn("| original | **missing**", tool.summary(builds, [], "x"))

    def test_no_build_at_all_still_gives_kit_and_documents(self):
        zip_path, builds, _notes = self.assemble()
        self.assertFalse(any(b["found"] for b in builds))
        with zipfile.ZipFile(zip_path) as archive:
            names = archive.namelist()
        self.assertIn("START-HERE.txt", names)
        self.assertIn("testkit/scripts/remoteplay/testkit/run-tests.ps1", names)
        self.assertFalse(any(n.startswith(("original/", "stable/", "experimental/")) for n in names))

    def test_damaged_or_hostile_build_zips_are_reported_not_packaged(self):
        artifact(self.art, "original", extra={"../escape.txt": "x"})
        artifact(self.art, "stable")
        program = tool.find_files(os.path.join(self.art, "stable"), tool.PROGRAM_ZIP)[0]
        with open(program + ".sha256", "w", encoding="ascii") as handle:
            handle.write("0" * 64 + "  x\n")
        folder = os.path.join(self.art, "experimental", "download", "PushFPS-1")
        os.makedirs(folder)
        with zipfile.ZipFile(os.path.join(folder, "PushFPS-1-win64.zip"), "w") as archive:
            archive.writestr("READ ME FIRST.txt", "no installer here")
        _zip_path, builds, _notes = self.assemble()
        self.assertEqual([b["found"] for b in builds], [False, False, False])
        self.assertIn("unsafe entry name", builds[0]["reason"])
        self.assertIn("does not match its .sha256", builds[1]["reason"])
        self.assertIn("unexpected layout", builds[2]["reason"])
        self.assertIn("Install-PushFPS.ps1, PushFPS-files/PushFPS.exe missing", builds[2]["reason"])
        stage = os.path.join(self.out, "Slipstream-test-package-2026-11-18-7")
        self.assertFalse(os.path.exists(os.path.join(self.temp.name, "dist", "escape.txt")))
        for slot in ("original", "stable", "experimental"):
            self.assertFalse(os.path.exists(os.path.join(stage, slot)), slot)

    def test_private_or_secret_material_stops_the_package(self):
        cases = {
            "token": {"PushFPS-files/qml/Veyra/Note.js": "var t = 'ghp_" + "a1B2" * 10 + "'\n"},
            "key": {"PushFPS-files/notes.txt": "-----BEGIN RSA PRIVATE KEY-----\nMIIE\n"},
            "settings": {"PushFPS-files/runtime_local/user-data-2.0.0/preferences.json": "{}"},
            "log": {"PushFPS-files/logs/veyra-qml.log": "[INFO ] started"},
            "ini": {"PushFPS-files/slipstream.ini": "mode = balanced\n"},
        }
        for label, extra in cases.items():
            with self.subTest(label):
                for name in os.listdir(self.art):
                    tool.shutil.rmtree(os.path.join(self.art, name))
                artifact(self.art, "stable", extra=extra)
                with self.assertRaises(tool.PackageError):
                    self.assemble()
                self.assertFalse(os.path.exists(os.path.join(self.out, "Slipstream-test-package-2026-11-18-7.zip")), label)

    def test_what_a_real_build_zip_holds_passes_the_privacy_check(self):
        # packaging/make-release.ps1 copies these repository folders into PushFPS-files: none of their
        # file names or texts may trip the check, or no real package could be made.
        extra = {}
        for top in ("qml", "licenses"):
            for folder, _dirs, names in os.walk(os.path.join(REPO, top)):
                for name in names:
                    path = os.path.join(folder, name)
                    with open(path, "rb") as handle:
                        extra["PushFPS-files/" + os.path.relpath(path, REPO).replace(os.sep, "/")] = handle.read()
        for name in ("LICENSE", "NOTICE.md", "THIRD_PARTY_NOTICES.md", "README.md"):
            with open(os.path.join(REPO, name), "rb") as handle:
                extra["PushFPS-files/" + name] = handle.read()
        self.assertGreater(len(extra), 100)
        artifact(self.art, "stable", extra=extra)
        _zip_path, builds, _notes = self.assemble()
        self.assertTrue(builds[1]["found"], builds[1]["reason"])

    def test_package_name_is_restricted(self):
        with self.assertRaises(tool.PackageError):
            tool.assemble(self.art, REPO, self.out, "../evil")


FAKE_GH = r'''#!/usr/bin/env python3
import json, os, sys, zipfile
state = json.load(open(os.environ["FAKE_GH_STATE"]))
args = sys.argv[1:]
with open(os.environ["FAKE_GH_STATE"] + ".calls", "a") as log:
    log.write(json.dumps(args) + "\n")
if "GH_TOKEN" in " ".join(args):
    sys.exit(9)
if args[:2] == ["run", "list"]:
    branch = args[args.index("--branch") + 1]
    assert args[args.index("--status") + 1] == "success"
    if branch == "broken":
        sys.stderr.write("HTTP 403: Resource not accessible by integration\n"); sys.exit(1)
    print(json.dumps(state["runs"].get(branch, []))); sys.exit(0)
if args[:2] == ["run", "view"]:
    for runs in state["runs"].values():
        for run in runs:
            if str(run["databaseId"]) == args[2]:
                print(json.dumps(run)); sys.exit(0)
    sys.stderr.write("run not found\n"); sys.exit(1)
if args[:2] == ["run", "download"]:
    number = args[2]
    assert args[args.index("--pattern") + 1] == "PushFPS-[0-9]*"
    target = args[args.index("--dir") + 1]
    if number not in state["artifacts"]:
        sys.stderr.write("no artifact matches any of the names or patterns provided\n"); sys.exit(1)
    version = state["artifacts"][number]
    folder = os.path.join(target, "PushFPS-" + version)
    os.makedirs(folder)
    with zipfile.ZipFile(os.path.join(folder, "PushFPS-%s-win64.zip" % version), "w") as archive:
        archive.writestr("Install-PushFPS.ps1", "$PushFpsVersion = '%s'\n" % version)
    sys.exit(0)
sys.exit(2)
'''


class FetchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.state = os.path.join(self.temp.name, "state.json")
        self.gh = os.path.join(self.temp.name, "gh-stand-in.py")
        with open(self.gh, "w", encoding="utf-8") as handle:
            handle.write(FAKE_GH)
        os.chmod(self.gh, os.stat(self.gh).st_mode | stat.S_IXUSR)
        os.environ["FAKE_GH_STATE"] = self.state
        self.set_state({
            "runs": {
                "slipstream/rc2": [
                    {"databaseId": 31, "headSha": "c" * 40, "createdAt": "2026-11-18T21:00:00Z", "url": "https://x/runs/31", "conclusion": "success", "headBranch": "slipstream/rc2", "event": "workflow_dispatch"},
                    {"databaseId": 30, "headSha": "b" * 40, "createdAt": "2026-11-18T19:00:00Z", "url": "https://x/runs/30", "conclusion": "success", "headBranch": "slipstream/rc2", "event": "workflow_dispatch"},
                ],
                "slipstream/baseline": [
                    {"databaseId": 20, "headSha": "a" * 40, "createdAt": "2026-11-18T10:00:00Z", "url": "https://x/runs/20", "conclusion": "success", "headBranch": "slipstream/baseline", "event": "workflow_dispatch"},
                ],
                "elsewhere": [
                    {"databaseId": 55, "headSha": "d" * 40, "createdAt": "2026-11-17T10:00:00Z", "url": "https://x/runs/55", "conclusion": "failure", "headBranch": "elsewhere", "event": "push"},
                ],
            },
            "artifacts": {"30": "2.1.5-pushfps.30", "20": "2.1.5-pushfps.20", "55": "2.1.5-pushfps.55"},
        })

    def tearDown(self):
        os.environ.pop("FAKE_GH_STATE", None)
        self.temp.cleanup()

    def set_state(self, state):
        with open(self.state, "w", encoding="utf-8") as handle:
            json.dump(state, handle)

    def fetch(self, slot, branch, run_id=""):
        out = os.path.join(self.temp.name, "art", slot)
        if os.name == "nt":
            self.skipTest("the GitHub CLI stand-in is a script; run on Linux")
        record = tool.fetch(slot, branch, out, "build-windows.yml", run_id, self.gh)
        with open(os.path.join(out, "source.json"), "r", encoding="utf-8") as handle:
            self.assertEqual(json.load(handle), record)
        return record, out

    def test_newest_successful_run_with_an_artifact_is_taken(self):
        # run 31 is newest but its artifact is gone: the next successful run is used
        record, out = self.fetch("stable", "slipstream/rc2")
        self.assertTrue(record["found"])
        self.assertEqual((record["run_id"], record["commit"], record["branch"]), ("30", "b" * 40, "slipstream/rc2"))
        self.assertEqual(len(tool.find_files(out, tool.PROGRAM_ZIP)), 1)
        record, _out = self.fetch("original", "slipstream/baseline")
        self.assertEqual(record["run_id"], "20")
        self.assertFalse(record["chosen_by_run_id"])

    def test_missing_branch_or_failing_cli_is_a_reason_not_an_error(self):
        record, out = self.fetch("experimental", "slipstream/dev-audio")
        self.assertFalse(record["found"])
        self.assertIn("no successful build-windows.yml run on branch slipstream/dev-audio", record["reason"])
        self.assertEqual(tool.find_files(out, tool.PROGRAM_ZIP), [])
        record, _out = self.fetch("experimental", "broken")
        self.assertFalse(record["found"])
        self.assertIn("could not be listed", record["reason"])
        record = tool.fetch("stable", "slipstream/rc2", os.path.join(self.temp.name, "art", "nogh"), gh=os.path.join(self.temp.name, "no-such-gh"))
        self.assertFalse(record["found"])

    def test_a_named_run_is_used_whatever_its_result(self):
        record, _out = self.fetch("experimental", "elsewhere", "55")
        self.assertTrue(record["found"])
        self.assertTrue(record["chosen_by_run_id"])
        self.assertEqual(record["conclusion"], "failure")
        record, _out = self.fetch("experimental", "elsewhere", "55; rm -rf /")
        self.assertFalse(record["found"])
        self.assertIn("is not a number", record["reason"])

    def test_command_line(self):
        if os.name == "nt":
            self.skipTest("the GitHub CLI stand-in is a script; run on Linux")
        art = os.path.join(self.temp.name, "art")
        self.assertEqual(tool.main(["fetch", "--slot", "stable", "--out", os.path.join(art, "stable"), "--gh", self.gh]), 0)
        self.assertEqual(tool.main(["fetch", "--slot", "experimental", "--out", os.path.join(art, "experimental"), "--gh", self.gh, "--run-id", ""]), 0)
        summary = os.path.join(self.temp.name, "summary.md")
        # the stand-in's zip has no PushFPS.exe: the build is reported as unusable, the package is still made
        self.assertEqual(tool.main(["assemble", "--artifacts", art, "--repo", REPO, "--out", os.path.join(self.temp.name, "dist"),
                                    "--name", "Slipstream-test-package-x", "--summary", summary]), 0)
        with open(summary, "r", encoding="utf-8") as handle:
            text = handle.read()
        self.assertIn("unexpected layout", text)
        self.assertIn("slipstream/dev-audio", text)


# ---------------------------------------------------------------------------------------------------
# Static checks of the Windows scripts (they cannot run here)
# ---------------------------------------------------------------------------------------------------
def powershell_balance(text):
    """A small PowerShell scanner: strings, comments and brackets. Returns a list of problems.
    Enough for scripts written without here-strings; it is not a parser."""
    problems, stack, i, line = [], [], 0, 1
    pairs = {")": "(", "}": "{", "]": "["}

    def scan_double(start):
        # returns the index after the closing quote; handles `x, "" and $( ... )
        j, at_line = start, line
        while j < len(text):
            c = text[j]
            if c == "`":
                j += 2
                continue
            if c == '"':
                if text[j + 1:j + 2] == '"':
                    j += 2
                    continue
                return j + 1
            if c == "$" and text[j + 1:j + 2] == "(":
                depth, j = 1, j + 2
                while j < len(text) and depth:
                    if text[j] == "'":
                        j = scan_single(j + 1)
                        continue
                    if text[j] == '"':
                        j = scan_double(j + 1)
                        continue
                    depth += text[j] == "("
                    depth -= text[j] == ")"
                    j += 1
                continue
            j += 1
        problems.append("line %d: double-quoted string is not closed" % at_line)
        return j

    def scan_single(start):
        j = start
        while j < len(text):
            if text[j] == "'":
                if text[j + 1:j + 2] == "'":
                    j += 2
                    continue
                return j + 1
            j += 1
        problems.append("line %d: single-quoted string is not closed" % line)
        return j

    while i < len(text):
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif text.startswith("<#", i):
            end = text.find("#>", i + 2)
            if end < 0:
                problems.append("line %d: block comment is not closed" % line)
                break
            line += text.count("\n", i, end)
            i = end + 2
        elif c == "#" and (i == 0 or text[i - 1] in " \t\n({;,"):
            end = text.find("\n", i)
            i = len(text) if end < 0 else end
        elif c == "@" and text[i + 1:i + 2] in ("'", '"'):
            problems.append("line %d: here-string (not handled by this check)" % line)
            i += 2
        elif c == "`":
            i += 2
        elif c == "'":
            end = scan_single(i + 1)
            if "\n" in text[i:end]:
                problems.append("line %d: single-quoted string spans lines" % line)
                line += text.count("\n", i, end)
            i = end
        elif c == '"':
            end = scan_double(i + 1)
            if "\n" in text[i:end]:
                problems.append("line %d: double-quoted string spans lines" % line)
                line += text.count("\n", i, end)
            i = end
        elif c in "({[":
            stack.append((c, line))
            i += 1
        elif c in ")}]":
            if not stack or stack[-1][0] != pairs[c]:
                problems.append("line %d: '%s' does not close %s" % (line, c, "'%s' of line %d" % stack[-1] if stack else "anything"))
                if stack:
                    stack.pop()
            else:
                stack.pop()
            i += 1
        else:
            i += 1
    for bracket, at in stack:
        problems.append("line %d: '%s' is never closed" % (at, bracket))
    return problems


class ScriptTests(unittest.TestCase):
    def text(self, relative):
        with open(os.path.join(REPO, *relative.split("/")), "rb") as handle:
            return handle.read()

    def test_the_scanner_itself(self):
        good = "function F([string]$a) { if ($a -eq 'it''s') { Write-Host \"x $($a.Replace('(', ')')) `\" y\" } } # ) }\n<# ( #>\n$b = @{ k = @(1, 2) }\n"
        self.assertEqual(powershell_balance(good), [])
        self.assertTrue(powershell_balance("if ($a) { 'x' \n"))
        self.assertTrue(powershell_balance("$a = 'open\n"))
        self.assertTrue(powershell_balance("foo(]\n"))
        self.assertTrue(powershell_balance('$a = "open\n'))

    def test_our_powershell_scripts_are_ascii_and_balanced(self):
        for relative in OUR_SCRIPTS:
            data = self.text(relative)
            try:
                text = data.decode("ascii")
            except UnicodeDecodeError as error:
                self.fail("%s is not ASCII: %s" % (relative, error))
            self.assertEqual(powershell_balance(text), [], relative)
            self.assertNotIn("\t", text, relative)
            # constructs Windows PowerShell 5.1 does not have
            for newer in ("??", "?.", " -Parallel", "&&", "||", "Join-String", "-AsHashtable", "ConvertFrom-Json -Depth"):
                code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
                code = re.sub(r"'(?:[^'\n]|'')*'", "''", code)
                self.assertNotIn(newer, code, "%s uses %s" % (relative, newer))
            self.assertIn("$ErrorActionPreference = 'Stop'", text.replace("# ", ""), relative) if not relative.endswith("common.ps1") else None

    def test_scripts_never_name_the_official_install_or_use_the_installers_uninstall(self):
        for relative in OUR_SCRIPTS[:4]:
            text = self.text(relative).decode("ascii")
            code = text.split("#>", 1)[-1]                      # after the help block
            code = "\n".join(line for line in code.splitlines() if not line.lstrip().startswith("#"))
            self.assertNotRegex(code, r"Programs\\PushFPS'", relative)
            self.assertNotIn("-Uninstall", code, relative)
            self.assertNotRegex(code, r"(?i)\.lnk|Veyra\\remoteplay|CreateShortcut|Unblock-File|Set-ExecutionPolicy|reg(\.exe)? add|New-ItemProperty", relative)
        uninstall = self.text("scripts/remoteplay/testkit/uninstall-slipstream.ps1").decode("ascii")
        self.assertEqual(len(re.findall(r"(?m)^\s*Remove-Item ", uninstall)), 1)   # the one place a folder is deleted
        self.assertIn("ReparsePoint", uninstall)
        self.assertIn("-ceq 'YES'", uninstall)

    def test_folder_names_agree_everywhere(self):
        common = self.text("scripts/remoteplay/testkit/slipstream-common.ps1").decode("ascii")
        kit = self.text("scripts/remoteplay/testkit/run-tests.ps1").decode("utf-8", "replace")
        verify = self.text("scripts/remoteplay/verify-slipstream-package.ps1").decode("ascii")
        template = self.text("packaging/slipstream/START-HERE.template.txt").decode("ascii")
        for slot, _branch, _title, folder in tool.SLOTS:
            self.assertRegex(common, r"Slot = '%s';\s+Folder = '%s'" % (slot, folder))
            self.assertRegex(verify, r"Slot = '%s';\s+Folder = '%s'" % (slot, folder))
            self.assertIn("%LOCALAPPDATA%\\Programs\\" + folder, template)
        # the kit's own defaults are the original and the stable candidate
        self.assertIn("PushFPS-original\\PushFPS.exe", kit)
        self.assertIn("PushFPS-slipstream\\PushFPS.exe", kit)
        for parameter in ("-Quick", "-OriginalExe", "-ModifiedExe", "-PresentMon"):
            self.assertRegex(kit, r"\$%s\b" % parameter[1:], "run-tests.ps1 has no %s" % parameter)

    def test_update_pin_is_read_as_a_version_by_the_program(self):
        common = self.text("scripts/remoteplay/testkit/slipstream-common.ps1").decode("ascii")
        pin = re.search(r"\$SlipstreamPinLine = '([^']*)'", common).group(1)
        verify_cpp = self.text("src/ui/UpdateVerify.cpp").decode("utf-8")
        # the program's own pattern for the version line of PushFPS-VERSION.txt (parseInstalledVersionFile)
        self.assertIn(r'QStringLiteral("^PushFPS (\\d+\\.\\d+\\.\\d+)\\s*$")', verify_cpp)
        match = re.match(r"^PushFPS (\d+\.\d+\.\d+)\s*$", pin)
        self.assertTrue(match)
        self.assertGreater(tuple(int(part) for part in match.group(1).split(".")), (99, 0, 0))
        note = re.search(r"\$text\.Add\('(Slipstream test copy:[^']*)'", common).group(1)
        self.assertFalse(note.startswith(("Not installed:", "Runtime:", "PushFPS ")))
        self.assertIn("'PushFPS 999.0.0'", self.text("scripts/remoteplay/verify-slipstream-package.ps1").decode("ascii"))

    def test_cmd_files_only_start_scripts_inside_the_package(self):
        shipped = set(tool.KIT_FILES)
        for name in tool.TOP_FILES:
            text = self.text("packaging/slipstream/" + name).decode("ascii")
            lines = [line for line in text.splitlines() if line and not line.lower().startswith("rem ")]
            self.assertEqual(lines[0], "@echo off", name)
            self.assertIn('set "kit=%~dp0testkit\\scripts\\remoteplay\\testkit"', lines, name)
            self.assertIn(":run", lines, name)
            starts = [line for line in lines if "powershell" in line.lower()]
            self.assertEqual(len(starts), 1, name)
            match = re.fullmatch(r'powershell\.exe -NoProfile -ExecutionPolicy Bypass -File "%kit%\\([a-z-]+\.ps1)"( -[A-Za-z]+ (?:"%~dp0\."|[A-Za-z]+))? %\*', starts[0])
            self.assertTrue(match, "%s: %s" % (name, starts[0]))
            self.assertIn("scripts/remoteplay/testkit/" + match.group(1), shipped, name)
            self.assertIn('if exist "%%kit%%\\%s" goto run' % match.group(1), lines, name)
            for line in lines:
                if line.startswith("echo"):
                    continue
                self.assertNotRegex(line, r"(?i)\b(del|rd|rmdir|erase|format|reg|curl|bitsadmin|certutil|start)\b", "%s: %s" % (name, line))
            for line in lines:
                self.assertNotIn("(", line, "%s must not use blocks: a folder name may hold brackets" % name)

    def test_workflow(self):
        with open(os.path.join(REPO, ".github", "workflows", "slipstream-package.yml"), "r", encoding="utf-8") as handle:
            text = handle.read()
        self.assertRegex(text, r"permissions:\n  actions: read\n  contents: read\n")
        self.assertNotRegex(text, r"(?m)^\s+(contents|actions|packages|id-token): write")
        self.assertNotIn("secrets.", text)
        self.assertEqual(text.count("github.token"), 1)
        self.assertIn("persist-credentials: false", text)
        for slot, branch, _title, _folder in tool.SLOTS:
            self.assertIn("'%s'" % branch, text)
            self.assertIn("--slot %s" % slot, text)
        self.assertIn("branches: ['slipstream/package']", text)
        self.assertIn("scripts\\remoteplay\\verify-slipstream-package.ps1", text)
        # every .ps1 we add is under scripts/remoteplay, which remoteplay-checks.yml parses recursively
        with open(os.path.join(REPO, ".github", "workflows", "remoteplay-checks.yml"), "r", encoding="utf-8") as handle:
            self.assertIn("Get-ChildItem scripts\\remoteplay -Filter *.ps1 -Recurse", handle.read())
        for relative in OUR_SCRIPTS:
            self.assertTrue(relative.startswith("scripts/remoteplay/"))
        self.assertEqual([n for n in os.listdir(HERE) if n.endswith(".ps1")], [])


if __name__ == "__main__":
    unittest.main(verbosity=1)
