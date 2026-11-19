# Slipstream: completion checklist

Status on 19 November 2026 (third sprint). "Slipstream" is the temporary internal name of this
low-latency PS5 Remote Play work on PushFPS (built on Veyra by Likely7, GPL-3.0). It is not an
official PushFPS release.

**Nothing here has run on a gaming PC, a real graphics card or a PS5.** No latency, pacing, picture
quality, audio or reliability figure has been measured on hardware. Numbers in the other documents
that come from Linux models and simulators are models. Software timestamps are not proof of
input-to-photon latency: only a camera or a light sensor on the screen is.

## What "complete" means here

A subsystem is **engineering-complete** only when all of these hold:

1. implemented;
2. compiled by MSVC inside the app on GitHub's Windows machine;
3. its automatic tests pass (portable tests on Linux under plain, ThreadSanitizer and
   AddressSanitizer + UBSan builds and on Windows under MSVC; QML tests in the Windows build; GPU
   tests on Windows' software renderer);
4. an independent reviewer (an agent that did not write it) accepted it, and confirmed findings
   were fixed;
5. no known critical defect;
6. failure and fallback behaviour is tested;
7. documented and wired in.

**Physically validated** is a separate requirement and is **"no" for every row**. Nothing becomes a
default before real-hardware evidence supports it. An experiment that cannot meet its criteria
stays off.

"Stable" is the stable candidate of the test package, "experimental" the experimental build. Every
switch is off unless you turn it on; the stable candidate's installer writes
`mode = ultra-low-latency` into `slipstream.ini`, nothing else.

## Checklist

| # | Subsystem | In | Implemented | Compiled (Windows) | Automatic tests | Independent review | Engineering-complete | Physically validated | Known defects / remaining work |
|---|---|---|---|---|---|---|---|---|---|
| 1 | Core low-latency path: event-driven hand-off network -> decoder -> engine, controller input on change, 2 ms poll, adaptive presentation phase | stable | yes | yes | yes, incl. thread sanitizer; this sprint: thread stress tests and 22 simulated hours of soak, no defect found in this code | accepted as the first build to test on hardware; no blocker; three findings open (below) | **no** (review findings open) | no | open review findings: (a) the 2 ms input poll sends about four times as many controller packets as the original (about 500 a second) and no console has seen that yet: if input lags or drops, `Rollback-Slipstream.cmd` halves it (4 ms, the build's own default) and `VEYRA_TEST_RP_LEGACY_INPUT=1` restores the original's 8 ms; the test kit measures these as separate arms; (b) gyro samples can be dropped while the decoder is busy; (c) up to 4 ms of extra wait after a rejected video sample. (b) and (c) are being fixed for a later experimental build. The backported controller-history patch is always on, which the A/B against the original cannot separate |
| 2 | Adaptive LAN networking: LAN profile, reorder wait 2-16 ms, receive buffer, thread priority, 1 ms timer, health log, bounded backlog, keyframe retry | stable (profile, health, backlog); experimental (adaptive wait, study) | yes | yes | yes: 16-condition reorder study, 2 simulated hours of random impairments with no standing delay, queue model checked against the patched library on 40 traces | three rounds: rejected twice (dead reconnection code, widening on a clean link), fixed each time; the last fix was not reviewed again | **no** | no | in the app the adaptive wait cannot widen today (the library does not report the per-frame late-packet evidence the rule needs): with the LAN profile it equals a fixed 2 ms wait; without the profile it starts at 16 ms and narrows to 2 ms. In the model 2 of 60 clean-link runs still ended stuck at 6 ms. For a wired LAN the model's answer is 2 ms. All thresholds provisional |
| 3 | Latency governor | experimental | yes | yes | yes: closed-loop simulator (11 scenarios + random traces), invariants checked on every trace, mutation-tested | accepted with switches off (observe only). Active mode: **not accepted**: two blockers fixed, policy changed, and the last re-review still found it rebuilding the graph every few minutes for no gain when decode stalls come in bursts | **no** | no | `VEYRA_EXP_RP_GOVERNOR=1` only logs and is safe to use. **Do not use `=apply` yet.** For latency it can only lower the frame-generation multiplier; for GPU overload it may also lower the quality tier (at most once a minute, because that rebuilds the graph); never above your setting. Costs in the simulator are invented; thresholds uncalibrated; present lateness and thread scheduling delay are not available to it |
| 4 | GPU workload management | experimental | yes | yes | yes (model, driver, simulated hardware profiles) | accepted (two rounds) | **no** | no | `VEYRA_EXP_RP_GPU_BUDGET=1` logs, `=apply` lowers tier, then multiplier, then generation. A tier step rebuilds the graph (a visible hitch), so it is limited to once a minute. Enhancement and super resolution are not reduced (their setters rebuild the graph). No memory-pressure input exists. Refuses capture-card sources |
| 5 | Modern hardware compatibility (NVIDIA RTX 30/40/50, AMD RX 6000/7000/9000, Intel Arc A/B) | experimental | yes | yes | yes: 26 simulated capability reports x 576 requests, fallback chains, init failures | accepted; four findings fixed | **yes, as far as it can be without hardware** | **no: no GPU of any vendor tested** | by reading the code nothing stops a target PC from starting or from Remote Play with automatic decode. Known gaps, not fixed: variable refresh state is read through NVIDIA's API only (AMD/Intel screens read as unknown; a diagnostic switch logs what Windows reports); the live recorder has no fallback when NVENC refuses; the installer picks its runtime package from the adapter name. See `HARDWARE_COMPATIBILITY.md` |
| 6 | First-launch calibration and recommended settings | experimental | yes | yes | yes: plan tests (monotonic: a slower measurement never gets more), strict QML tests | rejected twice (non-monotonic plan, concurrent runs; then: the app refused the new preferences), fixed each time; last fix not reviewed again | **no** | no | needs something playing (no built-in test source); the 16 s measurement path has never run on Windows; an unmeasured PC is always recommended frame generation off. Nothing is applied without pressing Apply; Undo restores |
| 7 | Performance modes (Ultra Low Latency, Competitive, Balanced, Maximum Smoothness; Automatic behind `VEYRA_EXP_AUTO_MODE`) | experimental (interface); stable (settings file) | yes | yes | yes (portable + strict QML) | reviewed with #6 | **no** | no | a mode picked in the interface now sets the same input and phase switches as the settings file, from the next connection. Automatic needs hardware evidence before it may act unattended |
| 8 | GPU duplicate-frame detection (30 FPS game in the 60 FPS stream) | experimental | yes | yes | yes: 6 stream sets, hold-outs scored once | **rejected three times** | **no: disabled** | no | every version dropped genuine pictures in some scene (faint motion; then colour-only changes). It now refuses to drop anything unless luma and chroma are both measured identical, and the GPU pass does not read chroma yet: **with the switch on it removes nothing**, and still waits up to 4 ms per frame for its measurement. Leave `VEYRA_EXP_RP_STREAM_DEDUP` unset. Remaining: chroma readback, then a real PS5 capture must show that repeats decode exactly at all |
| 9 | Keyboard and mouse as a controller, profiles, calibration (Ctrl+Alt+Shift+K) | stable (profiles); experimental (calibration) | yes | yes | yes: profile, calibration with a model player, stuck-input regression matrix (10,000 random sequences) | accepted for a first hardware try; no blocker; findings open (right) | **no** (review findings open) | no | the console sees an analog stick, never a mouse. Open review findings: the built-in profile smooths over 33 ms (about 14 ms of added delay on average; set `VEYRA_EXP_RP_KBM_SMOOTH_MS=8` in `slipstream.ini` to try without it); after calibration the camera can keep turning for up to about 280 ms after the hand stops; a Windows clean-up call is missing for raw input. No sequence that leaves a key stuck was found. Feel, the tap procedure and real games' response can only be judged at the PS5 |
| 10 | Audio latency and synchronisation | experimental | yes | yes | yes: end-to-end model, 2 h and 6 h drift runs, device events, 100 random mixes | accepted (two rounds); findings fixed | **no** | no | never heard. Found by reading the original code and confirmed by a reviewer: today's default rule (also upstream's) lets console-clock drift build up unseen and leaves audio about 90-98 ms late after a network stall of 100 ms or more until the next discontinuity; `VEYRA_EXP_RP_AUDIO_FAST_RECOVER` is the experiment against that. Listening list: `AUDIO.md` |
| 11 | Connection recovery | experimental | yes | yes | yes: 1 s / 5 s / 12 s / 60 s outages, console still holding the old session | reviewed with #2 (dead code found and fixed, fix confirmed) | **no** | no | `VEYRA_EXP_RP_RECONNECT_PATIENT`; how a PS5 answers repeated session requests is unknown. The on-screen text still reads "3/3" during the extra attempts (needs new translated strings) |
| 12 | Crash and GPU device recovery | partly | partly | yes (what exists) | restore-after-crash paths tested for governor, GPU budget, calibration, optimiser | reviewed with #3, #6 | **no** | no | **not implemented:** after a GPU device loss the app does not reconnect to the PS5 by itself; the design is written (`PS5_REMOTE_PLAY_EXPERIMENTS.md`) and needs engine changes. An upstream hand-off race (a frame arriving between a clock read and a lock is called expired) exists in the original too and is fixed only with `VEYRA_EXP_RP_BACKLOG` |
| 13 | Statistics overlay | experimental | yes | yes | strict QML tests | not reviewed separately | **no** | no | new governor keys are in the live statistics but not shown |
| 14 | Automatic settings rollback | experimental + package | yes | yes | yes | reviewed with #3, #6, #15 | **no** | no | governor / GPU budget restore your setting on stop, quit and after a crash; calibration Undo; mode restore; `Rollback-Slipstream.cmd` |
| 15 | Installation and configuration (three copies side by side, updater guard, settings file) | package | yes | n/a (scripts run on Windows in CI) | yes: install, start, rollback, uninstall, refusals run on GitHub's Windows machine | security audit: no blocker; kit review: rejected once, fixed, accepted | **yes** | no (never on a real PC) | nothing is code signed (Windows will warn) |
| 16 | Diagnostic collection | package | yes | n/a | yes: dry run on Windows; redaction rules tested equal in three places | security audit found four redaction gaps: fixed | **yes** | no | read the zip before sending it |
| 17 | Hardware test kit and regression tooling (first test, versus, full session, soak, reliability, baselines) | package | yes | n/a | yes: every mode dry on Windows PowerShell 5.1; Python tests on Linux and Windows | rejected once (a start option failed on default installs), fixed, accepted on re-review | **yes** | no (never against a real stream) | PresentMon is used only when Intel-signed; app log lacks a few fields the report would like (listed in RESULT.txt) |
| 18 | Security and distribution | all | yes | n/a | n/a | independent audit: no blocker; nothing private found in the public fork | **yes, for private testing** | n/a | fixed after the audit: redaction gaps, first name in test data, junction check in the installer, the kit no longer runs the app elevated, network paths refused for mouse profiles, modification notice, source of every packaged build kept on the fork. Open: mixed GPL-3.0-only / or-later headers in new files; public commits carry the tooling's session link |
| 19 | Predictive frame generation | none | CPU study only | no | no | no | not started, by agreement | no | waits for hardware measurements of the conventional pipeline |

Also checked this sprint: the intermittent failure of the software-renderer GPU test step (one run
failed, the same tree passed). The most likely cause was found by reading the test: a comparison
against a counter read twice, and a 50 ms time limit that a loaded software renderer can miss.
Both were removed from the test, failures now print one self-explaining line, and the suspect test
ran 25 times in a row on Windows without a failure. The original failure's log could not be read,
so the cause is likely, not proven.

## Builds

| Build | Public branch (fork `kashkevin/PushFPS`) | GitHub Actions run | Result |
|---|---|---|---|
| original (PushFPS 3.2.1 as published) | `slipstream/baseline` | build-windows #1 | built, tests passed, installed and started |
| stable candidate (unchanged since package 2) | `slipstream/rc2` | build-windows #7 | built, tests passed, installed and started |
| experimental build | `slipstream/dev2` | build-windows #26 (run 44546215516) | built, tests passed, installed and started |

The stable candidate is unchanged on purpose: the first hardware test compares it with the original,
and that comparison should rest on the smallest change that has been reviewed. Everything from this
sprint is in the experimental build only.

"Tests passed" means the project's own unit tests, the GPU tests on the software renderer, the
going-live tests, the strict QML tests and the install test of `build-windows.yml`, plus the portable
tests under MSVC and every dry run of the kit under Windows PowerShell 5.1 (`remoteplay-checks.yml`).
On Linux the same portable tests (515) pass in plain, ThreadSanitizer and AddressSanitizer + UBSan
builds. The source of every packaged build stays on the fork as a branch named `archive/<commit>`.

## Remaining physical tests (need your PC and PS5)

In this order; each is one double-click and one zip to send back unless noted.

1. `Run-First-Test.cmd`: original against stable, frame generation off (about 40 minutes).
2. `Run-First-Test.cmd -Session versus`: stable against experimental, one switch per arm.
3. `Run-Full-Test.cmd`: the same plus a 10-minute stability run and the reliability checks
   (cable pull, device switch, sleep/wake, GPU reset).
4. A camera or light-sensor measurement of input-to-photon latency for the best two arms
   (`HARDWARE_TEST_PROCEDURE.md`): the only proof of real latency.
5. Frame generation: does 30 -> 60 and 60 -> 120 show generated frames on a PS5 stream at all, and
   paced evenly on the 180 Hz screen.
6. Keyboard and mouse: bindings, feel, the calibration procedure in GTA or another game.
7. Audio: the listening list in `AUDIO.md` (USB headset, Bluetooth headset, monitor audio).
8. Soak: 2 hours, later 6 hours.
9. Anyone with an AMD or Intel card: the table in `HARDWARE_COMPATIBILITY.md`.

## Estimated remaining engineering

Rough, and before any hardware result: chroma readback for duplicate detection (about a day, and
only worth it if a PS5 capture shows exact repeats); reconnection after GPU device loss (1-2 days);
per-frame late-packet evidence in the library patch so the adaptive wait can widen (about a day);
variable-refresh detection for AMD and Intel (about a day plus a tester); calibrating every
threshold from real measurements (several sessions with the kit). Everything else depends on what
the first measurements show.
