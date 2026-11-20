# Presentation latency: decoded PS5 frame to scanout (Slipstream, sprint 4)

Status: **code reading plus a Linux model.** Nothing in this document was built or run on Windows by its
author. Every statement about the code names a file and line of branch `exp/s4-present` at the commit that
adds this document. The only measured numbers are the ones quoted from the first hardware benchmark
(`BENCH1`: RTX 5070, 2560x1440 at 179.952 Hz, G-SYNC Compatible OLED, 60 fps PS5 stream, frame generation
off; software counters, **Wi-Fi, one session**). Model numbers are labelled "Linux model".

## 1. The path of one decoded frame

### 1.1 Windows and swap chain

- The video is drawn into a native child window `VeyraQmlVideoHost`, created as a hidden popup and
  reparented into the Qt window (`apps/veyra-qml/main.cpp:175-196`, attached at `:728`). The swap chain is
  created on that borrowed HWND (`src/engine/VideoPresenter.cpp:68`, `targetWindow`), so the window that
  presents is never the top-level window; the Qt interface is drawn by Qt in the parent window.
- Factory: `CreateDXGIFactory2`, tearing support from
  `IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)` (`src/gfx/PresentSink.cpp:199`).
- Swap chain (`src/gfx/PresentSink.cpp:205-216`): **flip model** (`FLIP_DISCARD`, or `FLIP_SEQUENTIAL` for
  the capture-compatible choice, `:214`), **3 buffers** (`:211`), format R8G8B8A8_UNORM for SDR
  (R10G10B10A2 for HDR10, FP16 for scRGB, `:208`), flags `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` whenever the
  factory supports tearing (`:215`) plus `FRAME_LATENCY_WAITABLE_OBJECT` when `desc.waitable` and not XeSS
  (`:216`). `desc.waitable` is true unless `VEYRA_TEST_LEGACY_SWAPCHAIN` is set
  (`src/engine/VideoPresenter.cpp:69`). If creation with the waitable flag fails, it is retried without
  it (`src/gfx/PresentSink.cpp:261`). The actual description is checked: flip model, 3 buffers
  (`:296-299`). Alt+Enter and window changes by DXGI are blocked (`MakeWindowAssociation`, `:282`).
  The chain is created with `vsync=false`, `tearing=false` (`src/engine/VideoPresenter.cpp:68`); the
  presentation settings are applied afterwards by `configurePresentation`.
- The presenter has its own direct command queue and fence for the lifetime of the native chain
  (`src/engine/VideoPresenter.cpp:56`, `VEYRA_TEST_PRESENT_QUEUE_PRIORITY=high` raises its priority).

### 1.2 Per frame, from engine pickup to Present

1. **Arrival / decode.** The engine reads the newest decoded picture from the source mailbox. Its
   network arrival (`pkt.arrivalHost100ns`, takion callback) and decoded time (`pkt.decodedHost100ns`) travel
   with it (`src/engine/EngineController.cpp:2417`, `:2946`).
2. **Read gate (before pickup).** For a live source without frame generation the engine does not read a
   new frame while (a) the low-latency queue is on and one frame is still in the GPU scheduler
   (`src/engine/EngineController.cpp:2019`), or (b) the display queue is "paced" and either a frame is in the
   scheduler or the frame-latency object has no capacity (`:2024`); while only the display queue is
   outstanding it sleeps on the latency object itself (`waitPresentationReady(1)`, `:2041`). This is the
   "wait before reading" of latency-min B3: a frame never waits in DXGI's queue, it waits in the mailbox,
   where a newer one can replace it.
3. **GPU work.** The graph (decode copy, colour, optional NR/SR) runs on the graph queue. The presentation
   queue waits for the graph's producer fence **on the GPU** (`ID3D12CommandQueue::Wait`,
   `src/engine/VideoPresenter.cpp:275`), records the blit into the back buffer and submits
   (`:503`). There is no CPU fence wait inside `VideoPresenter::present`.
4. **CPU readiness.** For a PS5 stream `submitThenPresent` is false (it is true only for XeSS or a physical
   capture card, `src/engine/EngineController.cpp:2942-2943`), so the live step presents a real frame only
   after the engine has observed its graph work complete (`watch->ready`, `:2971`, `:2986`). This CPU-side
   observation sits between pickup and the Present call.
