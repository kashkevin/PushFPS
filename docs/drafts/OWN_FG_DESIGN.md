# PushWarp: a vendor-neutral frame generator of our own, and which schedule architecture to build (24 Nov 2026)

Working name "PushWarp" (no NVIDIA marks). Branch `exp/s9-ownfg`. This is a DESIGN: nothing here is built into the app.

Labels used throughout:
- **MEASURED ON HIS PC**: the owner's own log of 24 Nov 2026 (`P15_REAL_LOG_ANALYSIS.md`, software timers, not photons).
- **OFFLINE PICTURE MEASUREMENT**: `tools/ownfg_eval/RESULTS.md`, CPU, synthetic 4K material (not game footage).
- **CODE**: read in this branch. **ESTIMATE**: arithmetic with a stated assumption; never a measurement.
- Nothing is hardware-verified. **No delay readout is projected for his PC**: section 1 adds measured parts on the
  output grid; the arithmetic is not a prediction of what his readout will show.

## 0. What changed with his log, and what that means for this design

1. NVIDIA VFG's first generated frame costs about **2.0 ms at 3840x2160 HDR10** on his card (P95 2.3-3.3, P99 2.5-3.5;
   the second ready 4.2-4.5 ms after the pair starts). MEASURED ON HIS PC. The 6.6 ms figure earlier research used was
   wrong for his card. **An own interpolator cannot win delay by being cheaper than NVIDIA's for the interpolated
   frame**: section 5's cost estimate for our I(2/3) on the critical path is of the same order (about 0.8-2.5 ms,
   ESTIMATE), i.e. no meaningful difference. That argument is dropped.
2. His readout splits into picture 4.7 + first generated frame 2.0 + **schedule wait 22.1** + margin 2.2 ms (MEASURED).
   The schedule wait is the bottleneck. Package 15's mode, which halves it, never ran on his PC: it refuses with
   Smoothness Mode and with Live HUD Lock, and he uses both. **Any architecture below must run with Smoothness Mode's
   target, Live HUD Lock and exclusion boxes, in HDR10 (PQ, R10G10B10A2) at 3840x2160 from a 30 Hz capture.**
3. What is ours and unmeasured is the **predicted frame P**, and in Package 15 its flow is the repository's strict GPU
   DIS (third_party/gpu-dis), which requires 64-bit floating-point shader support and records about **1010 dispatches**
   (with a UAV barrier after most) per bidirectional 960x540 pair (CODE: counted from `native_strict_dis_provider.cpp`,
   section 4.1). It has never been timed anywhere. That is the biggest cost unknown of every own-generator design.

So the value of PushWarp is not speed. It is: (i) vendor neutrality (any D3D12 card, no CUDA interop); (ii) no
dependence on NVIDIA accepting an unusual request (FrameIndex 2 alone, Package 15); (iii) one motion field per pair
shared by every generated frame of the pair, so the predicted frame needs no second flow; (iv) full control of HUD,
HDR and fallback behaviour per region; (v) the only route to the fully causal schedule, which no NVIDIA API offers.

## 1. The architectures, compared

Grid arithmetic: 30 FPS content, 3X, one output slot = 11.1 ms. A real frame B can be shown only after every frame of
its pair that is displayed BEFORE it. Parts below are his MEASURED means (picture 4.7, VFG first frame 2.0, margin
2.2); "own I" uses the ESTIMATE of section 5 (0.8-2.5 ms). The sums are arithmetic on parts, not projections of his
readout; margins in a new schedule are unknown until it runs. **NOT A PROJECTION (review 2, SHOULD 8d): never quote these
sums to him as what a schedule will give; the picture numbers in this section are the s9 study's and are superseded by
§9.5 (the implemented scheme).**

