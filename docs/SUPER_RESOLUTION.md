# Super resolution in the PS5 Remote Play path: what runs, what it costs, what to test

Written 20 November 2026 (sprint 4, stream "sr", internal codename Slipstream) **by reading the code** of branch
`exp/s4-sr` (base `exp/rc4`). Every claim below cites the file and line it comes from. **No cost in this document has
been measured on the owner's RTX 5070, or on any GPU.** Costs are stated "in principle" (pixel counts and pass counts);
the `sr` autorun step (below) is what will measure them. The owner reports that RTX Video Super Resolution "consumes
substantial processing time" (1080p PS5 stream, 2560x1440 180 Hz display); priorities: input latency, then pacing,
then image quality. **The owner's actual SR settings (choice, level, target) are unknown**: the evidence logs of the first
hardware run (BENCH1.md) contain no SR lines (no `[video-sr]`, no `[resolution]`, no `gpuSrP95Ms`). Everything below
about "the default" is what a fresh selection gets, not what the owner ran. "Optimise for my PC" measured super resolution at the CURRENT target (4K unless changed): see section 2.

File:line references were re-checked against the code of this branch on 20 November 2026 after the independent
review (an earlier version cited some lines that had drifted, and wrongly said noise reduction runs at 4K).

## 1. The choices

`EnhancementSettings::videoSrQuality` holds the choice (`include/veyra/engine/EnhancementSettings.h:92-121`):

| value | choice | API / runtime | temporal? |
|---|---|---|---|
| 0 | DLSS Super Resolution | NGX DLSS SR (`ngx::DlssSrBackend`, EnhanceGraph.cpp:3675-3693) | yes, reads optical-flow motion (EnhancementSettings.h:121) |
| 1-4 | RTX Video SR, standard family, level = value (1 low, 2 medium, 3 high, 4 ultra) | RTX Video SDK through NGX, feature `NVSDK_NGX_Feature_Reserved16` (src/ngx/VideoSrBackend.cpp:10) | no (spatial) |
| 5 | AMD FSR upscaling (`kVideoSrFsr`) | FidelityFX, any DX12 GPU (EnhanceGraph.cpp:3649-3672) | yes |
| 6 | NVIDIA Image Scaling (`kVideoSrNis`) | NIS 1.0.3 shaders, any DX12 GPU (include/veyra/pipeline/NisPass.h:1-12; EnhanceGraph.cpp:3694-3703) | no |
| 16-19 | RTX Video SR "high bitrate" family, tier = value-15 | NVIDIA Video Effects VideoSuperRes through CUDA (src/pipeline/VfxSrBackend.cpp; EnhanceGraph.cpp:3292-3303) | no |
| 21, 23 | RTX Video SR "streaming" family (medium / ultra only) | Video Effects, Ampere or newer (EnhancementSettings.h:102-117) | no |

The default for a fresh selection is **2, RTX Video SR standard medium** (`kDefaultVideoSrQuality=2`,
EnhancementSettings.h:101; also `videoSrQuality=kDefaultVideoSrQuality` at :404). The user picks the family in
*Professional → super resolution* (`srBackendChoices`, src/ui/bridge/Hardware.cpp:476-490) and the tier separately
(`SrStageEditor.qml:56-58`, `srTierChoices`, src/ui/bridge/Enhancement.cpp:105-112). **So by default it is not the top
level**: medium (2) of 4; the user may have chosen 4 ("ultra"). The quality level reaches the SDK only at evaluate
time: `params_->Set("VSR.QualityLevel",quality)` inside `VideoSrBackend::evaluate` (VideoSrBackend.cpp:39); the feature
is created without it (VideoSrBackend.cpp:22-30). A double SR (two passes, `dualSrEnabled`) is a further option
(Enhancement.cpp:44-58); everything below assumes a single pass unless it says otherwise.

## 2. Sizes actually used for a 1080p stream on a 1440p window

