# Measuring PS5 Remote Play latency to a few milliseconds (codename Slipstream)

Status: design and code for the experimental build, sprint 4 (20 November 2026). Everything below is
**unverified on Windows** and has not run on a PS5. The numbers quoted from the first hardware run are software
counters from one Wi-Fi session (BENCH1: Ryzen 7 8700F, RTX 5070, 180 Hz display, 1080p60 HEVC stream); the
other numbers are arithmetic from those counters, labelled as such. Nothing here is input-to-photon.

Code: `include/veyra/engine/PushLatencyStats.h` (statistics, portable, tests in
`tests/remoteplay/LatencyStatsTests.inc`), `src/ui/bridge/PushLatencyTest.cpp` (the test),
`src/engine/PushInputLatencyProbe.cpp` (reading presented frames), `include/veyra/engine/PushAutorunPlan.h` and
`src/ui/bridge/PushAutorun.cpp` (automation). Log lines and switches: section "Sprint 4: measurement and
automation" of `docs/PS5_REMOTE_PLAY_EXPERIMENTS.md`.

## 1. What limited the first run

The built-in test presses the PS button through Remote Play and waits until the bottom 45 % of the presented
picture changes clearly (`latency::Detector`: mean luma difference against the last frame before the press above
max(0.04, 3 x the recent frame-to-frame motion)). That is the control centre sliding in.

| Source of spread | Size | Effect |
|---|---|---|
| The console's own response to the PS button (home menu, control centre animation) | median ~557 ms, per-press sd 26 ms (BENCH1, software counter) | dominates everything else; the same in every arm |
| Frame quantisation at the console: a press waits for the console's next game/encoder frame | uniform 0-16.7 ms at 60 fps (0-33 ms for a 30 fps game) | adds ~5-10 ms of sd |
| Frame quantisation of the stream and the display | 0-16.7 ms stream, 0-5.6 ms at 180 Hz | adds spread; the display part is inside the PC share |
| Press phase: with a fixed gap between presses, presses can lock to one phase of the stream's frame clock | a bias of up to one frame interval that differs from run to run | looks like a difference between arms where there is none |
| Number of presses | 12 per test in the original build | the 95 % interval of a median of 12 presses with sd 26 ms is about +-18 ms |

With sd 26 ms the smallest difference between two runs that a test finds 80 % of the time at the 5 % level is
`(1.96 + 0.84) x sqrt(2) x 1.2533 x sd / sqrt(n)` (median of n presses): about 37 ms for 12 presses, 18.2 ms for
50, 9.1 ms for 200, 5.8 ms for 500, and 5 ms needs about 670 presses per arm (arithmetic, `pressesForDifference`;
the reviewer recomputed the 50 / 200 / 500 figures). **The button test therefore cannot resolve a difference of a
few milliseconds in the total**, at any press count this build offers. What can resolve the PC-side part is the
in-app split below (`pcP50` and its own detectable difference) and the continuous `[slipstream-latency]`
percentiles; the total only says whether something large changed.

## 2. The design that reaches a few ms

### (a) Split every press at the network arrival of the frame that answered

For each answered press the probe now knows three host times on one steady clock (100 ns):

- **sent**: the moment the press went to the controller path (`markSent`, unchanged);
- **arrival**: when the frame whose picture showed the answer arrived from the network (the takion callback's
  `arrival100ns`, recorded per real frame by one line in `EngineController.cpp`; for a generated frame, the
  real frame it was made towards);
- **present**: just before that frame's `Present` (`PresentCapture::record`, unchanged).

`sendToArrivalMs = arrival - sent` is the console, its encoder and the network. PC-side changes move it only
through the time the input goes out (pad polling, input queue). `arrivalToPresentMs = present - arrival` is the PC
pipeline from the network arrival **up to the present-capture record**, taken just before the frame's `Present`
call: decode, hand-off, enhancement, frame generation and the wait for the presenter's turn. It does **not**
include what follows `Present`: the swap chain's queue (DXGI frame latency, flip queue), the wait for vblank and
scan-out. It is what our PC-side work changes, and it is expected to have a much smaller spread than the total;
its per-press spread has not been measured yet (see below).
`arrivalToScanoutMs` is the same up to the moment the frame reached the screen; the scanout time per frame
belongs to the presenter (stream "present"), so this build logs -1 there and the PC share uses Present
(`pcBasis=present`). When the presenter exposes per-frame scanout, `PushLatencyStats::split` already takes it.

