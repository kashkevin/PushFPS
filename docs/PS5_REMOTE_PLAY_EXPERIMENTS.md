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

## Settings file and performance modes (`slipstream.ini`, added 2026-11-18)

`slipstream.ini` beside `PushFPS.exe` is read once at start-up (`veyra/ui/SlipstreamSettingsFile.h`,
parser `veyra/engine/SlipstreamSettings.h`). No file: nothing changes. Keys named `VEYRA_EXP_RP_*` or
`VEYRA_TEST_RP_*` are put into the process environment unless the environment already has them; no
other variable can be set from the file. `mode=` and `lan=` expand into switches through
`veyra/engine/PushPerformanceMode.h`: Ultra Low Latency and Competitive ask for
`VEYRA_EXP_RP_PAD_POLL_MS=2`, every mode but Ultra Low Latency for `VEYRA_EXP_RP_ADAPTIVE_PHASE=1`,
and `lan=1` for `VEYRA_EXP_RP_LAN_PROFILE=1` (read only by the network work, not on this branch).
A malformed file applies nothing and logs one `[remoteplay-settings]` warning with the line number.
User documentation: `docs/SETUP.md`.

- `VEYRA_TEST_RP_NO_SETTINGS_FILE=1` (environment only): the file is not read. The hardware test kit
  (`scripts/remoteplay/testkit/run-tests.ps1`) sets it so each arm runs with exactly its own switches.
- The modes are a table and a controller in a header with tests. The engine does not read a mode:
  frame generation, its multiplier and variable refresh are still chosen in the interface, and
  `PushAutomaticMode` is not called by anything. Unverified on Windows.
## Keyboard and mouse, second round (`VEYRA_EXP_RP_KBM_*`)

All of this is inside `VEYRA_EXP_RP_KBM`; with that unset nothing here runs. The user guide is
`docs/KEYBOARD_MOUSE.md`. Unverified on Windows.

| Variable | Effect |
|---|---|
| `VEYRA_EXP_RP_KBM_PROFILE=<path>` | a plain `key=value` text file with the walking, aiming and driving profiles (sensitivity, curve, dead zone, ramps, keys). Read once at the first session of a run. Lines that cannot be read are logged by number and ignored. Unset: the built-in profiles. |
| `VEYRA_EXP_RP_KBM_RAW_DRAIN=0` | read one mouse report per `WM_INPUT` message, as in the first round. Default: after the message's own report, `GetRawInputBuffer` takes every report still queued (Microsoft's buffered read), so a 1000 Hz or 8000 Hz mouse cannot fall behind. A buffered read that fails eight times in a row switches itself off. |

Changed against the first round, with `VEYRA_EXP_RP_KBM` set and nothing else: the default keys are now the
built-in GTA-style layout of `include/veyra/remoteplay/KbmProfile.h` (an untuned guess; the layout listed
above is the single-profile one the unit tests still use), the smoothing window is 33 ms instead of 8 ms,
stick output is in whole 8-bit steps with the remainder carried, Caps Lock switches between the walking
and the driving profile, and holding the aim button uses the aiming profile. The first round's number
variables (`_SENS`, `_FULL`, ...) still work and apply to all three profiles; the profile file wins over them.

When a capture ends the log has one `[remoteplay-kbm] keyboard and mouse released (<reason>); raw input:
messages=… reports=… largestBacklog=…` line: `largestBacklog` above 1 means reports had queued behind one
message.
## Same-LAN transport profile (branch `exp/lan-profile`)

**Unverified on Windows and on a PS5.** Linux unit tests and cross-compiler syntax checks only. Nothing
here was measured; no latency figure is claimed. With none of these variables set, no new library
function is called and the stream is handled exactly as before.

| Variable | Effect |
|---|---|
| `VEYRA_EXP_RP_LAN_PROFILE=1` | the library waits 2000 us (not 16000) for a missing video packet before giving up on it, and asks for a 1 MiB socket receive buffer (not 100 KiB). The receive window advertised to the console stays 0x19000 |
| `VEYRA_EXP_RP_REORDER_US=500..16000` | with the profile: another wait. Outside the range, or without the profile: ignored and said so in the log |
| `VEYRA_EXP_RP_RCVBUF=102400..8388608` | with the profile: another receive buffer size in bytes. Same rule |
| `VEYRA_EXP_RP_THREAD_PRIORITY=1` | Chiaki's Takion and feedback sender threads and the session owner thread run at `THREAD_PRIORITY_ABOVE_NORMAL`. Nothing else |
| `VEYRA_EXP_RP_TIMER_1MS=1` | `timeBeginPeriod(1)` while the session owner runs, `timeEndPeriod(1)` when it ends. Nothing else |

A value of `0` or an empty value is off. No switch implies another: the LAN profile sets neither the
thread priority nor the timer resolution, and the thread priority switch no longer raises the timer
resolution (they were one switch at first; split so that an A/B run can say which of the two had an
effect). The timer resolution is system-wide while it is held, and the app or another program may have
raised it already, so check `timer1ms=` in the `effective` line and compare runs on an otherwise idle PC.

The log shows `[remoteplay-lan] experiment requested: ...` when
a session starts and `[remoteplay-lan] effective ...` once the console has connected, with the wait and
buffer the library took (`getsockopt` for the buffer) and how many threads were raised. A receive
buffer size the system refuses does not end the connection: the library sets its own 100 KiB instead,
as it would without the profile, warns once in its log and counts it (`receiveBufferFallbacks=` in the
`effective` line; `receiveBufferEffective` then shows what is really in use).

How it reaches the library: chiaki patch `0004-chiaki-transport-tuning.patch` adds
`chiaki_veyra_transport_set_tuning()` and counters. Zero means the library's own value, and the patch
is never called without a switch. Thread priority uses the library's existing per-thread hook
(`chiaki_thread_set_affinity_cb`), installed only when the switch is on.

Needs a real PS5 before enabling by default:

- **Reorder wait.** Nothing new is sent, but a packet that arrives more than the wait after its
  successor is now treated as lost. The frame then goes to FEC, and when FEC cannot complete it the
  library reports a corrupt frame and asks for a keyframe, exactly as it does today after 16 ms. On a
  wired LAN reordering beyond 2 ms should be rare; on Wi-Fi it may not be. Compare `reorderTimeouts`,
  `fecFailed` and `keyframeRequests` with the profile on and off before trusting it.
- **Thread priority.** Changes when feedback packets leave, not what they contain.
- **Timer resolution.** Changes how precisely timed waits in the process end (the owner's wait for the
  next pad tick among them), so it can change when feedback packets leave, not what they contain.
  Unmeasured.

### Connection health (`VEYRA_EXP_RP_HEALTH=1`)

Observation only: nothing sent to the console changes. Every five seconds the session owner writes one
line and keeps the same values for `RemotePlaySessionSource::connectionHealth()`:

```
[remoteplay-health] grade=stable interval=stable seconds=5.0 lossPercent=0.000 lost=0 received=10412
  frameJitterMs=0.31 frameJitterPeakMs=1.20 reordered=0 late=0 reorderTimeouts=0 skipped=0 fecRecovered=0
  fecFailed=0 keyframeRequests=0 decoderResets=0 frameDrops=0 ingressDepth=0 ingressPeak=1
  mailboxOverwrites=0 mbps=14.80 fps=59.9 rtt=not-measured route=direct-lan link=ethernet linkMbps=1000
```

(one line in the log; the numbers above are an illustration of the format, not a measurement.)

| Field | Meaning and limit |
|---|---|
| `lossPercent`, `lost`, `received` | the library's own packet accounting, summed over its 200 ms congestion windows, before the 5 % cap on what the console is told |
| `frameJitterMs`, `frameJitterPeakMs` | RFC 3550 interarrival jitter (J += (\|D\| - J) / 16) per complete video frame against the nominal frame rate, and the largest \|D\| of the last 256 to 512 frames. Takion packets carry no sender timestamp, so this is per frame and includes the console's encoder pacing and frame assembly |
| `reordered`, `late`, `reorderTimeouts`, `skipped` | gaps that closed in time, packets the reorder queue refused, waits that ran out, packets given up |
| `fecRecovered`, `fecFailed`, `keyframeRequests`, `decoderResets` | recovery events: frames completed by FEC, frames FEC could not complete, keyframe requests sent, ingress recoveries (each resets the decoder) |
| `frameDrops` | frames the library reported lost plus complete frames discarded while waiting for a keyframe. The second part also counts a slow decoder, not only the network |
| `ingressDepth`, `ingressPeak`, `mailboxOverwrites` | compressed frames waiting now and at most in the interval; decoded frames replaced before the engine took them |
| `rtt` | only what the library measured while connecting (Senkusha); `not-measured` when it did not. There is no continuous round-trip measurement |
| `route`, `link`, `linkMbps` | `direct-lan` = the console's address is private (RFC 1918 or link-local) and on a subnet of one of this PC's adapters; link type and speed are that adapter's (`GetAdaptersAddresses`). A host name is not resolved (`unknown-host-name`). Virtual switches report as Ethernet, and the console's own link (cable or Wi-Fi) cannot be seen from here |
| `grade` | `stable` / `unsteady` / `unstable`: falls as soon as one interval is worse, climbs one step after two better intervals in a row, survives a reconnect. The thresholds (0.1 % and 1 % loss, 4 and 10 ms jitter, any decoder reset or reorder timeout) are provisional and were not calibrated on a PS5 |

One warning per session when this PC's route to the console is Wi-Fi.

### Bounded decode backlog (`VEYRA_EXP_RP_BACKLOG=1`)

Today a fifth compressed frame arriving while four wait drops all of them and asks the console for a
keyframe, so a short burst (frames released together after a brief network or scheduling stall) costs
a recovery. With the switch the queue may hold up to 16 frames and the decoder works through them.
Two ceilings keep that from becoming delay: no frame may wait longer than 100 ms (the existing rule),
and the queue must be back below four within 100 ms of first exceeding four. Past either, the
original path runs: drop everything, ask for a keyframe. (Below four, not at four: a first version
ended the episode at four, so a decoder exactly as fast as the stream, with the depth going 5, 4, 5...,
got a new 100 ms on every frame and kept four frame times of delay for good. Found in review, fixed,
test `backlog_at_exactly_the_limit_does_not_restart_the_allowance`.) Frame gaps, lost references, decoder errors and
the byte limit recover exactly as before. `[remoteplay-lan] attempt=... recoveries=... backlogEpisodes=...`
at the end of each connection attempt (written when any switch of this section is set) gives the
numbers to compare.

