# Reliability and measurement: what the kit records, and how it is judged

Companion to `HARDWARE_TEST_PROCEDURE.md`. Everything here is **unverified on Windows**: the scripts were
written without a Windows PC and have only been checked statically and, by the checks workflow, in a dry
run on made-up data. Nothing has run against a PS5.

Kit: `scripts\remoteplay\testkit\run-tests.ps1` (kit version 2). Reading the result:
`python tools\remoteplay_latency_bench\report.py <zip>`. Every number in the report is what the runs in
the zip show; the built-in latency test is a software clock (press handed to the session until the answer
is in a presented picture) and is never input to photon. Only the camera table is.

## 1. What a comparison needs, where it comes from, what the kit collects

| Value | Source | Collected |
|---|---|---|
| Software latency per press: median, P95, P99 | `[push-latency] trials answerMs=[...]` | yes; the original build logs run summaries only (no P95/P99) |
| Displayed frame times, dropped frames | PresentMon `MsBetweenDisplayChange`, dropped rows | yes, with PresentMon beside the kit (only PresentMon is elevated: one prompt per run, none for a member of "Performance Log Users"); `presentmon.py` reads it by header names |
| Present mode, tearing | PresentMon `PresentMode`, `AllowsTearing` | yes, with PresentMon |
| Variable refresh state | `[display-vrr]`, `[display-refresh]`, `[display-latency]` | yes from kit 2 (these families were not copied before) |
| GPU load, video memory | nvidia-smi (whole first GPU); Windows GPU counters for the PushFPS process (any maker); `[frame-rate] vramMiB` | yes from kit 2 for the process and non-NVIDIA cards |
| Decoder load | nvidia-smi `utilization.decoder`; Windows `engtype_VideoDecode` of the process; PresentMon `VideoBusy` | yes |
| CPU load | `samples.csv` (whole PC and the process) | yes |
| Network health: loss, jitter, late packets, bitrate, route type | `[remoteplay-health]`, logged only with `VEYRA_EXP_RP_HEALTH=1` | with `-Health` (all arms alike); always in soak, impairment and reliability runs |
| Recovery events | `[remoteplay-recovery]`, `[remoteplay-transport]` | yes |
| Duplicate and cadence statistics | `[remoteplay-cadence]`, logged only with `VEYRA_EXP_RP_STREAM_DEDUP=1` | when the arm sets that switch |
| Frames presented: real, generated, expired, cancelled | `[frame-flow]`, `[fg-validity]`, `[fg-drops]` | yes from kit 2 |
| Stream settings: resolution, frame rate, codec, requested bitrate | `[remoteplay] session started`, `[remoteplay-decode] actual=` | yes |
| Hardware and Windows settings | `hardware.json` (`detect-hardware.ps1`) | yes |
| Presentation settings: low-latency queue, display sync, VRR request, DXGI waitable / frame latency / V-Sync / tearing | `[slipstream-config]` when the build writes it, else `[pacing] revision= requested=a/b/c` and `[pacing] DXGI` | yes from kit 4; compared key by key (see below) |
| PC pipeline, console + network and total per press; frame delivery; audio pops; SR and FG cost | `[slipstream-press]`, `[slipstream-latency]`, `[slipstream-delivery]`, `[remoteplay-audio]`, `[slipstream-sr]`, `[slipstream-fg]` (experimental build, `VEYRA_EXP_RP_STATS=1`) | yes from kit 4, in experimental arms; `[frame-rate]` and `[frame-flow]` give frame delivery in every build |
| Network route (Wi-Fi or wired) | `[slipstream-config] route=`, `[remoteplay-health] link=`, `hardware.json` default route | yes; Wi-Fi labels every table and verdict "diagnostic only" |
| Build and switches per arm | `kit.json` (file hash, file version), `run.json` (switches), `[qml] [update-check] build=`, `[remoteplay-timing]`, `[remoteplay-lan]`, `[remoteplay-settings]` | yes; no log line states the commit (see the report of this work for the fields still missing) |

The report section **What this evidence holds** repeats this table for the folder at hand, per arm, with
`MISSING` and how to get the value. The report **refuses** a comparison (no difference, no interval) when
the two arms ran with different stream settings, a different present mode or tearing setting, or a
different variable refresh state, or when the runs of one arm did not all have the same settings or
switches. What one side did not record is named as not checked, never assumed equal.