The SR output size is **not the window size**. It is the "SR target", `EnhancementSettings::srTarget`, whose default is
**4K, 3840x2160** (EnhancementSettings.h:403; target extents ResolutionPlan.h:27-39). Nothing clamps it to the window.
The live graph's plan takes it from the settings (`compileChainExecutionPlan(chain,{{w,h},settings.srTarget,...})`,
include/veyra/engine/GraphDescription.h:204, also :136; the sizes: `srStageExtents`, ResolutionPlan.h:79-105, "fit into
the box, enlarging only"). Nothing in the PS5 quick setup or the performance modes changes it (no
`srTarget`/`srTargetIndex` use in `PushModes.js`, `PushQuickSetup.qml`). Its writers are the SR editor's target row
(`SrStageEditor.qml:25`, `setSrTargetIndex`, src/ui/bridge/Export.cpp:260: 1 = 2K 2560x1440, 2 = 4K, ...), export and
presets.

**"Optimise for my PC" measured super resolution at the CURRENT target (4K unless changed).** Its `+sr2` test (offered when the screen is at least 1440 lines high and the
stream smaller) sets `videoSrQuality = 2` and turns SR on (`apply`, qml/Veyra/PushOptimizeDialog.qml:120; the step is
added at :423) but never touches the target, so it runs RTX Video SR level 2 **at the current target, 4K by default**:
whatever that test measured and recommended was SR into 3840x2160, then shrunk to the screen.

So with the defaults, for a 1920x1080 stream on a 2560x1440 window:

- SR input 1920x1080 (`srInW_` = source, EnhanceGraph.cpp:778), SR output **3840x2160** (`srOutW_` = work size, :779).
- The **work size** is 3840x2160 (the `resolution` log line, EnhanceGraph.cpp:796: `base`, `fg` and `output` print
  `workW_ x workH_`; `nr` prints `nrW_ x nrH_`, which is not the work size). HUD lock, colour and **frame generation run
  at 4K** (ResolutionPlan.h:160: `fg = output = base`).
- **Noise reduction does not run at 4K by default.** The default NR size policy is *Realtime*
  (EnhancementSettings.h:278 per layer, :464 globally), which caps NR at 1080 lines (`nrHeightLimit`,
  ResolutionPlan.h:23-24; applied in ResolutionPlan.h:143-152), so NR runs at 1920x1080 and its result is resized to
  the work size. Only a user who chose another NR size (1440p, native, a percentage) runs NR larger.
- The presenter then shrinks 3840x2160 to the 2560x1440 swap chain in `PresentBlit.hlsl`; for a minification it takes
  **one bilinear sample per output pixel** (`if(footprint>1.001)return sourceTex.SampleLevel(linearClamp,uv,0)`,
  shaders/PresentBlit.hlsl:87; the cubic path is only for magnification). A 1.5x bilinear minification skips part of
  the detail the SR produced and can alias thin lines.

With the target set to 2K the SR output and work size would be 2560x1440 (ResolutionPlan.h:31): 3.69 Mpixel instead of
8.29 Mpixel, **2.25x fewer output pixels** for SR, colour and frame generation (NR stays at 1080 lines either way; only
its resize to the work size gets smaller), and no minification at present.

The optional "SR output" (supersample shrink, `srOutput`, EnhanceGraph.cpp:779, `SrResolve.hlsl`, default `Target` =
off, EnhancementSettings.h:411) would evaluate at the target and area-reduce into a smaller work size; it is off by
default.

## 3. Passes around the RTX Video SR evaluate (single pass, `finalVideoSr`, EnhanceGraph.cpp:3347-3372)

The graph works in linear FP16 (NisPass.h:7 "the graph's linear FP16 colour"); the RTX Video SDK takes encoded SDR
RGBA8 only (`evaluate` refuses anything but `R8G8B8A8_UNORM`, VideoSrBackend.cpp:35). So each real frame gets:

1. an **encode pass** at the input size: linear -> RGBA8 `videoSrInput_` (1920x1080), EnhanceGraph.cpp:3351 (HDR: a PQ
   encode, constant `3.0`, :3350);
2. the **SDK evaluate** 1920x1080 -> `videoSrOutput_` 3840x2160 (:3354);
3. a **decode pass** at the output size: RGBA8 -> linear FP16 work texture (3840x2160), :3360-3369; in HDR a restore
   pass (`hdrVideoSrPass_`, :3366) that also reads the encoded input as a base, since an 8-bit SDR model cannot carry HDR.

