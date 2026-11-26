# Stutter with the "DLSS 5" option on: what it is and the cause (stream "nrperf", 22 Nov 2026)

Scope: the owner's eighteenth message ("When I enable DLSS 5, my screen becomes noticeably stuttery"). Base:
exp/s6-capture. Everything below is CODE READING of this tree, published or press figures, or the owner's own P9 logs
(/home/claude/evidence/p9/x, eight Remote Play runs with frame generation and the neural layer OFF). **No run with the
neural layer ON exists from his PC**, so every size in milliseconds is an estimate and every cause is ranked, not proven.

Labels: **code** (file and function named, this tree) · **owner-P9** (his logs, NR off) · **upstream-measured** (the
PushFPS / Veyra developers' PCs, recorded in this repository) · **published** (NVIDIA) · **press** (a site's
measurement) · **estimate** (arithmetic from the above) · **hypothesis** (needs his run). CONFIRMED = the mechanism is
in the code; its SIZE on his PC is still an estimate unless labelled owner-P9.

Symbols: T = source interval of a pair (16.68 ms for the 59.94 fps stream; 33.4 ms for a 30 fps game once duplicates
are removed); M = multiplier; base = GPU time of one real frame's own processing (colour, clean-up, flow, SR, NR,
composite, final write); fg = GPU time of one generated frame.

## 0. Short answer

