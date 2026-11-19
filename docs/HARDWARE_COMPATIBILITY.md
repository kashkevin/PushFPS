# Hardware compatibility (Slipstream build)

Status on 19 November 2026. One universal build is meant for modern Windows 11 gaming PCs: NVIDIA
GeForce RTX 30/40/50 first, AMD Radeon RX 6000/7000/9000, Intel Arc A/B, Ryzen 5000+ or Intel 12th
gen+ CPUs, 60-360 Hz displays. **No PC has been tested physically yet.** Nothing below is a
performance claim.

What decides support is what the PC reports, not its model name: the capability model in
`include/veyra/engine/PushHardwareCapabilities.h` (tests in `tests/remoteplay/GpuBudgetTests.inc`)
turns the reported capabilities into safe defaults, and `pushAdapterCapabilities()` in
`src/ui/bridge/Hardware.cpp` collects them (log tag `[push-gpu-caps]`). Unknown is never treated as
present: with nothing known the result is H.264, SDR and frame generation off, which still lets PS5
Remote Play run.

## Matrix

Columns: **Physically tested** on a real PC; **CI software adapter**: compiled by MSVC and run on
Windows' WARP software renderer in `build-windows.yml` (NIS, comparison clips, HUD sampling, PushFlow
smoke run at 640x360) or the portable tests in `remoteplay-checks.yml`; **Expected by capability**:
what should work and the capability that decides it; **Fallback**: what happens when that capability
is missing.

| Family | Physically tested | CI software adapter | Expected by capability (deciding capability) | Unsupported / fallback behaviour |
|---|---|---|---|---|
| NVIDIA RTX 50 (Blackwell) | none yet | yes (WARP, not the GPU) | Hardware H.264/HEVC/HEVC 10-bit decode (decoder profiles of the D3D video device); PushFlow (D3D12 FL 12_0 + PushFlow runtime installed); DLSS FG, RTX Video SR, NR, VFG, TrueHDR when the NVIDIA runtime files are installed (`fgMethods` / effect capability bits); NVIDIA optical flow only in builds with the NVOF headers (`flow0`) | Missing runtime file: that effect is reported unavailable with its reason, the saved choice moves to the next method (FSR 4, FSR 3.1, XeSS, DLSS) or generation off |
| NVIDIA RTX 40 (Ada) | none yet | yes (WARP) | as RTX 50, except the Blackwell-only NR runtime (`nr3`) | as above |
| NVIDIA RTX 30 (Ampere) | none yet | yes (WARP) | as RTX 40; VFG is marked experimental on RTX 30 | as above |
| AMD RX 9000 (RDNA 4) | none yet | yes (WARP) | hardware decode by profile; PushFlow; FSR 3.1 FG; FSR 4 ML FG when the driver's provider offers it (`fsr4`); AMD FRC when the driver's AMF runtime is installed (`amdfrc`); AMD FidelityFX optical flow | FSR 4 refused: FSR 3.1 at 2X with a notice; no NVIDIA files are installed on AMD by default (rule 5) |
| AMD RX 7000 (RDNA 3) | none yet | yes (WARP) | as RX 9000 without FSR 4 ML; RDNA 3 AI enhancement only with HIP 7 runtime and the opt-in extras | as above |
| AMD RX 6000 (RDNA 2) | none yet | yes (WARP) | hardware decode by profile, PushFlow, FSR 3.1, AMD FRC, XeSS | no AI enhancement runtime: reported unavailable |
| Intel Arc A / B | none yet | yes (WARP) | hardware decode by profile (incl. HEVC 10-bit where listed), PushFlow (DirectML), FSR 3.1, XeSS FG when its files are present | NVIDIA/AMD-only methods reported unavailable; next compatible method or off |
| Integrated GPU (UMA) / hybrid laptop | none yet | n/a | the capability model caps PushFlow at Fast (UMA memory says little) and flags `hybrid-gpu`; battery power caps the tier at Fast | the display may be driven by the other adapter: a cross-adapter copy per frame (not measured) |
| CPU: Ryzen 5000+, Intel 12th gen+ | none yet | n/a | no CPU-specific code; software decode is the fallback when no hardware decoder opens (`DecodeMode::Automatic`) | software HEVC at 4K is heavy: the model requests H.264 when hardware HEVC decode is missing or unknown |
| Displays 60-360 Hz | none yet | n/a | output targets are whole multipliers whose output fits the refresh (`pushValidOutputTargets`, 2 % tolerance); VRR state as the bridge reports it (0-4) | a source that fills the panel (60 fps on 60 Hz): no generation; targets that do not divide the refresh (120 on 180 Hz) are flagged `evenWithoutVrr=false` |
| Older GPUs (below D3D12 FL 12_0), Windows 10 | not a target | n/a | not supported by design | the engine refuses adapters without FL 12_0 (`engineAdapterDesc`); the model turns generation off (`fg-feature-level`) |

