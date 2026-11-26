# Capture-card frame generation: 120 stable, 180 stretch (stream "capture", 21 Nov 2026)

Scope: the owner's fifteenth message (an HDMI capture card, the controller plugged into the PS5, frame generation
to 120 FPS, 180 FPS as a stretch, 30 fps games such as GTA 6 first). Base: exp/rel-9 (Package 9). The owner has not
named the card yet, so everything here is device-neutral and says what depends on the model.

Labels used on every number and claim:
- **code**: read in this repository (file named);
- **upstream-measured**: recorded by the PushFPS developer in this repository's own documents on THEIR PC (RTX 5070,
  Ryzen 7 9700X, 2560x1440 320 Hz panel, a T911 card at 3840x2160 60 Hz NV12, one 1080p NR layer, software
  counters). Same GPU model as the owner's, different card, CPU, panel and settings. Not the owner's PC;
- **published**: a vendor document (linked);
- **estimate**: arithmetic from the above, never a measurement;
- **hypothesis**: needs the owner's run.

Nothing here was run on the owner's RTX 5070, his capture card or his PS5. Nothing was compiled on Windows by this
stream before its own commits went through the remote check.

## 0. Short answer

| Source (game) | Target | Multiplier | Even on screen? | GPU work per source frame (1440p) | Added delay of a real frame vs passthrough, lower bound: (M-1)/M x T, which already contains the wait for the next real frame [code formula, estimate] | Verdict |
|---|---|---|---|---|---|---|
| 30 fps | 60 | 2X | yes (every refresh rate that is a multiple of 60, or VRR) | 1 generated frame | 16.7 ms + one generated frame's GPU time | achievable (upstream-measured at 4K) |
| 30 fps | 90 | 3X | yes on 180 Hz, or VRR | 2 | 22.2 ms + ... | achievable (upstream-measured: VFG Medium 3X 1% low 88.1-88.3 at 4K) |
| 30 fps | **120** | 4X | only with VRR or Windows at 120 Hz (1.5 refreshes at a fixed 180) | 3 | **25.0 ms** + ... | **achievable on the GPU with VFG Low/Medium, mixed VFG High+Low or the DLSS path; NOT with VFG High** (upstream-measured at 4K: DLSS 4X 1% low 116.3; VFG High+Low 4X 111-114; VFG High 3X rejected 98 % of pairs). Confirm on his PC |
| 30 fps | 180 | 6X | only at an exact rate match or VRR at its ceiling | 5 | 27.8 ms + ... | **stretch**: VFG Low/Medium or DLSS 6X only (PushFlow stops at 4X; VFG High impossible); no measurement exists at 6X from a capture card |
| 60 fps | **120** | 2X | VRR or 120 Hz | 1 | **8.3 ms** + ... | **achievable** (upstream-measured at 4K60 capture: DLSS 2X 1% low 115.2-117.5, VFG 2X 114.5-116.7: docs/LIVE_LATENCY_REVIEW_2026-11-13.md) |
| 60 fps | 180 | 3X | 180 Hz at an exact match, or VRR | 2 | 11.1 ms + ... | likely with VFG Low/Medium or DLSS 3X (estimate); not with VFG High |
| 40 fps (120 Hz modes) | 120 / 160 | 3X / 4X | 120: yes; 160 only with VRR; 180 never even | 2 / 3 | 16.7 / 18.8 ms + ... | as 30 fps |

"Even" assumes the card delivers every HDMI frame and duplicate removal finds the game's real pictures (section 2).
The delay column is the arithmetic minimum of interpolation before any GPU time (section 4: add one generated frame and the frame's own processing, about 4-6 ms with VFG Medium at 1440p [estimate]), not a measurement. Generated frames are never
native PS5 frames: a 30 fps game at 4X shows 30 game frames a second and 90 guessed ones.

## 1. What exists (code), and what the brief assumed wrongly

- **The capture path is DirectShow, not Media Foundation** (`src/source/CaptureCardSource.cpp`): a native
  `ConnectDirect` to PushFPS's own sink (`NativeCaptureSink.cpp`, OBS's pin rule), no converters, the driver's own
  format (NV12, P010, P016, YUY2/UYVY, v210, RGB, I420/YV12 made NV12, MJPEG/H.264/HEVC decoded by
  `CaptureCompressedDecoder`). Vendor paths beside it: Magewell Pro Capture SDK low-latency video
  (`MagewellCapture.cpp`, stops the DirectShow graph to free the PCIe link), Blackmagic DeckLink native
  (`DeckLinkCapture.cpp`), Elgato HDR metadata (`ElgatoHdrControl.cpp`), AVerMedia vendor audio. Media Foundation is
  only used for camera/video layers of streaming mode.