**What it is (code).** The "DLSS 5" / "AI picture enhancement" option runs NVIDIA's `nvngx_dlssnr.dll` (the runtime of
NVIDIA's DLSS 5 "3D-Guided Neural Rendering", NGX feature id 18), version 310.8.0.0 on his PC (`[nr-runtime-status]
originalVersion=310.8.0.0 originalSigned=1 selected=original`, owner-P9). The app loads that DLL itself and calls its own
exports `NVSDK_NGX_D3D12_Init_Ext / CreateFeature / EvaluateFeature / ReleaseFeature / Shutdown1`
(`src/ngx/DlssNrRuntimeAdapter.cpp`), not through a public NVIDIA SDK (none exists: S5_NR_RESEARCH.md A3) and not
through Streamline. It is applied to the decoded video picture with optical-flow motion and a constant depth; it is not
NVIDIA's game-integrated DLSS 5 (no engine motion vectors, depth, jitter or masks).

**Most likely cause (CONFIRMED mechanism, size = estimate).** The neural layer adds one inference of about **5.5 ms
(estimate)** to every real frame at its default 1920x1080 inference size, plus four full-picture passes, in series
with frame generation on the same GPU queue. A PS5 stream is processed **one pair at a time**: the engine does not read
the next decoded frame until the previous pair's whole GPU work, generated frames included, has finished
(`enhancementPending`). When base + the generation group no longer fits in the 16.68 ms source interval, (a) decoded
frames are replaced in the one-slot mailbox before the engine reads them (real frames lost, a 2T step), and (b) a frame
that waited in the mailbox reaches admission with less time left, so its generated frames are skipped: 2X and 1X
alternate (the 60 <-> 90-100 pattern of FPS90.md P1), and a skip run of 1 s or more ends in a FgCostSettling hold
(exactly 60 for 2, 4, 8 s, P2). Whether the neural layer tips his configuration over depends on the multiplier, the VFG
quality and the working size: by estimate 2X at 1440p with VFG Medium still fits (~13 ms of 16.7), **3X (180 Hz), SR
to 4K (the default SR target), VFG High, `nrSize=1440` or `native` do not** (section 4).

**Also CONFIRMED, independent of the overrun:** on Remote Play every real frame is shown `T + P95(base)` after decode
when frame generation is on, so the layer's P95 is added to the latency of every frame, and its run-to-run spread moves
that phase pair to pair (section 5, C2); the layer switches on the graph's optical flow even when nothing else needs it
(C4); it runs on every decoded frame, including the console's repeated pictures of a 30 fps game (C3).

**What decides it on his PC:** the four-way comparison (section 7). If NR ON / FG OFF is smooth and NR ON / FG ON
stutters with `[slipstream-smooth] gpuLoadPct` near 90-100 and `receivedFps` below the decoder's rate, cause C1 is
proven; if NR ON / FG OFF already stutters, the layer alone exceeds the interval (C5: size) or spikes (C6, C7).

## 1. Exactly which NVIDIA component and API (code)

| Question | Answer | Where |
|---|---|---|
| Component | `nvngx_dlssnr.dll`, NVIDIA's DLSS 5 neural-rendering runtime ("snippet"), NVIDIA-signed original only, from `runtime\experimental\nr-original`; recorded version 310.8.0.0 (SHA-256 checked); his copy passed every check | `docs/NEURAL_RENDERING_RUNTIME.md`; `EnhanceGraph::initNgxFeatures` (`nr-runtime` log, `nrDirectory/=L"nr-original"`); owner-P9 `[nr-runtime-status]` |
| API | NGX D3D12 entry points exported by the snippet itself, feature id 18 (`DlssNrRuntimeAdapter::kFeatureId`), called through `GetProcAddress`; a caller-name shim on the snippet's own import slot makes it see the caller as `nvngx.dll` | `include/veyra/ngx/DlssNrRuntimeAdapter.h`, `src/ngx/DlssNrRuntimeAdapter.cpp` |
| Parameters | create: `DLSSNR.Width/Height` = `InputWidth/Height` = `OutputWidth/Height` = the inference size, `DLSSNR.Upscaling 0`, `Scale 1`, preset 0, `PerfQualityValue 1`; evaluate: `DLSSNR.Color` (8-bit proxy), `DLSSNR.Output`, `DLSSNR.MVec` (our optical flow at the inference size, or zero), `DLSSNR.Depth` (constant), reset, style, intensity, tone, structure, skin, auto mask, UI correction | `EnhanceGraph::createNrFeature` (~2223), evaluate block (~3975-4012), `include/veyra/ngx/DlssNrParameters.h` |
| Official SDK? | No public DLSS 5 SDK, header or licence exists (NVIDIA forum replies of 9 and 20 Nov: partners only) | S5_NR_RESEARCH.md A3 [S3] |
| Is it "real DLSS 5"? | Same runtime, different inputs: NVIDIA's integration passes engine motion vectors, depth, jitter and a control mask on an LDR picture (published, Remix source); ours passes compressed decoded colour with the HUD drawn in, optical flow and a constant depth | S5_NR_RESEARCH.md A5 [S41][S42] |

## 2. At what size (code)

The inference size is a 16:9 box of N lines fitted into the layer's input, never enlarging
(`pipeline::ResolutionPlan::make`, `include/veyra/pipeline/ResolutionPlan.h`). The layer's INPUT is the upscaler's
output (default order "SR -> NR -> residual -> FG", `[pipeline-order]`), so: input = work size; inference = min(policy,
input); the layer's change is composited back at the input (work) size; frame generation then runs at the work size.

| nrSize (policy) | SR off (work 1920x1080) | SR to 1440p (work 2560x1440: Enhanced 1440p) | SR to 4K (work 3840x2160: `EnhancementSettings::srTarget` default `Uhd4K`) |
|---|---|---|---|
| 480 / 720 / 900 | 852x480 / 1280x720 / 1600x900 | same | same |
| **1080 (Realtime, the default of every new layer and of Enhanced 1440p)** | 1920x1080, input borrowed (no downsample) | **1920x1080**, downsample 1440p -> 1080p first | **1920x1080**, downsample 4K -> 1080p first |
| 1440 | 1920x1080 (input is smaller) | 2560x1440 | 2560x1440 |
| native | 1920x1080 | 2560x1440 | **3840x2160** |
| composite of the change | 1080p | 1440p | 4K |

**Does it run at 4K when the upscaler targets 4K?** The inference does only with `nrSize=native` (Professional mode;
the profiles' planner caps it at 1440 lines, `PushPipelinePlan.h` `nrMaxHeight`, but the live graph has no such cap).
With the default 1080 policy the inference stays at 1920x1080, **but its composite, the final write and frame
generation run at 3840x2160** for the 2560x1440 panel (2.25x the pixels; SMOOTHNESS_ANALYSIS.md C1). The log line
`[nr-model] runtime=original preset=0 internal=WxH` and `[resolution] ... nr=WxH ... output=WxH` give the sizes in force;
`[slipstream-nr] internal= output=` too.

## 3. On which frames, where, synchronous or not (code)

- **Real frames only.** The layer runs inside `EnhanceGraph::process` (2889), once per real (decoded) frame, before the
  final write (`5. Working frame -> ... videoFrame[parity]`, ~4153) and before frame generation, which reads the
  enhanced output frames of this and the previous real frame (`// FG reads enhanced SDR color`, ~4258). Generated
  frames are interpolated from two enhanced frames: they are never enhanced themselves, and they inherit the look.
