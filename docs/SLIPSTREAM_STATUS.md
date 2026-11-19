# Slipstream: what exists, and how far each part has been checked

Status on 19 November 2026. "Slipstream" is the temporary internal name of this low-latency PS5
Remote Play work on PushFPS (built on Veyra by Likely7, GPL-3.0). It is not an official PushFPS
release.

**Nothing in this table has run on a gaming PC, a real graphics card or a PS5.** No latency, frame
pacing, picture quality or reliability figure has been measured on hardware. Figures from Linux
models and simulators in the other documents are models, not measurements, and software timestamps
are not proof of input-to-photon latency: only a camera or a light sensor on the screen is.

## The five levels

| Level | Meaning |
|---|---|
| implemented | the code exists |
| compiled | built by MSVC (and dxc for shaders) on GitHub's Windows machine, inside the app |
| automatically tested | has tests that pass: portable tests on Linux (plain, ThreadSanitizer, AddressSanitizer + UBSan) and on Windows (MSVC), QML tests in the Windows build, or GPU tests on Windows' software renderer (WARP) |
| hardware-validated | checked on a real PC with a PS5. **Nothing is at this level yet** |
| incomplete | known gap, listed in the last column |

"Stable" is the stable candidate of the test package (`slipstream/rc2`); "experimental" is the
experimental build of the same package. Every switch is off unless you turn it on; the stable
candidate's installer writes `mode = ultra-low-latency` into `slipstream.ini`, nothing else.

## Features

| Feature | In | Switch (default off unless noted) | Implemented | Compiled | Automatically tested | Hardware-validated | Incomplete / notes |
|---|---|---|---|---|---|---|---|
| Event-driven hand-off between network, decode and engine (no polling waits) | stable | on; `VEYRA_TEST_RP_LEGACY_*` switch each part back | yes | yes | yes (portable + thread sanitizer) | no | the A/B against the original build is the first hardware test |
| Controller input sent on change, 2 ms poll | stable | `VEYRA_EXP_RP_PAD_POLL_MS` (set by Ultra Low Latency and Competitive) | yes | yes | yes (portable) | no | |
| Adaptive presentation phase | stable | `VEYRA_EXP_RP_ADAPTIVE_PHASE` | yes | yes | yes (portable) | no | |
| Settings file and modes (`slipstream.ini`) | stable | file beside the program | yes | yes | yes | no | in the stable candidate a mode sets only the two switches above |
| Updater guard (the official updater cannot replace a test copy) | stable | on in Slipstream builds | yes | yes | yes; installer path run on Windows | no | |
| Keyboard and mouse as a controller, profiles, raw input | stable | `VEYRA_EXP_RP_KBM` | yes | yes | yes (portable + input simulator) | no | the console sees an analog stick, never a mouse; feel is untested |
| Same-LAN transport profile (reorder wait, receive buffer, thread priority, 1 ms timer) | stable | `VEYRA_EXP_RP_LAN_PROFILE` and the single switches | yes | yes | yes; independently reviewed, findings fixed | no | thresholds are estimates until a real link is measured |
| Connection health log | stable | `VEYRA_EXP_RP_HEALTH` | yes | yes | yes | no | |
| Bounded decode backlog, keyframe retry | stable | `VEYRA_EXP_RP_BACKLOG`, `VEYRA_EXP_RP_IDR_RETRY_MS` | yes | yes | yes | no | |
| Fix for "no generated frames shown" (upstream pull request 2) and Veyra's feedback-history patch | stable | on | yes | yes | builds and starts | no | whether generated frames now show on a PS5 stream is a hardware check |
| Performance modes in the interface, wired to real engine settings; Automatic behind a switch | experimental | mode row; `VEYRA_EXP_AUTO_MODE` | yes | yes | yes (strict QML tests in the Windows build) | no | modes do not set the 2 ms poll or adaptive phase from the interface; Automatic cannot step back up without `VEYRA_EXP_RP_HEALTH=1` |
| Statistics overlay, first-launch welcome card | experimental | preferences `pushStatsOverlay`, `pushWelcomeDone` | yes | yes | yes (QML) | no | the card shows once for existing users too |
| Hardware detection script, test kit, report tooling | package | - | yes | n/a | yes: dry runs on Windows PowerShell 5.1, Python tests on Linux and Windows | no | never run against a real stream |
| Reliability checks, 2 h / 6 h soak, regression baselines, one-click redacted diagnostics | experimental kit | kit options | yes | n/a | yes: dry runs on Windows, Python tests | no | after a GPU device loss the app does not reconnect to the PS5 by itself |
| Adaptive reorder wait (2-16 ms) | experimental | `VEYRA_EXP_RP_REORDER_ADAPTIVE` | yes | yes | yes (portable + simulator) | no | thresholds provisional |
| Patient reconnection (8 attempts) | experimental | `VEYRA_EXP_RP_RECONNECT_PATIENT` | yes | yes | yes (portable) | no | on-screen text still reads "3/3" during the extra attempts; console tolerance unknown |
| Bottleneck classification in the health log | experimental | with `VEYRA_EXP_RP_HEALTH` | yes | yes | yes | no | thresholds provisional |
| Hardware capability model, GPU workload budget | experimental | model only; observe/apply switches come with the "wiring" work | yes | yes | yes | no | not driving the engine in this build |
| Latency governor (recommendation only) | experimental | not wired in this build | yes | yes (header only) | yes | no | by design it never raises latency for more displayed FPS |
| Mouse calibration (Ctrl+Alt+Shift+K), polling-rate measurement, calibrated profiles | experimental | inside `VEYRA_EXP_RP_KBM` | yes | yes | yes (portable + model player) | no | only a player at the PS5 can check it |
| Audio: fast recovery after stalls, small shared-mode buffer | separate branch `slipstream/dev-audio` | `VEYRA_EXP_RP_AUDIO_FAST_RECOVER`, `VEYRA_EXP_RP_AUDIO_LOW_BUFFER` | yes | first version: yes; version with the review fixes: not yet | controller: yes; Windows part: no | no | **not in the package.** An independent review found over-dropping and a drift-correction lapse; both fixed in code, not yet rebuilt; needs listening tests |
| GPU duplicate-frame detection for 30 FPS games in the 60 FPS stream | experimental | `VEYRA_EXP_RP_STREAM_DEDUP` | yes | yes (first version) | yes on synthetic and software-encoded sets; GPU test on WARP | no | **keep off**: an independent re-review showed it drops real pictures when something faint moves at 60 FPS over a 30 FPS scene; second fix round in progress |
| Predictive frame generation (extrapolation) | not in any build | - | CPU study only | no | no | no | parked until the conventional pipeline is measured on hardware |