5. **Capacity before Present.** Before every present the step acquires display-queue capacity
   (`presenter.presentationReady()`, `src/engine/EngineController.cpp:3051`); `PresentSink::waitPresentationReady`
   waits on the latency object only while pacing (`src/gfx/PresentSink.cpp:596-602`).
6. **Present.** `PresentSink::present` (`src/gfx/PresentSink.cpp:382-393`): `syncInterval = vsync ? 1 : 0`,
   `flags = DXGI_PRESENT_ALLOW_TEARING` only when tearing was asked, VSync is off and tearing is supported.
   Just before the call it samples the frame statistics (`:391`) and the QPC time of the call (`:392`).
   `VideoPresenter` stamps the call (`dxgiStart`, `src/engine/VideoPresenter.cpp:512`) and its return (`:518`).

### 1.3 What each setting changes

| Setting | Code | Effect on presentation |
|---|---|---|
| Display sync **Vsync** | `presentationVsync` (`include/veyra/engine/PresentationSettings.h:61`) | `Present(1,0)`; the sink is paced: `SetMaximumFrameLatency(1)` and the engine waits on the latency object (`src/engine/VideoPresenter.cpp:741`, `src/gfx/PresentSink.cpp:570`, stale counts dropped `:589`). |
| Display sync **Automatic** | same file `:45-60` comment, `:61` | `Present(0,0)`, not paced, maximum latency 3 (`src/gfx/PresentSink.cpp:570`); an unshown frame is replaced at the next vblank. |
| Display sync **Tearing** | `presentationTearing` (`PresentationSettings.h:64`) | `Present(0, ALLOW_TEARING)` when supported (`src/gfx/PresentSink.cpp:383`), not paced. |
| **Low-latency queue** (`PresentationSettings::enabled`, bridge `fgLowQueue`) | `PresentationSettings.h:20`; `EngineController.cpp:2019` | Does **not** change the DXGI queue: maximum latency is 1 whenever explicit VSync is in force, whatever this switch says (`VideoPresenter.cpp:741` does not read `enabled`). It changes the engine's read gate (one frame in the GPU scheduler at a time) and is required for NVIDIA low latency. So `[pacing] DXGI enabled=true maximumLatency=1` in every BENCH1 arm follows from VSync, not from this switch. |
| **NVIDIA low latency** | `VideoPresenter.cpp:639` (implies the low queue), `:753`; `include/veyra/gfx/ReflexSession.h:49-69` | NvAPI `D3D12_SetSleepMode` (low latency on, markers on, no boost), optional out-of-band queue notification for the presentation queue, latency markers, and one `D3D12_Sleep` per **source** frame before it is read (`EngineController.cpp:2118`, `:2754`). For a video presenter it can only delay the read until the driver expects the GPU to be free; it does not touch the flip queue or the vblank wait. |
| **Variable refresh** (`variableRefresh`, 可变刷新) | `PresentationSettings.h:30`; `VideoPresenter.h:114-116`; `VideoPresenter.cpp:686-689` | Only with display sync **Automatic** and VRR level >= 3: Automatic is presented as VSync (`vrrSync=true`). With explicit VSync it does nothing; BENCH1 shows `requested=true/0/1` (display = 1 = VSync), hence `vrrSync=false`. |
| **Smooth pacing** (`pushSmoothPacing`) | `qml/Veyra/PushQuickSetup.qml:25-29` (`setSmoothPacing`) | Sets display sync **VSync** + `variableRefresh` + output cap Follow display. So the VRR rule above never applies under smooth pacing; whether the panel runs VRR is up to the driver alone. |
| `displayQueuePaced` (log) | `EngineController.cpp:1496`, `:1499` | `presenter.pacingActive()` (the sink acquired the latency object) and not `VEYRA_TEST_LEGACY_VSYNC_QUEUE`. |
| `vrrSync` (log) | `VideoPresenter.cpp:686` | Result of the VRR rule above. |
| Output cap Follow display | `VideoPresenter.cpp:663-668` | Becomes a custom cap at the refresh rate; a 60 fps stream never reaches it. |
| **Auto-fullscreen** (`pushAutoFullscreen`) | `qml/Veyra/Main.qml:1131-1140`, `toggleFullscreen` `:513-523` | Sets the Qt window to `Window.FullScreen`. The video child is then sized by the interface layout; it covers the monitor only when nothing of the interface is laid out beside it. |