- **Every decoded frame, repeats included.** Without duplicate removal, both copies of each picture of a 30 fps game in
  the 59.94 fps stream are enhanced (NEURAL_RENDERING.md section 4.2).
- **Where:** colour -> clean-up -> optical flow -> SR -> **downsample (unless borrowed) -> 8-bit encode -> evaluate ->
  decode -> composite at the work size** -> tail colour -> final write -> frame generation (VFG on CUDA; DLSS FG /
  PushFlow on the graph queue) -> the presenter's blit on its own queue.
- **Synchronous?** Not on the CPU per frame: the snippet records its work into our command list
  (`EvaluateFeature(ID3D12GraphicsCommandList*, ...)`), and the graph's direct queue executes it in order. Our code does
  NOT wait on a fence for it. Two costs exist on the CPU side: the list is SPLIT before each layer's evaluate (one
  extra `ExecuteCommandLists` + fence signal per layer per frame, "a snippet constraint", ~3961-3966), and the
  snippet's own CPU time for evaluate runs on the engine thread (unmeasured; `[graph-cpu-stall] nrAcquireMs= nrEvaluateMs=`
  only when a frame's CPU recording takes 30 ms or more). Feature creation is synchronous (`createNrFeature`:
  `submitAndSignal` + `waitIdle`), but only when the graph is built. **The effective synchronisation is the engine's
  one-pair-at-a-time rule for live sources** (C1).

## 4. GPU cost and video memory