For a PC-side A/B, compare `pcP50` (and, much better, the continuous `[slipstream-latency]` arrival-to-scanout
percentiles over every frame, thousands per minute) instead of the total. How finely `pcP50` resolves depends on
the per-press spread of `arrivalToPresentMs`, which no run has measured yet: BENCH1's 0.09-1.1 ms is the spread
of `pushfpsMs`, a continuous counter averaged per run, not a per-press spread, so it cannot be used to predict
the press count (an earlier version of this page did, and claimed 100 presses resolve 0.5 ms: withdrawn). Each
test reports what it achieved (`pcDetectable=` in `[slipstream-latency-test]`); read that, not a prediction.

### (b) Many more presses, with the press phase dithered

- Up to **500 presses per test** in this build (`latencystats::kTrialsMax`; the original build accepts 20 in the
  bridge and offers 12 in the dialog). The dialog offers 5, 8, 12, 50, 200, 500; the default stays 8.
  Orchestrator decision (review S5): the 50 / 200 / 500 choices and the button choices are visible **without a
  switch**, because the owner asked for more presses. The default (8 presses of the PS button) is unchanged and
  runs the original test byte for byte.
- The wait before each press is 300 ms plus a **uniform random share of four stream frame intervals**
  (`pressGapMs`, deterministic generator seeded per test), so presses land at every phase of the stream's frame
  clock. A press's phase (`phaseMs` = time since the newest frame arrival before the press, modulo the frame
  interval) is logged so the kit can check the phases cover the interval evenly.
- Duration (approximate cycle lengths from the code): PS button about 3 s per press with its ~0.56 s answer (500
  presses: about 25 minutes); D-pad about 0.75 s plus its answer per press (500 presses: about 7-8 minutes).

### (c) Robust statistics

For each part (console, PC, total): median, P95, 10 % trimmed mean, a percentile bootstrap 95 % interval of the
median (1000 resamples, deterministic), and the **minimum detectable difference** for two runs like this one
(`detectableDifferenceMs`: 2.80 x sqrt(2) x the standard error of the median, taken from the bootstrap interval).
The dialog shows the split medians and "Smallest difference it can show" for the total; the log line carries all
of them (`[slipstream-latency-test] ... totalDetectable=`). The figure is unstable at small counts (reviewer: with
sd 26 ms and 12 presses it ranged 12-73 ms over 40 simulated runs); it means something from about 30 presses on.

### (d) Paired, interleaved design for the kit

Sessions differ (Wi-Fi conditions, console state, the scene), so a run is a block, not a set of independent
presses:

1. Interleave arms: A B B A A B B A (four starts each), same scene, same `[slipstream-config]` apart from the
   switch under test (the kit refuses otherwise).
2. In each start: `settle:20` then one latency test of 100-200 presses (D-pad) or 100 (PS), then `exit`.
3. Compare **block medians** between arms (8 blocks: a permutation test or Mann-Whitney on the block medians, and
   the Hodges-Lehmann shift with its interval), for `pcP50` and `consoleP50` separately. Per-press pooling across
   starts overstates precision when blocks differ.
4. Report the detectable difference the design achieved; "no difference" is only a statement down to that size.
5. Wi-Fi runs (`route=wifi`) are diagnostic only.

### (e) Which button

- **`ps`** (default, unchanged): the control centre. Always available, in any game, but the console takes ~557 ms
  with sd 26 ms; only for totals and as a continuity check with the original build.
