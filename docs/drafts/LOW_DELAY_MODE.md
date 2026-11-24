# "Lower delay": one predicted frame per real frame (Package 15, stream lowdelay, 24 Nov 2026)

Labels used throughout: **SIMULATED** (the scheduling simulator, modelled costs, no GPU), **TESTED** (unit or QML tests on
Linux, or the remote checks), **BUILT** (compiled into PushFPS.exe on CI's Windows machine, software renderer),
**HARDWARE-VERIFIED** (run on the owner's PC). Nothing here is HARDWARE-VERIFIED. No claim is made about the picture.

The owner, 23 Nov 2026: "i get 90 frames with 30 ms i need less ms delay than this". Priorities, in order: no lag or
stutter, a stable 90 from 30, full 4K, then delay.

## 1. What it does

Today a capture card's 3X pair shows I(1/3), I(2/3), B: the real frame B waits until both frames generated between A and
B have been shown. With "Lower delay" a pair shows I(2/3), B, P(B + 1/3): one frame generated between A and B, then the
real frame, then a frame **predicted** from the motion A -> B (the GPU half: `docs/drafts/PREDICT_GPU.md`). B goes out
one output slot earlier: a third of a game frame less wait (11.1 ms at a 29.97 FPS game) where the GPU has room and the
schedule holds. That figure is the schedule's arithmetic, SIMULATED below, not measured on any PC. One shown frame in
three is predicted, not interpolated; when motion starts, stops or turns, that frame is wrong for one slot.

**No per-frame guarantee.** Every figure below is an average or a share over a run. Single frames can come later than
with the mode off (section 4, "Per pair"), and the owner's 99 % on-grid target is missed in the simulator in some
sets (section 4). Nothing here promises a lower delay for each frame.

- **Off by default.** On through the quick-setup row (one click, applies in the running session) or, for the test kit,
  `VEYRA_EXP_RP_FG_SCHEDULE=onepredicted` set before PushFPS starts (the row then shows "on through the test switch").
- **Where:** a capture card (not PS5 Remote Play, not files), NVIDIA VFG, a pair that really runs 3X (not when the adaptive
  multiplier or the output cap makes it 2X: the row says "needs 3x"), Smoothness Mode off (both decide the schedule, so
  they are exclusive: the row says so), RGBA8 or RGB10A2 output, no Live HUD Lock
  or exclusion boxes, no present-sink frame generation. These are checked before a pair is asked
  (`EnhanceGraph::predictStaticRefusal`): the mode does not ask, does not back off, and the row says why. 4K stays.
- **Paused** (the row says which) while the frame quality test, the optimiser's benchmark, HUD detection or a comparison
  clip runs.
- **Off, the frame path is Package 14's:** no pass exists (`EnhancementSettings::lowDelay` false, no kit switch), no pair
  is asked, the learners get Package 14's arguments. TESTED in the simulator present by present (16 card sets, card and
  stream) and with the row switched on and off in one session (frames before the switch identical to Package 14's).

## 2. The row and one click

Pref `pushLowDelay` -> `EnhancementSettings::lowDelay`, a graph-shape setting: with VFG at 3X or more, switching it
rebuilds the live graph as HUD Lock does, creating the predicted-frame pass (on) or releasing it (off); elsewhere it is
stored and changes nothing. The engine also reads the row once a pair, so "off" stops asking at once. The rebuild drains
presentation and recreates VFG and the pass: the picture can pause for a moment (its length is not modelled or measured).
States: idle, calm, active, starting (rebuild on its way), not-here (with the reason), unavailable (built for it, no
pass: the log says why), suspended (with the reason). Help text: no figure is stated as fact.

## 3. How the schedule decides (include/veyra/engine/PushLowDelay.h; the simulator runs the same code)

- **Enter** after a calm run of 8 pairs (eligible, not skipped, not held, not refused).
- **Stay in on a bad prediction (policy StayIn, the default).** P is always shown exactly as the pass composed it:
  trusted pixels moved, untrusted pixels B; a cut seen by VFG's GPU test or a P that could not be recorded is B as a
  whole. An "unusable" verdict (`pushPredictFrameUsable`: more than 25 % of the moving pixels fell back, an ESTIMATE)
  never changes the schedule by itself. The verdict arrives 1-2 pairs after its P (at GPU completion) and only feeds a
  sustained-failure counter: 45 of the last 60 unusable leaves once, then a calm run of 300 pairs (doubled up to 1800 if
  it fails again within about 8.5 s) is needed to return. Rollback to the first policy (leave at once on an unusable
  verdict, return after 8 calm pairs, back-off to 240): `VEYRA_TEST_RP_LOWDELAY_LEAVE_ON_UNUSABLE` (environment or
  slipstream.ini; named in the start-up switch line).
- **Leave at once** for what makes the schedule impossible: row off or paused, not eligible, a hold (LiveFgRejectionHold,
  FgCostSettling), a skipped pair, a graph refusal (after the static checks only a history reset remains). Off, paused
  and not eligible are not backed off.
- **Deadlines** follow each frame's timestamp: I at phase - T/3, B at phase, P at phase + T/3. Each schedule learns its own
  phase (two LivePairLatency); a pair is timed with the predicted learner only when it RAN predicted.
- **Switches:** each one moves the real frame by one slot (11.1 ms): entering skips the 1/3 content position once
  (a jump), leaving repeats it once. They are NOT free of off-grid intervals: when interpolation needs more than the
  predicted phase + T/3, the first pairs after an exit are late (the simulator's P95 tails). Stay-in exists to make
  switches rare.
- **Admission:** a pair asked to run predicted checks base + Fg1 against I's deadline and the whole group against P's;
  a history reset (refused by the graph) is judged against today's deadline.
- **Never out of order, never backwards:** P is part of its own pair's batch after B; a P whose time lies at or beyond the
  next pair's first frame, or whose presentation would start once that time has passed, is dropped before it is shown
  (`pushPredictedDropBeforeNext`, checked at the moment it would be queued). Deviation from the review's wording: the
  check is against the next pair's first frame TIME, not against "the next pair's first frame is queued", because the
  next pair is built after this one's batch; a later frame cannot be queued before P in the same queue.
- **Late watch** (`predictedOutcome`, `gridWindow`): the mode leaves (as a sustained exit, with back-off) when more than 2
  of the last 60 predicted frames were dropped late, or after 3 one-second windows below 98 % on the grid in the mode
  while interpolation, measured in the same session, held 98 % or more. Without that comparison it does not leave on the
  grid share alone: in the simulator a plain "below 98 %" rule made the heavy sets worse (near-limit 98.2 -> 93.2 %,
  overload 97.3 -> 34.7 %), because Package 14 is no steadier there. SIMULATED: at P = 3 ms the watch changes nothing
  (every present identical); at P = 18 ms it stops a runaway (near-limit mean delay 116-325 -> 38-39 ms, overload
  417-634 -> 22-24 ms). Not fixed by it: with P = 18 ms on from the start, bimodal-20/25 stay at 88-90 % on the grid
  against Package 14's 99 %.
- **Whole frame held:** when more than half of the moving pixels fell back, the pass shows B as the whole P (GPU
  verdict + predicated copy), counted apart (`lowDelayWholeHeld`). A half-moved, half-held frame tears worse than a
  repeat.
- **VFG refusing "index 2 alone":** the predicted pair asks VFG for FrameIndex 2 of 3 without index 1. If his driver
  refuses, the mode is unavailable for the session (one log line `VFG refused or failed a FrameIndex-2-only request`,
  `[fg-sched] lowDelay refusal=vfg-refused`, the row says so), interpolation goes on, no FG failure is recorded, no
  retry. **This is the first thing his log will show.** Switching the row off and on tries once more. Tested with a
  fake refusal (`VEYRA_TEST_VFG_REJECT_ALONE`, test only); not testable on WARP (VFG needs CUDA).
- **Counters** of each P are read by its own number (`PushPredictReadbackRing`), never another pair's.
- **Delay readout** stays capture arrival -> the REAL frame's Present returned.

## 4. SIMULATED results (FgScheduleSim.h; `tools/remoteplay_latency_bench/lowdelay_three_way.cpp`)

16 card cost sets x seeds 1, 2, 3, 7, 11, 99 (96 runs per row), 120 s each. P (the predicted frame: flow + warp) costing
3, 6 or 10 ms of GPU (unmeasured). Unusable predictions come in bursts of 8 pairs on average. Values: mean over the 16
sets, then the range of that mean across seeds. Delay = capture callback -> the real frame's Present returned.
"Grid" = intervals within 1 ms of T/3. "Step error" = content steps between shown frames against the ideal, in slots
per minute (repeats, jumps and partial steps all count). h = the share of a bad P's motion the pass holds at B
(h = 1, wholly B, is the upper bound: the pass holds only untrusted pixels).

| Design, P cost | unusable | delay mean ms | delay P5-P95 spread ms | P99 ms | grid % | switches/min | runs > 0.25 pt below P14's grid | runs below 99 % grid |
|---|---|---|---|---|---|---|---|---|
| Package 14 | - | 37.6 (37.2-37.9) | 9.7 (7.3-13.4) | 43.9 (43.2-44.2) | 91.1 (90.7-91.5) | 0 | - | 60 / 96 |
| stay-in, 3 ms | 0 % | 28.3 (28.2-28.4) | 6.6 (6.3-6.9) | 33.3 (33.0-33.6) | 99.1 (99.0-99.2) | 0.5 | 0 / 96 | 36 / 96 |
| stay-in, 6 ms | 0 % | 28.2 (28.0-28.4) | 6.9 (6.5-7.4) | 33.4 (33.1-33.5) | 98.7 (98.4-98.9) | 0.7 | 0 / 96 | 42 / 96 |
| stay-in, 10 ms | 0 % | 27.7 (27.2-27.9) | 6.2 (5.5-7.5) | 32.2 (31.9-32.5) | 97.7 (94.5-98.5) | 0.6 | 6 / 96 | 46 / 96 |
| stay-in, 3 ms | 5-10 % | 28.4 (28.2-28.8) | 8.0 (6.3-14.7) | 34.6 (33.3-40.8) | 98.8 (97.2-99.2) | 0.7 | 0 / 96 | 36 / 96 |
| stay-in, 10 ms | 5-10 % | 27.8 (27.2-28.6) | 7.4 (5.5-13.1) | 33.6 (32.0-40.4) | 97.6 (94.5-98.5) | 0.8 | 6 / 96 | 46 / 96 |
| stay-in, 3 ms | 20 % | 29.7 (28.3-33.4) | 14.6 (6.8-18.2) | 40.6 (33.4-43.4) | 96.1 (93.9-99.1) | 1.8 | 0 / 96 | 42 / 96 |
| stay-in, 3 ms | 30 % | 30.9 (29.7-33.8) | 17.0 (16.2-18.1) | 43.1 (42.5-44.3) | 95.3 (93.9-97.4) | 2.9 | 0 / 96 | 48 / 96 |
| leave (first policy), 3 ms | 10 % | 32.9 (30.8-35.5) | 18.9 (18.4-19.4) | 46.6 (46.2-46.8) | 93.6 (92.2-94.3) | 32.7 (21.4-38.1) | 10 / 96 | 58 / 96 |

The last column replaces the earlier "lost 99 %" column, which counted runs where Package 14 held 99 % and the mode did
not: none ever, so it said nothing. **Per set** (mean of the 6 seeds, unusable 0 %), the 99 % on-grid target is missed
in 5 / 6 / 7 of the 16 sets at P = 3 / 6 / 10 ms; Package 14 misses it in 8. At P = 3 ms: spikes 98.4, near-limit 98.2,
overload 97.3, bimodal-5 98.1, bimodal-10 98.1 (Package 14 there: 97.0, 85.3, 11.3, 93.0, 90.7). P = 6 ms adds hiccup
98.75 (overload 92.9); P = 10 ms: spikes 97.9, near-limit 96.4, overload 79.9, hiccup 98.3, bimodal-5 97.0,
bimodal-10 98.1, heavy 98.96.

**Per pair** (a driver comparing the same pair in both schedules): where Package 14 neither skips nor holds a pair, the
mode's real frame is never more than 5 ms later and at most 2 % of pairs are later at all (TESTED,
`LowDelaySimTests.inc`). Where Package 14 skips or holds, it shows that pair's real frame early and the mode can be
later: in overload about 52-56 % of pairs, near-limit about 9 %, bimodal-stretch/short 2-5 %, bimodal-5 +2-3 ms on up
to 1.6 % of pairs. "Never later than Package 14 for a pair it would skip" is not reachable by applying its skip
decision first: that decision depends on Package 14's own queue and history, which the mode's run does not have
(tried twice: admission by Package 14's rule first, and a doubled Fg1 estimate; neither changed the results).

Content step error, slots per minute (P 3 ms): Package 14 220 (185-254, from skipped pairs in the heavy sets); stay-in at
5-10 % unusable: h 0.25 68-113 (35-182 across seeds), h 0.5 110-201 (64-275), h 1 195-378 (122-483); leave at 10 %: 182
(154-202) at any h, plus a delay step of a slot at each of its ~33 switches a minute.

What the simulation says (SIMULATED only):
- **The gain is about 9 ms of mean delay (11.1 ms of schedule)** where the GPU has room and predictions hold: 37.6 ->
  28.3 ms over the 16 sets, with a narrower spread (6.6 against 9.7 ms) and more intervals on the grid (99.1 against
  91.1 %), because a predicted pair needs one VFG run instead of two and heavy sets skip fewer pairs.
- **It shrinks with unusable predictions** only through the sustained exit: up to 10 % the stay-in mode keeps the delay
  (28.4 ms) and its spread; at 20-30 % it leaves more often (spread 14-17 ms, delay 29.5-30.8 ms).
- **The price of staying in is the content of the bad predictions:** each wholly bad P (h = 1) is one held frame and a
  double step after it. With partial holds (h 0.25-0.5) the step error stays at or below Package 14's up to 10 %. How
  much of a bad P is really held is unknown until his PC logs it.
- **A dearer P costs steadiness near the GPU limit:** at P = 10 ms, 6 of 96 runs fall more than 0.25 points below
  Package 14's on-grid share (none at 3 or 6 ms); the mode does not detect that by itself (section 6).
- **Overload** (a GPU far over budget): the mode's delay is HIGHER than Package 14's for about half the pairs, because
  Package 14 skips most pairs there and shows real frames early; the mode shows 97.3 % on the grid instead of 11.3 %
  (P = 3 ms).
- **Switches still happen at once** for skips, holds and history resets: 0.5-0.8 a minute in the simulator with
  predictions usable; each one is a single jump or repeat of a third of a game frame and a step of 11.1 ms in delay.
- **The picture is unknown.** No simulation says anything about how P looks.

### What he may see (plain words; none of it is measured, all of it follows from how P is made)

- The front edge of a moving object briefly squashed (the backward gather does not advance it: section 6).
- Every third frame slightly softer where the picture moved (bilinear warp; still parts are exact).
- Thin lines (wires, fences, antennas) jittering: their motion is often not trusted, so parts move and parts stay.
- Repeating textures (grilles, tiles, water) can swim: the flow may match the wrong repeat.
- Fast pans and particles: P keeps the real picture there (held), so those parts step once in three frames, which
  looks like judder.
- When motion starts, stops or turns: one frame in the wrong place.
- A jump or a repeat of a third of a frame each time the mode switches in or out (above: 0.5-0.8 a minute simulated).

## 5. What is tested

- TESTED (g++, Linux; MSVC /W4 and sanitizers through the remote checks): `tests/remoteplay/LowDelayTests.inc` (schedule,
  both policies, sustained exit and its back-off, pauses, states, the row's graph shape, phase arguments, resets,
  admission), `LowDelaySimTests.inc` (off = Package 14 present by present; switched in a session; no fallbacks; bursts;
  stay-in spread; held share h), `PredictTests.inc` (pixel rule, trusted motion, cut model, static refusal, counter ring,
  P never beyond the next pair), `GpuPathTests.inc` (VFG back-to-back never chains to another pair's value),
  `LagfixSettingsTests.inc` (the rollback switch from slipstream.ini).
- TESTED (QML, CI): `tests/qml/quick/tst_push_lowdelay.qml`.
- WARP (CI's Windows build only): `predict`, `predict-hdr10` (including a quarter-size trust-scaled flow and the counter
  numbers). VfgBackend::copyInput (P = B on a cut or a failed P) needs CUDA: not testable on WARP.
- Local syntax checks of `EnhanceGraph.cpp` and `VfgBackend.cpp` stopped at a missing header before this round (FFmpeg,
  Windows.h case), so earlier reports that they passed locally were wrong; with stub headers both compile through with
  the same diagnostics as dc93f898. The remote MSVC checks were not affected.
- BUILT: dc93f898 (before this work) built, installed and passed its WARP tests on CI's Windows runner. The later
  commits compiled in the remote checks' MSVC jobs (core tests and headers), not as PushFPS.exe.

## 6. Known limits

- The late watch leaves only when interpolation measured in the same session was steadier; a P too dear for the GPU
  (P = 10 ms above) still lowers steadiness in some sets without leaving. The `[capture-timing]` line shows
  `onGrid1ms` and the `[fg-sched] lowDelay` line the counters, so his log shows it.
- The per-pixel verdict is a plain fallback share. A seam-aware verdict (regions, seam length) is not built.
- The warp is a backward gather of the motion at the target pixel: at the LEADING edge of a moving object the pixels
  just ahead are background, so the object's front does not advance (it steps 0, then 2v/3); beside a still object over
  a moving background the revealed strip falls back while the rest moves. A candidate search (self-consistent
  neighbouring vectors, the larger motion preferred) is not built. Disocclusions (content hidden in B) cannot be known:
  there P keeps B.
- Warped pixels are bilinear samples of the encoded values (PQ or gamma), softer than B; still and held pixels are
  bit-exact. Catmull-Rom sampling is not built.
- The rebuild on switching the row is not timed anywhere.

## 7. What only his GPU and eyes can decide

1. **Cost** of P at 4K (flow at 960x540 + warp): `gpuFg2` of predicted pairs. The simulation brackets 3-10 ms.
2. **How often and how much a prediction is bad** in his games: `[fg-sched] lowDelay ... unusableShown`,
   `[capture-timing] lowDelayUnusable`, `lowDelayExits`; whether the 25 % threshold and 45-of-60 are right.
3. **The look** (priority 3, by eye, in motion): motion starting, stopping, turning; edges of moving objects; HUDs;
   fast pans; particles. Turn it off if it smears.
4. **The delay** on his PC: `d90c-ld` against `d90c-fg3` (`-Arms d90c-fg3,d90c-ld`; he is not asked to run it).

## 8. Files

`include/veyra/engine/PushLowDelay.h`; `include/veyra/pipeline/PushPredict.h`; `src/engine/EngineController.cpp`
(Package 15 blocks); `src/pipeline/EnhanceGraph.cpp` (initPredict, predictStaticRefusal, the predicted pair);
`src/pipeline/VfgBackend.cpp` (copyInput, the back-to-back revision check); `src/pipeline/PushPredictPass.cpp`
(counter ring); `include/veyra/engine/EnhancementSettings.h` (`lowDelay`); `src/engine/EffectChain.cpp`
(requiresGraphRebuild); `tests/remoteplay/FgScheduleSim.h`; `qml/Veyra/PushLowDelayRow.qml`;
`src/ui/bridge/PushSlipstream.cpp`; `src/ui/QmlPlayerBridge.cpp`; `i18n/pending/lowdelay.json`;
`tools/remoteplay_latency_bench/lowdelay_three_way.cpp`.
