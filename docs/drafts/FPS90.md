# Steady 90 FPS with frame generation: what the code allows, and why the counter jumps 60 <-> 90-100 (stream "fps", 21 Nov 2026)

Scope: the owner's 14th message, part B, and objective 2 of part C. Base: exp/rel-9 (Package 9). Everything below is
either CODE READING (labelled "code") or a FACT FROM THE PACKAGE 9 EVIDENCE (/home/claude/evidence/p9/x, eight runs, frame
generation OFF, labelled "P9"). No frame-generation run exists from the owner's RTX 5070 yet, so every statement about
generated frames is a hypothesis until the retest below has run. Earlier analysis: docs/drafts/SMOOTHNESS_ANALYSIS.md.

## 1. What "90" can mean here (code)

- **Multipliers are whole numbers.** NVIDIA VFG generates the frames at phases k/M of a pair
  (`VfgBackend::generate(parity, subframe, multiplier, ...)`); the offered multipliers are 2..8
  (`displayFgMultiplierSelectable`). There is no 1.5X and no frame at an arbitrary time.
- **A 60 fps game (59.94 on the stream):** evenly paced outputs are 60 (off), 120 (2X) and 180 (3X). 90 is NOT
  reachable evenly. The only ways to get "90" are uneven:
  - generation on about half of the pairs (output = 60 x (1 + share x (M - 1))): holds of 16.7 ms and 8.3 + 8.3 ms
    alternate. 90 = 2X on half the pairs; 100 = 2X on two thirds of them. **Hypothesis (no frame-generation log from his
    PC exists): this matches the 60 <-> 90-100 the owner sees** arithmetically: a 2X stream whose pairs are partly
    skipped, and fully skipped for seconds at a time;
  - 2X with a 90 FPS output cap: presents are spaced evenly (11.1 ms) but every fourth generated frame is thrown away
    at present time (`PresentationScheduler::rateSkipsCandidate`), so the picture advances 8.3, 8.3, 16.7 ms per present
    (judder). `[slipstream-cadence]` cannot see this (it times present CALLS, which are even); `[slipstream-smooth]
    dropCapFps` about 30 shows it.
- **A 30 fps game (GTA in the 59.94 stream):** 90 = 3X, even, but ONLY while duplicate removal knows the game runs at 30
  (`contentFps` about 30); 120 = 4X; 180 = 6X. With duplicates not removed, VFG interpolates between identical pictures.