**Finding to validate on hardware: a clock value read too early (present before this branch).** The
owner reads the clock and then takes the queue's lock (`RemotePlaySource::read`). A frame the Takion
thread pushes in between has an arrival time later than the owner's value, and the age rule
(`VideoIngress::expiredLocked`, `now < arrival`) treats that as expired: the queue is dropped and a
keyframe requested for a frame that had just arrived. A two-thread stress model in review showed such
recoveries with every switch off. How often it happens in a real session is unknown (the window is
the few instructions between the clock read and the lock); look for `recoveries` rising in the
`[remoteplay-lan] attempt=...` line of a session without loss. **The default is not changed.** With
`VEYRA_EXP_RP_BACKLOG=1` both age checks ignore a clock value older than what it is compared with (the
frame's arrival, the start of the backlog episode) and decide at the next look; the first version of
the backlog check had the same fault (review S2: the owner's `tryPop` dropped a backlog whose fifth
frame arrived after its clock read). So an A/B of the switch compares two things at once, the backlog
and this tolerance. Tests `backlog_stale_consumer_clock_is_not_an_overdue_backlog`,
`stale_consumer_clock_and_the_age_rule` (the second pins the default's behaviour too).

Needs a real PS5 before enabling by default: it changes how often a keyframe is requested (fewer
requests in bursts), and whether the decoder really catches up within 100 ms on the RTX 5070 has not
been measured. Decoded frames replace each other in the single presentation slot, so catching up shows
as skipped pictures, not as added delay; that too is by design and unmeasured.

### Stalls: what the queues do afterwards (Linux model)

`tests/remoteplay/LanTransportTests.inc` runs the queueing rules through a simulated 200 ms and 2 s
stall (1 ms steps, 60 fps stream, 2 ms per decoded frame, a keyframe 35 ms after it is asked for).
These are properties of the rules in a model, not timings of the application:

| Case | Backlog off (default) | `VEYRA_EXP_RP_BACKLOG=1` |
|---|---|---|
| network releases 12 frames at once (200 ms) | queue dropped, one keyframe request, picture resumes when the keyframe arrives (51 ms in the model, of which 35 ms is the assumed console answer) | all 12 decoded, no request, longest wait 23 ms, queue empty again 26 ms later |
| network releases 120 frames at once (2 s) | same as above | same as off: past 16 frames the original path runs |
| owner (decoder) does not run for 200 ms or 2 s | frames older than 100 ms are dropped, one keyframe request | same |
| owner does not run for 90 ms | fifth waiting frame drops the queue | six frames decoded, none older than 100 ms, delay gone within 20 ms |

In every case no frame waits longer than 100 ms, and two seconds after the stall frames wait exactly as
long as before it. Controller queue: with the default immediate policy everything pending is sent when
the owner runs again, nothing older than 100 ms is sent (older entries become one release), and the
delay afterwards is zero. With `VEYRA_TEST_RP_LEGACY_INPUT` (the original one state per 4 ms tick) the
queue stays bounded at 16 entries but keeps what it gathered: 20 ms of standing delay in the model.
That is the original behaviour, kept only for A/B runs.

### Keyframe retry (`VEYRA_EXP_RP_IDR_RETRY_MS=250..1000`)

When no frame has been decoded for 1000 ms the owner flushes the decoder and asks the console for a
keyframe; a second request follows 2000 ms later and a reconnect after 6000 ms of silence. The switch
moves only the first step, to between 250 and 1000 ms (values below 250 become 250, above 1000 stay
1000, so it can never be later than today).

The queue has its own limit of one keyframe request per 500 ms, and that limit stays. Below 500 the
switch only lets the first repeat request of an outage through after N ms (it would otherwise be held
until 500 ms after the request that opened the outage); every later request keeps the 500 ms spacing.
A request opens an outage only when none was sent in the second before it, so a link that keeps
breaking cannot earn the early repeat again and again: no stretch of time holds more than one request
above what the 500 ms spacing alone allows (a one-minute outage: 121 requests instead of 120, in the
model). The first version lowered the spacing of every request to N, which doubled the sustained rate
at 250; found in review. `earlyKeyframeRequests=` in the `[remoteplay-lan] attempt=...` line counts the
requests that used the allowance.

**The console may throttle a client that asks for keyframes often; validate on a real PS5.** Also
unmeasured: a keyframe is several times the size of a normal frame, and the flush discards frames that
were merely late, so a low value on a link with pauses of a few hundred milliseconds could cause the
outages it is meant to shorten. Default unchanged.

### After the independent review (Linux only)

What the review of this branch changed, each with a test in `tests/remoteplay/LanTransportTests.inc`:
the backlog episode ends below four frames, not at four; a clock value read before a push is not an
overdue backlog (and, with the backlog switch only, not an expired frame); the timer resolution is its
own switch; a refused tuned receive buffer falls back instead of failing the connection; the keyframe
retry moves only the first repeat request; tests for two code changes nothing had noticed (the overdue
check on the producer's side, what a new session resets).

Two-thread runs (`ingress_two_threads_*`): a producer and a consumer thread on one queue, 20000 frames
each, a clock the consumer reads before it looks, no sleeps, bounded loops, under ThreadSanitizer and
AddressSanitizer in `verify.sh`. With the switch off and on: order, bounds and the accounting (every
frame decoded or counted as dropped). With the switch on and nothing lost, late or in excess: no
recovery at all. That last property does not hold for the default, by the finding described under the
backlog switch; the runs with the switch off allow those recoveries and check everything else. This is
a model of the queue, not of the application: the real clock, the real threads and their priorities
on Windows were not involved.
### Adaptive reorder wait (`VEYRA_EXP_RP_REORDER_ADAPTIVE=1`, branch `exp/w1-adaptive`)

**Unverified on Windows and on a PS5.** Off by default; without the variable the new library function is
never called. Its own switch: it sets no receive buffer and implies no other switch.

The library holds every later video packet while one is missing: 16 ms upstream, 2 ms with the LAN profile.
With this switch the wait starts at whatever the session is configured with (16 ms alone, 2 ms together
with `VEYRA_EXP_RP_LAN_PROFILE=1`) and then moves among 2, 3, 4, 6, 8, 12 and 16 ms from what the
library now counts (`include/veyra/remoteplay/ReorderWindow.h`, owner thread, about four times a second):

| Evidence | Counter (chiaki patch 0004) | Use |
|---|---|---|
| how long after its wait began each out-of-order packet arrived, in time or too late | nine buckets, `<=1 ... <=16 ms` and above | the 99th percentile, newest minute weighing most (half-life 15 s), is the wait the link needs |
| packets that arrived after they had been given up on | `REORDER_LATE_ARRIVALS` (the last four timeouts are remembered) | the wait was too short |
| frames FEC could not complete | `FRAMES_FEC_FAILED` | what a wait that is too short costs |
| waits that ran out and nothing came | `REORDER_TIMEOUTS` minus the late arrivals | a longer wait buys nothing: the period before narrowing drops from 30 s to 10 s |

Rules, all provisional numbers:

- **Widen** when two packets arrive later than the window within 10 s *and a frame was lost in the same
  10 s*, to the step that covers the 99th percentile, at most three steps per change and 1 s between
  changes. Late packets alone do not widen. (The first version widened on late packets alone. In the model
  below that raised the delay of held frames and saved no frame whenever FEC completed the frames anyway:
  giving a late packet up early and letting FEC finish is then the faster hand-over.)
- **A widening must pay.** One period (30 s) after it, frames must be lost at less than half the rate of
  before; otherwise it is rolled back to where it started and widening is blocked for 1 minute, then 2, 4,
  up to 16. The block ends at once if frames are then lost clearly faster than they were with the longer
  wait (more than twice that rate plus one frame per 5 s): the rollback was wrong.
- **Narrow** one step after a period in which at most 1 % of out-of-order packets needed more than the
  next lower step; a narrowing that had to be undone doubles the next period (up to 16 times), so a link
  that sits between two steps settles on the wider one.
- A link without reordering produces no evidence and the wait is never changed at the floor. Packets
  later than 16 ms are not chased. A reconnect keeps the window.
- Counters that contradict each other (one going back, more timeouts than waits, more late arrivals than
  packets given up) or a library that reports another wait than the one set send the wait to the
  library's own 16 ms for a minute; the third time for the rest of the session.

Log: `[remoteplay-lan] reorder wait 2000 -> 6000 us (late packets arrived after the wait); needUs=... gaps=...
timeouts=... skipped=... lateArrivals=...` on every change, and `attempt=... reorder wait at the end=...` when a
connection attempt ends.

Patch 0004 grew by: `chiaki_veyra_transport_set_av_reorder_timeout_live()` (one atomic; the Takion thread
takes the value the next time it looks at its video queue, never above 16000 us; cleared by
`chiaki_veyra_transport_set_tuning()`), the counters above and `REORDER_OVERSHOOT_US` (how much longer than
asked a wait that ran out lasted on the Takion thread). The existing setter was not a run-time setter: it
only applied to connections made afterwards. Patches 0001 to 0004 apply in order on the pin
(`git apply --check`), and the patched `takion.c` parses with gcc and the mingw cross-compiler; it was not
compiled by MSVC or linked.

`tools/remoteplay_latency_bench/reorder_library_check.sh <patched chiaki tree>` builds the library's own
queue code into a virtual-time harness and compares it with the model used by the tests and the simulator
(`include/veyra/remoteplay/ReorderQueueModel.h`): 60 generated traces (reordering, loss, bursts longer than
the queue, duplicates, live changes of the wait, the index wrap), 3.4 million released packets, every
packet, time and counter equal. Linux, and a check of the model against the C source only.

**Finding in the pinned library (not changed):** `chiaki_reorder_queue_push` drops video packet index 0
when it arrives while a gap is open across the 65535 to 0 wrap (the queue's "equal" compares 64-bit values
that were added without wrapping; `lib/src/reorderqueue.c:9,37,82-94`). The packet then counts as lost
and the reorder wait runs out for it. It needs a missing packet at the moment the index wraps (about every
16 s at 4000 packets a second), so it is rare on a clean link; with every switch off the library is
unpatched there and behaves as upstream. Test `reorder_model_keeps_the_library_behaviour_at_the_index_wrap`
pins what the model copies.

Needs a real PS5 before enabling by default: whether late packets occur at all on the wired LAN (then this
is the LAN profile's 2 ms and nothing more), the provisional numbers above, that the Takion thread really
takes a changed wait in the middle of a session, and how `fecFailed` and `keyframeRequests` compare with the
fixed 2 ms and 16 ms. When a late packet is given up changes when a corrupt-frame report or keyframe
request is sent; what is sent does not change.

### Reorder wait under made-up links (Linux model, `tools/remoteplay_latency_bench/reorder_sim.cpp`)

**Linux model. No PS5, no network and no Windows machine was involved; nothing here predicts what a real
console or LAN does.** The simulator replays packet arrival traces through the library's reorder-queue rule
(the model that `reorder_library_check.sh` compares with the C source) and through
`ReorderWindowController`, and compares the rules with each other on the same arrivals. Assumptions, all
invented: 59.94 frames a second, 24 source and 3 FEC packets per frame sent over 2 ms, a frame handed to
the decoder when 24 of its packets have left the queue and given up when the next frame begins. Raw
output: `tools/remoteplay_latency_bench/reorder_sim_results.json` (5 seeds of 120 s per link,
35960 frames per cell; `reorder_sim` prints the same bytes again on Linux with g++).

- `clean_lan`: queueing delay 0.05 ms mean; no loss, no reordering.
- `sustained_light_jitter`: queueing delay 0.4 ms mean (cap 3 ms); loss 0.01 %; 0.05 % of packets 0.2-1.5 ms late.
- `temporary_congestion`: as clean_lan, but for the middle sixth of the run: queueing delay 3 ms mean (cap 20 ms), loss 1 %, 2 % of packets 1-10 ms late.
- `pure_reordering`: no loss; 2 % of packets 0.2-5 ms late.
- `burst_loss`: no reordering; loss bursts of 12 packets on average, about one every 5 s.
- `mixed`: queueing delay 0.3 ms mean (cap 3 ms); loss 0.3 %; bursts of 10 packets about every 12 s; 1 % of packets 0.2-6 ms late.

"Held": handed over at least 0.5 ms later than the moment 24 of the frame's packets had reached the
socket. "Given up too early": not handed over although 24 of its packets did arrive. "Keyframe requests":
one per frame not handed over (the library asks on every frame FEC cannot complete), in brackets merged
within 500 ms, PushFPS's own spacing. *Late packets alone* is the first version of the rule (widen whenever
packets arrive after the wait), kept in the simulator for comparison; the app does not use it.

| Link | Rule | Frames held | Held delay ms, median / P95 / P99 | Given up too early | Lost by the link | Keyframe requests | Wait changes |
|---|---|---|---|---|---|---|---|
| `clean_lan` | fixed 2 ms | 0 | 0.00 / 0.00 / 0.00 | 0 | 0 | 0 (0) |  |
|  | fixed 16 ms | 0 | 0.00 / 0.00 / 0.00 | 0 | 0 | 0 (0) |  |
|  | adaptive, from 2 ms | 0 | 0.00 / 0.00 / 0.00 | 0 | 0 | 0 (0) | 0 |
|  | adaptive, from 16 ms | 0 | 0.00 / 0.00 / 0.00 | 0 | 0 | 0 (0) | 15 |
|  | *late packets alone* | 0 | 0.00 / 0.00 / 0.00 | 0 | 0 | 0 (0) | 0 |
| `sustained_light_jitter` | fixed 2 ms | 97 | 2.02 / 2.30 / 2.98 | 0 | 0 | 0 (0) |  |
|  | fixed 16 ms | 125 | 15.04 / 16.70 / 16.87 | 0 | 0 | 0 (0) |  |
|  | adaptive, from 2 ms | 97 | 2.02 / 2.30 / 2.98 | 0 | 0 | 0 (0) | 0 |
|  | adaptive, from 16 ms | 116 | 11.02 / 16.18 / 16.43 | 0 | 0 | 0 (0) | 20 |
|  | *late packets alone* | 97 | 2.02 / 2.30 / 2.98 | 0 | 0 | 0 (0) | 0 |
| `temporary_congestion` | fixed 2 ms | 723 | 2.02 / 4.01 / 7.05 | 1130 | 0 | 1130 (175) |  |
|  | fixed 16 ms | 1952 | 13.52 / 27.94 / 32.98 | 5 | 0 | 5 (5) |  |
|  | adaptive, from 2 ms | 1715 | 12.02 / 25.53 / 32.70 | 109 | 0 | 109 (32) | 26 |
|  | adaptive, from 16 ms | 1605 | 10.50 / 20.59 / 25.08 | 24 | 0 | 24 (20) | 15 |
|  | *late packets alone* | 1775 | 12.03 / 25.19 / 32.70 | 49 | 0 | 49 (21) | 30 |
| `pure_reordering` | fixed 2 ms | 10661 | 1.51 / 3.02 / 3.86 | 0 | 0 | 0 (0) |  |
|  | fixed 16 ms | 10701 | 2.41 / 4.17 / 4.51 | 0 | 0 | 0 (0) |  |
|  | adaptive, from 2 ms | 10661 | 1.51 / 3.02 / 3.86 | 0 | 0 | 0 (0) | 0 |
|  | adaptive, from 16 ms | 10701 | 2.41 / 4.17 / 4.51 | 0 | 0 | 0 (0) | 15 |
|  | *late packets alone* | 10700 | 2.40 / 4.16 / 4.51 | 0 | 0 | 0 (0) | 5 |
| `burst_loss` | fixed 2 ms | 33 | 1.15 / 2.16 / 2.26 | 0 | 152 | 152 (114) |  |
|  | fixed 16 ms | 36 | 14.83 / 16.01 / 16.03 | 0 | 152 | 152 (114) |  |
|  | adaptive, from 2 ms | 33 | 1.15 / 2.16 / 2.26 | 0 | 152 | 152 (114) | 0 |
|  | adaptive, from 16 ms | 36 | 11.00 / 15.74 / 15.80 | 0 | 152 | 152 (114) | 20 |
|  | *late packets alone* | 33 | 1.15 / 2.16 / 2.26 | 0 | 152 | 152 (114) | 0 |
| `mixed` | fixed 2 ms | 6792 | 1.90 / 3.01 / 4.04 | 0 | 67 | 67 (51) |  |
|  | fixed 16 ms | 7763 | 3.46 / 16.49 / 30.65 | 0 | 67 | 67 (51) |  |
|  | adaptive, from 2 ms | 6792 | 1.90 / 3.01 / 4.04 | 0 | 67 | 67 (51) | 0 |
|  | adaptive, from 16 ms | 7528 | 3.46 / 12.01 / 16.10 | 0 | 67 | 67 (51) | 15 |
|  | *late packets alone* | 7494 | 3.44 / 6.29 / 10.17 | 0 | 67 | 67 (51) | 5 |

What the model shows, and no more than that:

- Where FEC completes the frames (`pure_reordering`, `mixed`), giving a late
  packet up after 2 ms loses no frame and hands frames over sooner than waiting for it. Widening on late
  packets alone made those links slower for nothing; that is why the rule now also wants frames to be
  lost (0.5 a second or more) before it widens. With that, the adaptive rule started at 2 ms is the fixed
  2 ms on every link here except one.
- `temporary_congestion` is that one: the fixed 2 ms gives up 1130 frames that 16 ms would have completed,
  the adaptive rule 109 (those before it has widened), and it is back at 2 ms after the congestion.
  While it is wide, frames wait as long as with the fixed 16 ms.
- A loss burst costs the same frames under every rule; only how long the frames behind it are held
  differs (about 1 ms against about 15 ms at the median).
- Started at 16 ms (the switch without the LAN profile) the rule comes down one step per 30 s and is not
  at 2 ms within these 120 s runs.

Not modelled: the console's real packet pacing and FEC ratio, its answer to a keyframe request, the
Takion thread's scheduling, Wi-Fi. A real PS5 session with `VEYRA_EXP_RP_HEALTH=1` is what can say which
of these links, if any, resembles the user's.

## Duplicate removal for hardware-decoded pictures (`VEYRA_EXP_RP_STREAM_DEDUP=1`)

Off unless set to exactly `1`; unset, empty, `0` and any other value leave it off. Unverified on
Windows: the shader has never been compiled and the D3D12 and engine code has never been built or
run.

Not tested at all, not even on paper streams: 10-bit and HDR input (P010). The pass reads such a
plane as 16-bit values scaled to 8-bit steps, and the classifier's thresholds were chosen on 8-bit
SDR streams only; PQ code values of an HDR picture move differently. Treat HDR as unsupported until
it has been looked at on the PC. Pictures larger than 4096x2304 are not measured (the 32-bit sums of
the pass would not hold them); their frames are kept, as without the switch.

A 30 fps game arrives in the 59.94 fps stream as A A B B. With D3D12VA decoding the engine had no
picture number for a frame, so duplicate removal (content rate Auto, which the quick setup turns on
with frame generation) kept every frame and frame generation interpolated A-A pairs. With the switch:

1. `EnhanceGraph::measureStreamChange` runs `shaders/PushStreamChange.hlsl` on the decoder's own
   texture, against the previous picture: the mean of every 16x16 luma block (edge blocks included),
   and every luma sample (the pass keeps the previous luma plane in a texture of its own). 48 bytes
   are read back: largest block difference, mean difference, share of changed blocks, largest
   sample difference, number of samples that moved by more than 4 steps, and (since the second
   review) the sums of the samples that brightened and darkened, the blocks touched at all and the
   blocks that changed one way.
2. `engine/PushStreamDedup.h` gives the measurement to `remoteplay/StreamCadence.h`, which looks for
   the high-low rhythm of a doubled stream in the block numbers.
3. Only while that rhythm is trusted does a frame get a picture number; a repeat gets the number of
   the frame before it and `ContentDedup` drops it before any other GPU work. A repeat must stand in
   the repeat's place in the rhythm and look like the same picture coded again sample by sample:
   no luma sample moved by more than 16 steps (`pixelCeiling`) and far fewer samples moved than in
   the picture before it; it changed one way in far fewer blocks than the picture before it, did
   not move the picture as a whole, and stays near what this stream's confirmed repeats changed.
   Pictures in the repeat's place that turn out new count against the rhythm: three within 16
   pictures give it up (see "Second review" below).
   Since the third review (below) a repeat is dropped only when its decoded luma is the previous
   picture exactly, after the stream has shown such exact repeats; a refined repeat is kept.

No number means the frame is kept, exactly as without the switch. That is the answer for a 60 fps
game (no rhythm, however little its picture changes), for a GPU error or timeout, after a seek,
reset, lost or unread picture, for a likely repeat without enough confidence, and for a picture in
which any luma sample moved by more than 16 steps, however few. The switch does nothing while the content rate is
`Transport` (duplicate removal off) or frame generation is off.

After an independent review (18 November 2026). The first version called a picture a repeat when no
block mean moved by 0.6 of a step, rhythm or not, and judged repeats by block means alone. A block
mean cannot see what moves inside the block: of 600 real 60 fps pictures over a still picture it
dropped 226 (a 6x6 square moving one pixel per frame), 294 (a drifting 2x2 particle) and 301 (a fade
of 0.25 code values per frame). Both are gone: no rhythm, no number; and the samples decide.

Limits that remain, by design of the measurement:

- A repeat is only removed where the encoder's refinement of it stays under 16 steps in every
  sample. On camera footage and photographs under a pan that is most repeats; on hard synthetic
  content (random blocks, a fractal zoom) almost none: the experiment then does nothing.
- A real change of at most 16 steps per sample that falls on the repeat's place of a trusted rhythm
  is not always seen: a small faint thing drawn at 60 fps over a world drawn at 30 from the first
  picture on is still dropped on every second picture (second review, "What still drops" below).
  An object 24 steps from its background is seen (adversarial set).
- Only luma is measured. A change in colour alone is invisible to it.

Cost: one small dispatch and one bounded wait per stream picture, on the engine thread, in
`StreamChangeRunner::measure` (`pipeline/StreamChangePass.h`), and only while frame generation runs
and duplicate removal is active. The pass, its pipeline state and its history textures are created
with the graph when the switch is set (`EnhanceGraph::initStreamChange`), not on a live frame. The
wait ends when the GPU is done, and at the latest at a deadline the engine fixes before it clones
the frame: the smaller of 4 ms and the time until the live scheduler's next wake
(`pipeline::streamChangeDeadline`), so cloning, recording and submitting count against it. The same
thread presents frames at exact times: when a presentation is already due there is no time, and
then nothing is dispatched for that picture at all; it is kept without a number and the rhythm
starts again with the next one. The deadline is a high-resolution waitable timer waited for together
with the fence's event (as `engine/DeadlineWait.h`); a millisecond timeout alone would return on the
scheduler tick, up to 15.6 ms later. The pass queues behind GPU work already submitted, so the wait
is longest when the GPU is busiest; a measurement that misses it is used one frame late for the
rhythm only, and that frame is kept. How long the wait really is on the RTX 5070, and how many
pictures get no measurement because a presentation was due, is not known: `measureWaitMeanMs` /
`measureWaitMaxMs` / `gpuTimeouts` (missed waits and pictures not dispatched) in the log say. If
that count is high the experiment does little: every such picture restarts the rhythm.

Failures are final and logged once: 10-bit / HDR input (refused when the graph is created and again
if a decoder picture is not 8-bit NV12), history textures that cannot be allocated three times in a
row, a removed device. At shutdown the last measurement is waited for up to 2 s on its fence; if it
still runs, its GPU objects and the decoder picture it reads are leaked on purpose, not released
under it.

Log, every 10 s, counts over that window: `[remoteplay-cadence] state=doubled streamFps=59.94
sourceFps=29.97 realPresentedFps=... generatedPresentedFps=... presentedFps=... repeatsFound=...
repeatsRemoved=... keptUnsure=... unmeasured=... unreadOverwritten=... generatedExpired=...
fgGpuBatchP95Ms=... arrivalToPresentMs=... gpuTimeouts=... measureWaitMaxMs=...`.

| Field | What it counts |
|---|---|
| `streamFps` | decoded stream pictures per second (what the console sends) |
| `sourceFps` | of those, pictures not found to be repeats (what the game renders); `sourceFpsNow` is the classifier's own figure over its last 64 pictures |
| `realPresentedFps`, `generatedPresentedFps`, `presentedFps` | frames per second counted at the present call (`FrameFlowWindow`), real game frames and generated frames apart, and their sum. Never the source rate times the multiplier |
| `repeatsFound` / `repeatsRemoved` | repeats the classifier reported / frames `ContentDedup` dropped (it keeps some on a still screen) |
| `keptUnsure`, `unmeasured` | likely repeats kept for lack of evidence; pictures without a GPU measurement (kept) |
| `unreadOverwritten`, `generatedExpired`, `cancelledBeforePresent` | decoded pictures the engine never read; generated frames made but not shown in time; presentations cancelled |
| `fgGpuBatchP95Ms` | GPU time of one frame-generation batch, P95 (processing, not button-to-screen) |
| `arrivalToPresentMs` | decoded picture to return of its present call, mean of the last second (processing, not button-to-screen) |

The same values are in `PlayerSnapshot::streamCadence` (no UI reads it yet).
`VEYRA_VERBOSE_FRAME_LOGS=1` adds one `remoteplay-cadence-frame` line per picture with the raw
measurement: that is the data to re-tune the classifier on a real PS5 stream.

Evidence for the classifier (Linux model: libx264 and libx265 low-delay streams made by
`tools/remoteplay_latency_bench/cadence_streams.py`, 480 pictures each, 600 in the adversarial set;
every stream run through `PushStreamDedup` and the engine's own `ContentDedup` by `cadence_eval.cpp`
and `cadence_report.py`; results in `cadence_results.json`). None of it is a PS5 stream. All sets
were made again after the review, because the measurement changed (every sample, edge blocks).

| Set | Streams | New pictures dropped | Repeats kept (harmless) |
|---|---|---|---|
| legacy: the first study's synthetic scene, 720p | 40 | 0 of 13,755 | 4,809 of 5,445 |
| tune: synthetic scene, camera footage, a photograph under a pan, a fractal; 720p and 1080p; thresholds chosen here | 90 | 0 of 31,251 | 6,334 of 11,949 |
| adversarial: the reviewer's scenes through the encoders (below); thresholds chosen here too, the bar is zero | 79 | 0 of 37,404 | 6,145 of 9,996 |
| hold-out: an animated film clip, other footage, photographs and generators, other encoder settings; 45 streams 720p, 66 streams 1080p; made again without printing a statistic and scored once, after the thresholds were frozen (commit `114239e0`) | 111 | 0 of 37,764 | 7,440 of 15,516 |

The adversarial set: a still photograph with a 6x6 square moving one pixel per game picture, a
drifting 2x2 particle, or a fade of 0.25 code values per game picture. Each as a real 60 fps game;
as a 30 fps game after the rhythm has locked on a panning camera; across a 30-to-60 switch; with
early frames; with grain; 720p and 1080p; three encoder settings. Square and particle also 24 steps
from their background instead of white, and at 60 fps over a camera that moves at 30 (every second
picture differs from the one before it by the object alone). Without the per-sample evidence the
same classifier drops 4,143 of these pictures (4,141 in that last case, where the block means show
a perfect doubled stream, and 2 early frames of the square); with a sample ceiling of 24 instead of 16 it drops 1,184 (the faint
objects); with 16, 12 or 8 none.

Hold-out, repeats kept by content: film and camera clips 2,542 of 7,461, photographs under a pan
666 of 2,829, generators 2,223 of 3,213, the synthetic scene 2,009 of 2,013; by encoder setting
1,834 (x264 25 Mbit/s), 2,358 (x264 8 Mbit/s) and 3,248 (x265 6 Mbit/s) of 5,172 each. It includes
scene cuts (also exactly at a 30-to-60 switch), early and late frames, pauses, rate flips every
second, a 60 fps game with half-rate animation, and 20 and 40 fps games, which are left alone on
purpose (1,440 of the kept repeats). Given only the largest block difference, the same hold-out
streams lose 998 new pictures. This is the second time this hold-out content was scored: once
before the review with the first classifier (0 changed pictures called repeats then too), once
now. The first result did not drive the changes, the reviewer's scenes did; but it is not a
first look.

What the sample rule costs, tune set, repeats kept by content: camera footage 497 of 3,600,
photograph under a pan 625 of 3,021, synthetic scene 3,464 of 3,501, fractal zoom 1,748 of 1,827.
The last two are hard for the encoders: their repeats move samples by 30 to 190 steps, so they
cannot be told from a picture in which something small moved, and are kept. Without the ceiling
2,502 of the 11,949 would be kept, and the adversarial set would lose 4,141 pictures.

Second review (19 November 2026): mixed cadence. Inside a locked 30-in-60 rhythm the first round
called a picture a repeat when no luma sample moved by more than 16 steps and few moved by more
than 4. A faint thing drawn at 60 fps over a world drawn at 30 fits under that, and every second
picture of it was dropped. The reviewer's 25 failing scenes were rebuilt
(`cadence_streams.py --set mixed`: a sub-pixel camera with a 30 fps inset, a camera stepping 6 and
0.1 samples, a 6x6 square and a 2x2 dot within 16 steps of what is under them, soft blobs, a
one-step brightness ramp, a dim counter, grain of sigma 1, 2 and 4, clouds, a vignette pulse, a
dim crosshair; each raw, i.e. without an encoder, and through x264 at 10 and 4 Mbit/s, 720p, 600
pictures) plus five of them appearing 4 s in, and as pictures in the unit tests
(`StreamCadenceTests.inc`, `mixed_tests`).

What changed: the pass reads back signed evidence (above), and a picture in the repeat's place must
also change one way in at most half as many blocks as the new picture before it, must not touch or
move the picture as a whole, must not move its samples one way as a whole, and must stay within
three times the level of this stream's confirmed repeats (touched share, one-way share, mean
change, largest sample). A picture refused by a sample or signed rule counts against the rhythm:
four within 16 pictures give it up and it may not lock again for 120 pictures (doubling up to
15 s); while the rhythm is being learned, a refused fall breaks the run. The reviewer's premise that
refinement is zero-mean and incoherent does not hold for x264: real repeats move 16 to 53 % of
their absolute change one way, and up to a third of their blocks change one way. So the rules are
relative (to the picture before, to the stream's own repeats) or sit above what real repeats
reached in the tune set.

Thresholds frozen in commit `cb723dc2` on tune, adversarial and mixed; the hold-out set was then
made again (with the new columns, nothing printed) and scored once. Linux model, libx264/libx265,
not a PS5:

| Set | Streams | New pictures dropped | Repeats kept (harmless) | First round (`06df900f`) on the same files |
|---|---|---|---|---|
| tune | 90 | 0 of 31,251 | 6,840 of 11,949 | 0 dropped, 6,334 kept |
| adversarial (first review) | 79 | 0 of 37,404 | 7,095 of 9,996 | 0 dropped, 6,145 kept |
| mixed: the 25 rebuilt scenes, raw / 10 / 4 Mbit/s | 75 | 9,275 of 43,320 | (no repeats) | 17,499 dropped |
| mixed: five faint things appearing 4 s in | 15 | 639 of 7,200 | 93 of 1,800 | 2,648 dropped |
| hold-out (scored once) | 111 | 0 of 37,764 | 7,876 of 15,516 | 0 dropped, 6,918 kept |

What no longer drops (0 in raw, 10 and 4 Mbit/s): the sub-pixel camera with the 30 fps inset, the
brightness ramp, the vignette pulse (1 picture at 4 Mbit/s on one photograph), the dim counter, the
crosshair, grain of sigma 4; grain of sigma 1 and 2 without an encoder; clouds without an encoder
and at 10 Mbit/s; and, appearing after clean repeats, the square and the camera steps everywhere,
the dot and grain without an encoder and at 10 Mbit/s.

What still drops (of 600 new pictures per stream; raw / 10 / 4 Mbit/s): the 6x6 square or 2x2 dot
within 16 steps of what is under it, from the first picture on: 295 / 229-231 / 224-229; soft
blobs of amplitude 8 to 30: 295 / 229-231 / 223-228; the camera stepping 6 and 0.1 samples:
295 / 294 / 293; grain of sigma 1 and 2 through the encoder: 281-294 / 218-287; clouds at
4 Mbit/s: 9-33. Appearing 4 s in: the blob 179 / 115 / 115 of 480, the dot and grain at 4 Mbit/s
115 each. In all of these the repeat-slot picture changes less than an encoder's refinement of a
real repeat in every number the pass has (largest sample, samples over 4 steps, touched, one-way
and mean change), and a stream that has it from the first picture has no clean repeat to learn
its level from. Separating them would need where the change is, not how much: not measured. The
unit test `stream_dedup_mixed_cadence_the_limit_that_remains` pins this; when it fails the limit
moved and this paragraph must change. Not one picture was dropped in any set made of real footage,
photographs or generators.

The cost: more repeats are kept than in the first round (tune +506, adversarial +950, hold-out
+958, mostly where the encoder's own refinement exceeds a rule now and then and the rhythm is given
up for 2 s; a pure fade at 30 fps after a pan is refused by the sign rule and kept whole). On a
PS5 stream the log's `mixedUnlocks`, `signedVetoes`, `floorVetoes` and `pixelVetoes` say how often
that happens; the per-picture log carries `signedMean`, `absMean`, `touchedShare` and
`coherentShare` for re-tuning.

What to check first on the PC: the app starts with the switch set (shader loads), `gpuFailures=0`,
a 60 fps game stays `state=unique` with `repeatsFound=0`, and `measureWaitMaxMs` / `gpuTimeouts`.
Then the open question: on a 30 fps game, how many repeats does it remove? `state=doubled` with
`sourceFps` near 30 and `repeatsRemoved` near 300 per 10 s means the PS5 encoder's refinement stays
under 16 steps per sample; a high `keptUnsure` with `sourceFps` near 60 means it does not, and the
experiment does little on that stream. The per-picture log (`pixelMax`, `pixelsOver`) shows which.

Third review (19 November 2026): exact repeats only. The owner's ruling: keeping every real game
picture outranks removing repeats. The second round still dropped 9,275 of 43,320 new pictures in
the rebuilt mixed-cadence scenes. So the question changed from "which threshold" to "what can be
told apart at all". Everything below is a Linux model with software encoders, not a PS5.

What the measurements look like in the repeat's place (medians, 5th-95th percentile in brackets;
`tools/remoteplay_latency_bench/cadence_probe.py`: `levels` over the set files, `place 240` for where
the largest sample change is):

| In the repeat's place | raw | x264 10 Mbit/s | x264 4 Mbit/s |
|---|---|---|---|
| a true repeat of the same world: largest sample change | 0 (0-0) | 1 (1-4) | 3 (1-8) |
| ... share of blocks touched | 0 | 0.012 | 0.11 |
| soft blob moving at 60: largest sample change | 1 (1-1) | 2 (1-6) | 3 (2-13) |
| ... share of blocks touched | 0.014 | 0.025 | 0.095 |
| 6x6 square, 2x2 dot within 16 steps: largest sample change | 10-11 | 10-11 | 9-11 |
| grain sigma 1-2: largest sample change / touched | 7 / 1.0 | 9 / 0.49 | 10 / 0.38 |
| true repeats of the tune and hold-out sets (camera footage, photographs, generators; their encoders at 5-25 Mbit/s): largest sample change | - | medians 6-18 by encoder, 1-150 | - |

- In raw streams a true repeat is exact (no luma sample moved) and every faint class moves at least
  one sample by at least one step: exactness separates every class there.
- Through an encoder nothing the pass measures separates the soft blob, clouds or grain from the
  encoder's refinement of a real repeat: their values lie inside the true repeats' range of the
  same world, and far inside that of harder content. The square and the dot are larger than the
  refinement of this quiet world (11 against 1-4) but far smaller than refinement of camera footage.
- Where the change is: the 16x16 block holding the largest sample change stays within one block from
  one repeat-slot picture to the next for a moving square (1.00 / 1.00 / 0.93 of picture pairs, raw /
  10 / 4 Mbit/s), a 2x2 dot (1.00 / 0.91 / 0.51) and the 0.1 px camera (0.92 / 0.90 / 0.87), but
  not for the blob (0.60 / 0.11 / 0.03), clouds (1.00 / 0.05 / 0.06) or grain (0.00 / 0.00 / 0.05),
  while true repeats reach 0.30 (this quiet world at 10 Mbit/s) and 0.01-0.04 elsewhere. A persistence
  rule would need a new word from the pass (word 11 is free) and would still not protect the blob,
  clouds or grain. Not built.
- Size, sign and coherence were already shown not to separate them (second review).

So no rule separates the blob, clouds and grain classes from a refined repeat, and the design fails
safe for them: a picture is dropped only when, in addition to every rule of the first two rounds
(trusted rhythm, its place in the rhythm, confidence, the sample, signed and learned-level rules):

1. its decoded luma is the previous picture exactly: largest block and sample change, samples over
   4 steps, mean absolute change, touched and one-way blocks all zero (`exactOnly`, `exactPixel`
   0.5 steps; every word must agree, a word missing or not sane keeps the picture);
2. the stream has proven it: 4 exact pictures in a row in the repeat's place (`resumeAfter`) before
   the first drop, again after every boundary (reset, seek, lost or unmeasured picture);
3. nothing moved in the repeat's place lately: 2 non-exact repeat-slot pictures in a row
   (`suspendAfter`) suspend dropping, and the proof of point 2 is needed again (hysteresis).

While dropping is not proven or is suspended, no picture gets a number at all (a number changes how
`ContentDedup` times kept frames), so a stream whose repeats the encoder refines never gets past
point 2 and behaves exactly as with the switch off, apart from the cost of the measurement. Configuration: `engine::streamDedupConfig()` in
`include/veyra/engine/PushStreamDedup.h`; the classifier keeps the second round's verdict for the
evaluation tool (`cadence_eval --set exactOnly=0`). No shader, D3D12 or `StreamChangePass.h` change.
`EngineController.cpp`: the 10 s log line gains `exactVetoes` (repeats the rhythm found that were not
exact: kept) and `suspendedKeeps` (exact ones kept while suspended); one format string, unverified
on Windows.

Thresholds frozen in the commit that introduced them (`exactPixel` 0.5, `suspendAfter` 2,
`resumeAfter` 4; chosen by reasoning, not tuned), then the third hold-out was generated without
printing a statistic and scored once:

| Set | Streams | New pictures dropped | Repeats kept (harmless) | Second round on the same files |
|---|---|---|---|---|
| legacy | 40 | 0 of 13,755 | 5,445 of 5,445 | 0 dropped, 5,329 kept |
| tune | 90 | 0 of 31,251 | 11,949 of 11,949 | 0 dropped, 6,840 kept |
| adversarial (first review) | 79 | 0 of 37,404 | 9,425 of 9,996 | 0 dropped, 7,095 kept |
| hold-out (second round's, scored before) | 111 | 0 of 37,764 | 15,516 of 15,516 | 0 dropped, 7,876 kept |
| mixed: the 25 rebuilt scenes and the five appearing 4 s in, raw / 10 / 4 Mbit/s | 90 | 0 of 50,520 | 2,371 of 3,480 | 9,914 dropped, 375 kept |
| third hold-out (new generator, scored once): 17 scenes x raw, x264 and x265 at 10 and 4 Mbit/s, 720p, 240-300 pictures | 85 | 0 of 18,665 | 2,544 of 2,935 | 2,124 dropped, 995 kept |

Third hold-out by class (new pictures dropped, engine / second round; every class through all five
encodings): film grain sigma 0.5: 0 / 438; grain sigma 3: 0 / 184; dim HUD counter: 0 / 519; marker
blinking every 15 pictures: 0 / 24; fog drifting 0.5 px: 0 / 176; fade of 0.2 steps per picture:
0 / 0; 40 one- and two-sample particles: 0 / 61; camera at 0.15 px per picture: 0 / 0; rain: 0 / 0;
water shimmer: 0 / 519; text scrolling 0.5 px: 0 / 203; picture-in-picture at 60: 0 / 0. Controls:
true 30-in-60 game worlds (2): 0 dropped, repeats removed only without an encoder (286 of 300 raw;
none of 1,200 encoded); 60 fps: 0 dropped, never locked; 40 and 45 fps: 0 dropped, nothing removed
(not a doubled rhythm, by design). Results in `tools/remoteplay_latency_bench/cadence_results.json` (`holdout3`).

Mixed set by class, second round -> third: blob 3,409 -> 0, square 2,254 -> 0, grain 1,711 -> 0,
dot 1,615 -> 0, camera steps 882 -> 0, clouds 42 -> 0, vignette 1 -> 0 (pan, ramp, counter,
crosshair were already 0). Repeats removed there: raw 1,109 of 1,160, x264 none.

What being conservative costs, plainly: in this model the experiment now removes repeats only from
streams whose repeats decode exactly: raw (a lossless link) and some libx265 repeats of still
pictures (571 of the adversarial set's 9,996). Not one repeat of any libx264 stream, at any bitrate,
nor of camera footage, photographs or generators through any encoder, is removed any more: tune and
both earlier hold-outs went from about half removed to none. On such a stream the switch changes
nothing but the cost of the measurement.

Residual risk, plainly:

- A dropped picture's luma is identical to the picture kept before it, in every sample. What it can
  still differ in is chroma, which is not measured: a change in colour alone, at 60 fps over a
  30 fps world, in a stream that has shown exact repeats, would be dropped on the repeat's place
  (until two non-exact repeat-slot pictures in a row suspend it). This was untested in the third
  round (the sets measured luma only), not "not seen"; the fourth review found it (below).
- "Exact" is exact in what the pass reads back (8-bit luma, 16x16 block sums, per-sample maxima and
  counts). 10-bit / HDR is refused by the pass, as before.
- None of this was run on a PS5 or on the RTX 5070. Whether the PS5's encoder sends repeats that
  decode exactly is the open question that decides whether the experiment does anything at all.

Fourth review (19 November 2026): chroma. The independent re-review rejected the third round:
chroma is never read, so on a stream with exact luma repeats a colour-only change at 60 fps was
dropped on almost every repeat slot (its streams, 720p, 420 pictures, seeds 5101-5103: a HUD hue at
60 over a panned world at 30, 203 new pictures dropped raw; a colour HUD at 60 over a still
background with a sprite at 30, 202 raw and 54 through x265 at 10 Mbit/s, whose decoded chroma
differed by up to 24 codes over about 1,600 samples; an isoluminant blink, 41 raw; a colour grade
drifting by one code in V, 153 raw).

What changed (fail-safe, the alternative the coordinator allowed): "exact" now also requires the
chroma of the picture to be measured and identical (`StreamCadence::Input::chromaChanged == 0`,
`pipeline::StreamChangeMeasurement::chromaChanged`). The GPU pass does not read the chroma plane
(there was no time to add and check a plane-1 readback blind), so its measurement always says "not
measured" (-1), and **with the switch on the experiment now drops nothing and numbers nothing on
any stream.** It still measures every picture and logs the rhythm; that is all it does. No shader,
D3D12, `StreamChangePass.h` or `EnhanceGraph` change. A chroma readback (plane 1, R8G8 at half size,
a per-sample exact compare against a kept chroma plane, one more result word) is what is needed
before it can drop anything.

The model and the generator measure chroma now: `cadence_streams.py` writes a last column, the chroma
samples (U and V) that changed at all, and keeps the whole I420 picture for raw streams;
`cadence_eval` counts a dropped picture as "identical" only when Y, U and V are all unchanged, and
`--chroma` gives the classifier that column (a model of a pass that reads chroma). The reviewer's
generator is `tools/remoteplay_latency_bench/cadence_chroma.py` (its chroma scenes with raw, x264 and
x265 at 10 and 4 Mbit/s).

Every set scored again with the thresholds unchanged (Linux model, software encoders, not a PS5). The
earlier sets were made before the chroma column existed: there, as in the engine, chroma counts as
not measured. The chroma set is the reviewer's five chroma scenes, each raw and through x264 and
x265 at 10 and 4 Mbit/s, 720p, 420 pictures (`cadence_chroma.py`):

| Set | Streams | New pictures dropped (engine) | Repeats removed (engine) | With a modelled chroma readback (`--chroma`) | Third round's / second round's verdict |
|---|---|---|---|---|---|
| legacy | 40 | 0 of 13,755 | 0 of 5,445 | (no chroma column) | 0 / 0 dropped |
| tune | 90 | 0 of 31,251 | 0 of 11,949 | (no chroma column) | 0 / 0 |
| adversarial (first review) | 79 | 0 of 37,404 | 0 of 9,996 | (no chroma column) | 0 / 0 |
| hold-out (second round's) | 111 | 0 of 37,764 | 0 of 15,516 | (no chroma column) | 0 / 0 |
| mixed (second review) | 90 | 0 of 50,520 | 0 of 3,480 | (no chroma column) | 0 / 9,914 |
| third hold-out | 85 | 0 of 18,665 | 0 of 2,935 | (no chroma column) | 0 / 2,124 |
| chroma (fourth review) | 25 | 0 of 9,100 | 0 of 1,400 | 0 dropped, 220 removed (blink raw 162, x265 10 Mbit/s 5; colour pulse over a pause raw 53) | the review's figures above / 2,491 |

So: nothing is dropped anywhere, and nothing is removed anywhere. With the switch on, the experiment
costs the measurement and gives nothing until a chroma readback exists; with a chroma readback the
model says it would remove repeats only where Y, U and V repeat exactly, and drop no new picture in
any set here. Unit tests: `stream_dedup_exact_a_colour_only_change_is_never_a_repeat` and
`stream_dedup_exact_without_a_chroma_measurement_nothing_is_dropped` (`StreamCadenceTests.inc`); the
GPU test checks that an NV12 picture with unchanged luma reports chroma as not measured (it does not
fill or read plane 1; unverified on Windows).

Cost of the switch even when it drops nothing (the re-review's point; it was not stated plainly
before): with frame generation and duplicate removal on, every hardware-decoded picture is measured
and the engine thread waits for the answer before it decides about the frame, up to 4 ms and never
past the next presentation, also while dropping is unproven or suspended. That wait is added
latency on every frame. The pass also keeps a full-size R32_FLOAT copy of the previous luma plane
(about 8 MB at 1080p, 33 MB at 4K) plus the block means. With the switch off none of it exists.

What a real PS5 capture must show before the feature is worth anything (all of it, on the PC, with
the switch on, `VEYRA_VERBOSE_FRAME_LOGS=1`):

1. a chroma readback in the pass; until then nothing is dropped, whatever the capture shows;
2. on a 30 fps game, repeat-slot pictures with `pixelMax=0`, `touchedShare=0` and chroma 0 (exact
   in Y, U and V), and `repeatsRemoved` near 300 per 10 s with `exactVetoes` near 0;
3. a full-YUV hash check on a scene with a colour HUD at 60 fps (every dropped picture's decoded
   Y, U and V hash equal to the kept picture's);
4. the measurement wait P50 and P99 on the real GPU (`measureWaitMeanMs`, `measureWaitMaxMs`,
   `gpuTimeouts`), because that wait is paid on every frame.

The switch `VEYRA_EXP_RP_STREAM_DEDUP` stays off by default.

What to check first on the PC (replaces the second round's paragraph above): with the switch set
and a 30 fps game, `repeatsRemoved` near 300 per 10 s and `exactVetoes` near 0 mean the PS5's
repeats decode exactly; `repeatsRemoved=0` with `exactVetoes` near 300 means they are refined and
the experiment does nothing on that stream (by design). The per-picture log (`pixelMax`,
`pixelsOver`, `absMean`, `touchedShare`) shows how far from exact they are; any rule that drops
refined repeats needs PS5 streams with faint 60 fps content to be judged on, not this model.

GPU test (`testStreamChange`, `tests/integration/PushGpuTests.cpp`): it failed once on CI's software
adapter (run #15) and passed on the same tree; its two timing-dependent checks are gone. The fence
is read once when checking whether a list may still read the texture, and the wall-clock bound on a
call (deadline + 50 ms) is replaced by a check of the mechanism: a deliberately tiny deadline is
reported as timed out, and the next measurement works and hands a queued answer over. Unverified on
Windows until the GPU test step runs.

How to make every set again (from `tools/remoteplay_latency_bench`, Python 3 with numpy, PyAV,
OpenCV and scikit-image; libx264 and libx265 through PyAV; clips as in the file's header):

    python3 cadence_streams.py <dir>/legacy --set legacy
    python3 cadence_streams.py <dir>/tune --set tune --clips <clips>
    python3 cadence_streams.py <dir>/holdout --set holdout --clips <clips> --quiet
    python3 cadence_streams.py <dir>/adversarial --set adversarial --frames 600
    python3 cadence_streams.py <dir>/mixed --set mixed --frames 600
    python3 cadence_streams.py <dir>/holdout3 --set holdout3 --frames 300 --quiet --only h3-game0-immunohistochemistry
    python3 cadence_streams.py <dir>/holdout3 --set holdout3 --frames 300 --quiet --only h3-game0-cat
    for c in h3-game0-retina h3-grain h3-counter h3-blink h3-fog h3-fade h3-particles h3-subpan h3-rain h3-shimmer h3-scroll h3-pip; do
      python3 cadence_streams.py <dir>/holdout3 --set holdout3 --frames 240 --quiet --only $c; done
    python3 cadence_chroma.py <dir>/chroma        # the fourth review's chroma scenes (seeds 5101-5103); write an index.json
                                                  # (label, content, encoder per file) before cadence_report
    g++ -std=c++20 -O2 -I../../include cadence_eval.cpp -o cadence_eval
    python3 cadence_report.py --eval ./cadence_eval --sets <dir>     # every set, engine and second-round verdicts

Seeds are fixed in the manifests (`manifest()` in `cadence_streams.py`: holdout3 uses 7001-7073 and
the worlds immunohistochemistry, cat, retina with camera, gravel and moon insets; the earlier sets
use 3-99 and coffee, astronaut, rocket, chelsea, hubble_deep_field), so the same command gives the
same pictures; the encoders' output can differ with other libx264/libx265 builds. Unit tests:
`stream_dedup_exact_*` (proof, suspension and resumption, boundaries, refined repeats give no
numbers, every class of the second review's mixed scenes from the first picture and from 4 s, the
first review's scenes at every rate with and without the encoder model) and
`stream_dedup_mixed_cadence_the_second_rounds_limit_is_gone` (the second round's verdict still drops
250-300 of 600 there; the engine's none) in `tests/remoteplay/StreamCadenceTests.inc`.

## In-app updater off in development builds (`SLIPSTREAM_BUILD`, `VEYRA_EXP_RP_NO_UPDATE=1`)

A development build reports a version such as `2.1.5-pushfps.<run>`; the official project publishes 3.x
releases. The unguarded update check therefore offers the official release, and an installed copy can
replace itself with it in one click. Two guards, both in `include/veyra/ui/UpdatePolicy.h` (a pure
function, tested in `tests/remoteplay/UpdatePolicyTests.inc`):

- **`SLIPSTREAM_BUILD`** (CMake option, OFF by default). `build-windows.yml` passes ON for every
  `slipstream/**` branch and for every untagged build outside the official repository. Such a build never
  asks the release server, never shows the update banner and refuses the install action; the log says
  `[update-check] disabled: Slipstream development build`, and the version in the window title and in
  About ends in `-slipstream`. No switch turns the updater back on.
- **`VEYRA_EXP_RP_NO_UPDATE=1`** (environment, or the same line in `slipstream.ini`): any build that
  contains this code stops checking while it is set. `0`, `off`, `no`, `false` or an empty value leave the
  updater on. The log says `[update-check] disabled: VEYRA_EXP_RP_NO_UPDATE is set`.

With neither, nothing changes: that is every official build. The **original** build (`ed317efb`) has no
such code; the package's installer protects it another way (`docs/SETUP.md`, "The in-app updater").
Verified on Linux only: the policy tests, and `tests/unit/UpdateCheckerTests.cpp` built against Qt 6.4
(31 cases, including no request to the server with either guard). Unverified on Windows.
## Performance modes in the interface (branch `exp/w1-ui`, 2026-11-18)

Unverified on Windows: none of this has run in the app, on the RTX 5070 or against a PS5.

The quick setup has a row "Performance mode" (`pushfps-mode`, `qml/Veyra/PushModeRow.qml`): Off (my own
settings), Ultra low latency, Competitive, Balanced, Maximum smoothness. A mode writes settings that
exist already, through the same bridge properties as the other rows (`qml/Veyra/PushModes.js`):

| Mode | 30 fps game | 60 fps game | Queue | Sync, variable refresh confirmed by the driver | Sync otherwise | Output cap |
|---|---|---|---|---|---|---|
| Ultra low latency | off | off | one frame | V-Sync + VRR | tearing | off |
| Competitive | off | 2X -> 120 | one frame | V-Sync + VRR | Automatic | 120 with generation, else off |
| Balanced | 3X -> 90 | 2X -> 120 | as it was | V-Sync + VRR | V-Sync + VRR request | 90 / 120 |
| Maximum smoothness | 4X -> 120 | 3X -> 180 | as it was | V-Sync + VRR | V-Sync + VRR request | 120 / 180 |

- "One frame" is NVIDIA low latency where the adapter has it, else the low queue. "Confirmed" is
  `vrrState` 3 or 4 (the driver's report, NVIDIA only today); 0 to 2 count as not confirmed.
- The multiplier is lowered until source x M fits the refresh rate (2 % over allowed, as
  `pushModePlan`), then to what the frame generation method offers; without generation the cap is off.
  A cap equal to the refresh rate becomes "follow the display". Each such step is said in the row.
- With generation on, duplicate removal goes from Off to Automatic; a removal mode the user chose stays.
- The row's line gives (M-1)/M x source interval for the multiplier in use: arithmetic, not a measurement.
- Source rate: the measured one when it is 30 or 60 (`cadenceSourceFps` under
  `VEYRA_EXP_RP_STREAM_DEDUP`, else the engine's `contentFps`), else the quick setup's game frame rate.
  40 fps has no row: no generation. Open question for a 30 fps game inside the 59.94 fps PS5 stream:
  removing its repeated pictures on the hardware-decoded path is the experiment
  `VEYRA_EXP_RP_STREAM_DEDUP=1`, which a mode does not set. Without it the mode starts from the chosen
  30 (3X or 4X under a 90 or 120 cap), and if the engine then reports the stream as 60 the driver follows
  that after three seconds (2X -> 120, generated between repeated pictures). Which of the two happens has
  to be read from the log on a PS5 (`[push-mode] applied ... reason=source`).
- The settings from before the first mode are kept in the preference `pushModeRestore` and come back with
  Off. `PushModeDriver.qml` (one instance, Main.qml) re-applies the mode when the measured rate or the
  display moves to another row of the table for three seconds, with a notice. No mode chosen: no timer,
  no bridge call, nothing written.
- The modes' own experiment switches (`VEYRA_EXP_RP_PAD_POLL_MS=2`, `VEYRA_EXP_RP_ADAPTIVE_PHASE=1`) are
  still set only by `slipstream.ini`: the interface does not set them.

LAN switch in the same row (preference `pushLanProfile`): sets `VEYRA_EXP_RP_LAN_PROFILE=1` in the process
for the next PS5 connection (`applyPushLanProfile`, `src/ui/bridge/PushSlipstream.cpp`), at start-up and
when toggled. A value already in the environment (the user's, or `slipstream.ini`'s) is never touched,
only a value this switch set is removed again, and nothing is applied under
`VEYRA_TEST_RP_NO_SETTINGS_FILE`. Limit: a key held off in `slipstream.ini` (`VEYRA_EXP_RP_LAN_PROFILE=`)
is not remembered past start-up, so the switch can still turn it on.

| Switch | Effect when set |
|---|---|
| `VEYRA_EXP_AUTO_MODE=1` | shows the row "Adjust the mode automatically" (preference `pushModeAuto`). Once a second `pushAutoModeStep` feeds `PushAutomaticMode` (ceiling = the chosen mode) with the source rate, refresh rate, GPU utilisation, `fgBudgetLimited` (counted as a busy GPU), the stream's ingress depth and the connection grade; a change is applied, logged as `[push-mode] automatic mode=... reason=...` and shown as a notice. A PS5 stream without `VEYRA_EXP_RP_HEALTH=1` has no grade, which is never counted as healthy: it can step down, not back up. Must be in the environment when PushFPS starts (`slipstream.ini` only accepts `VEYRA_EXP_RP_*` keys) |
## Recovery after outages (`VEYRA_EXP_RP_RECONNECT_PATIENT=1`)

Off unless set (`0` and empty leave it off). Unverified on Windows and on a PS5. The tests
(`tests/remoteplay/AdaptiveTransportTests.inc`, `recovery_*`) replay the owner loop's recovery rule
in virtual time; they are a model, not a session.

What happens today, unchanged with the switch unset:

| Outage | What the app does | Where |
|---|---|---|
| 1 s | one keyframe request after 1 s without a decoded frame; the stream continues, no reconnect | `include/veyra/remoteplay/StreamRecovery.h:52` |
| 5 s | keyframe requests at 1 s and 3 s; no reconnect (that needs 6 s of silence) | `StreamRecovery.h:48`, `:52` |
| cable pull, back within about 13 s plus the failed connects | reconnect after 6 s, the next attempts 1, 2 and 4 s apart, without restarting the app | `StreamRecovery.h:48`, `src/source/RemotePlaySessionSource.cpp:185`, `:409` |
| longer (a router restart) | after the third failed attempt the session stops; the app keeps running and Start reconnects | `StreamRecovery.h:49` |

Only 30 s of continuous decoded video renews the three attempts (`StreamRecovery.h:39`), so
repeated short outages cannot loop for ever. Nothing stands in a queue after an outage: the first frame
after a gap empties the ingress queue and waits for a keyframe (`src/remoteplay/VideoIngress.cpp:76`),
no frame is handed over older than 100 ms (`VideoIngress.cpp:86`, `:102`; `VideoIngress.h:13`), and
the test with a 300 ms decoder stall after the outage shows the wait back at 2 ms a second later. An
outage gives the adaptive reorder wait no reason to widen (losses, not late arrivals), and a wait kept
across a reconnect (`RemotePlaySessionSource.cpp:172`) narrows back one step per 30 s on a clean link.

The gap: a 60 s outage, with a connect that fails within 2 s, ends at about 19 s after the pull. With
the switch: up to 8 attempts per outage instead of 3, the fourth and later 8 s apart (40 s for a console
that still holds the previous session), so in the model a 60 s outage reconnects at most about 10.5 s
after the link is back, and a link that never comes back stops after 8 attempts (about 53 s plus the
failed connects). The on-screen message keeps its `n/3` text and reads `3/3` during the extra
attempts; the log line names the real count. What a PS5 does when it is asked to start a session
again and again while the old one is gone is not known: a real cable-pull run is needed before this
is enabled.

## What limits the stream: `bottleneck=` in the health line (with `VEYRA_EXP_RP_HEALTH=1`)

No switch of its own: the `[remoteplay-health]` line gains `bottleneck=`, `signs=`, `overshootMs=` and
`decodeP95Ms=` at its end (earlier keys unchanged). Pure rules on one five-second interval
(`include/veyra/remoteplay/ConnectionHealth.h`, `bottleneckSigns` / `classifyBottleneck`, tests
`bottleneck_*`); the thresholds are provisional and not calibrated on a PS5.

| Name | Sign |
|---|---|
| `loss` | measured loss at least 0.1 %, or a frame FEC could not complete |
| `reordering` | at least 1 in 1000 packets (and 10) arrived out of order |
| `jitter` | frame interarrival jitter at least 2 ms |
| `receive-scheduling` | over at least 4 reorder waits, they lasted 2 ms or more longer than asked (patch 0004's overshoot counter): the receive thread woke late |
| `decoder-backlog` | the ingress queue reached 3 frames, or decode time P95 is at least 0.8 of a frame interval |
| `wifi-route` | this PC reaches the console over Wi-Fi |

`signs=` lists every sign seen; `bottleneck=` names one: a late receive thread first, then a decoder
backlog (both local, and the first also produces reordering and jitter), then the Wi-Fi route when
network signs are present, then loss, reordering, jitter. A clean interval, also on Wi-Fi, says `none`.

## Latency governor (`include/veyra/engine/PushLatencyGovernor.h`, recommendations only)

Not wired to anything: no switch, no engine or interface code calls it, so nothing changes at run time.
A caller would pass one measurement about once a second (network jitter and loss, frames lost, decode and
GPU time, owner-thread scheduling delay, ingress queue depth, how late frames are presented, source
cadence, GPU headroom) with the current knob values, and decide itself whether to apply what comes back.

At most one recommendation at a time, 10 s apart, each after 3 bad periods in a row (10 calm ones for
the two "back to shorter" rules), with a band between the bad and the clear thresholds in which nothing
moves: backlog off when the queue stands; frame generation, then the quality tier, down one step when
the GPU has under 10 % headroom or frames are presented 2 ms late; the controller poll interval one step
longer only when the owner thread is already scheduled at least that late, and shorter again after a
calm stretch; the reorder profile from the long wait to the short one on a clean link. Five seconds
after a change its target (queue depth, GPU time, scheduling delay or frames lost per second) is compared
with the periods before it; worse by more than 10 % and a small margin, the change is rolled back and
that knob left alone for 1 minute, doubling up to 16. A recommendation not applied within 5 s is
dropped the same way.

It never recommends turning the backlog on, more frame generation, a higher quality tier or a longer
reorder wait: nothing that raises latency to show more frames. Steps back up are the user's. Tests:
`governor_*` in `tests/remoteplay/AdaptiveTransportTests.inc` (made-up measurements; thresholds not
calibrated on any hardware).
## Audio: fast recovery and low endpoint buffer (branch `exp/w2-audio`, 2026-11-19)

Unverified on Windows: none of this has been compiled by MSVC, run in the app or heard. Audit, model and
controller: `docs/AUDIO.md`. Both switches are read when a PS5 stream's audio starts (each connection
attempt); unset or `0` = the original behaviour. Only the PS5 Remote Play audio session uses them; capture
cards, Moonlight and Xbox are unchanged.

| Switch | Effect when set |
|---|---|
| `VEYRA_EXP_RP_AUDIO_FAST_RECOVER=1` | Auto sync only (manual offset and Off keep the original rule). Target = median of the last 9 presented frames' delay (generated frames included) plus the audio PTS drift since the session started (`AudioArrivalDrift`: clock difference and burst re-anchors, `docs/AUDIO.md` findings 1 and 11). Every endpoint period instead of every 250 ms: ratio within the original 5000 ppm; late audio removed from the head with a 2.5 ms crossfade (all of it right after an endpoint gap, quiet windows, or 5 ms cuts at most every 100 ms once 15 ms has lasted 150 ms; a cut counts as pending until the endpoint has played it); fade-and-restart only when 400 ms late or 60 ms early. First reopen after a device change after 50 ms instead of 500 ms |
| `VEYRA_EXP_RP_AUDIO_LOW_BUFFER=1` | Opens the endpoint in shared mode with the smallest engine period the device reports (`IAudioClient3::InitializeSharedAudioStream`); any failure (no `IAudioClient3`, no smaller period, a format the engine would have to convert) silently uses the original 20 ms `Initialize`. Never exclusive mode. Given up for the rest of the stream (one `[remoteplay-audio] low-latency endpoint period abandoned` line, then a reopen with the original buffer) when a step after creating the low-latency stream fails or after 8 endpoint underruns while PCM was arriving. The scheduling reserve stays 14-20 ms (Opus arrives in 10 ms blocks). Logged as `low-latency period request ... opened=` |

With either switch, one line every 10 s: `[remoteplay-audio] queueMs= endpointMs= delayMs= targetMs= errorMs=
driftPpm= ratioPpm= underruns= resets= deviceChanges= drops quiet= loud= afterGap= droppedMs= lowBuffer=
active|fallback|off bufferMs= fastRecover=` (the audio owner's own estimates, not a measurement at the
speaker). Rollback: unset the switch, or `git revert` the wiring commit.

Needs listening on real hardware before anyone relies on it: crossfaded cuts in music and speech, the
pitch of the ratio, a USB headset unplugged and replugged mid-game, a Bluetooth headset (its link latency
is invisible to the device clock either way), and a default-device change while playing.

## Watching the governor and the GPU budget (`VEYRA_EXP_RP_GOVERNOR`, `VEYRA_EXP_RP_GPU_BUDGET`)

Both are off unless set, and read once per run. Without them nothing changes: `pushSlipstreamWatch`
(`src/ui/bridge/PushSlipstream.cpp`) returns at its first line. With them it runs on the interface's existing
one-second tick (`pushFlowBudgetWatch`), never on the frame path, and only while something plays, settings
are settled, and neither "Optimise for my PC" nor the frame quality test runs. The numbers come from the
player snapshot as it is (`include/veyra/engine/PushGovernorFeed.h`; tests `governor_feed_*` in
`tests/remoteplay/GovernorFeedTests.inc`). No interface, no strings.

- `VEYRA_EXP_RP_GOVERNOR=1`: feeds `PushLatencyGovernor` and logs `[slipstream-governor] mode=observe
  recommend=<knob from->to effect=... reason="..."|none> applied=0 ...inputs... knobs ... unavailable=[...]
  rollbacks= declined= invalid= session=`. Never applied: an unapplied recommendation is dropped by the
  governor after 5 s and counted as `declined` (that knob is then left alone for a minute, doubling).
- `VEYRA_EXP_RP_GPU_BUDGET=1`: feeds `PushGpuBudget` and logs `[slipstream-gpu-budget] mode=observe level=<i>
  multiplier= tier= reason=... request multiplier= tier= method= ...inputs... unavailable=[...]`.
- `VEYRA_EXP_RP_GPU_BUDGET=apply`: the same, and the level is put in place through the interface's own
  setters (`setFgMultiplier`, `setVfgQuality`): PushFlow tier down, multiplier down, generation off, one step
  at a time; recovery after a long healthy stretch, never above what the user chose. A change the user
  makes becomes the new ceiling. While it applies, PushFlow's own "too slow" tier step
  (`pushFlowBudgetWatch`) is skipped. The setters save settings, so the user's choice is put back (logged
  `restored the user's choice (...)`) when the stream stops, when the optimiser or the quality test starts
  (the budget then stands down), and at quit before the final save. Residual risk: the optimiser writes its
  restore record when it starts, up to a second before this tick notices it, so that record can hold the
  reduced level. A level the setter refuses is not retried for 60 s (`refused=1`, then `held=1`).
  Apply is refused, with one log line, when the decode-queue delay is not measured (capture cards: the
  budget could never see a healthy period and would only step down); it observes there instead.

Each log line is written at most every 10 s, or at once when the recommendation (governor) or the level
(budget) changes. Inputs the snapshot does not have are listed in `unavailable=`: present lateness and the
PS5 owner thread's scheduling delay (the governor gets a neutral value between its thresholds, so they can
neither start nor clear a rule), thermal state (always unknown), late real frames (missed deadlines count only
generated frames that expired), and the decode-queue delay for capture cards (stream sources only; without it
the budget never calls a capture card healthy, so it never steps back up there). The per-frame GPU time is the
enhancement P95 minus the FG batch P95, an approximation. While either switch is set,
`pushHardwareSummary` also carries the capability model's keys (`capsVrrState`, `capsDecodeH264`... where a
name was taken), asked once per process; without the switches it is unchanged. The LAN profile, reorder,
backlog and pad poll switches the governor reports as its knobs are read once per stream, when the watch
starts for it.

Everything here is unverified on Windows: it passed the Linux tests and a mingw syntax check, was never
compiled by MSVC, and no threshold has run on any GPU or PS5.

## Variable refresh without NVAPI: a diagnostic (`VEYRA_EXP_VRR_DXGI_FALLBACK=1`, 2026-11-19)

The variable-refresh state (`vrrState` 0-4) is read only through NVAPI (`gfx::queryVrr`, `NvidiaVrr.h`). On an
AMD or Intel PC NVAPI is missing, so a FreeSync / adaptive-sync screen reads 0 (unknown): "Lowest latency" then
does not ask for variable refresh and says the screen cannot do it, and the uneven-target hint shows.

DXGI cannot fix that: its tearing support (`IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)`)
is an OS / driver capability that is true on almost every PC whatever the screen, so treating it as "VRR capable"
would make "Lowest latency" force smooth pacing, say VRR is on and hide the uneven-target hint on fixed-refresh
screens. So the switch changes nothing the interface or the presenter reads: with it set to exactly `1` (read
once) and only when NVAPI is not ready (no NVIDIA driver, or its initialisation failed), `queryVrr` asks DXGI once
per process, writes one log line `[display-vrr] nvapi=unavailable dxgiTearing=1|0|unknown (diagnostic only; VRR
level unchanged)` and stores the answer in `VrrState::dxgiTearing`, which nothing reads; the level stays exactly
as without the switch (static_assert in `NvidiaVrr.h`; `engine::pushDxgiTearingReport` tested in
`HardwareProfileTests.inc`). On an NVIDIA PC with a working driver nothing changes at all.

Testers with AMD / Intel: run with the switch and send that log line, plus whether the screen has FreeSync /
adaptive sync on (monitor menu, AMD Software or Intel Graphics Command Center) and Windows' "Variable refresh
rate" setting. This collects what a vendor-neutral VRR detection would need; it fixes nothing yet. Unverified on
Windows (mingw syntax check of `DisplayStateService.cpp` and `VideoPresenter.cpp` only).

## PushFlow crash guard order (`VEYRA_EXP_PUSHFLOW_CRASH_ORDER=1`, 2026-11-19)

After PushFPS stopped inside PushFlow, the next start switches to another frame-generation method
(`QmlPlayerBridge.cpp`, `pushflow::crashedLastTime`). Its order put experimental providers first (VFG, then DLSS,
FSR 4, FSR 3.1, XeSS, ANR, AMD FRC). With the switch set to exactly `1` (read once) it uses
`engine::kPushCrashGuardOrder` (`PushHardwareProfiles.h`): FSR 4 ML, FSR 3.1, XeSS, DLSS (the capability model's and
the settings sanitizer's order), then VFG, ANR, AMD FRC; frame generation off when none is available. Note that on
an NVIDIA PC with every file this picks FSR 3.1, not DLSS, as the sanitizer does. Tested in
`HardwareProfileTests.inc` (`hardware_pushflow_crash_guard_prefers_non_experimental_methods`). Without the switch
nothing changes. Unverified on Windows (mingw syntax check only).
## Reorder wait study, sprint 3 (Linux model, `tools/remoteplay_latency_bench/reorder_study.cpp`)

**Linux model. No PS5, no network and no Windows machine was involved; the numbers compare rules on the
same made-up arrivals and predict nothing about a real console or LAN.** Conditions and runner:
`include/veyra/remoteplay/ReorderStudy.h` (the same queue model as above, checked again against the
patched C source on 19 Nov: 40 traces, 2.28 million released packets, equal). Every condition is clean,
impaired from 30 s for the stated time, then clean again; 5 seeds of 120 s (35960 frames per cell).
Raw output: `tools/remoteplay_latency_bench/reorder_study_results.json` (byte-identical on a second run).
Displaced packets pass the link's queue and are then held back by the stated delay; a frame needs 24 of
its packets (24 + 3 FEC, or 24 + 0 for `nofec`). "Lost" = frames not handed to the decoder; each is a
keyframe request in the library, merged within 500 ms in the second number. Added delay = hand-over minus
the moment 24 of the frame's packets had reached the socket, over delivered frames (median, P95, P99 in
ms). "Back" = after the impairment, until every frame of one second is delivered within 1 ms of the
baseline whole delay (send to hand-over); it was the same for every policy in every condition, and the
P99 whole delay 10-20 s after each impairment was 0.1-0.2 ms above baseline everywhere: nothing
accumulated. Best fixed = fewest lost frames, ties within max(2, 2 %) broken by the lower P99 added delay;
the adaptive rule is within margin when it loses at most max(5, 10 %) more frames and its P99 added delay
is at most 1 ms above. Best fixed wait, exactly: among the fixed waits that lose at most max(2, 2 % of the
fewest) frames more than the fewest, the one with the lowest P99 added delay; "best (ties)" lists every such
candidate within 0.05 ms of that P99 (`tiedFixed` in the JSON).

| Condition | fixed 2 ms: lost / requests / added | fixed 4 ms | fixed 8 ms | fixed 16 ms | adaptive (refined) | adaptive before | best (ties) | within margin | back, ms (worst seed) |
|---|---|---|---|---|---|---|---|---|---|
| `clean_wired_lan` | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 | all four tie | yes | 13 |
| `light_reordering` | 5 / 5 / 0.00, 0.47, 2.14 | 5 / 5 / 0.00, 0.48, 2.70 | 5 / 5 / 0.00, 0.49, 2.71 | 5 / 5 / 0.00, 0.49, 2.71 | 5 / 5 / 0.00, 0.47, 2.14 | 5 | 2ms | yes | 5 |
| `heavy_reordering` | 156 / 137 / 0.00, 3.78, 5.12 | 9 / 9 / 0.00, 7.01, 8.56 | 5 / 5 / 0.00, 8.50, 10.72 | 5 / 5 / 0.00, 9.96, 11.06 | 20 / 20 / 0.00, 7.88, 10.51 | 127 | 8ms | no | 5 |
| `burst_loss_ge2_fec` | 10 / 10 / 0.00, 0.00, 0.00 | 10 / 10 / 0.00, 0.00, 0.00 | 10 / 10 / 0.00, 0.00, 0.00 | 10 / 10 / 0.00, 0.00, 0.00 | 10 / 10 / 0.00, 0.00, 0.00 | 10 | all four tie | yes | 5 |
| `burst_loss_ge5_fec` | 60 / 56 / 0.00, 0.00, 0.00 | 60 / 56 / 0.00, 0.00, 0.00 | 60 / 56 / 0.00, 0.00, 0.00 | 60 / 56 / 0.00, 0.00, 0.00 | 60 / 56 / 0.00, 0.00, 0.00 | 60 | all four tie | yes | 5 |
| `burst_loss_ge15_fec` | 167 / 96 / 0.00, 0.00, 0.00 | 167 / 96 / 0.00, 0.00, 0.00 | 167 / 96 / 0.00, 0.00, 0.00 | 167 / 96 / 0.00, 0.00, 0.00 | 167 / 96 / 0.00, 0.00, 0.00 | 167 | all four tie | yes | 5 |
| `burst_loss_ge2_nofec` | 121 / 100 / 0.00, 0.00, 0.00 | 121 / 100 / 0.00, 0.00, 0.00 | 121 / 100 / 0.00, 0.00, 0.00 | 121 / 100 / 0.00, 0.00, 0.00 | 121 / 100 / 0.00, 0.00, 0.00 | 121 | all four tie | yes | 5 |
| `burst_loss_ge5_nofec` | 142 / 100 / 0.00, 0.00, 0.00 | 142 / 100 / 0.00, 0.00, 0.00 | 142 / 100 / 0.00, 0.00, 0.00 | 142 / 100 / 0.00, 0.00, 0.00 | 142 / 100 / 0.00, 0.00, 0.00 | 142 | all four tie | yes | 5 |
| `congestion_queue` | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 | all four tie | yes | 81 |
| `congestion_with_reordering` | 19 / 17 / 0.00, 0.00, 0.65 | 6 / 5 / 0.00, 0.00, 0.65 | 0 / 0 / 0.00, 0.00, 0.89 | 0 / 0 / 0.00, 1.29, 6.32 | 15 / 15 / 0.00, 0.00, 0.65 | 19 | 8ms | no | 106 |
| `jitter_spikes` | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 | all four tie | yes | 5 |
| `interruption_1s` | 300 / 10 / 0.00, 0.00, 0.00 | 300 / 10 / 0.00, 0.00, 0.00 | 300 / 10 / 0.00, 0.00, 0.00 | 300 / 10 / 0.00, 0.00, 0.00 | 300 / 10 / 0.00, 0.00, 0.00 | 300 | all four tie | yes | 14 |
| `interruption_5s` | 1495 / 50 / 0.00, 0.00, 0.00 | 1495 / 50 / 0.00, 0.00, 0.00 | 1495 / 50 / 0.00, 0.00, 0.00 | 1495 / 50 / 0.00, 0.00, 0.00 | 1495 / 50 / 0.00, 0.00, 0.00 | 1495 | all four tie | yes | 1 |
| `bandwidth_step` | 1876 / 100 / 0.00, 0.00, 2.14 | 1876 / 100 / 0.00, 0.00, 4.28 | 1876 / 100 / 0.00, 0.00, 8.57 | 1876 / 100 / 0.00, 0.00, 38.56 | 1876 / 100 / 0.00, 0.00, 2.14 | 1876 | 2ms | yes | 39 |
| `late_receive_thread` | 0 / 0 / 0.00, 0.00, 6.07 | 0 / 0 / 0.00, 0.00, 8.07 | 0 / 0 / 0.00, 0.00, 12.07 | 0 / 0 / 0.00, 0.00, 15.35 | 0 / 0 / 0.00, 0.00, 6.07 | 0 | 2ms | yes | 5 |
| `slow_decoder` | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 / 0 / 0.00, 0.00, 0.00 | 0 | all four tie | yes | 5 |

Read the table with these in mind:

- **In 11 of 16 conditions all four fixed waits tie** on lost frames and P99 (nothing out of order, or
  losses no wait can save). 2 ms is better on delay alone in 3 (light reordering, the bandwidth step, the
  late receive thread: there a longer wait holds the frames behind a given-up packet longer, P99 2.1 against
  38.6 ms at 16 ms in the bandwidth step). A longer wait wins only in the 2 conditions with heavy
  reordering: 8 ms in `heavy_reordering` (5 lost; 4 ms loses 9 at 2 ms less P99) and in
  `congestion_with_reordering` (0 lost against 19 at 2 ms). 16 ms never wins.
- **One lost frame per seed in every reordering cell is the library's known index-wrap drop** (video packet
  index 0 arriving while a gap is open across the 65535 to 0 wrap, at packet 131072 here; see "Finding in the
  pinned library" above): it is lost under every wait.
- **The two conditions that separate the waits are not like a wired LAN**: 5 % of packets 1-12 ms late,
  and a second path beside a filling queue. For a wired LAN the model's answer is 2 ms, with the adaptive rule
  widening only on evidence that a longer wait would have saved frames.

The adaptive rule was within margin in 14 of 16 conditions before and after the refinement; where not:

- `heavy_reordering`: 20 lost against 5 (best, 8 ms) and 9 (4 ms). Before the refinement it lost 127.
- `congestion_with_reordering`: 15 lost against 0. The impairment lasts 10 s and the rule starts at 2 ms:
  these are frames lost before any evidence exists. A rule that learns cannot avoid them; only starting
  wider can, at a cost on every other link.

Refinement (`include/veyra/remoteplay/ReorderWindow.h`, still 2-16 ms, still only with
`VEYRA_EXP_RP_REORDER_ADAPTIVE=1`): widening needs frames lost at 0.1 a second (was 0.5); a frame lost
*since the last change* (an old loss no longer carries the wait up step after step to the 99th
percentile); and, after the review, at least two of the frames lost since the last change, and at least
half of them, lost in a 250 ms look in which packets also arrived after being given up (evidence that a
longer wait would have helped). The review's case, one frame lost to the link every ~10 s beside 0.1 % of
packets 3-5 ms late, made the first refinement sit at 6 ms for up to 62 % of the time and one seed end
there; test `reorder_window_unrelated_losses_beside_late_packets_do_not_hold_it_wide` (5 seeds of 10 min:
ends at 2 ms, at most two changes; fails with the first refinement). Tried and dropped: one step per change,
a minimum count for the pay check, a share of timeouts that came late. Cost that remains in the earlier
simulator (`reorder_sim_results.json`, regenerated): `mixed` (loss bursts and frequent late packets
together; FEC completes the late ones) still widens and rolls back, P99 held delay 4.04 -> 7.65 ms with no
frame saved, because its late packets are frequent enough to sit beside most of its link losses: the
counters cannot tell those apart. `temporary_congestion` gives up 71 frames instead of 109. The table in
the section above is the rule before this change.

### Bottleneck classes against the study conditions (item 4)

Test `study_bottleneck_classes_match_each_condition` (`tests/remoteplay/LanStudyTests.inc`) runs every
condition and checks the class of every impaired interval, and that intervals from 10 s after the
impairment say `none` and grade stable. Three misclassifications found and fixed in
`include/veyra/remoteplay/ConnectionHealth.h` (thresholds still provisional, not calibrated on a PS5):

- heavy reordering with a 2 ms wait was named `loss` (frames FEC could not complete). Now a failed frame
  counts as loss only when no packet arrived after being given up; with late arrivals it is `reordering`
  (the wait was too short). New field `lateArrivals` (patch 0004's counter), appended to the health line.
- jitter spikes (2 % of frames stalled 5-30 ms) were named `none` in 11 of 12 intervals: the smoothed RFC
  3550 value had decayed by the end of the interval. `jitter` now also when one frame was 8 ms or more off
  its cadence (`frameJitterPeakMs`).
- a queue that drained after congestion released frames together and was named `decoder-backlog`. An
  ingress queue of 3 is now a decoder backlog only when the decode time is unknown or slow, or the frames
  did not arrive in a burst; a slow decoder is still named first.


### No accumulating latency (item 2, Linux model)

Tests `no_accumulating_latency_*` (`tests/remoteplay/LanStudyTests.inc`): four fixed seeds of 30 one-minute
segments, two simulated hours in all, each segment one random study condition from 10 s on (2-20 s; the
interruption 1, 5 or 8 s), then clean. The packets go through the reorder-queue model (fixed 2 ms, or the
adaptive rule), frames are handed over when 24 packets are out, then through the real `VideoIngress`
(original queue, or the 16-frame backlog), one decoder (3 ms a frame; 18 ms in a slow-decoder window) and
`StreamRecovery` polled every 50 ms (keyframe requests; reconnect after 6 s of silence, with its backoff;
the patient switch in one seed). The console answers a keyframe request with the first frame it sends 30 ms
later. Checked for every segment: the first frame is decoded within 6 s of the impairment's end; what the
receiver adds (reorder hold + ingress wait) is at most 1 ms from 2 s after the impairment (5 s after an
interruption); in the last 10 s every frame is decoded and none adds more than 1 ms; the last segment's tail
adds no more than the first one's (+0.1 ms); after the run and an hour of clean link the adaptive wait is
back at 2 ms by the end of the run (its last 5 minutes are clean) and after an hour of clean link; a
reconnect never leaves the wait wider than before it. The patient seed also has the console hold the old
session for 4 s after an interruption (the RP_IN_USE path above). In the runs: 0-2 reconnects and about
100-140 keyframe requests per seed, longest settle 0.3 s, worst tail 0 ms. Mutations that the checks catch: a
decoder at 17 ms a frame (all four runs fail); a wait that widens one step per reconnect (fails the three
runs that reconnect). `no_accumulating_latency_session_inbox_bursts_drain_every_time` sends 300
bursts of 1-4 frames through `SessionInbox` (real clock): empty after each, no recovery.

### Recovery without restart (item 3)

**(a) The `3/3` text.** The on-screen message is `std::format(L"…（{}/3）…",shown)` with the `3` inside the
Chinese source string that the catalog is keyed by (`i18n/catalog.json`, three entries). Showing the real
count (`4/8`) needs new source strings and their translations, which this stream may not add; the text is
unchanged (it reads `3/3` during the patient switch's extra attempts, and the `remoteplay-recovery` log line
names the real count). For whoever owns i18n: replace the three strings with `（{}/{}）` and pass
`recovery.maxEpisodeRetries()` in `src/source/RemotePlaySessionSource.cpp` (the `recovery_={...}` block).

**(b) After a GPU device loss the PS5 session is not resumed.** Code reading (not run):
`src/engine/EngineController.cpp:1731` (live scheduler failed: `status(..., true); break;` after waiting for
the removal), `:929` and `:1616` (backend and XeSS recovery give up when `GetDeviceRemovedReason` fails) end
the frame loop; teardown then closes the session (`:3391-3392`, `remote->close()`), and `run()` returns.
Nothing posts a new `run()`: `openRemotePlay` (`:204-208`) posts it once, and the connect description was
moved into the session at `:676` (its `decodeDevice` is the lost device; the pairing credentials are
move-only and wiped when the session's owner thread ends). Not fixable inside this stream's files (the loop,
the teardown and the restart are in `src/engine`). Design for `VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1`:
1. `RemotePlaySessionSource` (`include/veyra/source/RemotePlaySessionSource.h` + `.cpp`): a member
   `std::optional<RemotePlayConnectDesc> resume_` under `mutex_`; at the end of `run()`, when the switch is
   set and the stop came from `close()` (not a failure), move `desc` there instead of destroying it, with
   `decodeDevice` reset; `std::optional<RemotePlayConnectDesc> takeResume()` hands it over once. Credentials
   stay in memory only, wiped by the existing destructor if never taken.
2. `EngineController::run()`: remember `deviceLost` at `:1731` (and `:929`/`:1616` when the device is
   removed); after `remote->close()` at `:3392`, if `deviceLost` and the switch is set and `takeResume()` has a
   value, post `run(window, L"remoteplay:", opts, std::make_shared<...>(std::move(*resume)))` once more
   (`openRemotePlay`'s own post), at most 3 times in 10 minutes (a counter in `EngineController`), with a
   status line instead of the failure.
3. `StreamRecovery` is not involved: the new `run()` starts a new session (new device, new decoder, new
   inbox generation); the console sees a normal reconnect, as after the 6 s silence rule.
Needs Windows (device removal: `VEYRA_TEST_GPU_FAILURE_FRAMES` exists in the engine for the test) and a PS5.

**(c) The 60 s outage with `VEYRA_EXP_RP_RECONNECT_PATIENT=1`.** Already covered for a console that lets go
at once (`recovery_long_cable_pull_original_stops_patient_reconnects`). New test
`recovery_patient_sixty_second_outage_with_the_console_holding_the_session`: if the console still holds
the old session for 20 s after the link is back and refuses every connect (RP_IN_USE), all eight patient
attempts were spent (the last at 77 s) before it let go at 90 s: the session stopped. **Changed, patient
switch only:** such a refusal does not use up the outage's attempts, up to 4 per outage
(`StreamRecovery::pollRefusedInUse`, `kPatientInUseRetries`; each waits the RP_IN_USE backoff, at most 40 s;
the allowance is renewed with the attempts after 30 s of video), called from the connect-failed path in
`RemotePlaySessionSource.cpp` only when the switch is set and the refusal was RP_IN_USE. Model: nine attempts (backoffs 1, 2, 4, 8, 8, 8, 8, 8, 40 s), back
27.6 s after the console lets go (within one 40 s backoff); a console that never lets go still ends the session
after 12 attempts. Without the switch the call is never made (`recovery_refused_in_use_is_the_original_rule_without_the_patient_switch`).
Unverified on Windows and on a PS5: how a PS5 answers repeated session requests while it holds the old one
is not known.
## Latency governor, active control (`VEYRA_EXP_RP_GOVERNOR=apply`, experimental)

Experimental until it has been validated on hardware: nothing here has run on a GPU, a gaming PC or a PS5.
Portable logic: `include/veyra/engine/PushGovernorControl.h` (tests `governor_control_*` in
`tests/remoteplay/GovernorControlTests.inc`); wiring: `pushSlipstreamWatch` and `pushGpuBudgetRestore` in
`src/ui/bridge/PushSlipstream.cpp`.

Switches (read once per run):
- `VEYRA_EXP_RP_GOVERNOR` unset or `0`: nothing (as before). `=1`: observe only, exactly as before (the
  recommendation log above). `=apply`: active control as below; the observe-only `PushLatencyGovernor` is then
  not fed (its frame generation recommendations would compete).
- `VEYRA_EXP_RP_GOVERNOR_BUDGET_MS=<1..200>`: the latency budget (default 10 ms, hysteresis 30 % of it).
- `VEYRA_EXP_RP_GPU_BUDGET` keeps its meaning; with the governor on `apply` its own apply mode is superseded
  (logged once): the governor runs its own copy of the budget as decode/presentation protection.

What it changes: only the frame generation multiplier (`setFgMultiplier`, 1 = off) and PushFlow's tier
(`setVfgQuality`), from the user's request down (tiers, then lower multipliers, then off), read back after
setting (a setter that refused is not a user change). One ordered decision per second: decode and presentation
first (the GPU budget's decode-queue delay, missed deadlines, GPU load, generated-frame cost, skipped pairs),
then latency (the P95 "decoded picture to present call" time minus the hold interpolation plans,
(M-1)/M of a source interval: the unplanned delay). A latency step goes to the next lower multiplier, a
protection step to the next level. Not changed, and why: presentation/pacing mode and the low-latency
choices (the performance modes already set the latency-first values; a live swap-chain queue change may
recreate it), reorder/backlog/pad-poll switches (read when a PS5 session connects; not live), AI picture
enhancement and super resolution (their setters rebuild the graph: a visible stall).

Rules, each checked as an invariant by `pushGovernorInvariantViolation` over every simulator scenario (three
configurations) and 150 randomized 600 s traces with user changes, new streams and refused setters (two
configurations, fixed seeds): never above the user's request; a step up (more generated frames) only after a
whole healthy hold (20 s, doubled after a failed return, up to 8x) with the unplanned delay measured and under
budget minus hysteresis on every tick, no protection trouble, and the level above not predicted over that line
(its last reading within 4 holds, shifted by what the step down saved); at least 3 s between changes and at most
6 a minute (across streams too); a step down is judged 5 s later and rolled back to where it came from when its
target (the unplanned delay, the decode delay, the missed share or the GPU time) did not drop by 5 % and 0.5,
and that step is then locked for 60 s, doubling; 3 direction reversals within 2 min lock the governor out for
60 s, doubling (decode/presentation steps down may pass a lock-out, never the interval or the cap; see the
review fixes below for the decay and the probe); without a
source rate nothing happens, without its trigger measured no step is taken; a setting the user changes becomes
the request and is left alone until the next stream (both changed: suspended); a refused level is not asked
again for 60 s.

Restore: the user's choice is put back before a stop, before the optimiser or the quality test measures, and at
quit before the final save (`pushGpuBudgetRestore`, logged `[slipstream-governor] restored the user's choice
(...)`). While it applies, `pushFlowBudgetWatch` does not step PushFlow's tier. Apply is refused (one line)
without a measured decode-queue delay (capture cards).

Log: `[slipstream-governor] mode=apply action=step|rollback|none from= to= cause=decode|presentation|gpu-load|
latency|recovery|rollback reason= refused= unplannedMs= latencyP95Ms= budgetMs= level= protectLevel=
budgetReason= request= frozen= lockedOut= rollbacks= lockouts= changes= unavailable=[...] session=`: every action
and rollback at once, otherwise at most every 10 s or when the reason changes. Overlay: new `pushLiveStats` keys
only while the switch is set: `governorMode`, `governorApplying`, `governorMultiplier`, `governorTier`,
`governorRequestMultiplier`, `governorRequestTier`, `governorReduced`, `governorCause`, `governorReason`,
`governorUnplannedMs`, `governorBudgetMs`, `governorRollbacks`, `governorLockouts`, `governorChanges`,
`governorFrozenMultiplier`, `governorFrozenTier` (no QML reads them yet).

Inputs still unavailable: present lateness and late real frames, the PS5 owner thread's scheduling delay,
thermal state, decode-queue delay for capture cards, a GPU memory-pressure signal (the snapshot only has the
VRAM runaway watchdog, a leak detector; shedding frame generation would not fix a leak, so it is not used), and
a growing ingress queue as its own trigger (it grows on the network/decoder side; when decoding waits for the
GPU the decode-queue delay already shows it).

Simulator (`include/veyra/engine/PushGovernorSim.h`, `tools/remoteplay_latency_bench/governor_sim.cpp`,
results `tools/remoteplay_latency_bench/governor_sim_results.json`, deterministic). **Linux model**: invented
costs, one tick per second, PushFlow 3X Quality requested on a 30 fps source, closed loop (the governor's level
changes the modelled GPU load, backlog and latency it reads next). "none" keeps the request; "ideal" is an
oracle that knows each second's world and picks the most work whose steady unplanned delay fits the budget.
Latency columns are the model's P95 "decoded picture to present call" time, not a measurement.

| scenario (Linux model, 300 s) | changes (down/up) | rollbacks | lock-outs | reversals | mean latency gov / none / ideal (ms) | P95 gov / none / ideal (ms) | s over budget gov / none / ideal | output fps gov / none / ideal | s at level 0/1/2/3/4 | invariants |
|---|---|---|---|---|---|---|---|---|---|---|
| clean-lan | 0 (0/0) | 0 | 0 | 0 | 28.3 / 28.3 / 28.3 | 28.8 / 28.8 / 28.8 | 0 / 0 / 0 | 90 / 90 / 90 | 300/0/0/0/0 | hold |
| jitter-spikes | 8 (4/4) | 0 | 2 | 7 | 26.2 / 29.3 / 25.9 | 35.3 / 35.6 / 28.8 | 42 / 42 / 42 | 77 / 90 / 82 | 168/0/0/132/0 | hold |
| burst-loss | 0 (0/0) | 0 | 0 | 0 | 28.8 / 28.8 / 27.8 | 28.8 / 28.8 / 28.8 | 12 / 12 / 12 | 90 / 90 / 88 | 300/0/0/0/0 | hold |
| congestion-then-recovery | 2 (1/1) | 0 | 0 | 1 | 26.7 / 29.9 / 29.7 | 28.8 / 32.6 / 32.1 | 2 / 54 / 0 | 76 / 90 / 90 | 164/0/0/136/0 | hold |
| gpu-overload-ramp-and-release | 4 (2/2) | 0 | 0 | 1 | 20.9 / 36.3 / 24.0 | 30.5 / 66.6 / 31.9 | 4 / 111 / 0 | 66 / 88 / 75 | 160/0/0/39/101 | hold |
| thermal-like-slow-drift | 1 (1/0) | 0 | 0 | 0 | 27.9 / 31.6 / 30.7 | 31.6 / 34.5 / 32.0 | 3 / 125 / 0 | 77 / 90 / 90 | 167/0/0/133/0 | hold |
| decode-stall | 7 (5/2) | 1 | 1 | 3 | 26.8 / 30.8 / 28.4 | 53.1 / 53.5 / 28.8 | 30 / 30 / 30 | 76 / 90 / 84 | 162/15/5/98/20 | hold |
| cadence-switch-60-30 | 5 (3/2) | 0 | 0 | 4 | 21.2 / 21.7 / 21.7 | 28.7 / 28.7 / 28.7 | 0 / 0 / 0 | 144 / 144 / 144 | 127/173/0/0/0 | hold |
| flapping-at-threshold | 0 (0/0) | 0 | 0 | 0 | 30.7 / 30.7 / 30.7 | 31.4 / 31.4 / 31.4 | 0 / 0 / 0 | 90 / 90 / 90 | 300/0/0/0/0 | hold |
| unknown-or-missing-inputs | 3 (3/0) | 0 | 0 | 0 | 16.2 / 37.1 / 23.0 | 35.1 / 73.7 / 28.8 | 74 / 84 / 0 | 56 / 88 / 74 | 62/64/0/7/167 | hold |
| everything-at-once | 6 (5/1) | 1 | 0 | 2 | 17.2 / 49.8 / 19.4 | 35.5 / 106.2 / 31.1 | 77 / 192 / 76 | 69 / 111 / 93 | 71/5/14/13/197 | hold |
Randomized: 400 traces x 600 s, 1243 changes, 0 invariant violations. Periodic short bursts (18 ms jitter for
6 s every 30-150 s, 24 h, `pushRunBurstSim`): reduced 45.9 % of the time, 612 changes, 169 lock-outs (before the
back-off decay below: about 100 %). Read with care: lower latency in the model mostly means fewer generated frames
(see output fps); network jitter that ends inside the 5 s judgement window can be credited to a step that did not
cause the improvement, and a load that keeps rising during the window can make a helpful step look useless.
Reacting to every burst longer than the 3 s sustain is what the budget asks for; a longer `sustainMs` would
ignore such bursts (a tuning decision, not made here). Mutation checks run
by hand (each one made the simulator or tests fail): no minimum interval, no per-minute cap, no lock-out, no
healthy hold, ignoring a frozen multiplier, stepping up on unknown latency, rolling back to the wrong level,
stepping down on equal protection level, acting without a source rate, no prediction of the level above.
## Audio, round 2: end-to-end model and three fixes inside `VEYRA_EXP_RP_AUDIO_FAST_RECOVER` (branch `exp/s3-audio2`, 2026-11-19)

No new switch; with `VEYRA_EXP_RP_AUDIO_FAST_RECOVER` unset nothing changes. A Linux model of the whole audio
path (`include/veyra/sink/AudioPathModel.h`, tests `tests/remoteplay/AudioSoakTests.inc`, table from
`tools/remoteplay_latency_bench/audio_sim.cpp` in `docs/AUDIO.md`) found three defects in the experiment,
now fixed: a discontinuity in the drift estimate's first 2 s became its reference (audio ~90 ms late for
the session); a device clock stall was recovered by seconds of 5 ms cuts and could be cut twice (audio up
to 55 ms early); a burst re-anchor during an endpoint reopen was followed at 2 ms/s. With the switch set,
the controller now holds while the device clock stands still and removes the stall's delay in one masked
cut when it runs again, and every reset rebases the drift estimate (`CaptureAudioSession.cpp`, one line).
After review, a fourth: with the low buffer the controller reset after every device clock stall (a cut
heard before its pending deadline counted twice); an early reset now needs the raw error early too.
The low buffer stays a separate, riskier switch (more underruns, its gain is outside the model).
Model only: unverified on Windows, not heard. The listening list for the owner is at the end of
`docs/AUDIO.md`.
at once (`recovery_long_cable_pull_original_stops_patient_reconnects`). How a refusal reaches the app:
`connect()` only starts the session thread, so while the console still holds the old session `connect()`
returns true and RP_IN_USE arrives later as the session failing with `startupRetryAllowed` set, polled in the
two owner loops (`RemotePlaySessionSource.cpp`, controller-only and streaming). A `connect()` that fails has
closed the backend, so `startupRetryAllowed` is never set on that path. Test
`recovery_patient_sixty_second_outage_with_the_console_holding_the_session` models exactly that: if the console
holds the old session for 20 s after the link is back, the eight patient attempts were all spent before it
let go and the session stopped. **Changed, patient switch only:** such a refusal does not use up the outage's
attempts, up to 4 per outage (`StreamRecovery::pollRefusedInUse`, called in place of `poll` with the same
arguments in both loops only when the switch is set; each refusal waits the RP_IN_USE backoff, at most 40 s;
the allowance is renewed with the attempts after 30 s of video). The `[remoteplay-recovery]` line "waiting Ns
before retry n/m" counts these refusals too (it showed `8/8` repeatedly). Model: nine attempts (backoffs 1, 2,
4, 8, 8, 8, 8, 8, 40 s), back 27.8 s after the console lets go; a console that never lets go still ends the
session after 12 attempts. The patient long run of item 2 also passes a 4 s held session through this path.
Without the switch `pollRefusedInUse` is never called and decides as `poll` anyway
(`recovery_refused_in_use_is_the_original_rule_without_the_patient_switch`). Unverified on Windows and on a
PS5: how a PS5 answers repeated session requests while it holds the old one is not known.

Review fixes (19 November 2026):
- A user change while reduced makes only the changed setting the new request; the other keeps the user's
  own value (request 3x/t2, governor at 2x/t0, user picks tier 1 -> request 3x/t1).
- Crash safety: preference `pushGovernorRestore` (`{"method","multiplier","tier"}`) is written before the
  first reduced level is set (governor and GPU budget `=apply`), cleared ("{}") when the request is back or the
  user's own choice is in force, and put back at the first watch tick of the next run whatever the switches say
  (nothing happens unless it was written; a record for another method is dropped).
- The first-launch calibration (`pushCalibrationRestore` non-empty and not "{}") pauses both watches like the
  optimiser, after the user's choice is put back. Remaining window: the optimiser and the calibration snapshot
  the settings in force when they start, up to one tick (1 s) before the watch sees their preference, so that
  snapshot can hold a reduced level.
- After a protection step (decode, presentation, GPU load) the return line is the budget itself, not budget
  minus hysteresis: a stream that sits at 8 ms of a 10 ms budget gets its request back.
- Back-offs decay: each full 20 s healthy hold forgets one failed return; a return counts as failed only when
  the level above did not last two holds; two oscillation windows without a reversal forget one lock-out
  doubling. While reduced and healthy, a probe step up comes at most every 5 min (doubling after a failed probe,
  up to 4x) even before a full hold; it is judged after 5 s and rolled back when the level went over the budget.
- The invariant checker recomputes the unplanned delay from the recorded latency, multiplier and source rate
  (it no longer trusts the controller's figure; the protection level is still the controller's).
- `VEYRA_EXP_RP_GOVERNOR_BUDGET_MS` is parsed as decimal.

Not one arbiter for everything: the performance modes' driver (`PushModeDriver.qml`, re-applying a mode when the
source rate changes) and Automatic mode (`VEYRA_EXP_AUTO_MODE`) also set the multiplier. The governor cannot
tell those from the user: it takes their change as a user change, makes it the request and leaves that setting
alone for the rest of the stream. Running Automatic mode together with `VEYRA_EXP_RP_GOVERNOR=apply` is
therefore not useful.
### Widening only on per-frame evidence (second review, Linux model)

The review ran one lost frame every 8, 10 or 12 s (bursts of 5 or 30 packets) beside 0.1 % of packets 3-5 ms
late: 11 of 60 runs ended at 6 ms, wide for 96-99 % of the run. Two causes, both fixed in
`ReorderWindow.h`: a loss was counted as "explained" by any late packet in the same 250 ms look, and
narrowing needed the lateness percentile below the lower step, which 3-5 ms packets never are. Now:

- Widening needs two frames lost since the last change to **one of their own packets** that was given up
  and arrived afterwards, all of whose needed packets reached the socket in the end (a longer wait would
  have completed them): new evidence field `ReorderEvidence::lateFramesFailed`, computed per frame by the
  models (`ReorderStudy.h`, `reorder_sim.cpp`).
- A period without such losses narrows one step, whatever the lateness, unless the step was reached by a
  widening whose pay check passed.

**In the app `lateFramesFailed` is 0**: chiaki patch 0004 does not link a late arrival to the frame it
belonged to, so with `VEYRA_EXP_RP_REORDER_ADAPTIVE=1` the wait now only narrows (from 16 ms without the LAN
profile) and never widens. Counting it in the library (frame index of a late-arriving skipped packet,
compared with the frames reported lost) is the next step and needs a patch change and a PS5.

Test `reorder_window_unrelated_losses_beside_late_packets_do_not_hold_it_wide`: the review's matrix
(spacing 8/10/12 s x bursts 5/30 x 10 seeds of 300 s, seeds apart from the other tests): every run ends at 2
ms, wide at most 5 % of the time, never more frames lost than the fixed 2 ms; it fails with the previous
rule. Re-run: `reorder_study_results.json` unchanged (no adaptive number moved). `reorder_sim_results.json`
regenerated: `mixed` no longer widens (P99 held 7.65 -> 4.04 ms, as fixed 2 ms); `temporary_congestion`
gives up 71 frames (fixed 2 ms: 1130).

Second review (19 November 2026), policy changes; this table supersedes the one above (Linux model,
`governor_sim_results.json`):
- Cause first: a latency step needs the unplanned delay minus the measured network jitter over the budget
  (jitter not measured = no latency step); network jitter and loss never trigger one. In the reviewer's burst trace
  (`pushRunBurstSim`, 24 h of 6 s jitter bursts) the governor now makes 0 changes (5766 s over budget with and
  without it: no frame generation setting can help there).
- A latency step only lowers the multiplier, never a tier. Tier changes (graph rebuilds, a hitch each) come only
  from GPU-cost protection, at most one per 60 s (`tierIntervalMs`).
- Restore record format 2 stores the reduced level set; at start it is applied only when the settings in force
  are exactly that level, else dropped; a refused setter is logged as "could not restore".
- The calibration pause is only in the experimental watch (`pushSlipstreamWatch`); `pushFlowBudgetWatch` is as in
  exp/rc3 before this work, so with every switch unset nothing changes.
- Not done: crediting a step only when the improvement outlasts its triggering episode (jitter is now discounted
  instead, which removes the main misattribution).

| scenario | changes | graph rebuilds | down/up | rollbacks | lock-outs | reversals | mean latency gov / none / ideal (ms) | P95 gov / none / ideal (ms) | s over budget gov / none / ideal | output fps gov / none / ideal | seconds at level 0..4 | invariants |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| clean-lan | 0 | 0 | 0/0 | 0 | 0 | 0 | 28.33 / 28.33 / 28.33 | 28.78 / 28.78 / 28.78 | 0 / 0 / 0 | 90.00 / 90.00 / 90.00 | 300/0/0/0/0 | hold |
| jitter-spikes | 0 | 0 | 0/0 | 0 | 0 | 0 | 29.33 / 29.33 / 25.87 | 35.56 / 35.56 / 28.79 | 42 / 42 / 42 | 90.00 / 90.00 / 81.60 | 300/0/0/0/0 | hold |
| burst-loss | 0 | 0 | 0/0 | 0 | 0 | 0 | 28.82 / 28.82 / 27.83 | 28.84 / 28.84 / 28.79 | 12 / 12 / 12 | 90.00 / 90.00 / 87.60 | 300/0/0/0/0 | hold |
| congestion-then-recovery | 0 | 0 | 0/0 | 0 | 0 | 0 | 29.87 / 29.87 / 29.71 | 32.57 / 32.57 / 32.11 | 54 / 54 / 0 | 90.00 / 90.00 / 90.00 | 300/0/0/0/0 | hold |
| gpu-overload-ramp-and-release | 4 | 4 | 2/2 | 0 | 0 | 1 | 21.48 / 36.33 / 23.98 | 30.70 / 66.56 / 31.86 | 10 / 111 / 0 | 66.80 / 88.14 / 75.00 | 161/0/0/46/93 | hold |
| thermal-like-slow-drift | 1 | 1 | 1/0 | 0 | 0 | 0 | 28.34 / 31.60 / 30.72 | 32.01 / 34.54 / 32.01 | 12 / 125 / 0 | 78.50 / 90.00 / 90.00 | 184/0/0/116/0 | hold |
| decode-stall | 5 | 5 | 3/2 | 1 | 0 | 3 | 30.35 / 30.84 / 28.37 | 53.08 / 53.45 / 28.82 | 30 / 30 / 30 | 90.00 / 90.00 / 84.00 | 162/96/42/0/0 | hold |
| cadence-switch-60-30 | 5 | 5 | 3/2 | 0 | 0 | 4 | 21.19 / 21.71 / 21.71 | 28.74 / 28.74 / 28.74 | 0 / 0 / 0 | 143.86 / 143.86 / 143.86 | 127/173/0/0/0 | hold |
| flapping-at-threshold | 0 | 0 | 0/0 | 0 | 0 | 0 | 30.72 / 30.72 / 30.72 | 31.35 / 31.35 / 31.35 | 0 / 0 / 0 | 90.00 / 90.00 / 90.00 | 300/0/0/0/0 | hold |
| unknown-or-missing-inputs | 3 | 3 | 3/0 | 0 | 0 | 0 | 16.23 / 37.09 / 23.04 | 35.11 / 73.69 / 28.77 | 74 / 84 / 0 | 56.01 / 87.70 / 74.00 | 62/64/0/7/167 | hold |
| everything-at-once | 2 | 2 | 2/0 | 0 | 0 | 0 | 16.11 / 49.76 / 19.40 | 30.72 / 106.17 / 31.05 | 79 / 192 / 76 | 61.94 / 110.97 / 92.91 | 26/0/0/82/192 | hold |
