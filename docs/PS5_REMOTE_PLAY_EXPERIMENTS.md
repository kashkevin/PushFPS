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