## NVIDIA notes

- **Needs NVIDIA components:** DLSS frame generation (`nvngx_dlssg.dll`), DLSS SR (`nvngx_dlss.dll`),
  RTX Video SR (`nvngx_vsr.dll`, Video Effects), AI enhancement / NR (`nvngx_dlssnr.dll`), VFG
  (NVIDIA Video Effects runtime), TrueHDR (`nvngx_truehdr.dll`), NVIDIA low latency and the G-SYNC
  profile check (driver). PushFlow, FSR 3.1, XeSS, NIS and the GPU-DIS optical flow do not.
- **Installer** (`packaging/Install-PushFPS.ps1`, project rule 5): NVIDIA files are installed only as
  NVIDIA-signed originals; a modified NVIDIA file and NVIDIA's model runtimes on AMD installs are
  "experimental extras", off by default (`-ExperimentalExtras Yes` or the installer's question).
  Left out, the feature is reported unavailable, never broken.
- **NVIDIA Optical Flow:** compiled in only when the repository secret `NVOF_SDK_HEADERS` is set; a
  build without it uses `src/ngx/NvOfSessionUnavailable.cpp`, reports `flow0` unavailable and every
  chain uses AMD FidelityFX optical flow. A fork build therefore differs from the release on RTX cards.
- No NVIDIA model names in new code: the capability model reads availability bits. Existing Veyra/PushFPS
  code still matches names: `vfgHardwarePossible` (`Hardware.cpp:54`), `effectGpu` (`Hardware.cpp:245-249`),
  and the RTX 5070 device id 0x2f04 for automatic NR (`Hardware.cpp:269`, `EngineController.cpp:848`).
  Left unchanged (behaviour); replacing them needs the NVIDIA runtime's own capability query.

## GPU workload budget

`include/veyra/engine/PushGpuBudget.h` keeps headroom for decoding and presentation: from the user's
request it steps down quality tier, then multiplier, then frame generation off, one level at a time
(2 s of trouble and 3 s between steps; severe overload after 0.5 s), and steps back up only after
20 s healthy (doubling after a failed return), never above the request, never on unknown input or on
battery. Its header lists what the existing pieces (FgRecoveryBudget, FgCostSettling,
VfgOverloadEstimate, admitLiveFg, LiveGpuScheduler, PushAdaptiveFg, pushFlowBudgetWatch,
PushAutomaticMode) already do. Thresholds are estimates; nothing measured them.

### Engine hooks (not added: files owned elsewhere)

1. `EngineController.cpp`, once a second on the engine thread, behind a new switch
   `VEYRA_EXP_RP_GPU_BUDGET=1`: fill `PushGpuBudgetSample` from `fgBudget.baseCost()` (gpuFrameMs),
   the FG P95 (fgMs), the decoder mailbox wait (decodeQueueDelayMs), presented/expired outputs
   (`generatedExpiredAfterEval`, late real frames), the admission's rejected-pair share, and apply
   the decision's tier/multiplier through the existing settings path (as `fgAdaptive` does: a ceiling
   for `pushAdaptiveFgMultiplier`, no graph rebuild for the multiplier; a tier change rebuilds).
2. `PushFps.cpp` `pushFlowBudgetWatch`: skip its tier step while the budget switch is on (both would
   lower the tier).
3. `PushSlipstream.cpp` `pushHardwareSummary`: merge `pushAdapterCapabilities(displayRefreshHz, vrrState)`
   so the first-launch card and the log show it; the decode-mode/codec choice in the Remote Play
   connect path may use `defaults.codec`.
