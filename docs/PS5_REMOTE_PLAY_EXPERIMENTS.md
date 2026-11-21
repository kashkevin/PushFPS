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
above is the single-profile one the unit tests still use), the smoothing window is 8 ms as in the first round (it was 33 ms from the second round until 19 November 2026; 33 is now the opt-in smoother camera, `VEYRA_EXP_RP_KBM_SMOOTH_MS=33` or `smooth_ms=33`),
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

**What `VEYRA_EXP_RP_REORDER_ADAPTIVE=1` does in the app today, exactly.** `reorderEvidence()`
(`ReorderWindow.h`) never sets `lateFramesFailed`, and chiaki patch 0004 exposes no link between a late
packet and the frame it belonged to, so the controller never widens in the app:

- with `VEYRA_EXP_RP_LAN_PROFILE=1` it starts at 2 ms and stays there: the same as the LAN profile's
  fixed 2 ms wait;
- without the LAN profile it starts at the library's 16 ms and narrows one step per 30 s period (16, 12, 8,
  6, 4, 3, 2 ms) to 2 ms, whatever the reordering: under heavy reordering that ends at the study's worst
  choice (2 ms), after minutes at waits that only add delay on a clean link;
- it can still be wider than 2 ms through the fault path: counters that contradict each other, or a library
  that reports another wait, set 16 ms for 60 s; the third time keeps 16 ms for the rest of the session.

Consequence: until the library patch reports per-frame late-packet evidence, the adaptive switch has no
benefit over the LAN profile on a wired LAN and should not be used, with or without the profile. Starting
the no-profile case at 2 ms was not done: it is the LAN profile's wait under another switch, and the
start value follows the configured wait by design (a change of default behaviour of the switch, not a
one-line fix). The model results above (widening on heavy reordering) describe what the rule would do once
the evidence exists.

Final review: a paid widening protected its step for good when its pay check passed by chance on rare
losses, and two genuine late-packet losses minutes apart could widen. Now the two losses must be within
60 s of each other, and a paid step gives way after 3 periods in a row without a frame lost to a late packet
(`kExplainedWindow100ns`, `kPaidQuietPeriods`). The reviewer's 60-run matrix (600 s) ends at 2 ms in every run,
never wide; test `reorder_window_a_paid_widening_expires_when_no_longer_needed` (12 s x 5 packets, 20 seeds of
1800 s: ends at 2 ms, wide at most 5 %). Study and simulator JSON unchanged by this.

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

**Status: `VEYRA_EXP_RP_GOVERNOR=apply` is NOT recommended** until it has been re-reviewed and tried on
hardware. `VEYRA_EXP_RP_GOVERNOR=1` (observe only) is the supported mode.

Third review (19 November 2026), changes:
- Ladder: lower multipliers keep the request's tier; tiers are lowered only at the lowest multiplier. A latency
  step therefore never changes the tier; only GPU-cost protection can.
- Every tier change (protection, recovery, probe, rollback) waits `tierIntervalMs` (60 s) after the last one:
  each rebuilds the graph.
- While the network bursts (jitter over 3 ms or loss over 0.5 %), the embedded GPU budget is not fed and no
  latency step is attributed: a decode stall that coincides with a burst is the clump of late frames. The
  governor holds and counts as "in trouble" (no step up).
- After 2 steps in a stream that were rolled back as useless, the governor stands down until the next stream
  (`stood-down`). The step it stood down on was the rolled-back one, so the request is normally in force; the
  stop/quit restore covers the rest. Crediting a step against a quiet-period baseline after its episode has
  ended (the reviewer's 1(c)) is NOT done; the stand-down limits the cost instead.
- Known limit found while testing: after a protection episode the embedded budget can stay at a lower level of
  its own (its step-up prediction refuses), which keeps the governor from returning until the next stream.
- Note: `pushFlowBudgetWatch` (the original PushFlow tier watch) does not pause for the first-launch calibration,
  as before this work; it can lower PushFlow's tier during a calibration run, as during any measurement except
  the optimiser.

Reviewer's 24 h traces (their harness, before these changes; over-budget seconds with / without the governor):
slow GPU 28 / 86,400 (the governor pays); GPU sine 1,477 / 21,447 (pays, but a graph rebuild about every 2.5 min
before the tier spacing); pure jitter bursts 5,766 / 5,766 with 0 changes (no gain, no cost); bursts with decode
stall 9,153 / 9,153 with 620 rebuilds and 67 % of the time reduced, and bursts with loss + stall: generation off
99.9 % of the day, both for no gain (both fixed in the model below: 0 changes).

Simulator after these changes (Linux model; bursts with decode stall and with loss + stall: 0 changes, 0
rebuilds, tested over 6 h):

| scenario | changes | graph rebuilds | down/up | rollbacks | lock-outs | reversals | mean latency gov / none / ideal (ms) | P95 gov / none / ideal (ms) | s over budget gov / none / ideal | output fps gov / none / ideal | seconds at level 0..4 | invariants |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| clean-lan | 0 | 0 | 0/0 | 0 | 0 | 0 | 28.33 / 28.33 / 28.33 | 28.78 / 28.78 / 28.78 | 0 / 0 / 0 | 90.00 / 90.00 / 90.00 | 300/0/0/0/0 | hold |
| jitter-spikes | 0 | 0 | 0/0 | 0 | 0 | 0 | 29.33 / 29.33 / 25.87 | 35.56 / 35.56 / 28.79 | 42 / 42 / 42 | 90.00 / 90.00 / 81.60 | 300/0/0/0/0 | hold |
| burst-loss | 0 | 0 | 0/0 | 0 | 0 | 0 | 28.82 / 28.82 / 27.83 | 28.84 / 28.84 / 28.79 | 12 / 12 / 12 | 90.00 / 90.00 / 87.60 | 300/0/0/0/0 | hold |
| congestion-then-recovery | 0 | 0 | 0/0 | 0 | 0 | 0 | 29.87 / 29.87 / 28.71 | 32.57 / 32.57 / 32.11 | 54 / 54 / 0 | 90.00 / 90.00 / 84.60 | 300/0/0/0/0 | hold |
| gpu-overload-ramp-and-release | 5 | 2 | 3/2 | 1 | 1 | 3 | 19.65 / 36.33 / 23.31 | 31.01 / 66.56 / 30.71 | 17 / 111 / 0 | 58.80 / 88.14 / 71.40 | 93/101/0/0/106 | hold |
| thermal-like-slow-drift | 4 | 1 | 3/1 | 1 | 0 | 2 | 25.12 / 31.60 / 28.40 | 33.08 / 34.54 / 31.75 | 56 / 125 / 0 | 75.20 / 90.00 / 77.50 | 220/10/0/0/70 | hold |
| decode-stall | 4 | 0 | 2/2 | 2 | 1 | 3 | 30.56 / 30.84 / 28.37 | 48.23 / 53.45 / 28.82 | 30 / 30 / 30 | 88.50 / 90.00 / 84.00 | 285/15/0/0/0 | hold |
| cadence-switch-60-30 | 5 | 0 | 3/2 | 0 | 0 | 4 | 20.09 / 21.71 / 21.71 | 28.74 / 28.74 / 28.74 | 0 / 0 / 0 | 109.69 / 143.86 / 143.86 | 127/173/0/0/0 | hold |
| flapping-at-threshold | 0 | 0 | 0/0 | 0 | 0 | 0 | 30.72 / 30.72 / 30.72 | 31.35 / 31.35 / 31.35 | 0 / 0 / 0 | 90.00 / 90.00 / 90.00 | 300/0/0/0/0 | hold |
| unknown-or-missing-inputs | 2 | 1 | 2/0 | 0 | 0 | 0 | 15.09 / 37.09 / 23.04 | 31.90 / 73.69 / 28.77 | 67 / 84 / 0 | 48.91 / 87.70 / 74.00 | 62/64/0/0/174 | hold |
| everything-at-once | 5 | 4 | 4/1 | 1 | 0 | 2 | 22.22 / 49.76 / 18.77 | 43.71 / 106.17 / 30.34 | 140 / 192 / 76 | 71.70 / 110.97 / 75.12 | 78/6/73/0/143 | hold |

## Sprint 4: measurement and automation (stream "auto", branch `exp/s4-auto`, 2026-11-20)

Design and reasoning: `docs/LATENCY_MEASUREMENT.md`. All of it unverified on Windows; nothing ran on a PS5.

| Switch | Effect | Unset |
|---|---|---|
| `VEYRA_EXP_RP_STATS=1` | latency test v2 for every test (randomised gaps, per-press split), `[slipstream-press]` per answered press, `[slipstream-latency-test]` per test, `[log-summary]` every 60 s | none of these lines; the test runs as before |
| `VEYRA_TEST_RP_AUTOCONNECT=1` | once at start: `ps5Load` + `ps5Connect` on the most recently used console (the `resumeLastSource` path); `[slipstream-autorun] step=0 name=connect state=failed reason=no-console` when none is registered | nothing |
| `VEYRA_TEST_RP_AUTORUN=<plan>` | runs the plan once a PS5 stream presents (grammar and limits in `include/veyra/engine/PushAutorunPlan.h`) | nothing |

Latency test v2 is also used, without the switch, when the dialog or a plan asks for more than 20 presses or for
the `options` / `dpad-right-left` button (then without the `[slipstream-*]` lines). PS button, at most 20 presses,
no switch: exactly the original test.

Plan: `settle:<1..3600 s>`, `latency:<3..500>[:ps|options|dpad-right-left]`, `wait:<0..3600 s>`,
`sr:<1..600 s>`, `fg:<1..600 s>`, `quality`, `exit`; `;` between steps, at most 32 steps and 1024 bytes. An
unknown step or a bad argument fails that step and stops the plan. Hook steps (`sr`, `fg`, `quality`) are run by
a handler another part registers with `engine::autorun::registerStep(name, handler)` (called with Start, then Poll
on every tick until Done / Failed, Cancel on timeout or when the program closes); without one the step fails with
`reason=unsupported`. Timeouts: settle s+120 s, wait s+10 s, latency 5 s per press + 60 s, quality 600 s, sr/fg
max(600 s, 16 x s + 60 s). Never during the optimiser, the frame quality test or the calibration
(`reason=busy`). A plan saves no preference. `exit` closes the main window (the `--exit-after` path, 20 s quit
fallback); a plan that fails still closes the app when it contained `exit`, so the kit never waits.

Lines (`[family] key=value`, unknown = -1 / unknown):
- `[slipstream-autorun] plan steps= valid= exit=`; `step=<i> name=<step> state=start|done|failed reason=<word>`
  (start: `none`, done: `ok`); `finished ok=<0|1> steps=<n>`; at close while running: `reason=closing`.
- `[slipstream-config]` (always, once per session and on any change, checked once a second while a session runs):
  the contract keys, then `modeSource=user|auto|none fgActive= srActive=`. `fg`/`fgMethod`/`fgMultiplier`/`fgTier`
  are the configured values also while generation is off; `sr` 0 off, 1 DLSS SR, 2 RTX Video SR, 3 FSR, 4 NIS;
  `srQuality` the raw `videoSrQuality`; `window` the main window in physical pixels, `output` its screen's mode;
  `hdr` an HDR stream or source or RTX Video HDR active; `decoder` hardware / software / software-fallback /
  unknown; `route` from the console address's link class (the address itself is never logged).
- `[slipstream-press] n= sendToArrivalMs= arrivalToScanoutMs=-1 totalMs= phaseMs= arrivalToPresentMs=` (scanout
  per frame is not known to the test in this build; totalMs is then send -> present).
- `[slipstream-latency-test] trials= answered= button= consoleP50= consoleP95= pcP50= pcP95= totalP50= totalP95=
  pcBasis=present|scanout|none` then, for console, pc and total: `<part>N= <part>Trimmed= <part>CiLow= <part>CiHigh=
  <part>Detectable=`.
- `[log-summary] errors= warnings= when=periodic|exit` (warning/error lines since start; at exit always).
- `[push-latency] v2 button= randomGapMs=0..<ms> split=1` right after the unchanged start line (v2 only).

Example plan for one arm: `VEYRA_EXP_RP_STATS=1 VEYRA_TEST_RP_AUTOCONNECT=1
VEYRA_TEST_RP_AUTORUN="settle:20;latency:200:dpad-right-left;wait:5;latency:50;exit"`.
## Super resolution: statistics, level override, level budget, autorun `sr` / `fg` / `quality` (sprint 4, stream "sr")

Full explanation, code references and the hardware table to fill: `docs/SUPER_RESOLUTION.md`. By code reading, the
default SR target is 4K (3840x2160) whatever the window size, so on a 1440p screen RTX Video SR, colour and frame
generation run at 2.25x the pixels the display shows (noise reduction does not: by default its Realtime size policy
caps it at 1080 lines and its result is resized to the work size, EnhancementSettings.h:278/464,
ResolutionPlan.h:23-24, 143-152), and the SR stage's GPU time is on the latency path (it runs once per real frame,
never on generated frames). "Optimise for my PC" turns on RTX level 2 at the current target, so it measured super
resolution at the CURRENT target (4K unless changed) (PushOptimizeDialog.qml:120, 423). The owner's actual SR settings are unknown: the evidence logs
contain no SR lines. Nothing here is measured on any GPU; all of it is unverified on Windows.

- `VEYRA_EXP_RP_STATS=1`: `[slipstream-sr]` (S4_CONTRACT.md keys, then `gpuMax work hdr dual levelSource budgetMs`)
  every 5 s and at each configuration change while an SR choice is active, from the existing GPU timestamps
  (`EnhanceGraph::noteSrTimings`); `[slipstream-fg]` every 5 s while frame generation is on. Off: no extra work.
  `[slipstream-sr]` brackets only the SR stage (with its encode/decode passes and, for Video Effects, the CUDA round
  trip), not the extra 4K cost of frame generation, colour and the final shrink: read each hold's in-app
  `latencyP95Ms` beside it.
- `VEYRA_EXP_SR_QUALITY=1..4`: the standard RTX Video SR pass evaluates that level for the whole run (setting and
  session untouched, no graph rebuild: the level is an evaluate-time SDK parameter). Other choices and double SR: no effect.
- `VEYRA_EXP_SR_BUDGET_MS=<0.5..50>`: one level down after 2 windows (5 s each) with the SR stage's P95 over the
  budget, one up after 6 windows under 60 % of it, 10 s between changes, no step up within 60 s of a step down (doubled,
  up to 16x, after each step up that had to be taken back); once that lockout has passed, 6 windows merely under the
  budget also try one level up (delta2 review: a lower level at 2.0 ms of a 3 ms budget kept the fitting 2.8 ms level
  off for hours; Linux model now: back after 85 s; a try that goes over is taken back, 6 to 8 changes an hour after
  the first hour), never above the user's level; the evaluated level follows
  the budget after every window, also when the user picks another level; at level 1 it only logs
  `event=budget-floor`. `VEYRA_EXP_SR_QUALITY` and an autorun hold win. If the SDK silently ignores a per-evaluate
  level, all four `rtx` holds of the `sr` step measure the same and the budget walks to the floor (see the Risks in
  docs/SUPER_RESOLUTION.md).
