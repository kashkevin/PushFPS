# PS5 Remote Play: research and experiments (2026-11-18, second pass)

Branch `perf/ps5-low-latency`. Continues `PS5_REMOTE_PLAY_LATENCY_2026-11-18.md`.

**Verification status: unverified on Windows.** No Windows build of this branch exists yet, and nothing
here has run on an RTX 5070, on a 180 Hz panel or against a PS5. Every experiment is off unless its
variable is set, so the default behaviour is the first commit's, and `VEYRA_TEST_RP_LEGACY_TIMING`
still restores upstream's timing.

## Switches

| Variable | Effect | State |
|---|---|---|
| `VEYRA_TEST_RP_LEGACY_TIMING` (and `_INPUT`, `_OWNER_WAIT`, `_ENGINE_WAKE`) | upstream timing, whole or per part | first commit |
| `VEYRA_EXP_RP_ADAPTIVE_PHASE=1` | with frame generation, the stream learns its pair phase from measured readiness instead of holding the real frame a full source interval | experiment |
| `VEYRA_EXP_RP_PAD_POLL_MS=1..8` | pad, keyboard and mouse poll interval (default 4) | experiment |
| `VEYRA_EXP_RP_KBM=1` | keyboard and mouse as a DualSense, mouse on the right stick | experiment |
| `VEYRA_EXP_RP_KBM=gyro` | the same, mouse as the pad's motion sensor | experiment |
| `VEYRA_EXP_RP_KBM_SENS`, `_FULL`, `_DEADZONE`, `_CURVE`, `_YSCALE`, `_SMOOTH_MS`, `_GYRO_DEG`, `_INVERT_X`, `_INVERT_Y`, `_BINDS` | mouse and key settings, see below | experiment |

## What limits the latency

Button to screen on a stream is: pad report, PC poll, send, console (game, encoder), network,
receive, decode, effects, present, panel. This application owns the second, third and the last five.
After the first commit their sum with frame generation off is a few milliseconds, of which decode on
the GPU is the largest part. The console's share is fixed by the game and by Sony's encoder; Sony
publishes no figure for it.

That leaves three places where time can still be taken out, in order of size:

1. **Frame generation delay.** Interpolation needs the next real frame. On a stream PushFPS shows
   real frame B one source interval plus a processing allowance after it was decoded
   (`legacyPairDelay`, 16.7 ms + allowance at 60 fps). Capture cards already use a learned phase
   (`LivePairLatency`): the first generated frame goes out when it is ready, B follows (M-1)/M of an
   interval later. `VEYRA_EXP_RP_ADAPTIVE_PHASE` gives a stream the same. Expected at 2X: about half an
   interval plus the generator's GPU time instead of a whole interval plus allowance, so 5-8 ms less,
   by construction of the code and not measured. Extrapolation would remove this delay entirely (below).
2. **Display wait.** Without variable refresh a frame waits for the next refresh: up to 5.6 ms at 180 Hz.
3. **Input sampling.** In the input model (`input_sim --mode new --poll-ms N`, virtual time, not a
   measurement) a state change reaches Chiaki after 2.1 ms on average at the 4 ms poll, 1.1 ms at 2 ms
   and 0.6 ms at 1 ms. A DualSense reports every 4 ms over USB, so for a pad the gain is the poll phase
   only; a 1000 Hz mouse benefits fully. With a moving stick or mouse every poll sends a state packet:
   about 1000 a second at 1 ms. Whether the console is indifferent to that rate is untested.

## Predictive frame generation: what the literature says, and what a prototype measured

- **Extrapolation with engine data.** ExtraNet (Guo et al., SIGGRAPH Asia 2021) and ExtraSS (Wu et al.,
  2023) extrapolate with G-buffers and motion vectors from the renderer. A video stream has neither.
