# Hardware compatibility (Slipstream build)

Status on 19 November 2026. One universal build is meant for modern Windows 11 gaming PCs: NVIDIA
GeForce RTX 30/40/50 first, AMD Radeon RX 6000/7000/9000, Intel Arc A/B, Ryzen 5000+ or Intel 12th
gen+ CPUs, 60-360 Hz displays. **No PC has been tested physically yet.** Nothing below is a
performance claim.

What decides support is what the PC reports, not its model name: the capability model in
`include/veyra/engine/PushHardwareCapabilities.h` (tests in `tests/remoteplay/GpuBudgetTests.inc`; simulated profiles in `PushHardwareProfiles.h`, tests in `tests/remoteplay/HardwareProfileTests.inc`)
turns the reported capabilities into safe defaults, and `pushAdapterCapabilities()` in
`src/ui/bridge/Hardware.cpp` collects them (log tag `[push-gpu-caps]`). Unknown is never treated as
present: with nothing known the result is H.264, SDR and frame generation off, which still lets PS5
Remote Play run.

## Matrix

Columns: **Physically tested**: on a real PC of that family (none yet, for any family). **Compiled + software
renderer**: compiled by MSVC on GitHub's Windows runner and run on Windows' WARP software renderer
(`build-windows.yml`: NIS, comparison clips, HUD sampling, PushFlow smoke run at 640x360) or the portable
tests in `remoteplay-checks.yml`; WARP is not that GPU and says nothing about its driver. **Simulated
profile**: the profile ids in `include/veyra/engine/PushHardwareProfiles.h` whose capability report is run
through the capability model and the start-up model in `tests/remoteplay/HardwareProfileTests.inc` (Linux and
MSVC; a model of the decision code, not of the hardware). **Expected by capability**: what should work and the
capability that decides it. **Fallback**: what happens when that capability is missing.

