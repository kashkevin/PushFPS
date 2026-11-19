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
   and every luma sample (the pass keeps the previous luma plane in a texture of its own). 32 bytes
   are read back: largest block difference, mean difference, share of changed blocks, largest
   sample difference, number of samples that moved by more than 4 steps.
2. `engine/PushStreamDedup.h` gives the measurement to `remoteplay/StreamCadence.h`, which looks for
   the high-low rhythm of a doubled stream in the block numbers.
3. Only while that rhythm is trusted does a frame get a picture number; a repeat gets the number of
   the frame before it and `ContentDedup` drops it before any other GPU work. A repeat must stand in
   the repeat's place in the rhythm and look like the same picture coded again sample by sample:
   no luma sample moved by more than 16 steps (`pixelCeiling`) and far fewer samples moved than in
   the picture before it.

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
  is not seen: an object fainter than that drawn at 60 fps over a world drawn at 30, or a slow fade
  at 60 fps over one. An object 24 steps from its background is seen (adversarial set).
- Only luma is measured. A change in colour alone is invisible to it.

Cost: one small dispatch and one bounded wait per stream picture, on the engine thread, in
`StreamChangeRunner::measure` (`pipeline/StreamChangePass.h`), and only while frame generation runs
and duplicate removal is active. The wait ends when the GPU is done and is capped at the smaller of
4 ms and the time until the live scheduler's next wake (`pipeline::streamChangeWaitBudget`): the
same thread presents frames at exact times, so a presentation that is due is never held up, and
then the measurement is not waited for at all. The cap is a high-resolution waitable timer waited
for together with the fence's event (as `engine/DeadlineWait.h`); a millisecond timeout alone would
return on the scheduler tick, up to 15.6 ms later. The pass queues behind GPU work already
submitted, so the wait is longest when the GPU is busiest; a measurement that misses it is used one
frame late for the rhythm only, and that frame is kept. How long the wait really is on the RTX 5070,
and how many pictures miss it because a presentation was due, is not known: `measureWaitMeanMs` /
`measureWaitMaxMs` / `gpuTimeouts` in the log say.

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

What to check first on the PC: the app starts with the switch set (shader loads), `gpuFailures=0`,
a 60 fps game stays `state=unique` with `repeatsFound=0`, and `measureWaitMaxMs` / `gpuTimeouts`.
Then the open question: on a 30 fps game, how many repeats does it remove? `state=doubled` with
`sourceFps` near 30 and `repeatsRemoved` near 300 per 10 s means the PS5 encoder's refinement stays
under 16 steps per sample; a high `keptUnsure` with `sourceFps` near 60 means it does not, and the
experiment does little on that stream. The per-picture log (`pixelMax`, `pixelsOver`) shows which.

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