Two extra full-screen passes, one of them at 4K. The Video Effects families add a submit, a CUDA run and a copy in and
out on another API (`runVfx`, :3292-3303). NIS is three compute passes of its own (encode, scale at up to 2x, decode:
NisPass.h:7-11, NisPass.cpp:123-136). FSR writes the work texture directly (:3665).

## 4. Where it sits on the frame path

- **Once per real frame, never per generated frame.** `runSr` (EnhanceGraph.cpp:3640) is called from the real frame's
  recording (`if(!desc_.nrBeforeSr&&!runSr())`, :3727, and the two node-order variants after it); frame generation
  interpolates between the already super-resolved work textures. So the SR cost is **not** multiplied by the frame
  generation multiplier, but frame generation itself runs at the SR target size (4K by default), which multiplies
  **its** cost by the pixel count. (This is why a "super-resolve real frames only" experiment,
  `VEYRA_EXP_SR_REAL_ONLY`, was not built: that is already how it works.)
- **Synchronous with the frame.** The SR evaluate is recorded in the same command list, on the same direct queue, as
  the rest of the real frame (`gpuTimer_.mark(list,GpuStage::Sr)` around it, :3373-3379). The engine sets the frame's
  ready fence to signal an event (`SetEventOnCompletion(lease->readyFence,gpuReadyEvent.handle)`,
  src/engine/EngineController.cpp:2929) and presents a frame only after its GPU work is done. So, by the structure of the
  code, **the SR stage's GPU time is added to the arrival-to-present latency one for one**, plus the encode/decode
  passes and the extra cost of every later stage at the larger size. How many milliseconds that is can only be
  measured on hardware.
- **Repeated pictures.** Only while the content rate is *Transport*, the default of a fresh profile
  (EnhancementSettings.h:491), does every stream frame go through the graph, including the repeats of a 30 FPS game in
  a 60 FPS stream. The quick setup switches to automatic duplicate removal (content rate 1) as soon as a frame
  generation target, adaptive mode or pacing repair is set (qml/Veyra/PushQuickSetup.qml:147, 185, 213), and "Optimise
  for my PC" and the performance modes write the content rate too. With duplicate removal on, a repeat is dropped
  before the graph (`if(!decision.keep){...continue;}`, EngineController.cpp:2321), so SR runs once per new picture.
  Which case the owner ran is unknown (no SR or content-rate lines in the evidence).
- **HDR**: same evaluate on a PQ-encoded 8-bit picture, plus the HDR restore pass (above).
- **Level changes**: changing `videoSrQuality` rebuilds the graph (it is in `applySettings`' list of things it cannot
  apply in place, EnhanceGraph.cpp:4512-4513), although the SDK takes the level at evaluate time; the experiments below
  change the evaluated level without touching the setting, hence without a rebuild.
- **`enhancementProcessingMs`** (`[player-timing]`, `[output-queue]`) is the union of the GPU spans of Flow, SR, NR (all
  layers), Residual, FgBatch, anti-aliasing and beauty nodes for one graph record (include/veyra/diagnostics/EnhancementProcessingTime.h:13-50).
  Colour, the presenter blit and waits are not in it. The SR stage alone is already logged every second as
  `gpuSrP95Ms` in `[player-timing]` / `[capture-timing]` (EngineController.cpp:3378, :3393) when those lines are written.
  `[slipstream-sr]` (below) adds P50, input/output size, level and the level's source, per configuration. It brackets
  **only the SR stage** (`GpuStage::Sr` marks around `runVideoSr`, EnhanceGraph.cpp:3373-3379: the encode and decode
  passes included, and for the Video Effects families the CUDA round trip). It does **not** show the extra cost that a
  4K work size puts on frame generation, colour and the presenter's shrink: read each hold's `hold-end latencyP95Ms`
  beside it.
- **VRAM**: the RTX path keeps `videoSrInput_` (1920x1080x4 B = 8.3 MB) and `videoSrOutput_` (3840x2160x4 B = 33 MB)
  (EnhanceGraph.cpp:1015); the graph's own estimate charges `workW_*workH_*(76+4*generatedTextures)` bytes for the work
  textures (:890): at 4K about 630 MB plus 33 MB per generated texture, at 1440p about 280 MB. Plus the SDK's own model.
