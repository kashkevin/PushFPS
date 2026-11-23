# Frame-generation smoothness: root-cause analysis from the code (stream "smooth", 21 Nov 2026)

Scope: a 1080p60 PS5 stream, NVIDIA VFG, a 2560x1440 180 Hz G-SYNC-compatible OLED. Everything below is CODE READING
of exp/rel-8c (Package 8C1). Nothing here was measured: there is no GPU, no Windows and no PS5 in this environment.
The only figure with a hardware origin is quoted from a code comment and labelled as such. Each candidate names the
code, what it predicts, and the log line that proves or refutes it on the owner's PC.

## The path one 60 fps 1080p frame takes (as the code is written)

1. Decode (D3D12VA, 1920x1080) -> engine loop `EngineController::run` (one pair per decoded frame).
2. Content rate: `pushContentFps` = `activeSource->info().averageFps` (the REQUESTED stream rate, 60, for a PS5
   stream) unless duplicate removal is active (content mode Auto/30/50/60), then `ContentDedup::contentFps()` (measured).
3. Enhancement graph at the WORK size: `ResolutionPlan::make` sets `fg = output = base = SR output`. With SR on, the SR
   output is the SR target box (`EnhancementSettings::srTarget`, default `SrTarget::Uhd4K` = 3840x2160) unless an RTX
   Video SR supersample shrink (`srOutput`) is set; DLSS SR (quality id 0) cannot use the shrink
   (`runningSrOutput`, GraphDescription.h). `EnhanceGraph::initVfg` gives VFG `workW_ x workH_`: **VFG, its own optical
   flow, every generated frame and the presenter's input are 3840x2160 whenever SR targets 4K**, on a 1440p panel.
   The planner (PushPipelinePlan.h, `max-fidelity`, and PushProfiles.js) sets `srTarget=4k` with `supersample=true`,
   which there only means "do not reduce the SR target to the display"; no shrink is applied (PushProfiles.js
   `target()` writes `srTargetIndex = 2` and never `srOutput`). The presenter then scales 3840x2160 into the 2560x1440
   swap chain (GpuStage::Blit).
