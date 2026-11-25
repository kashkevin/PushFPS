"""Linux tests of the analysis in lowdelay_e2e.py on synthetic logs in the app's own line formats (python3 -B -m pytest)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lowdelay_e2e as e2e  # noqa: E402

T = int(round(e2e.SLOT_MS * 3 * 1e4))   # 100 ns
SLOT = T // 3
TS = "2026-11-24T00:00:00.000Z"
HEADER = ("wall,frames,generated,fgActive,previewFgMultiplier,softwareLatencyMs,softwareLatencyP95Ms,realPresentFps,generatedPresentFps,"
          "captureAgeMs,schedulingWaitP95Ms,previewSkipped,rowWanted,rowState,rowRefusal,rowSuspend,vfgRefused,failed,presentationEnabled,"
          "display,smoothTarget,smoothChosen,sourceHdr")


def synth(predicted_every: int = 0, pairs: int = 300, extra_ms: float = 30.0, refused: bool = False, lowdelay_line: str = "") -> str:
    """Pairs as the engine logs them: one [pacing-sample] + one [submit] per present. predicted_every: 0 none, 1 all, n: one in n."""
    lines = [f"{TS} t=1 [WARN ] [vfg] TEST-ONLY fake VFG (VEYRA_TEST_FAKE_VFG=1): ... size=480x270 format=28 multiplier=3"]
    if refused:
        lines.append(f"{TS} t=2 [WARN ] [vfg] injected rejection of a FrameIndex-2-only request (VEYRA_TEST_VFG_REJECT_ALONE, TEST-ONLY fake VFG)")
    base = 10_000_000_000
    n_pred = 0
    for k in range(pairs):
        predicted = predicted_every > 0 and k % predicted_every == 0
        n_pred += predicted
        arrival = base + k * T
        b_pts = k * T
        real_end = arrival + int(extra_ms * 1e4) - (SLOT if predicted else 0)
        if predicted:
            items = [(b_pts - SLOT, real_end - SLOT, True), (b_pts, real_end, False), (b_pts + SLOT, real_end + SLOT, True)]
        else:
            items = [(b_pts - 2 * SLOT, real_end - 2 * SLOT, True), (b_pts - SLOT, real_end - SLOT, True), (b_pts, real_end, False)]
        for sub, (pts, end, gen) in enumerate(items, start=1):
            lines.append(f"{TS} t=3 [INFO ] [pacing-sample] mode=0 enabled=true sync=0 pts={pts} begin={end - 100} end={end} "
                         f"ready=0 process=0 generated={'true' if gen else 'false'} queue=1 lateMs=0.000 arrival={arrival} deadlinePaced=true duringProcess=false")
            lines.append(f"{TS} t=3 [INFO ] [submit] batch={k} epoch=1 revision=1 subframe={sub} pts100ns={pts} host100ns={end} fence=1 (submission, display unmeasured)")
        if k % 30 == 0:
            wait = 11.1 if predicted_every == 1 else 22.2
            ld = f" lowDelay=active predictedPairs=30 lowDelaySwitches=0" if predicted_every == 1 else " lowDelay=calm predictedPairs=0 lowDelaySwitches=0"
            lines.append(f"{TS} t=3 [INFO ] [capture-timing] revision=1 skippedPairs=0 delayMs={extra_ms:.2f} delayWaitMs={wait:.2f}{ld} (...)")
    if lowdelay_line:
        lines.append(f"{TS} t=4 [INFO ] [fg-sched] {lowdelay_line}")
    elif predicted_every:
        lines.append(f"{TS} t=4 [INFO ] [fg-sched] lowDelay state=active refusal=ok paused=none policy=stay-in unusableShown=0 kitSwitch=false row=true predictedPairs={n_pred} entries=1 exits=off:0 calmNeeded=8 predictPass=true predictedLate=0/{n_pred} gridExits=0 (one ...)")
    return "\n".join(lines)


def make(tmp_path, name, text, state, refusal, chosen=3, predict_lines=()):
    folder = tmp_path / name
    folder.mkdir()
    (folder / "engine.log").write_text(text + "".join(f"\n{TS} t=5 [INFO ] [push-predict] {m}" for m in predict_lines))
    rows = [HEADER] + [f"{w / 2},1,1,1,3,0,0,30,60,0,0,0,1,{state},{refusal},none,0,0,1,1,90,{chosen},0" for w in range(0, 40)]
    (folder / "states.csv").write_text("\n".join(rows) + "\n")
    (folder / "host-output.txt").write_text("OK 0\n")
    return e2e.load_run(folder, name)


def failed(checks):
    return [c for c in checks.items if not c["ok"]]


def test_patterns_and_readout(tmp_path):
    a = make(tmp_path, "A", synth(0), "idle", "switch-off")
    b = make(tmp_path, "B", synth(1), "active", "ok")
    pa, pb = e2e.pair_patterns(a), e2e.pair_patterns(b)
    assert pa["interpolate"] > 150 and pa["predicted"] == 0
    assert pb["predictedShare"] > 0.99 and pb["ptsOutOfOrder"] == 0
    ra, rb = e2e.readout(a), e2e.readout(b)
    assert abs((ra["meanMs"] - rb["meanMs"]) - e2e.SLOT_MS) < 0.05
    assert abs(ra["generatedPerReal"] - 2) < 0.02


def test_secondary_runs_pass(tmp_path):
    runs = {"A": make(tmp_path, "A", synth(0), "idle", "switch-off"),
            "B": make(tmp_path, "B", synth(1), "active", "ok"),
            "R": make(tmp_path, "R", synth(0, refused=True), "unavailable", "vfg-refused"),
            "hud": make(tmp_path, "hud", synth(0), "not-here", "hud-lock-or-exclusion")}
    checks = e2e.Checks()
    summary = e2e.assert_runs(runs, checks, "p15")
    assert not failed(checks), failed(checks)
    assert 0.95 < summary["B"]["gainSlots"] < 1.05


def test_owner_scenario_reproduces_p15(tmp_path):
    p15_line = "lowDelay state=not-here refusal=smoothness-mode paused=none policy=stay-in unusableShown=0 kitSwitch=false row=true predictedPairs=0 entries=0 exits=off:0 calmNeeded=8 predictPass=true (...)"
    runs = {"ownerOff": make(tmp_path, "ownerOff", synth(0), "idle", "switch-off"),
            "owner": make(tmp_path, "owner", synth(0, lowdelay_line=p15_line), "not-here", "smoothness-mode")}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "p15")
    assert not failed(checks), failed(checks)
    # The same output must FAIL the fixed phase.
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "fixed")
    names = {c["name"] for c in failed(checks)}
    assert any(n.startswith("owner: the mode enters") for n in names)
    assert any(n.startswith("owner: predicted pairs run") for n in names)
    assert any(n.startswith("owner: HUD-locked pixels") for n in names)


def test_owner_scenario_fixed(tmp_path):
    runs = {"ownerOff": make(tmp_path, "ownerOff", synth(0), "idle", "switch-off"),
            "owner": make(tmp_path, "owner", synth(1), "active", "ok", predict_lines=["hudLockMismatch=0 checked=1200"])}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "fixed")
    assert not failed(checks), failed(checks)
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "p15")
    assert any(c["name"].startswith("owner: REPRODUCES") for c in failed(checks))


def test_step_down_to_2x_must_be_named(tmp_path):
    # Smoothness Mode at 2X on WARP: the row must say not-here / not-3x, not keep running or say something else.
    runs = {"owner": make(tmp_path, "owner", synth(1), "active", "ok", chosen=2, predict_lines=["hudLockMismatch=0"])}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "fixed")
    assert any(c["name"].startswith("owner: at 2X the row says not-here / not-3x") for c in failed(checks))
    runs = {"owner": make(tmp_path, "owner2", synth(0), "not-here", "not-3x", chosen=2)}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "fixed")
    assert not failed(checks), failed(checks)   # a whole run at 2X: the named state is asserted, the rest are notes


def test_skip2_phases(tmp_path):
    text = synth(10).replace("skippedPairs=0", "skippedPairs=1")   # one pair in ten predicted: the collapse
    runs = {"skip2": make(tmp_path, "skip2", text, "calm", "ok")}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "p15")
    assert not failed(checks), failed(checks)
    # Fixed, 30.00 Hz: the share is a note (WARP); the 59.94 Hz twin asserts it and fails on the same output.
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "fixed")
    timing = [c for c in failed(checks) if c["name"].startswith("skip2:") and ("90 %" in c["name"] or "active (" in c["name"])]
    assert not timing, timing   # structure may fail (no whole active window); timing at 30.00 Hz never does
    runs = {"skip2_60": make(tmp_path, "skip2_60", text, "calm", "ok")}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "fixed")
    assert any(c["name"].startswith("skip2_60: >= 90 % of pairs shown I, B, P") for c in failed(checks))


def test_mode_never_entering_is_reported(tmp_path):
    runs = {"A": make(tmp_path, "A", synth(0), "idle", "switch-off"),
            "B": make(tmp_path, "B", synth(0), "calm", "ok")}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "p15")
    names = {c["name"] for c in failed(checks)}
    assert any(n.startswith("B: the row reports active") for n in names)
    assert any(n.startswith("B: >= 90 % of pairs shown I, B, P") for n in names)
    assert any(n.startswith("B: real frame's readout lower") for n in names)


def test_wrong_static_reason_fails(tmp_path):
    runs = {"m2": make(tmp_path, "m2", synth(0), "unavailable", "switch-off")}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "p15")
    assert any(c["name"].startswith("m2: the row says not-here / not-3x") for c in failed(checks))


def test_no_runs_fails(tmp_path):
    assert e2e.main(["--out", str(tmp_path / "none"), "--expect", "p15"]) == 1


def test_removed_device_fails_any_run(tmp_path, capsys):
    text = synth(0) + f"\n{TS} t=7 [ERROR] [gpu-failure] operation=GPU-frame-ready-timeout alive=false removedReason=0x887A0005 requestedFence=40"
    runs = {"A": make(tmp_path, "A", text, "idle", "switch-off")}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "p15")
    assert any(c["name"].startswith("A: the D3D12 device was never removed") for c in failed(checks))
    assert "last 40 lines of engine.log" in capsys.readouterr().out


def test_stall_effects(tmp_path):
    base = 10_000_000_000
    text = synth(0, pairs=600)
    stall_start = base + 300 * T
    # Frames 300-303 arrive at the end of a 120 ms gap: shown late by the gap's remainder.
    lines = []
    for line in text.splitlines():
        if "[pacing-sample]" in line and "generated=false" in line:
            d = e2e.kv(line)
            arrival = int(d["arrival"])
            k = (arrival - base) // T
            if 300 <= k <= 303:
                late = stall_start + 1_200_000 - arrival + int(30 * 1e4)
                line = line.replace(f"end={d['end']}", f"end={arrival + late}")
        lines.append(line)
    lines.append(f"{TS} t=9 [INFO ] [e2e-stall] stall=1 startHost100ns={stall_start} gapMs=120.0 (TEST ONLY)")
    run = make(tmp_path, "stallRide", "\n".join(lines), "idle", "switch-off")
    eff = e2e.stall_effects(run)
    assert len(eff) == 1 and eff[0]["lateReal"] >= 3 and eff[0]["worstMs"] > 100 and eff[0]["pacingBackMs"] is not None


def test_pair_readout_split_shows_b_one_slot_earlier(tmp_path):
    # Half the pairs predicted: the same run shows B one slot earlier in its predicted pairs.
    run = make(tmp_path, "half", synth(2), "active", "ok")
    pr = e2e.pair_readouts(run)
    assert pr["predicted"]["n"] >= 20 and pr["interpolate"]["n"] >= 20
    assert 0.95 < pr["gainSlots"] < 1.05
    checks, summary = e2e.Checks(), {}
    e2e.check_common("half", run, summary, checks)
    assert any("one output slot earlier in predicted pairs" in c["name"] and c["ok"] for c in checks.items)


def test_pair_readout_split_fails_when_b_is_not_earlier(tmp_path):
    # A defect: pairs presented I, B, P but B at an interpolated pair's time: the check must fail.
    run = make(tmp_path, "late", synth(2), "active", "ok")
    # Shift every real frame of a predicted pair one slot later (as an interpolated pair's B).
    pr0 = e2e.pair_readouts(run)
    assert pr0["gainSlots"] > 0.9
    for p in run.presents:
        if not p.generated and (p.pts // T) % 2 == 0:
            p.end += SLOT
    pr = e2e.pair_readouts(run)
    assert abs(pr["gainSlots"]) < 0.05
    checks, summary = e2e.Checks(), {}
    e2e.check_common("late", run, summary, checks)
    assert any("one output slot earlier in predicted pairs" in c["name"] and not c["ok"] for c in checks.items)


def test_gate_definition_twins_and_30hz_exits(tmp_path):
    late = ("lowDelay state=active refusal=ok paused=none policy=stay-in unusableShown=0 kitSwitch=false row=true predictedPairs=300 "
            "entries=2 exits=off:0,skipped:0,late:1,dear:0 lastStop=late calmNeeded=300 predictedLate=3/90 gridExits=0 (...)")
    skipped = late.replace("skipped:0,late:1", "skipped:1,late:0")
    dear = late.replace("late:1,dear:0", "late:0,dear:1")
    many = late.replace("predictedLate=3/90", "predictedLate=9/90")
    runs = {"A": make(tmp_path, "A", synth(0), "idle", "switch-off"),
            "clean60": make(tmp_path, "clean60", synth(2, lowdelay_line=late), "active", "ok"),
            "skip2_60": make(tmp_path, "skip2_60", synth(2, lowdelay_line=late), "active", "ok"),
            "B": make(tmp_path, "B", synth(2, lowdelay_line=many), "active", "ok"),
            "soak60": make(tmp_path, "soak60", synth(2, lowdelay_line=late), "active", "ok"),
            "clean30": make(tmp_path, "clean30", synth(2, lowdelay_line=skipped), "calm", "ok"),
            "predictedSoak": make(tmp_path, "predictedSoak", synth(2, lowdelay_line=dear), "calm", "ok")}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "fixed")
    names = {c["name"] for c in failed(checks)}
    assert "clean60: exits only of kind late" not in names and not any(n.startswith("clean60: predictedLate") for n in names)
    assert any(n.startswith("B: predictedLate at most") for n in names)               # 10 % late in a twin with an exit
    assert "soak60: no exits in 60 s" in names                                         # soak60: strict
    assert any(n.startswith("59.94 Hz twins: at most 2 runs") for n in names), names    # clean60, skip2_60, B: three
    assert "clean30: no exit of a kind other than late, skipped or grid" not in names  # skipped at 30 Hz: a note
    assert "predictedSoak: no exit of a kind other than late, skipped or grid" in names   # dear at 30 Hz fails


def test_exits_counted_after_start_up_from_the_per_second_totals():
    run = e2e.Run("x")
    run.exit_totals = [(100.0, {"skipped": 0, "late": 0}), (102.0, {"skipped": 1, "late": 0}),      # a start-up exit
                       (104.0, {"skipped": 1, "late": 0}), (115.0, {"skipped": 1, "late": 1})]     # a late one near the end
    assert e2e.exit_counts(run) == {"skipped": 0, "late": 1}
