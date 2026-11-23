# Frame-generation latency: where the delay comes from in our code (stream "fglat", 22 Nov 2026)

Scope: the owner's seventeenth message (lowest-latency frame generation on an RTX 5070, 2560x1440 180 Hz OLED, PS5
over Remote Play now and through an HDMI capture card next). Base: exp/s6-capture (which merges exp/s6-fps). Read
first and not repeated here: docs/drafts/CAPTURE_FG.md (capture assessment, GPU cost per backend, camera procedure),
docs/drafts/FPS90.md and SMOOTHNESS_ANALYSIS.md (multiplier choice, admission, holds), SYNC_FINDINGS.md on exp/s6-sync
(clocks), KBM_AUDIT.md on exp/s6-kbm (input path).

Labels on every number and claim:
- **code**: read in this repository (file and function named);
- **upstream-measured**: recorded by the PushFPS / Veyra developers in this repository's documents or code comments on
  THEIR PCs (not the owner's);
- **published**: a vendor document or paper (linked);
- **estimate**: arithmetic from the above, never a measurement;
- **hypothesis**: needs the owner's run.

Nothing in this file was measured on the owner's RTX 5070, PS5 or capture card, and nothing was compiled on Windows by
this stream. Every millisecond below is a formula from the code with example values, not a reading.

Symbols: **T** = content interval of the pair (16.68 ms for a 59.94 fps stream or a 60 fps game, 33.4 ms for a 30 fps
game once duplicates are removed); **M** = multiplier of the pair (2X ... 6X); **base** = GPU time of the real frame's
own processing (upload, colour, clean-up, SR, NR, flow); **fg1** = GPU time of the FIRST generated frame of the pair;
**fg** = per generated frame. "Added" always means: compared with the same frame shown with frame generation OFF.

## 0. Short answer: the three largest sources

| # | Source | Inherent or avoidable | Size of one real frame's added delay [code formula; example estimate] | Where (code) | Field that measures it on his PC |
|---|---|---|---|---|---|
| 1 | **The wait for the next real frame.** Nothing between A and B can be made before B has arrived and been processed; B is shown (M-1)/M x T after the first generated frame could be | **inherent** to interpolation | (M-1)/M x T + fg1 + margin (0.5 ms phase margin + the 0-1 ms deadline margin on cards). 30 fps game: 2X 16.7, 3X 22.2, 4X 25.0, 6X 27.8 ms; 60 fps: 2X 8.3, 3X 11.1 ms; each + fg1 (VFG Medium at 1440p about 3.5 ms, CAPTURE_FG section 3, estimate) | `EngineController.cpp` pair delay (`pairLatency.select`, ~2684), `PresentationScheduler::deadline` (host + (pts - source) + delay), deadlines of generated frames `pts = interpolate(a,b,k,M)`; per-subframe fences in `EnhanceGraph.cpp` (VFG / DLSS / PushFlow loops, ~4270-4440) | capture: `[slipstream-capture] realLatP50Ms/P95/P99` FG arm minus `cap-native`; stream: `[slipstream-latency] pickupToPresentCallP50/P95` FG arm minus FG-off arm; NEW `[slipstream-smooth] addedEstMs` (the real frame's hold P50, an estimate) and `realHoldP95/P99Ms` (section 3) |
| 2 | **The FG phase without frame generation ("dead phase").** A pair whose generated frames are skipped (admission rejected it, or a FgCostSettling hold of 2, 4 or 8 s is running) still shows its real frame at the full FG phase. The delay of source 1 is paid and no generated frame is shown | **avoidable** (the hold exists only to keep the next admitted pair even) | Remote Play: T + base_P95 - base for EVERY skipped pair, always (LiveFgRejectionHold runs only with card timing or `VEYRA_EXP_RP_ADAPTIVE_PHASE=1`); capture: the learned phase, (M-1)/M x T + fg1 + margin, for every skipped pair of a mixed window and for the first 1 s of an all-rejected run. 60 fps stream: about 16.7 ms per affected frame; 30 fps card at 4X: about 29 ms (estimate) | `EngineController.cpp` ~2972-2989: `fgHeld` only when `marginPair` (= `adaptiveCapturePhase`, false for a PS5 stream unless `VEYRA_EXP_RP_ADAPTIVE_PHASE=1`); `liveTimeline.resetPair(..., options.fg&&!fgHeld?pairDelay:0)`; `FgCostSettling::holding` in the admission lambda (~2716); `LiveFgRejectionHold` (`LivePairLatency.h`: 1 s, 8 admitted pairs to resume) | `[slipstream-smooth] rejectedPairsFps`, `holds`; NEW `deadPhaseFps` (real frames shown on the FG phase with no generated frame of their pair shown) and `fallbackFps` (real frames shown when ready by a hold) |
| 3 | **Remote Play's fixed pair phase.** A PS5 stream shows its real frame a whole T plus the P95 of the base cost after decode; capture cards learn the phase instead | **avoidable** (the experiment `VEYRA_EXP_RP_ADAPTIVE_PHASE=1` already exists) | extra over source 1 = T/M + (base_P95 - base) - fg1 - 0.5 ms. 60 fps stream: 2X about 4.3 ms, 3X about 1.6 ms, 4X about 0.2 ms; a 30 fps game with stream duplicate removal (T = 33.4): 2X about 12.7, 4X about 4.3 ms; each + the base cost's P95-minus-current jitter (estimate with fg1 = 3.5 ms). At 2X it is the largest avoidable steady-state extra | `legacyPairDelay = liveInterval + processingAllowance` (~2667; `FgRecoveryBudget::processingAllowance` = base P95, at most T); `pairDelay = marginPair ? pairLatency.select(...) : legacyPairDelay` (~2684) | NEW `[slipstream-smooth] phaseMs` (the pair delay in force) and `phaseLearned=0|1`; the FG-on minus FG-off difference of `[slipstream-latency] pickupToPresentCallP50` |

So **"does FG add a whole source interval?"**: on a PS5 stream, yes, by default: T + base_P95 after decode, of which
(M-1)/M x T + fg1 is inherent and the rest (source 3) is not. On a capture card, no: the phase is learned
((M-1)/M x T + fg1 + margin), as the capture review confirmed. On a pair whose generated frames are not shown, the whole
phase is lost (source 2). FPS90.md's sentence "graph frame generation holds it one content interval at any multiplier"
is true for the Remote Play default only.

The next largest, after these three: **VSync back-pressure at the VRR ceiling** (180 output on a 180 Hz panel: a present
waits up to one refresh, 5.6 ms, for the panel; estimate and hypothesis, section 1.10). It is the reason to prefer 120
over 180 when both are smooth.

## 1. The owner's list, item by item

### 1.1 Does interpolation wait for future source frames? [code]

Yes, always, by construction: generated frame k of pair (A, B) has `pts = A + k/M x (B - A)` and every backend reads both
A and B (VFG: `vfgBackend_->generate(parity, sub, M, ...)` on the two enhanced frames `videoFrame_[0/1]`; DLSS path:
`fgBackend_->evaluate` with B as backbuffer and the A->B flow; PushFlow packs both). So the first generated frame needs
B, and B is shown after the generated frames. Deadlines (`PresentationScheduler::deadline`): real B at
`anchor + delay`, generated k at `anchor + delay - (M-k)/M x T`. The smallest delay that lets the first generated
frame be on time is `ready1 + (M-1)/M x T`, where `ready1 = anchor -> first generated frame finished` = B's own
processing + fg1 (each subframe has its own fence: `f.lease->readyFence=out.genFenceValue` after each subframe's submit,
so only the FIRST generated frame is on the critical path while each later one fits its own slot).

- Inherent part: (M-1)/M x T + fg1. B's own processing is not FG-added: with FG off B waits for it too.
- How the code learns it (capture cards): `LivePairLatency::select` keeps the P95 of the observed
  `ready_k - anchor + (M-k)/M x T` over the last second (at most 64 samples, at least 8) plus 0.5 ms, falls at most
  min(0.5 ms, T/M/8) per pair and rises at once; `LiveDeadlineMargin` adds 0-1 ms so 99 % of generated frames are on time.
- Remote Play: not learned unless `VEYRA_EXP_RP_ADAPTIVE_PHASE=1`: the delay is `T + base_P95` (source 3).

| (M-1)/M x T [estimate] | 2X | 3X | 4X | 5X | 6X |
|---|---|---|---|---|---|
| 30 fps game, T = 33.4 ms | 16.7 | 22.2 | 25.0 | 26.7 | 27.8 |
| 60 fps game, T = 16.7 ms | 8.3 | 11.1 | 12.5 | 13.3 | 13.9 |

Each row + fg1 (+ the margins). The step from FG off to 2X is the expensive one; every further multiplier adds less
(4X costs 8.3 ms more than 2X for a 30 fps game, 6X another 2.8 ms).

### 1.2 How many frames are buffered? [code]

| Place | Depth | Rule |
|---|---|---|
| Capture card driver | 2 (<= 1080p) or 3 allocator buffers suggested (`CaptureBuffer.h`, pref `captureBufferMode`; 1 = Minimum) | advisory to the driver; drained at once by the callback (CAPTURE_FG section 1); the driver's own hold is `[capture-driver-age]` |
| Capture callback -> engine | latest-frame mailbox (reader, pending, staging); an unread frame is overwritten (`dropped`) | no queue |
| PS5 stream decode -> engine | one-slot mailbox (`publishDecoded`, KBM_AUDIT section 1); ingress queue of 4 access units before decode (SYNC_FINDINGS section 2) | latest-frame |
| Engine read | the next source frame is NOT read while the previous batch's GPU work (including its whole generation group) is still running (`enhancementPending`, ~2067: "never stack a second frame behind one the GPU is still enhancing") | a slow group makes the next frame wait in the mailbox (latest wins), it does not queue |
| Presentation scheduler | 2 batches (`LiveGpuScheduler`, capacity 2): pair (A,B) presenting while (B,C) is processed | bounded; `push` fails above 2 |
| DXGI | 3 swap-chain buffers, flip-discard, waitable object; `SetMaximumFrameLatency(1)` while pacing (VSync / VRR, the smooth-pacing default), 3 otherwise (`PresentSink::configurePacing`) | generated frames wait for capacity at most min(1-3 ms, half an output interval) and are dropped (`dropCapacityFps`); the real frame waits |

So the code holds **one real frame per pair on purpose** (B, until its generated frames have been shown): that is
source 1. No queue grows: every stage is latest-wins or bounded at 1-2 entries. Nothing here is a hidden triple buffer.

### 1.3 Does FG add an entire source interval? [code]

See section 0: Remote Play by default yes (T + base_P95, source 3 is the part above the inherent minimum); capture no
((M-1)/M x T + fg1 + margin); a pair without shown generated frames loses the whole phase (source 2).

### 1.4 Do frames accumulate in CPU or GPU queues? Do fences stall? [code]

- CPU: no accumulation (section 1.2). The engine thread is the single GPU owner and presents on deadlines between
  submissions (`LiveGpuScheduler::advance`). While it records and submits the next pair, frames that fall due are
  presented at checkpoints after each submit (`presentDuringProcess`, `previewCheckpoint`), **except with HDR output**
  (`presentDuringProcess` returns when `graph->hdrOutput()`): then a frame due during the submit waits until the submit
  returns (late by up to the CPU submit time, `CpuStage::Submit`; hypothesis: about 1-3 ms). Field: `[fg-deadline]
  decisionLateMs` (one sample per 10 s), NEW `presentLateP95Ms`.
- GPU: one graph queue (base work, then each subframe) and an independent presentation queue (normal priority,
  `VideoPresenter.cpp`; high priority only with the test switch `VEYRA_TEST_PRESENT_QUEUE_PRIORITY=high`). VFG runs in
  CUDA with external semaphores: the graph queue waits on the GPU for each VFG run, the CPU never waits
  (`VfgBackend.cpp`, `cuWaitExternalSemaphoresAsync` / `directQueue()->Wait(completion)`; upstream-measured host cost
  0.13 ms per run). The presentation blit shares the GPU with the next pair's work: while pair (B,C)'s base work and
  VFG run, the blit of (A,B)'s generated frame k and of B compete for the GPU (time-slicing, not a queue order).
  Size unknown: hypothesis, visible as `gpuBlitP95Ms` against its P50 and in PresentMon's `MsGPUActive`.
- A slow generation group (base + (M-1) x fg longer than T) delays the next read (section 1.2): the next pair starts
  late, its readiness is later, and on a card the learned phase grows with it. That is the GPU limit, not a queue:
  `[slipstream-smooth] gpuSpanP95Ms` against `pairBudgetMs`.

### 1.5 Are generated frames presented too late? [code]

A generated frame is shown at its deadline or dropped: `timeline.expired(pts, now, grace)` with grace 10 ms past the
later of its smoothed and callback due time (`kLiveGeneratedGrace100ns`), the output cap's slot test
(`rateSkipsCandidate`) and the capacity wait. So a generated frame can be up to 10 ms late (it is then shown late, which
moves its picture in time) or dropped (`dropDeadlineFps`). A real frame is never dropped; a late real frame is shown late.
The learned phase keeps 95 % (plus the margin's 99 %) on time on cards. Fields: `dropDeadlineFps`, `[fg-deadline]`,
NEW `presentLateP95Ms` (Present call minus deadline, real and generated).

### 1.6 Does interpolation block input handling? [code, from KBM_AUDIT]

No direct coupling: the pad / mouse poll runs every 4 ms on the interface thread, hands its state to the Remote Play
session under a short lock and never waits on the engine or the GPU (`EngineController::remotePlayController`; the engine
`mutex_` is held only for snapshot assignments). With a capture card the controller is on the PS5 and the PC never sees
it. Indirect couplings (hypothesis): FG's GPU load can slow the session owner's hardware decode of the next frame
(shared GPU, no lock: `[slipstream-latency] arrivalToDecodedP95`) and the interface thread's own work (QML), which
delays the poll (`[slipstream-input-poll] pollGapMsP95/P99`, FG on against off). What interpolation does by design: it
shows the console's answer to an input later (source 1); generated frames carry no newer input.

### 1.7 Is optical flow a significant delay? [code, upstream-measured, published]

- VFG (the default backend): the flow is inside NVIDIA's model; there is no separate flow stage, its cost is inside fg1.
  NVIDIA publishes 1.32 ms (RTX 4090) / 1.90 ms (RTX 5090) for VFG Medium 2X at 1280x720
  ([VFX SDK performance reference](https://docs.nvidia.com/maxine/vfx/1.3.0/WindowsVFXSDK/PerformanceReference.html)).
- DLSS path / FSR / ANR: the graph's own flow (NVIDIA Optical Flow when built with the NVOF headers, else AMD FidelityFX
  optical flow; `FlowRefine` / `FlowFill`) runs as part of B's base work, so it is on the critical path of fg1:
  upstream-measured 1.36 ms, 2.44 ms with refinement, at 1080p (CAPTURE_FG section 3). Field: `gpuFlowP50/P95Ms`.
- So flow is 1-2.5 ms of the first generated frame's readiness, not a frame of delay.

### 1.8 Do cadence changes create extra buffering? [code]

- After a settings change or a hard reset the learned phase starts again from the legacy delay (`pairLatency.reset()`,
  ~2540): until 8 pairs have been observed within a second, a card pays T/M + base_P95 - fg1 more (about 0.25 s at 30 fps).
- A GPU spike raises the phase at once (the P95 of the last second) and it comes back by at most 0.5 ms per pair
  (`LivePairLatency::select`, `step`): a 10 ms spike costs extra latency for about 20 pairs after it has left the window.
- Content-rate changes (a 30 fps game that runs 60 for a while, duplicate removal relearning after a stall: CAPTURE_FG
  section 2, `ContentDedup`) change T, hence the phase, immediately; the multiplier follows the content rate per pair
  (cap reduction, adaptive gate 0.5 s). No queue is involved.
- FgCostSettling: a cold probe that misses holds generation off for 2, 4, then 8 s; during the hold source 2 applies.
- Duplicate removal itself: for a 30 fps game in a 60 Hz signal it doubles T (33.4 instead of 16.7 ms), so
  interpolating real game motion costs (M-1)/M x 33.4 instead of interpolating between repeated pictures; inherent to
  interpolating a 30 fps game (without it, every second pair is between identical pictures: `stillGenFps`).
- The dedup content anchor's half-slot centring (`LiveArrivalSmoother::contentAnchor`, + T_signal/2) is learned back by
  the phase on the content clock (CAPTURE_FG section 4); compare `cap30-fg4` with `cap30-fg4-nodedup`.

### 1.9 Swap-chain depth, frame latency waitable object [code]

Flip-discard, 3 buffers, the waitable object, maximum frame latency 1 while pacing (smooth pacing on: VSync + VRR, the
default since 3.1.6), 3 otherwise; tearing only when the user picks display sync Tearing (or with the experiment `VEYRA_EXP_PRESENT_TEARING`, which never
applies with FG: `PushTearingInputs`). With maximum latency 1 at most one present is queued in DXGI, so the swap chain adds no frame of
its own; what remains is the wait for the vblank (1.10). With display sync Automatic (Present(0,0), latency 3) unshown
presents are replaced (smooth-pacing comment in CLAUDE.md), a pacing, not a latency, problem. Fields: `[slipstream-latency]
syncInterval queueLatency waitable flip`.

### 1.10 VSync back-pressure at the VRR ceiling [code, estimate, hypothesis]

Under VRR the panel refreshes when a present arrives, but never faster than its maximum (179.95 Hz: 5.557 ms). An output
of 180 FPS (60 x 3X, 30 x 6X) presents every 5.556 ms: at the ceiling, so any present that comes early (deadline jitter,
the 0-1 ms margins) waits for the panel, and with maximum latency 1 the NEXT present waits for that flip (the capacity
wait: generated frames are dropped after 1-3 ms, the real frame waits). Expected [estimate]: 0 to 1 refresh (0-5.6 ms)
on frames at the ceiling, about half a refresh on average, plus about one dropped frame every 20 s from 180 - 179.95
(SMOOTHNESS_ANALYSIS "Further causes"). At 120 FPS on the same panel every present has 2.8 ms of headroom: no wait
[estimate]. With FG OFF (60 FPS) there is no back-pressure either, so at the ceiling this is FG-added. Without VRR, a fixed
refresh adds 0-1 refresh at every rate (FG off too). Fields: `[slipstream-latency] presentCallP95` (Present blocking),
`dropCapacityFps`, PresentMon `MsUntilDisplayed`; the panel's own processing only the camera sees.
**This is the measurable reason for "120 before 180" (hypothesis until the run).**

## 2. Totals per arm (estimate, fg1 = 3.5 ms VFG Medium at 1440p, margins 0.5-1.5 ms)

Added delay of a real frame against the same frame with FG off, PC side only (callback / decode to Present return):

| Source -> target | M | Inherent (M-1)/M T + fg1 + margin | Capture card (default: learned phase) | Remote Play default (T + base_P95) | Remote Play, avoidable part |
|---|---|---|---|---|---|
| 30 -> 60 | 2X | 16.7 + 3.5 + ~1 = ~21 | ~21 | needs `VEYRA_EXP_RP_STREAM_DEDUP=1` (by default the stream is already 60 with every picture twice); then T = 33.4: 33.4 + base_P95 | 33.4/2 - 3.5 - 0.5 = ~12.7 |
| 30 -> 90 | 3X | 22.2 + 3.5 + ~1 = ~27 | ~27 | needs `VEYRA_EXP_RP_STREAM_DEDUP=1`; then T = 33.4: 33.4 + base_P95 | 33.4/3 - 3.5 - 0.5 = ~7 |
| 30 -> 120 | 4X | 25.0 + 4.5 = ~30 | ~30 | with stream dedup: 33.4 + base_P95 | 33.4/4 - 4 = ~4.3 |
| 30 -> 180 | 6X | 27.8 + 4.5 = ~32 | ~32 | with stream dedup: 33.4 + base_P95 | 33.4/6 - 4 = ~1.6 |
| 60 -> 90 | - | not a whole multiplier (FPS90.md): no even 90 from 60 | | | |
| 60 -> 120 | 2X | 8.3 + 4.5 = ~13 | ~13 | 16.7 + base_P95 - base | ~4.3 + jitter |
| 60 -> 180 | 3X | 11.1 + 4.5 = ~16 (+ VRR ceiling 0-5.6, 1.10) | ~16 + ceiling | 16.7 + base_P95 - base (+ ceiling) | ~1.6 + jitter |

For every pair whose generated frames are not shown, add the whole column value with no smoothness gained (source 2).
Compared with FG off the step 0 -> 2X costs 8-21 ms and is inherent; the avoidable extras of the fixed phase are 0-13 ms
in steady state (largest at 2X and for a 30 fps game) and 13-33 ms per frame of a skipped pair. Nothing here is measured; the run decides (section 4).

## 3. What the logs measure today, and what is missing

| Quantity the owner asked for | Present today | Added by this stream to `[slipstream-smooth]` (`VEYRA_EXP_RP_STATS=1` only; PushFgLatency.h `PushFgLatencyWindow`) |
|---|---|---|
| Real source FPS | `[slipstream-smooth] uniqueFps`, `contentFps`; capture `[slipstream-capture] uniqueFps` | |
| Generated FPS (never repeats) | `generatedFps` (valid generated frames), capture `motionGenFps` / `stillGenFps` | |
| Presented FPS | `[slipstream-cadence] submittedFps realFps generatedFps` (Present calls); PresentMon for the panel | |
| Source frame age at presentation | capture: `realLatP50/P95/P99Ms`; stream: `[slipstream-latency]` decoded->pickup->present call, no total | `realAgeP50/P95/P99Ms`: decode -> Present return of real frames (streams; a card writes -1 because its `realLat` is the same measure) |
| Future-frame wait | not separated | `addedEstMs` (P50) and `realHoldP95/P99Ms`: a ready real frame's wait (CPU observation of its fence -> Present call) for its FG deadline |
| FG GPU time | `gpuFgP50/P95Ms` (whole group), `gpuFlow*` | `firstReadyP50/P95Ms`: decode / callback -> the first generated frame seen ready (base + fg1, CPU observation) |
| CPU/GPU queue delay | `[slipstream-latency] decodedToPickup` (stream), `[capture-timing] readAgeMs` (card) | |
| Presentation delay | `[slipstream-latency] presentCallP50/P95` | `presentLateP95/P99Ms` (Present call minus the deadline, every paced frame) |
| Dropped / repeated | `dropCap/Deadline/CapacityFps`, capture `repeatedFps`, `missedNewFps`, `stillGenFps` | `deadPhaseFps` (FG phase paid, no generated frame shown), `fallbackFps` (shown when ready by a hold) |
| Estimated added latency | nothing | `addedEstMs`, labelled an estimate: the P50 of a ready real frame's hold; it leaves out the GPU time FG takes from the frame's own processing, the display and everything outside the PC. The measured PC-side difference is an arm comparison: `realAge` (stream) or `realLat` (card), FG arm minus FG-off arm |
| Phase in force | `[live-anchor] phaseMs` (cards only, every 1 s) | `phaseMs` (P50 of deadline minus decode / callback of real frames), `phaseLearned` in the 5 s window for both paths |

## 4. What the owner's run must show (hypotheses to confirm or refute)

1. Source 1: `addedEstMs` close to (M-1)/M x T + fg1 - base on a card; `realLatP50Ms` FG arm minus `cap-native` close
   to section 2's column (internal, Present return, not photons).
2. Source 2: `deadPhaseFps` > 0 whenever `rejectedPairsFps` > 0 or `holds` > 0 on Remote Play (it should equal the
   rejected pairs' real frames), and whether the 60 <-> 90-100 phases of FPS90 coincide with it.
3. Source 3: on Remote Play at 2X, `phaseMs` about T + base_P95 with `phaseLearned=0` and `addedEstMs` about
   T + base_P95 - base; with the learned phase (`VEYRA_EXP_RP_FG_ULL=phase`), about 4 ms lower at 2X.
4. VRR ceiling: `presentCallP95`, `presentLateP99Ms` and `dropCapacityFps` at 180 against 120 on the same scene (and PresentMon).
5. The camera procedure (CAPTURE_FG section 9) for input-to-photon: only it includes the console, HDMI, panel and the
   game's own frame time.

## 5. Fixes (exp/s6-fglat; each behind `VEYRA_EXP_RP_FG_ULL`, off by default; unverified on Windows; nothing measured)

### 5.1 Part `hold`: no dead FG phase (source 2)

What it does [code, `PushFgLatency.h` `PushFgFallbackHold`, `EngineController.cpp` where `fgHeld` is decided]: a pair's
real frame goes out when ready (deadline = its anchor, no FG phase) while no generated frame of the recent pairs is
shown:
- at once while FgCostSettling holds generation off (the budget then admits no pair for 2, 4 or 8 s: the whole hold
  is known to have no generated frames);
- after 250 ms of nothing but skipped pairs (instead of LiveFgRejectionHold's 1 s, which also never runs on a PS5
  stream unless `VEYRA_EXP_RP_ADAPTIVE_PHASE=1`);
- the phase returns after 8 admitted pairs in a row; pairs admitted meanwhile (the budget's probe) stay held, their
  generated frames expire unshown and their cost is still measured (as LiveFgRejectionHold does).

Expected effect [estimate]: during a hold, each real frame is shown phase - base earlier: about 16.7 ms on a 60 fps PS5
stream (T + base_P95 - base), about 27 ms on a 30 fps card at 4X ((M-1)/M x T + fg1 + margins - base). Outside holds and
runs of skipped pairs: nothing changes. The output rate in a hold is unchanged (real frames only, as before).

**What a default-on version would change for pacing (the orchestrator's question).** Showing the real frame earlier
moves the whole presentation timeline by Δ = phase - base. Model of the code (test
`fglat_hold_model_shows_two_uneven_intervals_per_hold`: 30 fps card, 4X, phase 29.5 ms, base 2 ms, a 1 s hold):
- entering: ONE interval shorter by Δ (33.4 -> 5.9 ms between the last real frame on the phase and the first held one,
  which shows a whole T more motion): the picture jumps Δ forward in time once;
- inside the hold: every interval is T again (60 or 30 even real frames; the generated frames were missing anyway);
- leaving: the picture stands still for Δ once. Counting real frames only, one interval is longer by Δ (33.4 -> 60.9
  ms in the model); with the generated frames back, the first one comes T + fg1 + margins after the last held real
  frame (about 37.9 ms in the model, where a held 30 fps stream had 33.4) but shows only T/M more motion, and from
  there on the output is even FG on the phase again.
So the cadence is uneven exactly twice per hold, at the switch points, never inside it, and a single skipped pair
between admitted ones does not switch (two uneven intervals for one missing group would be worse than one dead
phase). LiveFgRejectionHold, which is on by default for capture cards today, already has the same two uneven
intervals, only after 1 s. What the owner's run must show before this can be a default (in an arm with only this part on, `VEYRA_EXP_RP_FG_ULL=hold`;
the kit's ULL arms turn on every part, section 6): `[fg-ull] entries` per
minute (each costs two uneven intervals; a hold every few seconds would be visible as hitches), the frame-time
spike at the switch points in `[slipstream-cadence] intervalMaxMs` / PresentMon, and `realLat` / `realHold` during
holds against the default (it should drop by about Δ). If entries are rare (FgCostSettling holds are 2-8 s each), the
pacing cost is the same two intervals as today's hold and the latency gain is Δ per frame for the whole hold.

**Audio [code, estimate].** Automatic audio sync follows the measured delay of the shown REAL frames (capture:
`captureSource.videoPresented` per real frame; Remote Play: `remote->videoPresented`, CAPTURE_FG.md section 1 and
SYNC_FINDINGS.md). A hold changes that delay by Δ at its start and back at its end; the audio schedule trims at most
5000 ppm (5 ms per second) and restarts the sound only above a 60 ms error, so after each switch the sound is up to Δ
(17-28 ms) off the picture and catches up within about Δ / 5 ms seconds (3-6 s) without a restart. Today's
LiveFgRejectionHold on capture cards does the same. Whether that is audible is the owner's judgement; the fields are
`[audio-sync]` / `[remoteplay-audio] targetMs` around each `[fg-ull] held=` line.

### 5.2 Part `phase`: the learned phase on PS5 streams (source 3)

`VEYRA_EXP_RP_FG_ULL=phase` (or `1`) turns on what `VEYRA_EXP_RP_ADAPTIVE_PHASE=1` does: the stream's pair phase is
`LivePairLatency::select` against the decode time (`watch->phaseAnchor = liveInputReady`), with the 0-1 ms
`LiveDeadlineMargin`, instead of `T + base_P95`. Expected [estimate]: the real frame earlier by T/M + (base_P95 - base) -
fg1 - margins: about 4 ms at 60 -> 120, 1-2 ms at 3X. Pacing: the learned phase is the one capture cards use by default;
on a stream the decode times jitter (FPS90.md P12) and are not smoothed (`smoothAnchor` excludes streams), so generated
frames whose readiness was learned against a jittery anchor may expire more often than with the fixed phase, which had
T/M of slack (hypothesis: `dropDeadlineFps` against the default arm). Not a default: the reasons are the jitter and that
nothing has run.

### 5.3 Part `headroom`: below the VRR ceiling (section 1.10)

Smoothness Mode with an automatic target of the largest of 180 / 120 / 90 / 60 that is at most 97 % of the refresh, only
under confirmed VRR and only when no target is set by the user or the kit. On the owner's 179.95 Hz panel: 120. It lowers
the multiplier (never below 2X): a 60 fps game 2X instead of 3X (inherent hold 8.3 instead of 11.1 ms, and no present at
the ceiling), a 30 fps game 4X instead of 6X (25.0 instead of 27.8 ms). It never raises the multiplier above the user's.
It also reuses Smoothness Mode's step-down on load and on rejected pairs (the "reduce workload when deadlines are missed"
of the owner's mode). The price is the output rate: 120 instead of 180. The owner's rule "120 before 180 if 180 is less
responsive" is what this part tests; the run must show whether 180 really costs latency (section 4, item 4).

### 5.4 Part `grace`: a late generated frame is dropped, not shown late

The default shows a live generated frame up to 10 ms past its deadline. With the part on, half an output interval
(4.2 ms at 30 x 4X and 60 x 2X, 1 ms floor), plus the callback-to-anchor distance as before. A frame that late moves its
picture in time and leaves the next frame less than half an interval; under explicit VSync the real frame can then miss
its refresh (it waits for capacity). Expected: slightly more `dropDeadlineFps`, fewer real frames presented late
(`presentLateP95Ms`, section 3), no change when the GPU keeps up. Known limit: admission (`FgRecoveryBudget::canAdmit`)
still admits a group whose last frame is predicted up to 10 ms late, so under overload the GPU can make frames this part
then drops; the headroom part's step-down (on predicted load and rejected pairs) is what lowers that work.

### Not changed, with the reason

- The precise deadline wait for streams still ignores the stream's frame and fence events (`waitLive`): passing them
  would pick a new frame up at most about 1 ms earlier, and the comment there records that the stream's pacing was tuned
  without them; not worth the pacing risk without a measurement.
- The HDR output path keeps no present checkpoints during a submit (`presentDuringProcess`): the reason is not written in
  the code and cannot be checked without a Windows build; measured by `presentLateP95Ms` with HDR on.
- The independent presentation queue stays at normal priority; `VEYRA_TEST_PRESENT_QUEUE_PRIORITY=high` already exists for
  an A/B arm.
- No buffer is removed: every remaining one is either latest-wins or needed (section 1.2).

### 5.5 How the owner turns the mode on

Experiment only, off by default, no interface row (the brief: a quick-setup row only once a run shows it is a real mode
with a measured purpose; nothing has run, so there is no row and no new translated string). Either the kit's arms
(section 6) or one line in `slipstream.ini` next to PushFPS.exe: `VEYRA_EXP_RP_FG_ULL = 1` (all parts) or a list such as
`VEYRA_EXP_RP_FG_ULL = hold,phase` (the file passes `VEYRA_EXP_RP_*` keys to the environment; docs/SETUP.md). Remove the
line to return to the default. The log says what is on: `[fg-ull] experiment: Ultra Low Latency Frame Generation
parts=... understood=1`.

## 6. FG OFF against FG ON: the kit's arms and the camera procedure

**Inside the PC (the kit, `VEYRA_EXP_RP_STATS=1`, software times; never input-to-photon):**

| Session | FG off | FG on, default | FG on, Ultra Low Latency | What the difference shows |
|---|---|---|---|---|
| `-Session smoothness` (Remote Play) | `sm-native60` | `sm-fg2-vrr`, `sm-fg3` | `sm-fg2-ull`, `sm-fg3-ull` | `realAgeP50/P95/P99Ms` and `addedEstMs` FG on minus off (sources 1 + 3), ULL minus default at 2X (source 3 removed, expected about 4 ms less); `sm-fg3-ull` runs 2X under VRR at 180 Hz (headroom): 120 against 180 |
| `-Session capture` (card) | `cap-native` | `cap-fg4`, `cap-fg6` | `cap-fg4-ull`, `cap-fg6-ull` | `[slipstream-capture] realLatP50/P95/P99Ms` and `addedEstMs`; with a 30 fps game `cap-fg6-ull` runs 4X under VRR: 120 against 180 |

**The ULL arms turn on all four parts at once** (`VEYRA_EXP_RP_FG_ULL=1`), and in `sm-fg3-ull` / `cap-fg6-ull` the
headroom part also changes the multiplier (3X -> 2X, 6X -> 4X under VRR at 180 Hz). So these arms answer "does the whole
mode help", not which part does: before any single part can become a default it needs its own arm (for example
`VEYRA_EXP_RP_FG_ULL=hold` against the same arm without the switch), measured on its own.
**Those arms now exist** (the kit's `fglat` set, 22 Nov 2026; docs/HARDWARE_TEST_PROCEDURE.md "Frame generation latency,
part by part"): `flt-off`, `flt-fg2` (2X as installed), `flt-fg2-phase`, `flt-fg2-hold`, `flt-fg2-grace`,
`flt-fg2-headroom` (one part each against `flt-fg2`), `flt-fg2-ull` (all parts), `flt-fg2-ull-prio` (all parts +
`VEYRA_TEST_PRESENT_QUEUE_PRIORITY=high`, section 1.4's presentation-blit contention) and `flt-fg2-ull-low` (all parts +
VFG Low, a smaller fg1), about 25 minutes inside `-Session smoothness -Pass 120`. No engine code changed for them. Under
VRR at 120 Hz the headroom part (so every `-ull` arm) runs Smoothness Mode with target 90 (`pushUllHeadroomTarget`):
a 60 fps game stays 2X, but Smoothness Mode may step down and measures pair costs, so compare `chosen=` with `flt-fg2`.

Every arm: `deadPhaseFps` and `fallbackFps` (source 2; `fallbackFps` counts real frames shown when ready because a hold
held frame generation off, LiveFgRejectionHold or the ULL hold), `presentLateP99Ms`, `dropDeadlineFps` (the grace part's cost),
`[slipstream-cadence]` intervals (the pacing cost of the hold's switch points), `[fg-ull]` lines. A run in which
generation never misses (no rejected pairs, no holds) shows no difference from the hold part: that is the expected
result there, not a failure.

**Input-to-photon (the owner's manual step; only a camera sees the console, the game, HDMI and the panel):** the
procedure of CAPTURE_FG.md section 9 (a 240 fps phone camera seeing the input and the screen; count camera frames from
the input to the first changed pixel), with these arms, interleaved, the same scene and PS5 output settings, PushFPS
started normally WITHOUT the kit's `VEYRA_EXP_RP_STATS` (its picture compare biases the capture arms):
- **A** FG off; **B** FG on as installed (2X for a 60 fps game, 4X for a 30 fps game); **C** the same with
  `VEYRA_EXP_RP_FG_ULL = 1` in `slipstream.ini`; **D** (optional) B at the 180 target (3X / 6X) against C, which runs 120.
- Remote Play with mouse and keyboard: a quick mouse flick from rest, counted to the first frame in which the camera
  moves (the console's own response included); the PS button's control centre for a game-independent press. Capture
  card: the controller's button as in CAPTURE_FG.md.
- Presses: B - A is about 8-30 ms (section 2): 30 presses per arm show it. C - B on Remote Play at 2X is about 4 ms and
  needs about 120 presses per arm (CAPTURE_FG.md section 4's estimate for a 4 ms effect at 16.7 ms quantisation); on a card
  C - B is 0 unless pairs are skipped (the hold) or the headroom lowers the multiplier.
- Report median, P95 and the difference of B, C, D against A, in `camera-template.csv`.

## 7. Causal extrapolation and hybrid switching: verdict (deliverable 3)

### 7.1 What the published methods need, and what NVIDIA's components allow

- **NVIDIA VFG (the default backend) only interpolates.** Its inputs are the previous frame (t = 0) and the current frame
  (t = 1); the time is i/M or an explicit timestep "in the open interval (0.0, 1.0)", and each run produces a frame strictly
  between the two inputs ([VFX SDK, Video Frame Generation](https://docs.nvidia.com/maxine/vfx/1.3.0/Filters/VideoFrameGeneration.html),
  published). VFG cannot be asked for t > 1, and it exposes no flow field. With VFG there is no extrapolation path at all.
- **NVIDIA Optical Flow (the hardware the DLSS path can use).** NVIDIA's application note lists Turing (except TU117),
  Ampere and Ada and gives 1080p optical-flow rates (Ada, 4x4 grid: 536 / 1000 / 1296 frames per second for the slow /
  medium / fast presets, about 0.8-1.9 ms a frame); its frame-rate up-conversion library is also interpolation ("time taken
  by NVOFFRUC library for interpolating one frame", 4.41 ms on an RTX 4090 at 1080p); Blackwell is not named on that page
  ([NVOF application note](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-application-note), published).
  NVIDIA states that DLSS 4 frame generation no longer uses the optical flow accelerator (cited in
  docs/PS5_REMOTE_PLAY_EXPERIMENTS.md). Our build uses NVOF only when the build had the SDK headers; otherwise AMD
  FidelityFX optical flow (CLAUDE.md).
- **DLSS frame generation** interpolates too and is specified for game data (depth, motion vectors, HUD-less colour, Reflex):
  CAPTURE_FG.md section 5. **Reflex 2 Frame Warp** warps by the game's own camera: not available to a video application
  (docs/PS5_REMOTE_PLAY_EXPERIMENTS.md).
- **GFFE** (G-buffer-free frame extrapolation, Wu et al., ACM TOG 2024): extrapolation methods "do not introduce additional
  latency as opposed to frame interpolation methods"; GFFE analyses "the motion of dynamic fragments and different types of
  disocclusions" and adds "a light-weight shading correction network" ([Intel's article on GFFE](https://www.intel.com/content/www/us/en/developer/articles/technical/gffe-g-buffer-free-frame-extrapolation.html),
  published; the paper's PDF could not be fetched from this environment, so its runtime figures are not quoted). No
  implementation is published (previous note), and it is a trained network.
- **Oculus ASW** extrapolates linear motion well and non-linear motion worse (docs/PS5_REMOTE_PLAY_EXPERIMENTS.md).

### 7.2 What our existing optical flow allows

The graph's own flow (`fgBaseFlow_` / `baseFlow_`, current-to-previous at the flow size, `FlowRefine` / `FlowFill`) exists
only for the backends that consume it (DLSS path, FSR, ANR: `desc_.fgUsesFlow()` in `EnhanceGraph.cpp`), not for VFG or
PushFlow (each computes its own motion inside the model). An extrapolation prototype "warp the latest real frame forward
by its last flow field for the slots before N+1 arrives" would therefore need: the DLSS-path flow; a NEW forward-splat
shader (collisions resolved by depth proxy, crack and hole fill: the previous note's design); and an inverted schedule in
`EngineController.cpp` (today every generated frame of a pair is presented BEFORE its real frame: the batch order and
`PresentationScheduler` deadlines `B - (M-k)/M x T`), with frames AFTER the real one at `B + k/M x T`, cancelled when the
next real frame arrives early, and their own budget rules.

### 7.3 What it could gain, stated honestly

- Against interpolation: the real frame goes out when ready, as with FG off: the whole of source 1, (M-1)/M x T + fg1
  (8-28 ms, section 2), and source 2 cannot occur. That is the only latency gain.
- Against FG off: none for what the console shows. An extrapolated frame is a guess from frames already shown; it carries
  no newer game state, and the game still answers on its own frame grid (33 ms at 30 fps). Responsiveness is FG off's
  at best; the gain over FG off is smoother motion between real frames.
- Its errors fall where the owner cares most: at the start and the end of a camera turn (aiming). With constant velocity a
  turn that stops is continued for up to (M-1)/M x T (25 ms at 30 fps 4X, three output frames) and then snaps back.

### 7.4 What our own offline studies measured (synthetic CPU material; not game footage, not a GPU, not the owner's PC)

- `tools/fg_extrapolation_study/study.py` (60 -> 120, PSNR of the generated frame against the true instant): steady pan
  36.2 dB extrapolated against 38.9 interpolated; turn start / stop 27.2 against 34.5; bounce 21.8 against 23.3
  (docs/PS5_REMOTE_PLAY_EXPERIMENTS.md).
- `research/predictive-fg` branch (`docs/PREDICTIVE_FRAME_GENERATION_RESEARCH.md`, 30 -> 60 through x264): extrapolation
  beats repeating the last frame on 98.5 % of frames; its gap to interpolation is 0.2-0.75 dB in steady motion, 1.5 dB with
  occlusion, 2.3 dB on a fast pan and 4.1 dB on the turn that starts and stops; an acceleration model fixes the turn but
  costs 0.3-1.3 dB elsewhere; the confidence fallback did not help on average; a per-frame classifier of "nearly as good as
  interpolation" generalised worse than a constant rule (leave-one-sequence-out error 32.7 % against 20.6 %). The same
  branch's pass criteria for integration: at least 12 ms median end-to-end saving at 30 -> 60, 5th-percentile PSNR within
  3 dB of interpolation on steady scenes, visible artefacts in fewer than 1 of 20 generated frames in a blind A/B of fast
  turns and particles, GPU cost no higher than interpolation's.

### 7.5 Verdict: no prototype in the engine now (proof, and when to revisit)

1. The owner's backend (VFG) cannot extrapolate (7.1); a prototype could only run on the DLSS path's flow.
2. That prototype is a new shader plus an inverted presentation schedule, several hundred lines of pacing code written with
   no shader compiler, no GPU and no Windows build here; a shader that does not compile breaks the whole build
   (docs/PS5_REMOTE_PLAY_EXPERIMENTS.md "Why it is not in the engine yet"). No measurement could tell a gain from a bug.
3. Its only latency gain is to reach FG off's latency for real frames. The latency-first behaviour when generation cannot
   help, showing the real frame when ready, is built without any guessing: part `hold` (5.1). For the steady state the
   inherent cost stays: interpolation needs the next frame, and only the owner can choose to trade 8-28 ms for smoothness.
4. The evidence predicts its worst errors at aim start / stop (7.3, 7.4), the case the owner put first.
Revisit when a Windows build loop exists AND the owner has lossless 60 fps captures of his games (ground truth for the
offline quality pass), and integrate only if the research branch's four criteria (7.4) are met; then behind a switch.

### 7.6 Hybrid switching (interpolate / extrapolate / show the real frame)

- Built now: **show the real frame by the deadline** when generation cannot help (part `hold`), **drop a generated frame
  that would be late** (part `grace`), and **lower the work when deadlines are missed** (part `headroom` through Smoothness
  Mode's step-down). Remaining interpolation is the default.
- Not built: the extrapolation leg (7.5). A switch between interpolation and extrapolation moves the real frame by the whole
  phase Δ (17-28 ms at 30 fps), each switch one jump or one stall (the model of 5.1), so it can only be a slow decision
  (seconds, hysteresis): the earlier design note's "extrapolate while the player gives input, interpolate after about two
  seconds without input". The offline study found no per-frame feature that predicts when extrapolation is good enough,
  so a per-frame "confidence" switch is not supported by any evidence we have.
