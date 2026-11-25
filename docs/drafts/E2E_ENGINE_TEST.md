# End-to-end engine test of "Lower delay" (stream e2e, 24 Nov 2026)

Labels as in `LOW_DELAY_MODE.md`: **SIMULATED**, **TESTED**, **BUILT**, **HARDWARE-VERIFIED**. This test is a CI test on a
software renderer: at best it makes the chain **TESTED (WARP)**. It never makes anything HARDWARE-VERIFIED.

## Why

Packages 13-15 promised lower delay from the scheduling simulator (`tests/remoteplay/FgScheduleSim.h`: the decision code
with made-up costs). The simulator never runs the chain that has to work on the owner's PC: the row -> the preference ->
the graph rebuild -> the predicted-frame pass -> per-pair eligibility in `EngineController.cpp` -> VFG's call ->
presentation order and times -> the delay readout. This test runs that chain, minus the parts only an NVIDIA GPU has.

## What it runs

- **The real engine** (`EngineController`, `EnhanceGraph`, `VideoPresenter`, the live scheduler, the learners, admission,
  ContentDedup, the log), linked into a small host `veyra_lowdelay_e2e_tests` (`tests/integration/LowDelayE2ETests.cpp`),
  as `PresentationPacingTests` already did. No QML: the host does what `QmlPlayerBridge` does for the row (it sets
  `pushLowDelayShared().wanted` and `EnhancementSettings::lowDelay`; for the toggle run it also clears `vfgRefused` and calls
  `requestSettings`, as `applyPushLowDelay` does). The preference file and the QML row are not exercised here (they have
  their own QML test, `tst_push_lowdelay.qml`).
- **A capture card**: a lossless YUV4MPEG2 clip written by the host (`--make-clip`), 320x180 by default, 60000/1001 frames a
  second, every picture written twice (a 30 FPS game in a 59.94 Hz capture, bit-identical repeats), replayed through
  `captureReplayForTest` with `VEYRA_TEST_REPLAY_AS_CARD=1`: arrival stamped on the clip's PTS line (x 1.0008) like a
  card's callbacks, and every `physicalCapture || testReplayAsCard` branch of the Lower-delay chain taken (card timing,
  adaptive capture phase, `lowDelayCardVfg3x`, `[capture-timing]`).
- **Frame generation through VFG's code paths**: `frameGenerationBackend == Vfg`, quality 0, 3X, with VFG replaced by the
  test-only `TestVfgBackend` (`VEYRA_TEST_FAKE_VFG=1`, exactly "1"; never from slipstream.ini or a preference). It sits
  behind `VfgBackend`'s unchanged interface (one forwarding line per public method), so the graph and the engine cannot
  tell: same buffers, producer/completion fences and queue wait, bounded worker, back-to-back chained wait
  (`pushVfgChainedWait`), GPU shot-change test (`VfgShotChange.dxil`) with predicated copies, the FrameIndex-2-alone
  request and its refusal (`VEYRA_TEST_VFG_REJECT_ALONE=1`). A generated frame is a per-channel blend of A and B at
  FrameIndex / FrameMultiplier (`shaders/TestVfgBlend.hlsl`) on its own compute queue, after an artificial serial cost per
  request (`VEYRA_TEST_FAKE_VFG_COST_MS`, default 2 ms). On the software renderer the adapter is not NVIDIA, so under the
  same switch `EngineController` counts it as NVIDIA for stage selection (two lines; otherwise `describeStages` drops VFG).
  The start-up log names the fake twice (`[capture-test]`, `[vfg]`). The test shader is removed from release packages.
- **Presentation**: low queue on, display sync Tearing by default (`--sync`), so the CI display's refresh does not decide
  which frames are shown.

## The owner scenario (primary since his log of 24 Nov 2026)

His Package 15 log showed the mode never ran: `lowDelay state=not-here refusal=smoothness-mode predictedPairs=0`. The
primary runs therefore use HIS settings (from that log): a 30.00 Hz card without repeated pictures (dedup off), VFG 3X
quality 5, Smoothness Mode target 90 (`pushSmoothShared().targetFps`, as the bridge sets pref pushSmoothTarget), Live HUD
Lock, low queue, NVIDIA low latency requested (Reflex does not exist on WARP: the engine reports it unavailable), smooth
pacing (Vsync + variable refresh + output cap following the display), the same with display sync Automatic and with
smooth pacing switched off mid-run, windowed; the fake VFG costs his measured 2.0 ms (first frame) and 2.2 ms (second).