### 1.4 Independent flip or composition

The swap chain is flip model with `ALLOW_TEARING` on the chain, which is what independent flip needs from
the application. Whether DWM composes it is decided per present by Windows: independent flip (or an overlay
plane) needs the chain's window to cover the monitor with nothing drawn over it. Here the chain lives in a
child window inside a Qt window that has its own Qt swap chain; when the Qt interface is visible beside the
video (windowed, dock, dialogs) the child does not cover the monitor and DWM composes. Anything drawn over
it (the subtitle and protection overlays are layered child windows of the video window, created only when
subtitles or protection drawing are used: `apps/veyra/ui/SubtitleOverlay.cpp:61-64`, `src/ui/bridge/View.cpp:101`,
`src/ui/bridge/Subtitles.cpp:136`; the NVIDIA overlay, which BENCH1 found injected) can also force
composition. The code cannot see the mode: `IDXGISwapChainMedia::GetFrameStatisticsMedia`
(`src/gfx/PresentSink.cpp:552`) answered nothing in BENCH1 (`compositionMode=-1`); the HRESULT is now logged
once (`[slipstream-present] mediaStatistics hr=...`). **PresentMon's PresentMode column is the reference.**

### 1.5 How `[display-latency]` is computed

`PresentSink::sampleDisplayLatency` (`src/gfx/PresentSink.cpp:484-529`), called before every VSync or
Automatic DXGI Present (`:390-391`; never with tearing, never for XeSS):

- `GetFrameStatistics` gives the last present shown (`PresentCount`), the vblank index it was shown at
  (`PresentRefreshCount`), and the most recent vblank (`SyncRefreshCount`) with its QPC time
  (`SyncQPCTime`).
- The refresh period is measured from two successive statistics (`:490`), else taken from the monitor rate.
- Scanout of the shown present = `SyncQPCTime - (SyncRefreshCount - PresentRefreshCount) x period` (`:505`),
  i.e. the start of the vblank at which the flip took effect.
- Each present's call time (QPC just before `Present`, `:392`) is kept in a 16-entry ring by present id
  (`GetLastPresentCount`, `:395-397`); a present older than the shown one is dated back by whole periods
  (`:522`) only while presents were at most about one refresh apart (`:521`); Present(0,0) frames are dated
  only when named as shown (`:520`).
- The value is **Present call -> start of the vblank that showed it**, P50/P95/max over about one second
  (`:544-545`, logged by `VideoPresenter.cpp:530`).

Error sources: (1) it ends at the start of the vblank, not at photons: the top line is scanned out right
after, the middle of the screen about half a refresh later; (2) it starts at the Present **call**, so
anything before (GPU work, waits) is outside it; (3) the period estimate and the integer vblank arithmetic
assume a fixed refresh, so under active VRR the dating is not meaningful; (4) sampling happens only at the
next Present, so a present whose statistics were overtaken before sampling is skipped, never mis-dated;
(5) whether DXGI's `PresentRefreshCount` in composed mode names DWM's composition vblank or the display
vblank is not documented; this is hypothesis H2 below.

### 1.6 Where the waits are, in order

pickup gate (latency object / scheduler, `EngineController.cpp:2019-2041`) -> GPU-side producer wait
(`VideoPresenter.cpp:275`) -> CPU readiness observation (`EngineController.cpp:2986`) -> capacity check
before Present (`:3051`) -> `Present(1,0)` (no CPU wait expected: capacity was acquired) -> flip at the
first vblank the GPU work and the flip latch allow -> (composed only) DWM composition, shown one vblank later.

## 2. The measured 9-10 ms, term by term

BENCH1 (software counter, Wi-Fi, one session): `presentToScanoutP50Ms` 9.1-9.9 ms, P95 12.7-14.2 ms at a
5.557 ms refresh, every arm.

**The per-second values are not random.** Within one second the P50 and P95 differ by about 0.4 ms
(e.g. `9.827 / 10.187`, `9.977 / 10.396`), and the P50 drifts slowly. Over whole runs (all `[display-latency]`
lines of `/home/claude/evidence/e1/x/bench`, computed for this document):