| | Display order per pair | Real / interpolated / predicted per pair | Wait before B | Arithmetic: picture + first frame + wait + margin | Depends on | What breaks stability |
|---|---|---|---|---|---|---|
| (a) today | I(1/3), I(2/3), B | 1 / 2 / 0 | 2 slots, 22.2 | 4.7 + 2.0 + 22.1 + 2.2 = **31.0** (his readout 30.7-31.5) | NVIDIA VFG | nothing new (his on-grid share 95-96 % today) |
| (b) hybrid | I(2/3), B, P(1/3) | 1 / 1 / 1 | 1 slot, 11.1 | 4.7 + 2.0 + 11.1 + 2.2 = **20.0** | VFG accepting FrameIndex 2 alone; our P | leaving / entering the schedule moves B by 11.1 ms each time (Package 15's StayIn makes it rare); a P that is mostly held is a visible hold |
| (c) all-own, same order | I(2/3), B, P(1/3) | 1 / 1 / 1 | 1 slot | 4.7 + (0.8-2.5 own I, ESTIMATE) + 11.1 + 2.2 = **18.8-20.5** | nothing vendor-specific | as (b); plus our I's own failures (shown, not switched) |
| (d) fully causal | B, P(1/3), P(2/3) | 1 / 0 / 2 | 0 | 4.7 + 0 + 0 + 2.2 = **6.9** (B no longer waits for any generated frame) | our P only | must NEVER fall back to interpolation: that moves B by 22.2 ms; so P(2/3) is always shown, held per region where untrusted |
| (e) causal + late correction | B, P(1/3), X | see text | 0 | as (d) | | see text |

(e) was examined and does not exist as a distinct option on this grid. The third frame of a causal pair is due 22.2 ms
after B is shown; the next real frame C arrives 33.4 ms after B arrived, i.e. at the earliest about 11 ms - picture
after X's deadline. No interpolation between B and C can be ready for X without delaying X, which is schedule (b)/(c)
again. What does exist, and is part of (d): X = P(2/3) with a **per-region** choice at generation time between the
prediction and a hold (the regions whose motion is untrusted show P(1/3)'s or B's pixels). That keeps the delay stable
(B is always first) and moves the risk into the picture, which the harness measures. A per-PAIR choice between (d)
and (b) would move B by 11.1-22.2 ms at each switch: rejected for stability (his priority: consistent delay).

Picture quality per architecture (OFFLINE PICTURE MEASUREMENT, synthetic 4K, `tools/ownfg_eval/RESULTS.md`; NVIDIA's
model cannot run here, so "learned interpolator" = PushFlow's RIFE, NOT NVIDIA's):

| Architecture | Generated frames per pair and their measured stand-ins | Mean PSNR of those frames (dB) | Silhouette band (dB) | Misplacement P95 (px) | Reading |
|---|---|---:|---:|---:|---|
| (a) today | I(1/3), I(2/3) from NVIDIA VFG: **not measurable here**; stand-in learned interpolator RIFE Quality (PushFlow, NOT NVIDIA's) | 30.46 / 30.50 | 24.09 / 24.31 | 22.0 / 22.2 | the reference everything else is compared with; VFG's own quality is unknown here |
| (b) hybrid | I(2/3) NVIDIA (stand-in as above) + P(1/3) Package 15 rule, DIS 1/4 | 30.50 / 27.07 | 24.31 / 20.81 | 22.2 / 32.2 | one frame in three is predicted: -3.4 dB, band -3.5 dB, misplacement x1.5 against the stand-in |
| (c) all-own | I(2/3) own flow reversal, DIS 1/4 + P(1/3) as (b) | 30.26 / 27.07 | 22.99 / 20.81 | 11.3 / 32.2 | I level with the stand-in on average (-0.24 dB, band -1.3 dB, misplacement half), 8.6 dB worse on thin occluders (`crossing`); P as (b) |
| (d) fully causal | P(1/3), P(2/3), Package 15 rule, DIS 1/4 | 27.07 / 25.32 | 20.81 / 20.15 | 32.2 / 46.9 | two frames in three predicted, the second 22 ms ahead: -3.4 and -5.2 dB against the stand-in; 2.1x misplacement |
| (e) causal + per-region fallback | as (d), untrusted regions held | as (d) (the hold is part of Package 15's rule) | | | identical to (d) on this material; not a separate architecture (section 1 text) |

Frames shown per pair in each architecture: (a) R, I, I; (b) I, R, P; (c) I, R, P; (d) R, P, P. On this material the
hold (repeat) baseline is 23.6 / 22.0 dB for P(1/3) / P(2/3) and the plain blend 25.0 dB for I: every candidate above
is far from a repeat. Numbers: RESULTS.md 5.1 (42 frames per slot, 10 synthetic sequences; NVIDIA's model absent).

## 2. Recommendation

Based on his measured parts (section 1) and the offline picture evidence (RESULTS.md), with no delay projected:

1. **Schedule: one predicted frame (I(2/3), B, P(1/3)), never the fully causal one as the default.** The wait drops by
   one slot (22.2 -> 11.1 ms, grid arithmetic). The fully causal schedule would remove the whole wait, but in the
   picture evidence its second predicted frame is 5 dB below an interpolated frame and misplaces content by about
   47 px (P95) at a 30 FPS game's motion: that is a visible loss, against the owner's "no picture loss". It can exist
   only as an explicitly labelled option for him to judge by eye.
2. **Make the predicted schedule run with his settings first** (Smoothness Mode, Live HUD Lock, exclusion boxes,
   HDR10): that is the bottleneck his log shows, independent of who makes I. HUD Lock and boxes compose on P with B as
   the held frame (section 3.2).
3. **Motion for P: the block field of the estimator upstream runs (AMD FidelityFX optical flow), not the strict DIS.**
   The evidence (RESULTS.md 5.4): on 14 frames a FidelityFX-like 8x8 integer block field at 960x540, one direction,
   gives P(1/3) at least as good as the dense DIS with Package 15's backward check (27.53 vs 26.83 dB), because that
   check makes the dense field fall back too often (0.19 vs 0.07 of the moving pixels). No extra trust signal is needed
   for P: Package 15's own photometric test is the trust. The strict DIS costs about 1010 dispatches and FP64 (section
   4.1) and, per the orchestrator, removes the device on its first run on WARP: it should only remain a comparison.
4. **I(2/3): keep NVIDIA's frame where VFG is available (b), offer PushWarp's (c) as the vendor-neutral path.** Our
   own I is level with the learned stand-in on average but clearly worse at thin occluders and on per-frame-changing
   HUD text (RESULTS.md 5.6/5.7); NVIDIA's own quality is unknown here, and its cost on his card is measured (2.0 ms).
   (c) is the route for AMD/Intel cards and for "no dependence on NVIDIA accepting index 2 alone". It can use the same
   one-direction block field (I from B's field only: 29.9-30.1 dB, level with the bidirectional dense field, 30.0).
5. **Do not build acceleration as a default** (mixed: -0.4 dB on average with the gather, +1.8 dB on a turn that starts
   and stops with forward projection). Forward projection of P is level with the gather; keep the gather (it exists
   and is WARP-tested) and keep forward projection as the variant to try if his eyes report dragged leading edges.
6. Before any of this reaches him: the GPU time of the FidelityFX flow at 960x540 + P's passes on his card (GPU
   timestamps, already in the app's timer), and his eyes on recorded clips (comparison clip feature) of P at motion
   starts/stops and thin structures.

## 3. The PushWarp pipeline (3840x2160; SDR RGBA8 or HDR10 RGB10A2 PQ codes)

Inputs are the graph's real frames as they are presented (`videoFrame_[parity]`, `outputFormat()`: R8G8B8A8_UNORM for
SDR, R10G10B10A2_UNORM = PQ codes for HDR10; scRGB FP16 is not served in the first version, as Package 15). No CUDA
buffer, no copy: PushWarp reads the textures the presenter already shows. A is the previous real frame (parity 1 - p),
B the new one (parity p).

### 3.1 Once per pair (when B's picture is final)

| # | Pass | Reads | Writes | Notes |
|---|---|---|---|---|
| W1 | Downsample + luma of B | B (33.2 MB) | luma8 960x540 (0.5 MB), kept for the next pair as "A's luma" | box 4x4 of the codes (PQ codes for HDR10, as Package 15 averages "encoded values as they are"), BT.709 weights, R8. Package 15 downsampled BOTH A and B every pair; A's is reused here |
| W2 | Flow | B (BGRA8 960x540) | B->A at 960x540 (one direction) | **AMD FidelityFX optical flow** (section 4.3, about 25 dispatches, integer 8x8 blocks), densified; own block matching (4.2) where FidelityFX is not built; the strict DIS only as a comparison |
| W3 | Trust | the field, both lumas | v x trust RG16F + trust R8 | for P none beyond Package 15's photometric test; for I optionally the block SAD (RESULTS.md 5.4) |
| W4 | (only if accelerated P is on) keep the previous pair's A->Z field | | | one more RG16F + R8 pair (2.6 MB) per slot, swapped, no pass |

### 3.2 Per generated frame

| Frame | Passes | Reads (full size) | Writes |
|---|---|---|---|
| I(t) (schedules (a)-(c)) | R1 reversal splat at 960x540 (both sides; two passes so integer atomics reproduce the CPU reference's softmax weights exactly up to fixed-point rounding: pass 1 `InterlockedMax` of the quantised importance per target cell, pass 2 `InterlockedAdd` of fixed-point exp(beta (importance - that max)) x bilinear x trust and of the weighted Ft0, Ft1; no float atomics needed) + R2 push-pull fill (about 8 tiny dispatches) -> fields Ft0, Ft1, visibility (2 x RGBA16F 960x540); R3 compose at 4K: still test A,B; fields bilinear; one bilinear gather of A and of B; occlusion-weighted blend; untrusted -> nearer real frame | A, B (66 MB + taps from cache), fields (8 MB, cached) | genFrame slot (33.2 MB) |
| P(s) (s = 1/3, 2/3) | Package 15's gather (`PushPredict.hlsl`, unchanged maths, reading Texture2D instead of VFG's buffers) with step s, or the forward-projected variant (a 960x540 splat + the same gather); verdict pass (`PushPredictVerdict.hlsl`) | A (photometric check), B | genFrame slot |
| every generated frame | Live HUD Lock compose (`PushHudLockCompose.hlsl`), then exclusion boxes (`composeExclusion`): unchanged passes | generated, held real frame, lock state | generated |

**Held real frame for HUD Lock and exclusion** (the change Package 15 lacked): today `composeHudLock(slot, 1 - parity)`
takes HUD pixels from A for frames between A and B. For a frame AFTER B (any P) the held frame must be **B (parity
p)**: the HUD in P must be B's HUD (a counter that changed in B must not jump back to A's value). For I(2/3) in
schedules (b)/(c) it stays A, as today. Exclusion boxes the same (`composeExclusion(slot, p)` for P). With that,
HUD Lock and exclusion are ordinary compose passes on top of PushWarp's frames and the predicted schedule no longer
needs to refuse them (`PushPredictRefusal::HudLockOrExclusion` goes away for PushWarp).

**Smoothness Mode**: Package 15 refused it because Smoothness Mode decides the multiplier and assumes the real frame is
the batch's last (`PushPredictRefusal::SmoothnessMode`). With PushWarp the batch layout is ours: for a 3X pair the
batch is [I(2/3), B, P] or [B, P1, P2]; Smoothness Mode's 2X decision maps to [B, P(1/2)] (causal) or [I(1/2), B]
(today). The engine must learn that the real frame's index in the batch is a property of the schedule
(`FrameBatch` already carries per-frame pts and kinds; Smoothness Mode's code reads "last frame" assumptions in
EngineController: to be listed in the implementation plan, step list in section 10).

### 3.3 Formats and memory (ESTIMATE from sizes)

| Resource | Format | Size |
|---|---|---|
| luma pyramid, 2 frames | R8, 960x540 + 3 levels | about 1.4 MB |
| block vectors per level, both directions | RG16F | < 0.5 MB |
| flow + trust, both directions, 2 slots | RG16F + R8 960x540 | 2 x 2 x 2.6 MB = 10.4 MB |
| reversal fields | 2 x RGBA16F 960x540 | 8.3 MB |
| splat accumulators | R32_UINT x 4 (fixed point) 960x540 x 2 sides | 16.6 MB |
| generated frames | existing genFrame slots | 0 new |
| **total new** | | **about 37 MB** (Package 15's pass: 15-25 MB) |

### 3.4 HDR10

- What the app does today (CODE): the working frames are R10G10B10A2 holding **PQ codes**; Package 15 downsamples,
  measures flow, tests stillness (one 10-bit code) and photometric agreement (24/255 of the code range), and gathers,
  all on PQ codes. PushFlow encodes to sRGB/PQ for the network and decodes after the rebuild (`PushFlowPack.hlsl`,
  `PushFlowReconstruct.hlsl`). HUD Lock has an outputMode 2 = PQ.
- PushWarp: gathers (bilinear taps of one surface) in PQ codes, as Package 15 (an interpolation between neighbouring
  texels; PQ is perceptually uniform, so the error of a sub-pixel tap is spread evenly over brightness). Blends of the
  TWO warped samples (the I compose, the blend weights in occlusion zones): the harness measured both spaces on the
  HDR10 sequences (RESULTS.md, HDR table): blending in PQ codes vs in linear light differs by at most 0.03 dB for the warp-based I (0.3 dB for a plain
  blend), measured in PQ codes on 4 frames per slot: **blend in PQ codes** (cheaper; the same space as the gathers). The order of all
  candidates is the same in HDR10 as in SDR (RESULTS.md 5.3). What the app does with HUD Lock in PQ (outputMode 2) stays.
- Thresholds stay in code units (still = 1 code, photometric 24/255 of the range), as Package 15: in PQ that is about
  the same perceptual step everywhere.

## 4. The flow: what the app has, what it costs, and the cheaper one

### 4.1 The strict GPU DIS (Package 15's flow)

CODE (`third_party/gpu-dis/src/gpu/native_strict_dis_provider.cpp`): a port of OpenCV's CPU DIS that reproduces its
rounding; `initialize` refuses a device without `DoublePrecisionFloatShaderOps` ("Strict DIS CPU rounding requires GPU
double shader operations"). At 960x540, Fast preset: pyramid levels 5..2 (30x16 .. 240x135 working sizes), patch
search as wavefronts of diagonals (62 + 30 + 14 + 6 diagonals, twice: 224 dispatches per direction), densify, then
variational refinement 5 x (1 system + 10 SOR + 1 update) per level, plus image preparation. Counted: **about 488
dispatches per direction, about 1010 for a bidirectional pair**, most of them small and separated by UAV barriers.
ESTIMATE: at an assumed 2-10 us of fixed cost per dispatch + barrier on the GPU front end, that is **2-10 ms per pair
before any arithmetic**, plus 64-bit arithmetic (on consumer GPUs FP64 throughput is a small fraction of FP32: the
exact ratio for his card is not documented in anything this branch has; assume "much slower", unmeasured). It is off
the critical path in schedule (b) (P is needed one slot after B) but it occupies the graph queue for up to 1/3 of the
33 ms budget (ESTIMATE). In (c) and (d) the flow is on the critical path of the first generated frame (c) or of P(1/3)
(d). **It must be timed on his card before anything else** (the GPU timer's Fg2 stage of a Package 15 pair brackets it).

### 4.2 Own pyramid block matching (specified; CPU reference `flow.block_match`): only where FidelityFX is not in the build

Superseded as the first choice by 4.3 (the orchestrator's decision of 25 Nov; the picture evidence agrees). Kept as the
fallback for builds without the FidelityFX SDK.


- Levels: 960x540 luma -> 480x270 -> 240x135 -> 120x67 (2x2 box, R8 / R16F).
- Coarsest: per 8x8 block a full search +-6 (169 candidates; +-48 px at 960x540 = +-192 px at 4K: covers the
  `pan_fast` sequence's 180 px), zero-mean SAD (robust to a uniform brightness change), parabolic sub-pixel step on the
  costs, 3x3 median of the block vectors.
- Each finer level: the up-scaled vector (x2) as the prediction, +-2 refinement (25 candidates), sub-pixel, median.
- Densify: bilinear between block centres to every 960x540 pixel. Then `GpuDisValidate` for the trust, both directions.
- Shader shape: one dispatch per level and direction (one thread group per block, the search window in groupshared
  memory: (8 + 2 x 6)^2 = 400 texels at the coarsest level, 144 at finer ones), one median, one densify: **about 12-16
  dispatches per pair**, all FP32/integer.
- Cost ESTIMATE (unmeasured): finest level 120 x 67 = 8040 blocks x 25 candidates x 64 pixel differences x 2
  directions = 26 M absolute differences; coarsest 15 x 8 blocks x 169 x 64 x 2 = 2.6 M; all levels about 35 M
  operations and about 2 MB of texture reads. Arithmetic is negligible; the estimate is set by dispatch count and
  occupancy (small grids): **0.1-0.5 ms per pair**, under the 1 ms the orchestrator asked for, with margin for a second
  candidate set (e.g. the previous pair's vector as an extra predictor, temporal hint) if quality needs it.
- What it loses against the strict DIS: OFFLINE PICTURE MEASUREMENT in RESULTS.md (rows `*_bm4` vs `*_dis4`, and
  `*_uf4` = DIS without its variational stage): for I(2/3) the block matcher equals DIS in mean PSNR
  (30.26 vs 30.26 dB) but misplaces more (P95 17.9 vs 11.3 px) and smears colour into the gaps of occluders; DIS
  without variational refinement loses 0.07 dB for I and 0.6 dB for P(1/3); at P(2/3) the block matcher misplaces much
  more (P95 74 vs 47 px). The FidelityFX-like block field (4.3) does better than this own block matcher for P.

### 4.3 AMD FidelityFX optical flow as the motion source (the orchestrator's decision, 25 Nov 2026)

CODE (`src/guidance/AmdOpticalFlow.cpp`; SDK 1.1.4 shaders `gpu/opticalflow/*_v5.h`, read): input B8G8R8A8 at the flow
size (`nvofW_ x nvofH_`, optionally halved), output one INTEGER vector per 8x8 block (R16G16 at the input size, e.g.
120x68 for 960x540: 4 px steps at 4K), current -> previous only, plus a scene-change result; internally a 7-level
luminance pyramid, per level an 8x8 block full search of +-8 px around the best of 4 up-scaled predictors (msad4 SAD),
a 3x3 vector median, zero vector preferred at level 0 on a tie; it keeps the previous frame in its context.
Shader count per frame (from the shader files: prepare luma, pyramid, then per level search + filter + scale, scene
change): about 25 dispatches, no FP64. Cost ESTIMATE: 0.2-0.6 ms at 960x540 (unmeasured; vendor-neutral D3D12 compute).
For PushWarp: P uses Package 15's rule on the densified block field with its own photometric check (no further trust
needed, RESULTS.md 5.4); I(2/3) uses flow reversal from this one field (`interp_reversal_one`); the block SAD is the
cheap extra trust for I if one is wanted (+0.24 dB on 14 frames, within noise). Limits: integer vectors (sub-pixel
placement comes only from densification), one direction (no forward/backward check, no visibility from A's side),
+-8 px per level (large motion relies on the pyramid). Where its vectors fail (thin occluders, particles) the P rule
holds B, as with DIS.

### 4.4 Hardware optical flow

NVOF (NVIDIA only, needs the SDK headers at build time) stays an optional second estimator as NVIDIA_STREAMLINE_REFLEX_NVOF.md
proposes; PushWarp must not depend on it (vendor neutrality). Not measured here (no NVIDIA hardware).

## 5. Cost model per pair (ESTIMATE: bytes and fetches at 3840x2160, 4 bytes per pixel in both SDR and HDR10; GPU time
as a range at an assumed 300-450 GB/s effective memory bandwidth; NOT measured on any GPU)

| Pass | DRAM read | DRAM write | Fetches per output pixel | Time range (ESTIMATE) | On whose critical path |
|---|---|---|---|---|---|
| W1 downsample + luma (B only) | 33.2 MB | 0.5 MB | 16 loads per 960x540 pixel | 0.08-0.12 ms | first generated frame (c, d) |
| W2 FidelityFX flow (or own block matching) | about 2-4 MB | < 1 MB | (groupshared) | 0.2-0.6 ms (dispatch-bound) | same |
| W3 validate x 2 | about 6 MB | 5 MB | 4 + 4 bilinear | 0.03-0.05 ms | same |
| R1+R2 reversal splat + fill, per I | about 10 MB | 25 MB (atomics) | 4 atomics per grid pixel per side | 0.1-0.3 ms | I(2/3) (c) |
| R3 I compose at 4K | 66.4 MB (+ cache misses) | 33.2 MB | 2 still loads + 2 field taps (8 texels, cached) + 2 bilinear gathers (8 texels) = 18 | 0.25-0.6 ms (texture-rate bound more than bandwidth) | I(2/3) |
| P gather (Package 15 maths) | 66.4 MB | 33.2 MB | 2 still + 4 flow + 4 trust + 4 (A check) + 4 (B) = 18 | 0.25-0.6 ms | P(1/3) in (d) |
| P verdict + predicated copy (rare) | 0 / 33.2 MB | 0 / 33.2 MB | | 0-0.2 ms | |
| HUD Lock compose (exists today per generated frame) | about 83 MB | 33.2 MB | | 0.25-0.4 ms | every generated frame, as today |

Sums (ESTIMATE): **own I(2/3) critical path W1+W2+W3+R1-R3 = about 0.6-1.6 ms + HUD Lock's compose 0.25-0.4 = 0.8-2.0
ms**, plus dispatch gaps; the section-1 table uses 0.8-2.5 ms to leave room. NVIDIA's first frame on his card: 2.0 ms
MEASURED. **Same order; no delay claim is made from this difference.** Per pair total for (c): about 1.5-3.5 ms
(ESTIMATE) against VFG's batch 6.5 ms P50 (MEASURED, two frames). For (d): W1-W3 + 2 P gathers + 2 HUD Lock composes
= about 1-2.5 ms, none of it before B.

What is not modelled: dispatch overhead and barriers between the small passes (a reason to fuse W1 into the previous
pass and W3 into W2's last dispatch), cache behaviour of the gathers at large motion (a 180 px pan reads B 180 px away
from where it writes: still one pass over B, but less cache reuse across tiles), clock state between bursts.

## 6. Scheduling with the existing machinery (include/veyra/engine/PushLowDelay.h)

- (b)/(c): unchanged from Package 15: deadlines follow each frame's pts (I at phase - T/3, B at phase, P at phase + T/3),
  two learners, StayIn policy, `pushLowDelayAdmissionDeadline` (base + Fg1 against I's deadline, group against P's),
  `pushPredictedDropBeforeNext`. The graph's `setPairSchedule(OnePredicted)` produces [I(2/3), B, P] with PushWarp's I
  instead of VFG k = 2 (no `vfgRefused` path for PushWarp).
- (d): a third schedule `PushPairSchedule::Causal` = [B, P(1/3), P(2/3)]: B's deadline is the phase of a pair whose
  first frame is real (the learner's start: base only), P1 at phase + T/3, P2 at phase + 2T/3; P2 dropped like P when
  it would pass the next pair's B (`pushPredictedDropBeforeNext` generalised to the next pair's first frame). It must
  never leave to interpolation on picture grounds (stability); it leaves only for the reasons that make generation
  impossible (no history, cut, settings), and a cut pair shows [B, B, B]-holds (P = B), not interpolation.
- Interpolation with PushWarp (I(1/3), I(2/3), B) is the same schedule as today with the cheaper generator: useful on
  cards without VFG, and as the safe fallback of (c) when the predicted frame is off.
- Admission: group cost = PushWarp's measured GPU span (its own timestamps: W1-W3, each I, each P).

## 7. Failure and fallback per region; scene cuts

| Situation | I(2/3) | P(s) |
|---|---|---|
| pixel unchanged between A and B (HUD, letterbox) | exact copy | exact copy of B |
| flow untrusted at the sample (trust < 0.5) | the real frame nearer in time at that pixel | B at that pixel (held) |
| the two warped samples disagree by > 24/255 (occlusion) | the side with the larger visibility x time weight, alone (no double image) | (single source) photometric check fails -> held |
| nothing landed there (revealed area) | the other side's sample; if neither: push-pull filled motion, both samples | held B (forward variant); the gather variant has no holes |
| motion longer than 1/8 of the width per interval | as untrusted | held |
| more moving pixels held than moved | (no whole-frame rule for I) | whole P = B (Package 15's verdict) |
| scene cut (CPU detection resets history; GPU shot test) | the pair is not generated: hold (today's reset behaviour) | P = B (predicated copy, as Package 15) |
| flash / fade (brightness change) | zero-mean SAD in the flow keeps vectors; photometric checks fall back where the change is large | same |

## 8. As a frame-generation backend (no change for other users)

- `FrameGenerationBackend::PushWarp = 8` (append; never renumber: the value is stored in preferences).
  `crossVendorFrameGeneration` true, multipliers 2-3 (4 later), `twoXFrameGeneration` false.
- Capability `pushwarp` (`src/ui/bridge/Hardware.cpp`): available when the D3D12 device creates the passes (typed UAV
  stores of R8G8B8A8_UNORM / R10G10B10A2_UNORM, which D3D12 requires for every device; R32_UINT atomics); no runtime
  download, no vendor check (decided by capability only, never by GPU name).
- Label (`src/ui/bridge/FrameGeneration.cpp`): a Chinese source string in `qsTr`/`tr` with catalog entries, e.g.
  "PushWarp · PushFPS 自研运动补偿 · 2X～3X" (wording for the owner to decide; translations per CLAUDE.md rule 2).
- Schedules (b)/(c)/(d) become choices of the existing "Lower delay" row for PushWarp; VFG users keep Package 15's
  behaviour exactly; DLSS-path, PushFlow, FidelityFX, XeSS, AMD FRC and ANR users see no change (the backend is only
  created when chosen; `initPushWarp` returns before allocating anything otherwise, as `initPredict` does).
- Experiment switch first (`VEYRA_EXP_RP_PUSHWARP=1`), OFF by default, documented in PS5_REMOTE_PLAY_EXPERIMENTS.md.

## 9. PushFlow tiers as the higher-quality "own" option

PushFlow already runs RIFE on our D3D12 queue through ONNX Runtime + DirectML at a reduced size with a full-size
rebuild (docs/PUSHFLOW.md, CODE). For schedule (c) it could make I(2/3) instead of PushWarp's warp (one network run at
t = 2/3; the arbitrary timestep is a network input). What the documents and code say it costs: **no GPU timing exists
on any real card** (docs/PUSHFLOW.md: "Still not measured: any of this on a real graphics card"; only WARP times,
which are a CPU). Its CPU-side cost, its warm-up and its crash guards are documented; its GPU cost is not. No number
is invented here. Two facts limit it for the predicted schedules: RIFE does not extrapolate (t > 1 is outside its
training), so P must come from PushWarp's field anyway; and RIFE's flow is inside the network, so the motion is NOT
shared with P (two motion computations per pair). Picture: the harness's `rife_fast` / `rife_quality` rows
(RESULTS.md) are the evidence for what it would give for I. PushFlowBench on his card is the way to get its cost.

## 10. Compatibility with captured video (every element)

| Element | Needs | Available from a capture card? |
|---|---|---|
| Motion field | two real frames | yes |
| Trust | the two frames and both flow directions | yes |
| Occlusion (flow reversal coverage) | the motion field only (depth proxy = speed) | yes; no depth exists, the proxy is a heuristic (it can pick the wrong layer when a slow foreground passes a fast background: measured in the harness's band PSNR) |
| Acceleration | the previous pair's field | yes (kept from the previous pair) |
| HUD protection | stillness over time (HUD Lock), user boxes | yes |
| Scene cut | the frames (CPU analysis, GPU pair test) | yes |
| Future frame for P | - | not used: P uses only A, B (and Z) |
| Engine motion vectors, depth, camera, input | - | not used anywhere |

## 11. What only his PC can answer (none of it was measured)

1. The strict DIS's GPU time at 960x540 (Package 15 pairs: `gpuFg2P50/P95Ms`), against PushWarp's block matching.
2. PushWarp's passes' GPU time (its own timestamps), and whether they delay VFG / presentation work on the same queue.
3. The look, by his eyes, in motion: P at motion starts/stops, silhouettes, HUD, flashes; I(2/3) against VFG's.
4. The delay readout and on-grid share of whichever schedule is built (no figure is promised).

## 12. Implementation plan

Deliverable 3 of the task: only after this document and RESULTS.md are reviewed and confirmed. Outline of the steps
(each compiles and is tested on WARP): (1) PushPredict reads Texture2D real frames and the FidelityFX densified field
(no CUDA buffers), refusals for HUD Lock / exclusion / Smoothness Mode removed, HUD Lock and boxes composed on P with
B held; (2) WARP tests: P with HUD Lock on, P in HDR10, P from a block field; (3) PushWarp I(2/3) (reversal splat in
two passes + compose) behind `VEYRA_EXP_RP_PUSHWARP`; (4) backend value, capability, label; (5) GPU timestamps per pass
in the existing timer for his log.

## 9. As built on exp/s11-ownfg (25 Nov 2026): the owner's priorities as design constraints

**What is evidence here and what is not.** Everything below is from the code, the CPU reference (`pushWarpReference`),
WARP runs (Microsoft's software rasteriser) and the offline harness (CPU, synthetic 4K scenes). WARP proves structure,
correctness against the CPU reference and safety (no device removal, contained self-checks). It is **not** a timing
measurement of any graphics card, and the relaxed test gates (`VEYRA_TEST_OWNFRAME_COST_GUARD=0`,
`VEYRA_TEST_PREDICT_ENTRY_GUARD=0`, the zero flow source) are not evidence of real-world performance or stability. The
first real measurement will be the owner's log after he chooses PushWarp.

PushWarp can be chosen for every schedule NVIDIA's VFG runs in, with one control each, at identical settings:

- standard schedule I(1/n) .. I((n-1)/n), B: the frame generation choice "PushWarp" (`FrameGenerationBackend::PushWarp`,
  2X-4X);
- one predicted frame I(2/3), B, P: the Lower delay row's source "PushFPS" (or the PushWarp generator);
- fully causal B, P, P: the experimental switch (step 4).

**Which A/B answers which question (review 2).** In the standard schedule B is shown after every generated frame of its
pair, and the pair's phase is learned from when all of them are ready. B's delay is therefore the generators' cost (about
2 ms per frame for NVIDIA in his log) plus the schedule's wait of (n - 1) displayed frames (22 ms at 3X and 90 FPS),
which no generator changes. A faster generator lowers only the first part. So:

- **Standard schedule, VFG vs PushWarp:** compares picture, GPU cost and pacing at identical settings. It is not a delay
  comparison beyond the generator's own cost.
- **Delay:** the one-predicted schedule with NVIDIA's I(2/3) against the same schedule with PushWarp's I(2/3) (the Lower
  delay row's source), and the causal schedule (B first, nothing generated before it). Those change the schedule's wait.
  How much, on his card, is unmeasured.
- Every selector stays the user's: no automatic choice ever picks PushWarp (the optimiser and the generic fallbacks
  skip it; review 2 MUST 1).

Every `[capture-timing]` / `[fg-health]` line ends with `generator=<nvidia-vfg|pushwarp|...>`. The other fields keep
the same names for both generators: the delay split, late or dropped frames, on-grid share, the real frame's P95, and
`ownI*` (PushWarp's stage costs, held share). One log with a minute of each generator therefore answers "which is better
on his card".

### 9.1 Motion once per pair, reused (priority 1)

| Schedule | Fields per pair | PushWarp passes per pair | Shared between the pair's frames |
|---|---:|---|---|
| Standard, n X | 1 (FidelityFX B -> A, recorded with the real frame in its own list) | n - 1 records: trust, splat (2), fill (log2 levels), compose, verdict, cut test | the field and its trust texture; A and B buffers |
| One predicted | 1 (the same field for I(2/3) and P) | I: 1 record; P: PushPredict's warp | the field |
| Causal | 1 | P1, P2: 2 records | the field |

The splat cannot be shared between frames. Flow reversal splats B's vectors to time t (target `x + (1 - t) g`), so its
targets depend on t. The per-cell trust (the 4x4-cell SAD against A) does not depend on t, and it is now shared: the
pair's first frame computes the cells and its later frames reuse them, so the trust pass only clears the accumulators
(`cellsToken`; WARP checks `cellsReused` = n - 2 per pair, with every frame still matching the CPU reference). The
acceleration never shares, because its cells depend on t.

### 9.2 The real frame's wait (priority 2)

- **Standard schedule.** B is presented last. Its wait is the schedule's (n - 1) slots, plus nothing of PushWarp's if
  the frames are ready. Each generated frame is recorded in a list of its own and submitted at once, so I(1/3) never
  waits for I(2/3).
- **Flow timing.** The FidelityFX list is submitted right after B's base list: the flow starts once B's base work is
  submitted, and the queue orders it after that work. It cannot start earlier, because it reads B's pitch-linear copy,
  which is made at the end of the base list. What carries over from the previous pair: FidelityFX's own history and
  A's pyramid, the PushWarp pipeline states and root signature, and the descriptor heap slots (`kSlots = 8`).
- **One-predicted schedule.** B waits for I(2/3) only: the flow plus one PushWarp record. That path is guarded: entry
  on the start-up play-size sample, exit on the P95 in play, both at 25 % of an output slot (review 1, SHOULD 4).
- **Causal schedule.** B waits for nothing generated.

### 9.3 GPU synchronisation, counted from the code (priority 3)

| Per pair at 3X | NVIDIA VFG (VfgBackend.cpp) | PushWarp |
|---|---|---|
| Queues / APIs | D3D12 direct queue, CUDA stream, CPU worker thread that submits CUDA work | D3D12 direct queue only |
| Cross-API fence operations | per generate call: D3D12 Signal(producer) -> CUDA wait external semaphore -> CUDA signal -> D3D12 queue Wait(completion) | none |
| Full-frame copies | B into the CUDA-shared input buffer (per real frame), the CUDA output into the graph texture (per generated frame) | B into its pitch-linear buffer (per real frame; read by the flow and by PushWarp). Generated frames are written in place (UAV). The verdict / cut copies are predicated and do no work unless they fire |
| CPU read-back on the frame path | none awaited (CUDA job state polled) | none (counters and stage times read back later, for the log only) |
| Shared memory | his log: `sharedBytes=199065600` | none (own buffers: 2 x 33 MB at 4K without VFG, scratch about 45 MB at 960x540) |

This is where an own generator can beat NVIDIA's on his card even at a similar shader cost. That is unmeasured.

### 9.4 Pacing in fast motion: a bounded cost (priority 4)

- **Flow.** FidelityFX's search range is fixed by the effect, so its cost does not depend on motion.
- **Trust.** Per flow cell: 16 samples.
- **Splat.** 4 taps per cell in two passes. A vector longer than `maxMotionShare` (1/8 of the field's width) is
  untrusted and does not move, so motion magnitude does not change the amount of work. The worst case is contention:
  many cells landing on one target serialise their atomic adds. That is bounded by the number of flow cells (518 400
  at 960x540), and the fixed-point sums cannot overflow (review 1: 2.2e7, about 100x below int32).
- **Fill.** A fixed pyramid.
- **Compose.** A fixed number of taps per pixel.
- **Fast-pan fallbacks.** The whole-frame verdict and the cut test are predicated copies of constant cost, so they
  cannot make the frame late.

What can still vary with content: cache behaviour of scattered reads in fast motion (not measured).

### 9.5 Picture: the measured scheme is the implemented scheme (priority 5)

`tools/pushwarp_eval` runs `pushWarpReference` (the exact rules the GPU passes are checked against) on the study's
synthetic 4K scenes. The field is FidelityFX-like: 8x8 blocks at 960x540, photometric trust. The comparison is with
RIFE v4.26 ("quality") I(2/3) of the same 44 pairs. OFFLINE, CPU, not game footage.

| Frame | PSNR mean | Against RIFE I(2/3) | Worst sequences |
|---|---:|---:|---|
| I(2/3) | 29.58 dB | -0.58 dB | crossing (thin railing, cars behind) -4.05; flash_fade -3.37 |
| P(1/3), no acceleration | 28.36 dB | -1.80 dB | crossing -5.04; flash_fade -5.41 |
| P(2/3), no acceleration | 26.25 dB | -3.91 dB | crossing -7.65; pan_medium -5.42 |

Acceleration from the previous pair made P worse on average on this field: -0.3 / -0.4 dB. It gains only at starts
and stops (turn_start_stop: +1.3 / +1.0 dB). A relative gate at half the speed lost that gain too. It is therefore off
in the causal pair.

**Known weakness: thin occluders.** At the field's 4 px cells, a railing's bars and what moves behind them share
vectors. The cheapest improvement the study supports is a finer field near thin structures, without a second flow:
the field at 1920x1080 ("ffx2"). In the block-field study's two crossing pairs (photo_nb trust) it gave +0.6 dB for I(2/3)
and +0.4 / +0.3 dB for P(1/3) / P(2/3); on drive and hud_pan it gave +0.4 to +1.4 dB. Using it only where the trust is
low is a proposal that has not been measured; its flow costs about 4x the 960x540 field's (unmeasured on a card).

### 9.6 The comparison evidence (both generators, same names)

- `generator=<nvidia-vfg|pushwarp|...>` on every `[capture-timing]` / `[fg-health]` line.
- `motionP50Px` / `motionP95Px` (trusted vectors' length in full-size px per real frame, last second, counted once per
  pair by PushWarp's trust pass), `motionPairs`, `motionSource`. These are `n/a` when PushWarp does not run (NVIDIA's
  frames): no flow or pass is started only to log them.
- `flowMs`: our flow's own GPU stage (mean / P95 this second). It is -1 where our flow did not run (NVIDIA VFG without
  Lower delay), so it is never folded into another field.
- Cost fields mean the same for both generators (review 2, MUST 3):
  - `gpu_fgBatch` and `gpu_fg1` open in B's base list before the copy of B into the generator's input, for both. For
    VFG that copy is into the CUDA-shared buffer (with its cut test); for the PushWarp generator it is the pitch-linear
    copy its flow and passes read. Both copies are in `pictureVfgCaptureMs` (the stage's name is VFG's; it is the
    generator's input copy for either).
  - The flow (our FidelityFX list, when it runs) lies inside `gpu_fg1` for both, and is also `flowMs` on its own.
  - `gpu_fg<k>` closes after generated frame k and `gpu_fg<k+1>` opens: VFG's CUDA work and interop for frame k, or
    PushWarp's record for frame k. `gpu_fgBatch` closes after the last one. Smoothness Mode's reading of `gpu_fg1` thus
    sees the same span (copy + flow + first frame) for either generator.
  - PushWarp's per-stage times are `ownIGpuMs` in addition.
- `ownINoMotion`: generated frames shown as the nearer real frame because no field existed yet (at the start, after a
  reset). It is counted, not treated as a fault. Review 2 SHOULD 2: the PushWarp generator now feeds the flow on pairs
  skipped by admission too, so a skipped pair no longer costs the next pair its field.
- The frame quality test (opt-in, his click) keeps its last result per generator (pref `pushQualityByGenerator`) and
  shows every generator tested in one table. It withholds every second real frame while it runs; the score combines the
  generated frames' accuracy, the gain over a blend, the HUD boxes and the on-time share.

### 9.7 Not done yet (said plainly)

- The Professional node editor has no PushWarp node (it is filtered out of the node list and the generic "add frame
  generation" fallback). The generator is chosen in the frame generation menu.
- Rollback: `PushLegacyPart::OwnFrame16` (`VEYRA_TEST_RP_LEGACY_OWNFRAME`, alias `VEYRA_TEST_OWNFRAME_OFF`, restored by
  the PACKAGE15/14/13 masters) removes PushWarp entirely.
- Fault memory: PushWarp's own marker (`lowdelay-warp-check.txt`): "running"/"failed" block within one start of Windows
  (and this program file); a device removal in play while PushWarp worked in the last 5 s marks "suspect", the second
  in a row "failed" (`noteOwnWarpSessionEnd`). A start-up budget miss alone is checked once more by the next graph.
- Fast pans: `maxMotionShare` (1/8 of the field width, 480 px per real-frame interval at 4K) is unchanged. Raising it
  costs no GPU time by construction (see 9.4), but the quality of long block-field vectors is unmeasured (the synthetic
  pan_fast moves about 180 px). It needs an offline sequence with 500-900 px pans first.
- Nothing here has been measured on a graphics card.