- **Low-latency queue and fence wait.** SR changes neither; it lengthens the GPU work that the ready-fence wait (above)
  and, with the low-latency queue on, `Present()` with `maximumLatency=1` (BENCH1.md result 3) wait for. With the low
  queue on the extra time shows inside `pushfpsMs`/`softwareLatencyMs`; with it off the frame waits in the DXGI queue
  instead, where that number cannot see it (BENCH1.md result 3).

## 5. Why it is expensive here (ranked, by reading)

1. **The output is 4K, not the window's 1440p** (section 2): 2.25x the output pixels the display can show. RTX Video
   SR's cost grows with the output size (BENCH1.md, "Facts looked up"), and the 4K work size then also feeds colour,
   frame generation (whose cost is per output pixel) and a 4K->1440p shrink at present. (NR is not among them by
   default: it stays capped at 1080 lines, section 2.)
2. **The level** (1-4). The default is 2; at 4 the SDK runs its most expensive model (BENCH1.md: users report several
   times the GPU power at 4 vs 1; a 2025 model update cut the top level's cost by up to 30 %).
3. **It runs on every stream frame** while the content rate is Transport (a fresh profile's default; the quick setup
   leaves it as soon as frame generation targets, adaptive mode or pacing repair are used, section 4), i.e. then 60
   evaluations a second for a 30 FPS game.
4. **Two extra full-screen passes** (encode at 1080p, decode at 4K) and, in HDR, the restore pass.
5. **It is serial with presentation**: nothing hides its time; it is latency, not just GPU load.

## 6. Cost in principle (relative; nothing measured)

Output pixels per real frame for a 1080p input: 1440p 3.69 M, 4K 8.29 M (2.25x). Per the NVIDIA notes in BENCH1.md
the level multiplies cost on top (level 1 cheapest; level 4 "several times" level 1). NIS: three simple compute passes,
no network; FSR: one compute effect with motion input; bilinear (SR off): the graph's resize only, or nothing if the
work size equals the source (EnhanceGraph.cpp:800: SR is "enabled" only when it changes the size or restores). The
order of cost expected by reading is: off < NIS ≈ FSR < RTX 1 < RTX 2 < RTX 3 < RTX 4, each RTX level at 1440p well
under the same level at 4K. **To be confirmed by the `sr` autorun step on the owner's hardware.**

## 7. Hypotheses per performance mode (for the hardware run to confirm or reject; not a recommendation yet)

| mode | super resolution | why |
|---|---|---|
| Ultra Low Latency | **off** (or NIS if the picture is too soft) | every millisecond of SR is a millisecond of latency |
| Competitive | **NIS at the window's size** (SR target 2K on a 1440p screen) | single-pass shaders, no network, no 4K detour |
| Balanced | **RTX Video SR level 1 or 2 at the window's size** (target 2K on 1440p) | the AI clean-up of stream compression at the lowest output cost |
| Maximum Smoothness | **RTX Video SR level 2 at the window's size**; level 3-4 only if `[slipstream-sr] gpuP95` stays under the budget | frame generation runs at the work size, so the 4K detour costs twice |

Hypothesis for every mode: set the SR target to the window's size, not 4K (the existing *SR target* row; nothing new).
By reading that is the largest saving, and the hypothesis is that it **costs no picture detail the 1440p screen can
show**, since the presenter shrinks the 4K picture with one bilinear sample per pixel anyway. That is not established:
RTX Video SR's network may produce detail that survives the shrink better than a direct 1440p output; the scored
recordings of section 9 must show it. This document changes no default.

## 8. What was added (all off by default)

| switch / step | what it does | where |
|---|---|---|
| `VEYRA_EXP_RP_STATS=1` | `[slipstream-sr] choice= quality= in=WxH out=WxH gpuP50= gpuP95= frames= generatedToo=0 gpuMax= work=WxH hdr= dual= levelSource=user\|autorun\|switch\|budget budgetMs=`, one per 5 s and one whenever the configuration changes, from the GPU timestamps the graph already takes. `quality` = the RTX level evaluated (tier for the Video Effects families, -1 otherwise) | `EnhanceGraph::noteSrTimings` (EnhanceGraph.cpp:4655), `PushSrWindow`, `pushSrStatsLine` (include/veyra/engine/PushSrPlan.h) |
| `VEYRA_EXP_SR_QUALITY=1..4` | the standard RTX Video SR pass evaluates that level for the whole run, whatever the setting (higher or lower); the setting and the session are untouched; no rebuild | `srLevelFor`, EnhanceGraph.cpp:3354 |
| `VEYRA_EXP_SR_BUDGET_MS=<0.5..50>` | the level steps down one when the SR stage's 5 s P95 exceeds the budget for 2 windows in a row (10 s apart at least), up one after 6 windows under 60 % of it and not within 60 s of a step down, and also, once that lockout has passed, after 6 windows merely under the budget (one try even when the lower level costs more than 60 %: the delta2 review's model, level 3 at 2.0 ms and level 4 at 2.8 ms with a 3 ms budget, sat at level 3 for 6 hours; now back at level 4 after 85 s, Linux model); a step up taken back within 60 s doubles that lockout (up to 16x = 16 min; a step up that holds clears it), so a cost just over the budget at one level and well under at the next no longer cycles once a minute (Linux model: 103 changes an hour before, 13 in the first hour and 8 an hour after; a level over the budget above one in the band between 60 % and 100 % now also tries and is taken back, with the same numbers: 13, then 6 to 8 changes an hour); never above the user's level; the level the graph evaluates follows the budget after every window, also when the user chooses another level; at level 1 it logs `event=budget-floor` and stops (off/NIS stays the user's decision). Logs `[slipstream-sr] event=budget ...` per change | `PushSrBudget`, `pushSrBudgetObserve` (PushSrPlan.h), `noteSrTimings` |
| autorun `sr:<s>` | holds, `s` seconds each (default 20): off, NIS, FSR (when available), RTX standard level 1-4 at the user's target, then level 1-4 at the window's target if that is smaller; then the user's settings back. Per hold: `[slipstream-sr] event=hold ...`, the engine's `[slipstream-sr]` windows, and `event=hold-end ... fps= latencyP50Ms= latencyP95Ms=` from the benchmark window (after a settle of min(5 s, s/3)) | `pushCompareStartStep` / `pushComparePollStep` / `pushCompareCancelStep`, src/ui/bridge/PushCompare.cpp; plan `pushSrPlan` |
| autorun `fg:<s>` | frame generation off, then each multiplier of the current method; `[slipstream-fg]` at the end of each hold (`hold=1`); then the user's multiplier back, and with frame generation off before the step also the multiplier it keeps while off (`event=restored ... kept=`; when the current method does not offer the kept multiplier it cannot be put back, the last hold's stays kept and the line says `ok=0 reason=multiplier-not-restorable`). With the adaptive multiplier on, the hold lines end in `adaptive=1`: the multiplier in force can then be below the labelled one | PushCompare.cpp, `pushFgPlan`, `pushFgRestoreSteps` |
| autorun `quality` | starts the existing frame quality test (`pushQualityBegin`, 10 s) and waits for it; its own `[push-quality]` result line is the result | PushCompare.cpp |
| `VEYRA_EXP_RP_STATS=1` | `[slipstream-fg]` every 5 s while frame generation is on (outside an `fg` step) | `registerPushCompareAutorunSteps` |