From kit 4 (sprint 4) both the kit and the report also compare every configuration key both arms logged
(`report.py` `infer_config` / `normal_config` / `config_refusal`, `run-tests.ps1` `Update-ConfigKeys` /
`Merge-ConfigKeys` / `Compare-ConfigKeys`): any difference outside the keys the switch under test may change
(`arms.csv` column `differs`) refuses the comparison, with the keys, both values and a one-line fix. Every copy
is started with the kit's test profile (`docs/HARDWARE_TEST_PROCEDURE.md`, "In short") so that this should not
happen; a run whose log shows the profile did not take effect is named at once and can be repeated.

## 2. Regression check against a named baseline

```
python tools\remoteplay_latency_bench\baseline.py save rig-2026-11 <evidence zip> --arm mod-legacy
python tools\remoteplay_latency_bench\baseline.py compare rig-2026-11 <new evidence zip> --candidate mod-all
```

`save` stores, per subsystem (input latency, network stability, decode latency, frame pacing, GPU
utilisation, memory, image quality when a quality test was run), the median, P95, P99 and sample counts
of one arm, with the hardware, stream settings and present mode. `compare` pairs run *k* of the reference
arm with run *k* of the candidate in the **new** folder (the kit interleaves them) and prints one verdict
table. A row is a `REGRESSION` only when there are at least 3 pairs, the arms were interleaved and ran
under the same conditions, the 95 % interval of the paired difference excludes zero on the worse side
**and** the difference is larger than the metric's tolerance (`--tolerance metric=value` changes one).
Exit code 1 on a regression. A stored number from another day is never compared with the candidate
directly; when today's reference runs do not reproduce the stored baseline the table says `DRIFTED`.
Software latency rows are the built-in test's clock, not input to photon.

## 3. Soak

```powershell
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -SkipLatency -Soak                 # 2 hours
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -SkipLatency -Soak -SoakHours 6
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -SkipLatency -Soak -SoakMinutes 45
```

Connect, stand in the game or play, leave it. Recorded once a second: working set, private memory,
handles and threads of PushFPS; every other second its GPU load and video memory; from its log:
reconnects, keyframe requests, drops, the program's own video memory and processing times; from
PresentMon: one minute of frames every 15 minutes (`-SoakPresentMonSeconds`, `-SoakPresentMonEveryMinutes`).

**Kill-switch.** Press `Q` or `Ctrl+C` in the kit's window, or create a file named `stop.txt` in the
evidence folder (the kit prints the path; this also works from another PC over a shared folder). The kit
closes PushFPS, saves everything recorded so far and makes the zip. If PushFPS does not close within 30 s
of being asked (also at the end of the planned time) the kit ends it and records that.

**Reading it.** `report.py` (or `soak.py <zip>`, exit code 1 on a failure) reduces every value to one
median per minute, leaves out the first five minutes, fits a Theil-Sen line and reports the change per
hour. A value is flagged (`GROWTH`, or `DECLINE` for frame rates) only when the change per hour exceeds
its threshold (memory 20 MB or 2 % of the level, handles 30, threads 2, processing times 0.5 ms, frame
rates 0.5, frame time P99 1 ms) and each third of the run is worse than the one before; a level that
rose early and stayed is reported as that. Less than 30 minutes after the warm-up is "too short to judge".
Result: `FAIL` for a flagged trend, a crash, GPU failure, engine hang or abandoned recovery line, the
program ending by itself, or a forced stop; `INCOMPLETE` after the kill-switch; otherwise `PASS`.
The latency rows of a soak are processing times inside the program, not input to photon.

## 4. Reliability checks

```powershell
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -SkipLatency -Reliability                      # about 12 minutes
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -SkipLatency -Reliability -ReliabilityKbm      # with the focus check
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -SkipLatency -Reliability -ReliabilityGpuLoss  # optional, see below
```

