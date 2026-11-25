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

## Package 16 (25 November 2026): the predicted schedule made to run with the owner's settings; evidence by itself

**First hardware data of the capture-card path** (the owner connected the experimental copy's log folder, read-only):
Package 15's "Lower delay" never ran on his PC (`lowDelay state=not-here refusal=smoothness-mode predictedPairs=0`): it
was excluded with Smoothness Mode (his 90 FPS target) and Live HUD Lock, both of which he uses. MEASURED there (software
timers): readout 31 ms = picture 4.7 + first generated frame 2.0 (NVIDIA VFG at 3840x2160 HDR10) + schedule wait 22.1 +
margin 2.2; Present to the screen refresh a median of about 10 ms more (windowed, VRR not active for the app); all frames
shown, about 95 % of Present intervals within 1 ms of the 11.1 ms grid; a pair of capture stalls after every quarter
hour of the wall clock (cause outside the program, not identified). **Nothing new in Package 16 has run on a gaming PC or
a real graphics card. Nothing is called fixed. No delay figure is promised.** TESTED = unit / QML tests and the Windows
build machine's software renderer (WARP): logic and safety guards, never timing or stability.

| Part | Default | Evidence | Undo | Hardware-verified |
|---|---|---|---|---|
| "Lower delay" runs with Smoothness Mode (pause below 3X, resume by itself) and with Live HUD Lock / exclusion boxes (locked pixels of the predicted frame from the real frame) | OFF; the preference is reset once at the first start of this version | end-to-end engine test with a test-only stand-in for VFG and the owner's configuration: the mode enters, order I, B, P, the real frame one slot earlier per predicted pair, HUD mismatch 0; WARP `predict-hud-lock*` | the row; `VEYRA_TEST_RP_LEGACY_LOWDELAY_SCHEDULE`, `VEYRA_TEST_RP_LEGACY_LOWDELAY_MIGRATE` | no |
| Predicted frame's motion from AMD FidelityFX optical flow behind a start-up self-check on the user's card (six known motions through the real predicted path, scored against the exact picture; a 3 s budget; a fault is remembered); Package 15's strict (FP64) GPU DIS switched off: it removed the device on WARP at its first FP64 shader | with the row | self-check mechanics TESTED on WARP; FidelityFX's motion CANNOT be tested there (its search needs waves of 32 lanes; WARP has 4): first real run is the user's card | - | no |
| VFG refusal ladder for the frame-index-2-only request (timestep mode, then index 1 + 2 with 1 discarded); a refusal is built never to switch frame generation off | with the row | unit tests, the stand-in refusing | - | no (VFG itself needs a real card) |
| Entry / exit rule that survives isolated skips and capture stalls; exits for late, too-slow and unusable predictions; stop for the playback after three; the real frame does not wait for its own pair's predicted frame | with the row | unit tests, simulator, the end-to-end test | `VEYRA_TEST_RP_LEGACY_LOWDELAY_SCHEDULE` | no |
| The row's line from live counters ("Running: N predicted frames shown a second", "Waiting", "Not running: reason"); only the first switch-on rebuilds | with the row | QML tests | - | no |
| `[fg-health]` / `[present-path]` additions: "to screen" (Present to vblank), late Presents and their cause, completion-poll duration, picture-part GPU stages, submit to GPU start; quick-setup and tooltip "to screen" lines; `[vrr-profile] start-state` | on (log / display only) | tested; full Windows build | `VEYRA_TEST_RP_LEGACY_PACKAGE15` (interface lines) | no |
| Stall witness: `[capture-stall]`, `[capture-stall-cause]` (numbers), `[stall-witness]` (program names at a stall: private; not in the kit's zip unless `-IncludeProcessNames`); once-a-second lowest-priority sampler, no handle to other processes; records only | on while a capture card runs | unit tests; first MSVC compile; never run with a capture card | `VEYRA_TEST_RP_LEGACY_STALL_WITNESS` | no |
| Opt-in only: `VEYRA_EXP_RP_PRESENT_BEFORE_POLL`, `VEYRA_EXP_RP_DEADLINE_HOLD`, `VEYRA_EXP_RP_STALL_CLOCK`, `VEYRA_EXP_RP_FULLSCREEN_NO_REGION`, `VEYRA_EXP_RP_STALL_WITNESS_DEEP`, `VEYRA_EXP_RP_PRESENT_BLIT_AHEAD` | off | model tests | - | no |
| The interface lines, the stall witness, the one-time reset and the row's schedule back to Package 15 (the always-on log figures and timestamps stay) | - | tested | `VEYRA_TEST_RP_LEGACY_PACKAGE15` (implied by `_PACKAGE14`, `_PACKAGE13`) | - |

The end-to-end engine test (`docs/drafts/E2E_ENGINE_TEST.md`) first reproduced the owner's Package 15 log on Package 15's
code and found that its first predicted pair removes the D3D12 device on WARP. Its gate on this tree: structure and
safety strict in every run; timing with a stated tolerance, because the software renderer's timing varies between runs.
In work for the following package: the project's own frame generator (motion once per pair for both generated frames,
selectable next to NVIDIA's for a direct comparison), and a fully predicted schedule as a labelled experiment.

## Package 15 (24 November 2026): a lower-delay schedule (opt-in) and the picture's path to the screen (capture card)

The owner on Package 14: 31 ms at 90 FPS (30 real + 60 generated), 4K; he asked for a much lower and stable delay
without picture loss, close to 15 ms if possible, and for the 3X pipeline, the alternatives and the display path to be
researched. **None of this has run on a gaming PC, a graphics card or a capture card. Nothing is called fixed.**
SIMULATED = the scheduling simulator (the program's own decision code, made-up 4K-like costs, 16 cost sets x seeds
1, 2, 3, 7, 11, 99). TESTED = unit / QML tests and CI's Windows build on a software renderer (WARP).

| Part | Default | Evidence | Undo | Hardware-verified |
|---|---|---|---|---|
| "Lower delay" row (`pushLowDelay`): per pair I(2/3) from NVIDIA VFG, the real frame one slot earlier, then a frame predicted from the A->B motion (own pass: GPU DIS flow at quarter size, warp by the true motion, untrusted pixels keep the real picture, whole frame = real picture on a cut or when more than half of the moving pixels fall back). Stay-in policy, sustained exit (45 of 60 unusable), late-frame / on-grid watch, pauses during the app's tests, exclusive with HUD Lock / exclusion boxes / Smoothness Mode, VFG refusal of the index-2-only request handled without stopping frame generation | OFF (one click, applies in the running session) | SIMULATED: mean delay 37.6 -> 28.3 ms over the sets, on-grid 91.1 -> 99.1 % with usable predictions; 99 % on-grid missed in 5 / 6 / 7 of 16 sets at a predicted-frame cost of 3 / 6 / 10 ms (Package 14: 8); later than Package 14 on some pairs near or over the GPU's limit. TESTED: schedule, policies, counters, QML row, WARP `predict*` (see START-HERE's "what has been checked" for the build that ran them). Two independent reviews. Picture: unknown | the row; `VEYRA_TEST_RP_LOWDELAY_LEAVE_ON_UNUSABLE` restores the first policy | no (the VFG request itself has never run on a GPU) |
| Row off: every frame takes Package 14's path | - | review of every changed line; the simulator (this package's code) with the row off is unchanged by the mode's parameters, present by present | - | no |
| Delay split: picture + first generated frame + wait (calculated from the schedule) + margin, in the badge tooltip, `pushLiveStats` and `[capture-timing]` | on (display / log only) | tested | - | no |
| In-picture readouts: delay badge, statistics overlay and PS5 network panel drawn inside the swap chain, so the video window's region stays one rectangle (earlier logs: a complex region went with 7-9 ms present-to-vblank and VRR inactive; a correlation) | on | WARP `video-overlay*` (pixels outside the items bit-identical; SDR, scRGB, HDR10), QML and Qt composition tests, two reviews; benefit unmeasured and not visible in the readout | `VEYRA_TEST_RP_LEGACY_VIDEO_OVERLAY` | no |
| NVIDIA low latency's per-frame Sleep measured against the sample's arrival; ended for the source session only on sustained overruns (3 s warm-up, 4 of 60 over 0.5 ms in two consecutive blocks) | on, capture card with frame generation | tested (transients do not trip it), two reviews | `VEYRA_TEST_RP_LEGACY_REFLEX_SLEEP` | no |
| `[present-path]` (how the picture reaches the screen; at most one line per 5 s) and `[fg-health]` (every 10 s: delay mean / P50 / P95 / P99, Present spacing, drops by cause, duplicates, GPU stages, Sleep overrun), `triage.py` | always on (log only) | tested | - | no |
| Blit ahead, Present at the deadline | OFF (`VEYRA_EXP_RP_PRESENT_BLIT_AHEAD=1`, environment) | model tests, WARP `blit-ahead-order`; not provably never-later for the next pair | - | no |
| Parts 4 and 5 at once back to Package 14 | - | tested | `VEYRA_TEST_RP_LEGACY_PACKAGE14` (also implied by `_PACKAGE13`) | - |

Research behind it (three reports, synthesis in `docs/drafts/LATENCY_RESEARCH_RANKING.md`): every interpolating frame
generator holds the newest real frame, only prediction removes the wait; Streamline DLSS-G, Reflex 2 Frame Warp, FSR 3 /
XeSS frame generation and NVIDIA's FRUC library are not usable for captured video; about 15 ms on the 90 FPS grid needs a
first generated frame of about 1-3 ms (motion once per pair at reduced size feeding full-size warps for both extra
frames, replacing VFG's run; estimate 14-20 ms, picture quality unproven): the next experimental branch. Known limits of
the predicted frame (`docs/drafts/LOW_DELAY_MODE.md`): no leading-edge search, bilinear sampling, no seam-aware verdict.

## Package 14 (24 November 2026): the Package 13 lag, and frame generation that does not drop out (capture card)

The owner reported Package 13 as worse on his PC (delay readout about 32 ms instead of about 30 ms, "lag mid frames").
His PC could not be read; this package is built from code reading, a scheduling simulator, tests, reviews and CI. **None
of it has run on a gaming PC, a graphics card or a capture card. Nothing is called fixed.** Scope: capture card, 4K,
30 FPS game shown as 90 FPS. SIMULATED = `tests/remoteplay/FgScheduleSim.h` (the program's own decision code with
made-up 4K-like costs).

| Part | Default | Evidence | Undo (`= 1` in slipstream.ini) | Hardware-verified |
|---|---|---|---|---|
| Every part of `VEYRA_EXP_RP_FG_ULL` and `VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY` inert; slipstream.ini cannot set frame-path switches; `[experiment-switches]` start-up line | on | SIMULATED (the parts dropped generated frames or delayed real frames under load), tested | none (they stay inert) | no |
| robustAdmit with its stop (`include/veyra/engine/PushFgSchedule.h`): admission cannot lock out for 2 s after a few slow groups | on, capture cards | SIMULATED: more generated frames shown than Package 13 in every hard set; with the tests' fixed random draw no measure worse in any of 16 card sets, other draws can show a longer single gap; where it acts the readout's average rises (bimodal-5: 23.4 -> 42.5 ms); tested | `VEYRA_TEST_RP_LEGACY_ROBUST_ADMIT` | no |
| gridEarly: a skipped pair's real frame on the earliest 11.1 ms grid slot | on, capture cards | SIMULATED, tested, reviewed for order | `VEYRA_TEST_RP_LEGACY_GRID_EARLY` | no |
| ContentDedup cadence repairs (falling content rate; restart after a still picture) | on, capture cards | tested: steady rates give identical content times | `VEYRA_TEST_RP_LEGACY_DEDUP_CADENCE` | no |
| The real frame's presentation waits for its own lists (VFG, no HUD Lock / exclusion) | on | reviewed list by list; WARP test of the queue contract; benefit unmeasured | `VEYRA_TEST_RP_LEGACY_GPU_REAL_WAITS` | no |
| Capture upload buffer released after the base list | on | reviewed reader by reader; benefit unmeasured | `VEYRA_TEST_RP_LEGACY_UPLOAD_RELEASE` | no |
| Original-runtime neural layer held off by a marker until switched on | on | tested, reviewed twice | `VEYRA_TEST_RP_LEGACY_NR_HOLD` | no |
| `VEYRA_EXP_RP_STATS` on a PS5 stream: statistics no longer between decode and publish | with the switch | tested | - | no |
| Steadiness in `[capture-timing]` (onGrid1msPct, shownPct, robustAdmits, gridEarly, skippedPairs), `triage.py` | always on (log only) | tested | - | no |
| All default-on parts at once back to Package 13 | - | tested: the simulator's rows equal Package 13's | `VEYRA_TEST_RP_LEGACY_PACKAGE13` | - |
| The predicted-frame pass and `VEYRA_EXP_RP_FG_SCHEDULE` (Package 15's lower-delay schedule, GPU half) | NOT in this package (branch `exp/s7-predict`) | - | - | no |

The owner's acceptance number (99 % of intervals on the 11.1 ms grid, 99 % of frames shown): SIMULATED it holds when the
GPU has room (99.2 % or more) and falls short with GPU spikes (96.8-98.8 % on grid). Not known for his PC; the log now
measures it in normal play.

## Package 13 (23 November 2026): the delay of a 30 FPS game at a steady 90 FPS, at 4K

The owner's rule (23 November 2026): 4K (3840x2160) processing is kept; the delay is reduced by scheduling, buffering,
GPU use and frame generation efficiency; lower sizes appear only as labelled diagnostic runs. Everything of Package 12A
(below) plus the rows here. **None of it has run on a gaming PC, a graphics card, a capture card or a PS5.** Nothing new
changes behaviour by default; the one always-on addition is a log line.

| Part | Default? | Tested where | Review | Physically validated |
|---|---|---|---|---|
| Analysis docs/drafts/DELAY_90.md (the working size is 4K at every stage; the delay part by part; the floor of today's interpolation at 4K: 22.2 ms + the frame's own processing + the first generated frame; other scheduling approaches are the next investigation) | doc | code reading | see CHECKED.txt | no: your step 2 zip |
| `[slipstream-sizes]` log line (real size of every stage; `include/veyra/engine/PushSizes.h`) | always on, changes nothing | unit tests, kit dry run | see CHECKED.txt | no |
| The delay split and `gpuFg1P50/P95Ms`, `uploadCpuCopyFps` in `[slipstream-smooth]` | only with `VEYRA_EXP_RP_STATS=1` | unit tests (plain, TSan, ASan+UBSan), kit dry run | see CHECKED.txt | no |
| `VEYRA_EXP_RP_UPLOAD_EARLY_RELEASE=1` (capture upload buffer released after the base work) | off | code reading; compiled by MSVC | see CHECKED.txt | no |
| `VEYRA_EXP_RP_GRAPH_QUEUE_PRIORITY=high` (the graph's GPU queue at high priority, `[graph-priority]`) | off | compiled by MSVC | see CHECKED.txt | no |
| `VEYRA_EXP_RP_FIT_DISPLAY=1` | off; DIAGNOSTIC only, never a recommendation | unit tests | see CHECKED.txt | no |
| Kit sets `d90` (Remote Play) and `d90-capture`, about 30 minutes each, all at 4K except the labelled diagnostic runs | test-only | kit tests, Windows dry run | see CHECKED.txt | no |

## Package 12A (23 November 2026): frame generation latency, part by part (test kit only)

The same program as Package 12 (experimental build 66, `archive/c41755fe`); only the test kit changed. New arm set
`fglat` (inside `-Session smoothness -Pass 120 -Arms flt-...`, about 25 minutes): frame generation off, VFG 2X as
installed, each part of `VEYRA_EXP_RP_FG_ULL` alone (phase, hold, grace, headroom), all parts, all parts with the
presentation queue at high priority, and all parts with VFG Low (docs/HARDWARE_TEST_PROCEDURE.md "Frame generation
latency, part by part"). The kit also keeps `[present-priority]` and clears `VEYRA_TEST_PRESENT_*` between runs.
Checked: 242 kit tests, remoteplay-checks 207 (the Windows dry run of the whole set) and checks 227; an independent
review accepted it with fixes (the `flt-fg2-headroom` arm, the Smoothness Mode note), all made. Nothing measured on a
gaming PC: the run decides which parts lower the real-frame age without hurting evenness.

## Package 12 (22 November 2026): frame generation delay and the "DLSS 5" stutter

Everything of Package 11 (below) plus the rows here. **None of it has run on a gaming PC, a graphics card or a PS5.**
Nothing new is on by default.

| Part | Default? | Tested where | Review | Physically validated |
|---|---|---|---|---|
| Frame generation delay audit docs/drafts/FG_LATENCY.md (the three largest sources; extrapolation studied, not built) | doc | code reading | accepted | no: your smoothness passes |
| `VEYRA_EXP_RP_FG_ULL` (parts hold, phase, headroom, grace; `include/veyra/engine/PushFgLatency.h`); new `[slipstream-smooth]` latency fields with `VEYRA_EXP_RP_STATS=1`; kit arms `sm-fg2-ull`, `sm-fg3-ull`, `cap-fg4-ull`, `cap-fg6-ull` | off | unit tests (plain, TSan, ASan+UBSan), kit dry run | accepted, three minor findings fixed | no |
| "DLSS 5" stutter analysis docs/drafts/NR_STUTTER.md (NVIDIA's neural runtime on the decoded video; likely cause and decision table; placement study: keep the order) | doc | code reading | accepted | no: your four-way runs |
| `VEYRA_EXP_RP_NR_ADAPT=1` (neural-layer result reuse under a time budget, `include/veyra/engine/PushNrAdapt.h`); kit arms `nrp-*`, `nrpc-*` | off | unit tests, kit dry run | accepted, two doc findings fixed | no |

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