Precedence of the evaluated level: autorun hold > `VEYRA_EXP_SR_QUALITY` > budget > the user's level
(`pushSrEffectiveLevel`). Only the single standard-family RTX pass (values 1-4, no double SR) is ever affected.

`[slipstream-fg]` keys the bridge cannot see are `-1`: `generatedExpired`, `skippedPairs`, `shownGapP95`, `shownGapMax`
(read them from the engine's `[frame-flow]` line: `generatedExpiredAfterEval`, `fgSkippedBeforeEval`). `costP50/P95`
are PushFlow's network times (`pushFlowP50Ms/P95Ms`), -1 for other methods. `pairDelayMs` is a model value: frame
generation shows a real frame one content interval later, so 1000 / source FPS with a multiplier above 1.

### Risks and limits

- The level override assumes the SDK honours a different `VSR.QualityLevel` on each evaluate of one feature handle, as
  the code passes it per call (VideoSrBackend.cpp:39). Unverified on hardware, and nothing in the code tells what the
  SDK does otherwise. It might refuse it (`[video-sr] op=1` errors, and the frame fails over like any SR failure), or,
  as likely, ignore it silently and keep the level it first ran with. The `sr` step's output shows the silent case:
  the four `rtx1`..`rtx4` holds (`event=hold ... level=1..4`, and `quality=1..4` in their `[slipstream-sr]` lines)
  would all show the same `gpuP50`/`gpuP95` within run-to-run noise and the same `hold-end latencyP95Ms`, where the
  levels should differ clearly; and under `VEYRA_EXP_SR_BUDGET_MS` the budget would walk down to level 1
  (`event=budget` lines with an unchanged `gpuP95`, then `event=budget-floor`) without the cost falling. If that is
  seen, a level change needs the feature to be created again, and both experiments must be treated as not working.
- Off/NIS/FSR/target holds change the settings through the user's setters: the graph rebuilds (the settle period hides
  that from the hold's numbers). The user's settings go back at the end, on failure and on cancel. While an `sr`, `fg`
  or `quality` step runs the session file is **not written** (`engine::pushHookHoldActive`,
  include/veyra/engine/PushHookHold.h, checked in `persistSession`, src/ui/QmlPlayerBridge_p.h; normally the file follows
  a change within 300 ms, QmlPlayerBridge.cpp:453-463), so a crash or a close during a hold leaves the user's own
  choice in the session; the restore is saved as usual. Chosen over a restore record (like `pushOptimizeRestore` /
  `pushGovernorRestore`) because it needs no new preference and nothing applied at the next start. Preferences are
  never written. Gap (code reading, delta2 review): a user who switches the mode (preset) while a hold runs reaches two
  direct `saveChainSession` calls in the mode switch (src/ui/bridge/Chain.cpp:261, 264) that do not go through
  `persistSession`, so they bypass the hold gate and write the held SR settings into the session. Only that user
  action reaches them; the autorun runner never switches mode. The hook steps only run from the autorun runner (`VEYRA_TEST_RP_AUTORUN`).
- The steps join the autorun runner through `engine::autorun::registerStep` (include/veyra/engine/PushAutorunPlan.h,
  stream "auto": Start once, Poll every tick, Cancel on timeout or abandon). `registerPushCompareAutorunSteps`
  (declared in include/veyra/engine/PushCompareSteps.h) must be called once after the bridge exists; the call site
  belongs to the auto stream.
- A window left open when SR is switched off is written at the next SR window (delayed, with its own configuration).

## 9. Image quality: offline CPU model

`tools/image_quality/sr_compare.py model` (Python + numpy + OpenCV; ffmpeg for the stream): synthetic game-like frames
at 2560x1440 (buildings with lit windows, foliage noise, thin wires, sign text, a HUD; scenes still / pan / fast pan),
area-downscaled to 1920x1080, through **HEVC at 15 Mbit/s, 60 fps** (libx265 preset fast, 1 s lead-in), upscaled back
by CPU scalers and scored against the 1440p reference (luma PSNR, SSIM, sharpness = gradient energy vs reference,
overshoot = halo outside the reference's local 3x3 range on edges). "sharpen" is a lanczos + adaptive clamped unsharp
mask, a stand-in for a single-pass sharpening scaler, **not NIS**. The two "4k-then" rows model the app's default path:
an upscale to 3840x2160 and the presenter's bilinear shrink (or an exact area shrink) to 1440p.

**Labelled: offline CPU model, synthetic material, 6 frames x 3 scenes; no GPU, no RTX Video SR, no NIS.** Its HEVC
step (offline libx265, preset fast) is kinder to the picture than a console's real-time encoder at the same bitrate,
so the compression losses here are lower bounds. Results:
`tools/image_quality/results/sr_model_hevc15.json` and `sr_model_uncompressed.json`.

Mean over 3 scenes x 6 frames (worst = the lowest single-frame PSNR, averaged over scenes):

| scaler (1080p -> 1440p) | HEVC 15 Mbit/s: PSNR dB | worst | SSIM | sharpness | overshoot | uncompressed: PSNR dB | SSIM |
|---|---|---|---|---|---|---|---|
| bilinear | 29.34 | 29.23 | 0.9400 | 0.883 | 0.000 | 29.75 | 0.9481 |
| bicubic | 30.77 | 30.63 | 0.9547 | 0.978 | 0.002 | 31.30 | 0.9630 |
| lanczos4 | 31.01 | 30.88 | 0.9556 | 0.981 | 0.004 | 31.55 | 0.9638 |
| sharpen (lanczos4 + adaptive clamped unsharp mask; not NIS) | 31.36 | 31.22 | 0.9554 | 1.045 | 0.027 | 32.05 | 0.9659 |
| lanczos4 to 3840x2160, then bilinear to 1440p (the app's default path, presenter shrink) | 30.43 | 30.32 | 0.9511 | 0.950 | 0.000 | 30.93 | 0.9590 |
| lanczos4 to 3840x2160, then exact area to 1440p | 30.23 | 30.12 | 0.9491 | 0.947 | 0.001 | 30.67 | 0.9568 |

What it says, within the model: going through a 4K target and shrinking to 1440p **loses** about 0.6 dB (bilinear
shrink) to 0.8 dB (area shrink) against scaling straight to 1440p with the same filter, and the picture is softer
(sharpness 0.95 against 0.98); so for the non-AI choices the 4K detour costs time and some detail. A mild sharpening
scaler scores best here (+0.35 dB over lanczos4 with HEVC) at the price of a small halo (overshoot 0.03 levels on
edges). Stream compression costs every scaler about 0.5 dB. The bilinear shrink scoring above the area shrink is
the area filter's extra softness, not aliasing being harmless: these synthetic scenes have few periodic patterns. Whether RTX Video SR's network detail survives the shrink better is
exactly what the hardware scoring below must show.

### Scoring the RTX output on hardware (the yardstick)

1. `python3 tools/image_quality/sr_compare.py make-reference --out ref` writes `ref-reference.mkv` (2560x1440,
   lossless FFV1), `ref-1080p.mkv` (lossless) and `ref-1080p-hevc15.mkv` (the 15 Mbit/s stream).
2. Feed `ref-1080p-hevc15.mkv` to the app as a video file (*Watch → open file*, `openFileDialog`, the video playback
   mode in CLAUDE.md) or through the Frame Input API (`frameinput:`, docs/FRAME-INPUT-API.md; the sample sender
   `tools/frame-input/send_test_pattern.py` sends a test pattern, a file sender would be a small variant). The file
   path decodes like a stream, not like the PS5's Remote Play decoder; that difference is the test's limit.
3. Record the processed picture with the ordinary recorder (`toggleRecording`; *Settings → recording*): the recorder
   captures the presented frames after every effect (`LiveRecorder::capture`, CLAUDE.md "Before/after comparison
   clips": the PushFPS side is "every frame"). Set the recording size to 2560x1440 (`recordWidth`/`recordHeight`; the
   canvas pass box-filters a 4K work picture down, `PushCanvasConvert.hlsl`) and a high quality
   (`recordAdvanced` on, constant-quality rate control, high `recordQuality`) so the encoder adds little loss; frame
   generation off. Do one recording per SR choice and level (the `sr` step's list).
4. `python3 tools/image_quality/sr_compare.py score --reference ref-reference.mkv --recording <file>.mp4 --label rtx2`:
   each recorded frame is matched to the best reference frame and scored like the model rows. Compare rows only from
   recordings with identical settings except the SR choice; the recorder's own encode is in every row.

## 10. What the hardware run must show before any default changes

From one session with `VEYRA_EXP_RP_STATS=1` and the plan `settle:10;sr:20;fg:15;quality;exit` (wired LAN if possible;
Wi-Fi runs are diagnostic only), a table per hold:

| hold | `[slipstream-sr] gpuP50 / gpuP95` (ms) | in -> out | `hold-end latencyP50 / P95` (ms) | `hold-end fps`, gaps | `[slipstream-latency] arrivalToScanoutP95` (stream "present") |
|---|---|---|---|---|---|
| off | — | — | | | |
| nis | | 1920x1080 -> target | | | |
| fsr | | | | | |
| rtx1 .. rtx4 (user's target, 4K by default) | | 1920x1080 -> 3840x2160 | | | |
| rtx1-window .. rtx4-window (2K) | | 1920x1080 -> 2560x1440 | | | |

A default may change only if: the 2K rows cost clearly less than the 4K rows at the same level (expected ~2x by
pixels), the latency P95 difference between off and the chosen row is known, no hold shows dropped presents (`gaps`)
that off does not, and the scored recordings (section 9) show the chosen row is not worse than the 4K row it replaces.