| Family | Physically tested | Compiled + software renderer | Simulated profile | Expected by capability (deciding capability) | Fallback |
|---|---|---|---|---|---|
| NVIDIA RTX 50 | none | vendor-neutral code only (WARP) | `nvidia-rtx50-12gb-180hz-hdr`, `nvidia-rtx50-16gb-360hz`, `nvidia-no-runtime-files`, `contradictory-vendor-known-level-unknown` | hardware H.264/HEVC/HEVC 10-bit decode (decoder profiles); PushFlow (FL 12_0 + runtime); DLSS FG, VFG, RTX Video SR, NR, TrueHDR when their files are installed (`fgMethods`, effect bits); NVIDIA optical flow only in builds with the NVOF headers (`flow0`); NVENC recording incl. HDR | missing file: that effect reported unavailable with its reason, saved choice moves to FSR 4 / FSR 3.1 / XeSS / DLSS or generation off; NVENC refused: recording fails with its message (no Media Foundation fallback in the live recorder, audit #15) |
| NVIDIA RTX 40 | none | vendor-neutral code only (WARP) | `nvidia-rtx40-8gb-144hz`, `nvidia-rtx40-fork-build-no-nvof`, `decode-profiles-unknown`, `hybrid-rtx40-laptop-battery` | as RTX 50 except the Blackwell-only NR runtime (`nr3`) | as above; fork build without NVOF: AMD FidelityFX optical flow |
| NVIDIA RTX 30 | none | vendor-neutral code only (WARP) | `nvidia-rtx30-10gb-60hz-sdr`, `nvidia-4gb-vram` | as RTX 40; VFG experimental | as above; 60 Hz panel with a 60 fps stream: no generation (`fg-source-fills-display`) |
| AMD RX 9000 | none | vendor-neutral code only (WARP) | `amd-rx9000-16gb-240hz` | decode by profile; PushFlow; FSR 3.1; FSR 4 ML when offered (`fsr4`, today gated by the name "RX 90", audit #3); AMD FRC with the driver's AMF runtime; FidelityFX optical flow; Media Foundation hardware encoder (SDR only) | FSR 4 refused: FSR 3.1 2X; FSR 3.1 refused: generation off, picture still shown; HDR recording refused with its message |
| AMD RX 7000 | none | vendor-neutral code only (WARP) | `amd-rx7000-20gb-144hz-hdr`, `hybrid-rx7000s-laptop-mains` | as RX 9000 without FSR 4 ML; AI enhancement only with HIP 7 and the opt-in extras | as above |
| AMD RX 6000 | none | vendor-neutral code only (WARP) | `amd-rx6000-8gb-165hz`, `amd-rx6000-no-amf-runtime`, `no-hevc-decode`, `no-hardware-encoder` | decode by profile, PushFlow, FSR 3.1, XeSS, AMD FRC | no AI enhancement runtime: reported unavailable |
| Intel Arc A / B | none | vendor-neutral code only (WARP) | `intel-arc-b-12gb-180hz`, `intel-arc-a-8gb-120hz`, `hevc-8-bit-only-hdr-display` | decode by profile, PushFlow (DirectML), FSR 3.1, XeSS when its files are present; Media Foundation encoder | NVIDIA/AMD-only methods unavailable; next compatible method or off |
| Integrated GPU / hybrid laptop | none | n/a | `intel-integrated-uma`, `hybrid-rtx40-laptop-battery`, `hybrid-rx7000s-laptop-mains` | the engine takes the high-performance adapter; the model caps PushFlow at Fast for UMA and on battery and flags `hybrid-gpu` | the display may be driven by the other adapter: a cross-adapter copy per frame (not measured) |
| CPU: Ryzen 5000+, Intel 12th gen+ | none | n/a | (decode paths in every profile) | no CPU-specific code; software decode is the fallback (`DecodeMode::Automatic`) | software decode runs one thread (audit #13): the model asks for H.264 when hardware HEVC decode is missing or unknown |
| Displays 60-360 Hz | none | n/a | 60 / 75 / 120 / 144 / 165 / 180 / 240 / 360 Hz across the profiles; refresh unknown in `contradictory-uma-16gb-fsr4-bit-on-nvidia` | whole multipliers whose output fits the refresh (`pushValidOutputTargets`, 2 % tolerance) | source fills the panel: no generation; uneven targets flagged; VRR state is read through NVAPI only (audit #19): unknown on AMD / Intel |
| Odd reports | n/a | n/a | `software-adapter-only`, `unknown-vendor-12_0`, `driver-reports-nothing`, `non-rtx-nvidia-name`, contradictory profiles | unknown is never present: H.264, SDR, generation off | software adapter only: the engine refuses (no FL 12_0 hardware adapter), the UI starts and says so in the log |
| Older GPUs (below FL 12_0), Windows 10 | not a target | n/a | `software-adapter-only` stands for "no usable adapter" | not supported by design | the engine refuses adapters without FL 12_0 (`engineAdapterDesc`, `D3D12DeviceContext.cpp:222`) |

### Simulated profiles: what the tests assert

For each of the 26 profiles and 576 requests each (every method, 2X/4X/8X, every tier, HDR on/off, HEVC/H.264,
30/60 fps): the answer satisfies `pushDefaultsValid` (HEVC only with a hardware HEVC decoder, HDR only with the
10-bit decoder and an HDR display, a method only with its own availability bit, a selectable multiplier whose
output fits the refresh, a tier the method has, optical flow that exists, no reason lost); a method the user did
not ask for is never an experimental one; with every proprietary bit removed there is still a valid answer;
the first-launch method is kept only when available; with any set of failing frame-generation backends the
start-up model ends on a working method or on plain presentation within the engine's 6 attempts, and the
Automatic decoder always ends on a decoder. Availability bits come from the real `ui::hardwareSupports` gates;
the fallback order and the first-launch method are held to the engine's by static_asserts in `Hardware.cpp` (checked by clang and the mingw syntax check; unverified under MSVC until the next Windows build).
The init-failure, decoder and encoder rules are copies of engine code (named in the header), not checked by
the compiler.

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
  code still matches names: `vfgHardwarePossible` (`Hardware.cpp:51`), `effectGpu` (`Hardware.cpp:272-285`),
  and the RTX 5070 device id 0x2f04 for automatic NR (`Hardware.cpp:303`, `EngineController.cpp:848`); see the audit below.
  Left unchanged (behaviour); replacing them needs the NVIDIA runtime's own capability query.

## Vendor-assumption audit (19 November 2026, read-only outside the capability files)

Classes: **BLOCKS START** (the picture never shows), **BLOCKS REMOTE PLAY** (the PS5 stream cannot run),
**LOSES FEATURE** (with a clean fallback and, where marked, a message), **COSMETIC**. Line numbers are those of
branch `exp/s3-hw`. "Model test" = covered by `HardwareProfileTests.inc` (a model, not the code itself).
By reading the code, nothing below stops an RX 6000+ / Arc / RTX 30+ PC from starting or from running PS5 Remote Play with
Automatic decoding; the one PC that cannot start is one without a D3D12 FL 12_0 hardware adapter, which is out
of scope.

| # | Where | What happens on AMD / Intel / hybrid / no HEVC decode / no FL 12_0 | Fallback and where tested | Class | Minimal fix (proposal unless marked done) |
|---|---|---|---|---|---|
| 1 | `D3D12DeviceContext.cpp:210-222`, `Hardware.cpp:30` `engineAdapterDesc` | needs a hardware adapter that creates a device at FL 12_0, high-performance order (hybrid: the discrete GPU); none: "no compatible requested D3D12 hardware adapter found", no playback; all target GPUs are 12_1+ | none (by design); model test `software-adapter-only` | BLOCKS START (out of target only) | none |
| 2 | `QmlPlayerBridge_p.h:743-756` first-launch defaults by `VendorId` | AMD: lmxxf NR runtime; AMD/Intel: FidelityFX flow, FSR SR backend; FG method DLSS / FSR / FSR 4 ("RX 90" in the name). Generation, NR and SR start off (`multiplier=1`, `nr=sr=false`). Unknown vendor: DLSS selected (not enabled). Logs the adapter description (`nr-default`) | `disableUnavailableEffects` on load (`Hardware.cpp` sanitizer); static_assert `pushFirstLaunchAgrees`; model test `first_launch` | COSMETIC | drop the description from the `nr-default` log line (privacy rule): log the vendor class |
| 3 | `Hardware.cpp:272-285` `effectGpu`, `EffectAvailability.h:16-27` | gates by vendor id AND name: "RTX", "RTX 50/40", "GeForce RTX 30", "RX 90", RX 7000 by `amdNrProvider`. A target card whose name does not match (an RDNA 4 workstation card, an RTX A-series Ampere) loses FSR 4 / VFG / NR; `CrossVendor` needs vendor NVIDIA/AMD/Intel, so another vendor gets no PushFlow / FSR / XeSS / NIS | features reported unavailable with reasons; model tests `gates_follow_the_bridge_rules`, `unknown-vendor-12_0` | LOSES FEATURE | replace name tests with the runtimes' own capability queries (FSR 4: the provider's version query; VFG/NR: NGX capability parameters); `CrossVendor`: any hardware adapter at FL 12_0 |
| 4 | `Hardware.cpp:51` `vfgHardwarePossible`, `VfgBackend.cpp:444` | VFG refused on non-NVIDIA | FG off with notice (`EngineController.cpp:~940`) | LOSES FEATURE | none needed |
| 5 | `Hardware.cpp:303` (`nrAuto`), `EngineController.cpp:848`, `EnhanceGraph.cpp:2140`, `PreviewGpuSession.cpp:38` | automatic NR pool only on device id 0x2f04 | fixed 1080p NR | LOSES FEATURE (one card only) | gate on measured VRAM headroom instead of the device id |
| 6 | `FrameGeneration.cpp:189`, `VideoPresenter.cpp:631` | NVIDIA low latency only on NVIDIA; elsewhere the plain one-frame queue (`presentation.enabled`) | "Lowest latency" uses the low queue | LOSES FEATURE | none |
| 7 | `BackendRecovery.h:18-34` via `EngineController.cpp:815` | non-NVIDIA: NR off (AMD keeps lmxxf), RTX HDR off, SR only FSR/NIS, DLSS FG off; a warning is shown | yes | LOSES FEATURE | none |
| 8 | `EnhanceGraph.cpp:1714-1790` NGX core | only created when an NGX stage is enabled; DLSS never reaches it on non-NVIDIA (#7); a failure is `FailedBackend::NgxCore` | NGX stages off, picture kept | LOSES FEATURE | none |
| 9 | `EnhanceGraph.cpp:777`, `:4435` | NVIDIA optical flow is replaced by FidelityFX only for vendor 0x1002; Intel relies on the UI sanitizer (`flow0` gated NVIDIA). A configuration reaching the engine unsanitized on Intel: NVOF fails, `FailedBackend::OpticalFlow` turns NR and generation off | sanitizer; model test `proprietary_features_are_optional` | LOSES FEATURE | done: both lines now test `!context_.adapter().isNvidia` (`isNvidia` = vendor 0x10DE). NVIDIA: unchanged (never replaced). AMD: unchanged (replaced as before). Intel and unknown vendors: NVIDIA flow is replaced by FidelityFX instead of failing NVOF |
| 10 | `EngineController.cpp:930`, `BackendRecovery.h:72-75` frame-generation init failure | FSR 4 -> FSR 3.1 2X; any other method -> generation off (PushFlow failing on Arc does not move to FSR 3.1) | yes; model test `failed_backends_land_on_the_next_choice` | LOSES FEATURE | optional: after PushFlow fails, try `restoredFrameGenerationBackend` with PushFlow marked unavailable |
| 11 | `QmlPlayerBridge.cpp:426-440` PushFlow crash guard | after a crash inside PushFlow the next start picks the first available of vfg, dlss, fsr4, fsr3, xess, anr, amdfrc: experimental providers first (VFG, ANR, AMD FRC), unlike the sanitizer | generation off when none; notice shown | LOSES FEATURE (after one crash) | experiment added: `VEYRA_EXP_PUSHFLOW_CRASH_ORDER=1` uses `engine::kPushCrashGuardOrder` (non-experimental first, the model's order) |
| 12 | `Hardware.cpp:335` (`fsr4`) | the FSR 4 provider crashed PushFPS on non-AMD (field report 2026-10-01): gated to AMD + "RX 90" | FSR 3.1 | LOSES FEATURE on unmatched RDNA 4 names | see #3 |
| 13 | `RemotePlaySource.cpp:160-238`, `:436-446`; default codec H.265 `RemotePlay.cpp:421` | D3D12VA on the engine's device; Automatic: open failure or first decode failure -> software at the next keyframe; decode mode Hardware on a PC without the profile fails the stream ("choose automatic or software"). Software decode uses `thread_count = 1` (`:194`): HEVC 1080p60 on one thread, unmeasured | Automatic: yes; model test `decode_fallback` | BLOCKS REMOTE PLAY only when the user forces Hardware decode; otherwise LOSES FEATURE (latency risk) | behind `VEYRA_EXP_RP_HW_DEFAULTS`: in `ps5Connect` request H.264 when `pushDecodeCapabilities()` is known and lists no HEVC (the capability model already says `hevc-decode-missing`) |
| 14 | `RemotePlaySource.cpp:138`, `:296` H.265 HDR | no 10-bit hardware decoder: software 10-bit decode; an 8-bit fallback stream is tagged SDR | yes | LOSES FEATURE | the model already asks for SDR (`hdr-needs-hevc10-decode`) |
| 15 | `LiveRecorder.cpp:1549-1551`, `:852`, `:856`; `MfVideoEncoder.cpp:285` | recording/streaming: NVENC iff vendor NVIDIA; HDR recording refused elsewhere; NVENC refused -> recording fails (no Media Foundation fallback, unlike export); Media Foundation needs a hardware MFT (no software encoder) | message shown, playback unaffected; model test `recording_encoder` | LOSES FEATURE | in `encodeNvenc`, when the standard settings are refused and the frame is SDR, run `encodeMediaFoundation()` instead of `fail` |
| 16 | `VideoEncoderFactory.cpp:22`, `:32` export | HDR export NVENC only; NVENC -> MF fallback exists | yes | LOSES FEATURE | none |
| 17 | `VideoPresenter.cpp:110` | HDR + FG compatibility copy only on NVIDIA | direct presentation | LOSES FEATURE (unmeasured difference) | none until measured |
| 18 | `Preferences.cpp:195`, `PreviewGpuSession.cpp:74` | enhancement prewarm only on NVIDIA | no prewarm | COSMETIC | none |
| 19 | `NvidiaVrr.h:53-90`, `DisplayStateService.cpp:22`, `PushQuickSetup.qml:36-85` | VRR state comes from NVAPI only: `vrrState` = 0 on AMD/Intel FreeSync screens (and possibly NVIDIA laptops whose screen hangs on the iGPU); "Lowest latency" then does not request VRR and says the screen cannot do it; `unevenTarget` hint shown wrongly | the smooth-pacing default still sets `variableRefresh` (`QmlPlayerBridge.cpp:391`) | LOSES FEATURE + wrong message | not fixed: `VEYRA_EXP_VRR_DXGI_FALLBACK=1` only logs DXGI tearing support (`[display-vrr] ... dxgiTearing=`) when NVAPI is not ready; the level is unchanged (tearing support says nothing about the screen). A diagnostic to collect from AMD / Intel testers |
| 20 | `NvidiaVrrProfile.cpp:61`, `NvapiLifetime.cpp:20`, `NvapiArchSpoof.cpp:93` | load `nvapi64.dll` from System32; missing -> unavailable | yes | COSMETIC | none |
| 21 | `Install-PushFPS.ps1:108-111` | package by WMI adapter name: any "NVIDIA" adapter -> NVIDIA package, else AMD/Intel package; WMI failure -> AMD package (an NVIDIA PC then loses DLSS until `-Gpu NVIDIA`); a desktop with an AMD display card and a spare NVIDIA card gets the NVIDIA package | PushFlow/FSR/XeSS in both packages (assumed from `make-release.ps1`; not checked against the package contents) | LOSES FEATURE | decide from `PNPDeviceID` `VEN_10DE` of the high-performance adapter instead of the name |
| 22 | `Install-PushFPS.ps1:165-210` experimental extras | NVIDIA files that are not NVIDIA-signed originals and NVIDIA's models on AMD are off unless `-ExperimentalExtras Yes` | the effect is reported unavailable, never broken | LOSES FEATURE (by licence rule) | none |
| 23 | `PushSlipstream.cpp:233-235` | welcome card: vendor by id, `gpuName` by description (the card shows it; logs carry only the vendor id) | n/a | COSMETIC | none |

Done in this change (`Hardware.cpp`, capability parts only): decode profiles that answer nothing count as unknown
in the capability model; an HDR output enumeration error is unknown, not "no"; the effect gates for frame
generation and optical flow read `pushFgGate` / `pushFlowGate` (the same table as the profiles); a known vendor
with an unknown feature level turns generation off in the model (`fg-feature-level-unknown`). All reporting only:
nothing applies the model.

### Known gaps, not fixed

- Live recorder: no Media Foundation fallback when NVENC refuses (`LiveRecorder.cpp:852`, `:856`; choice at
  `:1549`), so an NVIDIA PC whose NVENC session is refused cannot record or go live (audit #15).
- Installer package choice by WMI adapter name (`packaging/Install-PushFPS.ps1:108-111`), audit #21.
- Software HEVC decode on one thread with H.265 as the default codec (`RemotePlaySource.cpp:194`,
  `RemotePlay.cpp:421`); the capability model's `hevc-decode-missing` is not applied to the connect request (audit #13).

## GPU workload budget

`include/veyra/engine/PushGpuBudget.h` keeps headroom for decoding and presentation: from the user's
request it steps down quality tier, then multiplier, then frame generation off, one level at a time
(2 s of trouble and 3 s between steps; severe overload after 0.5 s), and steps back up only after
20 s healthy (doubling after a failed return), never above the request, never on unknown input or on
battery. Its header lists what the existing pieces (FgRecoveryBudget, FgCostSettling,
VfgOverloadEstimate, admitLiveFg, LiveGpuScheduler, PushAdaptiveFg, pushFlowBudgetWatch,
PushAutomaticMode) already do. Thresholds are estimates; nothing measured them.

### One arbiter (VEYRA_EXP_RP_GOVERNOR=apply)

`include/veyra/engine/PushGovernorControl.h` embeds this budget as decode and presentation protection and is
then the only automatic step-down/step-up logic for frame generation while a stream plays (the performance modes'
driver and Automatic mode can still set the multiplier; the governor treats that as a user change): protection first, then latency, one
change per decision, with the rate limits, rollback and lock-outs listed in docs/PS5_REMOTE_PLAY_EXPERIMENTS.md.
The GPU budget's own apply mode and pushFlowBudgetWatch's tier step stand aside; the optimiser and the quality
test get the user's choice back first. Enhancement reductions (AI picture enhancement, super resolution) are not
in its action set: their setters rebuild the graph. It decides from measurements only, never from a GPU name.
Experimental until validated on hardware.

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

## What a tester should run and send

Everyone, any PC:

1. `powershell -ExecutionPolicy Bypass -File scripts\remoteplay\detect-hardware.ps1 -DecodeProfiles`
   (or the test kit's `collect-diagnostics`): sends `hardware.json` with vendor class, driver version and
   date, video memory, Windows build, power source and dxdiag's decode modes. It writes no host name, user
   name or serial number; check the file before sending.
2. Start PushFPS with `VEYRA_EXP_RP_GPU_BUDGET=1` (reporting only), open the first-launch card once, connect to
   the PS5 with decode mode Automatic, play 2 minutes, close. Send the log lines `[push-gpu-caps]`,
   `[push-hardware]`, `remoteplay` / `remoteplay-decode` (which decoder opened, any fallback), `fg-default`,
   `backend-recovery`, `capability`, `reset-lifecycle`.
3. A PresentMon capture of 60 s (`--track_gpu_video`).
4. Anything reported unavailable that should not be (with the reason shown) and any crash with its log.

By family, in addition:

| Family | Also run | Watch for |
|---|---|---|
| NVIDIA RTX 30/40/50 | frame generation PushFlow 2X, then DLSS 2X; record 30 s SDR and 30 s HDR | `flow0` availability (release vs fork build), NVENC refusals |
| AMD RX 9000 | FSR 4 2X, then FSR 3.1 2X, then PushFlow; record 30 s SDR; once with `VEYRA_EXP_VRR_DXGI_FALLBACK=1` (diagnostic) | whether `fsr4` is available (name gate, audit #3/#12), the Media Foundation encoder name in `live-recorder` |
| AMD RX 6000 / 7000 | FSR 3.1, AMD FRC, PushFlow; record 30 s SDR; once with `VEYRA_EXP_VRR_DXGI_FALLBACK=1` (diagnostic) | `amdfrc` availability, the `[display-vrr] ... dxgiTearing=` line and whether the screen has FreeSync on (audit #19) |
| Intel Arc A / B | XeSS 2X, FSR 3.1, PushFlow; record 30 s SDR; once with `VEYRA_EXP_VRR_DXGI_FALLBACK=1` (diagnostic) | DirectML/PushFlow start-up (`pushflow` lines), optical flow chosen (`flow1`), `vrrState` (audit #19) |
| Laptop with two GPUs | everything above for its discrete GPU, once on mains and once on battery | `hybrid=yes`, `power=battery`, which adapter drives the screen (Windows Settings > Display > Graphics) |
| No hardware HEVC decoder, or an old driver | PS5 in H.265 and in H.264, decode mode Automatic | `remoteplay-decode actual=software`, stutter in PresentMon |
| 60 Hz, 144 Hz, 240 Hz, 360 Hz screens | a 30 fps game and a 60 fps game with frame generation on | the multiplier offered and used, `MsBetweenDisplayChange` P95/P99 |
