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
            lines.append(f"{TS} t=3 [INFO ] [capture-timing] revision=1 skippedPairs=0 delayMs={extra_ms:.2f} delayWaitMs={wait:.2f} (...)")
    if lowdelay_line:
        lines.append(f"{TS} t=4 [INFO ] [fg-sched] {lowdelay_line}")
    elif predicted_every:
        lines.append(f"{TS} t=4 [INFO ] [fg-sched] lowDelay state=active refusal=ok paused=none policy=stay-in unusableShown=0 kitSwitch=false row=true predictedPairs={n_pred} entries=1 exits=off:0 calmNeeded=8 predictPass=true (one ...)")
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
    assert any(n.startswith("owner: the row reports active") for n in names)
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


def test_smoothness_must_choose_3x(tmp_path):
    runs = {"owner": make(tmp_path, "owner", synth(1), "active", "ok", chosen=2, predict_lines=["hudLockMismatch=0"])}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "fixed")
    assert any(c["name"].startswith("owner: Smoothness Mode (target 90) chooses 3X") for c in failed(checks))


def test_skip2_phases(tmp_path):
    text = synth(10).replace("skippedPairs=0", "skippedPairs=1")   # one pair in ten predicted: the collapse
    runs = {"skip2": make(tmp_path, "skip2", text, "calm", "ok")}
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "p15")
    assert not failed(checks), failed(checks)
    checks = e2e.Checks()
    e2e.assert_runs(runs, checks, "fixed")
    assert any(c["name"].startswith("skip2: with 2 % skipped pairs >= 90 %") for c in failed(checks))


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
