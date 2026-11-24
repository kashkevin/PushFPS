# The GPU half of the "one predicted" schedule (stream "predict", 24 Nov 2026)

Scope: the orchestrator's task after the owner's message of 23 Nov 23:39 ET (90 FPS at about 30 ms, and he wants less
delay). Schedule design and its simulation: `docs/drafts/FG_SCHEDULING_NEXT.md` sections 2-3 (stream lagfix). Earlier
offline research: `docs/PREDICTIVE_FRAME_GENERATION_RESEARCH.md` (branch of the predictive-fg stream). Base: exp/s6-gpupath.
Branch: exp/s7-predict.

Labels: **code** (read or written here), **tested** (unit on Linux and MSVC; WARP in CI), **built** (CI's MSVC),
**estimate** (arithmetic with a stated assumption), **hardware-verified** (nothing). No picture-quality claim is made
anywhere in this file.

Everything is OFF unless `VEYRA_EXP_RP_FG_SCHEDULE=onepredicted` is in the environment (never from slipstream.ini). Unset,
the graph creates nothing, records nothing extra and runs every pair exactly as before (section 8).

## 1. What a predicted pair is

Per content pair (A, B) at 3X, in display order: **I** = VFG's frame at t = 2/3 between A and B; **B**; **P** = B advanced by
1/3 of the A -> B motion. Today: I(1/3), I(2/3), B. B is shown one output slot (11.1 ms at a 30 FPS game) earlier
(simulated, stream lagfix); one shown frame in three is a prediction.

## 2. Design choices

### 2a. VFG for one frame at t = 2/3 (code)

VFG's API (as `VfgBackend::cudaJob` drives it) takes `FrameMultiplier` N and `FrameIndex` k and produces the frame at
k/N; the pair's two inputs are bound once per pair (the first subframe of a new pair rebinds; `capture()` bumps a
revision, so a pair whose first call is k = 2 rebinds too). Asking for k = 2 of N = 3 alone is exactly the call today's
second subframe makes: **same effect, same multiplier, no rebuild, no new inference configuration**. What is not known
without his GPU: whether the SDK does work for k = 1 internally anyway (that decides how much of today's second Run's
cost this saves; `gpuFg1P50Ms` of a predicted pair against today's will tell). Mixed tiers pick their effect for k = 2
as today (`vfgPrimarySubframe`).

### 2b. Source of motion (code; cost estimate)

- **VFG pairs have no optical flow today.** `motionUsesFlow` is false for VFG, AMD FRC and PushFlow, and
  `EnhanceGraph::initNvof` skips the flow session when nothing consumes it ("keep the zero-motion and VFG paths free of
  an unused optical-flow session"). In his setup (capture, VFG, NR off) no flow runs.
- **The graph's existing flow** (NVOF, FidelityFX or GPU DIS, chosen by settings) runs in the BASE list on the source
  frames before SR and NR, followed by densify / adapt passes. Turning it on for VFG would put the whole flow in front
  of VFG's Run, i.e. in front of the first shown frame (the critical path), and tie the predicted frame to whichever
  backend the settings pick. Rejected.
- **Chosen: our own flow AFTER the interpolated frame**, on the enhanced A and B exactly as VFG holds them (its
  pitch-linear input buffers; no copy, B was already copied there for VFG). Both are box-filtered to a quarter size
  (3840x2160 -> 960x540, `pushPredictFlowExtent`) into BGRA8 and fed to `GpuDisOpticalFlow` (the repository's GPU DIS,
  bidirectional, vendor neutral): it returns the B -> A vectors already multiplied by a per-pixel trust that combines
  forward/backward consistency and photometric agreement. This list starts after I's copy back, so it delays nothing
  shown before B, and its result is needed only one slot after B.
- Cost (estimate, nothing measured): downsample reads 2 x 33 MB (about 0.2 ms at an assumed 400 GB/s); DIS at 960x540
  is unknown on his card (the repository has no GPU timing of it; the research doc's CPU timings say nothing about GPU
  cost): assumed 0.5-2 ms.

### 2c. The warp (code)

Chosen: **backward gather with a hard per-pixel fallback**, P(y) = B(y + m(y)/3) with m the B -> A flow at y.
- Picture risk first: a forward splat moves silhouettes correctly but leaves holes behind moving objects that must be
  invented (filled from the background's motion, which samples the object itself in B: a smear, or a ghost); the
  backward gather never invents a pixel. Its own error is at moving edges (the flow at y belongs to what B shows at y, so
  a leading edge does not advance and a trailing edge drags). Those are exactly the pixels whose flow trust or whose
  photometric check fails, and they become B.
- Fallback is a **selection, never a blend**: the offline research found that blending back to B produces a double image
  (lower PSNR on the fast pan and occlusion sequences). A pixel that falls back is a held pixel.
- Cost second: one dispatch, per output pixel two texel loads for the still test, four bilinear taps of the flow, and
  two bilinear samples (A for the check, B for the result) of 4 texels each; no atomics per pixel (counters by a
  groupshared reduction, one atomic per 256 pixels).

Per-pixel rule (`pushPredictPixelRule`, the shader implements the same):

| Kind | When | Output |
|---|---|---|
| still | max over RGB of abs(B(y) - A(y)) <= 1/1023 (8-bit: exactly equal) | B(y), the texel loaded and stored unchanged: **bit-identical** (HUD, letterbox, still parts) |
| held | the flow's length < 0.25 work pixels per interval | B(y) |
| fallback | length > 1/8 of the width, trust < 0.5, y + m or y + m/3 outside the picture, or abs(B(y) - A(y + m)) > 24/255 in any channel | B(y) |
| warped | otherwise | bilinear B(y + m/3) |

Whole-frame confidence: four counters per frame (still, held, fallback, warped). `pushPredictFallbackShare` = fallback /
(fallback + warped); `pushPredictFrameUsable` (default at most 0.25: an estimate to be set on his PC) is the scheduler's
per-pair verdict.

## 3. The API (code: `include/veyra/pipeline/PushPredict.h`, `EnhanceGraph.h`)

| Call | Who | Meaning |
|---|---|---|
| `graph->setPairSchedule(PushPairSchedule::OnePredicted / Interpolate)` | scheduler, before `process()` of the pair | what it wants for the NEXT pair; it stays until changed |
| `out.predictedPair` | after `process()` | true: the batch is [I, B, P]; false: today's batch |
| `graph->lastPredictRefusal()` | after `process()` | why a requested pair ran today's schedule (`pushPredictRefusalName` for logs) |
| `graph->predictCounters(parity, counters)` | after that pair's `genFenceValue` completed | the predicted frame's four counters |
| `graph->predictAvailable()` | any time | the pass and its flow exist (switch set, VFG at 3X, supported format) |

Batch of a predicted pair (FrameBatch, append order = display order):

| # | kind | subframe | pts (100 ns) | texture (genFrame_ slot) | ready fence |
|---|---|---|---|---|---|
| 0 | Generated (I) | 2 | `FrameBatch::interpolate(a, b, 2, 3)` | parity + 2 (VFG's usual slot for k = 2) | I's copy-back list |
| 1 | Real (B) | 0 | b | videoFrame_[parity] | the base list (presentation wait: its own lists, stream gpupath) |
| 2 | Generated (P), `lease->predicted` | 1 | `pushPredictedPts(a, b, 3)` = b + (b - a)/3 | parity (VFG's slot for k = 1, unused in this pair) | P's list (= `out.genFenceValue`) |

P beyond b is new for the scheduler (today every generated frame lies between a and b). `resolveFrame` treats P as a
generated frame that is valid once its fence completes (it is not a VFG submission).

## 4. Lists and fences of one pair (code)

| Order on the graph queue | Today (3X) | Predicted pair |
|---|---|---|
| 1 | base list (incl. B -> CUDA input copy, shot-change test) -> v0 | same -> v0 |
| 2 | Signal producer p1; Wait completion c1; CUDA Run k = 1 | Signal p1; Wait c1; CUDA Run k = 2 |
| 3 | copy back I(1/3) -> v1 | copy back I(2/3) -> v1 |
| 4 | Signal p2; Wait c2; CUDA Run k = 2 | P list: downsample A, B; DIS flow; warp; counters -> v2 |
| 5 | copy back I(2/3) -> v2 | - |
| Shown first | I(1/3) | I(2/3) |
| Shown last | B | P |

No CPU wait is added. P's list starts only after I's copy back (queue order), so CUDA has finished reading A and B, and
the next pair's base list (which overwrites A's buffer) comes after it on the same queue.

## 5. GPU time per pair against today (ESTIMATE, 4K, nothing measured)

Today: base + 2 VFG Runs + 2 copy backs (about 0.17 ms each). VFG Medium is upstream-measured at +6.6 ms per source
frame at 4K; whether that is one or two Runs was not recorded, so one Run is 3.3-6.6 ms.

Predicted pair: base + 1 VFG Run + 1 copy back + downsample (about 0.2 ms) + DIS at 960x540 (assumed 0.5-2 ms) + warp
(writes 33 MB, reads about 2-3 x 33 MB with cache reuse: about 0.3-0.8 ms) = **today minus one Run (3.3-6.6 ms) plus
about 1-3 ms**. Expected about equal or lower GPU time per pair; the first shown frame's readiness is unchanged
(same base, one Run). His `gpuFg1P50Ms` (I) and `gpuFg2P50Ms` (P: flow + warp; the GPU timer's Fg2 stage now brackets
P's list) decide it.

## 6. Memory (estimate from the allocations in `PushPredictPass`)

No new full-size texture: P uses a generated-frame slot that exists at 3X. New: 2 slots x 2 BGRA8 flow pictures at
960x540 (8.3 MB), 2 x RG16F flow (4.1 MB), 2 x R8 trust (1 MB), counters (96 bytes), GPU DIS's own pyramids and
buffers for 960x540 (not counted here; a few MB). About 15-25 MB in all, only with the switch.

## 7. Edge cases (code)

| Case | What happens |
|---|---|
| Switch unset | `initPredict` returns before creating anything (`pushPredictCreate`, unit-tested over every combination); the refusal is SwitchOff; the generation loop is the unchanged one |
| Switch set, not VFG / not 3X / scRGB FP16 (VFG has no FP16) / present-sink FG / HUD Lock / exclusion | the pass is not created, or the pair is refused before anything is recorded; today's pair |
| Pass or flow creation fails | logged once per graph; every pair is refused (pass-unavailable); the graph keeps working |
| History reset (start, seek, settings change, a CPU scene cut) | `resetFg`: refused (no-history); today's reset pair |
| Shot change seen only by VFG's GPU test | I shows B (VFG's predicated copy, as today); P's flow across a cut has low trust, so P falls back to B almost everywhere and the counters report it; the scheduler should fall back for the next pairs (its hysteresis) |
| Device reset / graph rebuild | the pass is recreated with the graph (it is a graph member); no state survives |
| HDR10 (R10G10B10A2, PQ) | same passes; the still test is one 10-bit code; the photometric test works on PQ codes (perceptual); WARP-tested |
| A pair the scheduler later skips or expires | P is an ordinary generated lease: unshown, its slot is released as today |

## 8. What the scheduling side must do (EngineController, owned by stream lagfix; not edited here)

1. Before `graph->process(...)` of a live pair: `graph->setPairSchedule(choice)`; choice = OnePredicted only with the
   switch set, its hysteresis calm (enter after N calm pairs, leave at once: FG_SCHEDULING_NEXT.md 3.4), and the last
   finished predicted frame usable (`graph->predictCounters(parity, c)` once that pair's `genFenceValue` completed,
   then `pushPredictFrameUsable(c)`). One line beside the process call.
2. After `process()`: `out.predictedPair` says the batch layout. Deadlines: I at phase - T/3, B at phase, P at phase + T/3
   (today -2T/3, -T/3, 0). P's pts is beyond b; `LivePairLatency::observe` and expiry treat it as generated.
3. Admission: a predicted pair's group cost is one VFG Run + P (the GPU timer's Fg1 and Fg2), not two Runs.
4. Log the refusal reason on changes (`pushPredictRefusalName`), count predicted pairs (`Metrics::predictedPairs`).

## 9. What only his GPU and eyes can decide; what to measure first

1. **Cost**: `gpuFg1P50/P95Ms` and `gpuFg2P50/P95Ms` of predicted pairs against today's pairs, same scene, 4K, 3X
   (the flow at 960x540 and the warp at 4K have never run on a real GPU).
2. **How often P is mostly held**: the fallback share over a session (counters), per scene type.
3. **The look** (priority 3 decides, by eye, in motion): a predicted frame when motion starts, stops or reverses (it
   overshoots for one slot, 11 ms), at the edges of moving objects (seams between moved and held pixels), on HUDs (should
   be identical), on fast pans and particles. A recorded clip of these moments, today's schedule against this one.
4. **Delay**: the real frame's capture-to-present against today's at identical settings (the simulated 11.1 ms).

## 10. Tests

| Test | What it checks | Where it ran |
|---|---|---|
| `tests/remoteplay/PredictTests.inc` (8) | the switch, every refusal, creation only with the switch (all combinations), the plan, timestamps, flow size, the pixel rule, confidence | Linux g++ -Werror (strict warnings) in a standalone driver; CI (MSVC /W4, g++, sanitizers) through ck |
| WARP `predict`, `predict-hdr10` (`tests/integration/PushGpuTests.cpp`) | zero motion: P == B bit for bit; still picture: P == B, all still; 3 px translation: P == B moved 1 px bit for bit, edge columns fall back, exact counters; static HUD bit-identical over a moving background with the background's flow over it; low trust and a wrong flow: every pixel B, frame unusable; RGBA8 and RGB10A2; no debug-layer error; an uninitialised pass holds nothing | CI's Windows build only (not run here) |
| Shaders | `PushPredict.hlsl`, `PushPredictDownsample.hlsl` | glslangValidator's HLSL front end here (not DXC); DXC in CI's full build |

Not tested anywhere: VFG with k = 2 alone (CUDA does not exist on WARP), the DIS flow inside the graph, the graph's
predicted pair end to end (no engine caller yet).
