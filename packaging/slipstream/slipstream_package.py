#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the Slipstream test package: one zip with the original build, the stable candidate and the
experimental build side by side, the measuring kit, the documents and the double-click files.

Used by .github/workflows/slipstream-package.yml; Python 3 standard library only, runs on Linux and Windows.

  slipstream_package.py fetch --slot stable --branch slipstream/rc2 --out art/stable
      Downloads the PushFPS artifact of the newest successful build-windows run of that branch with the
      GitHub CLI (GH_TOKEN and GH_REPO in the environment). A branch without such a run is not an error:
      the reason is written to art/stable/source.json and the package says what is missing.

  slipstream_package.py assemble --artifacts art --repo . --out dist --name Slipstream-test-package-...
      Unpacks each build that was fetched, adds testkit/, docs/, START-HERE.txt, the .cmd files and
      SHA256SUMS.txt, checks that nothing private got in, and writes <name>.zip and <name>.zip.sha256.

The package never contains credentials, console registration data, settings or logs: it is made only
from build artifacts (program files) and files of the repository named in KIT_FILES and DOC_FILES, and
check_private() refuses to write a package that holds anything else of that kind.
"""
import argparse
import datetime
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import zipfile

# slot (folder in the package), default branch, what it is, where Install-Slipstream puts it
SLOTS = (
    ("original", "slipstream/baseline", "original build (PushFPS 3.2.1 as published)", "PushFPS-original"),
    ("stable", "slipstream/rc2", "stable candidate", "PushFPS-slipstream"),
    ("experimental", "slipstream/dev2", "experimental build", "PushFPS-slipstream-exp"),
)
ARTIFACT_PATTERN = "PushFPS-[0-9]*"          # PushFPS-<version>; not PushFPS-OBS-Plugin
PROGRAM_ZIP = "PushFPS-*-win64.zip"
SOURCE_ZIP = "PushFPS-*-streaming-source.zip"
NEEDED_IN_BUILD = ("Install-PushFPS.ps1", "PushFPS-files/PushFPS.exe")
# Left out of every build folder: the build zip's own double-click installer and its note. Started from
# the package they would install a development build into %LOCALAPPDATA%\Programs\PushFPS, over an
# official PushFPS, and replace its shortcuts. Install-Slipstream.cmd runs Install-PushFPS.ps1 with
# the right folder instead.
LEFT_OUT_OF_BUILD = ("Install-PushFPS.cmd", "READ ME FIRST.txt")

# Repository files that go under testkit/ with their repository paths, so that every command in the
# documents works from <package>\testkit exactly as from the repository root, and run-tests.ps1 finds
# detect-hardware.ps1 one folder above itself.
KIT_FILES = (
    "scripts/remoteplay/testkit/run-tests.ps1",
    "scripts/remoteplay/testkit/arms.csv",
    "scripts/remoteplay/testkit/camera-template.csv",
    "scripts/remoteplay/testkit/sensor-template.csv",
    "scripts/remoteplay/testkit/slipstream-common.ps1",
    "scripts/remoteplay/testkit/install-slipstream.ps1",
    "scripts/remoteplay/testkit/uninstall-slipstream.ps1",
    "scripts/remoteplay/testkit/run-first-test.ps1",
    "scripts/remoteplay/detect-hardware.ps1",
    "scripts/remoteplay/latency-ab.ps1",
    "tools/remoteplay_latency_bench/report.py",
    "tools/remoteplay_latency_bench/analyze.py",
    "tools/remoteplay_latency_bench/test_report.py",
    "scripts/remoteplay/testkit/collect-diagnostics.ps1",
    "scripts/remoteplay/testkit/collect-diagnostics.cmd",
    "tools/remoteplay_latency_bench/baseline.py",
    "tools/remoteplay_latency_bench/reliability.py",
    "tools/remoteplay_latency_bench/soak.py",
)
KIT_REQUIRED = ("scripts/remoteplay/testkit/install-slipstream.ps1", "scripts/remoteplay/testkit/uninstall-slipstream.ps1",
                "scripts/remoteplay/testkit/slipstream-common.ps1", "scripts/remoteplay/testkit/run-first-test.ps1",
                "scripts/remoteplay/testkit/collect-diagnostics.ps1", "scripts/remoteplay/testkit/run-tests.ps1")
DOC_FILES = ("docs/SETUP.md", "docs/HARDWARE_TEST_PROCEDURE.md", "docs/KEYBOARD_MOUSE.md", "docs/PS5_REMOTE_PLAY_EXPERIMENTS.md",
             "docs/SLIPSTREAM_STATUS.md", "docs/HARDWARE_COMPATIBILITY.md", "docs/RELIABILITY.md", "docs/AUDIO.md")
TOP_FILES = ("Install-Slipstream.cmd", "Run-First-Test.cmd", "Run-Full-Test.cmd", "Collect-Diagnostics.cmd", "Rollback-Slipstream.cmd",
             "Uninstall-Slipstream.cmd")

# Text the package itself adds is written with Windows line endings (what cmd.exe labels need and what
# the Windows checks ran); the scripts must stay ASCII so that Windows PowerShell 5.1 reads them the
# same with any code page.
CRLF_TYPES = (".cmd", ".ps1", ".txt", ".csv", ".md")
ASCII_TYPES = (".cmd", ".ps1")

# Never in a package: user data, logs, settings, key material.
PRIVATE_NAMES = ("runtime_local", "user-data*", "logs", "*.log", "slipstream.ini", "slipstream.ini.*", "Slipstream-INSTALL.txt",
                 "PushFPS-VERSION.txt", "hardware.json", "evidence-*", "*.pem", "*.key", "*.pfx", "*.p12", ".env", "*.kdbx",
                 "id_rsa*", "id_ed25519*", ".git", ".github", "source.json")
TEXT_TYPES = (".txt", ".md", ".ps1", ".cmd", ".csv", ".json", ".py", ".qml", ".js", ".ini", ".cfg", ".yml", ".yaml", ".xml", ".conf")
SECRET_PATTERNS = (
    ("GitHub token", re.compile(rb"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,})")),
    ("private key", re.compile(rb"-----BEGIN [A-Z ]*PRIVATE KEY-----")),
    ("bearer token", re.compile(rb"(?i)authorization:\s*(?:bearer|token)\s+[A-Za-z0-9._-]{20,}")),
)


class PackageError(Exception):
    pass


def log(text):
    print(text, flush=True)


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def find_files(root, pattern):
    found = []
    for folder, _dirs, names in os.walk(root):
        for name in names:
            if fnmatch.fnmatch(name, pattern):
                found.append(os.path.join(folder, name))
    return sorted(found)


# ---------------------------------------------------------------------------------------------------
# fetch
# ---------------------------------------------------------------------------------------------------
def run_gh(arguments, gh="gh"):
    """(exit code, stdout, stderr). The token is only ever in the environment gh inherits."""
    try:
        done = subprocess.run([gh] + list(arguments), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, check=False)
    except OSError as error:
        return 127, "", str(error)
    return done.returncode, done.stdout, done.stderr


def fetch(slot, branch, out, workflow="build-windows.yml", run_id="", gh="gh", tries=5):
    """Downloads the build artifact for one slot. Returns the record also written to out/source.json."""
    os.makedirs(out, exist_ok=True)
    record = {"slot": slot, "branch": branch, "workflow": workflow, "found": False, "reason": ""}
    fields = "databaseId,headSha,createdAt,url,conclusion,headBranch,event"
    runs = []
    if run_id:
        if not re.fullmatch(r"[0-9]{1,20}", run_id):
            record["reason"] = "run id '%s' is not a number" % run_id
        else:
            code, text, error = run_gh(["run", "view", run_id, "--json", fields], gh)
            if code != 0:
                record["reason"] = "run %s could not be read: %s" % (run_id, last_line(error))
            else:
                runs = [json.loads(text)]
    else:
        code, text, error = run_gh(["run", "list", "--workflow", workflow, "--branch", branch, "--status", "success",
                                    "--limit", str(tries), "--json", fields], gh)
        if code != 0:
            record["reason"] = "the runs of branch %s could not be listed: %s" % (branch, last_line(error))
        else:
            runs = json.loads(text or "[]")
            if not runs:
                record["reason"] = "no successful %s run on branch %s" % (workflow, branch)
    for run in runs:
        number = str(run.get("databaseId", ""))
        target = os.path.join(out, "download")
        shutil.rmtree(target, ignore_errors=True)
        code, _text, error = run_gh(["run", "download", number, "--pattern", ARTIFACT_PATTERN, "--dir", target], gh)
        if code != 0 or not find_files(target, PROGRAM_ZIP):
            record["reason"] = "run %s has no PushFPS artifact to download (expired, or the build job did not finish): %s" % (
                number, last_line(error) or "no %s in it" % PROGRAM_ZIP)
            shutil.rmtree(target, ignore_errors=True)
            continue
        record.update({"found": True, "reason": "", "run_id": number, "run_url": run.get("url", ""), "commit": run.get("headSha", ""),
                       "built": run.get("createdAt", ""), "conclusion": run.get("conclusion", ""),
                       "run_branch": run.get("headBranch", ""), "chosen_by_run_id": bool(run_id)})
        break
    with open(os.path.join(out, "source.json"), "w", encoding="utf-8") as handle:
        json.dump(record, handle, indent=2)
    log("%-13s %s" % (slot, "run %s (%s)" % (record["run_id"], record.get("commit", "")[:8]) if record["found"] else "MISSING: " + record["reason"]))
    return record


def last_line(text):
    lines = [line.strip() for line in (text or "").splitlines() if line.strip()]
    return lines[-1][:300] if lines else ""


# ---------------------------------------------------------------------------------------------------
# assemble
# ---------------------------------------------------------------------------------------------------
def safe_extract(zip_path, target):
    """Unpacks a build zip. Entry names may use either slash (Compress-Archive of Windows PowerShell 5.1
    writes backslashes); absolute names, drive letters and '..' are refused."""
    count = 0
    root = os.path.realpath(target)
    with zipfile.ZipFile(zip_path) as archive:
        for info in archive.infolist():
            name = info.filename.replace("\\", "/")
            if name.endswith("/"):
                continue
            parts = name.split("/")
            if name.startswith("/") or ":" in name or any(part in ("", ".", "..") for part in parts):
                raise PackageError("unsafe entry name in %s: %r" % (os.path.basename(zip_path), info.filename))
            if (info.external_attr >> 16) & 0o170000 == 0o120000:
                raise PackageError("link entry in %s: %r" % (os.path.basename(zip_path), info.filename))
            destination = os.path.realpath(os.path.join(root, *parts))
            if not destination.startswith(root + os.sep):
                raise PackageError("entry leaves the folder in %s: %r" % (os.path.basename(zip_path), info.filename))
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            with archive.open(info) as source, open(destination, "wb") as output:
                shutil.copyfileobj(source, output, 1 << 20)
            count += 1
    return count


def sidecar_hash(zip_path):
    """The hash in <zip>.sha256 written by the build, or '' when there is none."""
    path = zip_path + ".sha256"
    if not os.path.isfile(path):
        return ""
    with open(path, "r", encoding="utf-8-sig", errors="replace") as handle:
        match = re.match(r"\s*([0-9a-fA-F]{64})\b", handle.read())
    return match.group(1).lower() if match else ""


def packaged_version(installer):
    with open(installer, "r", encoding="utf-8-sig", errors="replace") as handle:
        match = re.search(r"^\s*\$PushFpsVersion\s*=\s*'([^']*)'", handle.read(), re.MULTILINE)
    return match.group(1) if match else "unknown"


def add_build(slot, title, artifacts, stage):
    """Unpacks one build into stage/<slot>. Returns a dict with found/reason and what is known about it."""
    folder = os.path.join(artifacts, slot)
    info = {"slot": slot, "title": title, "found": False, "reason": "no artifact was fetched for this build"}
    record_path = os.path.join(folder, "source.json")
    if os.path.isfile(record_path):
        with open(record_path, "r", encoding="utf-8") as handle:
            info.update(json.load(handle))
        info["title"] = title
        if not info.get("found"):
            return info
    zips = find_files(folder, PROGRAM_ZIP) if os.path.isdir(folder) else []
    info["found"] = False
    if not zips:
        info["reason"] = info.get("reason") or "no %s in the artifact" % PROGRAM_ZIP
        return info
    if len(zips) > 1:
        info["reason"] = "the artifact holds %d program zips, expected one" % len(zips)
        return info
    program = zips[0]
    info["zip"] = os.path.basename(program)
    info["zip_sha256"] = sha256_of(program)
    expected = sidecar_hash(program)
    if expected and expected != info["zip_sha256"]:
        info["reason"] = "%s does not match its .sha256 file" % info["zip"]
        return info
    target = os.path.join(stage, slot)
    try:
        safe_extract(program, target)
    except (PackageError, zipfile.BadZipFile, OSError) as error:
        shutil.rmtree(target, ignore_errors=True)
        info["reason"] = "the program zip could not be unpacked: %s" % error
        return info
    missing = [name for name in NEEDED_IN_BUILD if not os.path.isfile(os.path.join(target, *name.split("/")))]
    if missing:
        shutil.rmtree(target, ignore_errors=True)
        info["reason"] = "unexpected layout of %s: %s missing" % (info["zip"], ", ".join(missing))
        return info
    info["version"] = packaged_version(os.path.join(target, "Install-PushFPS.ps1"))
    for name in LEFT_OUT_OF_BUILD:
        if os.path.isfile(os.path.join(target, name)):
            os.remove(os.path.join(target, name))
    # The corresponding source of the streaming libraries (AGPL/GPL/MPL) travels with the program.
    for source in find_files(folder, SOURCE_ZIP) + find_files(folder, SOURCE_ZIP + ".sha256"):
        shutil.copy2(source, os.path.join(target, os.path.basename(source)))
    lines = [
        "Slipstream test package: %s" % title,
        "version: %s" % info["version"],
        "branch: %s" % info.get("branch", "unknown"),
        "commit: %s" % info.get("commit", "unknown"),
        "built: %s" % info.get("built", "unknown"),
        "build run: %s" % info.get("run_url", "unknown"),
        "program zip: %s" % info["zip"],
        "program zip sha256: %s" % info["zip_sha256"],
        "Unpacked from the artifact of that build run. Left out: %s (they install into the folder of an" % ", ".join(LEFT_OUT_OF_BUILD),
        "official PushFPS; use Install-Slipstream.cmd of this package). Nothing else was changed.",
        "Licence: GNU GPL-3.0, no warranty. PushFPS is a modified version of Veyra by Likely7. Licence and notices:",
        "PushFPS-files (LICENSE, NOTICE.md, THIRD_PARTY_NOTICES.md). Source code: the commit above; the source of",
        "the streaming libraries is the streaming-source zip in this folder.",
    ]
    write_text(os.path.join(target, "SOURCE.txt"), "\n".join(lines) + "\n")
    info["found"] = True
    info["reason"] = ""
    return info


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def copy_text(source, destination):
    """Copies a repository text file into the package; CRLF_TYPES get Windows line endings."""
    with open(source, "rb") as handle:
        data = handle.read()
    extension = os.path.splitext(destination)[1].lower()
    if extension in ASCII_TYPES:
        try:
            data.decode("ascii")
        except UnicodeDecodeError as error:
            raise PackageError("%s must be ASCII (Windows PowerShell 5.1 and cmd.exe): %s" % (source, error))
    if extension in CRLF_TYPES:
        data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
    os.makedirs(os.path.dirname(destination), exist_ok=True)
    with open(destination, "wb") as handle:
        handle.write(data)


def start_here(template, name, builds, notes, made_by):
    rows, missing, sources = [], [], []
    for build in builds:
        label = build["slot"] + "\\"
        if build["found"]:
            rows.append("  %-15s %s, version %s" % (label, build["title"], build.get("version", "unknown")))
            sources.append("  %-13s branch %s, commit %s, built %s" % (build["slot"], build.get("branch", "unknown"),
                                                                    build.get("commit", "unknown")[:12], build.get("built", "unknown")))
            sources.append("  %-13s %s" % ("", build.get("run_url", "")))
            if build.get("chosen_by_run_id"):
                sources.append("  %-13s chosen by run number, not as the newest successful run (result of that run: %s)" % ("", build.get("conclusion", "unknown")))
        else:
            rows.append("  %-15s NOT IN THIS PACKAGE (%s)" % (label, build["title"]))
            missing.append("  %s: %s" % (build["slot"], build["reason"]))
            sources.append("  %-13s not in this package: %s" % (build["slot"], build["reason"]))
    missing_text = ""
    if missing or notes:
        lines = ["", "MISSING FROM THIS PACKAGE"]
        lines += missing
        if missing:
            lines.append("  Install-Slipstream.cmd installs what is here and says what it skipped. Without the original")
            lines.append("  build there is nothing to compare against; without the stable candidate, steps 2 and 3 below")
            lines.append("  do not apply.")
        lines += ["  " + note for note in notes]
        missing_text = "\n".join(lines)
    if made_by:
        sources.append("  Package made by: %s" % made_by)
    text = template.replace("@PACKAGE@", name).replace("@BUILDS@", "\n".join(rows)).replace("@MISSING@", missing_text)
    text = text.replace("@SOURCES@", "\n".join(sources))
    left = re.search(r"@[A-Z_]+@", text)
    if left:
        raise PackageError("unfilled placeholder in START-HERE.txt: %s" % left.group(0))
    return text


def check_private(stage):
    """Problems that must stop the package: user data, logs, settings or key material in it."""
    problems = []
    for folder, dirs, names in os.walk(stage):
        for name in list(dirs) + names:
            for pattern in PRIVATE_NAMES:
                if fnmatch.fnmatch(name.lower(), pattern.lower()):
                    problems.append("%s (matches %s)" % (os.path.relpath(os.path.join(folder, name), stage), pattern))
        for name in names:
            path = os.path.join(folder, name)
            if os.path.islink(path):
                problems.append("%s (a link)" % os.path.relpath(path, stage))
                continue
            if os.path.splitext(name)[1].lower() not in TEXT_TYPES or os.path.getsize(path) > (8 << 20):
                continue
            with open(path, "rb") as handle:
                data = handle.read()
            for what, pattern in SECRET_PATTERNS:
                if pattern.search(data):
                    problems.append("%s (%s)" % (os.path.relpath(path, stage), what))
    return problems


def write_sums(stage):
    lines = []
    for folder, dirs, names in os.walk(stage):
        dirs.sort()
        for name in sorted(names):
            path = os.path.join(folder, name)
            relative = os.path.relpath(path, stage).replace(os.sep, "/")
            if relative == "SHA256SUMS.txt":
                continue
            lines.append("%s  %s" % (sha256_of(path), relative))
    lines.sort(key=lambda line: line[66:])
    write_text(os.path.join(stage, "SHA256SUMS.txt"), "\r\n".join(lines) + "\r\n")
    return len(lines)


def zip_folder(stage, zip_path):
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for folder, dirs, names in os.walk(stage):
            dirs.sort()
            for name in sorted(names):
                path = os.path.join(folder, name)
                archive.write(path, os.path.relpath(path, stage).replace(os.sep, "/"))


def assemble(artifacts, repo, out, name, made_by=""):
    """Builds out/<name>/ and out/<name>.zip. Returns (zip path, builds, notes)."""
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,100}", name):
        raise PackageError("package name may hold letters, digits, '.', '_' and '-' only: %r" % name)
    stage = os.path.join(out, name)
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    notes = []

    builds = [add_build(slot, title, artifacts, stage) for slot, _branch, title, _folder in SLOTS]
    for build in builds:
        log("%-13s %s" % (build["slot"], "version %s" % build["version"] if build["found"] else "MISSING: " + build["reason"]))

    for relative in KIT_FILES:
        source = os.path.join(repo, *relative.split("/"))
        if not os.path.isfile(source):
            if relative in KIT_REQUIRED:
                raise PackageError("the repository has no %s: the package's .cmd files would not work" % relative)
            notes.append("testkit: %s is not in the branch this package was made from" % relative)
            continue
        copy_text(source, os.path.join(stage, "testkit", *relative.split("/")))
    for relative in DOC_FILES:
        source = os.path.join(repo, *relative.split("/"))
        if not os.path.isfile(source):
            notes.append("docs: %s is not in the branch this package was made from" % os.path.basename(relative))
            continue
        copy_text(source, os.path.join(stage, "docs", os.path.basename(relative)))
    here = os.path.join(repo, "packaging", "slipstream")
    for top in TOP_FILES:
        copy_text(os.path.join(here, top), os.path.join(stage, top))
    licence = os.path.join(repo, "LICENSE")
    if os.path.isfile(licence):
        shutil.copyfile(licence, os.path.join(stage, "LICENSE.txt"))

    with open(os.path.join(here, "START-HERE.template.txt"), "r", encoding="ascii") as handle:
        template = handle.read()
    text = start_here(template, name, builds, notes, made_by)
    write_text(os.path.join(stage, "START-HERE.txt"), text.replace("\r\n", "\n").replace("\n", "\r\n"))

    problems = check_private(stage)
    if problems:
        raise PackageError("the package would hold private or secret material:\n  " + "\n  ".join(problems[:20]))
    count = write_sums(stage)
    zip_path = os.path.join(out, name + ".zip")
    if os.path.exists(zip_path):
        os.remove(zip_path)
    zip_folder(stage, zip_path)
    digest = sha256_of(zip_path)
    write_text(zip_path + ".sha256", "%s  %s\n" % (digest, os.path.basename(zip_path)))
    log("%s: %d files, %.1f MB, sha256 %s" % (os.path.basename(zip_path), count + 1, os.path.getsize(zip_path) / 1e6, digest))
    return zip_path, builds, notes


def summary(builds, notes, name):
    lines = ["### %s" % name, "", "| build | in the package | version | branch | commit |", "|---|---|---|---|---|"]
    for build in builds:
        if build["found"]:
            lines.append("| %s | yes | %s | %s | %s |" % (build["slot"], build.get("version", ""), build.get("branch", ""), build.get("commit", "")[:12]))
        else:
            lines.append("| %s | **missing**: %s | | %s | |" % (build["slot"], build["reason"].replace("|", "/"), build.get("branch", "")))
    lines += ["", "The artifact is a zip that holds the package zip: unpack it once to get the file to hand on."]
    lines += ["- " + note for note in notes]
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    get = commands.add_parser("fetch")
    get.add_argument("--slot", required=True, choices=[slot for slot, _b, _t, _f in SLOTS])
    get.add_argument("--branch", default="")
    get.add_argument("--workflow", default="build-windows.yml")
    get.add_argument("--run-id", default="")
    get.add_argument("--out", required=True)
    get.add_argument("--gh", default="gh")
    make = commands.add_parser("assemble")
    make.add_argument("--artifacts", required=True)
    make.add_argument("--repo", default=".")
    make.add_argument("--out", default="dist")
    make.add_argument("--name", default="")
    make.add_argument("--made-by", default="")
    make.add_argument("--summary", default="")
    arguments = parser.parse_args(argv)
    try:
        if arguments.command == "fetch":
            branch = arguments.branch or dict((slot, default) for slot, default, _t, _f in SLOTS)[arguments.slot]
            fetch(arguments.slot, branch, arguments.out, arguments.workflow, arguments.run_id.strip(), arguments.gh)
            return 0
        name = arguments.name or "Slipstream-test-package-%s-local" % datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
        _zip_path, builds, notes = assemble(arguments.artifacts, arguments.repo, arguments.out, name, arguments.made_by)
        if arguments.summary:
            with open(arguments.summary, "a", encoding="utf-8") as handle:
                handle.write(summary(builds, notes, name))
        if not any(build["found"] for build in builds):
            log("WARNING: no build is in this package; it holds the kit and the documents only")
        return 0
    except PackageError as error:
        print("ERROR: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