- **100 is never even:** 100 / 59.94 = 1.67 and 100 / 29.97 = 3.34 are not whole multipliers.
- **The owner's panel ran at 119.998 Hz in all eight P9 runs** (`[slipstream-config] refreshHz=120.00`, `[display-vrr]
  refreshHz=119.998 active=1`; hardware.json maxRefreshHz 179). Follow display then caps output at 120: with a 60 fps
  game only 2X is useful; 3X or more is cut back per pair (code: EngineController output-cap reduction).

**So the honest targets are: 120 for a 60 fps game (2X, VRR on, or Windows at 120 Hz), 90 only for a 30 fps game with
working duplicate removal (3X), and never 100.**

## 2. Every path from "60" to "90-100" and back, with the field that separates it

| # | Path | Code | Effect | Separating fields (per 5 s window) |
|---|---|---|---|---|
| P1 | Admission skips whole pairs when the predicted GPU cost (P95 of base + group) misses the deadline | `FgRecoveryBudget::admit`, `admitFg` | 2X on part of the pairs: 90-100 | `[slipstream-smooth] rejectedPairsFps` > 0 with `evaluatedPairsFps` 30-40, `generatedFps` 30-40; `gpuSpanP95Ms` near `pairBudgetMs`; `[live-fg-admission] rejectedPairs` |
| P2 | FgCostSettling hold after a cold probe misses: no generation for 2, 4, then 8 s | `FgCostSettling::observe` | exactly 60 for seconds: the "steady 60" phases | `holds` > 0 and `generatedFps` 0 in the same window; `[vfg-probe] retryInMs=`; `[slipstream-cadence] generatedFps=0.00` |
| P3 | Cap rounding with a measured rate: `ceil(cap / content)`; the quick setup turns duplicate removal on whenever it applies a target (`PushQuickSetup.qml` "contentRate = 1"), so content is MEASURED (59.94): ceil(119.998 / 59.94) = 3, so 3X is generated for a 120 cap and 60 frames a second are thrown away | EngineController output-cap reduction | 50 % more generation work than 2X, which then feeds P1/P2; uneven picture | `chosen=3X why=cap dropCapFps` about 60; fixed by `VEYRA_EXP_RP_SMOOTH_CAP_FIT=1` (Package 9, off by default) |
| P4 | The quick setup's game frame rate defaults to 30 (`pushGameFps || 30`): with a 60 fps game the multiplier is chosen for 30 (target 120 -> 4X, target 90 -> 3X), then cut back per pair by the cap | `PushQuickSetup.qml multiplierFor` | the same as P3 | `ceiling=4X` (or 3X) with `contentFps` about 60 |
| P5 | Duplicate-removal state flips (a 30 fps game classified 60 for a while, or the reverse) | `ContentDedup`, `PushStreamDedup` | the per-pair multiplier jumps (ceil(120/30) = 4 vs ceil(120/60) = 2) | `contentFps` and `chosenMin` / `chosenMax` differ within a window; `[remoteplay-cadence] locks= unlocks=` |
| P6 | Smoothness Mode | `PushSmoothnessMode` | never below 2X, never off: it cannot produce the 60 phases. With target 90 and a 60 fps game it runs 2X (120) | `why=smooth smoothReason= stepDowns=` |
| P7 | 4K working size (SR to 4K on a 1440p panel) | `ResolutionPlan` (fg = SR output) | 2.25x the pixels for VFG and its flow: P1/P2 more likely | `work=3840x2160 display=2560x1440`; `VEYRA_EXP_RP_SMOOTH_SR_DISPLAY=1` |
| P8 | RTX Video SR / neural layer cost spikes | graph stages before generation | a slow base pushes the group past its deadline: P1 | `gpuSrP95Ms`, `gpuNrP95Ms`, `gpuSpanP95Ms`; `[gpu-stage-stall]` (>= 80 ms) |
| P9 | VFG's faster model (2X with auto-regulate only) | `vfg-fast` | still generated: no FPS change, possibly softer frames | `fastPairsFps`; `[vfg-fast]` |
| P10 | Performance modes and controllers: the mode driver re-applies a mode when the measured source changes rows (Balanced: 3X for 30, 2X for 60); Automatic (`VEYRA_EXP_AUTO_MODE=1`) can step to Competitive (no generation for a 30 fps game); the governor / GPU budget in apply mode can lower the multiplier or turn generation off | `PushModeDriver.qml`, `PushPerformanceMode.h`, `PushSlipstream.cpp` | whole-mode jumps, including 60 | `[push-mode] applied ... reason=`, `[slipstream-governor] action=`, `[slipstream-gpu-budget] reason=` |
| P11 | VRR and refresh: 120 on a FIXED 180 Hz is uneven (1-2 holds); on the owner's 120 Hz with VRR, 120 is even | presenter | uneven but not 60 <-> 90 | `[slipstream-cadence] refreshHz vrr fixedHold1/2/3` |
| P12 | **The PS5 frames already arrive unevenly (P9, FG off):** per run (median of its 5 s windows) `[slipstream-delivery] gapP50` 11.6-18.1 ms and `longGaps` 24-140 per 5 s over the eight runs (one window of the 50 Mbps run: gapP50 19, gapP95 28, longGaps 70); native-60 present calls `[slipstream-cadence] intervalP95Ms` about 26 ms and `offPct` 41-100 % in all eight runs. With frame generation, a PS5 stream anchors each pair at its own decode time (`smoothAnchor` excludes streams, EngineController "!isStream"), so generated frames inherit that jitter (hypothesis for FG; the arrival jitter itself is measured) | network / PS5 encoder pacing; EngineController | uneven pacing at every rate, the counter steady | `[slipstream-delivery] gapP95 longGaps`, `[slipstream-cadence] intervalP95Ms offPct` |

P1, P2 and P3/P4 together produce the reported pattern arithmetically: 2X (or 3X cut to 2X) on a 120 Hz cap, some
pairs skipped (90-100), all pairs skipped while a hold lasts (60). Which of them dominates on the owner's PC is what
the retest must show; the reported 60 / 90-100 alternation fits P1 + P2 exactly (hypothesis, no FG log exists).

## 3. Defects the code proves (fixed in this stream, see section 5)

- `[slipstream-smooth] chosen=2X` is printed with frame generation off (P9 logs: `backend=none ceiling=1X chosen=2X`):
  the window recorded the multiplier the graph would use, not 1.
- Smoothness Mode with a target between the game's rate and twice it (90 for a 60 fps game) runs 2X = 120 and says
  "the game is already close to the target" (`target-below-2x`), which is not what happens.

## 4. The source rate (stream "sync", P9)

The console sent fewer than 60 frames a second at lower bitrates in the owner's scene (wire index rate per run: 15 Mbps
58.6 / 57.7, 30 Mbps 57.5 / 57.9, 50 Mbps 59.0 / 59.4, 100 Mbps 59.9 / 59.8; zero index gaps, zero loss: the encoder
itself sent fewer). Consequences for frame generation (code):
- 2X of 57.5 is 115: under a 120 cap that is BELOW the cap. Both the old rule (ceil(120 / 57.5) = 3) and the tolerant one
  (57.5 x 2 x 1.02 = 117.3 < 120) choose 3X, and the cap then throws away about 52 frames a second. Fixed for confirmed
  VRR (section 5): 2X, 115 shown as they come.
- The stream's presentation times are stamped wireIndex / 60 with a limited phase correction (RemotePlayClock), so a
  57.5 FPS source looks like a 60 FPS source whose frames arrive late; frame generation interpolates on those times.
  Stream sync's `VEYRA_EXP_RP_CLOCK_RATE` addresses the clock; the retest reports the rates per arm ("source rate":
  `[remoteplay-progress] decodeFps`, `[slipstream-smooth] contentFps receivedFps`).
- Whatever the multiplier, the output follows the source: 2X of a source that varies 57.5-60 shows 115-120, never a
  fixed number. Nothing on the PC can create frames the console did not send.

## 5. What changed in this stream (code; unverified on Windows; nothing measured)

| Commit | Change | Default? |
|---|---|---|
| 64ac01e8 | `[slipstream-smooth]` records 1X / why=off with frame generation off; Smoothness Mode reason `target-not-a-multiple` (90 for a 60 fps game runs 2X = 120, and says so) | yes (reporting / wording) |
| 75e534e1 | the output-cap reduction with the 2 % tolerance: 59.94 under 119.998 gives 2X (was 3X), under 179.95 3X (was 4X); only ever lowers the multiplier, the frames removed are the ones the cap threw away; `[push-cap-fit]` logs each change; `VEYRA_TEST_SMOOTH_CAP_CEIL=1` restores the old rule | yes |
| 0c04b867 | under confirmed VRR the reduction keeps the output within the cap (57.5 FPS under 120: 2X = 115, was 3X) | yes |
| ff323428 | test key `cap=display|off|30..360` in `VEYRA_TEST_RP_PIPELINE` (the kit's 90 FPS arm) | test only |
| 37f420ae | the retest: eleven arms, `-Pass 120` / `-Pass 180` | kit |

What a user who set nothing sees after 75e534e1 / 0c04b867: only while graph frame generation runs with an output cap
(smooth pacing's Follow display, on by default, or a custom cap), adaptive off and a multiplier of 3X or more: one
multiplier lower where the higher one only fed frames to the cap (120 Hz panel, 60 fps game: 2X instead of 3X). No
queue, no wait and no pacing rule changed; the latency of a real frame is unchanged (graph frame generation holds it one
content interval at any multiplier).

Not changed, with the reason: the quick setup's default game frame rate (30) picks a 4X ceiling for a 120 target on a
60 fps stream; with the cap rule fixed the per-pair multiplier is 2X, and what remains is a graph built for 4X (video
memory for three generated frames, about the size of three frames at the working size), shown as `ceiling=4X
chosen=2X`. A smoothed presentation anchor for PS5 streams (P12) would move frames later than they arrive: it is a
latency trade and is left to a measured decision.

## 6. Answers to the owner's B1-B10 (confirmed = from code or P9 logs; hypothesis = needs the retest)

1. **Why generated FPS moves between 60 and 100.** Hypothesis, consistent with the arithmetic: 2X under a 120 cap with
   part of the pairs skipped by admission gives 90-100 (P1); a FgCostSettling hold gives exactly 60 for 2-8 s (P2);
   3X generated under the 120 cap (P3, fixed by default now) makes both more likely. The retest separates them
   (`rejectedPairsFps`, `holds`, `chosen`, `dropCapFps`).
2. **Source, decoded, generated, submitted, displayed.** Confirmed available per 5 s: source and decoded
   (`[remoteplay-progress] decodeFps`, `[slipstream-smooth] receivedFps contentFps`), generated and thrown away
   (`generatedFps`, `dropCapFps`, `dropDeadlineFps`), submitted (`[slipstream-cadence] submittedFps`), displayed: only
   PresentMon measures it; `[slipstream-cadence]` gives the fixed-refresh MODEL with VRR off.
3. **Disabled, throttled, skipped or GPU-limited.** Confirmed mechanisms: skipped (P1), held off (P2), lowered by the cap
   (P3/P4), by a mode or controller (P10); GPU limit = `gpuSpanP95Ms` against `pairBudgetMs`. Which one happens on his
   PC: retest.
4. **Pacing, VSync, VRR, refresh.** Confirmed: his panel ran at 119.998 Hz with VRR active in P9; the PS5 frames arrive
   unevenly (P12), visible even without frame generation (native-60 present spacing P95 about 26 ms).
5. **Smoothness Mode against fixed settings.** Arms `sm-mode-120` / `sm-mode-90` against `sm-fg2-vrr`. Expected from code:
   at 120 Hz with a 60 fps game the mode runs 2X like the fixed arm (no difference); it cannot remove the 60 phases
   caused by holds (P2), and at 90 it runs 2X = 120 for a 60 fps game.
6. **4K processing.** Confirmed: with SR to 4K, frame generation works at 3840x2160 (`work=`). The retest's
   `sm-sr4k-fg2` against `sm-sr1440-fg2` measures the cost.
7. **RTX Video SR / neural layer.** `sm-vsr-fg2` and `sm-nr-fg2` against `sm-fg2-vrr`: their stage times and whether
   pairs are skipped with them. No measurement yet.
8. **Consistent delivery over a high counter.** The default change removes generated frames the cap threw away; the
   retest page judges arms by present spacing (`offPct`, P95/P99) and frames thrown away, not by the counter.
9. **No added latency or buffering.** Nothing in this stream adds a queue or a wait; every behaviour change only lowers
   the multiplier.
10. **Image quality.** Unchanged by the default changes (the same frames, fewer thrown away). The 4K-versus-1440p
    question is a picture A/B the owner judges by eye (`sm-sr4k-fg2` against `sm-sr1440-fg2`).

**The highest genuinely sustainable target, stated honestly from code:** for a 60 fps game, 120 FPS (2X) with VRR on his
panel, IF the retest shows no skipped pairs and no holds at 2X with his picture settings; otherwise 60 (frame generation
off) is the only other even rate. A steady, even 90 or 100 is not possible from a 60 fps game with whole multipliers.
For a 30 fps game with working duplicate removal, 90 (3X) and 120 (4X) are even. 180 (3X of 60) needs the panel at
180 Hz and three times the generation work per second of 60; whether the RTX 5070 holds it is the 180 Hz pass.
