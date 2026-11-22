# Slipstream: completion checklist

Status on 20 November 2026 (fourth sprint; the checklist further down is the third sprint's, unchanged). "Slipstream" is the temporary internal name of this
low-latency PS5 Remote Play work on PushFPS (built on Veyra by Likely7, GPL-3.0). It is not an
official PushFPS release.

**What has run on real hardware:** one session of package 6 on the owner's PC on 19 November 2026
(RTX 5070, 2560x1440 at 180 Hz, PS5 over **Wi-Fi**, 12 runs, the app's own software counters). Nothing
else. **Nothing from sprint 4 below (including SmartLink) has run on a gaming PC, a graphics card or a
PS5.** It has been compiled by MSVC and has passed the automatic tests on GitHub's Windows machine, which
has no graphics card (software renderer), no PS5 and no screen to measure (see "Sprint 4"). Numbers
in the other documents that come from Linux models and simulators are models. Software timestamps are
not proof of input-to-photon latency: only a camera or a light sensor on the screen is.

## Package 11 (22 November 2026): capture card frame generation and GTA V-style mouse and keyboard

Everything of Package 10 (below) plus the rows here. **None of it has run on a gaming PC, a graphics card, a capture
card or a PS5.** Nothing new is on by default.

| Part | Default? | Tested where | Review | Physically validated |
|---|---|---|---|---|
| Capture card assessment docs/drafts/CAPTURE_FG.md (what 120 and 180 need, the inherent delay of interpolation, why DLSS FG cannot apply to captured video) | doc | code reading | accepted after two rounds | no: your capture session |
| `[slipstream-capture]` statistics (real against generated frames, repeated frames never counted as new), the card line, kit session `capture` (13 arms, `-Pass 120/180`), `VEYRA_TEST_RP_CAPTURE=1` (never connects to the PS5) | test-only | unit tests, kit dry run on Windows | accepted after two rounds | no |
| `VEYRA_EXP_RP_CAPTURE_DEDUP_SPAN=1` (duplicate removal keeps the cadence across 100 ms samples at 120 Hz) | off | model of the code in unit tests | accepted | no |
| GTA V-style keys (button "Apply GTA V-style keys"; on foot, aiming, vehicle, menu; every key can be changed); docs/drafts/KBM_AUDIT.md | a choice | unit tests (old profiles send byte-identical output, 8 x 200,000 events) | accepted after two rounds | no: your input session |
| Camera timing: `VEYRA_EXP_RP_KBM_INPUT=timing=report`, `stickSteps=8`, `VEYRA_EXP_RP_KBM_TIMING=report`, `VEYRA_EXP_RP_KBM_PRESET=gta-pc`; kit arms `i-gta-pc`, `i-report` | off | 123 keyboard and mouse unit tests, kit dry run | accepted after two rounds | no |
| `VEYRA_EXP_RP_KBM_CONTROL=1`: keyboard and mouse in the capture card's controller-only Remote Play session | off | unit tests | accepted | no: needs a capture card |

## Package 10 (21 November 2026): answers to the Package 9 evidence

Everything of Package 9 (below) plus the rows here. **None of it has run on a gaming PC, a graphics card or a PS5.**

| Part | Default? | Tested where | Review | Physically validated |
|---|---|---|---|---|
| Sound follows each picture's real arrival (docs/SYNC_FINDINGS.md); `VEYRA_TEST_RP_LEGACY_AUDIO_PICTURE=1` = Package 9 | yes: it can only make sound earlier | unit tests incl. simulated 20-minute runs at the Package 9 frame rates (target <= 6.5 ms; old rule 1500 ms); Linux audio model | accepted after a blocker (a 5 s bound that switched the fix off in long play) was fixed | no: your retest and lip-sync check |
| Stale-clock expiry rule as Veyra and PushFPS ship it; `VEYRA_TEST_RP_LEGACY_EXPIRY=1` = Package 9 | yes | unit tests | accepted | no |
| Frame cap fit: frame generation never makes frames the cap throws away (docs/FPS90.md); `VEYRA_TEST_SMOOTH_CAP_CEIL=1` = Package 9 | yes: it only lowers the multiplier | unit tests; ~70 million combinations by the reviewer | accepted after two rounds | no |
| Log lines: `[remoteplay-clock] wireFps=` (the PS5's real frame rate), `[remoteplay-ingress] expiryRule=` and causes, `[remoteplay-audio] pictureTerm=`, `[push-cap-fit]`, `scanoutKnownPct=` | yes, log fields | unit tests | accepted | no |
| Matched picture captures `VEYRA_TEST_RP_PROBE_FRAMES`, tools/clarity_frames/compare.py, kit session retest | test-only | unit tests, kit dry run | accepted after two rounds | no |
| `VEYRA_EXP_RP_CLOCK_RATE` (video clock follows the console's rate) | off | unit tests | accepted | no |

## Package 9 (21 November 2026): smoothness, clarity and network tools on top of 8C1

Everything of 8C1 (below) plus the rows here. **None of it has run on a gaming PC, a graphics card or a PS5.**

| Part | In 9 | Tested where | Review | Physically validated |
|---|---|---|---|---|
| Frame-generation analysis (docs/SMOOTHNESS_ANALYSIS.md): VFG works at 4K when the upscaler targets 4K; the output cap rounds the multiplier up per pair; one slow probe pauses generation for seconds | document, from code | read by the reviewer at the named lines | accepted | no: your smoothness session decides |
| Switches `VEYRA_EXP_RP_SMOOTH_SR_DISPLAY`, `_CAP_FIT`, `_STEADY`, `_TARGET` and the `[slipstream-smooth]` line | yes, all off | unit tests on Linux and GitHub's Windows machine | accepted after fixes | no |
| Smoothness Mode (quick setup, off by default): aims at 60/90/120/180; never above your multiplier, never below 2X, never turns generation off | yes, off until chosen | unit tests incl. the reviewer's adversarial cases; QML test | accepted after two review rounds | no |
| Clarity verdict (docs/CLARITY_VERDICT.md) | document | sources linked; code claims read by the reviewer | see the review line in START-HERE | no |
| Bitrate/codec test switch `VEYRA_TEST_RP_BITRATE_KBPS` / `_CODEC` (that run only; the saved profile never changes), clip switch `VEYRA_TEST_RP_CLIP`, kit arms c-* and cl-* | yes, test-only | unit tests, kit dry run on GitHub's Windows machine | see START-HERE | no |
| Network audit (docs/NETWORK_AUDIT.md), `[slipstream-net]`, the dominant-cause classifier, the Network Diagnostics panel, kit arms nd-* (one existing switch each) | yes, with `VEYRA_EXP_RP_STATS=1` | unit tests on Linux and GitHub's Windows machine; the impairment results are a Linux simulation, not a PS5 | accepted after two review rounds | no |

## Package 8C (21 November 2026): the complete Package 8

Package 8C is everything of 8B (below, which includes 8A) plus the optional neural layer. **None of it
has run on a gaming PC, a graphics card or a PS5.**

| Part | In 8C | Compiled on Windows (MSVC) | Tested where | Review | Physically validated |
|---|---|---|---|---|---|
| Neural layer (AI picture enhancement with NVIDIA's runtime): runtime choice, sizes, a conservative look, its place after the clean-up and before frame generation | yes, off until chosen | see the build line below | unit and QML tests on GitHub; no NVIDIA GPU there, so the layer itself never ran | accepted with fixes; three small items open (a folder-swap race in the installer, a rescan on the interface thread, no test for one review case) | no |
| Verified installation of NVIDIA's runtime: kept only with your yes and only when the file is an NVIDIA-signed original; never a modified file; no NVIDIA file in this zip or the repository | yes, in the experimental copy: its installer asks (`-NeuralRuntime Ask` by default); the original, stable and Package 7 copies keep the signed file without asking, as PushFPS 3.2.1 does | see the build line below | GitHub's install test runs `-NeuralRuntime No` and `Yes` with the signature and SHA-256 checks; Ask and the update path were only read | accepted with fixes | no |
| Kit: session neural and the neural arms of visual | yes | scripts dry-run on GitHub | dry run | accepted | no |

Build line: compiled on Windows (MSVC, warnings as errors) in build-windows 50, run 44868744683, with every
automatic test green; the first 8C tree did not compile (a merge error found by the integration review and
fixed before that build).

The neural layer is video processing of the PS5 stream. It is **not** NVIDIA's game-integrated DLSS 5.
No public NVIDIA licence or SDK covers a third party using that runtime file this way; whether to use it is
your decision. Nothing here claims a better picture: that needs your comparison clips on your screen.

## Package 8B (21 November 2026): what is added to 8A

Package 8B is the second of three Package 8 downloads: everything of 8A (below) plus the rows here. The
8A table's "Package 8B" row is this section. **None of it has run on a gaming PC, a graphics card or a PS5.**

| Part | In 8B | Compiled on Windows (MSVC) | Tested where | Review | Physically validated |
|---|---|---|---|---|---|
| Adaptive mouse and keyboard (raw input, mapping, sensitivity and dead-zone, capture and focus, controller switching; no smoothing or buffering added) and its page with three input profiles | yes, off until switched on | yes | unit tests (legacy path identical on 15 million recorded events), QML tests on GitHub | accepted with fixes, fixed | no |
| Frame-pacing instrumentation `[slipstream-cadence]`, the shown / submitted / uneven readout, VRR re-read at 5 s and 15 s, `VEYRA_EXP_PRESENT_EVEN_CADENCE` (only ever lowers the multiplier) | readout and re-read always on; the switch off | yes | unit tests on GitHub | accepted with fixes, fixed | no; **the uneven 120 FPS is not claimed fixed** |
| Veyra ports (controller queue expiry, neural shutdown, DLL loader fix; L2/R2 filter behind a switch) | on, except the filter | yes | unit tests on GitHub | accepted | no |
| chiaki-ng fixes 0005-0010 (0010 behind a switch) | on, except 0010's switch | yes | unit tests on GitHub | accepted | no |
| Kit sessions input, pacing, audiofix, framegen, smoothness | yes | scripts dry-run on GitHub's Windows machine | dry run | accepted | no |
| Optional neural layer | **no**: Package 8C | | | | |

Not done in 8B (from the profiles review): "Original" clears frame generation while "Clean 1440p" keeps it,
and the lowest-latency switch keeps its own restore record; the known issue below still applies.

## Package 8A (21 November 2026): what is in this download

Package 8A is the first of three Package 8 downloads. Everything new in it is in the experimental build
only and off until chosen. **None of it has run on a gaming PC, a graphics card or a PS5.**

| Part | In 8A | Compiled on Windows (MSVC) | Tested where | Review | Physically validated |
|---|---|---|---|---|---|
| Clean-up stage for the compressed 1080p stream (deblock, dering, deband; ports from libplacebo, LGPL-2.1+, see THIRD_PARTY_NOTICES.md) in front of the upscaler | yes, off until chosen | yes | GPU tests on GitHub's software renderer (SDR and HDR) | accepted with fixes, fixed | no |
| Picture profiles Original / Clean 1440p / Enhanced 1440p / Maximum Fidelity / Ultra Low Latency | yes, "none" by default | yes | QML tests on GitHub | accepted | no |
| Pipeline planner, picture metrics, `[slipstream-nr]` / `[slipstream-clean]` statistics | yes, statistics only with `VEYRA_EXP_RP_STATS=1` | yes | unit and GPU tests on GitHub | accepted | no |
| Test kit 5 (sessions visual and builds; others say "not in this build") | yes | its scripts ran on GitHub's Windows machine on made-up logs | dry run on GitHub | accepted after fixes | no |
| Adaptive mouse and keyboard, frame-pacing and audio fixes | **no**: Package 8B | | | | |
| Optional neural layer and its verified installation | **no**: Package 8C | | | | |

Known issue: after using "Ultra Low Latency" together with the lowest-latency switch, check Smooth pacing.
No claim is made here that the picture is better: that needs your comparison clips on your screen.

## Sprint 4 and SmartLink (20 November 2026): where each part stands

**Windows build: done on 20 November 2026.** GitHub started no build for this repository's pushes for
about twelve hours (from 04:36 UTC; working again at 16:47 UTC). The whole sprint then compiled at the
first attempt under MSVC `/W4 /permissive- /WX` and passed the project's unit tests, the GPU tests on
Windows' software renderer, the going-live tests, the strict QML tests and the install test
(build-windows #31). The separate header check found one warning in a test-only header (a byte array
filled with an integer zero); with that fixed the same code is build-windows #33 (run 44780265264),
**the experimental build of package 7**. Column "compiled on Windows" is therefore **yes** for every row
below. "Physically validated" is **no** for every row.

What was done for every row: implemented; compiled by MSVC; the portable tests pass on Linux (711 core
tests under plain, ThreadSanitizer and AddressSanitizer + UBSan builds) and under MSVC; 212 Python tests
of the kit, report and package tools; translations check 0 missing; an independent reviewer who did not
write the code reviewed it, and the findings were fixed and re-reviewed where the table says so. A row
is engineering-complete by the rule below only where its review column ends in "accepted": rows that
say "not re-reviewed", "rejected", "trial only" or "not accepted" are not. Windows-only code was written
without a Windows compiler; it has now been compiled and run by the automatic tests on GitHub's machine,
and nowhere else.

**The test kit ran on Windows for the first time in this sprint's form, and that found real defects.**
The check that should have run every new session passed its options in a way PowerShell reads as
positions, so until 20 November no sprint-4 session had actually run there. With that fixed:

- found by reading, before any run: the kit compared every run with the session's *first* run, and its
  test-profile check ignored what an arm changes on purpose. The first session
  (`Run-First-Test.cmd -Session presentation`) would have stopped at a "set this run aside and repeat
  it" question for `p-lowqueue-off` (low-latency queue off on purpose) and for `p-fullscreen` (window
  size and variable refresh differ from the windowed arm), and refused both in RESULT.txt. Now a run is
  compared with its arm's pair, what an arm is expected to change is declared in `arms.csv` (column
  `differs`), and the short sessions keep a run that differs and go on;
- found by the independent review of that fix (accept with fixes, all applied): the short sessions still
  asked for the scene at the start and could ask "one more round" at the end; `p-test-uncovered` did not
  declare that it may change how Windows presents the window;
- found by the first real run of the sessions: a network session on Wi-Fi said "wired" in RESULT.txt
  when the folder held no latency run.

After these fixes every session of the kit (first, full, versus, switches, quality, dedup-probe,
network, presentation, audio) passes its dry run on Windows PowerShell 5.1 with made-up logs
(remoteplay-checks #36). A dry run exercises the kit's own logic, not PushFPS, not a console and not a
real log: the first real session can still find what made-up logs cannot.

| Part | What it is | Independent review (last verdict) | Default | Open points |
|---|---|---|---|---|
| Frame hand-off (event-driven) | Kept exactly as in package 6's stable build | unchanged | on (stable build) | First hardware run: seconds under 57 presents 4.2 % vs 23.3 % with polling |
| Presentation delay (9–10 ms) | Investigation, `[slipstream-latency]` / `[slipstream-swapchain]` / window facts, experiments tearing, VRR request, low queue off, test window behind the picture | accept with fixes; fixes applied; the last small changes accepted in the final review | diagnostics off unless `VEYRA_EXP_RP_STATS=1`; experiments off | Cause of the constant extra refresh (about 5.5 ms) is a hypothesis: the video window is composed by Windows instead of flipped directly. Needs PresentMon on the PC |
| Frame delivery / micro-stutter | `[slipstream-delivery]`, model, experiment `VEYRA_EXP_RP_DELIVERY=catchup` | accept with fixes; fixes applied; bound tightened afterwards | off | Catch-up delays the newest frame by about one refresh when used: off below 144 Hz for a 60 fps stream. Model only |
| Latency test v2 and automation | Up to 500 presses, per-press split, autorun / autoconnect, `[slipstream-config]` | PS-button path and automation: accepted after two fix rounds. OPTIONS / D-pad detector: accepted only behind `VEYRA_EXP_RP_LATENCY_BUTTONS=1` | PS button, 8 presses as before; buttons experiment off | The button test cannot resolve a few milliseconds (per-press spread 26 ms: 500 presses resolve about 6 ms); the in-app split and PresentMon can |
| Super resolution cost | Finding by code reading, `[slipstream-sr]`, experiments level override and budget, autorun `sr` / `fg` / `quality` | accept with fixes; fixes applied; the last budget step-up change is tested but not re-reviewed | off | Finding: the super-resolution output defaults to 3840x2160 whatever the screen, so on 2560x1440 it works on 2.25x the pixels shown and is then shrunk. Unmeasured |
| Audio pops | Cause by code reading (output runs dry and stops on a loud sample, no fade), experiments SMOOTH, ADAPTIVE; counters | accepted after fixes | off | On the default path audio delay can build up over hours (clock drift; behaviour inherited from the upstream app). `FAST_RECOVER` corrects it; test it only together with SMOOTH and ADAPTIVE |
| Network evidence | Per-frame late-packet evidence, console round trip and target rate, expiry fix, reorder-queue wrap fix, early loss | accept with fixes; fixes applied; re-checked in the final SmartLink review | off | Units of the console's round trip are inferred and unconfirmed. Early loss: no gain in the model, not recommended |
| Reconnect after GPU reset | `VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1` | accepted after fixes | off | A loss that surfaces as an engine exception does not resume |
| Latency governor, active mode | `VEYRA_EXP_RP_GOVERNOR=apply` | **rejected six times** (sixth review: with periodic short link stalls it can put an overloaded level back for the rest of the stream; the reviewer's two-line fix is not applied yet) | off; **not accepted: do not use `=apply`** | Observe mode (`=1`) is accepted |
| Test kit | Comparable-settings check, Wi-Fi labels, PresentMon (owner supplies it), automation, presentation / audio / network sessions | rejected twice, third review: accept with fixes; the fix round after it (new short sessions, "cannot tell" wording) is tested but **not re-reviewed**; the last change (pair comparison, short sessions without questions): accept with fixes, applied, and run on Windows | n/a | Runs on GitHub's Windows machine on made-up logs only, never against a real stream. If the two runs of one arm end in a different state the arm shows as REFUSED although the session gives its values. The 40-minute network session can only show large effects |
| **SmartLink engine** | Signals, regime detection, profiles (wired / strong Wi-Fi / unstable Wi-Fi), decisions; simulator | observe: accepted. Apply: accepted **as a supervised opt-in trial only**, after fixes | **off** (interface choice "Off") | Model: no gain under congestion or interruptions; worse than the existing switches in some scenarios (listed in `docs/SMARTLINK.md`). Learned model: no measurable benefit, statistics stay the default |
| SmartLink network wiring | 200 ms windows, reorder wait 2/4/8/16 ms in apply mode, trace recorder, Wi-Fi signal (RSSI only) | accepted after fixes (final integrated review) | off | Wi-Fi link rate and band not read. Opt-in `VEYRA_EXP_RP_WIFI_STREAMING_MODE=1` changes adapter behaviour: **the owner decides** |
| SmartLink audio continuity | Audio reserve follows the decision in apply mode (capped), audio feed | accepted after fixes | off | Costs audio delay while trouble lasts (model: up to about +30 ms) |
| SmartLink interface | "Connection" row: Off / Advise / Automatic, plain-language advice | accepted in the final integrated review (one blocker found and fixed) | Off | Blind Windows code and QML tested with stubs only |
| Network-aware governor | Governor reads SmartLink's link state | part of the governor row: not accepted in apply mode | off | — |
| Frame concealment | Offline study only, on a research branch, not in the app | n/a (research) | not in the app | Recommendation: **no-go** for the app (wrong on HUD changes and scene cuts; synthetic footage only) |
| Dynamic bitrate | Research: the protocol has no live bitrate command; the console adapts by itself from the client's 200 ms loss report | n/a (research) | — | Safest alternative built: a suggested start rate for the next session (advice only) |

Not done in sprint 4: duplicate detection chroma read-back (deferred until a real capture shows exact
repeats); Wi-Fi link rate and band; three upstream chiaki-ng fixes not ported (reasons in
`docs/PS5_REMOTE_PLAY_EXPERIMENTS.md`); code signing.

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
| 1 | Core low-latency path: event-driven hand-off network -> decoder -> engine, controller input on change, 2 ms poll, adaptive presentation phase | stable | yes | yes | yes, incl. thread sanitizer; this sprint: thread stress tests and 22 simulated hours of soak, no defect found in this code | accepted as the first build to test on hardware; no blocker; three findings, two fixed in the experimental build and the fixes reviewed | **no** (one finding needs a console; the stable candidate predates the fixes) | no | review findings: (a) the 2 ms input poll sends about four times as many controller packets as the original (about 500 a second) and no console has seen that yet: if input lags or drops, `Rollback-Slipstream.cmd` halves it (4 ms, the build's own default) and `VEYRA_TEST_RP_LEGACY_INPUT=1` restores the original's 8 ms; the test kit measures these as separate arms; (b) gyro samples could be dropped while the decoder is busy; (c) up to 4 ms of extra wait after a rejected video sample. (b) and (c) are fixed in the experimental build (fixes reviewed: correct, no busy loop); the stable candidate still has them. The backported controller-history patch is always on, which the A/B against the original cannot separate |
| 2 | Adaptive LAN networking: LAN profile, reorder wait 2-16 ms, receive buffer, thread priority, 1 ms timer, health log, bounded backlog, keyframe retry | stable (profile, health, backlog); experimental (adaptive wait, study) | yes | yes | yes: 16-condition reorder study, 2 simulated hours of random impairments with no standing delay, queue model checked against the patched library on 40 traces | four rounds: rejected for dead reconnection code and for widening on a clean link, fixed each time; the last fix was not reviewed again | **no** | no | in the app the adaptive wait cannot widen today (the library does not report the per-frame late-packet evidence the rule needs): with the LAN profile it equals a fixed 2 ms wait; without the profile it starts at 16 ms and narrows to 2 ms. After the last fix the model returns to 2 ms in every clean-link run (60 runs of 600 s, 20 of 1800 s); that fix was not reviewed again. Do not use the adaptive switch without the LAN profile. For a wired LAN the model's answer is 2 ms. All thresholds provisional |
| 3 | Latency governor | experimental | yes | yes | yes: closed-loop simulator (11 scenarios + random traces), invariants checked on every trace, mutation-tested | accepted with switches off (observe only). Active mode: **not accepted**: three review rounds each found a way it acts for no gain (last: rebuilding the graph every few minutes when decode stalls come in bursts); the fix for that (no action during network bursts, tier kept on multiplier steps, every tier change at least 60 s apart, stand-down after two unhelpful steps) has not been reviewed | **no** | no | `VEYRA_EXP_RP_GOVERNOR=1` only logs and is safe to use. **Do not use `=apply` yet.** For latency it can only lower the frame-generation multiplier; for GPU overload it may also lower the quality tier (at most once a minute, because that rebuilds the graph); never above your setting. Costs in the simulator are invented; thresholds uncalibrated; present lateness and thread scheduling delay are not available to it |
| 4 | GPU workload management | experimental | yes | yes | yes (model, driver, simulated hardware profiles) | accepted (two rounds) | **no** | no | `VEYRA_EXP_RP_GPU_BUDGET=1` logs, `=apply` lowers tier, then multiplier, then generation. A tier step rebuilds the graph (a visible hitch), so it is limited to once a minute. Enhancement and super resolution are not reduced (their setters rebuild the graph). No memory-pressure input exists. Refuses capture-card sources |
| 5 | Modern hardware compatibility (NVIDIA RTX 30/40/50, AMD RX 6000/7000/9000, Intel Arc A/B) | experimental | yes | yes | yes: 26 simulated capability reports x 576 requests, fallback chains, init failures | accepted; four findings fixed | **yes, as far as it can be without hardware** | **no: no GPU of any vendor tested** | by reading the code nothing stops a target PC from starting or from Remote Play with automatic decode. Known gaps, not fixed: variable refresh state is read through NVIDIA's API only (AMD/Intel screens read as unknown; a diagnostic switch logs what Windows reports); the live recorder has no fallback when NVENC refuses; the installer picks its runtime package from the adapter name. See `HARDWARE_COMPATIBILITY.md` |
| 6 | First-launch calibration and recommended settings | experimental | yes | yes | yes: plan tests (monotonic: a slower measurement never gets more), strict QML tests | rejected twice (non-monotonic plan, concurrent runs; then: the app refused the new preferences), fixed each time; last fix not reviewed again | **no** | no | needs something playing (no built-in test source); the 16 s measurement path has never run on Windows; an unmeasured PC is always recommended frame generation off. Nothing is applied without pressing Apply; Undo restores |
| 7 | Performance modes (Ultra Low Latency, Competitive, Balanced, Maximum Smoothness; Automatic behind `VEYRA_EXP_AUTO_MODE`) | experimental (interface); stable (settings file) | yes | yes | yes (portable + strict QML) | reviewed with #6 | **no** | no | a mode picked in the interface now sets the same input and phase switches as the settings file, from the next connection. Automatic needs hardware evidence before it may act unattended |
| 8 | GPU duplicate-frame detection (30 FPS game in the 60 FPS stream) | experimental | yes | yes | yes: 6 stream sets, hold-outs scored once | **rejected three times** | **no: disabled** | no | every version dropped genuine pictures in some scene (faint motion; then colour-only changes). It now refuses to drop anything unless luma and chroma are both measured identical, and the GPU pass does not read chroma yet: **with the switch on it removes nothing**, and still waits up to 4 ms per frame for its measurement. Leave `VEYRA_EXP_RP_STREAM_DEDUP` unset. Remaining: chroma readback, then a real PS5 capture must show that repeats decode exactly at all |
| 9 | Keyboard and mouse as a controller, profiles, calibration (Ctrl+Alt+Shift+K) | stable (profiles); experimental (calibration) | yes | yes | yes: profile, calibration with a model player, stuck-input regression matrix (10,000 random sequences) | accepted for a first hardware try; no blocker; findings fixed in the experimental build and the fixes reviewed | **yes** in the experimental build | no | the console sees an analog stick, never a mouse. Fixed after review, experimental build only: built-in smoothing 8 ms instead of 33 (the stable candidate still smooths over 33 ms, about 14 ms of added delay on average: set `VEYRA_EXP_RP_KBM_SMOOTH_MS=8` in its `slipstream.ini`); after calibration the camera stops within about 50 ms of the hand (was up to 280 ms); the missing Windows clean-up call for raw input; calibration stops after 60 s without a tap. No sequence that leaves a key stuck was found. The calibration speaks only through the log. Feel, the tap procedure and real games' response can only be judged at the PS5 |
| 10 | Audio latency and synchronisation | experimental | yes | yes | yes: end-to-end model, 2 h and 6 h drift runs, device events, 100 random mixes | accepted (two rounds); findings fixed | **no** | no | never heard. Found by reading the original code and confirmed by a reviewer: today's default rule (also upstream's) lets console-clock drift build up unseen and leaves audio about 90-98 ms late after a network stall of 100 ms or more until the next discontinuity; `VEYRA_EXP_RP_AUDIO_FAST_RECOVER` is the experiment against that. Listening list: `AUDIO.md` |
| 11 | Connection recovery | experimental | yes | yes | yes: 1 s / 5 s / 12 s / 60 s outages, console still holding the old session | reviewed with #2 (dead code found and fixed, fix confirmed) | **no** | no | `VEYRA_EXP_RP_RECONNECT_PATIENT`; how a PS5 answers repeated session requests is unknown. The on-screen text still reads "3/3" during the extra attempts (needs new translated strings) |
| 12 | Crash and GPU device recovery | partly | partly | yes (what exists) | restore-after-crash paths tested for governor, GPU budget, calibration, optimiser | reviewed with #3, #6 | **no** | no | **not implemented:** after a GPU device loss the app does not reconnect to the PS5 by itself; the design is written (`PS5_REMOTE_PLAY_EXPERIMENTS.md`) and needs engine changes. An upstream hand-off race (a frame arriving between a clock read and a lock is called expired) exists in the original too and is fixed only with `VEYRA_EXP_RP_BACKLOG` |
| 13 | Statistics overlay | experimental | yes | yes | strict QML tests | not reviewed separately | **no** | no | new governor keys are in the live statistics but not shown |
| 14 | Automatic settings rollback | experimental + package | yes | yes | yes | reviewed with #3, #6, #15 | **no** | no | governor / GPU budget restore your setting on stop, quit and after a crash; calibration Undo; mode restore; `Rollback-Slipstream.cmd` |
| 15 | Installation and configuration (three copies side by side, updater guard, settings file) | package | yes | n/a (scripts run on Windows in CI) | yes: install, start, rollback, uninstall, refusals run on GitHub's Windows machine | security audit: no blocker; kit review: rejected once, fixed, accepted | **yes** | no (never on a real PC) | nothing is code signed (Windows will warn) |
| 16 | Diagnostic collection | package | yes | n/a | yes: dry run on Windows; redaction rules tested equal in three places | security audit found four redaction gaps: fixed | **yes** | no | read the zip before sending it |
| 17 | Hardware test kit and regression tooling (first test, versus, full session, soak, reliability, baselines) | package | yes | n/a | yes: every mode dry on Windows PowerShell 5.1; Python tests on Linux and Windows | rejected once (a start option failed on default installs), fixed, accepted on re-review | **yes** | no (never against a real stream) | PresentMon is used only when Intel-signed; app log lacks a few fields the report would like (listed in RESULT.txt) |
| 18 | Security and distribution | all | yes | n/a | n/a | independent audit: no blocker; nothing private found in the public fork | **yes, for private testing** | n/a | fixed after the audit: redaction gaps, first name in test data, junction check in the installer, the kit no longer runs the app elevated, network paths refused for mouse profiles, modification notice, source of every packaged build kept on the fork. Open: mixed GPL-3.0-only / or-later headers in new files; public commits carry the tooling's session link |
| 19 | Predictive frame generation | none (research branch only) | offline CPU prototype on synthetic video | no | no | no | not part of the application, by agreement | no | the prototype (11 synthetic sequences through x264) and the GPU benchmark to run later are on branch `research/predictive-fg`; nothing of it is in any build. It waits for hardware measurements of the conventional pipeline |

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
| experimental build (package 7: sprint 4 and SmartLink) | `slipstream/dev2`, kept as `archive/6fe95fa4` | build-windows #33 (run 44780265264) | built, tests passed, installed and started |
| previous experimental build (package 6, the rollback checkpoint) | kept as `archive/7410ff90` | build-windows #29 (run 44555596876) | built, tests passed, installed and started |
| experimental build before that (package 5) | kept as `archive/db631348` | build-windows #26 (run 44546215516) | built, tests passed, installed and started |

The stable candidate is unchanged on purpose: the first hardware test compares it with the original,
and that comparison should rest on the smallest change that has been reviewed. Everything from this
sprint is in the experimental build only.

"Tests passed" means the project's own unit tests, the GPU tests on the software renderer, the
going-live tests, the strict QML tests and the install test of `build-windows.yml`, plus the portable
tests under MSVC and every dry run of the kit under Windows PowerShell 5.1 (`remoteplay-checks.yml`).
On Linux the same portable tests (524) pass in plain, ThreadSanitizer and AddressSanitizer + UBSan
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