- **`dpad-right-left`**: right, then left on the next press, so a highlight moves and comes back; nothing to close.
  Use it on a still menu (the PS5 home screen or a settings list), not in a game, where the D-pad may change
  weapons or open wheels. The answer is detected anywhere in the picture: the mean difference over the whole
  picture, or at least 0.15 % of the 320 x 180 luma cells changing by more than 0.1 (a highlight's outline), above
  three times the motion seen before the press (`Detector::Mode::Anywhere`, rules in "Moving pictures" below). A
  menu highlight usually moves on the console's next UI frame without an animation, so this is expected to answer
  within a few frames with a much smaller console spread than the control centre: **expected, not measured**.
- **`options`**: OPTIONS opens the game's own pause menu and OPTIONS again closes it. Game-dependent (some games
  fade the menu in, some pause first); detected anywhere like the D-pad. Press it from a **still** picture (a menu,
  a paused or static scene): in moving gameplay the press cannot be measured (below).

That D-pad and OPTIONS answer faster than the PS button is expected, not measured. Which button answers fastest
and with the least spread must be measured on the console: run 50 presses of each and compare `consoleP50` and
the console part's interval width.

### (f) Moving pictures: "not measurable" instead of a wrong time (review B1)

The first version of the Anywhere detector compared each frame after the press with the last frame before it, but
learnt the motion level from frame-to-frame change. Against a fixed frame, motion adds up: a 6 x 12-cell object
walking over a still picture read as an answer after 3 frames with no press at all. Since the review
(`latency::Detector::judge`, Anywhere mode only; the PS button's detector is unchanged):

1. The motion level is learnt the way the answer is judged: the change of every frame before the press against
   the **first** frame of that window (mean difference and changed share), and it is scaled to how many frames after
   the press a frame is judged (motion that went on for longer is allowed proportionally more).
2. **Still enough to tell**: at least 4 frames before the press, and that change stays under two thirds of the
   answer floors (mean 0.027, 0.10 % of the cells), so motion never raises a floor more than 2x. Otherwise the
   press is **not measurable: picture moving**.
3. An answer is **confirmed by a jump**: the frame also differs that much from the frame 4 frames (67 ms at 60 fps)
   before it, so slow drifts (a character starting to walk, a pan, a fade) never add up to one. Since the second
   review the confirmation does not set the time: the answer is timed at its **onset**, the first frame from which
   the change against the frame before the press stayed above the threshold (2(j)). A change whose jump has not
   confirmed within 12 frames (200 ms) of that onset is too slow: not found (logged as no answer, never as a time).
4. A change sooner than **one stream frame** after the press cannot be the console's answer: not measurable.

A press that is not measurable has no time: `[push-latency] trial N not measurable reason=picture-moving`,
`[slipstream-press] ... outcome=picture-moving` with every time -1; it is left out of every statistic and counted
(`unmeasurable=` in `[slipstream-latency-test]`, status key `unmeasurable`). When at least 6 presses were judged and
most of them were not measurable, the test stops (`[push-latency] stopped reason=picture-moving ...`, state
failed, status `stopReason=picture-moving`); the `trials answerMs=[...]` line is still written. Tests (Linux
model, synthetic pictures): `tests/unit/PushInputLatencyTests.cpp` (the reviewer's walking object and pans at 8
speeds never answer; a menu on a still picture is found on its exact frame; grain, fades, a blinking cursor) and
`LatencyStatsTests.inc` `latencyStatsPictureMovingPresses`.

### (g) Automation rules (review B2, S1, S2, S3, S7)

- Steps are numbered from **1** in `[slipstream-autorun] step=<i>`; `step=0` is the connection of
  `VEYRA_TEST_RP_AUTOCONNECT`.
- A plan that does not parse **fails closed**: on the first tick its first bad step is logged as `state=failed`
  (no `start` line) and `finished ok=0`; nothing runs (no wait for a stream, no press, and AUTOCONNECT does not
  connect: `step=0 name=connect state=failed reason=invalid-plan`). The app closes only when the plan text holds a
  well-formed `exit` step (`exit` exactly; `exit:1` is not one).
- Once the stream was seen, the plan is abandoned when that session ends or is replaced (disconnect, reconnect,
  the user's Stop) or presents nothing for 10 s: the current step fails with `reason=stream-lost` (a running
  latency test is cancelled, a hook step gets Cancel). A latency test the user cancels fails its step at once with
  `reason=cancelled`. A plan never resumes pressing on a later session.
- Closing the app while a plan runs (`reason=closing`) also releases the button (the latency test's cancel).
- No console registered (AUTOCONNECT or AUTORUN set): `step=0 name=connect state=failed reason=no-console` (with
  AUTOCONNECT) and `finished ok=0 steps=<n> reason=no-console`, once, at start; nothing else happens, not even
  `exit`.
- `slipstream.ini` cannot set `VEYRA_TEST_RP_AUTORUN` or `VEYRA_TEST_RP_AUTOCONNECT` (reported as unknown keys):
  only the real environment can, so a line left in the file never connects or presses at every start.
- `[slipstream-config]`: at a session's start and on a change, at most one line per 5 s; `fgActive`, `srActive`
  and `window` alone never cause a line (they move by themselves). `refreshHz=-1` while the refresh is unknown.

### (h) The alternative buttons are an experiment (second review)

OPTIONS and the D-pad (`Detector::Mode::Anywhere`) are offered only with `VEYRA_EXP_RP_LATENCY_BUTTONS=1`. The
second independent review did **not accept** the Anywhere detector as of this build: on a picture that was still
before the press, a change that begins after it by itself (an object starting to walk, a tile pulsing every
0.5-1 s) was timed as the answer, the 4-frame jump rule timed slow fades late, and noise was scaled as if it
accumulated, so late answers under film grain were lost more often than early ones (the median leaned fast).
(i)-(k) below are the answers; they are tested on synthetic pictures on Linux only. Without the switch the bridge
refuses such a test (`[push-latency] refused reason=buttons-experimental button=<b>`), a plan step asking for one
fails with `reason=buttons-experimental` before anything is pressed, and the status map's `buttonsAvailable` is
false. The PS button, at any number of presses up to 500, needs no switch.

### (i) Sham presses: is the picture changing by itself? (second review, blocker 1)

The detector cannot tell a change the press caused from one that began by itself just after it. With OPTIONS / the
D-pad the test therefore interleaves **sham presses**: max(14, presses / 4) slots, in random order among the real
ones (`latencystats::shamSchedule`), with the same random gap and waits, in which nothing is pressed; the detector
judges each exactly as a press. Shams are never in a latency statistic. The test is **valid** only when the judged
shams' answers are at most `shamAnswersAllowed(n)`: the largest k with P(Binomial(n, 0.2) <= k) < 5 %, so a scene
that answers 20 % of shams by itself is accepted less than 5 % of the time. No such k exists under 14 shams
(0.8^13 = 5.5 %; 5 shams would accept such a scene 33 % of the time), hence the minimum of 14: n = 14 or 20
allows 0, 25 allows 1, 30 allows 2, 125 (500 presses) allows 17. A sham whose picture was not still before it
cannot be judged and does not count in n. A test that is not valid reports **no time** ("not valid: the picture
changes by itself", reason `picture-changes-by-itself`; state failed). Model results (Linux, synthetic pictures,
`tests/unit/PushInputLatencyTests.cpp`): a still menu passes with 0 sham answers and every press found (336/336);
the reviewer's object that starts walking at the press (1 and 3 cells per frame) answers 14 of 14 shams; the
pulsing tiles of 128-256 cells (every 250 ms, 500 ms, 1 s) end not valid at every size, period and seed. Not done:
the reviewer's alternative check that the changed region is the same across presses.

### (j) The time is the onset, not the confirmation (second review, blocker 2)

The jump against the frame 4 frames earlier only confirms an answer. The time reported is the first frame from
which the change against the frame before the press stayed above the threshold (`Detector::onsetFramesBack`; the
probe keeps the last 12 judged frames' times). Not confirmed within 12 frames of that onset: not found, never a
time. Model results (the reviewers' det2 / det3 shapes, `tests/unit/PushInputLatencyTests.cpp`): over 540 cases
(6 shapes, 1-30 frames, starting 2-40 frames after the press, two panel sizes) the reported frame equals the
threshold onset in every case found (0 frames off; 420 found). The **residual bias** is the threshold's own: how
long a shape takes to change enough to cross it, measured from the first frame of the animation (panel 100 x 120
cells, still picture; "none" = too slow, not found):

| Shape (amplitude) | 1 fr | 4 fr | 6 fr | 8 fr | 10 fr | 12 fr | 15 fr | 20 fr | 30 fr |
|---|---|---|---|---|---|---|---|---|---|
| step / slide-in | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| ease-out fade (0.3) | 0 | 0 | +1 | +1 | +1 | +2 | +2 | +3 | none |
| smoothstep fade (0.3) | 0 | +1 | +2 | +3 | +3 | +4 | +5 | none | none |
| linear fade (0.3) | 0 | +1 | +1 | +2 | +3 | +3 | none | none | none |
| linear fade (0.15) | 0 | +2 | +3 | none | none | none | none | none | none |
| ease-in fade (0.3) | 0 | +2 | +3 | +4 | +5 | +6 | +8 | +11 | none |

(frames at 60 fps; +11 frames = 183 ms). A slow fade is therefore timed late by the time it takes to become
visible, the same for every arm of an A/B comparison on the same menu; absolute times of fading menus are not
meaningful. The PS button's detector is unchanged.

### (k) Noise is not scaled with elapsed time (second review, statistics bias)

The allowance for change against the frame before the press is 3 x (frame-to-frame noise + drift x elapsed /
learnt span); only the drift (what the change against the first frame added beyond the noise) grows with time.
Before, noise was scaled too, so under film grain late answers were lost ("no answer") and the median leaned fast.
Model result: Gaussian grain sd 0.02 and a 30 x 15-cell panel at frame 6, 33 or 60: found 20/20/20, each on its
frame (before the fix 20/20/0). The reviewer's no-press scenes give the same false-answer counts as before.

### (l) What the Anywhere detector can and cannot measure

From the reviewers' harnesses (synthetic pictures, Linux): **measurable**: a still menu or settings list; slow
waves and pans up to 0.05 cells per frame; grain up to sd 0.02; a small blinking cursor; a clock digit changing
once a second. **Never measurable** (presses reported not measurable, or the test not valid): pans of 0.2 cells
per frame or faster, grain sd 0.03, particles, an 8 x 16 block cursor blinking at 2 Hz, anything that starts
changing by itself after a still moment (caught by the sham presses). In practice: use the D-pad on the PS5 home
screen or a settings list, never in gameplay.

**The accepted instruments** are the PS-button test (Bottom detector, any number of presses) and the in-app split
(`[slipstream-press]` / `[slipstream-latency-test]`, `[slipstream-latency]`). The alternative-button test stays an
experiment behind `VEYRA_EXP_RP_LATENCY_BUTTONS=1` until an independent review accepts (h)-(k).

## 3. What remains unmeasurable in software

- The display's own processing and pixel response (panel response, overdrive, the scanout position of the
  changed region), and the time from scanout to light.
- The hand and the physical controller (here the press is injected in software; with a real pad, its polling and
  Bluetooth/USB transport add to the console part).
- The console's internal time from input to the frame it encodes is inside `sendToArrival` together with the
  network; software on the PC cannot separate them.

A camera (240 fps or faster) or a light sensor on the screen with a button-press trigger remains the only physical
proof of input-to-photon latency. The software split above is for comparing PC-side changes precisely, not for
absolute numbers.

## 4. What a real session must show

- `[slipstream-config]` once per session with every key, `trialsMax=500`, a sensible `route`.
- With `VEYRA_EXP_RP_STATS=1`: one `[slipstream-press]` per answered press with `sendToArrivalMs` > 0 and
  `arrivalToPresentMs` in the same range as `pushfpsMs` (a few to ~15 ms; it stops before Present, so it misses
  the swap-chain queue and scan-out); `phaseMs` spread over 0-16.7 ms; at the
  end one `[slipstream-latency-test]` whose `pcBasis=present`.
- A press whose arrival is -1 means the answering frame's timestamp was not found among the recorded arrivals
  (for example a source without arrival times); the total is still logged.
- The existing `[push-latency] done ...` and `trials answerMs=[...]` lines unchanged.