| Run | `--expect p15` (Package 15: must REPRODUCE his log) | `--expect fixed` (the branch that makes it run) |
|---|---|---|
| ownerOff | row off: wait part about two slots | same |
| owner (display sync Automatic) | `not-here` / `smoothness-mode`, predictedPairs 0, wait about two slots, readout as row off, Smoothness Mode 3X | `active`, >= 90 % of pairs I, B, P, wait about one slot, readout 0.6-1.4 slots lower than row off, Smoothness Mode 3X, `[push-predict] hudLockMismatch=0` (a GPU check the fixed branch must log) |
| ownerToggle | on -> off at 8 s -> on at 14 s: stall per toggle from `[engine-stall]`, off reads idle, on reads smoothness-mode again | active again within 90 pairs of each switch-on |
| clean30 | 30 Hz card, no Smoothness Mode / HUD Lock: the mode runs (>= 90 %) | same |
| skip2 | clean30 + 2 % skipped pairs (`VEYRA_TEST_SKIP_PAIRS_PERMILLE=20`): predicted share <= 50 % (the forensic report's scratch model: 1 %) | >= 90 % |
| ownerHdr (needs ffmpeg) | a 10-bit PQ clip + `VEYRA_TEST_ASSUME_HDR_DISPLAY=1`: frame generation in format 24 (R10G10B10A2) | same + the owner checks |
| ownerUhd (needs ffmpeg) | 3840x2160: the fake VFG and the predicted-frame pass are created (correctness, not speed) | same |
| hud, smooth (secondary) | `not-here` with their reasons | `active` |

Test-only switches added for this (environment only, never in slipstream.ini, each logged when set):
`VEYRA_TEST_FAKE_VFG`, `VEYRA_TEST_FAKE_VFG_COST_MS` ("first,later"), `VEYRA_TEST_FAKE_VFG_ALONE_COST_MS`,
`VEYRA_TEST_ALLOW_WARP` (`D3D12DeviceContext` refuses software adapters otherwise), `VEYRA_TEST_SKIP_PAIRS_PERMILLE`
(admission skips, fixed-seed generator), `VEYRA_TEST_ASSUME_HDR_DISPLAY` (the graph builds its HDR output on an SDR
display). The cost of the predicted frame P is printed per run as `fg2P50` of `[fg-health]` on predicted pairs: on WARP
it is a CPU rasteriser's time, NOT a GPU figure; his GPU's cost of P has never been measured.

### Build 45511031793 (5cce1b5a) and what changed after it

That build compiled and every run started (WARP device, fake VFG, first frames), but nothing could be observed:
generated frames were almost never shown (`generatedPerReal` 0.00-0.01), because (1) the fake's 2 ms cost was a
`sleep_until`, which the runner's system timer rounded to about 10-18 ms (`gpu_fg1P50` 10-18 ms), so every generated frame
missed its slot, and (2) the runner's display is 64 Hz, so Smoothness Mode with target 90 chose 2X. Changes: the fake waits
on a high-resolution waitable timer and logs where its time goes (`TEST-ONLY fake VFG timing`); every run sets the existing
`VEYRA_TEST_DISPLAY_HZ=179.952` (his refresh) for Smoothness Mode and the output hint; the owner runs use display sync
Automatic (his later session; Vsync on a 64 Hz display caps presents at 64 a second), and `ownerVsync` (smooth pacing on,
switched off at 10 s) asserts only the mode's state; the picture is 320x180; the 59.94 Hz runs set content-rate detection
(`--content-auto`) so repeats are removed; the toggle mirrors the bridge's state write (off reads idle at once). Every
other assertion keeps "the pipeline keeps up" as its precondition.

## Time scale (why a small picture and his 2.0 / 2.2 ms)

The real chain is timed by the clock, not scaled: the clip is played at 59.94 Hz and the schedule's deadlines are the real
ones (content interval 33.37 ms, one output slot 11.12 ms). What must hold is that the pipeline keeps up on WARP: the
graph's own work per pair plus the fake's cost must stay well inside one slot, or admission skips pairs and the
comparison measures the overload, not the schedule. Hence a small picture (WARP's per-pixel cost) and a 2 ms fake cost.
Every run first asserts it kept up (>= 1.8 generated frames shown per real frame); if not, the readout checks are not
meaningful and say so by failing that check first.

## The runs (`tests/integration/lowdelay_e2e.py`, one process each)

| Run | Configuration | Asserted from the app's own output |
|---|---|---|
| A | row off | no pair ran predicted; >= 90 % of pairs shown I, I, B; pipeline keeps up |
| B | row on | row state `active` in >= 90 % of samples after 4 s; `predictedPairs > 0` ([fg-sched] or [capture-timing]); >= 90 % of pairs shown I, B, P; every pair's presents in content order; 3X still shown; real frame's readout lower than A's by 0.6-1.4 output slots |
| L | row off, `VEYRA_TEST_RP_LEGACY_PACKAGE14=1` | A's readout equals L's within 0.25 slot (mean) and 0.5 slot (P95); same pair pattern |
| R | row on, fake refuses index 2 alone | the refusal is logged; row `unavailable` / `vfg-refused`; pairs continue as I, I, B; readout within 0.3 slot of A |
| T | row switched on at 8 s while playing | the rebuild happens and the row becomes `active` |
| hud, excl | row on with HUD Lock / an exclusion box | row `not-here` / `hud-lock-or-exclusion`; no predicted pair; generation continues |
| smooth | row on, Smoothness Mode target 90 | `not-here` / `smoothness-mode` |
| m2, m4 | row on at 2X / 4X | `not-here` / `not-3x` |
| adaptive | row on, adaptive multiplier | `not-3x` when the pair runs below 3X (else it may run) |

Readout = capture arrival -> the real frame's `Present` returned, per real frame, from `[pacing-sample]` (`end - arrival`),
the definition of the delay badge. Pair order from `[submit]` (batch = pair) joined with `[pacing-sample]` by PTS
(generated or real). The first 4 s are start-up (graph creation, learners, the calm run of 8 pairs).

**Tolerances.** The schedule moves the real frame by exactly one slot; around that, each schedule's phase learner adds its
own margin from measured costs, and on WARP these costs are noisy and differ from a GPU's. 0.6-1.4 slots accepts a learner
margin difference of up to 0.4 slot (4.4 ms) either way and still rejects "no gain" and "two slots". A vs L is a
same-logic comparison in two processes, so only run-to-run noise is allowed (0.25 slot mean). HDR10 output is not run:
the software renderer's display is not HDR, so `useHdrPreview` never selects it.

## What it proves (when it passes on the build machine)

That, in the built program, the row's setting reaches the engine, the graph is built with the predicted-frame pass at that
size and format, the per-pair eligibility admits capture-card pairs at 3X, the predicted schedule's frames are presented in
the order I, B, P, and the real frame leaves one slot earlier by the app's own readout, with the clock really running; that
a refusal of the index-2-alone request is reported and frame generation continues; that each static refusal is named; and
that the row off behaves as Package 14's logic.

## What it cannot prove

- **No NVIDIA model.** Whether VFG accepts FrameIndex 2 alone, and what it costs, is not tested: the fake accepts or refuses
  on command. The first log from the owner's PC must show it (`push-predict` / `[fg-sched] lowDelay refusal=vfg-refused`).
- **No CUDA interop**: no CUDA-D3D12 fence semantics, no shared-memory import, no CUDA stream timing.
- **No real GPU timing**: WARP is a CPU rasteriser; a 4K pass that costs 3 ms on a GPU can cost seconds there. Absolute
  times are never compared with anything; only A against B in the same run conditions.
- **No display**: Present returning is not scanout; G-SYNC/VRR, the monitor's refresh and the capture card's own delay are
  not in the readout and not in this test.
- **Not 4K** by default (a lossless 4K clip is ~12 MB per frame; a compressed one needs an encoder on the runner).
- **The picture** of the predicted frame is not judged (the blend is not a prediction).

## Fast loop results on Package 15's code (e2e-quick run 45530162944, WARP, 320x180; TESTED (WARP), not hardware)

- **Reproduced (his log):** with Smoothness Mode 90 the row reads `not-here` / `smoothness-mode`, `predictedPairs=0`, in
  `owner`, `ownerVsync` and after each switch-on of `ownerToggle`; on the 59.94 Hz path (`smooth`) the same with 3X kept,
  pairs shown I, I, B, `delayWaitMs` 22.24 (two slots, his 22.1), 2.0 generated frames shown per real frame. `hud`,
  `excl`, `m4` name their reasons; `A` equals Package 14's logic (mean 52.96 / 53.01 ms on WARP).
- **The predicted pair removes the D3D12 device on WARP.** In every run where the mode entered (`clean30`, `skip2`, `B`,
  `R`, `T`, `adaptive`): `[gpu-dis] record ... dispatches=552` for the first predicted pair (the GPU DIS flow of P, 80x45
  here), then `DXGI_ERROR_DEVICE_REMOVED` (`[gpu-failure] ... alive=false removedReason=0x887A0005`) and the engine stops.
  The debug layer (installed, on) reported nothing; DRED had no breadcrumbs. The flow had never run before on any machine
  we can see (the WARP `predict` test supplies its flow). Whether it is WARP's slowness with 552 dispatches (several use
  FP64) or a fault is not decided here; it is the first thing the mode would run on his PC once it is allowed to enter.
- **WARP is too slow for his card path with Smoothness Mode:** at 30.00 Hz without repeats, Smoothness Mode steps down to
  2X (`reason=step-down-rejected-pairs`: admission rejects pairs, the graph's work before VFG is 8-12 ms on WARP,
  `queuedToProducer`), and with Live HUD Lock the first generated frame takes 14 ms there. Without Smoothness Mode
  (`diagPlain`) the same path keeps up (I, I, B, wait 22.2 ms, 2.0 per real frame). So the owner runs reproduce his
  refusal but cannot yet show his 3X timing; the 59.94 Hz runs can.