No extra software. The kit starts PushFPS (arm `mod-all` unless `-ReliabilityArm`), you connect to the
PS5 and stand in the game. Each step starts when you press **Enter in the kit's window** (that writes a
time marker) and ends by itself after the time in the table (a countdown is printed; a beep marks the
end). At the end the kit prints one line per check and writes them to `reliability.csv`;
`report.py` (or `reliability.py <zip>`, exit code 1 on a `FAIL`) judges the same evidence again and
shows whether the two agree. `NOT-EXERCISED` means the log does not show that the thing was done;
`SKIPPED` that the check does not apply. A step you cannot do: press Enter and wait.

| Check | What you do | What the kit watches in the log | Pass |
|---|---|---|---|
| `cable-out-1s` (30 s) | Pull the network cable out of the PC for about 1 s, put it back | `[remoteplay-progress] decodeFps` before and after, `[remoteplay-recovery]` | decoded rate back to 90 % of the rate before within **10 s**; no "recovery stopped", crash, hang or restart of the program |
| `cable-out-5s` (75 s) | The same for about 5 s | the same | back within **60 s** (a reconnect is allowed; the console can hold the old session for about 20 s) |
| `display-mode-change` (60 s) | Change the refresh rate or resolution of the screen in Windows display settings, wait 5 s, change it back | `[display-refresh]`, `[display-query] invalidate`; `[crash]`, `[engine-hang]`, `[gpu-failure]`; decoded rate and `[frame-rate] presentSubmitFps` at the end | a display line was seen, none of the failure lines, the stream at 90 % of its rate and frames being presented when the step ends |
| `display-reconnect` (60 s) | Pull the monitor cable for 5 s, put it back (the beep tells you when the step is over; a single-monitor PC shows nothing meanwhile) | the same | the same |
| `focus-loss-keys-held` (30 s) | Keyboard and mouse mode (`-ReliabilityKbm`): hold W and the left mouse button, Alt+Tab away while holding, release, come back; answer the question | `[remoteplay-kbm] keyboard and mouse released (focus lost)` | that line, and you did not answer that input stayed held. Without keyboard and mouse mode: `SKIPPED` |
| `crash-recovery` (120 s) | Press Enter: **the kit ends PushFPS abruptly** (as a crash would) and starts it again; you connect to the PS5 again | the program's start line, `[crash]`, `session started`, decoded rate | the program started again, no crash line, pictures decoded again within the step. Started but not connected: `NOT-EXERCISED` |
| `safe-start` (20 s, own start) | Nothing: the kit starts PushFPS with **no switch and `slipstream.ini` not read** and closes it after 20 s | start line, `[crash]`, `[engine-hang]`, any "experiment" line | it started, stayed up, no experiment was active, it closed when asked |
| `gpu-device-loss` (150 s, own start, only with `-ReliabilityGpuLoss`) | Connect; about 15 s later PushFPS removes **its own** graphics device once; connect again | `[engine-test]`, `[gpu-failure]`, `[gpu-reconnect]`, `[crash]`, `presentSubmitFps` after | no crash or restart, and a picture again within the step. PushFPS does not reconnect a PS5 session by itself after a device loss today, so a pass needs your reconnect and says so (the experiment `VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1` changes this: see below) |

**Safe configuration reset.** `safe-start` shows that the build works with nothing set. To get there for
normal use: rename `slipstream.ini` beside `PushFPS.exe`, and remove any `VEYRA_EXP_RP_*` /
`VEYRA_TEST_RP_*` variables you set for your user (`Get-ChildItem Env:VEYRA_*` lists them). The kit never
changes either.

**Graphics device loss, how it is exercised.** `-ReliabilityGpuLoss` starts PushFPS with
`VEYRA_TEST_GPU_FAILURE=900:500`, a test hook of the program: after 900 frames of a live session it
treats one frame as lost and half a second later calls `RemoveDevice` on its own Direct3D device. No
driver is restarted and no other program is affected; the hook fires again in every new session of that
start, so close PushFPS when the step has ended. Optional, and a harsher alternative for a tester who
accepts it: `Win+Ctrl+Shift+B` (Windows restarts the graphics driver; every window flickers) during a
`display-mode-change` step. Never pull power or disable the adapter in Device Manager for this.