| run | per-second P50: min | median | max |
|---|---|---|---|
| stable (mod-all, mod-legacy), 8 runs | 6.19-6.41 ms | 9.05-9.68 ms | 11.12-12.47 ms |
| original, 4 runs | 0.10-0.40 ms | 9.00-9.20 ms | 10.56-11.12 ms |

The stable runs sweep exactly the band **[1 x refresh, 2 x refresh] + 0.6-0.8 ms**. That is the signature of
a 59.94 fps stream beating against a 179.952 Hz grid: one frame interval is 0.012 ms longer than three
refreshes, so the arrival phase slides through a whole refresh in about 450 frames (7.5 s) and the wait for
the next vblank sweeps 0 to 1 refresh. The extra full refresh is constant.

Sum of named terms (theory for 60 fps content on a fixed 180 Hz refresh):

| term | value | evidence |
|---|---|---|
| wait for the next vblank after an unsynchronised Present | uniform 0 to 5.56 ms, mean 2.78 ms | the sweep above |
| flip latch / GPU completion before the vblank | ~0.6-0.8 ms | floor of the sweep above 1 refresh; `latchMs_` (`PresentSink.cpp:523`) tracks the same quantity |
| display-queue depth x refresh | 0 (one-deep queue, waited before the read; `replacedAtLeast=0`) | `EngineController.cpp:2024`, `PresentSink.cpp:570` |
| **one extra refresh** | **5.56 ms** | floor 6.2-6.4 ms in every stable run; never below one refresh |
| statistics convention | ends at vblank start (top of the picture) | section 1.5 |

5.56 x 1.5 + 0.7 = 9.0 ms mean, 2 x 5.56 + 0.7 = 11.8 ms upper edge: the measured P50 (9.1-9.9) and the
per-second P95 (mostly 10-11.5) fit. The P95 values of 12.7-14.2 and the worst 18-27 ms are presents that
waited one or more further refreshes (a missed composition or flip).

### Ranked hypotheses for the extra refresh

1. **H1: DWM composes the video window (PresentMon "Composed: Flip").** A composed present is picked up by
   DWM at its next composition and appears one vblank later: exactly +1 refresh. It also explains why VRR did
   not engage (DWM composes at the fixed rate) and why the original build shows intervals of 0.1-2.6 ms
   (21 of 176 and 14 of 309 seconds in two runs): those look like periods of independent flip (or VRR), which
   the stable runs never reached. Those sub-refresh seconds also have few samples (mean 5-7 instead of 40)
   and "replaced" presents, so they are a different regime, not noise.
   *Confirm:* PresentMon `PresentMode` = `Composed: Flip` in the baseline arm and `Hardware: Independent Flip`
   or `Hardware Composed: Independent Flip` in the fullscreen arm; `[slipstream-swapchain] fullscreen=1
   windowCoversMonitor=1` in that arm; `[slipstream-latency] presentToScanoutP50` near 3-3.5 ms there.
2. **H2: the statistics are off by one refresh in composed mode** (DXGI reports a vblank one later than the
   one that showed the frame). *Confirm:* PresentMon `MsUntilDisplayed` (Present call -> display) against
   `[slipstream-latency] presentToScanoutP50` in the same run: they must agree within ~0.5 ms; if PresentMon
   says ~3.3 ms while the app says ~9, the app's dating is wrong, not the path.
3. **H3: a second frame queued somewhere** (the latency object not limiting the queue in this mode, e.g.
   after `configurePacing` re-armed). Evidence against: one-deep queue waited before the read, `replacedAtLeast=0`.
   *Confirm:* `[display-stats] presentsAhead` <= 1 and `[slipstream-latency] presentCallP95` < 1 ms (Present
   not blocking); PresentMon `MsInPresentAPI` small.
4. **H4: GPU completion or overlay hooks crossing the latch.** GPU load was 1-2 %; the NVIDIA overlay is
   injected. *Confirm:* an arm with the NVIDIA overlay (and instant replay) disabled; PresentMon
   `MsRenderPresentLatency` (`MsGPULatency` in 2.x) well under 1 ms.

## 3. What VRR changes for 60 fps on a 180 Hz panel

