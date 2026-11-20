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
`(1.96 + 0.84) x sqrt(2) x 1.2533 x sd / sqrt(n)` (median of n presses): about 37 ms for 12 presses, 5.8 ms for
500 presses, and 5 ms needs about 670 presses per arm (arithmetic, `pressesForDifference`). Total press-to-picture
alone cannot reach a few ms in a reasonable time.

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
pipeline: decode, hand-off, enhancement, frame generation, the present queue. It has a small spread (the
continuous counter `pushfpsMs` had sd 0.09-1.1 ms per run in BENCH1) and it is what our work changes.
`arrivalToScanoutMs` is the same up to the moment the frame reached the screen; the scanout time per frame
belongs to the presenter (stream "present"), so this build logs -1 there and the PC share uses Present
(`pcBasis=present`). When the presenter exposes per-frame scanout, `PushLatencyStats::split` already takes it.

For a PC-side A/B, compare `pcP50` (and, much better, the continuous `[slipstream-latency]` arrival-to-scanout
percentiles over every frame, thousands per minute) instead of the total. With sd ~1 ms, 100 presses resolve
about 0.5 ms (arithmetic).

### (b) Many more presses, with the press phase dithered

- Up to **500 presses per test** in this build (`latencystats::kTrialsMax`; the original build accepts 20 in the
  bridge and offers 12 in the dialog). The dialog offers 5, 8, 12, 50, 200, 500; the default stays 8.
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
of them (`[slipstream-latency-test] ... totalDetectable=`).

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
  three times the motion seen before the press (`Detector::Mode::Anywhere`). A menu highlight usually moves on the
  console's next UI frame without an animation, so this is expected to answer within a few frames with a much
  smaller console spread than the control centre; expected, not measured.
- **`options`**: OPTIONS opens the game's own pause menu and OPTIONS again closes it. Game-dependent (some games
  fade the menu in, some pause first); detected anywhere like the D-pad.

Which button answers fastest and with the least spread must be measured on the console: run 50 presses of each
and compare `consoleP50` and the console part's interval width.

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
  `arrivalToPresentMs` in the same range as `pushfpsMs` (a few to ~15 ms); `phaseMs` spread over 0-16.7 ms; at the
  end one `[slipstream-latency-test]` whose `pcBasis=present`.
- A press whose arrival is -1 means the answering frame's timestamp was not found among the recorded arrivals
  (for example a source without arrival times); the total is still logged.
- The existing `[push-latency] done ...` and `trials answerMs=[...]` lines unchanged.
