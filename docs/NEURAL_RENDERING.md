# The enhancement pipeline for a PS5 stream: stage order, hand-offs, shared and repeated work

Written 20 November 2026 (sprint 5, Package 8, stream "pipeline", internal codename Slipstream) **by reading the
code** of branch `exp/s5-pipeline` (base `exp/rc5`). Line numbers are those of `exp/rc5` (later merges shift them;
search for the named function); the clean-up stage of
stream "restore" is not in it yet and is described from its contract. **Nothing here was measured by us and nothing
has run on a GPU.** Section 8 keeps what the code proves apart from hypotheses, and names the test that decides
each. Companion documents: docs/SUPER_RESOLUTION.md (the upscalers in detail), docs/VISUAL_REFERENCE.md (the
owner's reference), docs/RESTORATION.md (stream "restore"), docs/NEURAL_RENDERING_RUNTIME.md (stream "nrrun"; it arrives with
Package 8C, the download that adds the neural layer's runtime work: Package 8A has this document without it).

"Neural layer" = the app's NR layers = "AI picture enhancement" = what the interface calls DLSS 5 NR: NVIDIA's
`nvngx_dlssnr.dll` run on decoded video. It is **not** NVIDIA's game-integrated DLSS 5: there the model receives
motion vectors, depth, jitter and a mask from the game engine and an LDR picture (S5_NR_RESEARCH.md); here it
receives a decoded picture, optical flow or zero motion, and a constant depth. It stays optional and experimental,
and **it is not verified for HDR**: its behaviour and memory need on decoded HDR video are undocumented. The
planner marks every plan with it `experimental` and adds the note `neural-hdr-unverified` on an HDR stream.

## 1. Stage order today (one real frame of a PS5 stream)

All of it is `EnhanceGraph::process` (src/pipeline/EnhanceGraph.cpp:2828), recorded on the graph's own direct
queue, once per real frame. The log line `[pipeline-order]` (EnhanceGraph.cpp:797) names the order in force.

1. **Decode** (FFmpeg D3D12VA, or D3D11VA through a shared texture): the decoder's NV12 / P010 texture is used
   as it is; the graph waits on the decoder's fence on the GPU, no copy (:3030-3055, "decoder allocation padding
   is never read").
2. **Colour ingress** `GpuStage::Color` (:2949-3257): one compute pass, YUV to linear FP16 `srcRgba_` at the
   stream's size ("2. YUV -> RGBA16F", :3191). Grading, when on, is fused into this pass.
3. **Clean-up slot** (stream "restore", S5_CONTRACT.md item 6): on the decoded picture, at the stream's size,
   before any upscaler or neural layer; its own span and `[slipstream-clean]` line.
4. **Optical flow** `GpuStage::Flow` (:3383-3528), only when something reads it (`runMotion`, :3383): two small
   passes build the flow inputs at the flow size (:3389-3398), then NVIDIA Optical Flow, AMD FidelityFX or the
   GPU DIS flow. Afterwards the field is adapted to the neural layer's size (`nrFlow_`) and to the work size
   (`baseFlow_`) (:3504-3531).
5. **Upscaler** `GpuStage::Sr`, `runSr` (:3640-3716): source size to the SR target = the **work size**
   (4K by default, docs/SUPER_RESOLUTION.md section 2).
6. **Neural layers** `GpuStage::Nr` + `NrLayer0..3` (:3788-3987), default order *after* the upscaler
   ("SR -> NR -> residual -> FG"); with the node order "NR first" they run on the source instead
   (`desc_.nrBeforeSr`, :3727 / :4068). Then the **residual composite** `GpuStage::Residual` (:3993-4064).
7. Tail colour nodes, RTX Video HDR when on (`GpuStage::VideoHdr`, :4075-4089).
8. **Final write** (:4090-4104, "5. Working frame -> ... videoFrame[parity]"): the FP16 work picture is encoded
   into the output frame at the work size: RGBA8 for SDR, R10G10B10A2 (PQ) for HDR10, FP16 for scRGB.
9. **Frame generation** `GpuStage::FgBatch` (:4135-4360): reads the encoded output frames of this and the
   previous real frame; generated frames are further output frames.
10. **Presentation** (src/engine/VideoPresenter.cpp): `PresentBlit.hlsl` scales the output frame to the swap chain
    (`GpuStage::Blit`), then `Present`. Recording, the HUD sampler and the pipeline probe read the output frame in
    the same command list (PresentCapture.h).

## 2. Hand-offs: which texture, format and size; copy or conversion; why

Sizes for a 1920x1080 stream; "work" = the SR target (3840x2160 by default, 2560x1440 when set to 2K).

| From -> to | Texture | Format | Size | Copy / conversion | Why |
|---|---|---|---|---|---|
| decoder -> colour ingress | the decoder's own surface | NV12 (SDR) / P010 (HDR) | 1080p | none (shader reads it) | zero copy, GPU-side fence wait |
| colour ingress -> everything | `srcRgba_` | linear FP16 RGBA | 1080p | 1 conversion pass | the graph works in linear FP16 |
| source -> flow | `nvofInA_/B_` | encoded, flow format | flow size (<= source; follows the neural layer's size when that is reduced, ResolutionPlan.h:156-159) | 1-2 passes | the flow engines take encoded pictures |
| flow -> readers | `nrFlow_`, `baseFlow_` (+ per layer, per double-SR pass) | R16G16F | NR size, work size | 1 pass each, **always both** (:3511) | each reader wants vectors in its own pixels |
| source -> DLSS SR / FSR | `srStageInput()` directly | FP16 | 1080p -> work | none in SDR; HDR: 1 restore pass after it (`finishTemporalOutput`, :545) | they take the graph's colour |
| source -> RTX Video SR (standard) | `videoSrInput_` -> SDK -> `videoSrOutput_` -> `workRgba_` | **RGBA8** (HDR: 8-bit PQ) | 1080p in, work out | encode pass at the input size + decode (HDR: restore) pass at the work size (`finalVideoSr`, :3347-3372) | the SDK refuses anything but R8G8B8A8 (src/ngx/VideoSrBackend.cpp:35) |
| source -> RTX Video SR (high bitrate / streaming) | the same + Video Effects' own textures | **RGBA8** | the same | encode, copy in, list submitted, CUDA run, copy out on the next list, decode (`runVfx`, :3292-3303) | another API (CUDA) |
| source -> NIS | NIS's own FP16 targets | FP16 | 1080p -> work | its encode and decode around the scaler (NisPass.cpp) | NIS scales an encoded signal |
| work -> neural layer | `layer->input()` -> `layer->proxy()` | FP16 -> **RGBA8** | NR size (1080 lines by default) | downsample pass (skipped when the layer runs at its input's size: "borrowed") + encode pass (:3826-3849) | the model takes an 8-bit picture |
| neural layer -> work | `layer->neural()` -> `finalRgba()` -> `fullTarget()` / `residualRgba_` | RGBA8 -> FP16 | NR size -> work | decode pass (:3968-3979) + residual composite at full size (`compositeNrLayer`, :3746) | only the layer's CHANGE is added to the FP16 picture, so the picture's own precision survives the 8-bit detour |
| work -> output frame | `videoFrame_[parity]` | RGBA8 / R10G10B10A2 / FP16 | work | 1 pass, dither **only while a colour grade is active** (`outputDitherStep`, :244) | frame generation, recording and presentation take the encoded frame |
| output frame -> VFG | Video Effects' textures | as the output frame | work | copy in, CUDA interpolation, copy out (VfgBackend.h) | another API |
| output frame -> swap chain | back buffer | swap chain format | window | 1 blit; a shrink takes ONE bilinear sample per pixel (PresentBlit.hlsl:87), an enlargement the finer filter in linear light (`FineSample`, :80) | |
| raw console picture (only for comparison clips and the probe) | `sourceReferences_`, `baseReferences_` | FP16 | 1080p, work | 2 full-frame copies per real frame (:3734-3740) | kept only when asked (`retainReferences`) |

**The 8-bit hand-offs** (what a debanded gradient passes through): the RTX Video SR input and output, every neural
layer's proxy (its change only), the RTX Video HDR input, and the final SDR output frame. The final write is
dithered only with a colour grade. So a gradient that the clean-up stage smoothed below one 8-bit step is quantised
again by an RTX Video SR input and by an ungraded SDR output; DLSS SR, FSR and NIS keep FP16 until the final write.
The planner says so (`deband-requantised`, a note) and stream "restore" adds a dither for the clean-up-on case.

## 3. What the AI features share

- **One optical flow per real frame**, shared by DLSS SR / FSR (`baseFlow_`), the neural layers (`nrFlow_` or a
  per-layer adaptation), DLAA nodes, DLSS / FSR / ANR frame generation (`baseFlow_` or the refined `fgBaseFlow_`).
- **Not shared: VFG, AMD FRC and PushFlow estimate motion themselves** from the two pictures
  (EnhancementSettings.h:183-187). See section 4.
- **History**: each neural layer has its own handle, textures and history (NrInstance.h); DLSS SR and FSR keep
  theirs inside the SDK; frame generation's history is the previous output frame.
- **Depth** is a constant (zero / far) texture for every consumer: decoded video has none.
- **Neural inference reuse** (docs/NR_INFERENCE_REUSE_2026-11-07.md, `NrReuse.h`): on a reused frame the whole
  layer chain is skipped and the previous inference's change is carried along the flow onto the new frame
  (:3544-3560, :4000-4009). It needs the flow and the kept previous input and output; live preview only.
- **GPU timestamps**: one `GpuTimer` record per real frame for all stages (section 6).

## 4. Work that is repeated (proved by the code)

1. **Motion is estimated twice in the owner's reference** (DLSS SR + VFG): the graph's optical flow runs because
   DLSS SR reads it (`runMotion`, :3383), and VFG estimates motion again inside Video Effects. With a spatial
   upscaler and VFG, no optical flow session exists at all (:1325-1328).
2. **Every repeated picture is processed again.** With the content rate at *Transport*, both copies of each picture
   of a 30 FPS game in the 59.94 FPS stream go through colour, clean-up, upscaler and neural layer (docs/SUPER_RESOLUTION.md
   section 4). The automatic detector that would remove them stays disabled by the owner's order.
3. **Colour encodings back and forth**: linear FP16 -> 8-bit -> FP16 around RTX Video SR and around every neural
   layer, then FP16 -> 8/10-bit for the output frame; with RTX Video SR as the last image stage the picture is
   decoded to FP16 and encoded again two passes later.
4. **A 4K intermediate that the presenter shrinks**: with the default target, upscaler output, residual composite,
   final write, HUD lock and frame generation run at 3840x2160 (8.29 Mpixel) for a 2560x1440 screen (3.69 Mpixel).
5. **A neural layer after the upscaler** downsamples the work picture to its inference size and composites its
   change back at the work size; before the upscaler it borrows the source (no downsample) and composites at 1080p.
6. **Both flow adaptations run whenever there is flow** (:3511), also the neural-size one when no neural layer and
   no reuse pass reads it (skipped with `VEYRA_EXP_PIPE_IDLE_NR_FLOW=1`, section 9).

## 5. What the neural layer's size policies really set

`pipeline::NrSizePolicy` (include/veyra/pipeline/ResolutionPlan.h:15-25, applied at :143-152): the layer's
**inference size**, as a 16:9 box of N lines fitted into its input without enlarging: `Realtime` (the default) and
`Auto` 1080, `P480`, `P720`, `P900`, `P1440`, `Native` = the input's size, `Manual` = a percentage. The input is the
work picture (after the upscaler) or the source (before it). The policy does **not** change the size the result is
composited at (always the layer's input size), nor the work size. When the inference size is below the source size
the optical flow runs at that size too (:153-159). `Auto` additionally lets a controller choose among a pool of
prepared sizes, but only under narrow conditions that include one card's device id (`createAutoNrPool`, :2196-2199:
stream "nrrun" replaces that test with capabilities in Package 8C).

Why the inference size is the main cost lever: the model runs once per pixel of it. The only per-frame figure found
is a press measurement of NVIDIA's first model, not ours (S5_NR_RESEARCH.md: about 10 ms at 1440p and 22 ms at 4K on
an RTX 5070, growing with pixels). The owner asked for 720, 900, 1080 and 1440 lines to be benchmarked; the
planner's headroom rule steps down through exactly those.

## 6. Where GPU time is recorded

`gpuTimer_.mark(list, GpuStage::X)` pairs inside `process` (stages: include/veyra/diagnostics/FrameMetrics.h:10).
Collected by `EnhanceGraph::takeGpuTimings` from `EngineController`'s `collectTimings` (EngineController.cpp:1341).

| Number | What it contains | What it leaves out |
|---|---|---|
| `Color` | ingress conversion, grading | |
| `Flow` | flow inputs, the flow engine, refinement and adaptation (with refinement on) | |
| `Sr` | DLSS SR / FSR: the evaluate only. RTX Video SR: encode + evaluate + decode (and the CUDA round trip). NIS: its three passes | **DLSS SR's HDR restore pass** (recorded after the closing mark, :3692-3693); the optical flow DLSS SR needs |
| `NrLayer0..3` | one layer's evaluate + decode | its downsample and encode (before the mark) |
| `Nr` | the LAST layer's evaluate only (every layer overwrites it) | |
| `Residual` | final composite, stabiliser | |
| `FgBatch`, `Fg1..7` | all generated frames of the pair, each one | |
| `Blit` | the presenter's blit | |
| `enhancementProcessingMs` | union of Flow, Sr, Nr + layers, Residual, FgBatch, anti-aliasing, beauty spans (EnhancementProcessingTime.h) | Color, final write, Blit, waits |

Log lines: `[player-timing]` (`gpu*P95Ms`, every second), `[slipstream-sr]` (5 s, upscaler only),
`[slipstream-clean]` (stream "restore"), and new in this sprint **`[slipstream-nr]`** (5 s, only with
`VEYRA_EXP_RP_STATS=1`; include/veyra/engine/PushNrStats.h): `nrMs` (the layers' spans added up), `srMs`, `fgMs`,
`graphMs` (first to last timestamp of the record), `copyCount` (a model of section 2 for the configuration in force,
not instrumentation), video memory, runtime slot and file version.

## 7. Differences from proven renderers (one line each)

- libplacebo debands first, on the ENCODED signal at source size, before any scaler (S5_SRC_VIDEO.md section 3): ours
  converts to linear FP16 first; the clean-up stage therefore works on re-encoded values (stream "restore" does that).
- libplacebo upscales in encoded light (a sigmoid for SDR, the PQ signal for HDR): our presenter's `FineSample`
  filters in linear light, which rings more at high-contrast edges.
- libplacebo dithers last, at the final write, always for 8- and 10-bit targets: ours only with a colour grade.
- moonlight-qt presents the decoder's texture from one D3D11 renderer with no intermediate picture and no
  frame-latency limit (S5_SRC_STREAM.md, M5): ours always goes source -> FP16 -> output frame -> swap chain, even
  with every effect off.

## 8. Phase 4: why was DLSS SR (Model M) much cheaper than "one of the RTX Video SR configurations"?

Which RTX Video SR configuration the owner ran (family, level, target) is not recorded in any evidence. The two paths
were read side by side (`runSr`, :3640-3716; `finalVideoSr` / `runVfx`, :3292-3372; src/ngx/DlssSrBackend.cpp,
VideoSrBackend.cpp, src/pipeline/VfxSrBackend.cpp).

**Proved by the code**

| | DLSS SR | RTX Video SR standard (NGX) | RTX Video SR high bitrate / streaming (Video Effects) |
|---|---|---|---|
| Input | the graph's FP16 picture, no conversion | 8-bit encode pass at 1080p | the same encode + a copy into CUDA's texture |
| Output | written into the work texture (HDR: a restore pass) | 8-bit, then a decode / HDR restore pass **at the work size** | copy out on a second command list, then the decode pass |
| API hops | none (NGX on the graph's queue) | none | D3D12 -> CUDA -> D3D12, the graph's list is split in two |
| Target size asked for | the SR target (4K by default) | the same | the same |
| Sharpening asked for | 0 (`EvalDesc ed{}`) | none to ask: part of the model | part of the model |
| Artifact reduction | none | part of the model (trained on compressed video) | streaming: strongest; high bitrate: none (EnhancementSettings.h:100-110) |
| Quality level | model K / M / L, set once | level 1-4 per evaluate, default 2 | tier per family |
| Motion input | optical flow (a whole extra stage, timed as `Flow`) | none | none |
| Inside `gpuSrP95Ms` | the evaluate only | encode + evaluate + decode | encode + copies + CUDA + decode |

So the two `SR` numbers do not measure the same thing: RTX Video SR's contains two or four extra full-picture
passes, one of them at the work size, and DLSS SR's leaves out its flow stage and its HDR pass. Neither path hides a
second scaler or a 4K intermediate the other lacks: both go straight from 1080p to the target.

**Hypotheses that need the hardware** (S5_SRC_VIDEO.md section 5, tested against the code above)

| # | Hypothesis | What the code says | What decides it |
|---|---|---|---|
| H1 | Family and level: the top levels and the Video Effects families simply cost more | consistent: the level reaches the SDK per evaluate; upstream's own probe (docs/RTX_VIDEO_SR_FAMILIES_2026-11-05.md, their PC, whole export time per frame) had ultra at 8.6 / 12.2 / 12.4 ms against 7.1-7.3 at low | the `sr` step's holds `rtx1..rtx4`, and arms with `sr=vsr-stream` / `sr=vsr-hb`: `[slipstream-sr] gpuP95` per hold |
| H2 | Interop and conversions | proved to exist (table); their size is unknown | the same holds: `[slipstream-nr] srMsP95` against `graphMsP95`, and DLSS SR's `gpuFlowP95Ms` added to its `srMs` |
| H3 | Hidden artifact reduction in the standard levels is the cost | not supported: the family without it (high bitrate) is the dearest at ultra in that probe | high bitrate against standard at one level and size |
| H4 | Output size: 4K is 2.25 times 1440p | proved to apply to both paths equally; decides only if the two were run at different targets | `rtxN` against `rtxN-window`; `v-reference` (4K) against `v-dlss` (1440p) |
| H5 | The SDK's scale design: 1.33x may run a larger factor inside and shrink | nothing in our code can show it | RTX Video SR timed at 1.33x, 1.5x and 2x from one input |

The planner encodes what is proved: a target above the display is dropped unless supersampling is asked for
(`above-display`), a cleaning family beside our clean-up gives way (`double-clean`; note that NVIDIA documents its
Video Effects artifact reduction as tuned for H.264, while this stream is H.265: how much it cleans here is itself
unmeasured), a sharpening family takes no second sharpening stage (`double-sharpen`), and with measured costs the
most expensive image stage is reduced first (`gpu-budget`). It does not choose between DLSS SR and RTX Video SR.

## 9. Avoidable copies and redundant work, ranked (expected savings are reasoned, not measured)

| # | What | Where | Expected saving, and why | Risk | Status |
|---|---|---|---|---|---|
| 1 | The 4K work size on a 1440p screen | `EnhancementSettings::srTarget` (default `Uhd4K`), `compileChainExecutionPlan` | the largest: every stage from the upscaler's output to the presenter touches 2.25x fewer pixels, and the presenter's shrink disappears | the 4K picture may look better after the shrink; the kit must show | **planner rule** (`above-display`); no default changed |
| 2 | Processing every repeated picture of a 30 FPS game | `EngineController.cpp` content rate, `ContentDedup` | up to half of all image-stage work | the detector that would do it dropped genuine pictures three times | **not implemented**: disabled by the owner's order |
| 3 | Motion estimated twice with DLSS SR + VFG | `runMotion` (:3383), `EnhancementSettings::srMotion` | the whole `Flow` stage, when DLSS SR is its only reader (set its motion source to zero) | DLSS SR without motion may be less stable on moving video | **recommendation**: an existing setting; one kit arm |
| 4 | Neural layer after the upscaler | node order (`nrBeforeSr`) | its downsample pass, and the composite at 1080p instead of the work size | the upscaler then enlarges the layer's changes; picture order is a matter for the arms | **planner input** (`nrPlace=before`), nothing changed |
| 5 | Decode to FP16 and encode again when RTX Video SR is the last image stage | `finalVideoSr` (:3347) + final write (:4090) | two passes at the work size | touches every reader of `workRgba_` | recommendation only |
| 6 | Encode + downsample, and decode + composite, as one pass each per neural layer | :3826-3849, :3968-3979, `compositeNrLayer` | two passes at the inference size per layer | new shaders; the parity encode is a contract of the SDK path | recommendation only |
| 7 | The flow adaptation nobody reads | `process`, the `adapt(nrFlow_...)` call after the flow | one compute pass at the neural layer's size (1920x1080 by default, also with no layer) per real frame with flow: with DLSS SR and no neural layer (the owner's reference) it is written and never read. Small: one R16G16F write of about 2 Mpixel | low: the readers of `nrFlow_` are only the neural layers (their motion input, created in `initNgxFeatures`) and the inference-reuse pass (`createNrReusePass`); `baseFlow_` and the per-layer, double-SR and DLAA fields, which have other readers, are untouched. A layer or the reuse pass needs a graph rebuild, which adapts again | **implemented** behind `VEYRA_EXP_PIPE_IDLE_NR_FLOW=1` (off by default; `engine::pipeIdleNrFlowSkippable`, unit-tested; no WARP test: the graph's flow path does not run on the software renderer here) |

Implemented in the graph this sprint: **one**, item 7, behind `VEYRA_EXP_PIPE_IDLE_NR_FLOW=1` (off by default; with
it unset the graph records exactly what it did). Its reader audit is complete in the code (every use of `nrFlow_`
was read on 21 Nov), so the skipped pass cannot change a picture; the saving is small and unmeasured. The brief
allowed at most the two safest behind a switch. Every other graph-side item needs either a new shader (no shader
compiler here) or an audit of indirect readers that only a GPU run can confirm, and the largest savings (1, 3, 4)
are reachable through existing settings, which the planner and the kit's arms use. A wrong picture is worse than a
pass saved.

## 10. A vendor-neutral path (reasoned; nothing was run on AMD or Intel cards)

Today the vendor-neutral 1080p -> 1440p choices are the presenter's bilinear / cubic filter, NIS and the app's FSR
upscaler; FSR is what the interface seeds on AMD and Intel adapters. (S5_NR_RESEARCH.md names the spatial FSR 1 as
the vendor-neutral method for video; the app's FSR choice is the temporal FidelityFX upscaler fed with optical flow
and a constant depth, which is what exists today.) XeSS upscaling needs engine motion vectors and is not usable on
video. The planner's fallback order when an NVIDIA runtime is missing: DLSS SR -> FSR, then NIS;
RTX Video SR -> NIS, then FSR; frame generation -> FSR, then PushFlow; the neural layer has no stand-in and is
dropped; clean-up is vendor neutral and stays. Recommendation, not implemented: a Lanczos-3 scaler with anti-ringing
after libplacebo's published filter (S5_SRC_VIDEO.md section 2.3, rated there at 2-3 hours) as a faithful
1080p -> 1440p step that neither sharpens nor invents detail.

## 11. What the next hardware run must show

- `[slipstream-nr]` lines per arm: `graphMsP95`, `srMsP95`, `nrMsP95` at 720 / 900 / 1080 / 1440 lines, `fgMsP95`,
  and that `runtime=` / `version=` name the signed original.
- Section 8's table filled: H1, H2, H4 from the `sr` step and the arms; DLSS SR's `Flow` time beside its `Sr` time.
- `[slipstream-quality]` output lines of `v-clean-dlss` against `v-dlss` and `v-original` (does clean-up lower
  blockiness and banding without losing detail), and of `v-reference` against `v-dlss` (does the 4K target earn its cost).
- Whether the neural layer runs on the HDR stream at all, and what it does to `flicker` and `hudDelta`.