With VRR active the panel starts a refresh when a flip arrives (no sooner than 1/180 s after the previous
one), so the "wait for the next vblank" term (mean 2.78 ms, up to 5.56 ms) disappears, and, if H1 holds, so
does DWM's refresh, because VRR needs independent flip. The lowest-latency configuration known in the field
(Microsoft's tearing/VRR guidance, NVIDIA G-SYNC guidance): flip model, independent flip (borderless window
covering the monitor, nothing over it), VRR active, a queue of 1, and either tearing allowed
(`Present(0, ALLOW_TEARING)`) or VSync with the frame rate capped below the maximum refresh. For this stream
(60 fps, far below 180) a cap is not needed; VSync under VRR only costs something when two frames arrive
within 5.56 ms of each other (Wi-Fi clumps), where tearing would show the second frame at once with a tear.

What applies here: BENCH1's `[display-vrr] ... requested=false active=false displayInVrrMode=true` was read
**at the start of the chain, before the first present** (`VideoPresenter::startDisplayState`,
`src/engine/VideoPresenter.cpp:592-606`), and is re-read only on events or 3 s after `vrrSync` starts
(`VideoPresenter.cpp:686-689`), which never happens under explicit VSync. So the log cannot say whether VRR
ran during play; the per-second sweep says it did not (a VRR display would not show a fixed-grid sawtooth).
NVIDIA's per-program G-SYNC mode ("fullscreen only" vs "fullscreen and windowed", the two profile settings
written by the G-SYNC button, `include/veyra/gfx/NvidiaVrrProfile.h:7`, `planVrrProfileRepair` `:49-61`) also
decides whether a windowed chain may use VRR.

## 4. Linux model (tools/remoteplay_latency_bench/present_sim.cpp)

**Linux model, not a measurement.** 10 simulated minutes per row; 59.94 fps; decode 1-3 ms; pickup ->
Present 0.4-1.2 ms; flip latch 0.5 ms (VRR 0.3 ms); timer wake 0-0.2 ms late; VRR range 48 Hz to the refresh.
Wired-like: 0.3 ms + |N(0,0.15)| jitter, 0.2 % of frames 1-4 ms late. Wi-Fi-like: 1 ms + exponential (mean
1.5 ms), 3 % of frames held 5-25 ms with the frames behind them clumped. All assumptions. Results:
`present_sim_results.json`; reproduce with `present_sim --table`.

Rules: `composed` = today's VSync path under DWM composition; `independent` = the same path in independent
flip; `vrr-vsync` = VSync with VRR active; `tearing-vrr` = `VEYRA_EXP_PRESENT_TEARING` with VRR active;
`late-<margin>` = just-in-time presentation (independent flip). "dropped": decoded frames never shown;
"missed vblank": just-in-time presents that missed their vblank; "torn": frames flipped within 1/maxHz of
the previous refresh start.

180 Hz (179.952):

| network | Hz | rule | arrival->scanout P50 | P95 | P99 | present->scanout P50 | P95 | dropped | missed vblank | torn |
|---|---|---|---|---|---|---|---|---|---|---|
| wired-like | 180 | composed | 11.64 | 14.38 | 14.98 | 8.84 | 11.33 | 0 | 0 | 0 |
| wired-like | 180 | independent | 6.09 | 8.82 | 9.42 | 3.28 | 5.78 | 0 | 0 | 0 |
| wired-like | 180 | vrr-vsync | 3.10 | 4.09 | 4.32 | 0.30 | 0.30 | 0 | 0 | 0 |
| wired-like | 180 | tearing-vrr | 3.10 | 4.09 | 4.31 | 0.30 | 0.30 | 0 | 0 | 7 |
| wired-like | 180 | late-800us | 11.04 | 13.77 | 14.35 | 5.46 | 5.83 | 0 | 35964 | 0 |
| wired-like | 180 | late-1200us | 10.51 | 14.00 | 14.65 | 5.66 | 6.02 | 0 | 27000 | 0 |
| wired-like | 180 | late-1600us | 7.21 | 13.49 | 14.66 | 0.90 | 6.02 | 0 | 8950 | 0 |
| wired-like | 180 | late-2500us | 7.17 | 9.89 | 10.47 | 1.60 | 1.97 | 0 | 0 | 0 |
| wifi-like | 180 | composed | 11.71 | 14.58 | 16.26 | 8.90 | 11.33 | 6 | 0 | 0 |
| wifi-like | 180 | independent | 6.16 | 9.02 | 10.70 | 3.34 | 5.78 | 6 | 0 | 0 |
| wifi-like | 180 | vrr-vsync | 3.16 | 5.91 | 9.02 | 0.30 | 2.94 | 2 | 0 | 0 |
| wifi-like | 180 | tearing-vrr | 3.11 | 4.15 | 4.49 | 0.30 | 0.30 | 0 | 0 | 2288 |
| wifi-like | 180 | late-800us | 11.10 | 13.95 | 16.22 | 5.46 | 5.83 | 484 | 35480 | 0 |
| wifi-like | 180 | late-1200us | 10.56 | 14.12 | 15.09 | 5.66 | 6.02 | 478 | 26590 | 0 |
| wifi-like | 180 | late-1600us | 7.23 | 13.52 | 14.73 | 0.89 | 6.02 | 455 | 8823 | 0 |
| wifi-like | 180 | late-2500us | 7.18 | 9.93 | 10.58 | 1.59 | 1.97 | 431 | 0 | 0 |

