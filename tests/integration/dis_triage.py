# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 DosukaSOL and the PushFPS contributors. Part of PushFPS, a modified version of Veyra.
"""Crash triage of GPU DIS (the predicted frame's flow) on WARP or a GPU: .github/workflows/gpu-quick.yml.

Runs every opt-in case dis-<variant>-<WxH> of veyra_push_gpu_tests in its own process (a crash ends a process). For a case
that crashes or hangs, it lists the case's dispatches without executing any of GPU DIS's own (VEYRA_TEST_DIS_LIMIT=0 with
VEYRA_TEST_DIS_TRACE=1), then bisects the limit, each run a new process, to the first dispatch whose execution fails, and
runs that limit once more with GPU-based validation (VEYRA_PUSH_GPU_GBV=1). Writes summary.json and one log per run.

    python dis_triage.py --exe veyra_push_gpu_tests.exe --out <dir> [--cases dis-float-64x36,...] [--timeout 600]
"""
import argparse
import json
import os
import re
import subprocess
import sys

CASES = ["dis-float-64x36", "dis-strict-64x36", "dis-float-240x135", "dis-strict-240x135", "dis-float-960x540", "dis-strict-960x540"]
BAD = re.compile(r"crashed got=exception|timed out got=|did not finish a submission|GPU wait ended with fence")


def run(exe, case, out, name, timeout, env_extra):
    env = dict(os.environ)
    env["VEYRA_PUSH_GPU_DEBUG"] = "1"
    for key in ("VEYRA_TEST_DIS_LIMIT", "VEYRA_TEST_DIS_TRACE", "VEYRA_PUSH_GPU_GBV"):
        env.pop(key, None)
    env.update(env_extra)
    try:
        p = subprocess.run([exe, "--only", case, "--test-timeout", str(timeout)], env=env, capture_output=True, text=True,
                           errors="replace", timeout=timeout + 60)
        text, code = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        text, code = (e.stdout or "") if isinstance(e.stdout, str) else "", -999
        text += "\nFAIL triage: process killed after the timeout\n"
    with open(os.path.join(out, f"{name}.log"), "w", encoding="utf-8") as f:
        f.write(text)
    bad = code == -999 or bool(BAD.search(text))
    steps = re.findall(r"^STEP [^:]+: (.+)$", text, re.M)
    return {"exit": code, "bad": bad, "lastStep": steps[-1] if steps else None,
            "fails": [l for l in text.splitlines() if l.startswith("FAIL ")][:12],
            "info": [l for l in text.splitlines() if l.startswith("INFO ") or l.startswith("NOTE ")][:12]}, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--cases", default=",".join(CASES))
    ap.add_argument("--timeout", type=int, default=600)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    summary = {}
    for case in [c for c in a.cases.split(",") if c]:
        entry = {}
        full, _ = run(a.exe, case, a.out, f"{case}-full", a.timeout, {})
        entry["full"] = full
        print(f"{case}: full run exit={full['exit']} bad={full['bad']} lastStep={full['lastStep']}", flush=True)
        if full["bad"]:
            listing, text = run(a.exe, case, a.out, f"{case}-list", a.timeout, {"VEYRA_TEST_DIS_LIMIT": "0", "VEYRA_TEST_DIS_TRACE": "1"})
            names = [m.group(2) for m in re.finditer(r"^TRACE [^:]+: (\d+) (.+)$", text, re.M)]
            entry["limit0"] = listing
            entry["dispatches"] = len(names)
            print(f"{case}: {len(names)} dispatches; limit 0 (no GPU DIS dispatch executed) bad={listing['bad']}", flush=True)
            if not listing["bad"] and names:
                lo, hi = 0, len(names)   # lo: known good, hi: known bad
                while hi - lo > 1:
                    mid = (lo + hi) // 2
                    r, _ = run(a.exe, case, a.out, f"{case}-limit{mid}", a.timeout, {"VEYRA_TEST_DIS_LIMIT": str(mid)})
                    print(f"{case}: limit {mid} bad={r['bad']} exit={r['exit']}", flush=True)
                    if r["bad"]:
                        hi = mid
                    else:
                        lo = mid
                entry["firstBadDispatch"] = {"index": hi - 1, "name": names[hi - 1], "previous": names[max(0, hi - 4):hi - 1]}
                print(f"{case}: first failing dispatch #{hi - 1}: {names[hi - 1]}", flush=True)
                gbv, _ = run(a.exe, case, a.out, f"{case}-limit{hi}-gbv", a.timeout, {"VEYRA_TEST_DIS_LIMIT": str(hi), "VEYRA_PUSH_GPU_GBV": "1"})
                entry["gbv"] = gbv
        summary[case] = entry
    with open(os.path.join(a.out, "summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