- Autorun hook steps (registered by `registerPushCompareAutorunSteps`, src/ui/bridge/PushCompare.cpp): `sr:<s>` holds
  off, NIS, FSR, RTX level 1-4 at the user's target and, when the window is smaller, level 1-4 at the window's target;
  `fg:<s>` holds frame generation off and each multiplier; `quality` runs the frame quality test. The user's settings
  come back at the end, on failure and on cancel (for `fg` also the multiplier kept while frame generation is off);
  preferences are never written, and the session file is not written while a step runs (`PushHookHold.h`), so a crash
  during a hold leaves the user's own settings there (except: a user switching the mode during a hold reaches two direct
  session saves that bypass the gate, Chain.cpp:261, 264). `fg`: a kept multiplier the current method does not offer
  cannot be put back (`event=restored ok=0 reason=multiplier-not-restorable`); hold lines end in `adaptive=1` when
  the adaptive multiplier is on (the multiplier in force can then be below the labelled one).
- Not built: `VEYRA_EXP_SR_REAL_ONLY` (SR already runs on real frames only).
## Presentation latency instrument and experiments (sprint 4, stream "present")

Full analysis: `docs/PRESENTATION_LATENCY.md` (code reading with file:line, BENCH1's present->scanout explained
term by term, Linux model `tools/remoteplay_latency_bench/present_sim.cpp`). All unverified on Windows.

- `VEYRA_EXP_RP_STATS=1`: `[slipstream-latency]` every 5 s while presenting (arrival -> decoded -> pickup ->
  Present call -> Present return -> scanout dated by DXGI frame statistics, P50/P95 per stage, arrival->scanout
  P50/P95/P99/max, `scanoutKnown`, the present contract and `flip=`), `[slipstream-swapchain]` on swap-chain
  creation, pacing change and resize (flip model, buffers, format, waitable, tearing support, sync interval,
  flags, whether the top-level window / the video window cover the monitor), and `[slipstream-present]
  mediaStatistics hr=...` once per chain (why `compositionMode` is -1). Portable part and tests:
  `include/veyra/engine/PushPresentTiming.h`, `tests/remoteplay/PresentTimingTests.inc`.
- `VEYRA_EXP_PRESENT_TEARING=1`: where explicit VSync applies, present with sync interval 0 +
  `DXGI_PRESENT_ALLOW_TEARING` and no display-queue wait, only with VRR level 4, tearing support, no frame
  generation, no XeSS provider and no output cap other than Follow display. Logs `[slipstream-present]
  experiment=tearing active=<0|1> reason=<off|not-vsync|provider|frame-generation|output-cap|no-tearing-support|
  vrr-level-below-4|active>` (level 4 = the driver reports the display in VRR mode; it does not say VRR runs for
  this window: BENCH1 had level 4 with `active=false`, so the experiment does switch on in that windowed set-up).
  Risk: tearing can only appear when the window reaches independent flip; a composed window does not tear (DWM
  composes it at its own vblank). With independent flip and VRR not running for the window, the picture can tear.
  Torn presents are not dated (`scanoutKnown=0`): use PresentMon.
- `VEYRA_EXP_PRESENT_VRR_REQUEST=1`: re-reads the driver's VRR state 3 s into each new chain (`[display-vrr]`
  while presenting; without it explicit VSync never re-reads it) and logs once per process a read-only inspection
  of the program's NVIDIA G-SYNC profile (`[slipstream-present] vrrProfile ... configured= repairWouldChange=`).
  Never writes a driver profile.
- `VEYRA_EXP_PRESENT_LATE`: not implemented; the Linux model shows just-in-time presentation cannot lower the
  latency of a 60 fps stream on a 120-180 Hz panel (section 4 of the document).
- These three `VEYRA_EXP_PRESENT_*` switches are not `VEYRA_EXP_RP_*` names, so the Slipstream settings file
  cannot set them: the kit sets them in the process environment.
## Frame delivery: statistics and catch-up (`VEYRA_EXP_RP_STATS=1`, `VEYRA_EXP_RP_DELIVERY`, branch `exp/s4-stutter`, 2026-11-20)

Background, code reading and the model behind this: `docs/FRAME_DELIVERY.md`.

- `VEYRA_EXP_RP_STATS=1`: every 5 s while frames are taken, one line
  `[slipstream-delivery] arrived= shown= replaced= gapP50= gapP95= gapMax= clumps= longGaps= shownGapP95= shownGapMax= policy=`
  (the contract keys), then `caughtUp= decodedGapP95= decodedClumps= pickupDelayP50= pickupDelayP95= pickupDelayMax= windowS= ringOverflows=`.
  Times in ms. `gap*` are takion arrival gaps of the frames that reached the mailbox (frames dropped before
  decode are not in it), `clumps` gaps under 4 ms, `longGaps` over 25 ms; `decoded*` the same for the
  decoder's output; `shown*` the engine's pickups (not scanout); `pickupDelay*` decoded -> taken by the engine.
  `replaced` = frames dropped unseen in the mailbox (same events as `mailboxOverwritten`).
- `VEYRA_EXP_RP_DELIVERY=catchup`: when two decoded frames wait at pickup, the older one is handed out first and
  the newer one at the next pickup, only if frame generation is off, one refresh is at most 0.9 x the bound
  below (delta2 review: so 143.86 Hz and up for a 60 fps stream, 100 Hz and up for 30 fps; off at 120 Hz with
  60 fps, where 0.2 ms of pickup jitter tripped the guard, and at 60 Hz with 30 fps; table in
  docs/FRAME_DELIVERY.md section 3; the refresh is the nominal one, also with variable refresh, and an unknown
  refresh means off), the older frame is younger than one source interval,
  the previous pickup was not a catch-up, and no recent catch-up cost more than
  **min(2 refreshes, 0.5 x source interval) x 1.02** (catch-up pickup -> next pickup; 8.5 ms for a 60 fps
  stream at 144-240 Hz, 17.0 / 14.2 / 11.3 / 8.5 ms for 30 fps at 120 / 144 / 180 / 240 Hz). One trip
  switches it off for 5 s; further trips within 80 s of the previous hold's end double the hold up to 80 s.
  Otherwise newest wins, as today. At most two frames wait.
  Cost, from the code: the held-back newest frame is late by about one refresh (8.3 ms at 120 Hz, 5.6 ms at
  180 Hz); one catch-up above the bound can happen before the guard reacts (it measures after the fact), at
  most once per hold; the frame after the held one can wait up to about one more refresh.
  `VEYRA_EXP_RP_DELIVERY=newest`: the same two-slot code with today's rule (an A/B of the path itself).
  Any other value is ignored and logged once (`[remoteplay-timing] unknown VEYRA_EXP_RP_DELIVERY value
  ignored`), whether or not `VEYRA_EXP_RP_STATS` is set.
- Both unset: the original one-slot mailbox and code path, unchanged in behaviour. What still runs (code
  reading): one branch on a constant bool per publish and per read, two relaxed atomic stores per engine loop
  (`deliveryContext`, `EngineController.cpp` near :1484), and `slots_.clear()` / `catchUpGuard_.reset()` on
  empty objects at reconnect and close; no lock, nothing measurable. Either set: one `[remoteplay-timing]` line at session start names the policy.
- **`VEYRA_EXP_RP_STATS=1` alone runs the two-slot code path with the newest-wins rule**, not the stable
  build's literal one-slot code. Equivalent by reading (same lock, sequence counter, `skipped_` count, flag
  merge, `SetEvent`; a Linux test compares the hand-off with a copy of the one-slot code), but STATS arms
  measure that path; only an arm with both switches unset runs the stable code.
- Not in the model: the PS5 path paces to a smoothed source-PTS anchor even with frame generation off
  (`livePacesSourcePts`, `EngineController.cpp` near :2884); catch-up holds one more decoded surface.
- Model robustness (independent reviewer, MODEL): other Wi-Fi fits matching the same three field numbers gave
  22 %, 27 % and 31 % fewer replaced frames; P50 unchanged; latency P99 +0.2 to +0.7 ms, newest-information
  P99 +0.3 to +1.5 ms.
- Status: implemented; portable logic tested on Linux (plain, ThreadSanitizer); the Windows wiring
  (`RemotePlaySessionSource::publishDelivery` / `readDelivery`, one line in `EngineController.cpp`
  `applyPresentation`) is blind code, unverified on Windows and on a PS5. The model
  (`tools/remoteplay_latency_bench/delivery_sim.cpp`, results JSON beside it) expects catch-up to remove about a
  quarter of the replaced frames on Wi-Fi at 144-240 Hz in the model, nothing on a wired LAN, nothing at 60 Hz
  and, since the delta2 fix, nothing at 120 Hz (off there).
## Resume after a GPU device loss (`VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1`, experimental, branch `exp/s4-devloss`)

Implements design (b) of "Recovery without restart" above. Off unless the variable is exactly `1`
(in `slipstream.ini` it is an on/off switch: `on`, `true`, `yes` and `1` are written as `1`; `0`, `off`,
`false`, `no` leave it unset); with it unset nothing below runs (the session never keeps its description,
the teardown block is skipped, the interface's `deviceLossResumePending` is never set).
Unverified on Windows and on a PS5: only the portable policy ran (Linux, `tests/remoteplay/DeviceLossResumeTests.inc`).

The design's citations, checked against exp/rc4 (line numbers before this change): the live-presentation
failure at `:1722-1734` (`:1731` is the bounded wait for the removal; the status line and `break` follow),
`:929` (backend recovery gives up when `GetDeviceRemovedReason` fails), `:1619` (XeSS presentation recovery,
called only from the two presentation-failure sites, which then end the run), the teardown close at
`:3391-3392`, `openRemotePlay`'s post at `:204-208`, the move into the session at `:676`: all as described.
The test hook is `VEYRA_TEST_GPU_FAILURE=<frames>:<removeAfterMs>` (not `_FRAMES`): after that many loop
frames it fails live presentation once and removes the process's own D3D12 device `removeAfterMs` later.

What happens (blind Windows code: `EngineController::run` teardown, `RemotePlaySessionSource::keepForResume`
/ `takeResume`; policy `include/veyra/engine/PushDeviceLossResume.h`):
1. Teardown, before `remote->close()`: with the switch, the engine arms the session (`keepForResume`). When
   the owner loop then ends because close() stopped it and it had not failed, it keeps its connect
   description (the lost `decodeDevice` released first) instead of destroying it.
2. Before the GPU session is shut down: was this run's device removed (`checkDeviceAlive`)? This covers
   every site above, because any of them ends the run with the device removed.
3. After it: the decision (`decidePushDeviceLossResume`): never when the app is closing, another source was
   opened or queued (also PS5 controller-only), the user pressed Stop, the device was not lost, the engine
   threw, the session ended on its own (StreamRecovery gave up, refused, threw), its owner thread (and
   StreamRecovery with it) still runs, or there is no description. Then the budget: at most 3 in any 10
   minutes (sliding), waiting 1 s, 3 s, 10 s; then up to 10 s more until a new device can be made
   (`D3D12CreateDevice` check on the default adapter, every 250 ms). The wait is on the engine's own
   condition variable: stop(), a new open and the destructor notify it and end it at once. The transport
   reads Opening during the wait so that the PS5 pad poll keeps running.
4. Then, under the same lock as the last check, openRemotePlay's snapshot reset and post (same thread, same
   queued task, current settings, a new session id). The new run makes a new device, decoder and session;
   the console sees a normal reconnect.

Log lines (`[remoteplay-recovery]`), only after a device loss of a PS5 session:
`device-loss resume attempt=<n> waiting delayMs=<ms>, then for a usable device ...`,
`device-loss resume attempt=<n> started: same console and settings, new device and session`,
`device-loss not resumed reason=<word>` (`user-stop`, `other-source`, `app-exit`, `engine-error`,
`session-ended`, `recovery-running`, `no-description`, `budget`, `device-unavailable`).

Credentials: the description moves session → `takeResume()` (once; a second call is empty) → a
`shared_ptr` in the posted task → the new session, never copied or logged. Not resumed: the engine's
`kept` / `request` is destroyed at the end of the teardown block; never taken (switch on, no device loss):
destroyed with the session object at the end of `run()`; stop() before the posted task starts: destroyed
with the task (`pending_={}`; no log line for this microsecond window); app exit during the wait: the wait
ends (`app-exit`) and it is destroyed. Every path ends in `~PairingCredentials`, which wipes the keys.

The interface (review fix): at the loss, before the run publishes its failure, the engine sets
`PlayerSnapshot::deviceLossResumePending` when the switch is on, the run is a PS5 session and the budget has
room (`pushResumePendingAtLoss`; nothing is spent until the resume is posted). While it is set the bridge
neither shows the failure notice nor leaves the playing page (`pushLiveFailureShownNow` in
`QmlPlayerBridge.cpp`); the status bar still reads as failed during the wait. The resume's snapshot reset
clears it together with the failure, so nothing is shown; on every path that does not resume (another
condition, `budget`, `device-unavailable`, Stop, a new open, app exit) teardown clears it and the notice and
the change to the start page come as before. Tested only as a portable life-cycle test
(`device_loss_resume_pending_flag_life_cycle`). "The picture back without a click" therefore means: the
playing page stays, and the new session plays on it once the console accepts it; whether fullscreen and the
presentation mode survive the device loss is not known (the window is reused; unverified on Windows).

Known limits (code reading; all of it unverified on Windows and on a PS5):
- A device loss that surfaces as an engine exception (`run()` catches it, `failed=true`) never resumes
  (`engine-error`), and the failure is shown as before; only the live-presentation failure path sets the
  pending flag.
- The device probe is weak: `D3D12CreateDevice` on the default adapter with a null output only asks whether
  a device could be made, while the removed device may still be alive in the process, so
  `device-unavailable` will rarely be logged; a resumed run whose own device creation then fails ends as a
  normal failure (`no-description` is not logged for it).
- The console may still hold the old session and answer "in use" for a while: the resumed session then
  goes through the existing reconnect logic (StreamRecovery, `backoffSeconds`), which waits 10 s, 20 s and
  40 s (at most 4 such refusals per outage, the last also 40 s) between its attempts, so the picture can take
  up to about two minutes to come back, or not come back.
- The resumed run reuses the old window handle (HWND).
- `takeResume` runs after close() joined the session's owner thread, so the `recovery-running` check never
  fires (harmless).

Manual test (Windows, PS5, the kit): set `VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1` in slipstream.ini or the
environment and run `scripts/remoteplay/testkit/run-tests.ps1 -ReliabilityGpuLoss`; the kit sets
`VEYRA_TEST_GPU_FAILURE=900:500`. Do not reconnect by hand. Expected: `[gpu-failure] operation=live-presentation-failed`,
then `[remoteplay-recovery] device-loss resume attempt=1 ...started`, no failure notice, the playing page
kept, and the picture back (frame-rate lines) without a click, unless the console answers "in use" (see the
limits above). The hook fires in every run, so the resumed sessions lose the device again after 900
frames: attempts 2 (3 s) and 3 (10 s), then `device-loss not resumed reason=budget`. Pressing Stop during a
wait must give `reason=user-stop` and no new session. The kit's verdict names "reconnected by the program"
only for `[gpu-reconnect] ... automatic=true` (the capture-card path); for this experiment it should look for
`\] \[remoteplay-recovery\] device-loss resume attempt=\d+ started`.