- **Latest-frame mailbox, one CPU copy** (code): three frames (reader, pending, staging). The callback copies the
  driver sample straight into a D3D12 upload buffer (`CaptureUploadFrame.h`, "single CPU copy when 3" in the
  `[capture] mailbox frames` line), outside the lock, and publishes by pointer swap; a frame not yet read is
  overwritten (counted `dropped`). No queue. The engine waits on the callback's event, not a timer.
- **Driver buffering**: `CaptureBuffer.h` suggests 2 allocator buffers up to 1080p, 3 above (Auto), 1 (Minimum, a user
  setting, pref `captureBufferMode`) or nothing (driver default). The suggestion is advisory
  ([IAMBufferNegotiation](https://learn.microsoft.com/en-us/windows/win32/api/strmif/nf-strmif-iambuffernegotiation-suggestallocatorproperties):
  "If the number of buffers or size of each buffer is too small, the filter graph might drop samples"); the
  `[capture-buffer]` line says whether the driver honoured it. Buffers only add delay when the consumer is late,
  because the callback drains each sample at once; what the DRIVER holds is visible only in `[capture-driver-age]`
  (driver timestamp to our callback, P50/P95 every 600 frames).
- **Duplicate removal for 30 fps games works for capture cards** (unlike hardware-decoded PS5 streams): the
  callback compares every component of each sample with the last new picture (`PictureCompare.h`, about 0.3 ms at
  1440p NV12 by the code's own note) and numbers it; `ContentDedup.h` drops repeats before any GPU work, gives kept
  frames content timestamps on a fitted cadence, and holds a still picture at the content rate. It is on in the
  content modes Auto / 30 / 50 / 60 (pref `contentRate`, the quick setup turns Auto on with a target), off in
  Transport (the engine default). Upstream-measured on a real 30 fps game: "自动识别逐帧全对" (every frame classified
  correctly), new pictures handed over in 0.57 ms (docs/LIVE_LATENCY_REVIEW_2026-11-13.md).
- **Pair timing for cards** (code): a learned phase (`LivePairLatency`: P95 readiness + 0.5 ms), an automatic
  0-1 ms deadline margin (`LiveDeadlineMargin`), a smoothed presentation anchor that removes callback jitter
  (`LiveArrivalSmoother`, upstream-measured callback jitter 1.9-2.6 ms on USB), admission per pair
  (`FgRecoveryBudget`), settling after skips (`FgCostSettling`), and **the graceful fallback the owner asked for**:
  after one second of rejected pairs `LiveFgRejectionHold` shows real frames as soon as they are ready, without the
  generation phase (upstream-measured: real-frame delay 38-46 ms -> about 6.5 ms when VFG High 3X could not keep up),
  and resumes only after 8 admitted pairs in a row. A generated frame that misses its deadline is never shown
  (`expiredGenerated`); nothing repeats a frame and calls it generated at present time.
- **Scene cuts** (code): the graph's luma analysis marks cuts and cadence breaks for CPU-uploaded (capture) frames and
  resets the generation history (`EnhanceGraph.cpp`, `[scene] history boundary`); VFG additionally runs its GPU
  shot-change test and shows the real picture on a cut; PushFlow resets the pair.
- **HUD protection** (code): HUD Lock (`PushHudLock`) and the HUD boxes / automatic HUD detection apply to every graph
  frame-generation backend, capture included (only XeSS, which generates inside its own presenter, is excluded:
  `EnhanceGraph::initHudLock`). Whether they hold on GTA 6's HUD is a picture question for the owner (kit arms
  `cap-fg4-hud`).
- **Audio sync** (code): every presented REAL capture frame reports its callback arrival and present time to the
  audio session (`captureSource.videoPresented`, `CaptureAudioSession::videoPresented`), so automatic audio sync
  follows the measured video delay, including the extra delay of interpolation. Generated frames are not reported.
- **Cap rounding** (code, merged from stream fps, exp/s6-fps 8b8b7d86, into this branch): the output-cap reduction now
  has a 2 % tolerance (`pushSmoothCapReduction`, `[push-cap-fit]`). Before it, a content rate of 29.97 under a 119.998 Hz
  cap rounded up (ceil(4.004) = 5) and generated 5X when 5X or more was allowed, and the cap threw a fifth away; now
  29.97 x 4 = 119.9 counts as reaching the cap and 4X is used. `VEYRA_TEST_SMOOTH_CAP_CEIL=1` restores ceil().

## 2. Duplicate removal: what the cadence classifier must do

The card delivers the HDMI signal rate, not the game's rate. The PS5 sends 120 Hz only "for your TV and games that
support 120 Hz frame rate" ([PlayStation: 120 Hz Output](https://www.playstation.com/support/hardware/ps5-4k-resolution-guide));
a 30 fps game is normally inside a 60 Hz signal. So the engine sees:

| Game | Signal | Samples per picture | What dedup must keep |
|---|---|---|---|
| 30 fps | 60 Hz | 2 (A A B B) | the first copy of each picture, timestamps 33.3 ms apart |
| 30 fps | 120 Hz | 4 | the same; 3 of 4 samples removed |
| 40 fps | 120 Hz | 3 | 25 ms content interval |
| 60 fps | 60 Hz | 1 | everything (no repeats; a still picture is held at the content rate) |
| 60 fps unlocked in a 120 Hz mode | 120 Hz | 1-2, varying | a varying content rate (the adaptive multiplier follows it) |

Requirements and how the code meets them (code, `ContentDedup.h`):
1. **Never call a new picture a repeat**: exact comparison inside a noise band of the whole picture, not a sample of
   points. A repeat is at most about 5 per million components beyond the band.
2. **Find the new picture on its first sample**: the callback numbers every sample, also ones the engine never
   reads, so a picture whose first copy was overwritten in the mailbox still counts once.
3. **Content timestamps**: a least-squares cadence over the last kept frames, clamped into the window in which the
   picture changed, so a regular 30 fps game gets 33.3 ms steps even when the engine read a later copy.
4. **Hitches and still pictures**: a picture repeated for up to 2.5 content intervals is a hitch (the next picture
   spans the wait); longer, it is held and re-sent at the content cadence so the GPU work stays steady.

What was missing for the owner's question "repeated frames must not count as generated" (added by this stream, section 11):
- a counter of **new pictures the engine never saw** (the mailbox overwrote every sample of it: a real game frame lost,
  visible as judder), now `[slipstream-capture] missedNewFps`;
- a counter of **generated frames made from two identical pictures** (a held still picture with dedup on, or every
  second pair of a 30 fps game with dedup OFF), now `[slipstream-capture] stillGenFps`, beside `motionGenFps`.

Robustness found in a model of the code (tests/remoteplay/CaptureStatsTests.inc; Linux, not the card):
- 30 / 40 / 60 fps games in a 120 Hz signal: every picture kept once on even 33.3 / 25 / 16.7 ms content steps; a
  still picture held at about 30 a second, never 120; engine read gaps of 1-3 samples lose nothing.
- Reads 41.7 ms apart (the engine late on every frame) lose whole pictures (119 of 600), which the engine cannot see;
  the rate the multiplier uses then reads about 2 % low (29.3). `missedNewFps` makes this visible.
- **Six signal periods break the cadence**: 100 ms at 60 Hz, only 50 ms at 120 Hz. An 80 ms engine stall in a 120 Hz
  signal left the content rate unknown for about 225 ms (the output-cap reduction is off meanwhile, so a 6X ceiling on a
  120 Hz cap runs 6X for that time). `VEYRA_EXP_RP_CAPTURE_DEDUP_SPAN=1` sets a 100 ms floor (off by default).

## 3. GPU cost per backend at 2560x1440 (the owner's working size)

No measurement at 1440p exists on an RTX 5070 for any backend; the figures are what the repository and NVIDIA record.

| Backend | Recorded figure | Source | What it means at 1440p (estimate) |
|---|---|---|---|
| VFG Medium (default, `vfgQuality=1`) | +2.2 ms per SOURCE frame at 1080p, +6.6 ms at 4K, RTX 5070, export run (frames processed in series; the multiplier is not stated) | upstream-measured, docs/RTX_VIDEO_SR_FAMILIES_2026-11-05.md ("每个源帧多出的时间") | about 3-4 ms per generated frame IF the run was 2X (one generated frame per source frame), which the record does not say |
| VFG Medium 2X | 1.32 ms on an RTX 4090, 1.90 ms on an RTX 5090, 1280x720 | published, [NVIDIA VFX SDK performance reference](https://docs.nvidia.com/maxine/vfx/1.3.0/WindowsVFXSDK/PerformanceReference.html) | no RTX 5070 or 1440p figure is published |
| VFG High | +15.1 ms per source frame at 1080p, +13.2 ms at 4K (same export run, multiplier not stated); "VFG High 2X at 4K is 16 ms on an RTX 5070" (live) | upstream-measured; code comment `FgRecoveryBudget::phaseAllowance` | about 13-16 ms each: High does not get cheaper at lower resolutions in these figures |
| VFG Low | no figure recorded | | cheaper than Medium (NVIDIA: "lowest-complexity model") |
| VFG mixed High+Low (quality 4) | 60->30 4X at 4K: 1% low 111-114 | upstream-measured, LIVE_LATENCY_REVIEW | one High plus N-2 Low frames per pair |
| DLSS path (NGX DLSS-G with optical-flow motion) | 2X at 4K60 capture: 1% low 115.2-117.5; 4X (60->30): 116.1-116.3; optical-flow stage 1.36 ms (2.44 ms with refinement) at 1080p | upstream-measured | 2X, 3X, 4X, 6X offered; per-frame cost not recorded separately |
| PushFlow Fast / Balanced / Quality (RIFE on DirectML) | none on a real GPU (CI: Windows software renderer only) | docs/PUSHFLOW.md | unknown; the older RIFE worker cost +9.0 ms per source frame at 1080p on an RTX 5070 (upstream-measured, same export run) and was removed for cost; PushFlow offers 2X-4X only |

Every VFG subframe is a full inference (code, `EnhancementSettings.h`), so a pair costs (M-1) x the per-frame time. The
table below reads the export figures as per generated frame (a 2X run), the assumption named above; if that run made
more than one frame per source frame, every figure here is too high:

| Arm (1440p, estimate) | Generated frames per second | VFG Medium (3.5 ms each) | VFG High (14 ms each) | Fits the source interval? |
|---|---|---|---|---|
| 30 -> 120 (4X) | 90 | 10.5 ms per 33.3 ms | 42 ms per 33.3 ms | Medium yes, High **no** |
| 30 -> 180 (6X) | 150 | 17.5 ms per 33.3 ms | 70 ms | Medium yes (half the GPU), High no |
| 60 -> 120 (2X) | 60 | 3.5 ms per 16.7 ms | 14 ms per 16.7 ms | Medium yes, High marginal (upstream: 16 ms at 4K) |
| 60 -> 180 (3X) | 120 | 7 ms per 16.7 ms | 28 ms | Medium yes, High no |

Add the per-frame base work (colour, upload, optional SR/NR: `gpuColorP95Ms`, `gpuSrP95Ms`, `gpuNrP95Ms` in
`[capture-timing]`). A 1080p capture upscaled to 1440p works at 1440p; a 4K capture or SR to 4K works at 4K (2.25x the
pixels): **process at the panel's 1440p** (stream fps's `VEYRA_EXP_RP_SMOOTH_SR_DISPLAY`, or capture at 1440p).

## 4. The inherent latency of interpolation

Interpolation needs the next real frame: nothing between A and B can be made before B has arrived. With content
interval T and multiplier M the screen shows A, then generated frames k = 1 .. M-1 at A + kT/M, then B. The first
generated frame needs B, so (code: the live pair timeline `liveTimeline.resetPair`, deadlines `anchor + delay -
(M-k)/M x T`, and the phase `LivePairLatency`, which learns `ready_k - anchor + (M-k)/M x T` per generated frame and
keeps the P95 plus 0.5 ms):

  shown(g_1) = shown(A) + T/M  >=  arrival(B) + ready_1                     (the wait for the NEXT frame, B)
  shown(A)  >=  arrival(B) - T/M + ready_1  =  arrival(A) + T - T/M + ready_1

so every real frame is shown at least

  delay = (M-1)/M x T + ready_1 + margin

after its own arrival [code: the formula is what the engine schedules; estimate: the numbers below]. The term
T - T/M IS the wait for the next real frame, less the one output slot T/M by which A is shown before the next frame is
due; it is not added on top. `ready_1` is the time from B's callback until the first generated frame between A and B
is finished on the GPU: B's own processing (upload, colour, optional SR/NR) plus ONE generated frame (VFG makes the
subframes in order 1, 2, ... after B is processed: `EnhanceGraph.cpp`, the `vfgBackend_->generate(parity,sub,...)`
loop; later subframes have later deadlines, so only the first one is on the critical path while each fits its slot).
The margin is the phase's 0.5 ms plus the automatic 0-1 ms deadline margin of capture pairs.