4. Multiplier per pair (`previewFgMultiplier`), in this order:
   - adaptive (`fgAdaptive`, ceiling >= 3): `pushAdaptiveFgMultiplier` (closest on a log scale), 0.5 s gate;
   - otherwise, with an output cap (`OutputRateMode::Custom`; Follow display is turned into Custom = refresh by
     `VideoPresenter::configurePresentation`, so smooth pacing on means a cap): `ceil(cap / content - 1e-6)`, clamped
     to [2, user's multiplier], recomputed EVERY pair with no hysteresis;
   - no cap (smooth pacing off, display sync Automatic): the user's multiplier as is (6X stays 6X).
5. Admission (`admitFg`, ~line 2633): the whole group of a pair is evaluated or the whole pair is SKIPPED
   (`FgRecoveryBudget::admit`: P95 of base + FG cost over 10/20 s against the last generated frame's deadline + 10 ms);
   `FgCostSettling` makes the first two evaluations after a 1 s gap warm-up samples and, when that probe misses, holds
   generation off for 2, 4, then 8 s (`[vfg-probe] retryInMs=`).
6. Presentation: generated frames whose output-cap slot has passed are dropped before Present
   (`cadence.rateSkipsCandidate` -> `generatedDropCap`), late ones too (`generatedDropDeadline`, `generatedDropCapacity`;
   `[fg-drops]` session totals). VRR presents as VSync with a one-deep queue.

## Candidate causes, most likely first

| # | Cause | Code | What it predicts | Proves / refutes on the owner's PC |
|---|---|---|---|---|
| C1 | **4K working size on a 1440p panel.** VFG, its flow and every generated frame run at 3840x2160 (2.25x the pixels of 2560x1440). A code comment records "VFG High 2X at 4K is 16 ms on an RTX 5070" (FgRecoveryBudget.h `phaseAllowance`, field note of 13 Nov, not ours): a 60 fps pair has 16.7 ms, so at 4K even 2X leaves no room for SR, clean-up and the blit, and 3X cannot fit. | `ResolutionPlan::make` (`p.fg=p.output=p.base`), `EnhanceGraph::initVfg(workW_,workH_)`, `EnhancementSettings::srTarget` default, `max-fidelity` profile | GPU near saturation; admission rejects whole pairs at random P95 peaks; FgCostSettling holds of 2-8 s with no generation at all ("displayed FPS drops" that come and go) | NEW `[slipstream-smooth] work=3840x2160 display=2560x1440 gpuFgP95Ms= gpuSpanP95Ms= frameBudgetMs=` (deliverable 2); existing `[video-sr] selected ... output=3840x2160`, `[live-fg-admission] rejectedPairs=`, `[vfg-probe] retryInMs=`. Refuted if `work=` is 2560x1440 or `gpuSpanP95Ms` stays well under `frameBudgetMs` |
| C2 | **The cap rounds UP across a measured rate.** With duplicate removal on, the content rate is measured (59.94 on the PS5 stream), so `ceil(179.95 / 59.94) = 4`: 4X (240/s) is generated for a 180 cap and a quarter is thrown away at present time (uneven holds, 50 % more generation work than 3X). The rate wobbles around 59.98 (= 179.95/3), so the choice flips between 3X and 4X pair by pair: no gate on this path. With duplicate removal off the requested 60 is used and 3X results. | EngineController ~2569 (`needed=unsigned(std::ceil(cap/sourceFps-1e-6))`) | `chosen` changes between 3X and 4X within seconds; `[fg-drops] cap=` grows by about 60/s while 4X | NEW `[slipstream-smooth] chosen= changes= capDropFps= contentFps=`; existing `[fg-drops] cap=` deltas. Refuted if `changes=0` and `capDropFps` about 0 |
| C3 | **No cap = 6X on a 180 Hz panel.** Smooth pacing off (Automatic display sync, output rate Off) keeps the user's 6X: 360 frames/s for a 180 Hz panel; Automatic's Present(0,0) replaces unshown frames (QmlPlayerBridge comment on smooth pacing). At 4K, 6X cannot be admitted at all for long. | EngineController ~2569 (reduction only with Custom); `VideoPresenter::configurePresentation` | `submittedFps` far above `refreshHz`, high GPU, most pairs rejected | `[slipstream-cadence] submittedFps refreshHz`, `[pacing] effective=` |
| C4 | **All-or-nothing admission + cold-probe hold.** A pair that does not fit is skipped whole: at 180 Hz that is 2 refreshes without a new picture (a visible stutter), then after 1 s idle the next evaluations are cold (comment: up to 1.7x) and a missed probe stops generation for 2, 4, 8 s. One slow sample can therefore cost seconds of 60 fps. | `FgRecoveryBudget::admit`, `FgCostSettling::observe` | Bursts of rejected pairs followed by `[vfg-probe]` lines; smoothness "comes and goes" in multi-second blocks | `[vfg-probe] retryInMs=`, `[live-fg-admission] rejectedPairs=`; NEW `[slipstream-smooth] rejectedPairs= holdMs= stepDowns= reason=` |
| C5 | **Duplicate removal decides how smooth it looks** (the owner's "smoother with fewer real and more generated frames"). A 30 fps game in a 60 fps stream: with duplicates removed, VFG interpolates between DIFFERENT pictures 33 ms apart (real 30/s, generated 150/s at 6X): even motion. When the cadence classifier is unsure it keeps the frame (`StreamCadence`, "without a confident answer there is no number"), pairs of identical pictures reach VFG, generated frames repeat the same picture and the motion judders at 30 with 60 real frames counted. Fewer real frames reported is the SYMPTOM of the good state, not a cause. | `PushStreamDedup` + `remoteplay::StreamCadence` (VEYRA_EXP_RP_STREAM_DEDUP), `ContentDedup` | Smooth periods coincide with `sourceFps` about 30 and `repeatsRemoved` growing; judder with `keptUnsure` growing | `[remoteplay-cadence] state= sourceFps= repeatsRemoved= keptUnsure= locks= unlocks=` against the owner's by-eye notes |
| C6 | **Fixed 120 on a 180 Hz panel without VRR** (2X): holds alternate 1 and 2 refreshes (3:2). Covered by the present stream (S5_PRESENT_API.md comparison D). | `PushCadence.h` model | `fixedUnevenPct` about 50 with `vrr=0` | `[slipstream-cadence] vrr= fixedUnevenPct=` |
| C7 | Governor / GPU budget step-downs (`VEYRA_EXP_RP_GOVERNOR=apply`, `VEYRA_EXP_RP_GPU_BUDGET`, Automatic mode `step-down-gpu-busy`) answer a saturated GPU by lowering the multiplier or tier; they are a CONSEQUENCE of C1-C3. Which of them wrote the owner's "scheduling step-downs" is not known from his message. | PushGovernorControl.h, PushSlipstream.cpp, PushPerformanceMode.h | Lines appear only after C1-C3 conditions | `[slipstream-governor] action=`, `[slipstream-gpu-budget] reason=`, `[push-mode] automatic reason=` |
| C8 | Model reloads: a multiplier change per pair rebuilds nothing (the graph is built for the user's multiplier). A settings change, a hard reset (seek, resize, device loss, a profile applied) rebuilds the graph and VFG (`initVfg`) and resets the budget (`fgBudget.reset()`), whose first samples are warm-up. Not expected while nothing changes. | EngineController `fgBudgetRevision`, `requiresGraphRebuild` | `[reset-lifecycle]` lines during play | `[reset-lifecycle] reason= totalMs=` |
| C9 | CPU-GPU waits: with VEYRA_EXP_RP_STREAM_DEDUP the engine thread waits up to 4 ms for the luma measurement (`measureStreamChange`, never past the next presentation); otherwise the engine waits on fences by event. Queue depth stays one (VSync/VRR) or the latency waiter. No unbounded queue was found. | EngineController ~1196 | `measureWaitMaxMs` near 4 | `[remoteplay-cadence] measureWaitMeanMs= measureWaitMaxMs=` |

## Further causes found in review (code reading)

- **3X x 60 = 180 against a 179.95 Hz VRR ceiling.** Inside VRR the panel cannot refresh faster than 179.95 Hz; 180
  presents a second queue up behind VSync (back-pressure on the one-deep queue) and the cap drops about one frame every
  20 s (180 - 179.95 = 0.05/s). Harmless by itself; visible as a periodic `dropCapFps` blip, not as unevenness.
- **VFG's faster model as a quality cause.** `vfg-fast` runs pairs with the next faster VFG model when the chosen one
  would miss the refresh or the budget (`fastPairsFps`, `[vfg-fast]`): the motion stays even but those generated frames
  may look softer, one possible source of the "smeared" look under load.
- **Adaptive overshoot.** `pushAdaptiveFgMultiplier` takes the closest output on a log scale, so it can choose ABOVE the
  target: a 50 FPS game at a 180 target gets 4X = 200 (log 200/180 = 0.105 < log 180/150 = 0.18), and the cap then
  throws away 20 frames a second. Smoothness Mode never goes above the target.
- **STEADY's cost.** With `VEYRA_EXP_RP_SMOOTH_STEADY=1` a series of probes can run twice the warm-up groups (bounded:
  the second miss in a row holds as before).

## Why "120 submitted" is not "120 shown"

`[slipstream-cadence]` counts Present CALLS. With Automatic display sync, unshown presents are replaced; with VSync/VRR a
present above the refresh waits. Only PresentMon (or the cadence line's fixed-refresh MODEL when `vrr=0`) says what
reached the panel; `[slipstream-smooth]` adds the frames thrown away BEFORE Present (cap, deadline, capacity), which the
cadence line cannot see.

## Expected effect of the fixes (reasoned, not measured)

- C1: `VEYRA_EXP_RP_SMOOTH_SR_DISPLAY=1` evaluates SR into the smallest target that covers the monitor (1440p here): VFG and
  every generated frame at 2560x1440, about 44 % of the 4K pixels. Picture: DLSS renders 1440p directly instead of 4K
  scaled down by the presenter; whether the owner sees a difference is an A/B question (kit arms below).
- C2: `VEYRA_EXP_RP_SMOOTH_CAP_FIT=1` picks the multiplier with the same 2 % fit tolerance as the display hint, plus the
  adaptive 0.5 s gate: 59.94 x 3 = 179.8 counts as reaching a 179.95 cap, so 3X, steadily.
- C4: `VEYRA_EXP_RP_SMOOTH_STEADY=1` keeps one slow probe from stopping generation for seconds (see the switch section).
- C3/C5/C6 are configuration: Smoothness Mode (deliverable 4) picks a multiplier whose output divides into the displayed
  target, and the recommended settings below avoid the uncapped 6X.

## Instrumentation: `[slipstream-smooth]` (contract)

Written with `VEYRA_EXP_RP_STATS=1` only, by the engine thread, at the first processed pair after its window is 5 s old
(nothing playing: no line). A window in which the backend, the user's multiplier, the source size or the working size
changed is dropped without a line. It extends `[slipstream-cadence]` (what was PRESENTED, Present-call spacing,
P50/P95/P99) with what happened BEFORE Present; it does not repeat cadence fields. Unknown values are -1.

| Field | Meaning |
|---|---|
| `window` | seconds |
| `backend` `ceiling` | frame generation method (`none` when off) and the user's multiplier |
| `chosen` `chosenMin` `chosenMax` `changes` | the multiplier used by the last pair, the lowest / highest in the window, how often it changed |
| `why` | what chose the last multiplier: `user`, `cap` (output-cap reduction), `cap-fit` (the switch), `adaptive`, `smooth` (Smoothness Mode) |
| `contentFps` `capFps` `refreshHz` `vrr` | mean content rate the choices used; output cap in force (Follow display = refresh); refresh; `pushVrrCadenceState` |
| `source` `work` `flow` `display` | decoded size; WORKING size = SR output = what frame generation and the presenter process; the graph's source-space flow (VFG computes its own flow at `work`); the monitor (0 unless a smooth switch or the statistics asked) |
| `srTarget` `srTargetAsked` | SR target in force / saved (pipeline::SrTarget: 0 1440p, 1 4K, ...; -1 SR off) |
| `receivedFps` | frames into the loop (with duplicate removal: frames it looked at; without: = `uniqueFps`) |
| `uniqueFps` | pairs processed per second (unique pictures only with duplicate removal on; without it every decoded frame is a pair) |
| `evaluatedPairsFps` `rejectedPairsFps` `fastPairsFps` | admission: groups generated / pairs skipped whole / pairs run with VFG's faster model |
| `generatedFps` `invalidFps` | generated frames that came out valid / invalid |
| `dropCapFps` `dropDeadlineFps` `dropCapacityFps` | generated frames thrown away before Present: cap slot passed / deadline passed / no present capacity |
| `resets` `holds` | temporal history resets; FgCostSettling holds started (generation off 2-8 s) |
| `gpu<Stage>P50Ms` `gpu<Stage>P95Ms` | GPU time per stage from the existing timestamps: Color, Clean, Sr, Nr (layers summed), Flow, Fg (whole group), Blit, Span (the whole graph) |
| `gpuFrames` | graph records in the window |
| `pairBudgetMs` | 1000 / contentFps |
| `gpuLoadPct` | MODEL: sum of graph spans / window (overlapping work counts twice); not the driver's utilisation |
| `smooth` `smoothReason` `stepDowns` `stepUps` | Smoothness Mode target (0 off) and state |
| `srFollow` `capFit` `steady` | the switches |

Decode time is in `[slipstream-latency] arrivalToDecodedP50/P95`, presented spacing in `[slipstream-cadence]`, what the
panel showed only in PresentMon (the kit's `presentmon.py`). Decision: C1 holds when `work=3840x2160` with
`display=2560x1440` and `gpuSpanP95Ms` near or above `pairBudgetMs`; C2 when `why=cap`, `changes>0` and `dropCapFps`
about 60; C4 when `holds>0` or `rejectedPairsFps` spikes while `gpuSpanP95Ms` is otherwise under budget.

## Switches (all off by default; docs/PS5_REMOTE_PLAY_EXPERIMENTS.md "Frame-generation smoothness")

`VEYRA_EXP_RP_SMOOTH_SR_DISPLAY=1` (C1), `VEYRA_EXP_RP_SMOOTH_CAP_FIT=1` (C2), `VEYRA_EXP_RP_SMOOTH_STEADY=1` (C4),
`VEYRA_EXP_RP_SMOOTH_TARGET=60|90|120|180` (Smoothness Mode for the kit; users: the quick setup row, pref
`pushSmoothTarget`). Not done, with the reason: keeping VFG warm across interruptions (a multiplier change already
rebuilds nothing; what reloads VFG is a graph rebuild on a settings change or a hard reset, where keeping the old
feature would mean a second VFG instance in video memory: not worth the risk blind); recovery after a temporary FG
failure (FgCostSettling already retries; a backend that fails initialisation needs Windows to study).

## Smoothness Mode

Preference `pushSmoothTarget` (0 off = the user's fixed configuration for A/B; 60 / 90 / 120 / 180). The user's
multiplier stays the ceiling and is never changed; the mode only picks the multiplier per pair, like adaptive frame
generation does, so turning it on or off rebuilds nothing. Choice: the largest offered multiplier whose output is at or
under min(target, cap, refresh) x 1.02; when the content rate asks for more again, after a 0.5 s gate. Step down one
level after 1 s of predicted load over 0.92 (base + the generation group's cost at that level, kept per level with a
fixed flow part and a per-frame part, + blit, over the pair interval) or over 10 % of pairs rejected; that GPU limit is
lifted after 5 s of predicted load under 0.75 at the level above (5 s without trouble when no cost is measured) and 10 s
after the last step down, or when the content rate changes; never below 2X, never off. It AIMS at the target: it reads
no presentation data (what was shown is `[slipstream-cadence]` / PresentMon), and the engine's budget can still skip
pairs and FgCostSettling hold generation unless STEADY is set. Every change is logged with its
reason, and the quick setup row shows the reason while the multiplier is below what the target asks for. It never adds
a queue or a wait: it only lowers the work per pair.

## Test procedure (kit session `smoothness`, Run-First-Test.cmd -Session smoothness, about 36 minutes)

Existing arms: `sm-native60`, `sm-fg2-vrr`, `sm-fg2-novrr`, `sm-fg3`, `sm-fg2-repair`, `sm-adaptive`. New:
`sm-sr4k-fg3` (DLSS to 4K + VFG 3x), `sm-sr-display` (the same with SR_DISPLAY), `sm-fg6` (4K + 6x, the reported setup),
`sm-fg6-capfit`, `sm-mode-120` / `sm-mode-180` (mode 120 / 180, ceiling 6x), `sm-mode-180-1440` (mode 180 + SR_DISPLAY + STEADY). Judge on
presented spacing (`[slipstream-cadence] intervalP95Ms intervalP99Ms offPct`, PresentMon where available) and by eye,
never on submitted FPS; the `[slipstream-smooth]` rows say why. Fixed 60 / 120 / 180 are `sm-native60`, `sm-fg2-*`,
`sm-fg3`; a fixed 90 exists only for a 30 fps game (3x). VRR off: the `*-novrr` arms or G-SYNC off in the driver.

## Recommended settings (expectations from the code, to be confirmed on the owner's PC)

For a 60 fps game on the 2560x1440 180 Hz panel:
- **180 FPS**: VFG 3X (not 6X: the panel shows at most 180, and with a cap the 6X request runs 3X or 4X anyway), smooth
  pacing on, G-SYNC on, DLSS upscaling targeting 1440p (or 4K with `VEYRA_EXP_RP_SMOOTH_SR_DISPLAY=1`). 60 x 3 = 180 is
  even with or without VRR. Expected to hold only if `[slipstream-smooth] gpuSpanP95Ms` stays clearly under
  `pairBudgetMs` (16.7 ms) with no `holds`; with the 4K target that is not expected (C1).
- **120 FPS**: VFG 2X with G-SYNC working (`vrr=1`); without VRR 120 on 180 Hz alternates 1 and 2 refreshes (uneven:
  set Windows to 120 Hz instead). Half the generation work of 3X; the choice when 180 shows `rejectedPairsFps` or
  `holds`.
- If 180 cannot be held, 120 steady is the better choice than 180 with skipped pairs (each skipped pair is a 2-refresh
  hole at 180 Hz). Smoothness Mode with target 180 does that step by itself and says why.
- Picture: keep the upscaler and the clean-up; their cost is `gpuSrP95Ms` / `gpuCleanP95Ms` in the same line, so the
  owner's run says which stage costs what. Lowering the SR target from 4K to 1440p changes how the picture is made
  (DLSS renders 1440p instead of 4K scaled down by the presenter); whether it looks different is the A/B of
  `sm-sr4k-fg3` against `sm-sr-display`.
- Latency: graph frame generation shows each real frame about one content interval later (16.7 ms at 60 fps,
  `pushFgPairDelayMs`) whatever the multiplier; a higher multiplier adds GPU time, not interval. Nothing here adds a
  queue.

For a 30 fps game (GTA in a 60 fps stream) duplicate removal must work first (C5): 6X for 180, 4X for 120, both far
heavier per second than the 60 fps cases because each pair carries more generated frames.

## Open questions only the owner's PC can answer

1. `work=` and `gpuSpanP95Ms` in his current setup (is C1 the limit?).
2. Which line wrote "scheduling step-downs" (`[slipstream-governor]`, `[slipstream-gpu-budget]`, `[push-mode]`,
   `[vfg-probe]`)?
3. Whether duplicate removal (content mode, `VEYRA_EXP_RP_STREAM_DEDUP`) was on when it looked smoothest (C5).
4. Whether DLSS to 1440p looks as clear as DLSS to 4K scaled down, on his panel (`sm-sr4k-fg3` against `sm-sr-display`).
5. The per-stage GPU costs, which decide the best quality/performance combination.