Review fixes (branch `exp/s4-autofix`; details in `docs/LATENCY_MEASUREMENT.md` 2(f) and 2(g)). These change what
the test kit sees; everything else above stands:
- An invalid plan runs nothing: its bad step `state=failed` (no `start` line), `finished ok=0`, exit only for a
  well-formed `exit`; AUTOCONNECT then logs `step=0 name=connect state=failed reason=invalid-plan`. (The "unknown
  step fails that step and stops the plan" above now happens before any step runs.)
- New reasons: `stream-lost` (session ended or replaced, or 10 s without presenting), `cancelled` (the user
  cancelled the latency test). The plan never continues on a later session.
- No console registered: `finished ok=0 steps=<n> reason=no-console` once at start, nothing else (no exit).
- `slipstream.ini` cannot set `VEYRA_TEST_RP_AUTORUN` / `VEYRA_TEST_RP_AUTOCONNECT`.
- `[slipstream-config]` at most once per 5 s, never for `fgActive` / `srActive` / `window` alone; `refreshHz=-1`
  when unknown. The `plan steps=` line now comes before the `step=0` connect line.
- OPTIONS / D-pad presses on a moving picture are not measurable: `[push-latency] trial N not measurable
  reason=picture-moving`, `[slipstream-press] ... outcome=picture-moving` (every time -1; `outcome=answered`
  is appended to answered presses), `unmeasurable=<n>` after `pcBasis=` in `[slipstream-latency-test]`, and
  `[push-latency] stopped reason=picture-moving trial=<i>/<n> unmeasurable=<k> answered=<a>` when most of at least
  6 presses were. `arrivalToPresentMs` ends at the present-capture record, before the swap-chain queue and
  scan-out.
### Latency governor, sprint 4: review 4 fixes (20 November 2026, branch `exp/s4-control`)

Behind `VEYRA_EXP_RP_GOVERNOR=apply` only; unset behaves exactly as on `exp/rc4`. (Correction, review 5: "the
changed code runs only inside the apply branch" was inaccurate: phase 2 below also changes what
`VEYRA_EXP_RP_GPU_BUDGET` is fed, in observe and apply mode, and reads SmartLink's state with either switch set in
any mode. With both switches unset the watch returns before any of it.) Unverified on Windows; nothing here ran on a GPU, a PC or a PS5.
`VEYRA_EXP_RP_GOVERNOR=apply` is still NOT recommended, in particular not on Wi-Fi, until it has been reviewed
again and tried on hardware.

What changed (`include/veyra/engine/PushGovernorControl.h` `PushGovernorControl::update`, new
`include/veyra/engine/PushGovernorStall.h`, wiring in `src/ui/bridge/PushSlipstream.cpp`):
- Finding 1, stalls the mean jitter cannot see. (a) A peak signal: the inbox's interarrival peak over the last
  256..512 frames (`frameJitterPeakMs`) is turned into "a stall seen this tick" by `PushStallFromPeak` (its rise
  since the last tick); a stall of 8 ms or more makes the network burst for 3 s (`networkBurstPeakMs`,
  `stallLingerMs`). (b) Nothing is stepped for trouble GPU work cannot explain: a decode-queue wait longer than all
  the GPU work per picture in force, missed deadlines with the GPU under 0.6 load, or a latency excess (less the
  jitter) beyond all the work plus the budget; any of them once the GPU is saturated (load 0.95). (c) Credit: a step
  is kept only when the trouble went away by no more than the work it removed allows (all the work before it, unless
  saturated), the work did drop, and its level had no trouble episode (3 s or longer, ending under budget minus
  hysteresis) end on its own within 10 min (`quietMemoryMs`); if the same trouble comes back about as large (80 %)
  at the level a credited step went to within that time, the step is withdrawn (`rollback-trouble-returned`,
  counted as useless). (d) The embedded GPU budget now runs on a ladder whose top is the level in force: its step
  means "sustained trouble here" and recovery and probes no longer wait for its own level (the "never returns"
  case); after a protection step a return needs the budget's healthy reading on every tick of the hold and the
  level above predicted under 0.725 load. (e) Probes are judged only on ticks frame generation can answer for (no
  burst, and over-budget only when the excess is within the GPU work), and also fail on GPU trouble they cause.
  During a burst the budget is still fed GPU time per picture (decode wait, missed deadlines and skipped pairs are
  withheld), so a genuinely overloaded GPU on a stalling link is still answered.
- Finding 2: generation on/off counts as a graph rebuild in `changesTier`, so every rebuild is at least
  `tierIntervalMs` (60 s) after the last. A rollback that rebuilds waits for the spacing instead of dropping its
  verdict. Probes never rebuild (such levels come back after a full healthy hold only).
- Finding 3: after 2 useless steps the governor stands down and puts the user's request back
  (`cause=stand-down`, log `action=restore`), keeping the minimum interval, the per-minute cap, the rebuild spacing,
  and waiting while the GPU is in trouble it explains; then nothing until the next stream.
- Invariants (`pushGovernorInvariantViolation`): rebuilds spaced by `tierIntervalMs`; at most 2 useless verdicts
  per stream, after which the only change allowed is one restore of exactly the request; a stand-down restore only
  after them; a rollback (more work) is refused only during GPU trouble the GPU explains.
- Log: `[slipstream-governor]` lines gain `networkBurst= stallMs= gpuExplains= stoodDown=`; new reasons
  `trouble-not-gpu`, `over-budget-not-gpu`, `rollback-improvement-not-explained`, `rollback-trouble-returned`,
  `rollback-probe-gpu-trouble`, `stood-down-restore-request`, `stood-down-restore-waiting`.
- `PushGpuBudget::lastTrouble()` (new accessor, no decision changed) gives the governor the signal also at the
  budget's floor or in its dwell; the governor's embedded copy runs without the budget's own dwell.

Tests: `tests/remoteplay/GovernorStallTests.inc` (new: the review's stall-only and Wi-Fi traces as permanent 6 h
tests, link stalls beside a slow GPU, decode and latency excesses the GPU cannot explain, credit withdrawal, the
stand-down restore, the new checker rules); `GovernorControlTests.inc` updated: its hand-written ticks now carry a
generation cost per level ((M-1) x (2 + tier) ms; a step's credit needs the work it removed), the decode-protection
tests give the GPU enough load to explain a 25 ms wait, the short-burst probe test makes 3x worse than 2x (bursts
as large at every level are now withdrawn instead), and the per-minute checker test turns rebuild spacing off.

Stated bound for the stall traces (tested, model): mean output at most 0.5 fps under no governor, at most 12
changes per 6 h, at most 1 % of the time reduced, none of the last hour reduced. Result: 0 changes on all of them.

**Model result (Linux simulator), not a measurement.** 24 h, PushFlow 3X Quality requested on a 30 fps source,
`tools/remoteplay_latency_bench/governor_sim.cpp` (`governor_sim_results.json`, `reviewTraces24h`). Before = the
reviewer's table on `exp/rc4` 4d237a97 (their harness); after = this branch.

| Trace | Changes before / after | Rebuilds before / after | Over-budget s gov / none, before | after | Output fps gov / none, after | Time reduced before / after | Longest reduced before / after |
|---|---|---|---|---|---|---|---|
| Pure bursts | 0 / 0 | 0 / 0 | 7100 / 7100 | 7100 / 7100 | 90.0 / 90.0 | 0 % / 0 % | 0 / 0 |
| Bursts + decode stall | 0 / 0 | 0 / 0 | 7100 / 7100 | 7100 / 7100 | 90.0 / 90.0 | 0 % / 0 % | 0 / 0 |
| Bursts + loss | 0 / 0 | 0 / 0 | 7100 / 7100 | 7100 / 7100 | 90.0 / 90.0 | 0 % / 0 % | 0 / 0 |
| Stall-only (finding 1) | not given / 0 | not given / 0 | 7100 / 7100 (30 fps vs 90) | 7100 / 7100 | 90.0 / 90.0 | 99.9 % / 0 % | day / 0 |
| Slow GPU (scale 2.2) | 146 / 2 | 145 / 1 | 369 / 86400 | 8 / 86400 | 30.0 / 90.0 | 100 % / 100 % | day / day |
| GPU load sine, 5 min | 8 / 8 | 2 / 2 | 39786 / 39906 | 39786 / 39906 | 89.6 / 89.7 | 0.2 % / 0.2 % | 152 / 153 |
| Overload then clean (x12) | 48 / 48 | 24 / 24 | 96 / 7272 | 96 / 7272 | 84.8 / 89.3 | 8.6 % / 8.9 % | 618 / 639 |
| User changes mid-reduction | 2 / 2 | 1 / 1 | 28700 / 28992 | 28700 / 28992 | 92.8 / 93.0 | 0.3 % / 0.3 % | 298 / 298 |
| Wi-Fi stalls seen as jitter | 0 / 0 | 0 / 0 | equal | 17269 / 17269 | 90.0 / 90.0 | 0 % / 0 % | 0 / 0 |
| Wi-Fi stalls 20-80 ms, jitter 2.5 ms (finding 1), peak unseen | not given / 0 | 169 / 0 | 17269 / 17269 (off all day) | 17269 / 17269 | 90.0 / 90.0 | 100 % / 0 % | day / 0 |
| same, stall seen in the peak (new) | - / 0 | - / 0 | - | 17269 / 17269 | 90.0 / 90.0 | - / 0 % | - / 0 |
| same + slow GPU 2.2 (new) | - / 2 | - / 1 | - | 17275 / 86400 | 30.0 / 90.0 | - / 100 % | - / day |

Slow GPU: the model's latency steps only lower the multiplier, so it ends at generation off (30 fps output) for
lower latency: latency first, displayed frames last, as the rules order them; at most 1 rebuild per 60 s in every
trace. The 300 s scenarios and 400 random traces (`governor_sim`) keep every invariant.

Mutations (each applied to a scratch copy of `PushGovernorControl.h`, run, reverted; Linux):

| Mutation | Unit tests (failing) | Simulator checker |
|---|---|---|
| A: tier/rebuild spacing removed (`tierBlocked` false) | 5 (incl. 2 new) | caught: "graph rebuilds closer than the tier interval" (everything-at-once, Wi-Fi + slow GPU, 80 of 200 random traces) |
| C: stand-down count 1000 | 7 (incl. 3 new) | caught: "a change after standing down other than restoring the request" (sine: 440 changes / 144 rebuilds; 1 random trace) |
| E: request restore removed (`pendingRestore` nullopt) | 3 | caught: "above the user's request" (2 random traces with new streams) |
| F: latency attribution uses budget minus hysteresis | 4 | caught: "a latency step down the network's jitter explains" / "a step down without a measured reason" (32 random traces) |

#### Phase 2: the link's state from SmartLink (same date and branch)

With `VEYRA_EXP_RP_GOVERNOR` or `VEYRA_EXP_RP_GPU_BUDGET` set, `pushSlipstreamWatch` reads the running PS5 stream's
SmartLink decision (`remoteplay::smartlink::latestShared()`, `include/veyra/remoteplay/SmartLinkShared.h` and
`SmartLinkTypes.h`, SmartLink's own headers; correction, review 5: an earlier wording said "copied unchanged", which
was inaccurate) and passes three plain values into the governor's
sample (`linkKnown`, `linkLimited`, `linkHeadroom`; the portable governor headers do not include SmartLink). Any
SmartLink mode is enough (observe changes no network setting). Not fresh (2 s), nothing published, or SmartLink
off: unknown, and the behaviour is exactly the phase 1 behaviour above (tested: identical decisions tick by tick,
and the simulator's JSON is byte-identical).
- `networkLimited`: the governor and the GPU budget (both its apply mode and the governor's embedded copy) make no
  reduction for stutter the link explains: they are fed GPU time per picture only (`pushGpuOnlySample`: decode-queue
  waits, missed deadlines and skipped pairs withheld); a decode wait over the budget's line is logged
  `reason=link-limited-no-reduction`. GPU load and generated-frame cost still act on their own, and a latency excess
  is still stepped only when it is within the GPU work in force (phase 1), so link stalls on a genuinely
  overloaded GPU still reduce (test `governor_link_limited_still_answers_an_overloaded_gpu`, Wi-Fi stalls + slow
  GPU 2 h model: 2 changes, over-budget seconds 1451 against 7200 without). A return needs the GPU side healthy only
  (load at most 0.60, no GPU trouble) while the link is limited.
- `linkHeadroom` with the GPU healthy (budget healthy, load at most 0.60) on every tick: a return may come after
  half a base hold (10 s, at least the minimum interval) instead of the full one; the checker allows the shorter hold
  only when the headroom was given on every tick of it.
- Log: `[slipstream-governor] ... link=unknown|limited|headroom|clear` (with "no reduction for what the link
  explains" or "shorter return hold"), `[slipstream-gpu-budget] ... link=`.
- Freshness compares the publisher's `updatedMs` with `steady_clock` milliseconds (`steady100ns() / 10000`): the
  publisher must use the same clock, else every decision reads as stale (= phase 1 behaviour, the safe side).
Tests: `tests/remoteplay/GovernorLinkTests.inc` (both directions, unknown = phase 1, 80 randomized traces with
link states). Model results only; unverified on Windows.

#### Review 5 fixes (20 November 2026, branch `exp/s4-control2`)

Behind `VEYRA_EXP_RP_GOVERNOR=apply` only. Active mode is **still NOT accepted**: it goes to a sixth independent
review and stays "not accepted" in every document until that passes. Observe mode (`=1`) and the unset path are
unchanged: `src/ui/bridge/PushSlipstream.cpp` was not touched in this round (`git diff exp/rc4 --
src/ui/bridge/PushSlipstream.cpp` is empty), so `pushSlipstreamWatch` still returns at `governorMode == Off &&
budgetMode == Off` before reading SmartLink or feeding anything; every change is inside `PushGovernorControl`, which
only the apply branch constructs. Unverified on Windows; nothing here ran on a GPU, a PC or a PS5.

Design decision (stated, not changed): with either switch set, in any mode, the watch reads SmartLink's link state
as information. It only withholds reductions (`networkLimited`: the governor and the GPU budget are fed GPU time per
picture only) and, in apply mode, lets a return toward the user's request come after half a hold when SmartLink
reports headroom with the GPU healthy. It never asks for a reduction and never changes a network setting.

What changed (`include/veyra/engine/PushGovernorControl.h`, `PushGovernorControl::update`):
- Blocker 1 (a GPU overload hidden on a stalling link): every step, not only probes, is judged on the ticks frame
  generation can answer for (no burst; for latency, an excess within the GPU work); GPU-load steps on every tick.
  The quiet-baseline rule does not refuse credit when the GPU load before the step was at or above the budget's
  `loadHigh` (0.85). This is the reviewer's scratch fix, ported unchanged in effect.
- Blocker 2 (generation turned back on into over-budget latency, in a loop): a return that goes over the budget
  within `returnFailWindowMs` (40 s) fails at that moment, not at the later step down; the excess the level showed is
  remembered for `returnMemoryMs` (3 h, its weight falling linearly; forgotten when the user changes a setting or a
  stream starts) and blocks the same return, probes included, until the weighted excess fits under the return line;
  a latency step down may pass the 60 s rebuild spacing (latency beats the rebuild count; the checker allows it and
  still counts it as a rebuild).
- Credit withdrawal made reachable: it runs before the step-down section, whatever explains the trouble at the lower
  level (unless the GPU explains it there: then more work would add to it), needs the same trouble over the budget on
  every tick for `sustainMs` (latency) or the budget's sustained trouble of the same kind (decode, presentation), at
  80 % of its size, within `creditMemoryMs` (30 min; was 10 min).
- Tick field `credited` (the tick that credited a step), for the checker.

Simulator (`include/veyra/engine/PushGovernorSim.h`): `pushRunReview5Trace` (14 traces, list in the header), a world
may answer to the governor's last change (trace c), a latency-only delay (`latencyStallMs`). New checker rules,
computed from the trace: no decode, presentation or latency step down within `stallLingerMs` of a rise of the
reported interarrival peak; no credit for a step from a level whose over-budget episode ended on its own within
`quietMemoryMs` with the GPU under `loadHigh`; not an hour or more below the request with no latency gain over it
(0.5 ms); a new stream starts at the previous request.

Tests (`tests/remoteplay/GovernorStallTests.inc`, 24 h model traces): `governor_review5_gpu_overload_on_a_stalling_link_is_answered`
(bound: over-budget seconds at most the stalls alone + 60 s/day), `governor_review5_a_failed_return_is_not_repeated`
(at most 30 changes/day, over-budget seconds within 60 s of generation off all day, at most one return an hour),
`governor_review5_credit_is_withdrawn_when_the_trouble_returns_below`, `governor_review5_stalls_episodes_and_new_streams`,
`governor_review5_checker_catches_the_new_rules`.

**Model result (Linux simulator), not a measurement.** 20 November 2026, 24 h, PushFlow 3X Quality on a 30 fps
source. Review 4's traces (`reviewTraces24h`) are unchanged by this round: same numbers as the table above (pure
bursts, bursts + decode stall, bursts + loss, stall-only 0 changes 7100 / 7100; slow GPU 2.2: 2 changes, 8 / 86400;
GPU sine 8 changes, 39786 / 39906; overload then clean 48 changes, 96 / 7272; user changes 2, 28700 / 28992; the
three Wi-Fi stall traces 0 changes, 17269 / 17269; Wi-Fi + slow GPU 2 changes, 17275 / 86400). Of the 300 s
scenarios only `everything-at-once` changed: 4 changes (was 3), over-budget seconds 96 (was 135), output 83.4 fps
(was 72.1), but mean latency 22.19 ms (was 21.54) and P95 48.35 ms (was 43.71): the latency step came earlier
(passing the rebuild spacing) and a credit was withdrawn. 400 random traces: 0 violations.

Review 5's traces in this tool (`review5Traces24h`); "before" = the reviewer's figures for the review 5 code
(24efc0be, their harness `review-gov5/h.cpp`, own stall seed), "old" = review 4's code in the same harness:

| Trace | Changes (old / before / now) | Over-budget s, gov (old / before / now) / none | Time reduced now | Output fps gov / none, now |
|---|---|---|---|---|
| (a) stalls seen + GPU 2.0 half the time | 118 / 6 / 2 | 17256 / 51741 / 17277 / 51832 | 97.9 % | 31.3 / 90.0 |
| (a) stalls seen + GPU 2.2 half | 118 / 6 / 2 | 17256 / 51741 / 17276 / 51832 | 97.9 % | 31.3 / 90.0 |
| (a) stalls seen + GPU 2.6 half | 118 / 4 / 2 | 17256 / 17314 / 17276 / 51945 | 97.9 % | 31.3 / 85.7 |
| (a) stalls unseen + GPU 2.0 / 2.2 / 2.6 half | 118 / 6, 2, 4 / 2 | 17256 / 51740, 17262, 51944 / 17277, 17276, 17276 / 51832, 51832, 51945 | 97.9 % | 31.3 / 90.0, 90.0, 85.7 |
| Steady GPU 2.0 (blocker 2) | 520 / 754 / 22 | 29584 / 22568 / 48 / 86400 (generation off all day: 0) | 100 % | 30.0 / 90.0 |
| Steady GPU 2.0 + new stream every 6 h | - / - / 32 | - / - / 80 / 86400 | 99.99 % | 30.0 / 90.0 |
| (c) stall stops at a step, back after 9 min | 279 / 3 / 6 | 281 / 85242 / 83625 / 83625 | 1.35 % (was 99.4 %) | 89.6 / 90.0 |
| (c) back after 10 min | 253 / 3 / 6 | 254 / 85122 / 83325 / 83325 | 1.49 % (was 99.3 %) | 89.6 / 90.0 |
| (c) back after 11 min | 233 / 3 / 6 | 234 / 85002 / 83025 / 83025 | 1.63 % (was 99.2 %) | 89.5 / 90.0 |
| Stall runs short enough for the GPU work to explain (mutation I) | - / - / 0 | - / - / 2880 / 2880 | 0 % | 90.0 / 90.0 |
| Overload x12 + new stream inside each (mutation E) | - / - / 72 | - / - / 192 / 7272 | 8.9 % | 84.8 / 89.3 |
| Latency episodes ending on their own (mutation H) | - / - / 4 | - / - / 1264 / 1264 | 0.01 % | 90.0 / 90.0 |

In the reviewer's own harness on this branch: (a) 17262-17264 over-budget s at every scale, peak seen or not (as
their scratch fix); steady GPU 2.0: 22 changes, 48 s; (c) 1.3 / 1.5 / 1.6 % reduced; every other row unchanged
except "d noisy peak + GPU 2.0" (12 -> 10 changes, 313 -> 33 s) and the e rows that share the blocker 2 world.
Notes: in (c) the world stops the stall whenever any level changes, so gov and none count the same over-budget
seconds and review 4's code "won" (281 s) only by changing a level 279 times a day; the figure that compares is the
time reduced. In (a) the governor ends at generation off for the whole day (31 fps shown instead of 90 in the GPU's
healthy halves): latency first, displayed frames last; it never returns because the stalls keep resetting the
healthy hold and a return to generation is never probed. Steady GPU 2.0: one failed return roughly every 2.6 h
(the remembered excess fades over 3 h), each over the budget for about 4 s.

Mutations (scratch copies outside the worktree, discarded; Linux):

| Mutation | Unit tests failing | Simulator checker |
|---|---|---|
| E: request restore removed | 6 (incl. `governor_review5_a_failed_return_is_not_repeated`, `governor_review5_stalls_episodes_and_new_streams`, 2 randomized-trace tests) | caught in 2 deterministic traces (steady GPU 2.0 + new streams; overload x12 + new streams): "a new stream started without the request put back" |
| G: credit withdrawal removed | 2 (`governor_stall_trouble_that_returns_at_the_lower_level_withdraws_the_credit`, `governor_review5_credit_is_withdrawn_when_the_trouble_returns_below`) | caught in the three (c) traces: "held below the request for an hour with no latency gain" (99.2-99.4 % reduced) |
| H: quiet-baseline rule removed | 1 (`governor_review5_stalls_episodes_and_new_streams`) | caught: "a step from a level whose trouble ended on its own was credited" (latency episodes trace: 316 changes) |
| I: stall detector never fires | 2 (`governor_stall_peak_rise_is_a_stall_seen_now`, `governor_review5_stalls_episodes_and_new_streams`) | caught: "a step down during a link stall" (stall runs trace) |
## SmartLink: adaptive networking (`VEYRA_EXP_RP_SMARTLINK=1|apply`, `VEYRA_EXP_RP_SMARTLINK_MODEL`, sprint 4, 2026-11-20)

Portable core only (branch `exp/s4-smartlink`); the Windows wiring is done by other streams against these headers.
Full description, tables and the trace format: `docs/SMARTLINK.md`.

- Switches: `VEYRA_EXP_RP_SMARTLINK` (or `slipstream.ini` `smartlink=`): unset/empty/`0`/`off` = off, `apply` = apply,
  anything else = observe (compute and log `[slipstream-smartlink]` at most every 5 s, change nothing).
  `VEYRA_EXP_RP_SMARTLINK_MODEL=learned` selects the learned comparison model; anything else = statistics (default).
- Outputs (`Engine::update(Sample) -> Decision`, `include/veyra/remoteplay/SmartLinkController.h`): reorder wait
  2/4/8/16 ms or 0, audio reserve target (at most 50 ms) or 0, `networkLimited`, `linkHeadroom`, `suggestedStartKbps`
  for the next session, reason bits, `advice` (one id, at most one change a minute); profile Wired / Strong Wi-Fi /
  Unstable Wi-Fi. On a clean link every output is 0 / false. **In apply mode 0 means RESTORE the configured reorder
  wait / audio target** (it follows a raised value), and the engine is `reset()` at every stream start. Advice never
  changes a system setting. Shared-state times (`publishShared` / `isFresh`) are `std::chrono::steady_clock`
  milliseconds in every publisher and reader.
- Evidence: Linux simulator only (`tools/smartlink_sim`, model results, not measurements): wired-clean 0 ms added and
  0 changes; trimming the library's 16 ms wait on lossy links removes most loss-induced hitches (the 2 ms LAN profile
  does the same); the audio reserve cuts audio gaps on Wi-Fi-drought scenarios at 5-7 ms more reserve while the
  trouble lasts; under congestion it is WORSE (2.5 against 1.25 audio gaps a minute for the fast audio rule alone);
  with frequent reordering it leaves the wait as configured (it was worse than the existing switches there). The
  simulator applies decisions 250 ms late, as the library does. The learned model shows no measurable benefit over
  the statistics on unseen scenario families. Full list: `docs/SMARTLINK.md`, "Where SmartLink is worse in the model".
- Unverified on Windows and on a PS5. What a hardware run must show: `docs/SMARTLINK.md`, "What only a hardware run can show".
## Audio, round 3: pop counters, smooth edges, adaptive playout (branch `exp/s4-audio`, 2026-11-20)

Why: the first real session (Wi-Fi) had audio pops; no audio line was logged. Analysis, model table and the
listening checklist: `docs/AUDIO.md`, "Round 3". All three are off by default; unverified on Windows.

- `VEYRA_EXP_RP_STATS=1` (shared with the other `[slipstream-*]` lines): the `[remoteplay-audio]` line every
  10 s even with every audio experiment off, with today's keys plus `pops=` (underrun starts + hard resets +
  cuts without a fade, since the stream started), `concealed=` (Opus frames Chiaki concealed, since the stream
  started), and over the last 10 s `underrunStarts= hardResets= tails= queueP50= queueP95= queueMin=` (session
  queue + endpoint after each pump, ms), `gapP95= gapMax=` (time between PCM pushes, 2 ms feeder poll),
  `paddingMin=` (endpoint padding before a write, ms; 0 = it ran dry), `smooth= adaptive= playoutTargetMs=`.
  Behaviour unchanged. `concealed` is counted always (a forwarding audio sink in `ChiakiBackend.cpp`, one atomic
  increment per concealed frame); it is only printed in this line.
- `VEYRA_EXP_RP_AUDIO_SMOOTH=1`: when the endpoint will run dry before the next pump (padding after the write
  below one engine period), `AudioRenderer::pumpOnce` appends a 4 ms continuation (mirror of the last written
  frames under a raised-cosine fade-out; not source PCM, left out of the media timeline like a gap) and arms
  the 5 ms fade-in for the next real data. Steady state is untouched (no added latency); after a certain gap
  the next real data can start up to 4 ms later. Independent of FAST_RECOVER and LOW_BUFFER.
- `VEYRA_EXP_RP_AUDIO_ADAPTIVE=1` (auto sync mode only): the playout reserve (prefill after a re-anchor, and
  the same extra added to the schedule) grows after an underrun to the largest recent arrival gap + 5 ms, by
  at least 10 ms, at most 80 ms; after 10 s without an underrun it shrinks at 2 ms/s towards today's reserve
  (or recent gaps + 5 ms). The rate correction keeps today's reserve. Never above today's value without an
  underrun, so a clean link keeps today's delay; on Wi-Fi it adds delay (model: +13 to +16 ms median).

## Audio continuity, SmartLink phase 2 (branch `exp/s4-audio`, 2026-11-20)

Details, model tables and the reasons: `docs/AUDIO.md`, "Round 4". Off by default; unverified on Windows; all
results are code reading or the Linux model.

- `VEYRA_EXP_RP_SMARTLINK` (owned by stream "core"; observe or apply): the audio sink reports
  `publishAudioFeed(queued + endpoint ms, underruns)` every 250 ms. In apply mode only, while the shared
  decision is fresh (2 s) and `audioTargetMs > 0`, the playout reserve's target follows it: the schedule moves
  at once, the queue itself grows by at most about 5 ms per second through the rate trim; released at 2 ms/s,
  at most 80 ms, never below what the sink keeps itself (auto sync mode only). Stale, observe or 0 = the
  behaviour without SmartLink. The audio side reads the switch as SmartLink does (`parseMode(...) != Off`):
  `VEYRA_EXP_RP_SMARTLINK=off` is off for audio too (review audio2 item 2).
- Review audio2 item 1 (fixed, model): a raise of the reserve no longer triggers the "error above 60 ms"
  fade-and-restart by itself (`ReserveChangeAllowance`): asks of 40/60/70/80 ms with no audio trouble for
  1 simulated hour give 0 resets on the default rule and with FAST_RECOVER (before: 4 and 1 at 80 ms); 12 h
  of Wi-Fi bursts on the default rule: 11 resets alone, 2 with an ask of 80 (before: 16). With the reserve
  unchanged (switches unset) the test is exactly the original one. Details: `docs/AUDIO.md`.
- Drift on the default path (existing upstream behaviour, model): audio delay builds up over hours, 1.5 s
  after about 4 h at +100 ppm, about 360 ms after 2 h at +50 ppm; real clock difference unmeasured. Test
  `VEYRA_EXP_RP_AUDIO_FAST_RECOVER` only together with SMOOTH and ADAPTIVE (alone: 2.6x the clicks on Wi-Fi
  in the model); a 30-minute run gives the first real `driftPpm=`.
- `VEYRA_EXP_RP_AUDIO_ADAPTIVE` now also raises the reserve before any underrun from a forgetting 0.995
  quantile of relative arrival delay + 5 ms (about 20 s memory). On a clean link it stays at today's value
  (model: identical rows).
- No `VEYRA_EXP_RP_AUDIO_CONCEAL`: Opus loss concealment and the packets' redundant frame copies are already
  used by the pinned library. No `VEYRA_EXP_RP_AUDIO_DRIFT`: the inaudible rate trim already exists in
  `VEYRA_EXP_RP_AUDIO_FAST_RECOVER`, which also removes the stall lateness.
- `[remoteplay-audio]` with STATS adds `playoutSource= smartlinkAskMs= aboveTargetPct= trimPpm=`.
- Kit "audio continuity" arm: `VEYRA_EXP_RP_STATS=1 VEYRA_EXP_RP_AUDIO_FAST_RECOVER=1 VEYRA_EXP_RP_AUDIO_SMOOTH=1
  VEYRA_EXP_RP_AUDIO_ADAPTIVE=1` (+ `VEYRA_EXP_RP_SMARTLINK=apply` for the SmartLink arm).
- Review fixes (2026-11-20): `VEYRA_EXP_RP_AUDIO_SMOOTH` no longer starts a tail when the endpoint is already
  empty (a late wake: that tail would itself click); the tail is 4 ms at the endpoint's rate.
  `VEYRA_EXP_RP_AUDIO_ADAPTIVE` ignores gaps above 80 ms and grows only on an underrun explained by an arrival
  gap within 1 s. The `[remoteplay-audio]` line gains `clicks=` (gaps that began on a real sample with no fade;
  `pops=` keeps the contract's meaning) and is formatted outside the session mutex. Kit fields for the A against
  A+SMOOTH comparison: `clicks=`, `tails=`, `underrunStarts=`, `pops=`.
## Network on Wi-Fi, sprint 4 (branch `exp/s4-net`, 20 Nov 2026)

**Linux checks and Linux models only.** The C change was compiled by gcc in a virtual-time harness and parsed by
the mingw cross-compiler, not built by MSVC; nothing here ran on Windows, a PS5 or the owner's Wi-Fi. No latency
figure below is a measurement. The owner's PC is on Wi-Fi until cables arrive (BENCH1), so this section is about
what the switches do there.

### 1. Per-frame late-packet evidence (chiaki patch 0004, counters only)

The gap described under "Widening only on per-frame evidence" is closed: patch 0004 now counts
`CHIAKI_VEYRA_STAT_LATE_FRAMES_FAILED`, read into `TransportCounters::lateFramesFailed` and passed by
`reorderEvidence()` (`ReorderWindow.h`). A frame counts once when (a) FEC could not complete it, (b) one of its
own packets that a reorder timeout had given up on arrived afterwards, and (c) the library saw, released in order
or late, at least as many of its packets as it has source units (a longer wait would have completed it).

How: a ring of the last 16 video frames by frame index inside `ChiakiTakion` (`veyra_frames`), written and read
only on the Takion thread: released packets (`takion_av_queue_flush_with_timeout`), late packets
(`veyra_takion_reorder_late_arrival`, which now gets the parsed packet), and the FEC failure, reported by
`videoreceiver.c` through `chiaki_veyra_takion_video_frame_failed()` (the video receiver runs inside the Takion
thread's AV callback). The counter itself is one more interlocked 64-bit value, like its neighbours; it is added
after `FRAMES_FEC_FAILED` for the same frame, and `readTransportCounters()` now reads every counter that is a part
of another before the whole (late frames before FEC failures, late arrivals before skips, timeouts before gaps), so
a poll between two library updates cannot look like the "inconsistent counters" fault. Nothing sent to the console
changes; with every switch unset the library behaves as before and only counts. A frame older than its ring slot
(more than 16 frames, about 270 ms) is not tracked; a packet the full queue pushed out is not seen by the library,
so (c) can be false where the trace's truth would say "it reached the socket" (the study now uses the library's
rule, `LateFrameEvidenceModel` in `ReorderQueueModel.h`; in `reorder_study_results.json` this moved one value:
`heavy_reordering` adaptive wait changes 5 -> 10, lost frames and delays unchanged; `reorder_sim.cpp` keeps the
trace's truth and its JSON is unchanged).

Check: `reorder_library_check.sh <patched tree> [traces] [study seeds]` builds the library's queue code with a
stand-in video receiver (a frame is complete when its source-unit count of packets is released, failed when a later
frame's packet is released first; the model's rule, simpler than the real frame processor) and compares, line by
line, every released packet and every counter including `fecFailed` and `lateFramesFailed`, with the C++ model:
40 generated traces (now with frames of 6-60 packets and a frame index that wraps) and 12 study traces of 60 s
(the four Wi-Fi conditions below, `heavy_reordering`, `congestion_with_reordering`, 2 seeds): 3.43 million released
packets, 5700 failed frames, 919 of them lost to late packets, all equal. Two mutations of the C rule (`<=` for `<`
in the unit count; the failure not recorded) are caught. Patches 0001-0004 apply in order on a fresh copy of the
pin (`git apply --check`); no new patch file, so `verify-chiaki-stage.py` and the workflow's list are unchanged.

**What `VEYRA_EXP_RP_REORDER_ADAPTIVE=1` does in the app now** (supersedes the paragraph of that name above):
the rule of `ReorderWindow.h`, unchanged, now with real per-frame evidence. With the LAN profile it starts at 2 ms
and widens (up to 16 ms) only after two frames within 60 s were lost to their own late packets, with frames being
lost at 0.1 a second or more; a widening that does not halve the loss rate is rolled back. Without the profile it
starts at 16 ms and narrows one step per 30 s while no frame is lost to a late packet. Linux model:
`study_wifi_retries_widen_the_adaptive_wait_from_library_evidence`.

### 2. Wi-Fi-like links in the study (Linux model, `reorder_study --wifi`)

Invented from the general shape of 802.11 behaviour, not measured on any network
(`ReorderStudy.h` `wifiConditions()`; the old conditions' traces are unchanged). Raw output:
`tools/remoteplay_latency_bench/reorder_study_wifi_results.json` (5 seeds of 120 s, impairment 30-90 s,
35960 frames per cell; the same bytes on a second run).

- `wifi_stall_clumps`: per-packet delay 0.3 ms mean; about every 2 s the path stalls 20-80 ms, then the held
  packets arrive together, in order.
- `wifi_retransmission_reorder`: 1 in 150 packets starts a group (mean 3) that the link retries, 1-4 ms per try,
  another try 30 %, lost after 4; retried packets arrive after their successors. A real 802.11 receiver usually
  restores the order itself (block-ack reordering), turning retries into a stall instead: this is the pessimistic case.
- `wifi_rate_dips`: about every 5 s the link serves 1200 packets/s (the stream needs ~1620) for 0.3-1.5 s, 60 ms buffer.
- `wifi_mixed`: milder versions of all three together.

Columns as in the sprint 3 study table; "lost (early / link)": given up although complete at the socket / lost on
the link; "requests / merged": keyframe requests, one per lost frame, and merged within 500 ms; "drops": decoder
queue drops of the original queue / with `VEYRA_EXP_RP_BACKLOG` (simplified decoder, 3 ms a frame); "back": worst
seed, ms after the impairment until the whole delay is at baseline.

| Condition | Rule | lost (early / link) | requests / merged | added ms P50 / P95 / P99 | drops | back | wait changes |
|---|---|---|---|---|---|---|---|
| `wifi_stall_clumps` | fixed 2, 4, 8, 16 ms and adaptive | 0 | 0 / 0 | 0.00 / 0.00 / 0.00 | 0 / 0 | 5 | 0 |
| `wifi_retransmission_reorder` | fixed 2 ms | 459 (458 / 1) | 459 / 286 | 0.00 / 0.14 / 2.01 | 0 / 0 | 5 |  |
|  | fixed 4 ms | 131 (130 / 1) | 131 / 100 | 0.00 / 0.41 / 3.28 | 0 / 0 | 5 |  |
|  | fixed 8 ms | 22 (21 / 1) | 22 / 22 | 0.00 / 0.46 / 4.34 | 0 / 0 | 5 |  |
|  | fixed 16 ms | 1 (0 / 1) | 1 / 1 | 0.00 / 0.46 / 4.34 | 0 / 0 | 5 |  |
|  | adaptive from 2 ms | 26 (25 / 1) | 26 / 21 | 0.00 / 0.46 / 4.25 | 0 / 0 | 5 | 10 |
| `wifi_rate_dips` | every rule | 2945 (0 / 2945) | 2945 / 129 | 0.00 / 0.00 / 0.00 | 0 / 0 | 5 | 0 |
| `wifi_mixed` | fixed 2 ms | 983 (199 / 784) | 983 / 194 | 0.00 / 0.00 / 2.00 | 0 / 0 | 5 |  |
|  | fixed 4 ms | 844 (60 / 784) | 844 / 100 | 0.00 / 0.00 / 2.63 | 0 / 0 | 5 |  |
|  | fixed 8 ms | 804 (20 / 784) | 804 / 63 | 0.00 / 0.00 / 2.73 | 0 / 0 | 5 |  |
|  | fixed 16 ms | 784 (0 / 784) | 784 / 43 | 0.00 / 0.00 / 2.73 | 0 / 0 | 5 |  |
|  | adaptive from 2 ms | 916 (132 / 784) | 916 / 149 | 0.00 / 0.00 / 2.00 | 0 / 0 | 5 | 22 |

What the model suggests for Wi-Fi, and no more:

- **Stalls of tens of ms are not a reorder-wait problem.** Nothing arrives out of order, so no wait helps or costs;
  the stall's own 20-80 ms reaches the picture whatever the receiver does. The answer there is "do not wait,
  recover fast": the clump must not cost a keyframe. With a 3 ms decoder a clump of up to 5 frames (a stall under
  about 85 ms) stays inside the original queue's four waiting frames; a longer stall drops the queue and asks for a
  keyframe unless `VEYRA_EXP_RP_BACKLOG=1` (see "Stalls" above: 12 frames released at once are decoded through).
- **Where link-layer retries arrive out of order, a longer wait is cheap and saves frames**: 16 ms loses 1 frame
  where 2 ms loses 459, for 2.3 ms more P99 added delay (the retried packets hold only the frames behind them). The
  LAN profile's fixed 2 ms is the model's worst choice on such a link. The adaptive rule from 2 ms loses 26 (it must
  see two losses before it widens, then needs one pay period) and is back at 2 ms about 2 minutes after the link is
  clean.
- **Rate dips are losses no wait can save**: every rule loses the same frames; only fast recovery matters.
- So on Wi-Fi: the LAN profile alone (fixed 2 ms) is not recommended; the library's own 16 ms (baseline) or the
  profile with the adaptive rule are the arms to compare. Whether the owner's link reorders at all (or, as 802.11
  usually does, turns retries into stalls) is exactly what `lateArrivals`, `reordered` and `bottleneck=` in the health
  line will show.

### 3. The expiry race (`VEYRA_EXP_RP_EXPIRY_FIX=1`)

Off unless set. With it, `VideoIngress::expiredLocked` returns false when the consumer's clock value is older than
the head frame's arrival (`QueueLimits::expiryFix`); a frame that really is older than 100 ms is still dropped (by
`tryPop` and by `push`). Nothing else changes: no backlog, no other switch implied. With the backlog switch the same
tolerance was already on; this switch gives it alone, so an A/B run can tell the two apart.

Tests: `expiry_fix_a_frame_that_arrived_after_the_owner_read_its_clock_is_not_expired` (the deterministic
reproduction of `known_defect_...` in `StressTests.inc`: default = one recovery, switch = frame decoded),
`ingress_two_threads_expiry_fix_never_recovers_without_a_reason` (20000 frames, two threads, no sleeps, at most
2 or 4 queued: 0 recoveries with the switch; the default in the same runs recovered 625 and 11 times, a worst case
with no sleeps, not a rate), `expiry_fix_switch_is_its_own_and_off_by_default`.

How often it can fire at 60 FPS (an estimate, not a measurement). The race needs the owner to read its clock
(`RemotePlaySource::read`) while the queue is empty and then to reach the inbox lock only after a frame was stamped
(`ChiakiBackend` video callback) and pushed. The producer copies the payload between stamping and pushing (a few to
tens of microseconds for a 15 Mbit/s frame), while the owner goes from its clock read to the lock in well under a
microsecond, so an undisturbed owner always wins and nothing happens. It fires only when the owner is held between
the two instructions for longer than the producer's copy: preempted, or (rarely) a page fault. Per frame:
P ≈ L x q x T, with L the owner's looks per second at an empty queue, q the share of looks held up and T how long.

| L (looks/s) | q | T | per frame | at 60 FPS |
|---|---|---|---|---|
| 300 | 1e-5 | 1 ms | 3e-6 | once every ~90 min |
| 300 | 1e-4 | 2 ms | 6e-5 | once every ~5 min |
| 300 | 1e-3 | 5 ms | 1.5e-3 | once every ~11 s |

L = 300 assumes the 4 ms controller tick plus audio wake-ups; q and T depend on CPU load and priorities and were not
measured. BENCH1's runs (Wi-Fi, several minutes each) logged one keyframe request per run, at the start, which fits
the first two rows rather than the third. Each occurrence costs a queue drop and a keyframe request. The switch is
the cheap way to remove it; `recoveries=` in the `[remoteplay-lan] attempt=...` line with and without it is the test.

### 4. Route class and Wi-Fi health classes

`[remoteplay-health]` lines end with `routeClass=wired|wifi|unknown` (`routeClass()` / `routeClassName()` in
`ConnectionHealth.h`: wired = the console's address is on the subnet of an Ethernet adapter of this PC, wifi = of a
Wi-Fi adapter, unknown = a host name, a routed address, another adapter type, or nothing assessed yet; virtual
switches report as Ethernet; the console's own link is not visible). The existing `route=` key keeps its values
(`direct-lan`, ...): the kit's and report's parsers read them, so the new class has its own key.

The route is now assessed at every connection attempt (one adapter query, no log line without a switch) and kept in
`RemotePlaySessionSource::connectionHealth().route`, also when `VEYRA_EXP_RP_HEALTH` is off. **Accessor for
`[slipstream-config] route=`:** `remoteplay::routeClassName(session.connectionHealth().route)`, or from the engine
snapshot `remoteplay::routeClassName(status.remoteHealth.route)` (`EngineController` copies `connectionHealth()`
into `remoteHealth`). It says `unknown` until the first attempt has started.

Health classes against the Wi-Fi conditions (`study_wifi_conditions_bottleneck_classes`, thresholds still
provisional): a stall followed by a clump is `jitter`, never `decoder-backlog` (the burst-arrival rule of sprint 3
holds); retries out of order are `reordering`; rate dips `loss`; mixed any of the three; 10 s after the impairment
`none` and stable. With `routeClass=wifi` the same intervals are named `wifi-route`, with the network sign among
`signs=`; clean intervals stay `none`. No threshold was changed.

### 5. Transport switches in the app after this change, and the arms for Wi-Fi

| Switch | What it does now |
|---|---|
| none | upstream behaviour: 16 ms reorder wait, 100 KiB receive buffer, original ingress queue and age rule; the patched library only counts |
| `VEYRA_EXP_RP_HEALTH=1` | observation only: the health line every 5 s (now with `routeClass=`); the library is configured with its own values |
| `VEYRA_EXP_RP_LAN_PROFILE=1` | fixed 2 ms reorder wait and a 1 MiB receive buffer (`REORDER_US`, `RCVBUF` adjust them) |
| `VEYRA_EXP_RP_REORDER_ADAPTIVE=1` | the wait moves among 2-16 ms on per-frame evidence (above); starts at 2 ms with the profile, 16 ms without |
| `VEYRA_EXP_RP_BACKLOG=1` | the ingress queue decodes through bursts of up to 16 frames within 100 ms; includes the expiry tolerance |
| `VEYRA_EXP_RP_EXPIRY_FIX=1` | only the expiry tolerance (section 3) |
| `VEYRA_EXP_RP_IDR_RETRY_MS`, `RECONNECT_PATIENT`, `THREAD_PRIORITY`, `TIMER_1MS` | unchanged by this sprint (sections above) |

Arms for the kit on Wi-Fi (every arm with `VEYRA_EXP_RP_HEALTH=1` so that each has the same counters; one switch
differs between neighbouring arms; label everything "Wi-Fi: diagnostic only"):

1. baseline: `HEALTH=1`
2. LAN profile: `HEALTH=1 LAN_PROFILE=1` (the model expects more lost frames than baseline if the link reorders)
3. adaptive with profile: `HEALTH=1 LAN_PROFILE=1 REORDER_ADAPTIVE=1`
4. expiry fix: `HEALTH=1 EXPIRY_FIX=1`
5. backlog: `HEALTH=1 BACKLOG=1`

What the next run must show, per arm: `fecFailed`, `keyframeRequests`, `lateArrivals` and `bottleneck=`/`signs=`
per health line; `routeClass=wifi`; for arm 3 the `[remoteplay-lan] reorder wait ... -> ...` lines (does it ever
widen; does it come back); for arms 4 and 5 `recoveries=` and `backlogEpisodes=` in `[remoteplay-lan] attempt=...`
against arm 1. If `lateArrivals` stays 0 on the owner's Wi-Fi, the link turns retries into stalls and the reorder
wait does not matter there; then arm 5 (decode through clumps) is the one to watch.

### Corrections after the review of section 2 and 1 (20 Nov, Linux model)

- **The Wi-Fi headline depends on its assumptions.** "16 ms loses 1 frame, 2 ms loses 459" holds only for the
  invented retries of 1-4 ms that arrive after their successors. Sensitivity (same code, conditions appended to
  `wifiConditions()`, 5 seeds of 120 s, `reorder_study_wifi_results.json`; lost frames, P99 added delay ms):

  | Condition | fixed 2 ms | fixed 4 ms | fixed 8 ms | fixed 16 ms | adaptive from 2 ms |
  |---|---|---|---|---|---|
  | `wifi_retransmission_reorder` (1-4 ms per retry, out of order) | 459 / 2.01 | 131 / 3.28 | 22 / 4.34 | 1 / 4.34 | 26 / 4.25 |
  | `wifi_retx_short` (0.3-1.2 ms per retry) | 19 / 0.95 | 5 / 0.96 | 5 / 0.96 | 5 / 0.96 | 15 / 0.96 |
  | `wifi_retx_in_order` (the receiver restores the order, block-ack: the usual 802.11 case) | 5 / 0 | 5 / 0 | 5 / 0 | 5 / 0 | 5 / 0 |
  | `wifi_retx_rare` (groups ten times rarer) | 81 / 0 | 19 / 0 | 5 / 0 | 0 / 0 | 20 / 0 |

  So the advice is: **use the LAN profile's fixed 2 ms on Wi-Fi only if the health line shows `lateArrivals` at 0**;
  with late arrivals above 0 a longer wait (or the adaptive rule / SmartLink apply, which now check that by
  themselves from the per-frame evidence) is the model's answer. Whether the owner's link reorders at all is not known.
- **What the library check compiles.** `reorder_library_check.sh` builds the library's `takion.c` and
  `reorderqueue.c`; the video receiver (`videoreceiver.c`, where FEC failure is detected and
  `chiaki_veyra_takion_video_frame_failed` is called) is replaced by a stand-in with the model's frame rule (a frame
  is complete when its source-unit count of packets is released). The one-line hook in `videoreceiver.c` is checked by
  code reading and the gcc/mingw parse only.
- **A late packet that arrives twice** was counted twice toward its frame (reviewer's case: a frame that could never
  be completed counted as lost to a late packet). Fixed in patch 0004 (ring of the last 32 late packet indexes on the
  Takion thread; `REORDER_LATE_ARRIVALS` still counts every arrival) and in the model; cases `late_duplicate` /
  `late_duplicate_once` in the library check.

## SmartLink network wiring (sprint 4 phase 2, branch `exp/s4-net`)

**Blind Windows code and Linux tests only; nothing ran on Windows, a PS5 or the owner's Wi-Fi.** With every switch
below unset: no SmartLink object is created (the session holds an empty `std::optional`), no thread, no adapter or
WLAN query, no file; the library only counts (its new counters cost a few additions per message on the Takion thread).

### The console's own view: CONNECTIONQUALITY (patch 0004)

`streamconnection.c` now copies the console's CONNECTIONQUALITY fields into counters (`CHIAKI_VEYRA_STAT_CQ_*`: the
newest values as received, a report count, the arrival time on the library's clock, the library's measured rate) right
where chiaki-ng logged them at VERBOSE. `ConsoleQuality.h` turns them into milliseconds and kbit/s; the health line gains
`consoleRttMs= consoleTargetKbps= consoleMeasuredKbps= consoleReports= consoleAgeMs=` (existing `rtt=` is still the
connect-time Senkusha value). What the code does not say, and stays unknown until a real session:

- **Units.** `rtt` is a protobuf double and `target_bitrate` a uint32 with no unit in the source. Patch 0004 keeps
  rtt x 1e6 as an integer (`CQ_RTT_MICRO`), so a sub-millisecond value sent in seconds is not rounded to 0. One
  `ConsoleUnits` per session, shared by the health line and the SmartLink feed, decides the unit only after 3
  reports whose converted value lies within 10x of our reference (the Senkusha round trip measured while
  connecting; the requested rate) and that lead any other unit by 3: `rtt` as seconds or milliseconds, the
  bitrate as bit/s, kbit/s or Mbit/s. Until then, without the reference, and whenever a converted value falls
  outside 0.1-2000 ms (100-1000000 kbit/s), the value is -1 (unknown), never a guess. Logged once per session:
  `[remoteplay-health] consoleUnits rtt=ms|s|undecided rate=... basis=reports:N ...` (numbers and unit words).
  **`consoleRttMs` is unconfirmed until its scale has been compared with the on-screen round trip in a real
  session**: compare it with `rtt=...(at-connect)` and the on-screen value, and `consoleTargetKbps` with the
  requested rate.
- **How often it arrives.** Chosen by the console; nothing in the client sets it. `consoleReports` and `consoleAgeMs`
  in two consecutive health lines give the interval.