| [estimate] | T | 2X | 3X | 4X | 6X |
|---|---|---|---|---|---|
| 30 fps game: (M-1)/M x T | 33.3 ms | 16.7 | 22.2 | 25.0 | 27.8 |
| 60 fps game: (M-1)/M x T | 16.7 ms | 8.3 | 11.1 | 12.5 | 13.9 |
| plus ready_1 (one generated frame + the frame's processing; 1440p, section 3) | | VFG Medium about +4-6 ms; VFG High about +15-18 ms | | | |

Without generation a captured frame is presented about 1.1 ms after its callback (upstream-measured median, callback
to Present return, RTX 5070, T911 4K60: 1.067 ms windowed / 1.081 ms fullscreen, docs/LIVE_DEADLINE_LATENCY_2026-11-12.md). So the added delay of 30 -> 120 at 4X with VFG Medium is about 25 + 5 = 30 ms
over passthrough [estimate], and of 60 -> 120 at 2X about 8.3 + 5 = 13 ms [estimate]. Consequences:
- **Any interpolation of a 30 fps game costs at least 17 ms more than passthrough; 4X costs 8.3 ms more than 2X and
  6X another 2.8 ms.** The step from "no generation" to "some generation" is the expensive one.
- If a group cannot keep up (ready_1 + (M-1)/M x T longer than the learned phase allows), its generated frames expire
  unshown and, after 1 s, `LiveFgRejectionHold` shows real frames as soon as they are ready [code].
- Where buffering could still be cut (each measurable, none removes the formula): driver buffers (`[capture-driver-age]`,
  arm `cap-fg4-buf1` with one buffer); a late mailbox read (`[capture-timing] readAgeMs`); the dedup content anchor's
  half-slot centring (`LiveArrivalSmoother::contentAnchor` adds T_signal/2, which the phase learns back on the content
  clock; compare `realLatP50Ms` of `cap30-fg4` with `cap30-fg4-nodedup`); the 0-1 ms deadline margin; the present path
  (VRR, low queue, independent flip: PresentMon).
- Removing the (M-1)/M x T term needs extrapolation (guessing the next frame), which is not built and not part of this
  work (`pushfps-frame-generation` skill).

**How it is measured.** Internally: `[slipstream-capture] realLatP50Ms / realLatP95Ms / realLatP99Ms` (each real frame's
capture callback to its Present return, this stream) in the passthrough arm and in each generation arm; the difference
is the added PC-side delay and should be close to the table [hypothesis until run]. **Bias of the reference arm**
[code, estimate]: with `VEYRA_EXP_RP_STATS=1` the card's callback compares every sample with the last new picture
BEFORE it hands the frame over (the picture number travels with the frame, so the compare cannot move after publishing
without a second hand-over; `CaptureCardSource.cpp` `writeCaptureFrame`), in every arm, also `cap-native` and
`cap-fg4-nodedup`, where duplicate removal alone would not compare. That adds about 0.3 ms per sample at 1440p NV12 (the
code's note) and about 0.7 ms at 4K [estimate, by pixel count] to every frame's callback-to-Present time. The arms with
duplicate removal on pay it anyway (that is what the owner runs); the passthrough reference pays it only because of the
counters. So `cap-native`'s `realLat` reads slightly HIGH and the added delay of generation (arm minus `cap-native`)
slightly LOW, by up to that amount. The camera (section 9) has the same bias only if the counters are on during its
arms: run the camera arms without `VEYRA_EXP_RP_STATS`. End to end: the camera procedure of
section 9, arms B (passthrough) and C / D (generation): C - B is the added delay including the wait for the next frame
and the display. A 240 fps camera resolves 4.2 ms per frame and a 30 fps game answers on a 33 ms grid, so the 17-28 ms
difference between passthrough and generation shows with 30 presses per arm, while the 8 ms between 2X and 4X needs
about 120 presses per arm [estimate, from the skill's quantisation simulation for a 4 ms effect at 16.7 ms quantisation].

## 5. Why NVIDIA DLSS Frame Generation / MFG does not apply to captured video as NVIDIA specifies it

- NVIDIA's integration guide for DLSS-G requires game data: "DLSS-G requires depth and motion vectors buffers", a
  "Dense motion vector field (i.e. includes camera motion, and motion of dynamic objects)", HUD-less colour that
  "Should contain the full viewable scene, without any HUD/UI elements in it", UI colour with alpha, "Various per frame
  camera related constants", and "It is required for sl.reflex to be integrated in the host application"
  ([Streamline ProgrammingGuideDLSS_G](https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md)).
  NVIDIA describes Frame Generation as using "game data such as motion vectors and depth, and an optical flow field",
  and Blackwell's "hardware Flip Metering, which shifts the frame pacing logic to the display engine"
  ([NVIDIA DLSS 4 announcement](https://www.nvidia.com/en-us/geforce/news/dlss4-multi-frame-generation-ai-innovations)).
- An HDMI signal carries finished pixels only: no depth, no engine motion vectors, no camera, no HUD-less picture,
  and the PS5 game cannot be given Reflex markers. So DLSS FG / MFG in the documented, game-integrated form cannot be
  applied, and nothing here may be described as it.
- What PushFPS's "DLSS" backend actually does (code, `src/ngx/DlssFgBackend.cpp`): it calls the DLSS-G feature
  through NGX with motion estimated from the two captured pictures (NVIDIA optical flow when the build has the NVOF
  headers, otherwise AMD FidelityFX optical flow), a synthetic depth and "A stationary synthetic camera", and paces the
  frames itself. That is outside NVIDIA's documented input contract: quality on game footage cannot be taken from
  NVIDIA's game results, the HUD cannot be separated, and flip metering is not involved (PushFPS's presenter paces).
  It is one backend among the video-only ones; VFG is NVIDIA's documented video interpolator ("AI-powered temporal
  interpolation filter that synthesizes intermediate frames" from a previous and a current frame:
  [NVIDIA VFX SDK, Video Frame Generation](https://docs.nvidia.com/maxine/vfx/1.3.0/Filters/VideoFrameGeneration.html)).

## 6. Capture card, HDMI, HDCP, HDR, audio, input

- **HDCP**: the PS5 must have Settings > System > HDMI > Enable HDCP off, or the card sees no picture
  ([Elgato help](https://help.elgato.com/hc/en-us/articles/360040482032)).
- **Signal**: resolution, 60 / 120 Hz, HDR and VRR are chosen on the PS5 (Settings > Screen and Video > Video
  Output) and limited by what the card's EDID offers. Never assume a card passes VRR, HDR or 120 Hz: the app reads
  what was negotiated (`[capture] configured WxH nominalFps=`, `[capture-device] ... nativeSubtype=`) and the kit
  reports it (`[slipstream-capture] event=card`).
- **Bandwidth** (estimate, uncompressed): NV12 is 1.5 bytes per pixel, YUY2 2, P010 3.

  | Mode | NV12 | YUY2 | P010 (HDR) |
  |---|---|---|---|
  | 1080p60 | 1.5 Gbit/s | 2.0 | 3.0 |
  | 1080p120 | 3.0 | 4.0 | 6.0 |
  | 1440p60 | 2.7 | 3.5 | 5.3 |
  | 1440p120 | 5.3 | 7.1 | 10.6 |
  | 2160p60 | 6.0 | 8.0 | 11.9 |

  A USB 5 Gbit/s card cannot carry 1440p120 or 2160p60 uncompressed: it compresses (MJPEG) or refuses. MJPEG adds a
  decode stage and a 3-deep queue (code; upstream-measured at 1440p144: "every frame waited ~18 ms" before parallel
  decoding). **Prefer an uncompressed mode** (`compressed=0` in `[slipstream-capture]`).
- **HDR**: needs a 10-bit format (P010 / v210) plus the signal's metadata; Elgato cards report it through
  `ElgatoHdrControl`. First runs: PS5 HDR off (fewer variables).
- **Audio**: the card's embedded audio pin or a separate WASAPI input; delay follows the real frames (section 1).
- **Input**: the controller is plugged into (or paired with) the PS5 directly; PushFPS never sees it and no Remote
  Play connection is made in capture mode (the kit's capture session never connects to the PS5). Input-to-photon =
  controller + PS5 game + HDMI + card + PushFPS + display. The PC can measure only its own part (callback to Present
  return); the rest needs the camera procedure (section 9).

## 7. Why Remote Play fluctuated 60 <-> 100, and which causes apply to capture

From docs/drafts/FPS90.md (stream fps, exp/s6-fps):

| Remote Play cause | Applies to capture? |
|---|---|
| P1 admission skips part of the pairs (90-100) | yes, same `FgRecoveryBudget`; field `rejectedPairs` |
| P2 FgCostSettling hold: no generation for 2-8 s (the "steady 60") | yes; for cards `LiveFgRejectionHold` then shows real frames early (lower latency, same 60/30) |
| P3/P4 cap rounding with a measured rate | yes (29.97 x 4 = 119.9 under 119.998: ceil gives 5 when allowed); fixed by stream fps's cap-fit |
| P5 dedup flips | far less likely: capture compares whole pictures exactly; field `irregular` in `[content-dedup]` |
| P6 Smoothness Mode | yes, unchanged |
| P7 4K working size | yes if the card captures 4K or SR targets 4K |
| P8 SR / NR cost spikes | yes |
| P9 VFG fast model | yes (only 2X with auto-regulate) |
| P10 modes / governor | yes |
| P11 refresh / VRR | yes: 120 on a fixed 180 Hz is uneven |
| P12 PS5 frames arrive unevenly over the network | **no**: HDMI is a fixed-cadence signal and card frames get a smoothed anchor (streams do not) |
| new: a real picture lost in the mailbox while the engine is late | capture only; `missedNew` |
| new: MJPEG decode queue at high modes | capture only; `compressed=1` |
| new: the game's own uneven 30 fps | both; `irregular`, optional Frame Pacing Repair |

## 8. Is 120 / 180 achievable, and what would confirm it

**Code and recorded evidence say:** 60 -> 120 yes; 30 -> 120 yes with VFG Low/Medium, mixed High+Low or the DLSS path,
not with VFG High; 30 -> 180 and 60 -> 180 are a stretch that only the cheaper models can afford and that needs exact
rate matching or VRR at its ceiling. All targets need VRR (or Windows at that refresh) to be even. None of this is
measured on the owner's PC.

The owner's run confirms a target per arm when, in steady windows (`[slipstream-capture]`, `[slipstream-smooth]`,
`[slipstream-cadence]`, `[capture-timing]`, `[content-dedup]`):
- `uniqueFps` is the game's rate (about 30 or 60) and `missedNew` about 0;
- `stillGenFps` about 0 with dedup on (a moving scene), and `motionGenFps` = uniqueFps x (M-1);
- `rejectedPairsFps` 0, `holds` 0, `dropDeadlineFps` about 0, `fgHeldPct` 0;
- `[slipstream-cadence] realFps + generatedFps` at the target, `offPct` <= 2 % with VRR active (and PresentMon's
  `MsBetweenDisplayChange` P99 close to the median when PresentMon is installed);
- `realLatP50Ms` / `realLatP95Ms` against the passthrough arm: the measured added delay (internal, Present return,
  not photons), to be compared with section 4. It reads up to about 0.3 ms (1440p) / 0.7 ms (4K) LOW, because the
  counters make the passthrough arm compare pictures too (section 4, bias of the reference arm).

Reading the arms [code]:
- The DLSS-path and PushFlow arms exist only at 4X (and the DLSS path at 6X). With a 60 fps game the output cap lowers
  them (4X of 60 = 240 is reduced to what the cap allows: 2X under a 120 cap, 3X under a 180 cap), so with a 60 fps
  game they measure 2X/3X, not 4X/6X: read `[slipstream-cadence]`'s shown rate and `[push-cap-fit]`, not the arm name.
- `VEYRA_TEST_RP_CAPTURE` starts the card through `resumeCaptureSession()`, which is asynchronous: `[slipstream-autorun]
  ... name=capture state=done` is written when the start was REQUESTED, before the card delivers a frame. The first
  `[slipstream-capture]` window starts at the first timing line after the graph runs; the kit's 8 s settle and the
  worst-5 s values' `skipFirst=1` keep start-up out of the steady values.
- Every `[slipstream-capture]` window belongs to one settings revision (`revision=`): a change (backend, multiplier,
  tier, duplicate removal) drops the part of the window already counted and starts a new one.

## 9. Camera procedure for input-to-photon (the owner's manual step)

The PC cannot see the controller or the panel; only a camera measures the whole chain.
1. Phone camera at 240 fps (4.2 ms per frame), on a stand, seeing the controller's button and the screen at once.
2. Same game, same spot, same PS5 video output (for example 1440p60 SDR, PS5 VRR off) in every arm, and PushFPS started
   normally, without the kit's `VEYRA_EXP_*` / `VEYRA_TEST_*` switches (the counters' picture compare would bias arm B).
3. Arms, interleaved A B C D, 30 presses each (at least): **A** PS5 straight into the monitor (or the card's
   passthrough output into the monitor, if it has one: then say so); **B** through the card into PushFPS, frame
   generation off; **C** 30 -> 120 (4X); **D** 30 -> 180 (6X) or 60 -> 120 / 180 for a 60 fps game.
4. Press: a quick tap of a button with an instant visible effect (the PS button's control centre is the same in every
   game; an in-game action such as jump shows the game's own delay too). Count camera frames from the button bottoming
   out to the first changed pixel of the response.
5. Fill `camera-template.csv` of the kit (arm, press, frames). Report median, P95 and the mean difference of B, C, D
   against A. B - A is the card plus PushFPS; C - B is the frame generation.

## 10. Tests only his card, PS5 and RTX 5070 can answer

1. What the card negotiates with the PS5: modes, formats (uncompressed or MJPEG), 120 Hz, HDR, VRR, audio (the kit's
   card line).
2. The card's own delay: `[capture-driver-age]` P50/P95, and the camera's B - A.
3. Whether the PS5 signal is 59.94 or 60.00 Hz (it decides whether 6X reaches exactly 180).
4. GPU cost per backend and tier at his working size: `gpuFgBatchP95Ms`, `rejectedPairsFps` per arm.
5. Whether 30 -> 120 holds without rejected pairs, holds or late frames for minutes in GTA-class scenes.
6. Whether 180 holds at all (6X / 3X) and whether the panel's VRR ceiling causes drops.
7. Duplicate removal on his game: `uniqueFps`, `irregular`, `missedNew`, still-pair generation.
8. HUD Lock on the game's HUD; scene cuts; ghosting: his eyes, arm by arm.
9. Audio-to-video sync with generation on (`[audio-sync]` lines and his ears).
10. Input-to-photon: the camera procedure; PresentMon for present-to-display.
11. Long-run stability (an hour of play at 4X).

## 11. What this stream changes (exp/s6-capture; unverified on Windows; nothing measured)

| Change | Default? | Where |
|---|---|---|
| This assessment | docs | docs/drafts/CAPTURE_FG.md |
| Merge of stream fps (cap-fit default, Smoothness fixes) so the capture arms measure what the owner will run | as fps | exp/s6-fps 8b8b7d86 |
| `[slipstream-capture]` window: new / repeated / never-read pictures, generated frames between identical pictures kept apart from motion, real-frame callback-to-Present P50/P95/P99, rejection-hold share, mode, card line | only with `VEYRA_EXP_RP_STATS=1` and a capture card (then the callback also compares pictures with duplicate removal off) | `include/veyra/engine/PushCaptureStats.h`, `EngineController.cpp`, `PushAutorun.cpp` |
| `VEYRA_TEST_RP_CAPTURE=1`: the kit starts the remembered card, never a PS5 connection; never from the settings file | test only | `PushAutorun.cpp`, `SlipstreamSettings.h` |
| Pipeline test keys `fgTier`, `dedup`, `hudLock` | test only | `PushPipelineHook.h/.cpp` |
| `VEYRA_EXP_RP_CAPTURE_DEDUP_SPAN=1`: 100 ms continuity floor for a card's duplicate removal | off | `ContentDedup.h`, `EngineController.cpp` |
| Kit session `capture` (13 arms, 58 values, card set-up once, `-Pass 120/180`) | kit | `scripts/remoteplay/testkit/*`, `report.py`, tests |
| Review fixes: the card-name cleaner cuts after any apostrophe-like mark (', `, U+00B4, U+2018, U+2019, U+02BC, U+FF07) and the rest of that word; the window restarts on a settings revision and logs `revision=`; worst-5 s values skip each run's first window; the reference-arm bias written in the kit's first line and here | as above | `PushCaptureStats.h`, `EngineController.cpp`, `measures.csv`, `run-tests.ps1`, `report.py` |

Not changed, with the reason: buffering (Auto = 2 or 3 driver buffers) stays, the one-buffer arm measures it first;
the content anchor's half-slot centring stays (the learned phase takes it back by design; the dedup-off arm shows the
difference); no new generation method, no new model; Remote Play untouched.