| Item | Figure | Label |
|---|---|---|
| Neural layer, NVIDIA's first model, RTX 5070 | about 10.2 ms per frame at 1440p, 22.1 ms at 4K; cost follows output pixels | press (TechSpot, game-integrated, S5_NR_RESEARCH.md A4 [S15]) |
| October model | "half the September GPU overhead", 20 % less 4K video memory | published (NVIDIA, 13 Oct [S1]); press headlines of the same: TechRadar ["Half the GPU overhead ..."](https://www.techradar.com/computing/gpu/half-the-gpu-overhead-with-comparable-visual-fidelity-nvidia-dramatically-improves-dlss-5-which-is-great-but-gamers-want-it-on-more-gpus), TweakTown ["... 2x faster and uses 20 percent less VRAM at 4K"](https://tweaktown.com/news/114064/nvidia-says-dlss-5-model-is-now-2x-faster-and-uses-20-percent-less-vram-at-4k) |
| Which model his 310.8.0.0 is | the NBA 2K27 early-access file of 27 Aug 2026 (press [S11]), before the faster model (13 Oct): **the first model**, so the slower figures apply | hypothesis (strong: version and date) |
| Inference at our sizes (first model, 2.7 ms per Mpixel) | 1920x1080 **~5.5 ms**; 1600x900 ~3.9; 1280x720 ~2.5; 2560x1440 ~10.2; 3840x2160 ~22 (more than T by itself) | estimate (halve each for the October model) |
| Passes around it | downsample, 8-bit encode, decode at the inference size; composite at the work size (a full-picture pass at 1440p or 4K) | code; cost unmeasured (`[slipstream-nr] graphMs` minus the other stages) |
| Optical flow the layer switches on | 1.36 ms at 1080p (2.44 with refinement) | upstream-measured (CAPTURE_FG.md section 3) |
| VFG Medium per generated frame | ~3.5 ms at 1440p; 6.6 ms per source frame at 4K (export run) | estimate / upstream-measured (CAPTURE_FG.md section 3) |
| VFG High | ~14 ms per generated frame at 1440p; "VFG High 2X at 4K is 16 ms on an RTX 5070" | estimate / upstream-measured (`FgRecoveryBudget::phaseAllowance` comment) |
| Video memory | NVIDIA publishes no figure; the runtime reports its own allocation. His PC: budget 11,175 MiB, 314 MiB in use with every effect off | published (none) / owner-P9 (`[slipstream-nr] vramMiB=314 budgetMiB=11175`) |

Fit of one pair at T = 16.68 ms (estimate; b0 = everything else in the base, unknown, 3-5 ms assumed):

| Configuration | NR off | NR on (1080) | Fits with NR? |
|---|---|---|---|
| Enhanced 1440p (clean + DLSS SR 1440p) + VFG Medium 2X (120) | b0 + 3.5 | b0 + ~6.5 + 3.5 ≈ 13-15 | yes, little room |
| same, 3X (180) | b0 + 7 | b0 + ~6.5 + 7 ≈ 16.5-18.5 | **no** |
| DLSS SR to 4K (default target) + NR 1080 + VFG Medium 2X | b0 + 6.6 | b0 + ~7 + 6.6 ≈ 17-19 | **no** |
| VFG High 2X at 1440p | b0 + 14 (already marginal) | b0 + ~6.5 + 14 | **no** |
| nrSize=1440 at 1440p, VFG Medium 2X | b0 + 3.5 | b0 + ~11 + 3.5 | **no** |
| nrSize=native with SR to 4K | | ~22 ms for the inference alone | never, even with FG off |
| 30 fps game, duplicates removed (T = 33.4), 4X / 6X, 1440p Medium | b0 + 10.5 / 17.5 | b0 + 17 / 24 | yes |
| 30 fps game, duplicates NOT removed (T = 16.68 per decoded frame) | as the 60 fps rows | as the 60 fps rows, with half the inferences spent on repeats | as above |

## 5. The chain from "NR on" to visible stutter, most likely first

| # | Cause | Status | Code | What it does on screen | Field that confirms it on his PC |
|---|---|---|---|---|---|
| **C1** | **Per-pair GPU work over the source interval, in a one-pair-at-a-time engine.** The next decoded frame is read only after the previous pair's GPU work, generated frames included, is complete. Over T: the one-slot mailbox replaces decoded frames (lost real frames; history is kept across a gap under 250 ms, so the next pair spans 2T); a frame that waited reaches admission with less time left, because the deadline is anchored at its decode time while the wait is not credited as progress, and its generated frames are skipped; a skip run of 1 s or more probes and holds generation 2, 4, 8 s | CONFIRMED (mechanism); size hypothesis | `EngineController.cpp` `enhancementPending` (~2053-2068: "never stack a second frame behind one the GPU is still enhancing"); mailbox `RemotePlaySessionSource::publishDecoded` / `publishDelivery` (~530-565); `liveInputReady = pkt.decodedHost100ns` (2461) vs `processStart` (2456); `admitFg` (2700-2716) -> `FgRecoveryBudget::canAdmit`; `FgCostSettling::observe`; bounded skip `kRetainHistoryGapMs=250` (~2506) | irregular holds of 2T, 2X/1X alternation (90-100 shown), seconds at exactly 60 | `[slipstream-smooth] gpuLoadPct` near 90-100, `gpuSpanP95Ms` against `pairBudgetMs`, `gpuNrP95Ms`, `receivedFps` below `[remoteplay-progress] decodeFps`, `rejectedPairsFps`, `holds`; `[remoteplay-health] mailboxOverwrites` per 5 s (owner-P9 baseline with everything off: 0 in 194 of 218 windows, at most 10); `[slipstream-delivery] replaced= pickupDelayMax=`, `[slipstream-latency] decodedToPickupP95` (the mailbox wait); `[preview-skip] retained history across a X ms gap`; `[live-fg-admission] remainingDeadlineMs rejectedPairs`; `[vfg-probe] retryInMs=` |
| **C2** | **The layer's P95 is in every real frame's phase (Remote Play).** With graph FG on a PS5 stream, pair (A,B) shows B at decode(B) + T + min(T, P95(base)) (`processingAllowance`); P95 over the last 64 samples (~1 s). The layer becomes most of the base, so (a) every frame is shown ~NR P95 later and (b) the phase moves whenever a slow inference enters or leaves those 64 samples, by the change of the P95, which at 120/180 Hz can move a present across a refresh | CONFIRMED (latency); the jitter's size is a hypothesis | `legacyPairDelay = liveInterval + processingAllowance` (2667); `FgRecoveryBudget::processingAllowance`, `p95()` (64 samples or 10 s); `PresentationScheduler::deadline` | added delay on every frame; occasional single long holds | `gpuNrP95Ms` minus `gpuNrP50Ms` in `[slipstream-smooth]` (the spread); `[slipstream-latency] pickupToPresentCallP50/P95` NR-on minus NR-off with FG on; stream fglat's new `phaseMs` (exp/s6-fglat) |
| **C3** | **Repeated pictures enhanced.** A 30 fps game in the 59.94 stream: every repeat is enhanced (and interpolated) unless duplicate removal is on; T stays 16.68 ms per decoded frame, so C1 applies at the 60 fps budget with half of the inferences wasted | CONFIRMED | NEURAL_RENDERING.md section 4.2; `ContentDedup`, `PushStreamDedup` | as C1 | `[slipstream-smooth] receivedFps` vs `uniqueFps`, `contentFps`; `[remoteplay-cadence] sourceFps repeatsRemoved` |
| **C4** | **The layer turns on the graph's optical flow.** With VFG (own motion estimation) and an upscaler without motion (NIS, RTX Video SR) or none, the layer alone makes the standalone optical-flow session run every real frame, plus its adaptation to the layer's size | CONFIRMED | `GraphDescription.h` (191: `enableNvofStandalone = enableNr && motionUsesFlow(nrMotion)`); `EnhancementSettings::nrMotion` default `OpticalFlow` | adds ~1.4 ms (upstream-measured, 1080p) to C1's base | `[slipstream-smooth] gpuFlowP95Ms` NR on vs off; `flow=` in the same line |
| **C5** | **Size: work or inference too large.** SR to 4K (the default target), `nrSize=1440`, `native`: see section 4. With `native` and SR to 4K the inference alone (~22 ms) exceeds T, so even FG OFF stutters | CONFIRMED (sizes); cost estimate | `ResolutionPlan::make`; no live cap above the display (`PushPipelinePlan.h` caps only the profiles) | as C1, also with FG off | `[nr-model] internal=`, `[slipstream-smooth] work= display=`, `[slipstream-nr] internal= output=` |
| C6 | **First-use and rebuild hitches.** Turning the layer on rebuilds the graph: the 158 MB runtime and its model are created synchronously (`waitIdle`), VFG is created again, the FG budget is reset, the first evaluations count as warm-up | CONFIRMED, one-time (not a lasting stutter) | `createNrFeature`; EngineController `fgBudget.reset()` on a revision change (~2540) | one hitch of up to a few hundred ms, then a few seconds of warm-up | `[reset-lifecycle] createMs totalMs`; `[gpu-stage-stall]` (a stage of 80 ms or more) |
| C7 | **Video memory pressure.** Over the budget, Windows pages video memory and the GPU stalls | hypothesis, unlikely at 1080p (no NVIDIA figure) | `[slipstream-nr]` reads the process's usage and budget | long single stalls | `[slipstream-nr] vramMiB` against `budgetMiB` (his budget 11,175 MiB); `[gpu-stage-stall]` |
| C8 | **The presenter's blit competes with the next pair's inference.** Generated frames of (A,B) are presented while (B,C)'s base runs; B itself at decode(C) + P95(base), about when C's inference ends. A long inference kernel can delay the blit on the independent present queue (normal priority) | hypothesis | `VideoPresenter.cpp` (independent queue; high priority only with `VEYRA_TEST_PRESENT_QUEUE_PRIORITY=high`); FG_LATENCY.md 1.4 | late or dropped generated frames | `[slipstream-smooth] gpuBlitP95Ms` vs P50 and `dropDeadlineFps` NR on vs off; PresentMon `MsGPUActive` |
| C9 | **CPU side of the evaluate.** One more list, submission and fence per layer per frame; the snippet's CPU time on the engine thread delays the NR list's submission (a GPU bubble counted in the base) | CONFIRMED (exists); size hypothesis, expected small | ~3961-3966, 4015 | adds to C1's per-pair time | `[graph-cpu-stall] nrAcquireMs nrParametersMs nrEvaluateMs` (only frames of 30 ms or more); `[frame-flow] slotWaitCount slotWaitMs` |

**How C1 shows up, from the code's rules (estimate).** Let S be one pair's service time (CPU recording before the first
submission + GPU base + generation group + the completion wake). With S > T the decode-to-pickup wait grows by S - T per
pair; when it passes T the next decoded frame has already replaced the waiting one. So about 1/T - 1/S real frames a
second are lost, regularly: S = 18 ms at T = 16.68 ms loses about 4.4 a second (one in ~13), S = 20 ms about 10. On top of
that the live admission (`FgRecoveryBudget::canAdmit` with the Remote Play pair phase T + min(T, P95(base))) reduces, for
a frame that waited w ms, to (M-1)/M x T + 10 ms - w >= P95(fg group) + blit: the base cancels out, the wait does not, so
generated frames are skipped at the top of each sawtooth (with VFG Medium at 1440p once w passes about 14 ms at 2X and
13.5 ms at 3X), more often as S grows. Both counters rise together with NR on: `[slipstream-delivery] replaced` (or
`[remoteplay-health] mailboxOverwrites`) at about 1/T - 1/S per second, and `rejectedPairsFps`; `decodedToPickupP95`
near T. If they stay at the owner-P9 baseline with NR on, C1 is refuted.

Not a cause (code): the layer never runs on generated frames; no queue or buffer grows (every hand-off is latest-wins
or bounded, FG_LATENCY.md 1.2); live admission adds no queued-GPU term because nothing is queued.

**Added latency (stream fglat's finding, applied here).** When C1 makes admission skip a pair, the real frame of that
pair is still shown at the full FG phase ("dead phase"): on Remote Play about T + P95(base) (~16.7 ms at 59.94) per
skipped pair, because `LiveFgRejectionHold` runs only with card timing or `VEYRA_EXP_RP_ADAPTIVE_PHASE=1`
(docs/drafts/FG_LATENCY.md on exp/s6-fglat, source 2). So the neural layer's overrun costs latency on top of the
stutter. Fields: `[slipstream-smooth] rejectedPairsFps holds`, fglat's `deadPhaseFps`. Stream fglat owns the fix
(`VEYRA_EXP_RP_FG_ULL`); this stream builds none for the phase.

## 6. The owner's ten points

1. **GPU time ON vs OFF**: `[slipstream-smooth] gpuNrP50/P95Ms gpuSpanP95Ms gpuLoadPct`, `[slipstream-nr] nrMsP95 graphMsP95`. C1, C5.
2. **VRAM, contention, scheduling**: C7 (`vramMiB` / `budgetMiB`), C8; the layer and frame generation share one queue in series, VFG on CUDA waits for it on the GPU.
3. **Enhancement and FG competing**: yes, in series per pair (C1); their sum is the quantity that matters, not either alone.
4. **CPU/GPU sync stalls**: no per-frame CPU fence wait for the layer; the engine waits for each pair's completion by design (C1), and C9.
5. **Pacing, deadlines, swap chain**: C2 (phase), C1 (rejections, holds); swap-chain depth unchanged by the layer.
6. **Buffering, stale frames**: none added; a frame that waits in the mailbox is replaced, not queued (C1).
7. **Conversions / transfers**: four full-picture passes around each inference (8-bit encode and decode, downsample, composite at the work size), section 3; no CPU copy.
8. **Higher resolution than necessary**: C5; the composite and FG at 4K with the default SR target; NR after SR downsamples the upscaled picture back to 1080p.
9. **Refresh 180 Hz**: his panel ran at 119.998-120 Hz in all eight P9 runs (`[slipstream-config] refreshHz=120.00`, owner-P9); check `refreshHz` in every run.
10. **Every generated frame enhanced?**: no, and it should not be: generated frames are made from two enhanced frames; enhancing them would multiply the cost by M and feed the model's temporal history with interpolated pictures.

## 7. What his PC must show (decision table)

Kit (arms.csv sets `nrperf` / `nrperf-capture`, run inside the smoothness / capture sessions; docs/HARDWARE_TEST_PROCEDURE.md):
`Run-First-Test.cmd -Session smoothness -Pass 120 -Arms nrp-base,nrp-nr,nrp-fg,nrp-nr-fg,nrp-nr-fg-adapt`, then `-Pass 180`
(DLSS to 1440p in every arm; `nrp-nr*` add the layer at 1080 lines; FG = VFG, 3x ceiling, so 2x at the 120 cap);
`-Arms nrp-fg,nrp-sr4k-fg,nrp-nr-sr4k-fg,nrp-nr-sr4k-fg-adapt` for the 4K upscaler target (C5); on a capture card
`-Session capture -Pass 120|180 -Arms nrpc-base,nrpc-nr,nrpc-fg,nrpc-nr-fg,nrpc-nr-fg-adapt`. The page shows the presented
spacing P95 / P99, uneven presents, pairs skipped, holds, late generated frames, GPU per stage (whole graph, neural layer
P50 / P95 / worst, flow, FG, upscaler, blit) against the pair budget, decoded vs received frames, decoded frames replaced,
decode-to-pickup P95, the in-app latency (software timestamps: internal, not input-to-photon), the inference size and
the reuse level. Nothing is fixed until these ON/OFF rows show it.


| Observation | Conclusion |
|---|---|
| NR ON / FG OFF smooth; NR ON / FG ON stutters; `gpuLoadPct` 90-100 and `receivedFps` < `decodeFps` or `mailboxOverwrites` up; `rejectedPairsFps` > 0 | **C1** (pair budget). Fix: less work per pair (smaller inference, adaptive skip, 1440p work, 2X instead of 3X) |
| as above at 180 Hz only, smooth at 120 Hz | C1 at 3X: 120 is the target with the layer on |
| NR ON / FG OFF already stutters; `gpuNrP95Ms` near or above `pairBudgetMs` | C5 (size) or C7 (`vramMiB` near `budgetMiB`) |
| `gpuLoadPct` well under 90, no rejections, no mailbox overwrites, but stutter | C2 (phase spread: `gpuNrP95Ms` - `gpuNrP50Ms` large) or C8 (`gpuBlitP95Ms`, `dropDeadlineFps`) |
| stutter only for a moment after turning it on | C6 (`[reset-lifecycle]`) |
| `flow=` non-zero only with NR on, `gpuFlowP95Ms` > 0 | C4 adds that time |
| `receivedFps` ~60 with `contentFps` ~30 | C3: turn on duplicate removal first |

## 8. Placement study (deliverable 4): before FG, after FG, selective

Written from the code and the costs above (estimates); nothing was run. Per real frame of a 59.94 fps stream, 1080-line
inference, work 2560x1440 unless named.

| Placement | What runs per pair | GPU cost per pair against today | Picture | Latency | Verdict |
|---|---|---|---|---|---|
| **A. After SR, before FG (today's default, "SR -> NR -> residual -> FG")** | downsample work -> 1080p, encode, inference, decode, composite at the work size; FG interpolates between two enhanced frames | reference: ~5.5 ms inference + 4 passes | generated frames inherit the look from both enhanced neighbours | + the layer's time on every real frame (FG off: its actual time; Remote Play with FG: its P95, C2) | keep |
| B. Before SR, before FG (existing setting: `EnhancementSettings::lowLatency` / Professional node order "NR first", `desc.nrBeforeSr`, `[pipeline-order] NR -> residual(source) -> SR -> FG`) | the layer borrows the 1080p source (no downsample), composites at 1080p; DLSS SR then enlarges the enhanced picture | the same inference; one downsample pass fewer and the composite on 2.07 instead of 3.69 Mpixel (8.29 with SR to 4K): a fraction of a millisecond (estimate; the passes are not timed separately) | DLSS SR's temporal history sees the layer's changes and may sharpen or smear them: an A/B question | slightly less than A | **no switch**: it exists as a setting, and its saving is far below the inference that decides C1 |
| C. After FG, on every presented frame | M inferences per pair (2X: two, 3X: three), each on the presented frame | x M: ~11 ms per pair at 2X, ~16.5 ms at 3X for the inference alone, more than the whole budget with FG (estimate) | the model's temporal history fed with interpolated frames, whose artefacts it would treat as content | each generated frame waits for its own inference | **rejected**: certain cost, no gain |
| D. Selective: real frames enhanced, generation from the UN-enhanced frames | one inference, FG on the raw pictures | as A, the FG input unchanged | real and generated frames differ every present (at 2X a 60 Hz alternation of detail and tone: flicker) | as A | **rejected** |
| E. Selective in time: infer some real frames, carry the previous change along the flow on the others (upstream's inference reuse) | one inference on 3 of 4, 2 of 3 or 1 of 2 frames; a reuse pass (P95 0.85 ms at 4K, upstream-measured) on the others | mean NR cost x 0.75 / 0.67 / 0.5 | a reused frame's change is one source frame old and falls back to the unenhanced pixel where the flow does not match (upstream: 45.7 dB on a real pan) | + the reuse motion pass's GPU time on every real frame (also at level 0), and so on the phase: small, unmeasured; the P95 stays an inferred frame's cost | **implemented** behind `VEYRA_EXP_RP_NR_ADAPT=1` (section 9) |
| F. Overlap: run the next frame's inference while the previous pair's frames are generated (a second queue) | the engine would read the next frame before the previous pair finished, which `enhancementPending` forbids because a stacked frame doubled capture-to-present latency (its comment: 3 layers at 1440p60, 49 ms for 20 ms of GPU work); VFG (CUDA) and the inference both run on the shader cores, so little would run in parallel | unknown, probably small | unchanged | + up to one interval per frame | **rejected** blind: it raises latency for an unmeasured throughput gain |

So placement is not the lever: A is right, B is a small saving that already exists as a setting (its picture effect needs
the owner's eye: a kit arm for it would need a new `nrPlace` key in `VEYRA_TEST_RP_PIPELINE`, not added), and the lever
is the amount of inference per pair (size, rate, or reuse).

## 9. What this stream changed (code; unverified on Windows; nothing measured)

| Commit | Change | Default? |
|---|---|---|
| f4dbd71a | this document | docs |
| 87fde54e, f95d7219 | `VEYRA_EXP_RP_NR_ADAPT=1` (engine/PushNrAdapt.h, two guarded hunks in EngineController.cpp): in a live session with NVIDIA's neural layer and graph frame generation, a reuse mode the user left Off runs as upstream's Auto (unchanged controller: 0.5 s over budget -> 1 in 4, 3, 2 frames reuse; back after 3 s with 15 % room), with the budget at 80 % of the PAIR interval (content rate once measured, else the stream rate); `[nr-adapt] reuse= reason=` per build | off |
| 9003c972 | kit: the four-way arms (`nrp-*`, `nrpc-*`) inside the smoothness / capture sessions at -Pass 120 / 180, their measures, the allow-list keeps `[nr-adapt]`, `[nr-reuse]`, `[nr-model]` | kit |

The switch's own cost while nothing is reused: one flow-sized motion pass per real frame, in series with the base (so
it also lengthens the pair phase by its GPU time), and about 100 MB (1440p work) to 175 MB (4K) of textures
(arithmetic). With fglat's `VEYRA_EXP_RP_FG_ULL` phase part on too, watch `dropDeadlineFps`. What the adaptive switch can fix by estimate (section 4): configurations whose inferred pair plus reused pair fit in two
intervals: Enhanced 1440p at 3X (~17.5 + ~12 ms), SR to 4K at 2X with VFG Medium (~18 + ~12 ms). It cannot fix VFG High
(the reused pair alone is over T) or `nrSize=native` at 4K. Not done, with the reason:
- **No live NR size cap above the display** (C5): the profiles' planner already caps at 1440 lines, and only
  Professional mode's `native` reaches 4K; a cap in the graph description would also move the optical-flow extent
  (`ResolutionPlan::make`: flow follows a reduced NR size) for an unproven case. The `[nr-model] internal=` line shows
  whether his setup has it; the advice until then: 1080 lines (or 900 with FG at 3X).
- **No adaptive inference SIZE**: a size change rebuilds the graph (a hitch, FG history reset, NR history reset); the
  existing size pool is limited to the community runtime without SR or FG. A reuse level change needs no rebuild.
- **No dead-phase fix**: stream fglat owns it (`VEYRA_EXP_RP_FG_ULL`, section 5).
- **No new log line**: every field the decision table needs exists (`[slipstream-smooth]`, `[slipstream-nr]`,
  `[slipstream-latency] decodedToPickupP95`, `[slipstream-delivery] replaced`, `[remoteplay-health] mailboxOverwrites`).