- The owner's on-screen "2-79 ms" is not from this app's PS5 path (the PS5 health line had no live round trip; the
  HUD's round-trip row belongs to the Moonlight/Xbox sources).

### Sample windows (`SmartLinkFeed.h`) and what is really filled on Windows

| Sample field | Source | On Windows |
|---|---|---|
| packetsReceived / packetsLost | library congestion accounting (its own 200 ms windows, not aligned) | filled |
| packetsReordered / packetsLate | reorder gaps closed in time / late arrivals | filled |
| packetGapMeanMs | window / packets (a mean, not per packet) | filled |
| packetGapMaxMs | the library keeps no per-packet times | **unknown** |
| framesArrived, framesIncomplete, framesFecRecovered | inbox access units, FEC counters | filled |
| frameGapMaxMs / ExcessMs | decoded frames' arrival stamps, gap still open at the window's end included | filled |
| lateRescueMs | per-frame late evidence + lateness buckets (1-4 ms resolution) | filled |
| reorderWaitMs | the wait the library reports | filled |
| rttMs, consoleTargetKbps | CONNECTIONQUALITY (above) | filled once a message arrived and the unit is decided, else -1 |
| measuredKbps / requestedKbps | inbox bytes / the launch request | filled |
| videoQueueDepth | inbox | filled |
| audioBufferedMs / audioUnderruns | `latestAudioFeed()` | only when the audio stream's sink publishes it |
| wifiRssiDbm, wifiSignalPercent | `wlan_intf_opcode_rssi` on the one connected WLAN interface; percent by Windows' -100..-50 dBm mapping | Wi-Fi route only; unknown with no or several connected WLAN interfaces |
| wifiLinkMbps, wifiBandGhz | need `current_connection` (network name, location consent) or `realtime_connection_quality` (not in every SDK); the channel number alone cannot tell 6 GHz | **unknown** |
| frameLateMeanMs | not computed yet | **unknown** |

### Switches

| Switch | What it does |
|---|---|
| `VEYRA_EXP_RP_SMARTLINK=observe` (or any value but 0/off/apply) | the engine runs every 200 ms on the session owner, publishes its decision (`SmartLinkShared.h`) and writes `[slipstream-smartlink] profile=... mode=observe reasons=...` at most every 5 s; it changes nothing |
| `VEYRA_EXP_RP_SMARTLINK=apply` | as observe, and the decision's reorder wait (2/4/8/16 ms; 0 = the configured wait) goes to the library's live wait. The adaptive reorder controller does not run then (one owner of the wait). An explicit `VEYRA_EXP_RP_REORDER_US` wins: SmartLink never changes it and logs that once. Other decisions (audio target, network-limited, headroom) are read by the other streams from the shared state |
| `slipstream.ini` `smartlink = observe|apply` | the same as the environment variable, value passed through (test `slipstream_ini_smartlink_key_passes_its_value`) |
| `VEYRA_EXP_RP_SMARTLINK_MODEL=learned` | the learned comparison model instead of the statistics (stream "core") |
| `VEYRA_EXP_RP_SMARTLINK_TRACE=1` | `smartlink-trace.csv` in the log folder, one `SmartLinkTrace.h` line per window (numbers only), at most 9000 lines (30 min) per stream. A new file at each stream start (not at a reconnect inside a stream): the previous `smartlink-trace.csv` becomes `smartlink-trace.1.csv`, `.1.csv` becomes `.2.csv` (3 kept). Written through a 64 KB buffer, flushed at the line limit and at stream end; if a rename fails, the earlier traces are kept and this stream records none (one warning line) |
| `VEYRA_EXP_RP_WIFI_STREAMING_MODE=1` | only when the route to the console is Wi-Fi: asks that Wi-Fi adapter for media streaming mode (`WlanSetInterface`, `wlan_intf_opcode_media_streaming_mode`) at stream start; Windows ends it when the handle closes at stream end. Logs `[slipstream-smartlink] wifiStreamingMode=requested|refused|released`, or `wifiStreamingMode=skipped-not-wifi` on a wired or unknown route. Changes adapter behaviour: off by default, never turned on by SmartLink or a profile; the owner decides |
| `VEYRA_EXP_RP_EARLY_LOSS=1` | patch 0004 option: a missing video packet is given up at once when its frame already misses more units than its FEC units (counted from units handed on or arrived late), but never while a packet arrived out of order in the last 300 s (Moonlight's speculative-RFI cooldown). Counters `earlyLossCuts` / `earlyLossWrong` in a `[remoteplay-lan] attempt=...` line |
| `VEYRA_EXP_RP_REORDER_WRAP_FIX=1` | patch 0004 option: the video reorder queue's index sum is truncated to 16 bits (below) |

**Early loss, what the model shows (Linux model, `early_loss_2ms` / `early_loss_16ms` in the Wi-Fi JSON):** no
condition gains. Frames lost and added delay are equal to the plain waits in the stall, rate-dip and mixed conditions
(the gaps there are single packets, or the wait runs out before the next frame has arrived anyway); with retries out
of order it loses 1-2 frames more per 5 seeds (cuts made before the first out-of-order packet starts the cooldown).
What it could shorten, the time until the corrupt-frame report and keyframe request leave, is not measured by this
model (the library reports a failed frame when the next frame's first packet is handed on, which early loss does not
move). Not recommended until a hardware log shows `earlyLossCuts` with `earlyLossWrong` at 0 and faster recovery.

**The index wrap (`VEYRA_EXP_RP_REORDER_WRAP_FIX`).** Confirmed in the pin and unchanged in upstream's newest clone
(`/home/claude/refsrc/streetpea_chiaki-ng` `43728af`, `lib/src/reorderqueue.c`): `seq_num_16_add` returns the sum of
two `uint16_t` promoted to `int` (up to 131070), and the queue compares with `==`, so when the queue ends exactly at
65535 packet 0 is dropped although it arrived in time (case `wrap` in the library check: `dropped=1`; `wrap_fixed`:
`dropped=0`). The fix is one line, the truncation `(ChiakiSeqNum16)(a + b)`, applied by replacing the queue's add
function pointer when the video queue is created (`veyra_seq_num_16_add_wrapped`); the model's `add` does the same
under the option, and all 56 library-check traces (half of the generated ones with the option) agree. It is a
candidate for default after a hardware run; it never turns on by itself.

**slipstream.ini.** Besides `smartlink = observe|apply`, `slipstream.ini` accepts any `VEYRA_EXP_RP_*` key, so it can
also turn on `VEYRA_EXP_RP_WIFI_STREAMING_MODE`, `VEYRA_EXP_RP_EARLY_LOSS` and `VEYRA_EXP_RP_REORDER_WRAP_FIX` (and
the trace). These are as experimental there as in the environment.

**Review net2 fixes (branch `exp/s4-fix1`, 20 Nov; Linux tests, blind Windows code):** one shared unit decision
after 3 agreeing reports, implausible values unknown (tests `console_quality_review_misreads_end_unknown_or_right`,
`console_quality_decides_the_unit_only_after_three_agreeing_reports`,
`console_units_shared_by_health_line_and_feed_and_logged_once`); rtt x 1e6 in patch 0004
(`console_quality_sub_millisecond_round_trip_in_seconds`; patches 0001-0004 apply in order to a fresh copy and
`reorder_library_check.sh` passes); a late packet counted in the window before its lost frame still gives
`lateRescueMs` (`smartlink_feed_late_rescue_carried_across_windows`); the trace buffer and rotation and the Wi-Fi-only
streaming mode above (Windows-only, not testable here).

### Upstream chiaki-ng fixes after the pin (section B of the research document)

| Upstream | Status here |
|---|---|
| `8134f45` re-request a lost IDR every second while waiting | **covered in effect, not ported:** with no decoded frame the app asks for a keyframe after 1 s (250-1000 ms with `VEYRA_EXP_RP_IDR_RETRY_MS`), again 2 s later, and reconnects after 6 s of silence (`StreamRecovery.h`, through `chiaki_session_request_idr`), so the frozen picture of the pin's library path ends within 6 s; upstream repeats every second until an IDR arrives. Porting it would add a second requester beside the app's; not done without a hardware run |
| `3e1a24d` reliable send buffer 16 -> 1024 | not ported: no evidence here of overflowing control messages; a size change at connect, candidate for a later switch |
| `727917c` reorder queue: dropped entries unset | not ported: `chiaki_reorder_queue_drop` is called only for the Takion **data** queue (`takion.c`), not the video queue patch 0004 works on; a correctness fix for control messages, candidate |
| `e5f1dd8` no send-buffer use after the receive loop ended | not ported: a teardown race fix touching the thread lifecycle (48 lines); out of this stream's scope this sprint |
| `bc397a7`, `c938844`, `bfbb296`, `4382c1c` feedback history | patch 0003 already sends one event per packet, numbers every event, fills the event count and resends the tail (by code reading of 0003, not a line-by-line comparison) |

### Congestion report and bitrate (directive item 2)

Every 200 ms the library sends `{received, lost}` (16-bit counts) measured from completed frames and audio sequence
gaps (`congestioncontrol.c`); `packet_loss_max` (the app passes 0.05, `ChiakiBackend.cpp`) clamps the reported loss to
5 % of the total. The app also passes `enable_idr_on_fec_failure=true` and turns `video_profile_auto_downgrade` off;
the requested rate is sent once in the launch spec. No known client message changes the rate during a session
(research document A1); the console adapts on its own from these reports, and its `target_bitrate` is now visible
(above). Nothing reported is changed in this phase: the research found no safe, supported lever (lowering or raising
the clamp shifts how hard the console reacts, unmeasured; the debug BITRATE/FPSCHANGE messages risk a disconnect).
The supported alternatives are SmartLink's `suggestedStartKbps` for the next session and showing "network-limited".

## SmartLink in the interface (stream "ui", branch `exp/s4-ui`, 2026-11-20)

Experimental; nothing here has run on Windows, a real PS5 or a network other than the test fixture.

- **Where:** quick setup, the "Connection" row (`pushfps-smartlink-row`, `qml/Veyra/PushSmartLinkRow.qml`), just
  after the performance mode row. Shown only in a Slipstream build (`SLIPSTREAM_BUILD=ON`) with PS5 Remote Play
  (`VEYRA_ENABLE_REMOTEPLAY`), and only for a PS5 Remote Play source, or with nothing playing while the console
  profile is a PS5 / PS5 Pro. Marked "Experimental".
- **The choice** (preference `pushSmartLink`, validated 0..2 in `Preferences.cpp`):
  - **Off** (0, the default): the app sets nothing. With the preference unset or 0 the behaviour is exactly as
    before.
  - **Advise** (1): sets `VEYRA_EXP_RP_SMARTLINK=1` (observe: SmartLink checks and logs, changes nothing).
  - **Automatic** (2): sets `VEYRA_EXP_RP_SMARTLINK=apply` (SmartLink may change the waiting times it supports).
  The variable is set for the NEXT PS5 connection (`applyPushSmartLink`, `src/ui/bridge/PushSmartLink.cpp`, at
  start-up and when the preference changes, like the LAN profile); a running stream keeps what it started with,
  and the row says so during a stream. A value already in the environment (set by the user or by `slipstream.ini`)
  wins and is never touched; only a value the app set is changed or removed. Nothing is set under
  `VEYRA_TEST_RP_NO_SETTINGS_FILE` (the test kit).
- **What is shown** while a PS5 stream runs and SmartLink's shared state is fresh (`pushSmartLink()`, once a second,
  from `SmartLinkShared.h`; stale = older than 2 s = nothing shown): the detected connection in plain words
  ("Wired", "Strong Wi-Fi", "Unstable Wi-Fi", "Checking…"), one advice sentence from the `Advice` value, and in
  Automatic only, and only when SmartLink changed something, "Waiting slightly longer for late pieces of the picture"
  (reorder wait set) and/or "Keeping a little more sound in reserve" (audio target set). `Decision` has no advice
  field yet, so the bridge reports advice 0 and no advice sentence appears until stream "core" adds one.
- **Log:** `[slipstream-config]` ends with `smartlink=off|observe|apply`: the mode the running stream published, else
  what `VEYRA_EXP_RP_SMARTLINK` says now.
- **Never:** a Windows, adapter or router setting; a network name, address or account; a claim that SmartLink
  improves anything. The interface says it "checks the connection and can adjust waiting times".
- **Tests:** `tests/qml/quick/tst_push_smartlink.qml` (default off, the three choices, refused keys and values,
  hints, every profile and advice text, changes only in Automatic, nothing when not fresh, hidden for other sources
  and builds); run locally with Qt 6.4 offscreen. Not validated: the bridge (MSVC has not compiled it here), the
  clock agreement with the publisher (`steady_clock` milliseconds assumed), and anything on hardware.

## Presentation stream 2 (sprint 4, branch exp/s4-present2): why the original build reached short present->scanout

Evidence and reasoning: docs/PRESENTATION_LATENCY.md section 8. All switches OFF unless set to `1`, read once per
process; with none set the program behaves as before. Diagnostics only under `VEYRA_EXP_RP_STATS=1`.

- `VEYRA_EXP_PRESENT_TEST_UNCOVERED=1` (`apps/veyra-qml/main.cpp` `collectCovers` / `syncVideoCovers`): while the
  built-in PS5 latency test RUNS, its dialog is not cut out of the native video window, so the video window keeps
  its whole rectangle (no `SetWindowRgn` region) during the timed presses. The dialog is hidden behind the picture
  until the test ends (it ends by itself after its presses) and then shows again for the next start. Logs
  `[slipstream-present] testUncovered=1|0` at each change. Risk: the test cannot be cancelled with the mouse while it
  runs (the dialog is hidden); closing the window still ends it. Question it answers: does the dialog's hole in the
  video window change present->scanout during a latency test.
- `VEYRA_EXP_PRESENT_LOW_QUEUE_OFF=1` (`src/engine/VideoPresenter.cpp` `configurePresentation`): presents as the
  original build ran in BENCH1 (`[pacing] requested=false/0/1`): the low queue (media-deadline hold before each
  present) and NVIDIA low latency are off for this run whatever the saved preference says; VSync, the one-deep
  display queue and VRR are unchanged. Logs `[slipstream-present] lowQueueOff applied ...` when it overrides a
  preference. Question it answers: does the low-queue setting (the one presentation setting that differed between the
  builds in BENCH1) keep the stable build out of the short-delay regime.
- `VEYRA_EXP_RP_STATS=1` adds: `scanoutUnder1Refresh= scanout1To2Refresh= scanoutOver2Refresh= refreshMs=` at the
  end of `[slipstream-latency]` (dated presents of the 5 s window by present->scanout); every 5 s
  `[slipstream-present] window event=periodic ...` (video window and monitor rectangles, `coversMonitor`, the video
  window's clipping region `regionKind` 0 none / 1 empty / 2 one rectangle / 3 complex and `regionRects`, `overlay`,
  `ownOverlaps` = visible top-level windows of this program over the picture, the top-level window's popup / caption /
  maximized / topmost / layered styles, `foreground`, `flipBlockers` bit mask 1 monitor not covered, 2 region, 4 own
  window over it, 8 not foreground) and `[slipstream-present] vrr level= known= requested= active= displayInVrrMode=`
  with a forced re-read of the driver's VRR state on the display worker every 5 s; `[slipstream-present] window
  event=change ...` within 250 ms of a dialog, menu, tip or badge opening or closing over the picture, a move/resize
  or a foreground change. Numbers and booleans only: no window titles, nothing of another process.
- With `VEYRA_EXP_PRESENT_VRR_REQUEST=1` the `vrr-confirm` re-read now always writes `[display-vrr]`, also when the
  state did not change; the read-only G-SYNC profile worker is joined at process exit instead of detached.


Second review round (branch `exp/s4-autofix`, 2026-11-20; details in `docs/LATENCY_MEASUREMENT.md` 2(h)):

| Switch | Effect | Unset |
|---|---|---|
| `VEYRA_EXP_RP_LATENCY_BUTTONS=1` | the latency test may use `options` / `dpad-right-left` (the "Anywhere" picture-change detector) | those buttons are refused before anything is pressed: the bridge logs `[push-latency] refused reason=buttons-experimental button=<b>`; a plan step `latency:<n>:options` / `:dpad-right-left` logs `state=failed reason=buttons-experimental` right after its `start` line; the status map's `buttonsAvailable` is false so the dialog can hide the choice |

The alternative-button detector was **not accepted by independent review as of this build**: with a still picture
before the press, something that starts changing after it (an object starting to walk, a tile pulsing every
0.5-1 s) was reported as a plausible press time, slow fades were timed late, and under film grain late answers were
lost more often than early ones. The fixes in 2(h)-(j) of `LATENCY_MEASUREMENT.md` address these on synthetic
pictures only (Linux model); until a review accepts them the buttons stay behind the switch. `slipstream.ini` may
set the switch like the other `VEYRA_EXP_RP_` switches. The PS button test, at any number of presses up to 500,
needs no switch and is unchanged.

With the switch, OPTIONS / D-pad tests add **sham presses** (`LATENCY_MEASUREMENT.md` 2(i)): max(14, presses / 4)
slots in which nothing is pressed, in random order among the presses, judged like a press and never in a statistic.
Kit-visible lines (PS-button tests unchanged):
- `[push-latency] shams=<n> (nothing pressed, judged like a press)` after the `v2` line; `[push-latency] sham <i>
  outcome=sham-clean|sham-answered|sham-moving` per sham; `[slipstream-press] ... outcome=sham-clean|sham-answered|
  sham-moving` (times -1; `totalMs` = when the detector answered for `sham-answered`).
- At the end `[push-latency] shams= shamAnswered= shamMoving= allowed= valid=0|1`. Valid: the usual `done ...` and
  `trials answerMs=[...]`. Not valid: `[push-latency] not valid reason=picture-changes-by-itself answered=<a>/<n>` and
  `trials invalid=1 answerMs=[...]` (raw values, not a result; no `done` line), state failed.
- `[slipstream-latency-test] ... shams= shamAnswered= shamMoving= valid=` appended (OPTIONS / D-pad only); when
  `valid=0` every latency figure is -1.
- Status map: `shams`, `shamsPlanned`, `shamAnswered`, `valid`; `error` = `stopReason` (`picture-moving` or
  `picture-changes-by-itself`) on an early stop or an invalid test. Plan step timeout counts the shams.

## Presentation follow-up after review (20 Nov 2026): `VEYRA_EXP_PRESENT_WINDOW_FACTS=1`

Off unless set to `1`; read once per swap chain; only has an effect together with `VEYRA_EXP_RP_STATS=1`. It turns
on the `[slipstream-present] window ...` lines (the video window's rectangle, region and this program's own windows
over it, four times a second and every 5 s) and a forced re-read of the driver's variable-refresh state every 5 s.
Both were part of every STATS run at first; the review moved them behind this switch because they query the
desktop and the driver from or beside the presenting thread and could disturb the latency being measured. The kit
sets it in the presentation arms only (`p-window`, `p-fullscreen`, `p-vrr`, `p-tearing`, `p-lowqueue-off`,
`p-test-uncovered`). Blind Windows code (`VideoPresenter::latencyObserved`, `openSlipstreamExperiments`); unverified
on Windows. `[slipstream-config]` reports the low-latency queue and NVIDIA low latency as off while
`VEYRA_EXP_PRESENT_LOW_QUEUE_OFF=1` overrides them.

## Sprint 5, stream "present": audio on its own clock, scan-out audit, one-rectangle video window (branch `exp/s5-present`, 2026-11-20)

Source study: moonlight-qt 2dc63d1f1385 and Sunshine de1e671e815c, both GPL-3.0 (summarised in the sprint's open-source review).
Nothing was copied from either: the four items re-implement rules those projects follow, in our own code. Set each
to `1` to turn it on (`VEYRA_EXP_PRESENT_RECT_REGION` takes exactly `1`; the `VEYRA_EXP_RP_*` ones, like the older
ones, take any value but empty or `0`); with none set the program behaves as before. Blind Windows code and Linux
tests of pure headers: unverified on Windows, nothing measured.

### `VEYRA_EXP_RP_AUDIO_OWN_CLOCK=1` (read at each PS5 stream start)

Evidence (BENCH2, real PC, all 10 undisturbed runs): `[remoteplay-audio] targetMs` 249-537 ms and `delayMs` 190-450 ms
after 40 s, 3-10 hard resets a minute, each a pop. Cause by code reading: the automatic rule waits for the picture by
`Present time - video PTS` (`CaptureSyncTarget::observe` without an arrival, `CaptureAudioSession.cpp`), and the
Remote Play video PTS is frame index / nominal rate with at most 0.5 % correction (`RemotePlayClock::video`,
`src/remoteplay/Timeline.cpp`). A 60 profile that delivers about 59 frames a second falls behind real time by 6-13 ms
every second; the audio target follows; the 5000 ppm trim (5 ms/s) cannot; the 60 ms rule fades and restarts.
`VEYRA_EXP_RP_AUDIO_FAST_RECOVER` uses the same `Present time - video PTS` and `VEYRA_EXP_RP_AUDIO_SMOOTH` only
softens underruns, so neither removes the cause.

Rule (what moonlight-qt does, `app/streaming/audio/renderers/sdlaud.cpp` `submitAudio`: audio is scheduled from its
own arrival, with a bounded queue and no video timestamp): with the switch, in automatic sync mode the picture term
of the schedule is the LOCAL picture delay, the Present time of a real frame minus the host time its data arrived
(`include/veyra/sink/AudioOwnClock.h` `AudioOwnClockTarget`: median of the last 9 real frames, at most 250 ms, moving
at most 4 ms/s while audio plays, at once while it is silent). Unchanged: the audio arrival mapping
(`ArrivalClockMapping`), the rate trim, the 60 ms restart rule (still the queue's bound after a real stall), the 20 ms
reserve, manual offset and Off. The engine reports a shown REAL frame together with the arrival time it already keeps
for that frame (`captureArrival`, one changed line in `EngineController.cpp`, as it does for a capture card); a
generated or re-read frame is reported without one and is ignored by the target. Until a first real frame is shown the
default rule stays in force (`ownFrames=0` in the log shows that case). `[remoteplay-audio]` gains `ownClock=1 ownTargetMs= ownMedianMs=
ownFrames= ownRejected=`.

Linux model (`AudioPathModel.h` with `videoStampHz`: timestamps advance 1/60 per delivered frame, 0.5 % correction;
tests `tests/remoteplay/AudioOwnClockTests.inc`; virtual time, not a measurement), 60 s, picture delay 12 ms:

| Delivered frames/s (stamped as 60) | Rule | Restarts | Largest target (ms) | Audio delay median / P95 / max (ms) |
|---|---|---|---|---|
| 59.4 | default | 0 | 313 | 150 / 277 / 291 |
| 59.4 | own clock | 0 | 13 | 12 / 12 / 12 |
| 59.0 | default | 8 | 716 | 351 / 676 / 679 |
| 59.0 | own clock | 0 | 13 | 12 / 12 / 12 |
| 58.5 | default | 16 | 1159 | 573 / 1054 / 1121 |
| 58.5 | own clock | 0 | 13 | 12 / 12 / 12 |

With true timestamps (the model's old assumption) both rules give the same delay within 3 ms. A +25 ms change of the
picture delay mid-stream (frame generation switched on) is followed in about 6 s without a restart, also together
with `VEYRA_EXP_RP_AUDIO_FAST_RECOVER`. A 300 ms network stall still restarts the audio from fresh sound (at most 2
restarts in the model) instead of playing late.

Lip sync, plainly: sound and picture keep the relation they had when they ARRIVED at the PC; the console's own offset
between them on the wire is unknown and not corrected (the default rule does not know it either). Present-to-screen
time (1-9 ms in BENCH2) and anything after the audio device's clock are not included. Needs listening on the real
PC: a minute of gameplay without pops, and speech against lips in a cutscene.

### `[slipstream-scanout]` (with `VEYRA_EXP_RP_STATS=1`, every 5 s; instrumentation only)

Evidence (BENCH2): once variable refresh is active only about 1 present in 6 gets a scan-out date and
`replacedAtLeast` counts hundreds per run although every present went out with sync interval 1, which DXGI never
replaces. `PresentSink::sampleDisplayLatency` infers "replaced" from `presents shown - refreshes counted`, which only
holds on a fixed refresh. The code does not say what the DXGI statistics do under variable refresh (refused as
disjoint, standing still, or counting differently), so `include/veyra/engine/PushScanoutAudit.h` records what they
did: `presents samples failed disjoint noSync unchanged named exact gapRejected passedVsync passedSync0 refreshShort
unsettled stepP50 stepMax statsAgeMsP50 statsAgeMsMax namedPct syncInterval`. `passedSync0` (presents issued with
sync interval 0 that the statistics skipped) is the only count that can mean "replaced before shown"; `refreshShort`
is the old quantity, kept for comparison. One more struct fill per present from the `GetFrameStatistics` call that
was already made; `[display-latency]` is unchanged. Tests `tests/remoteplay/ScanoutAuditTests.inc` (made-up
statistics sequences).

### `VEYRA_EXP_PRESENT_RECT_REGION=1` (read once per process) and `[slipstream-present] covers ...`

Evidence (BENCH2, all 8 presentation runs): present to scan-out is 7.3-9.4 ms (P50, about 50 dated frames a second,
variable refresh reported not active) exactly while `[slipstream-present] window` reports `regionKind=3
regionRects=56`, and about 1 ms from the second in which it reports `regionKind=2 regionRects=1`; the driver reports
variable refresh requested and active after that. The region is ours: `syncVideoCovers` (`apps/veyra-qml/main.cpp`)
cuts every interface item marked `videoCover` out of the native video window with rounded holes; one item is over
the picture for the first two seconds of a stream; after that the log still counts one cover (`covers=1`) but the
region is a single rectangle, so the item no longer cuts into the picture. The
four arms of the session (windowed, full screen, low queue off, test dialog uncovered) did not change this, which is
why it looked unexplained. Not proven: that the SHAPE of the region is the cause rather than the item being visible
(the two change together in the logs). BENCH1 (slow for whole runs, the owner's own layout) logged no window facts.

With the switch, while items are over the picture the window keeps ONE rectangle: the largest part of the picture
that no item touches (`include/veyra/engine/PushCoverRegion.h` `pushLargestClearRect`, at most 8 items, else the old
rule). Cost: the strip of picture beside the item is hidden while it is up. moonlight-qt avoids the question by
drawing its overlays inside the swap chain (`d3d11va.cpp` `renderFrame`); that is the real fix if the experiment
confirms the cause. With `VEYRA_EXP_RP_STATS=1` (or the switch) one line per change of the set of items:
`[slipstream-present] covers count= inPicture= rectRegion= items=<QmlType/objectName,...>` (identifiers from this
program's source only). Tests `tests/remoteplay/CoverRegionTests.inc`.

### Read and found already bounded (no change)

Frame hand-off: the source mailbox holds one frame and the newest wins (`remoteplay::LatestMailbox`, overwrites counted
as `mailboxOverwritten`); the engine does not read it while a job waits in `LiveGpuScheduler` (low queue on, no frame
generation) or a completion is pending (`EngineController.cpp`, "Backpressure before reading the capacity-one source
mailbox"); the scheduler's capacity of 2 is the frame-generation look-ahead; the DXGI queue is 1. moonlight-qt's pacer
allows up to 3 (`pacer.cpp`). Nothing to tighten.

### `VEYRA_EXP_RP_DECODE_KEEP_HW=1` (read at each PS5 stream start; network reliability, priority 6)

Code reading (`RemotePlaySource::read`, exp/rc5): in decode mode Automatic the FIRST failed submission while the
D3D12VA decoder is in use (`avcodec_send_packet` / `avcodec_receive_frame` error, a decoded picture with no matching
packet, a picture larger than 1920x1080) frees the hardware decoder and decodes in software for the rest of the
connection (`hardwareFallback_`, cleared only by a new `connect`), even after thousands of hardware frames. On a lossy
link (Wi-Fi) such a failure is far more often the stream (lost references, a damaged unit) than the hardware; the
software decoder then costs every later frame its decode time and a CPU core. No BENCH run shows it (they were short
and their logs say `actual=D3D12VA fallback=false` throughout): this is a gap found by reading, not a measured fault.

Rule (moonlight-qt `app/streaming/video/ffmpeg.cpp`, decoder thread: every decode error asks for an IDR, and only
`FAILED_DECODES_RESET_THRESHOLD` = 20 consecutive failures rebuild the decoder; one error never gives up hardware):
with the switch, a failure after hardware decoding has produced a frame on this connection is a stream error:
flushed, a keyframe is asked for (`SessionInbox::decodeFailed`, exactly what the software path does today), D3D12VA
stays. Three failures with fewer than 60 decoded frames between them (review 21 Nov: a decoder failing after every
keyframe must not stay on hardware), or a failure before hardware has decoded anything, fall back
as today (`include/veyra/remoteplay/DecodeFallback.h` `HardwareDecodeFallback`; 3, not 20, because each failure here
costs a keyframe round trip and `StreamRecovery` reconnects after 6 s without a decoded frame, which clears the choice:
a decoder that really broke must fall back before that, or it would reconnect in a loop). Nothing is buffered, nothing
waits longer than today; decode modes Hardware and Software and SmartLink are untouched. Log: a `keepHw=1` marker at
each stream start; on a kept failure `[remoteplay-decode] hardware decode failed; kept, waiting for a keyframe (keepHw=1
consecutive= of 3 hardwareFrames= kept= fallbacks=)`; on a fallback with the switch the old warning plus `(keepHw=1
hardwareFrames= kept= fallbacks=)`. Tests `tests/remoteplay/DecodeFallbackTests.inc` (made-up failure sequences).
Kit: a Wi-Fi session of several minutes with `VEYRA_EXP_RP_STATS=1` against the same plus the switch; decides:
count of `switching to software` lines (base) against `kept` lines and the `actual=` decoder in the last
`[remoteplay-decode]` line, `arrivalToDecodedP50/P95` of `[slipstream-latency]` after the first failure (software
decoding shows here), and `[remoteplay-progress] idrRequests`, which must not rise beyond one per kept failure.

### `[remoteplay-transport] sendBufferSize= sendBufferOverflows=` (always; counters only)

At the very end of both forms of the 1 s `[remoteplay-transport]` line: the size of the newest Takion connection's
reliable send buffer and how many reliable packets found it full (sent once, never resent), read in
`readTransportCounters` (`ChiakiBackend.cpp`) from the chiaki library's patch 0010 counters
(`CHIAKI_VEYRA_STAT_SEND_BUFFER_SIZE` / `_OVERFLOWS`) when the library declares `CHIAKI_VEYRA_TRANSPORT_SEND_BUFFER_API`;
0 and 0 with a library built without the patch. They measure patch 0009's switch `VEYRA_EXP_RP_CHIAKI_SEND_BUFFER`
(16 against 1024). Nothing sent or received changes.


### Uneven motion at about 120 FPS: analysis, `[slipstream-cadence]`, `VEYRA_EXP_PRESENT_EVEN_CADENCE=1` (21 Nov)

The owner's report: at about 120 FPS the motion is sometimes very smooth and sometimes visibly less fluid while the
counter still says about 120. No BENCH run had frame generation on, so everything below is CODE READING; nothing
here is measured, and nothing is claimed fixed.

What the counter is: `presentSubmitFps` (`FrameFlowWindow`), the rate of Present CALLS ("not scanout FPS"). It says
120 whatever the panel does with them. Mechanisms that make 120 calls a second look uneven, most likely first:

1. **Fixed refresh (variable refresh not engaged).** Every present goes out with sync interval 1
   (`PresentSink::present`, `[present-contract] sync=1`); on a fixed refresh each is shown at a vblank of its own.
   120 presents on the 179.95 Hz panel can then only be held 1 and 2 refreshes in turn (5.6 / 11.1 ms): judder at a
   steady 120 on the counter. With G-SYNC active the panel follows the presents and the same stream is even. BENCH1
   logged `[display-vrr] requested=false active=false` for whole runs; BENCH2 at start, and requested + active only
   from the 5 s re-read (`VEYRA_EXP_PRESENT_WINDOW_FACTS=1`). Whether G-SYNC engages, late or not at all, from one
   session or window state to the next would give exactly "sometimes smooth, sometimes not". Seen in:
   `[slipstream-present] vrr ... active=`, `[display-vrr]`, `[display-latency] presentToScanoutP50Ms` (about 8-9 ms
   while not engaged, about 1 ms engaged, BENCH2), and now `[slipstream-cadence] vrr= fixedUnevenPct=`.
2. **One missing source frame a second.** The console delivers about 59 of 60 frames (BENCH2). With 2x generation a
   missing frame leaves one interval of 25 ms (generated frame to the next real one) a second where 8.3 ms is
   expected; native 60 shows the same hole as one 33 ms hold. Seen in: `[remoteplay-progress] inputFps`,
   `[slipstream-cadence] intervalMaxMs offPct`.
3. **Generation pairs not presented.** Admission rejects a pair over the GPU budget (`[live-fg-admission]
   rejectedPairs`, `[frame-flow] fgSkippedBeforeEval fgReduced`), a held-rejection second shows real frames only
   (`[live-fg-hold] held=1`), and a generated frame is dropped when it expires (`[fg-drops] deadline=`) or the
   presenter is not ready within min(2 ms, half its interval) (`[fg-drops] capacity=`; `EngineController.cpp`, the
   "Explicit VSync" block of the presentation step). Each costs one 16.7 ms interval.
4. **Placement inside the pair.** A generated frame's deadline is the pair's smoothed anchor plus its timestamp
   offset (`PresentationScheduler::deadline`, `presentAnchorFor`); anchor snaps and arrival jitter move it
   (`[live-anchor] snaps= callbackJitterMs=`, `[fg-deadline] decisionLateMs=`). On a fixed refresh the vblank grid
   quantises it again (item 1).
5. **Replaced presents / queue build-up.** Not with sync interval 1 and maximum latency 1 (DXGI never replaces such
   presents; the scheduler drops instead of queueing). Only display sync Automatic (`Present(0,0)`) replaces: then
   `[present-contract] sync=0` and `[slipstream-scanout] passedSync0` > 0.
6. **Content-rate switching.** Duplicate removal follows the content-rate setting (`dedupContent`), and for the PS5
   stream only with `VEYRA_EXP_RP_STREAM_DEDUP`; adaptive frame generation changes the multiplier with a 0.5 s gate
   (`[push-adaptive-fg] chosen= changed=`). A switch changes the output rate, visible as a step in
   `[slipstream-cadence] submittedFps`.

`[slipstream-cadence]` (with `VEYRA_EXP_RP_STATS=1`, every 5 s, closed at the newest present's call time so the
rates are exact; `vrr` as `pushVrrCadenceState`; a window with a gap over 250 ms counts `pauses=`; `include/veyra/engine/PushCadence.h`, wired in
`VideoPresenter::present` and `latencyObserved`): `window submittedFps realFps generatedFps intervals intervalP50Ms
intervalP95Ms intervalP99Ms intervalMaxMs offPct refreshHz vrr fixedHold1 fixedHold2 fixedHold3 fixedUnevenPct
fixedShownFps evenFps evenDivisor overflow`. Call spacing of every present (real and generated; XeSS present-sink
generation is inside the provider and not counted); `offPct` = intervals off the mean by more than half a refresh;
`fixed*` = what sync interval 1 shows on a fixed refresh (each present on the first free vblank at or after its call),
a model of DXGI's rule, not a measurement, and not applicable while `vrr=1`; `evenFps` = refresh / `evenDivisor`, the
highest rate at or below the present rate with equal holds. Decoded source rate: `[remoteplay-progress] decodeFps`;
skipped and dropped generation: the lines of item 3. Tests `tests/remoteplay/CadenceTests.inc`.

`VEYRA_EXP_PRESENT_EVEN_CADENCE=1` (read once; exactly `1`): only where adaptive frame generation already chooses the
multiplier per pair (`fgAdaptive` on, ceiling 3X or more, graph frame generation), and only while the VRR state used by
the cadence figures is 0 (`pushVrrCadenceState`: the driver answered, VRR not enabled, in a reading taken after the
chain's first present, no confirmation pending, no resize in the last 3 s): the multiplier whose output has equal holds on the refresh, at or below the
output cap / refresh, closest to the target (`pushEvenFgMultiplier`). Review (21 Nov, a sweep of 456 cases):
**it changes nothing in the owner's case** (60 FPS picture on 179.95 Hz: adaptive already picks 3X for a refresh
target; with a 120 cap nothing is even above 1X, so 2X stays); where it changes the choice (41 of 456) it always picks a
LOWER multiplier. So it is no fallback for the reported symptom and has no kit arm. The fallbacks that exist are
settings: frame generation off (native 60, each frame held 3 refreshes on 180 Hz) or 3X with no output cap on a 180 Hz
panel, or G-SYNC actually engaged.

Interface readout (always on, no switch): the presenter keeps a 1 s cadence window and publishes
`engine::PushLiveCadence`; `pushLiveStats()` exposes `shownFpsKnown shownFps vrrActive unevenKnown unevenPct uneven`.
Every figure there is INFERRED from our own Present calls and DXGI's sync rule; no presentation statistics are read
and nothing measures the panel (PresentMon does). The latency badge, the statistics overlay and the streaming page say
显示 (shown) only when it is known, otherwise
提交 (submitted), and add 不均匀 (uneven) when more than 20 % of the holds differ from the most common one (fixed
refresh, the model) or of the present intervals are off their mean by more than half a refresh (variable refresh).
"Shown" is inferred: the present rate capped at the refresh while presents go out with sync interval 1 or more (DXGI
then shows each for at least one refresh); it is not known with display sync Automatic or XeSS SDK generation.
`vrrActive` / `vrr=` follow `pushVrrCadenceState` (phase-2 review, 21 Nov): 1 only when NVAPI reports VRR enabled
(`bIsVRREnabled`); the display-mode flag `bIsDisplayInVRRMode` is ignored, because it describes the monitor's mode
and was 1 in all 144 readings of the owner's panel, including BENCH1's whole runs on a fixed refresh grid
(docs/PRESENTATION_LATENCY.md); 0 when the driver answered and VRR is not enabled in a reading taken after this
chain's first present (always-on forced reads on the display worker, `cadence-confirm`: 5 s after it, and once more at about 15 s while VRR reads not enabled; a resize / full-screen change forgets the reading and reads again 5 s later), with no
confirmation pending and no resize / full-screen change in the last 3 s; -1 otherwise (no driver answer, as on AMD /
Intel, the start-up reading only, a pending confirmation, a recent resize), and then nothing is claimed about evenness.
So on the owner's panel a 2X stream on the fixed grid reads "uneven" after about 5 s, and with G-SYNC engaged it does
not. A cadence window with a gap over 250 ms (a pause) is no rate and is not shown.