- **Extrapolation from pictures alone.** GFFE (Wu et al., ACM TOG 43(6), 2024,
  https://arxiv.org/abs/2406.18551) needs no G-buffer: heuristics for dynamic fragments and
  disocclusions, then a small correction network. It is the closest published design to what a stream
  needs. No implementation is published.
- **VR.** Oculus Asynchronous Spacewarp extrapolates a frame from the motion between the two previous
  ones, with motion vectors from the GPU's video encoder
  (https://developers.meta.com/horizon/blog/asynchronous-spacewarp/). It predicts linear motion better
  than non-linear motion, which is also what the prototype below shows.
- **Late warp from input.** NVIDIA Reflex 2 Frame Warp shifts the rendered frame by the newest mouse
  input just before scan-out and inpaints the uncovered border
  (https://www.nvidia.com/en-us/geforce/news/reflex-2-even-lower-latency-gameplay-with-frame-warp/).
  It needs the game's camera. Outatime (Lee et al., MobiSys 2015) did the equivalent for cloud gaming
  with speculative rendering on the server. A client alone would have to guess how far the camera turns
  for a given stick deflection in each game; not attempted.
- **NVIDIA hardware.** DLSS 4 frame generation no longer uses the optical flow accelerator, and
  multi-frame generation paces its frames with Blackwell's hardware flip metering
  (https://www.nvidia.com/en-us/geforce/news/dlss4-multi-frame-generation-ai-innovations/). Both are
  inside the driver's DLSS path; an application reaches them only by using DLSS frame generation, which
  PushFPS already offers as a backend. NVIDIA's Optical Flow SDK page lists Turing, Ampere and Ada; whether
  the accelerator exists on an RTX 5070 was not confirmed.

### Prototype (`tools/fg_extrapolation_study/study.py`)

A CPU reference: OpenCV DIS optical flow, real frame B moved forward along the flow A to B (constant
velocity, two fixed-point iterations), against flow-based interpolation of A and B and against showing
B until the next frame. Five synthetic scenes rendered at 360 fps so the true picture exists at every
display instant, 640x360, 32 pairs each. PSNR in dB.

"Fidelity" compares a generated frame with the instant it claims to show. "On time" compares what is
on screen with the true picture at that moment, so delay counts as error.

| 2X (60 to 120) | fidelity: extrapolated | fidelity: interpolated | on time: hold | on time: extrapolated | on time: interpolated, half an interval late | a whole interval late |
|---|---:|---:|---:|---:|---:|---:|
| steady pan | 36.2 | 38.9 | 15.8 | 36.2 | 15.8 | 15.9 |
| turn start / stop | 27.2 | 34.5 | 21.6 | 27.2 | 21.4 | 19.0 |
| object over static background | 34.1 | 35.4 | 26.3 | 34.1 | 26.8 | 25.5 |
| bounce (direction reversal) | 21.8 | 23.3 | 19.1 | 21.8 | 20.1 | 16.5 |
| pan with HUD | 27.5 | 29.3 | 15.8 | 27.5 | 15.7 | 15.4 |

3X (60 to 180) shows the same pattern (`results.json`).

What this supports:

- Extrapolated frames are 1.3-3 dB worse than interpolated ones in steady motion and 7-8 dB worse while
  the camera accelerates. That is the overshoot at the start and end of a turn, and it is the main
  quality cost.
- Measured against the present moment, extrapolation is ahead in every scene, because a late frame is in
  the wrong place. PSNR punishes displacement of fine texture heavily, so the size of these gaps says
  "wrong place", not "looks this much worse".
- Damping the prediction (half the movement) was worse than full extrapolation in every scene, also the
  accelerating one (24.2 against 27.2 dB). There is no evidence here for a cautious fallback.
- The error of predicting B from the pair before it, which is available at run time, tracks the error of
  the next extrapolated frames across scenes (Pearson r = 0.90 pooled). Within a steady scene it carries
  no information.

What it does not show: real game footage, compression artefacts, the HUD Lock composition, GPU cost, or
how overshoot looks in motion. Synthetic scenes only.

### Adaptive switching between interpolation and extrapolation

Switching changes how late the real frames are shown, so each switch costs one uneven frame. The design
the data supports: extrapolate while the player gives input, interpolate after about two seconds without
input (cutscenes, menus), return at once on the first input. The run-time prediction error is a
monitor for content the extrapolator cannot follow, not a per-frame switch. Not implemented.

### Why it is not in the engine yet

It needs a warp pass (the existing `FlowRefine` / `FlowFill` output is already current-to-previous
motion), a batch whose generated frames come after the real one instead of before it, cancellation of
predicted frames when the next real frame arrives early, and its own budget rules. `EngineController`
schedules every generated frame before its real frame today. Shaders are compiled at build time, so a
shader that does not compile breaks the whole build. Without a Windows build to iterate on, this would
be several hundred untestable lines in the pacing code. It is the next step once a build exists.

## Stable 60, 90 and 120 on a 180 Hz panel

- 60: every frame for three refreshes, or with variable refresh one refresh per frame at 60 Hz. No
  generation, lowest latency.
- 120: 2X. 180 / 120 is not a whole number, so without variable refresh frames alternate between one and
  two refreshes (judder). With variable refresh the panel runs at 120 Hz.
- 90 from a 60 fps stream is 1.5X: two of three shown frames generated at phases 2/3 and 4/3, and every
  second real frame never shown. PushFPS generates whole multiples per pair (`PushAdaptiveFg.h` says why),
  so this does not exist, and it would show fewer real frames than 2X. 90 is a natural target for a
  30 fps game (3X), 120 for 30 (4X), 40 (3X) and 60 (2X).
- 180: 3X from 60, one frame per refresh without variable refresh.

Microsoft's guidance for the lowest presentation latency is a flip-model swap chain with the waitable
object or tearing allowed (https://learn.microsoft.com/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model);
NVIDIA's adds that flip model enables overlay planes with fullscreen-like latency in a window
(https://developer.nvidia.com/blog/advanced-api-performance-swap-chains/). PushFPS already uses flip
discard, the waitable object and optional tearing. What to verify on the machine is that PresentMon
reports `Hardware: Independent Flip` (or `Hardware Composed: Independent Flip`) for PushFPS and not
`Composed: Flip`; the analysis script prints it.

## RTX 5070 specifics

- One hardware decoder, sixth generation, with twice the H.264 decode throughput of the previous
  generation (https://developer.nvidia.com/video-encode-decode-support-matrix,
  https://blogs.nvidia.com/blog/generative-ai-studio-ces-geforce-rtx-50-series/). A 1080p60 stream uses a
  small fraction of it. PushFPS reaches it through D3D12VA; nothing Blackwell-specific is left unused on
  the decode side for a 4:2:0 stream.
- Flip metering and the transformer frame generation model are reached through the DLSS backend only.
- Reflex 2 Frame Warp has no interface for a video application.

So there is no 5070-specific code in this branch. The card-specific work is measurement: which frame
generation backend holds its deadline at 2X and 3X on this card, with `VEYRA_EXP_RP_ADAPTIVE_PHASE`.

## Network and input transmission

Read in the pinned Chiaki library: a video frame is handed over when its last unit arrives or forward
error correction can complete it; controller state changes wake the sender thread at once (the minimum
interval in the source is a TODO, not a limit); button edges travel as history events that are resent.
The receive buffer is 100 kB. On a wired LAN nothing here waits. Not changed.

## Keyboard and mouse (`VEYRA_EXP_RP_KBM`)

`KeyboardMousePad.h` (portable, unit-tested) and `RemotePlayKeyboardMouse.h` (Win32 capture, modelled on
the existing PC-streaming capture).

- Default layout: WASD left stick, Alt walk, Space Cross, C / Ctrl Circle, R Square, F Triangle, Q L1,
  E R1, Shift L3, V R3, arrows D-pad, Tab touchpad click, Esc Options, Backspace Create, Home PS; left
  mouse R2, right mouse L2, middle R3, side buttons L1 / R1, wheel Triangle.
  `VEYRA_EXP_RP_KBM_BINDS="F=Square;Mouse2=None"` overrides.
- Stick aiming follows GIMX (https://blog.gimx.fr/subposition-precision-11-translation/) and PCSX2
  pull request 7885: tilt = dead zone + (1 - dead zone) x (speed / full speed)^(1/curve). Movement faster
  than a full tilt is carried into the next polls (40 ms at most). `_DEADZONE`, `_CURVE` and `_FULL` are
  per game and must be tuned on the console; the defaults (0.12, 1, 4000 counts/s) are a starting point.
  A stick caps the turning speed: a mouse cannot turn faster than the game lets a stick turn.
- Gyro aiming sends the movement as angular velocity of the pad. In a game with motion aiming this is
  proportional and has no cap; XIM documents the same approach as "Motion Aim"
  (https://guide.xim.tech/Aim-Settings/). The axis signs are untested: `_INVERT_X` / `_INVERT_Y`.
- While captured the keyboard does not reach PushFPS's own interface. Ctrl+Alt+Shift+Z releases it
  until the window is left and entered again.
- Many online games forbid adapters that present a mouse as a controller. That is the player's
  responsibility.

## Capture card or Remote Play

| | Capture card | Remote Play |
|---|---|---|
| Picture | uncompressed, up to the card's 4K / 120 / HDR | H.264 / H.265, at most 1080p60 |
| Added before the PC | the card (USB cards: tens of ms in published preview tests, varying with card and software) | console encoder and network |
| PushFPS, frame generation off | about 2 ms from callback to present on an RTX 5070 (upstream's note in `LivePairLatency.h`) | a few ms: decode on the GPU plus the same path |
| Frame generation delay | learned phase | whole interval, or learned phase with the experiment |
| Pad | straight to the console | through the PC |

Neither path's total has been measured here. A card avoids the encoder but adds its own buffering, and
which is lower depends on the card. `latency-ab.ps1` and a camera measure both the same way.

## Benchmarks for the real machine

1. Build (GitHub Actions, `build-windows.yml`, on a fork), install.
2. `scripts/remoteplay/latency-ab.ps1 -Exe <PushFPS.exe> -Arm original`, then `-Arm new`, then
   `-Arm new -Experiment AdaptivePhase` (frame generation on for that pair of runs), alternating, six
   runs of the 20-press test each.
3. `python tools/remoteplay_latency_bench/analyze.py <PushFPS>\logs\bench`. It compares means with a
   bootstrap interval. With a simulated 4 ms saving, 16.7 ms frame quantisation and 4 ms of jitter, 120
   presses per configuration found the saving in 98 % of simulated experiments, 60 in 68 %, 20 in 45 %.
4. Frame pacing: capture PushFPS with PresentMon for 60 s at 60, 120 (2X) and 180 (3X), variable refresh
   on and off; `analyze.py --presentmon vrr120=file.csv ...` reports rate, spread, long frames and the
   present mode.
5. Button to screen including pad and panel: 240 fps phone camera on the pad and the screen, 20 presses
   per configuration, count frames (4.2 ms each).
6. Mouse: one 360-degree turn at a known mouse distance per game to tune `_FULL` and `_CURVE`.

## What was verified in this pass

- Linux, GCC 13: `CoreTests.cpp` 97/97 (9 new for mouse aim, key mapping and their path through the
  controller queue). The extrapolation study and the analysis script ran; the script was checked against
  synthetic results with a known difference.
- mingw-w64 GCC 13 `-fsyntax-only` with Qt 6.4, SDL 3.2 and FFmpeg 9.0.1 headers: `bridge/RemotePlay.cpp`
  (including the first commit's pad timer, unchecked until now), `bridge/PushLatencyTest.cpp`,
  `EngineController.cpp` and the remote play sources produce the same diagnostics as the original
  sources (all in mingw's own headers or mingw-versus-MSVC differences) and none new. The project builds
  with MSVC and Qt 6.8.3; this is not that.
- Not parsed: `latency-ab.ps1` (no PowerShell here).
- Not run: anything on Windows, a GPU or a console.