## Build results

From GitHub Actions; the end of `START-HERE.txt` in the package names the exact runs it was made from.

| Build | Branch (public fork `kashkevin/PushFPS`) | GitHub Actions run | Result |
|---|---|---|---|
| original (PushFPS 3.2.1 as published) | `slipstream/baseline` | build-windows #1 | built, tests passed, installed and started |
| stable candidate | `slipstream/rc2` | build-windows #7 | built, tests passed, installed and started |
| experimental build | `slipstream/dev2` | build-windows #17 (run 44484522093) | built, tests passed, installed and started |
| audio experiment (not packaged) | `slipstream/dev-audio` | build-windows #16 | first version compiled; see the package run for anything later |

"Tests passed" means the project's own unit tests, the GPU tests on the software renderer, the
going-live tests, the QML tests and the install test of `build-windows.yml`, plus the portable tests
and the kit's dry runs of `remoteplay-checks.yml`. One earlier run of the experimental tree (#15)
failed in the software-renderer GPU test step and the identical tree passed when run again (#17): that
step has an intermittent failure whose cause is not known yet (the run's log could not be read).

## Hardware

One build for all PCs; settings follow what the PC reports, not its model name
(`HARDWARE_COMPATIBILITY.md`). Physically tested hardware: **none yet**. Compiled and run only on
GitHub's Windows machine, which has no graphics card (software renderer).

## Remaining work

**Required before hardware testing:** nothing. The package installs, starts, measures and rolls back
on GitHub's Windows machine; the first test needs only the PC and the PS5.

**Can be done alongside hardware testing:**
- second fix round and re-review of duplicate detection (stays off meanwhile);
- audio review fixes; wiring the governor and the GPU budget as observe-only logs;
- reconnection after a GPU device loss; the "3/3" reconnection text.

**Needs real measurements first:**
- which of the switches actually lowers latency on this PC and link, and by how much (the A/B kit);
- every threshold in the LAN profile, the adaptive reorder wait, the health grades, the GPU budget
  and the governor;
- whether duplicate detection ever helps a real PS5 stream;
- audio delay and drift against the PS5's clock; mouse feel and calibration;
- turning any switch on by default; letting Automatic, the governor or the GPU budget act by themselves.

**After validation:**
- predictive frame generation (prototype only, never in the production path before that);
- friend-ready distribution: code signing, an installer that needs no instructions, wider hardware
  (AMD, Intel) checked by people who own it.
