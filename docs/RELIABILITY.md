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
| Displayed frame times, dropped frames | PresentMon `MsBetweenDisplayChange`, dropped rows | yes, with PresentMon in the kit folder and an elevated PowerShell |
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
| Build and switches per arm | `kit.json` (file hash, file version), `run.json` (switches), `[qml] [update-check] build=`, `[remoteplay-timing]`, `[remoteplay-lan]`, `[remoteplay-settings]` | yes; no log line states the commit (see the report of this work for the fields still missing) |

The report section **What this evidence holds** repeats this table for the folder at hand, per arm, with
`MISSING` and how to get the value. The report **refuses** a comparison (no difference, no interval) when
the two arms ran with different stream settings, a different present mode or tearing setting, or a
different variable refresh state, or when the runs of one arm did not all have the same settings or
switches. What one side did not record is named as not checked, never assumed equal.

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
| `gpu-device-loss` (150 s, own start, only with `-ReliabilityGpuLoss`) | Connect; about 15 s later PushFPS removes **its own** graphics device once; connect again | `[engine-test]`, `[gpu-failure]`, `[gpu-reconnect]`, `[crash]`, `presentSubmitFps` after | no crash or restart, and a picture again within the step. PushFPS does not reconnect a PS5 session by itself after a device loss today, so a pass needs your reconnect and says so |

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

A cable pull of 1 s and of 5 s with a second device loading the network is also in the older
`-Impairment` run, which records markers only and leaves the judgement to the report's table.

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