4. A thermal/power source for the sample (power: `GetSystemPowerStatus`, as in Hardware.cpp; GPU
   thermal throttling has no vendor-neutral Windows API, so it stays Unknown unless a vendor query exists).

## Shader-compilation, first-use and allocation findings (read only, not changed)

| # | Where | Finding | Proposed fix |
|---|---|---|---|
| 1 | `EngineController.cpp:1843-1891`, `EnhanceGraph.cpp:910-937` | A settings change that needs a rebuild drains the queue, stops presentation (`prepareGraphChange`), destroys the graph and initialises a new one synchronously on the engine thread: nothing is shown during `createMs` (logged by `reset-lifecycle`). The same on a PS5 resolution change (`EngineController.cpp:2219-2223`) and the VRAM recovery (`:2234-2236`). | Build the new graph beside the old one (where VRAM allows: `ctx.videoMemoryInfo`) and swap at a frame boundary; keep presenting the last output meanwhile. |
| 2 | `GpuPassUtils.h:131-203` (`ComputePass::create`), called ~25 times from `EnhanceGraph::createComputePasses` (`EnhanceGraph.cpp:2277`) | Every rebuild re-serialises root signatures and creates every compute PSO again; no `ID3D12PipelineLibrary` or process-wide PSO cache. The driver's disk cache usually hides this, but the first run after a driver update compiles everything at the first connect. | A PSO cache keyed by shader bytecode hash + root signature, owned by `PreviewGpuSession` (lives across rebuilds); optionally an `ID3D12PipelineLibrary1` stored in the data folder and discarded on driver/adapter change. |
| 3 | `GpuPassUtils.h:109`, `:226` (`loadShader`) | Each pass reads its `.dxil` from disk on every rebuild. | Load once per process into the same cache. |
| 4 | `GpuPassUtils.h:202`, `:211` | Each pass owns a shader-visible descriptor heap and `bind` calls `SetDescriptorHeaps` per dispatch; heap switches can serialise work on some GPUs. | One shared shader-visible heap with per-pass ranges, set once per command list. |
| 5 | `EnhanceGraph.cpp:943-1100` (`createResources`, ~59 `CreateCommittedResource`/texture helpers) | All textures are committed resources created at every rebuild; a mode change that keeps sizes re-allocates them anyway (only the NGX core host and the effects-off graph are cached: `RecentGraphCache`, `coreCache_`). | Reuse textures whose size/format did not change across rebuilds, or place them in one heap kept by the session. |
| 6 | first frame after `initialize` | PSOs and resources are created before the first frame, but the first dispatch of each pass is the first real use (some drivers finish compilation then). PushFlow already runs two warm-up inferences (`PushFlowBackend.cpp:760-777`) and FgCostSettling treats the first evaluations as warm-up. | One throwaway dispatch of each enabled pass on a small target at the end of `initialize`, before the first real frame is admitted. |
| 7 | `EnhanceGraph.cpp:943-953` | `fgDisable` readback buffers per pool slot: a GPU-to-CPU read on the generation path (ring-buffered, so not a stall by itself). | Confirm with PIX that the read never waits; otherwise read one frame late. |

All of the above is unmeasured. A fix needs PresentMon/PIX captures before and after (see
`.claude/ENGINEERING_STANDARDS.md`).

## What a tester should report

1. The `[push-gpu-caps]` and `[push-hardware]` log lines (vendor, VRAM, feature level, decode
   profiles, frame-generation methods, refresh, VRR, HDR, hybrid, power) and the GPU name, driver
   version, Windows build.
2. Which decoder opened (`D3D12VA decoder opened codec=...` or software) and the stream's codec and
   resolution.
3. Frame generation method, multiplier and tier used, and whether any notice changed them.
4. `reset-lifecycle` lines after a settings change (how long the picture stopped).
5. A PresentMon capture of 60 s (`--track_gpu_video`): present mode, `MsBetweenDisplayChange`
   median/P95/P99, dropped frames, GPU busy and video decode busy.
6. Anything unavailable that should not be (with the reason shown) and any crash with its log.
