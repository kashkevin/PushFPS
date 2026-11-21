# Visual reference: the owner's own configuration, and how a new combination is compared with it

Written 20 November 2026 (sprint 5, Package 8, stream "pipeline", internal codename Slipstream) by reading the code
of branch `exp/s5-pipeline`. **Nothing in this document was measured by us.** The numbers in section 1 are the
owner's own readings from his own play. Everything about what they "most likely" are is code reading.

## 1. The reference, exactly as the owner reported it

His PC: RTX 5070, Ryzen 7 8700F, 2560x1440 at 180 Hz, PS5 by Remote Play, wired Ethernet since 20 November.

| What he set | Value |
|---|---|
| Upscaler | DLSS Super Resolution 4.5, **Model M** |
| Output (upscaler target) | **4K** (3840x2160), shown on the 2560x1440 screen |
| Frame generation | NVIDIA **VFG 2x** |
| Stream | **H.265, HDR** |

| What he read | Value |
|---|---|
| Submitted frame rate | 120 FPS |
| 1 % low | 93.2 FPS |
| GPU | 61 % |
| Processing time | 9.3 ms |
| Reported application latency | 23 ms |

These are **his readings, one configuration, one scene, software counters**. They are a baseline to compare with,
not a guarantee and not a measurement of ours (S5_DIRECTIVE.txt, section 7: "Use my measured RTX 5070 results as an
initial baseline, not a performance guarantee").

## 2. What each number most likely is in the app (code reading; "most likely", not confirmed with him)

| His reading | Most likely counter | Where it comes from | What it is not |
|---|---|---|---|
| 120 FPS submitted | `submitFps` on the Professional page; `presentSubmitFps` in `[frame-rate]` and `[frame-flow]` | Present calls per second (`include/veyra/ui/QmlPlayerBridge.h`, "live performance") | Not pictures the panel showed, and not unique motion (section 5) |
| 93.2 FPS 1 % low | `submitLowFps` | the 1 % low of the presents over the last ten seconds (`FramePaceWindow`; `EngineController.h` `submissionLow1Fps`) | Not a scanout measurement |
| 61 % GPU | `gpuUtilization` on the Professional page, or his overlay | the whole card's load as Windows reports it | Not the app's share alone; BENCH1.md saw the NVIDIA overlay injected |
| 9.3 ms processing | `chainTotalMs` ("measured enhancement-interval ... NOT full-chain latency", `QmlPlayerBridge.h`) = the mean of `enhancementProcessingMs` | the union of the GPU spans of flow, super resolution, the neural layers, residual, the generated batch (`include/veyra/diagnostics/EnhancementProcessingTime.h`) for one real frame | Colour conversion, the presenter's blit and every wait are not in it |
| 23 ms application latency | `latencyMs` of the latency badge = `softwareLatencyMs` (`FrameFlowWindow.h`) | frame arrival from the network to `Present()` returned, a mean | Not input-to-photon; with the low-latency queue off part of the wait hides in the display queue (BENCH1.md result 3) |

With frame generation by interpolation, 23 ms contains the wait for the next real frame: a pair delay of one content
interval is the method's nature (`pushFgPairDelayMs`, `include/veyra/engine/PushSrPlan.h`), 16.7 ms at 59.94 real
frames a second. It is latency he accepted for smoothness; it is not an inefficiency to remove.

## 3. The settings that reproduce it

| Setting | Value | Where |
|---|---|---|
| Super resolution on, method DLSS SR | `videoSrQuality = 0` | *Professional -> super resolution -> method* (`srBackendChoices`, `src/ui/bridge/Hardware.cpp`) |
| DLSS SR model | **M** = `dlssSrModel = 1` ("DLSS 4.5 second-generation transformer (preset M)", `include/veyra/engine/EnhancementSettings.h`; 0 = K, the app's default; 2 = L) | the model row of the same editor (`dlssSrModelChoices`, `setDlssSrModel`, `src/ui/bridge/Enhancement.cpp`) |
| Target | 4K = `srTarget = Uhd4K`, the app's default | the *SR target* row (`SrStageEditor.qml`) |
| Frame generation | method VFG (`FrameGenerationBackend::Vfg`), multiplier 2 | quick setup / Professional |
| Stream | PS5 Remote Play, HEVC, HDR on | the connection dialog |
| Neural layer, clean-up | off | |

The test hook `VEYRA_TEST_RP_PIPELINE` spells the same thing `sr=dlss;srModel=1;srTarget=4k;fg=vfg;fgMultiplier=2;hdr=1`
(S5_CONTRACT.md item 1; the hook is stream "nrrun"'s work and arrives with Package 8C: in a Package 8A build set the
rows above by hand, and the kit's arms that need the hook say so), and the planner's profile `max-fidelity` is
this configuration (`pipelineProfileRequest`, `include/veyra/engine/PushPipelinePlan.h`): DLSS SR model M, 4K target with supersampling asked for explicitly,
VFG 2x, no clean-up, no neural layer. It stays the "strongest verified combination" until a hardware run shows a
better one: nothing else has been seen on a GPU.

About "Model M": the model is passed to the SDK when the feature is created, and the log says which one runs
(`[ngx] sr-backend: model=DLSS 4.5 (preset M) renderPreset=...`, `src/ngx/DlssSrBackend.cpp`); the earlier field problem with model
choices (docs/SR_MODEL_SELECTION_DIAGNOSIS_2026-11-06.md) was a wrong conversion of the upscaler METHOD id in the
interface of an older version (RTX Video SR choices arrived as DLSS, `quality=0`), not a fault of the DLSS models.
NVIDIA recommends M for half resolution per axis and K for the higher-resolution modes (S5_NR_RESEARCH.md); 1080p to
4K is 0.5 per axis, which is the case M is meant for. 1080p to 1440p is 0.75: whether M or K is better there is
open, and the kit's arms `v-dlss` (M) and `v-dlss-k` decide it.

What the reference costs by its structure (docs/SUPER_RESOLUTION.md, docs/NEURAL_RENDERING.md): the work size is
3840x2160, so frame generation and the final write run on 2.25 times the pixels of the screen, and the presenter
shrinks the picture to 2560x1440 with one bilinear sample per pixel. Whether a 1440p target looks the same or
worse is exactly what the comparison below must show.

## 4. How the kit compares a new combination with it

The kit (stream "kit": `scripts/remoteplay/testkit/run-tests.ps1`, `tools/remoteplay_latency_bench/report.py`)
runs each arm in its own app start with the test hook, the same scene, HDR and SDR never mixed, and reads only log
lines. The arm `v-reference` is the configuration above; `v-original` is the untouched stream.

Per arm the report takes:

1. **Cost**: `[slipstream-nr] graphMsP50/P95`, `srMsP95`, `fgMsP95`, `nrMsP95`, `vramMiB` (per real frame;
   `include/veyra/engine/PushNrStats.h`), `[slipstream-sr]` for the upscaler alone, and the hold's latency lines.
   A new combination is compared with `v-reference` on `graphMsP95` and on the latency P95 of the same run.
2. **Picture**: the autorun step `probe:<seconds>` (`include/veyra/engine/PushPipelineProbe.h`) logs two
   `[slipstream-quality]` lines, `stage=raw` (the decoded console picture) and `stage=output` (the graph's output at
   its work size). The numbers are the no-reference metrics of `include/veyra/engine/PushImageMetrics.h`:
   blockiness, banding, ringing, detail, flicker, hudDelta, mean luma. They are software numbers on luma, each blind
   to things its definition lists; none is a judgement of beauty. Arms are compared on the output line against
   `v-original`, and a new combination against `v-reference`, with the kit's fixed rule (10 % steps, detail kept).
3. **Frame generation**: `[slipstream-fg]` and `[frame-flow]` (section 5).

Limits to keep in mind when reading such a comparison:

- The output line is the graph's output, before the presenter's resize. For `v-reference` that is the 4K picture,
  for a 1440p arm the 1440p picture: the presenter's shrink of the 4K picture is not in it. An arm at 4K can look
  better in these numbers than it does on the 1440p screen. The eye on the real screen, and a recorded clip at
  2560x1440 (docs/SUPER_RESOLUTION.md section 9), close that gap.
- `detail` cannot tell detail from sharpening halos or noise; read it with `ringing` and `flicker`.
- `flicker` reads 0 for a pair of equal pictures: with a 30 FPS game in a 60 FPS stream and no duplicate removal,
  half the pairs are equal.
- A probe keeps the raw picture (two extra copies per real frame): never take timing from a window with a probe.

## 5. Frame generation: what "120 FPS" does and does not say

**Submitted FPS is Present calls.** It counts every picture handed to Windows: real frames, generated frames, and
repeats. It is not the number of pictures that show new motion.

- A 30 FPS game inside a 59.94 FPS stream delivers every game picture twice. With the content rate left at
  *Transport* (a fresh profile's default) the graph treats all 59.94 as real: VFG 2x then makes 59.94 in-between
  pictures, half of them between two EQUAL pictures (an equal picture again). 120 are submitted; about 60 differ
  from their predecessor. The planner reports this bound as `uniqueMotionFpsMax` (content rate x multiplier, never
  above the submitted rate; `PushPipelinePlan.h`).
- With duplicate removal by the user's choice (content rate automatic or 30) and 4x, 30 real + 90 generated are
  submitted: 120 again, with a different meaning and a longer pair delay (33 ms).

Counters that separate real from generated:

| Line | Keys | Meaning |
|---|---|---|
| `[frame-flow]` | `realPresented`, `generatedPresented`, `fgCandidate`, `fgEvaluated`, `fgSkippedBeforeEval`, `generatedExpiredAfterEval`, `fgInvalid`, `sourceSkippedBeforeGraph` | real and generated pictures actually presented; pairs the budget skipped; generated pictures made too late to show; pictures dropped before the graph |
| `[frame-rate]` | `completedReal`, `gpuCompletedFps`, `presentSubmitFps` | real frames the GPU finished against Present calls |
| `[slipstream-fg]` | `sourceFps`, `outputFps`, `generatedShown`, `generatedExpired`, `skippedPairs`, `shownGapP95`, `shownGapMax`, `pairDelayMs` | per hold of the `fg` step; keys the bridge cannot see are -1 |

What the logs can indicate about stutter: skipped pairs and expired generated frames (a generated picture missing
= a visible step), the gaps between shown pictures (`shownGapP95/Max`, `[display-latency]`), the 1 % low. What they
cannot give: **ghosting and warping**. No counter sees a wrong picture; the ground-truth frame quality test
(`quality` step, `PushQualityProbe`) compares generated pictures with withheld real ones but needs pictures in CPU
memory and reports hardware-decoded streams as unsupported. For a PS5 stream ghosting is judged by eye and from a
recorded clip.

**The automatic duplicate-frame detector stays off.** The GPU duplicate-frame detection for hardware-decoded
pictures (`VEYRA_EXP_RP_STREAM_DEDUP`, row 8 of docs/SLIPSTREAM_STATUS.md) was rejected three times because every
version dropped genuine pictures in some scene. It is off unless that switch is set, nothing in this sprint turns it
on, and the planner has no field for it. Duplicate removal remains the user's explicit content-rate choice.

Targets: 120 FPS is the frame-generation target; 180 FPS is planned only when asked for as explicitly experimental
(`fgExperimental180`), and the plan is then marked `experimental`.