144, 120 and 60 Hz (main rules and the only just-in-time margin without misses):

| network | Hz | rule | arrival->scanout P50 | P95 | P99 | present->scanout P50 | P95 | dropped | missed vblank | torn |
|---|---|---|---|---|---|---|---|---|---|---|
| wired-like | 144 | composed | 13.72 | 17.03 | 17.72 | 10.92 | 14.05 | 0 | 0 | 0 |
| wired-like | 144 | independent | 6.77 | 10.09 | 10.77 | 3.98 | 7.11 | 0 | 0 | 0 |
| wired-like | 144 | vrr-vsync | 3.10 | 4.10 | 4.32 | 0.30 | 0.30 | 0 | 0 | 0 |
| wired-like | 144 | tearing-vrr | 3.10 | 4.10 | 4.32 | 0.30 | 0.30 | 0 | 0 | 5 |
| wired-like | 144 | late-2500us | 7.88 | 11.18 | 11.83 | 1.60 | 1.97 | 0 | 0 | 0 |
| wired-like | 120 | composed | 15.80 | 19.71 | 20.44 | 13.01 | 16.75 | 0 | 0 | 0 |
| wired-like | 120 | independent | 7.47 | 11.38 | 12.10 | 4.67 | 8.42 | 0 | 0 | 0 |
| wired-like | 120 | vrr-vsync | 3.10 | 4.10 | 4.32 | 0.30 | 0.30 | 0 | 0 | 0 |
| wired-like | 120 | tearing-vrr | 3.10 | 4.10 | 4.32 | 0.30 | 0.30 | 0 | 0 | 7 |
| wired-like | 120 | late-2500us | 8.56 | 12.43 | 13.14 | 1.60 | 1.97 | 0 | 0 | 0 |
| wired-like | 60 | composed | 29.48 | 36.99 | 38.07 | 26.69 | 32.60 | 0 | 0 | 0 |
| wired-like | 60 | independent | 12.82 | 20.32 | 21.41 | 10.03 | 15.93 | 0 | 0 | 0 |
| wired-like | 60 | vrr-vsync | 4.11 | 5.78 | 6.60 | 1.40 | 3.11 | 0 | 0 | 0 |
| wired-like | 60 | tearing-vrr | 3.09 | 4.09 | 4.32 | 0.30 | 0.30 | 0 | 0 | 26698 |
| wired-like | 60 | late-2500us | 12.56 | 19.91 | 20.80 | 1.60 | 1.97 | 737 | 0 | 0 |
| wifi-like | 144 | composed | 13.79 | 17.30 | 20.10 | 11.01 | 14.03 | 13 | 0 | 0 |
| wifi-like | 144 | independent | 6.84 | 10.36 | 13.15 | 4.06 | 7.09 | 13 | 0 | 0 |
| wifi-like | 144 | vrr-vsync | 3.19 | 7.29 | 10.36 | 0.30 | 4.31 | 11 | 0 | 0 |
| wifi-like | 144 | tearing-vrr | 3.11 | 4.14 | 4.48 | 0.30 | 0.30 | 0 | 0 | 2412 |
| wifi-like | 144 | late-2500us | 7.88 | 11.20 | 11.89 | 1.60 | 1.97 | 521 | 0 | 0 |
| wifi-like | 120 | composed | 15.94 | 20.03 | 23.52 | 13.14 | 16.75 | 27 | 0 | 0 |
| wifi-like | 120 | independent | 7.60 | 11.70 | 15.18 | 4.81 | 8.41 | 27 | 0 | 0 |
| wifi-like | 120 | vrr-vsync | 3.21 | 9.11 | 11.79 | 0.30 | 6.06 | 29 | 0 | 0 |
| wifi-like | 120 | tearing-vrr | 3.11 | 4.15 | 4.51 | 0.30 | 0.30 | 0 | 0 | 2485 |
| wifi-like | 120 | late-2500us | 8.55 | 12.47 | 13.20 | 1.60 | 1.97 | 566 | 0 | 0 |
| wifi-like | 60 | composed | 40.79 | 49.10 | 50.57 | 32.36 | 32.80 | 230 | 0 | 0 |
| wifi-like | 60 | independent | 24.13 | 32.43 | 33.90 | 15.69 | 16.13 | 230 | 0 | 0 |
| wifi-like | 60 | vrr-vsync | 23.92 | 27.94 | 29.41 | 15.74 | 16.14 | 67 | 0 | 0 |
| wifi-like | 60 | tearing-vrr | 3.11 | 4.14 | 4.51 | 0.30 | 0.30 | 0 | 0 | 27186 |
| wifi-like | 60 | late-2500us | 12.30 | 19.80 | 20.89 | 1.60 | 1.97 | 2635 | 0 | 0 |