With the experiment `VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1` (environment, or `on` / `true` / `yes` / `1`
in `slipstream.ini`; off by default) PushFPS starts the PS5 session again by itself after this loss: no
failure notice, the playing page stays, and after 1 s (then 3 s, 10 s; at most 3 in 10 minutes) a new
session starts (`[remoteplay-recovery] device-loss resume attempt=<n> started`). Do not reconnect by hand
during the wait: that cancels the resume (`reason=other-source`). Limits: a loss that surfaces as an engine
exception does not resume; the check for a usable new device is weak; the console may answer "in use" for
a while, and the existing reconnect logic then waits 10, 20 and 40 s. All of it is unverified on Windows
and on a PS5 (details in docs/PS5_REMOTE_PLAY_EXPERIMENTS.md, "Resume after a GPU device loss").

A cable pull of 1 s and of 5 s with a second device loading the network is also in the older
`-Impairment` run, which records markers only and leaves the judgement to the report's table.

### GPU device loss with the resume switch (kit 4)

`run-tests.ps1 -Reliability -ReliabilityGpuLoss -ResumeAfterDeviceLoss` sets `VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1` for the
GPU loss run. The step then tells the tester to wait and touch nothing: connecting by hand during the wait cancels the
resume (`reason=other-source`). PASS when a `[remoteplay-recovery] device-loss resume attempt=N started` line is followed by
a running stream; otherwise FAIL with the reason in plain words (`budget`, `device-unavailable`, `other-source`, `user-stop`
...). The test hook (`VEYRA_TEST_GPU_FAILURE`) fails the GPU again in every resumed session, so attempts 1 to 3 and then
`reason=budget` is the EXPECTED sequence with the hook and is not scored as a failure. The same rule is in run-tests.ps1
(`Get-GpuLossVerdict`) and reliability.py (`gpu_loss_check`).

## 5. Diagnostics from a tester's PC (one click)

Double-click `scripts\remoteplay\testkit\collect-diagnostics.cmd` (or run `collect-diagnostics.ps1`).
It starts, installs and changes nothing; it reads and writes **one zip** to
`Documents\slipstream-diagnostics`, prints its summary and the path. No administrator rights, no Python.

In the zip: `summary.txt` (plain words: the PC in short, things that may cause problems, what is in the
zip and what is not), `hardware.json`, `network.txt` (adapter type and speed, 20 pings to the router, and
to the PS5 with `-Ps5Address`), `versions.txt`, `compatibility.txt` (overlay and capture tools from a
fixed list of names, what PushFPS logged about overlays, crashes, graphics failures and hangs, the
**names** of the switches in `slipstream.ini`), `sessions-<copy>.csv` (one row per PS5 session: length,
stream settings, decoded and presented frames per second, reconnects, loss, jitter, built-in latency
tests) and `log-<copy>.txt` (the last 8 MB of the log, the kit's families only, masked by the kit's rules).

**Not collected:** passwords and other credentials, console registration keys, PSN account ids, sign-in
tokens, Wi-Fi network names, personal files, recordings and screenshots, the settings store
(`runtime_local`), the full log, crash dump files (only their number), the computer name, the Windows user
name, folder paths, the list of installed or running programs. Every file is checked again by the kit's
rules before the zip is made; a file that fails is left out and named in `withheld.txt`.

## 6. Automated reliability tests on Linux (sprint 3, 19 November 2026)

Everything in this section ran on a shared Linux build machine (2 cores, GCC, x86-64). It exercises the
**portable** code on real threads; it is a model of the session, not the app. Nothing here ran on Windows,
on a GPU, on the RTX 5070 or against a PS5, and no number below is a latency measurement.

### 6.1 GPU test program: failures that explain themselves

`veyra_push_gpu_tests` (tests/integration/PushGpuTests.cpp) failed once on WARP in build-windows run #15
and passed on the same tree in #17; the log did not say which check. The program now prints, for every
failed check, one line `FAIL <test>: <what> got=<..> want=<..>`, a `RUN`/`TIME <test> <ms> ms` pair per test,
`class=hardware|software` on the adapter line and `SUMMARY run i/n: ... failed tests` at the end of each
run. stdout is unbuffered, an unhandled exception prints `FAIL <test>: crashed`, a test running longer
than `--test-timeout` seconds (default 1800) prints `FAIL <test>: timed out` and ends the program. New:
`--repeat N`, `--only a,b`, `--list`; without arguments it runs every test once, as before. A GPU wait
that ends early prints the fence values and the device state.

