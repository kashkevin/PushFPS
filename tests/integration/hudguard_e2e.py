#!/usr/bin/env python3
"""End-to-end engine test of HUD Guard (stream "s12-hud", review S12_HUD_REVIEW1 MUST 2): runs the real engine
(veyra_lowdelay_e2e_tests: EngineController, EnhanceGraph, presenter) with the TEST-ONLY fake VFG on WARP, Live HUD Lock on,
and asserts from engine.log that the graph hooks run:

  hg2 / hg3 / hg4   multiplier 2, 3, 4 (SDR): [hud-guard] created; state=executing preClean=on in steady seconds, with
                    cleans > 0 and composes close to (multiplier - 1) x cleans (each generated frame composed once);
  hgHdr             3X from a 10-bit PQ clip (HDR10 output, R10G10B10A2): the same, and the guard created with mode=2;
                    the clip carries the PQ / BT.2020 tags in its container (the file source decides HDR when it opens);
  hgLowDelay        3X with the Lower delay row on (zero motion source): preClean=off-lower-delay, composes still counted;
  hgLegacy          VEYRA_TEST_RP_LEGACY_HUD_GUARD=1: "not created", no [hud-guard] state lines (the restoring switch).
Every run: no device removal, no [d3d12-debug] error at all (the debug layer is on when E2E_D3D12_DEBUG=1; id=1047, a
real frame used by two queues without a fence, was in every run with Live HUD Lock up to Package 16 and is fixed:
/home/claude/out/research/D3D12_1047.md), no ERROR line of hud-guard / hud-lock. WARP proves the hooks run and are safe, not timing or picture quality.

  python hudguard_e2e.py --exe <veyra_lowdelay_e2e_tests.exe> --out <dir> [--runs hg2,hg3,...] [--seconds 14]

Uses lowdelay_e2e.py's clip maker and runner (same folder). Exit code 0 only if every assertion passed. Standard library only.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lowdelay_e2e as base  # noqa: E402

GUARD = re.compile(r"\[(?:INFO |WARN |ERROR)\] \[hud-guard\] (.*)$")
QUALITY = ["--quality", "5", "--sync", "tearing"]
RUNS = {
    "hg2": ("card30", ["--hud-lock", "--multiplier", "2"] + QUALITY, {}, 2, "on"),
    "hg3": ("card30", ["--hud-lock"] + QUALITY, {}, 3, "on"),
    "hg4": ("card30", ["--hud-lock", "--multiplier", "4"] + QUALITY, {}, 4, "on"),
    # VEYRA_TEST_REPLAY_HDR10_CONTRACT: the replayed file opens as HDR10, as the capture card does (D3D12_1047.md).
    "hgHdr": ("hdr", ["--hud-lock"] + QUALITY, {"VEYRA_TEST_ASSUME_HDR_DISPLAY": "1", "VEYRA_TEST_REPLAY_HDR10_CONTRACT": "1"}, 3, "on"),
    "hgLowDelay": ("card30", ["--hud-lock", "--low-delay"] + QUALITY, {"VEYRA_TEST_PREDICT_FLOW": "zero"}, 3, "off-lower-delay"),
    "hgLegacy": ("card30", ["--hud-lock"] + QUALITY, {"VEYRA_TEST_RP_LEGACY_HUD_GUARD": "1"}, 3, None),
}


def guard_lines(text: str) -> list:
    out = []
    for raw in text.splitlines():
        m = GUARD.search(raw)
        if m:
            out.append(m.group(1))
    return out


def guard_stamped(text: str) -> list:
    """(log seconds, fields) of each [hud-guard] state line."""
    out = []
    for raw in text.splitlines():
        m = base.LINE.search(raw)
        if m and m.group("tag") == "hud-guard" and m.group("msg").startswith("state="):
            ts = base.seconds_of(m.group("ts"))
            if ts is not None:
                out.append((ts, base.kv(m.group("msg"))))
    return out


def flow_totals(text: str) -> list:
    """(log seconds, fgEvaluated - fgWarmup) of each [frame-flow] line (session totals)."""
    out = []
    for raw in text.splitlines():
        m = base.LINE.search(raw)
        if m and m.group("tag") == "frame-flow":
            d = base.kv(m.group("msg"))
            ts = base.seconds_of(m.group("ts"))
            if ts is not None and "fgEvaluated" in d:
                out.append((ts, base.number(d.get("fgEvaluated"), 0) - base.number(d.get("fgWarmup"), 0)))
    return out


def evaluated_at(flow: list, ts: float) -> float:
    before = [v for t, v in flow if t <= ts]
    return before[-1] if before else 0.0


EXCERPT = ("[vfg]", "[graph] initialized", "[hud-guard] created", "[hud-lock] created", "hdr", "HDR", "[source", "[capture", "[decode")


def excerpt(text: str, limit: int = 40) -> None:
    for l in [l for l in text.splitlines() if any(k in l for k in EXCERPT)][:limit]:
        print("  # " + l[:300], flush=True)


DEBUG_ID = re.compile(r"\[d3d12-debug\] id=(\d+)")
KNOWN_DEBUG_IDS: set = set()   # D3D12_1047.md: id=1047 came from Live HUD Lock reading the previous real frame while the
                               # presentation queue showed it; fixed by the lock's private copies, so nothing is exempt now


def debug_ids(text: str) -> set:
    return {m.group(1) for l in text.splitlines() if "[ERROR]" in l for m in [DEBUG_ID.search(l)] if m}


def analyse(name: str, folder: Path, multiplier: int, pre_clean, checks: base.Checks, summary: dict, baseline_ids: set) -> None:
    log = folder / "engine.log"
    text = log.read_text(encoding="utf-8", errors="replace") if log.exists() else ""
    checks.check(bool(text), f"{name}: engine.log written", str(log))
    lines = guard_lines(text)
    created = [m for m in lines if m.startswith("created HUD guard")]
    states = [base.kv(m) for m in lines if m.startswith("state=")]
    failures = [l for l in text.splitlines() if "[gpu-failure]" in l or "removedReason=0x887A" in l or "DXGI_ERROR_DEVICE_REMOVED" in l]
    debug_errors = [l for l in text.splitlines() if "[d3d12-debug]" in l and "[ERROR]" in l and
                    (DEBUG_ID.search(l) is None or DEBUG_ID.search(l).group(1) not in baseline_ids)]
    known = sorted(debug_ids(text) & baseline_ids)
    if known:
        checks.note(f"{name}: debug-layer errors the engine has without the guard too", "ids " + ",".join(known))
    guard_errors = [l for l in text.splitlines() if "[ERROR]" in l and ("[hud-guard]" in l or "[hud-lock]" in l)]
    checks.check(not failures, f"{name}: no device removal / GPU failure", failures[0][:300] if failures else "")
    checks.check(not debug_errors, f"{name}: no D3D12 debug-layer error beyond those without the guard", debug_errors[0][:300] if debug_errors else "")
    checks.check(not guard_errors, f"{name}: no ERROR line of hud-guard / hud-lock", guard_errors[0][:300] if guard_errors else "")
    summary[name] = {"created": created, "stateLines": len(states), "last": states[-3:]}
    if name == "hgHdr" and "HDR input requires an explicit YUV HDR contract" in text:
        checks.check(False, f"{name}: the graph accepted the HDR10 clip", "[graph] HDR input requires an explicit YUV HDR contract")
        print(f"----- {name}: engine.log lines on the format and the source -----", flush=True)
        excerpt(text)
        return
    if pre_clean is None:   # the restoring switch
        checks.check(any("not created" in m for m in lines) and not created and not states,
                     f"{name}: VEYRA_TEST_RP_LEGACY_HUD_GUARD restores Package 16 (guard not created, no state lines)", "; ".join(lines[:3]))
        return
    checks.check(bool(created), f"{name}: [hud-guard] created", "; ".join(lines[:2]))
    if name == "hgHdr":
        # The graph may first be built before the source is known to be HDR; the guard of the graph that ran must be mode=2.
        checks.check(bool(created) and " mode=2 " in created[-1], f"{name}: created for HDR10 (mode=2, PQ thresholds)", " | ".join(c[:90] for c in created))
        ok = checks.check("format=24" in text, f"{name}: frame generation in R10G10B10A2 (24)", "")
        if not ok or " mode=2 " not in (created[-1] if created else ""):
            print(f"----- {name}: engine.log lines on the format and the source -----", flush=True)
            excerpt(text)
            host = folder / "host-output.txt"
            if host.exists():
                print("  host: " + host.read_text(encoding="utf-8", errors="replace")[-800:], flush=True)
    # Steady seconds: the guard updated, generated frames were composed, the pre-clean as expected.
    steady = [s for s in states if s.get("state") == "executing" and s.get("preClean") == pre_clean]
    checks.check(len(steady) >= 3, f"{name}: >= 3 seconds of [hud-guard] state=executing preClean={pre_clean}",
                 json.dumps(states[-4:]))
    if not steady:
        return
    ups = sum(base.number(s.get("updates"), 0) for s in steady)
    cleans = sum(base.number(s.get("cleans"), 0) for s in steady)
    composes = sum(base.number(s.get("composes"), 0) for s in steady)
    expected = (multiplier - 1) * ups
    summary[name].update({"updates": ups, "cleans": cleans, "composes": composes, "expectedComposes": expected})
    if pre_clean == "on":
        checks.check(cleans > 0 and cleans >= 0.8 * ups, f"{name}: the pre-clean ran for the VFG pairs (cleans {cleans:.0f} of {ups:.0f} updates)", "")
    else:
        checks.check(cleans == 0, f"{name}: no pre-clean while Lower delay may predict (cleans {cleans:.0f})", "")
    # Each generated frame composed once: composes = the generated frames the graph evaluated in the same seconds
    # ([frame-flow] fgEvaluated - fgWarmup, session totals; reset pairs are not shown and not composed). WARP skips pairs
    # when it falls behind, so (multiplier - 1) x updates is the upper bound, not the expectation.
    flow = flow_totals(text)
    stamped = guard_stamped(text)
    steady_ts = [ts for ts, d in stamped if d.get("state") == "executing" and d.get("preClean") == pre_clean]
    if flow and len(steady_ts) >= 2:
        t0, t1 = steady_ts[0], steady_ts[-1]
        comp = sum(base.number(d.get("composes"), 0) for ts, d in stamped if t0 < ts <= t1)
        ref = evaluated_at(flow, t1) - evaluated_at(flow, t0)
        slack = 0.1 * ref + 1.2 * (multiplier - 1) * 30
        summary[name].update({"composesWindow": comp, "evaluatedWindow": ref})
        checks.check(ref > 0 and abs(comp - ref) <= slack,
                     f"{name}: composes {comp:.0f} = the generated frames evaluated in the same seconds ({ref:.0f}, slack {slack:.0f})", "")
    else:
        checks.check(False, f"{name}: [frame-flow] and [hud-guard] lines to compare composes with", f"flow={len(flow)} steady={len(steady_ts)}")
    checks.check(composes <= expected + 2 * multiplier, f"{name}: composes {composes:.0f} <= (multiplier - 1) x updates ({expected:.0f})", "")


def make_hdr_clip(out: Path, size: str, seconds: int):
    """A 10-bit HEVC clip whose CONTAINER carries the PQ / BT.2020 tags: the file source decides HDR when it opens, from the
    stream parameters (MediaFileSource::parseColor), and the graph is built from that. Round 4 (e2e-quick 45739173611) opened
    the Matroska clip as transfer=2 (assumed) although its frames were PQ (first-frame hdr=true transfer=5), and the graph
    then refused the HDR frames. Both MP4 (colr atom) and Matroska are written; ffprobe picks the first that reads PQ."""
    import shutil
    import subprocess
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg:
        return None
    w, h = (int(v) for v in size.lower().split("x"))
    params = "colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc:range=limited:log-level=error"
    for ext, extra in (("mp4", ["-movflags", "+write_colr", "-tag:v", "hvc1"]), ("mkv", [])):
        clip = out / f"hdr10-tagged-{w}x{h}-30.{ext}"
        r = subprocess.run([ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i", f"testsrc2=size={w}x{h}:rate=30", "-t", str(seconds + 10),
                            "-vf", "format=yuv420p10le", "-c:v", "libx265", "-preset", "ultrafast", "-x265-params", params,
                            "-color_primaries", "bt2020", "-color_trc", "smpte2084", "-colorspace", "bt2020nc", "-color_range", "tv"] + extra + [str(clip)])
        if r.returncode != 0:
            print(f"NOTE HDR10 clip ({ext}) not written", flush=True)
            continue
        if not ffprobe:
            return clip
        p = subprocess.run([ffprobe, "-v", "error", "-select_streams", "v", "-show_entries", "stream=color_transfer,color_primaries,color_space,color_range",
                            "-of", "compact", str(clip)], capture_output=True, text=True)
        print(f"probe {clip.name}: {p.stdout.strip()}", flush=True)
        if "smpte2084" in p.stdout:
            return clip
    return None


def main(argv=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe")
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=int, default=14)
    ap.add_argument("--size", default="160x90")
    ap.add_argument("--runs", default=",".join(RUNS))
    a = ap.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    names = [n for n in a.runs.split(",") if n in RUNS]
    checks = base.Checks()
    if a.exe:
        exe = Path(a.exe)
        clips = base.make_clips(exe, out, a.size, a.seconds, 0, {RUNS[n][0] for n in names} - {"hdr"})
        if any(RUNS[n][0] == "hdr" for n in names):
            hdr = make_hdr_clip(out, a.size, a.seconds)
            if hdr:
                clips["hdr"] = hdr
        for n in names:
            kind, args, env_extra, _, _ = RUNS[n]
            if kind not in clips:
                checks.check(False, f"{n}: its clip ({kind}) could be made", "no clip: for HDR10 the runner needs ffmpeg with libx265")
                continue
            base.run_host(exe, clips[kind], out / n, a.seconds, args, env_extra)
    summary = {}
    baseline_ids = set(KNOWN_DEBUG_IDS)
    for n in names:
        if (out / n / "engine.log").exists():
            _, _, _, multiplier, pre_clean = RUNS[n]
            analyse(n, out / n, multiplier, pre_clean, checks, summary, baseline_ids)
    summary["checks"] = checks.items
    (out / "summary-hudguard.json").write_text(json.dumps(summary, indent=2, default=str), encoding="utf-8")
    print(f"hudguard e2e: {len([c for c in checks.items if c['ok']])} passed, {checks.failed} failed", flush=True)
    return 1 if checks.failed else 0


if __name__ == "__main__":
    sys.exit(main())