What the model says (as a model):

- The composed path reproduces the shape of BENCH1's present->scanout (model P50 8.8 / P95 11.3 at 180 Hz
  against the measured 9.1-9.9 / 12.7-14.2). Independent flip removes about one refresh at every rate.
- VRR (VSync or tearing) removes the remaining vblank wait: present->scanout becomes the flip latch.
  Tearing only adds over VRR VSync where frames clump (Wi-Fi), at the price of torn frames; on a 60 Hz panel
  a 59.94 fps stream is at the top of the VRR range, so "tearing" tears almost every frame there.
- At 60 Hz a 59.94 fps stream never lets a one-deep VSync queue drain after a Wi-Fi clump (the queue keeps
  one extra refresh until a frame is dropped): the Wi-Fi rows at 60 Hz are far worse than the wired ones.
- **Just-in-time presentation does not help a 60 fps stream on a 120-180 Hz panel.** With one frame per
  2-3 refreshes, presenting at once already shows the frame at the first vblank it can make; holding it until
  `vblank - margin` can only make that vblank or miss it. A margin below pickup->Present + latch + wake
  (here up to 1.9 ms) misses most vblanks; a safe margin (2.5 ms) is 1 ms worse than presenting at once at
  P50 and drops frames on Wi-Fi in exchange for a P99 gain of about 0.1 ms. It would only pay where several
  frames arrive per refresh (frame generation above the refresh, or a 60 Hz panel), where frame generation
  already has its own pacing. **`VEYRA_EXP_PRESENT_LATE` is therefore not implemented**; if it is ever built,
  the default margin from this model would be 2.5 ms (above the assumed 1.9 ms worst case), and it must be
  measured with `[slipstream-latency] pickupToPresentCallP95 + presentCallP95` on the PC first.

## 5. Experiments (all off unless set to `1`; read once per swap-chain creation)

These are process environment variables. **The Slipstream settings file cannot set them**: it only passes
keys that begin with `VEYRA_EXP_RP_` or `VEYRA_TEST_RP_` (`include/veyra/engine/SlipstreamSettings.h:26-27`).
The kit must set them in the environment of the launched process.

