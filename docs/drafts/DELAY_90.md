# The delay of a 30 FPS game shown at 90 FPS at 4K (stream "delay90", 23 Nov 2026)

> **Stream "lagfix" (23 Nov 2026): `VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY` is inert.** A HIGH graph queue with a NORMAL
> presentation queue can make a real frame's blit wait behind the next pair's graph work; nothing short of the owner's
> PC can show whether raising both helps. The value is still read and logged (`[graph-priority] ... selected=NORMAL
> disabled: can make playback worse`); the queue stays NORMAL.


Scope: the owner's nineteenth and twentieth messages. His setup: 3840x2160 processing, a 30 FPS game, frame generation to
a stable 90 FPS (3X), about 30 ms reported delay, a 2560x1440 180 Hz screen, an RTX 5070. His firm rule (twentieth
message): **4K processing is kept**. Lower sizes, lower frame-generation quality, 2X / 60 FPS and frame generation off
appear here only as labelled DIAGNOSTIC references, never as a solution. Base: exp/rel-12a (Package 12A); branch
exp/s6-delay90. Read first and not repeated: FG_LATENCY.md (the delay audit), CAPTURE_FG.md sections 3-4, FPS90.md.

Labels: **code** (read in this repository, file and function named); **upstream-measured** (recorded by the PushFPS /
Veyra developers in this repository on THEIR PCs, never the owner's); **estimate** (arithmetic, never a measurement);
**hypothesis** (needs his run). Nothing here was measured on his PC, nothing was built or run on Windows by this
stream, and no improvement is claimed. Every millisecond is a formula with example values.

Symbols: T = the pair's content interval (33.4 ms for a 30 FPS game once repeated pictures are removed); M = 3;
base = the real frame's own processing (upload, colour, clean-up, SR, NR, flow); fg1 = the first generated frame's time;
"interp" = (M-1)/M x T = 22.2 ms at 3X.

## 0. Short answer

1. **The working size really is 4K in both of his likely cases** [code]: `ResolutionPlan::make` sets
   `fg = output = base`. A 4K capture (SR off): source = base = NR composite = FG = output = 3840x2160. Remote Play 1080p
   with the upscaler targeting 4K: SR evaluates 3840x2160 and everything after it works at 3840x2160. The presenter
   then scales 3840x2160 into the 2560x1440 window. The new always-on `[slipstream-sizes]` line prints all of it (section 2).
2. **The delay of a real frame at 3X is base + fg1 + 22.2 ms + margins + present lateness.** 22.2 ms is fixed for
   interpolation at 3X. On a capture card the margins are the learned phase's 0.5 ms + the 0-1 ms deadline margin + the
   gap between the P95 and the median of readiness. With upstream-measured figures (section 3) the sum is about
   28.5-33 ms [estimate]: **his 30 ms reading is close to the floor at 4K**. What is left to remove without changing the
   picture is small on a capture card (estimate: 0-3 ms: margins, pickup wait, a possible second copy of the picture)
   and larger on Remote Play (estimate: 4-7 ms: the fixed pair phase, removed by the existing `VEYRA_EXP_RP_FG_ULL`
   part `phase`).
3. **fg1 at 4K is the largest controllable part** (VFG Medium: upstream-measured +6.6 ms per source frame at 4K, so
   3.3-6.6 ms per generated frame depending on the multiplier of that run). It is GPU work on the RTX 5070; the code
   changes here measure it exactly (`gpuFg1P50/P95Ms`) and try two scheduling levers (queue priority, an early release of
   the capture upload buffer). Making fg1 itself cheaper at 4K without touching the picture is not possible in this
   code: the model runs at the full working size (section 4c).
4. Built (all off by default; unverified on Windows): `[slipstream-sizes]`; the delay split in `[slipstream-smooth]`;
   `gpuFg1P50/P95Ms`, `uploadCpuCopyFps`; `VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE=1`; `VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY=high`;
   `VEYRA_EXP_RP_FIT_DISPLAY=1` (DIAGNOSTIC only); the kit sets `d90` and `d90-capture` (about 22 and 39 minutes since Package 14).

## 1. What was verified in code

| Question | Answer [code] | Where |
|---|---|---|
| Does the 4K setting apply throughout? | Yes. Base = SR result (or the source without SR); NR's composite, the flow adaptation target, every generated frame and the output use that size. NR itself may infer smaller (`nrSize`, e.g. 1080 lines) and composite back to 4K; the optical flow runs at the source size (1080p for a 1080p stream) and is adapted to 4K | `include/veyra/pipeline/ResolutionPlan.h` (`p.fg=p.output=p.base`), `EnhanceGraph::initialize` (`workW_`), the `[resolution]` line |
| Who scales to the screen? | The presenter's blit, from the work size into the swap chain's back buffer (the window's size) | `VideoPresenter.cpp` (`presentationRegion(graph.workWidth()...)`) |
| What the in-app readout measures | Capture callback / decoded picture -> Present call of each real frame (software, not photons) | `PushLatencyBadge.qml`, `FrameFlowWindow` |
| Is anything buffered beyond one real frame? | No: latest-wins mailboxes, 2 batches in the presentation scheduler, DXGI maximum latency 1 while pacing (FG_LATENCY.md 1.2) | |
| Does the next pair's pickup wait for the current group? | Yes: a new frame is not read while the previous batch (base + every generated frame) is on the GPU (`enhancementPending`). At 30 FPS with a group of base + 2 x fg well under T this costs nothing; it would if the group exceeded 33.4 ms | `EngineController.cpp` |

## 2. Evidence of the real working sizes (deliverable 1)

`[slipstream-sizes]` (always on, like `[slipstream-config]`; `engine/PushSizes.h`, `PushSizesHook.h`): written when the
live graph is built and whenever a value changes, looked at no more than once a second; it decides nothing. Keys:
`source srInput srEvaluated base nr flow fg output backBuffer client display mode modeHz sr srTarget srTargetAsked hdr
srDisplay fitDisplay fit fitReason`. On his PC it is expected to read [estimate]:
- 4K capture: `source=3840x2160 ... base=3840x2160 fg=3840x2160 output=3840x2160 backBuffer=2560x1440 display=2560x1440
  fit=0 fitReason=source-size` (a fact about the sizes: working at the capture's own 4K, his choice);
- Remote Play: `source=1920x1080 srInput=1920x1080 srEvaluated=3840x2160 base=3840x2160 ... fit=0 fitReason=as-set` (working at the 4K that was set).
The kit shows these per run in the group "processing sizes".

## 3. The delay part by part at 4K (deliverable 2)

With `VEYRA_EXP_RP_STATS=1` every real frame shown on an FG phase after its pair's first generated frame was seen ready
is split (`PushFgLatencyWindow::parts`; the six parts of one frame add up, to within microseconds, to its Present call minus its
callback / decode):

| Part | Field (P50; P95 where given) | Formula / what it contains | 4K capture (estimate) | Remote Play 1080p -> DLSS SR 4K (estimate) |
|---|---|---|---|---|
| wait before pickup | `pickupWaitP50/P95Ms` | callback/decode -> engine pickup. Capture: the callback's copy of the driver sample into the upload buffer is inside it (4K NV12 12.4 MB, upstream-measured 1.3 ms on the developers' machine; P010 24.9 MB about 2.6 ms, YUY2 16.6 MB about 1.7 ms, by bytes); then the wake-up | 1-3 ms | the decode -> pickup hand-off (`[slipstream-latency] decodedToPickupP50`), < 1 ms, plus the stream duplicate removal's GPU measurement of each decoded picture (a wait of at most 4 ms, never past the next presentation; `EngineController.cpp`, `measureStreamChange`) |
| base | `baseObsP50/P95Ms` (GPU stages: `gpuColorP50Ms`, `gpuSrP50Ms`, `gpuNrP50Ms`, `gpuFlowP50Ms`) | pickup -> real frame seen ready: CPU submit + GPU copy from the upload heap (bytes / PCIe rate: about 0.5 ms for NV12 at an assumed 25 GB/s) + colour conversion at 4K | upstream-measured passthrough callback -> Present return 1.07 ms in total (RTX 5070, T911 4K60, all effects off) | + DLSS SR to 4K: no upstream figure recorded; `gpuSrP50Ms` measures it |
| fg1 | `fg1ObsP50/P95Ms` (CPU view), `gpuFg1P50/P95Ms` (GPU timestamps) | VFG: copy of the real frame into CUDA's buffer, the semaphore hand-off, the inference at 3840x2160, the copy back | VFG Medium: 3.3-6.6 ms (upstream-measured +6.6 ms per source frame at 4K, multiplier of that run not recorded) | same |
| interpolation wait | `interpWaitP50Ms` | (M-1)/M x T of the pair's own timestamps | 22.2 ms (fixed) | 22.2 ms with the stream's duplicate removal (`VEYRA_EXP_RP_STREAM_DEDUP=1`); without it the stream is interpolated at T = 16.7 ms and 3X gives 180, not 90 |
| margin | `marginP50Ms` | phase in force - fg1 ready - interp. Card: 0.5 ms + 0-1 ms + (P95 - P50 of readiness). Remote Play default: the fixed phase T + base P95 leaves T/M + base P95 - base - fg1 | 1-3 ms | 4-8 ms default; about 1-3 ms with `VEYRA_EXP_RP_FG_ULL` part `phase` |
| present lateness | `realLateP50Ms` (all frames: `presentLateP95/P99Ms`) | Present call - deadline | about 0.1 ms | about 0.1 ms |
| **sum** | `partsSumP50Ms` against the measured `delayToCallP50Ms` | | **about 28.5-33 ms** | **about 30-37 ms + SR at 4K** |

The kit shows these in the group "where the delay goes", per arm, with the measured total next to the sum (a sum of
medians is an approximation of the median of the sums). Software times on the PC; never input-to-photon.

## 4. Every lever under his rules (deliverable 4)

### 4a. Capture upload at 4K [code, estimate]

Copies between the card's callback and the first GPU pass, 4K NV12 (P010 doubles every figure):
1. Driver sample -> the frame's own D3D12 upload buffer, in the callback (CPU, 12.4 MB, write-combined memory;
   upstream-measured 1.3 ms). Unavoidable: the driver's buffer is not GPU-visible. Splitting it over threads gained
   nothing upstream (`CaptureUploadFrame.h`).
2. Upload buffer -> source texture, `CopyTextureRegion` at the start of the base list (GPU, over PCIe: bytes / rate).
3. Compute conversion to the linear working texture (`Nv12Upload`, part of `gpuColorP50Ms`).
4. **Only when the GPU still held the upload buffer** as the callback wanted to write it: the callback writes the frame's
   CPU planes instead, and the graph copies those 12.4 MB again into its own upload buffer on the engine thread
   (upstream-measured 0.9 ms) before submitting. The graph released the buffer only at the fence of the pair's LAST
   list (every generated frame), although the only reader is the base list's copy at its start. Whether this happens on
   his PC was not visible in any log.
   - New: `uploadCpuCopyFps` counts it (`EnhanceGraph::Metrics::captureUploadCpuCopies`). Expected 0 because of the
     pickup gate (the next frame is read only after the previous group finished), so the buffer is normally free.
   - New: `VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE=1` releases it at the base list's fence. Expected: no change when the counter
     is 0; up to about 0.9 ms (NV12) / 1.8 ms (P010) less on each frame that had the second copy. Risk: none known (no
     later list reads the buffer: checked in `EnhanceGraph::process`); unverified on Windows.

### 4b. Base GPU work at 4K [code]

On the critical path before fg1: the upload copy and colour conversion (capture), DLSS SR to 4K (Remote Play), NR when
on, the clean-up when on, the optical flow when the backend uses it (DLSS path, FSR, ANR; not VFG), then VFG's copy of
the real frame into its CUDA buffer. The real frame's exclusion compose and the HUD Lock update are recorded in the
same list before the submit; they are needed for the real frame, not for fg1, and could move after fg1's submit, but
both are off unless the owner turned them on, so nothing is gained in his setup [estimate]; not built. The working
format question (RGBA16F at 4K for SDR): the linear working texture is what colour, SR and NR are written for; a
narrower format is not provably identical in picture, so it is not touched (his rule 1).

### 4c. fg1 at 4K [code, upstream-measured]

| Backend | What runs at full size | Recorded cost | Note |
|---|---|---|---|
| VFG Low / Medium / High | the inference at 3840x2160; one copy in (per pair) and one copy out (per generated frame) of the working texture (33 MB at 8 bits per channel, 66 MB at 16; about 0.1-0.3 ms each at an assumed 400 GB/s) | Medium +6.6 ms per source frame at 4K; High +13.2 ms per source frame, "2X at 4K is 16 ms" (live) | High at 3X 4K: 2 x 16 = 32 ms of generation per 33.4 ms pair: no room, and fg1 alone would add about 10 ms over Medium |
| DLSS path | the flow at the SOURCE size (1.36 ms, 2.44 ms with refinement, at 1080p), the generation at 4K | 2X at 4K60 capture: 1% low 115.2-117.5 (no per-frame cost recorded) | the flow is in base, before fg1 |
| PushFlow | the network at most 1280x720 (Balanced), the picture rebuilt at 4K | `pushFlowP50Ms` | lower frame generation quality: DIAGNOSTIC reference only |

Not built, with the reason:
- **"The first generated frame with the cheapest model"** (VFG Low for frame 1, Medium for frame 2): fg1 would shrink by
  the Low-Medium difference, but one generated frame in three would come from another model: a visible alternation of
  detail at 30 Hz is possible. That lowers picture quality: excluded by his rule 1 (a DIAGNOSTIC arm `d90-fg3-low` /
  `d90c-fg3-low` shows Low for every frame, labelled "DIAGNOSTIC reference (lower frame generation quality; not a
  solution)").
- **Generate at the stream's size, then upscale every shown frame to 4K** (Remote Play only): fg1 at 1080p (upstream
  +2.2 ms per source frame) instead of 4K, output still 4K. But DLSS SR is temporal: its history would then hold
  generated frames, every generated frame would also pay SR at 4K (so the group's GPU time grows), and the picture
  differs (the upscaler sees interpolation errors and sharpens them). Only his eyes could decide, and the graph order
  (FG before SR) does not exist; written blind it cannot be verified. Not built.
- **CUDA reading the D3D12 texture directly** (no copy in / out): NVIDIA's effect takes pitch-linear CUDA images; a
  texture would need a CUDA array and a conversion. Saves about 0.2-0.6 ms [estimate]; not safe blind.
- **Order of submits**: already optimal for latency: base list, then generated frame 1 (its own fence), then frame 2; the
  real frame is presented last. Frame 2 never delays frame 1.

### 4d. GPU utilisation at 4K, 30 Hz bursts [code, hypothesis]

The GPU works in bursts (base + 2 generated frames, roughly 10-17 ms) and idles for the rest of each 33.4 ms pair.
- Process GPU scheduling priority: already requested through D3DKMT (`gfx::GpuSchedulingPriority`, `[gpu-priority]`).
- Queue priority: the graph's direct queue was created NORMAL. **New: `VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY=high`**
  (`D3D12DeviceContext.cpp`, `[graph-priority]` says whether the device allowed it). Hypothesis: the base and
  generated-frame lists win time slices against other work; VFG's CUDA inference is NOT on this queue, so fg1's largest
  part is unaffected. Risk: the presentation blits can wait behind graph work: watch `[slipstream-cadence]` and
  `presentLateP99Ms`; the arm `-pprio` raises the presentation queue instead (existing test switch), `-all` both.
- **Hypothesis, the owner's own choice, never changed by code:** between bursts the GPU may lower its clocks and need
  time to raise them at the next burst. NVIDIA's control panel has a per-program "Power management mode: Prefer maximum
  performance" setting the owner can choose for PushFPS.exe. It would show as a smaller gap between `gpuFg1P95Ms` and
  `gpuFg1P50Ms`, and lower `gpuFg1P50Ms` / `gpuColorP50Ms`, against the same arm without it. Not measured; not claimed.

### 4e. The phase and margins at 3X [code]

| Item | Value at 3X from 30 FPS | Adds | FG_ULL part |
|---|---|---|---|
| Learned phase (cards; Remote Play with part `phase`) | P95 of the last second's readiness (about 30 pairs: the second-highest) + 0.5 ms | 0.5 ms + P95 - P50 | - |
| Deadline margin | 0.25 ms at start, 0-1 ms, learns 99 % on time | 0-1 ms | - |
| Fall rate after a spike | min(0.5 ms, T/M/8 = 1.4 ms) = 0.5 ms per pair = 15 ms per second | a 10 ms spike costs extra delay for about 0.7 s | - |
| Remote Play fixed phase (default) | T + base P95 = 33.4 + base P95 | T/M + base P95 - base - fg1 - 0.5 = about 4-7 ms at 4K | `phase` removes it (expected; jitter risk: `dropDeadlineFps`) |
| Grace for a late generated frame | 10 ms | a frame up to 10 ms late is shown late | `grace`: half an output interval, 5.6 ms (drops instead; evenness risk) |
| Skipped pairs | the real frame keeps the whole phase without generated frames | about 31 ms per affected frame | `hold` |
| Capacity wait / `SetMaximumFrameLatency(1)` | 90 FPS on a 180 Hz VRR panel: one present every 11.1 ms, 5.5 ms above the panel's 5.56 ms minimum: no back-pressure [estimate]; at a fixed 180 Hz, 90 = every second refresh, even, a present waits 0-5.6 ms for its vblank (also with FG off) | 0 under VRR | - |
| Headroom part | Smoothness Mode with target 120 on a 180 Hz panel; it never raises 3X but MAY STEP DOWN to 2X (60 FPS) on load | against stable 90 | **do not use at 3X**: the d90 arms use `hold,phase,grace` |

No real avoidable extra specific to 3X was found beyond these; no new FG_ULL part was added.

### 4f. Summary of levers

| Lever | Built as | Expected (estimate) | Risk | Measured by |
|---|---|---|---|---|
| Fixed phase on Remote Play | existing `VEYRA_EXP_RP_FG_ULL=phase` (in `hold,phase,grace`) | 4-7 ms less | late generated frames from decode jitter | `marginP50Ms`, `dropDeadlineFps` |
| Second copy of a capture picture | `VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE=1` | 0 (expected) to 0.9 / 1.8 ms per affected frame | none known | `uploadCpuCopyFps`, `pickupWaitP50Ms`, `baseObsP50Ms` |
| Graph queue priority | `VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY=high` | unknown, likely below 1 ms | presentation blits wait | `gpuFg1P95Ms`, `baseObsP95Ms`, cadence |
| Presentation queue priority | existing `VEYRA_TEST_PRESENT_QUEUE_PRIORITY=high` | unknown | graph work waits | `realLateP50Ms`, `presentLateP99Ms` |
| GPU power mode | the owner's driver setting (hypothesis) | unknown | none for the picture | `gpuFg1P50/P95Ms` |
| DLSS path at 4K | existing choice | smaller fg1 (unmeasured) | the same 4K output made by another method: compare the picture by eye, keep the current method unless it looks at least as good | `fg1ObsP50Ms` |
| VFG Low, PushFlow | existing choices | smaller fg1 | lower frame generation quality: DIAGNOSTIC references only, never a solution | `fg1ObsP50Ms` |

## 5. The kit (deliverable 5)

Two sets, every arm at 4K unless its note says DIAGNOSTIC; switch arms against the 3X arm (`d90-fg3` / `d90c-fg3`); 2 rounds; with the
kit's own arithmetic (2 rounds x arms x 83 s) the Remote Play set takes **about 22 minutes** (8 arms) and the capture set
**about 39 minutes** (14 arms) since Package 14, which removed the arms of switches that became inert (FG_ULL, the graph
queue priority, the early upload release, now the default) and added to the capture set the new defaults against
`VEYRA_TEST_RP_LEGACY_PACKAGE13=1` (`d90c-p13`) and each restore switch alone (`d90c-l-*`):
- Remote Play (smoothness session; DLSS SR to 4K; `dedup=auto` + the experimental stream duplicate removal
  `VEYRA_EXP_RP_STREAM_DEDUP=1` so a 30 FPS game pairs at
  33.4 ms; these are the only kit arms besides the `p-dedup` diagnostic that turn the stream's duplicate detector on, and
  only because 30 -> 90 over Remote Play does not exist without it; it stays off by default):
  `Run-First-Test.cmd -Session smoothness -Pass 180 -Arms d90-off,d90-fg3,d90-fg3-pprio,d90-fg3-low,d90-fg3-dlss,d90-fg3-flow,d90-fg3-fit,d90-fg2`
- 4K capture card (capture session; card format 3840x2160; duplicate removal on):
  `Run-First-Test.cmd -Session capture -Pass 180 -Arms d90c-off,d90c-fg3,d90c-p13,d90c-l-robust,d90c-l-grid,d90c-l-cadence,d90c-l-gpuwaits,d90c-l-upload,d90c-l-nrhold,d90c-fg3-pprio,d90c-fg3-low,d90c-fg3-dlss,d90c-fg3-flow,d90c-fg2`

`-off` is the reference (what the 4K base alone costs). `d90-fg3` is 3X at 4K with the experimental stream duplicate
removal (`VEYRA_EXP_RP_STREAM_DEDUP`; not the installed default); `d90c-fg3` is 3X at 4K as installed. `-ull` =
`VEYRA_EXP_RP_FG_ULL=hold,phase,grace`; `-early`, `-prio`, `-pprio` one switch each; `-all` every switch of the set
together. `-dlss` is the same 4K output made by another method: **compare the picture by eye and keep the current method
unless it looks at least as good**. `-low` and `-flow` (lower frame generation quality), `d90-fg3-fit` (the upscaler
lowered to the screen's size) and `-fg2` (60 FPS) are **DIAGNOSTIC references, not solutions or recommendations**. The
capture set has no screen-size arm: nothing reduces a 4K capture, and none will be built. Reading: "where the delay
goes", "first generated frame on the GPU", "processing sizes", and the existing cadence lines for evenness.

## 6. What only his PC can answer

- The real fg1 at 4K for each backend (`gpuFg1P50/P95Ms`), the base at 4K (`gpuColorP50Ms`, `gpuSrP50Ms`), and whether
  the sum of parts matches his ~30 ms (`partsSumP50Ms` against `delayToCallP50Ms`).
- Whether the second copy of a capture picture ever happens (`uploadCpuCopyFps`).
- Whether a queue priority or his driver's power mode moves `gpuFg1P95Ms`, and at what cost to evenness.
- Whether the learned phase on Remote Play keeps 90 even (`dropDeadlineFps`, `[slipstream-cadence]`).
- Input-to-photon: only the camera procedure (CAPTURE_FG.md section 9).

## 7. The floor of the CURRENT approach at 4K

The 22.2 ms wait is a property of the present method (every generated frame lies between two real frames, so the
newer real frame is shown last), not of the hardware. The owner's instruction (23 Nov 2026): after Package 13 is tested,
investigate other frame generation scheduling approaches that could shorten that wait at 4K and an even 90 FPS (for
example showing the real frame earlier and filling the slots after it with frames predicted from past motion). For 3X interpolation as built today the floor is **22.2 ms + base(4K) + fg1(4K)**, plus the capture copy on a card. With
upstream-measured figures that is about 26.5-30 ms on a capture card [estimate]; his ~30 ms reading is within a few
milliseconds of it. What separates them is the margins that keep 90 even (about 1-3 ms) and, on Remote Play, the fixed
phase (about 4-7 ms, removable by an existing experiment). Below the floor only fewer interpolated frames per pair
(2X: 16.7 ms; a 40 FPS game mode at 3X = 120 FPS: 16.7 ms) or another scheduling approach would go. Fewer frames is outside his rules; other
approaches are the next investigation, once this package's measurements are in. The RTX 5070
limits: VFG High at 3X does not fit a 4K pair (about 32 ms of generation per 33.4 ms); VFG Medium fits with room; the
model's own time at 4K is the largest part we can measure but not shorten without another picture.

## 8. Not built, and why

- A shrink before frame generation for a source larger than the display: against the 4K rule; never to be built.
- Extrapolated / predicted frames: the owner's standing rule.
- A cheaper model for the first generated frame only, FG before SR, direct CUDA texture interop: section 4c.
- Moving exclusion / HUD Lock work after fg1's submit: off in his setup, nothing to gain (4b).
- No new user-visible string, no interface row: these are experiments until his run.