Suspects for the intermittent failure, most likely first (none confirmed: the run's log is gone; re-run
with `--repeat 20` on the WARP runner to find out):

| Where | What depends on timing | Likelihood |
|---|---|---|
| PushGpuTests.cpp:1092 (`testStreamChange` → `run`): `check(o.reading == o.queued && runner.reading() == o.queued, ...)` | `o.reading` is a snapshot taken inside `measure`, `runner.reading()` reads the fence again; with the 0 ms and 0.3 ms waits the list is usually still running at the snapshot, and WARP can finish it before the second read. Then the check fails although nothing is wrong. | high |
| PushGpuTests.cpp:1090: `check(o.waitMs < wait/10000 + 50, "the wait is bounded")` | `waitMs` includes resetting, recording and submitting the list with the D3D12 debug layer on; 50 ms of slack on a shared CI VM with a software adapter. | high |
| `testHudSampler` | the sampler takes a frame every 60 ms of wall time; the picture moved per loop turn, so which six pictures it took depended on the adapter's speed. | medium; **fixed** (the picture now moves per frame taken) |
| `testPushFlowRuntime` PSNR margins (`flowDb > blendDb + 2`, tier 0 detail) | WARP's DirectML float results; margins measured once on WARP. | low-medium |
| the PushFlowBench calls in the same CI step (`--sizes 640x360 --pairs 2` is the only one whose exit code fails the step) | ONNX Runtime on WARP. | low |
| `Gpu::wait` 60 s, `debugErrors()` | a slow submission or a debug-layer message from an earlier test is reported by the next check. | low (now printed with the test's name) |

Minimal fixes proposed to the owner of `testStreamChange` (not changed here): compare `runner.reading()`
only when it is still running (`!o.queued || runner.reading() || !runner.pending()`), or drop the second
half; and give the wait check the adapter's class (`gpu.software ? 500.0 : 50.0`).

### 6.2 Core-test additions (tests/remoteplay, in every verify.sh run)

| File | What | Plain time |
|---|---|---|
| StressTests.inc | WakeSignal (4 notifiers, ping-pong 5,000 rounds, notify before wait, clear), SessionInbox restarted 1,000 times under video, audio and stopped-session callbacks, inbox destroyed while callbacks run (100 rounds), ControllerQueue against the pad poll with resets and focus loss, LatestMailbox against 1,000 generations | 1.7 s |
| ConfigRecoveryTests.inc | slipstream.ini damaged every way (every truncation, bad lines among good ones, UTF-16, size limit, CRLF/BOM, duplicates, unknown keys, 22,000 random files); KBM profile (3,000 damaged texts); calibrated profile file (.bak, failed backup, failed write, damaged block) | 1.0 s |
| StuckInputTests.inc | stuck keys / buttons / mouse: focus loss mid-press, overlay, release hotkey, profile switch, device removal, session end, window change, calibration abort, two keys on one button, auto-repeat, 10,000 random sequences | 2.1 s |

All pass plain and under ThreadSanitizer (0 reports) and AddressSanitizer + UBSan.

**Finding (reported, not fixed: not this stream's file).** `VideoIngress::expiredLocked`
(src/remoteplay/VideoIngress.cpp:41) calls a frame expired when the consumer's `now` is older than the
frame's arrival, unless the backlog experiment is on. The owner reads its clock before it takes the inbox
lock (`RemotePlaySource::read`), so a frame that arrives into an empty queue between the two is dropped
with the whole queue and a keyframe is requested: a hitch caused by a race, with no real delay in it.
`known_defect_frame_arriving_after_the_owner_read_its_clock_is_not_expired` shows it and prints
`KNOWN-DEFECT` instead of failing. Minimal fix: `if(now<arrival)return false;` behind its own switch (or as
part of VEYRA_EXP_RP_BACKLOG's behaviour). How often it happens on Windows is not known (it needs the
owner to poll while a frame lands, or to be preempted between the clock read and the lock).

Other observations, by design and not defects: a slipstream.ini whose **last** line is cut inside a value
(no newline, no quote) stays valid with the shorter value, since a file without a final newline is normal;
the KBM profile file is read line by line (bad lines skipped and named), unlike slipstream.ini.

### 6.3 Pipeline soak (tools/remoteplay_latency_bench/soak_pipeline.cpp)

Four threads (network callbacks, input, pad poll, owner) drive SessionInbox / VideoIngress / WakeSignal /
ControllerQueue, KeyboardMousePad with the capture state machine and calibration, StreamCadence,
FrameJitter, ConnectionHealth, the adaptive reorder window, AudioLatencyController, the governor feed,
latency governor and GPU budget driver, with seeded impairments (arrival jitter, bursts, outages up to
300 ms, lost frames, owner stalls up to 150 ms, reconnects every 2-22 simulated minutes, late callbacks of
the ended session, focus loss, device removal, overlay toggles, profile switches, calibration aborts).
Invariants are checked continuously (section header of the file); RSS is sampled every simulated minute.
The inbox ages frames by the real clock, so frames are sent `speed` times faster than 59.94 fps and the
age limit is checked in real time; the other controllers run on the simulated clock.

```bash
tools/remoteplay_latency_bench/run_soak.sh --minutes 15 --seeds "11 12" --parallel 2
.claude/skills/pushfps-testing-release/scripts/verify.sh --soak 5     # verify.sh plus 5 minutes per build
```

| Build | Seeds | Speed | Simulated | Wall | Violations | Sanitizer reports | RSS after warm-up |
|---|---|---|---|---|---|---|---|
| plain -O2 | 11, 12 | 20x | 5.0 h each | 15 min each | 0 | - | flat (-0.6 MB) |
| ThreadSanitizer | 11, 12 | 4x | 1.0 h each | 15 min each | 0 | 0 | +2.1 / +2.4 MB (shadow memory, reported only) |
| AddressSanitizer + LeakSanitizer + UBSan | 11, 12 | 8x | 2.0 h each | 15 min each | 0 | 0 (no leaks) | +8.0 / +9.8 MB (quarantine, reported only) |
| plain -O2 | 21 | 20x | 4.0 h | 12 min | 0 | - | flat (-0.7 MB) |
| AddressSanitizer + LeakSanitizer + UBSan | 21 | 8x | 1.6 h | 12 min | 0 | 0 (no leaks) | +5.2 MB (reported only) |
| ThreadSanitizer | 21 | 4x | 0.8 h | 12 min | 0 | 0 | +3.2 MB (reported only) |

In all: 14 simulated hours plain, 5.6 under AddressSanitizer, 2.8 under ThreadSanitizer, 2 h 6 min of wall
time over nine runs, at most two at a time on a machine shared with other jobs (load average about 10).

Per plain run: about 1.08 million frames sent, 0.65 million taken, 23-27 reconnects, 73,000 releases each
checked for a neutral pad, 10,700 profile switches, 7,100 calibrations started and aborted, the controller
queue at its capacity (16) and never above, the video queue at most 4 frames and 262 kB, no frame older
than 100 ms handed over (largest 23 ms plain, 99.5 ms under ThreadSanitizer), no stale callback accepted.

Limits of the model: the GPU budget driver never asked for a change in these runs (`budgetChanges` 0) and
the reorder window changed 4 times per run, so those two controllers were mostly checked for staying
finite and in range, not for their decisions; FrameJitter values under the sanitizers are inflated by the
slow producer; per-session drop counters restart with each reconnect; the keyframe spacing (500 ms real)
is long in simulated time.

### 6.4 What still needs Windows or hardware

- The GPU tests' new output and options, the HUD sampler change: compiled by mingw's syntax check only.
  Needs build-windows (WARP) and then `--repeat 20` there to find the intermittent check.
- moonlight::InputRouter (Win32 message translation, modifier tracking), Raw Input registration and device
  notifications, the cursor clip: Windows only, not in the stuck-input model.
- slipstream.ini and profile file reads through CreateFileW / QFile, read-only files and folders (the Linux
  runs are root, which ignores permissions), the mode row's preference JSON (Qt).
- Everything about timing on the target: the owner thread's scheduling on Windows, the PS5's cadence,
  decode on the RTX 5070, presentation at 180 Hz.