| Switch | What it changes | Where | Risk |
|---|---|---|---|
| `VEYRA_EXP_RP_STATS=1` | `[slipstream-latency]` every 5 s, `[slipstream-swapchain]` on creation, pacing change and resize, `[slipstream-present] mediaStatistics` once per chain. Records into fixed arrays only. | `PresentSink.cpp:144`, `:398`, `:524-526`, `:556-561`, `:676-698`; `VideoPresenter.cpp:797-807`; one call `EngineController.cpp:3159` | One extra loop over 16 entries and a few stores per real frame; a 5 s percentile sort of at most 1024 floats per metric on the presentation thread. |
| `VEYRA_EXP_PRESENT_TEARING=1` | Where explicit VSync would apply, presents like the Tearing choice: `Present(0, ALLOW_TEARING)`, not paced (no latency-object wait). Only while the driver reports VRR level 4, the factory and chain support tearing, no frame generation runs, no XeSS provider presents and no output cap other than Follow display is set (`pushTearingRefusal`, `include/veyra/engine/PushPresentTiming.h`). Logs `[slipstream-present] experiment=tearing active=<0|1> reason=<...>` when the decision changes. | `VideoPresenter.cpp:690-708` | If VRR is not really running for the window (composed, or "fullscreen only" G-SYNC with a windowed chain), frames tear visibly; if the VRR level drops during play the decision is not re-evaluated until the presentation is reconfigured. Frame statistics cannot date torn presents, so `[display-latency]` stops and `scanoutKnown=0`: use PresentMon. |
| `VEYRA_EXP_PRESENT_VRR_REQUEST=1` | Re-reads the driver's VRR state 3 s into each new chain (`[display-vrr]` then shows `requested` / `active` while presenting) and, once per process, reads the program's NVIDIA G-SYNC profile on a worker thread and logs `[slipstream-present] vrrProfile ... configured=<0|1> repairWouldChange=<0|1>` (values only, never the path). **Writes nothing**: making the profile "fullscreen and windowed" stays the user's G-SYNC button. | `VideoPresenter.cpp:130-132`, `:767-796` | A detached worker using NVAPI's DRS read calls (the same `inspectVrrProfile` the G-SYNC check uses, serialised by its own mutex); one extra background driver query per chain. |
| `VEYRA_EXP_PRESENT_LATE` | Not implemented (section 4). | - | - |

No queue or flag change without a downside was found: the 3-buffer chain cannot add a refresh because the
one-deep latency object is waited before the read, and maximum latency 1 is already applied for every VSync
chain. The two gaps are diagnostic: the composition mode is unknown (now logged with its HRESULT) and the VRR
state is never re-read under explicit VSync (the VRR-request switch re-reads it).

## 6. What to test on the PC (kit arms)

Every arm: experimental build, `VEYRA_EXP_RP_STATS=1`, frame generation off, same stream and preferences,
PresentMon 2.x running on `PushFPS.exe` for at least 60 s of steady play (columns: PresentMode,
AllowsTearing, MsUntilDisplayed, MsBetweenDisplayChange, MsInPresentAPI; option and column names differ
between PresentMon 1.x and 2.x, so read the tool's own `--help` at run time). Wi-Fi: diagnostic only.

| arm | set | expect in `[slipstream-latency]` / `[slipstream-swapchain]` | expect in PresentMon |
|---|---|---|---|
| baseline | nothing else (windowed, interface visible) | `syncInterval=1 tearing=0 queueLatency=1 waitable=1`; `presentToScanoutP50` ~9-10 ms as in BENCH1; `fullscreen=0` | `Composed: Flip` (H1) and `MsUntilDisplayed` ~ the app's value (H2 check) |
| fullscreen | `--pref pushAutoFullscreen=true` (or F11), dock hidden, no overlays | `fullscreen=1 windowCoversMonitor=1`; if H1: `presentToScanoutP50` ~3-3.5 ms, P95 under ~6.5 ms | `Hardware: Independent Flip` or `Hardware Composed: Independent Flip` |
| tearing | fullscreen arm + `VEYRA_EXP_PRESENT_TEARING=1` | `[slipstream-present] experiment=tearing active=1`; `syncInterval=0 tearing=1 queueLatency=3`; `scanoutKnown=0`; `pickupToPresentCall` and `presentCall` unchanged or lower | independent flip, `AllowsTearing=1`, `MsUntilDisplayed` under ~1 ms if VRR runs; visible tearing means VRR is not running |
| vrr-request | fullscreen arm + `VEYRA_EXP_PRESENT_VRR_REQUEST=1` | a second `[display-vrr]` ~3 s after start with `active=` / `requested=` during play; one `vrrProfile ... configured=` line | `MsBetweenDisplayChange` following the frames (~16.7 ms) instead of multiples of 5.56 ms if VRR runs |
| late | not built (section 4) | - | - |

The decision rule for the next sprint: if PresentMon shows `Composed: Flip` in the baseline and independent
flip with ~3 ms in the fullscreen arm, the fix is about the window (cover the monitor with the video child
while playing, keep overlays off it), not about DXGI flags. If PresentMon disagrees with the app's value by a
refresh, fix the statistics first (H2).
