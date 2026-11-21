# SmartLink — adaptive networking for PS5 Remote Play (experiment, off by default)

Status: Linux model and unit tests only. **Nothing here has run on Windows, on a PS5 or on a real Wi-Fi link.**
Every table below is a model result from the Linux simulator, not a measurement, and no claim of improvement on
real hardware is made.

## What SmartLink is, in plain words

When you play over Remote Play, the console sends the picture as a stream of small network packets. On a cable they
arrive evenly. On Wi-Fi they sometimes stop for 20-80 ms and then come in a rush, a few go missing, and some arrive in
the wrong order. The app has a few dials that decide how it copes with that:

- the **reorder wait**: how long the app waits for a missing packet before it gives up on it (2, 4, 8 or 16 ms);
- the **audio reserve**: how much sound is kept queued so that a short pause in the stream is not heard as a pop;
- what it tells the **latency governor** (the part that lowers graphics settings when the PC is too slow): whether the
  stutter is the network's fault, in which case lowering graphics would not help;
- a **suggested starting quality (bitrate)** for the next session.

SmartLink watches the stream every 200 ms and turns those dials for you. On a clean connection it does nothing at
all (every output is "leave as configured"), so a cable connection keeps the lowest latency. On a troubled
connection it widens the waits only when the stream shows that a longer wait would have saved frames, keeps a bigger
audio reserve while the trouble lasts, and brings everything back down afterwards. It never changes Windows, the
adapter or the router: it only gives plain-language advice ("A network cable would make the stream steadier").

Switches (all off by default; with them unset the app behaves exactly as `exp/rc4`):

| switch | values | effect |
|---|---|---|
| `VEYRA_EXP_RP_SMARTLINK` (or `slipstream.ini` `smartlink=`) | unset/0/off = off; `1`/`observe` = compute and log only; `apply` = apply the supported outputs | `parseMode` in `SmartLinkTypes.h` |
| `VEYRA_EXP_RP_SMARTLINK_MODEL` | unset/`stat` = statistics (default); exactly `learned` = the learned comparison model | `parseModel`; never the default (see the verdict below) |

One exception to "observe = compute and log only" (found by the final review, kept on purpose): if the separate
experimental latency governor or GPU budget is ALSO set to `apply` (`VEYRA_EXP_RP_GOVERNOR=apply`,
`VEYRA_EXP_RP_GPU_BUDGET=apply`, neither accepted by review), they read SmartLink's link state in observe mode
too, as information: `networkLimited` holds back their reductions and `linkHeadroom` lets them return after half a
hold. So do not combine those switches with SmartLink in one comparison. With them unset, observe changes nothing.

A stream that applied SmartLink's reorder wait clears the library's live wait when it ends
(`src/source/RemotePlaySmartLink.h`, destructor), so the next stream starts from its own configured wait even if
SmartLink is switched off in between (final review, blocker 1; blind Windows code).

Files: `include/veyra/remoteplay/SmartLinkTypes.h` (types, switch parsing, log line), `SmartLinkPredictor.h`,
`SmartLinkModel.h` (generated weights), `SmartLinkProfile.h`, `SmartLinkController.h` (`Engine`), `SmartLinkTrace.h`
(trace CSV), `SmartLinkSim.h` (Linux model); `tools/smartlink_sim/` (simulator, study, replay, `results.json`,
`study.json`); tests `tests/remoteplay/SmartLinkTests.inc`, `SmartLinkSimTests.inc`. Background research:
`docs/SMARTLINK_RESEARCH.md` (cited below by section).

## The signals (`Sample`, one per ~200 ms window)

Packets received / lost / reordered / late (late = arrived after the reorder wait gave it up), the gaps between
packets, complete / given-up / FEC-rescued frames, the largest gap between complete frames and its excess over the
frame interval, `lateRescueMs` (the smallest extra wait that would have saved every frame given up in the window),
the wait in effect, the console's own round trip and bitrate target (takion CONNECTIONQUALITY, research doc A3), the
measured and requested rates, decoder queue depth, audio reserve and underruns, Wi-Fi signal / RSSI / link rate /
band, and `frameLateMeanMs` (frame arrival against the nominal cadence; only its trend is used). Unknown values are
negative and never guessed. Which of them the Windows app can fill today is the "net" stream's business; SmartLink
copes with any of them missing (tested with all-unknown samples).

## How it predicts, and what it cannot foresee

No signal a PC can see gives seconds of warning of one single Wi-Fi drought: the cause sits in the radio's medium
access and the router's queue (research doc F: BLADE, Zhuge; I3). What can be recognised is the **regime** the link
is in, and SmartLink keeps a margin while that regime lasts. `SmartLinkPredictor.h` (O(1) per sample, no
allocation; about 2.5-3.4 microseconds per sample on this Linux machine, i.e. well under 0.01 % of one core at five
samples a second):

- **hazard**: how often stalls (a frame gap more than 25 ms beyond the interval) happened, over about a minute and
  over a few seconds (Wi-Fi trouble comes in clusters);
- **periodic spikes**: a period check of the last 12 spike times (the interval most others are a whole multiple of,
  within 12 %); the risk is raised from shortly before the next expected spike until shortly after; the pattern is
  dropped after four expected spikes that did not come (the "autocorrelation/period check" of research doc I3);
- **queue building**: a least-squares trendline slope of the frames' arrival lateness against an adaptive threshold
  (the GCC delay-trend idea, re-implemented from draft-ietf-rmcat-gcc-02 section 5; research doc E), plus one-sided
  CUSUM change detectors on the console round trip and on the frame-gap excess;
- **loss rate**.

They are combined into `stallRisk` = chance of a stall (gap excess above 25 ms) within the next 2 s, and a `Regime`
(Stable, PeriodicSpikes, Congested, WeakSignal, Interrupted). The Wi-Fi signal is deliberately not part of the risk
(research doc I2 #6): it only shapes the Regime (WeakSignal also needs trouble in the stream) and the advice.

**What the risk number means (simulator only):** it is over-confident. Windows with a risk of 0.5-0.6 were followed
by a stall within 2 s in 21 % of cases on the families it was tuned on (held-out seeds) and in 9 % on families it
had never seen; 0.0-0.1 means about 0.5-3 %. Treat it as a ranking ("more likely than usual"), not a probability.
The controller only uses it at two thresholds (0.5: keep a margin; 0.3: allowed to relax).

Constants: T = 25 ms (2.5 intervals shown instead of one at 60 fps: a visible hitch; the stutter metric below counts
the milder > 1.5 intervals); H = 2 s (long enough for an action to take effect: the reorder wait is applied at the
library's next look, the audio reserve grows at most 5 ms/s; short enough to mean "soon"); the other constants are
commented in the header with their source.

## Profiles and advice (`SmartLinkProfile.h`)

Wired whenever the route is wired. On Wi-Fi: **Unstable** when in the last minute there were 3 or more stall
clusters, loss above 0.5 %, a signal below 40 %, or periodic spikes at 2 or more a minute; otherwise **Strong**.
Hysteresis with two thresholds (the shape of Moonlight's connection-status rule, research doc C): "unstable" must
hold 10 s to switch to it, "strong" 60 s to switch back (tested: no flapping over 2 h of toggling input). BENCH1's
Wi-Fi session (software counters, one session) had about 1 % of seconds with 4+ frames missing, about one every
100 s: that link would stay **Strong**. Advice ids (`Advice`), at most two at a time: link is fine; use a cable; move
closer / use the 5 GHz network; something pauses Wi-Fi every few seconds (periodic spikes); start lower next time;
your connection can carry more next time. Advice never changes a setting.

## The decisions and their safety limits (`SmartLinkController.h`, `Engine::update`)

| output | rule | safety limit |
|---|---|---|
| `reorderWindowMs` (0 = **restore the configured wait**, else 2/4/8/16) | **widen** in the same window that shows frames a longer wait would have saved (`lateRescueMs`), to the smallest step >= the wait IN EFFECT during that window (`Sample::reorderWaitMs`, which lags a new decision by ~250 ms) + rescue; **trim** below the configured wait only when packets are lost (not late) in 2+ windows within 10 s and nothing was late for a minute (a lost packet holds every later one for the whole wait: pure delay); **relax** one step toward the configured wait after 30 s without evidence and risk < 0.5; with **frequent reordering** (>= 1 % of packets out of order, ~10 s average) it never widens and returns to the configured wait | never wider than the evidence asks, also when the library applies a change 1-2 windows late (tested with 0, 1, 2 windows of lag); never widens on a clean link; at most 4 changes a minute; a relax undone by a widening or a trim within a minute doubles the next dwell (up to 8x), cleared after a quiet minute, so loss pairs every 35 s make at most 12 changes an hour (was 203; tested); back to the configured wait at most 150 s after the last evidence |
| `audioTargetMs` (0 = **restore the sink's own target**, else reserve ms) | **raise** after a real underrun to the floor (20 ms) plus the recent large gap excess (10 ms steps); in the periodic-spike regime, with a link stall in the last 20 s, also **before** the next expected spike, but such a forecast-only raise lasts 3 s unless an underrun confirms it; **decay** 10 ms every 5 s after 10 s without underrun | at most **50 ms** in all (was 100): the reserve delays sound against the picture, EBU R37 allows sound at most 60 ms behind the picture end to end, and ~10 ms is left for the output device period; gaps longer than any reserve add only one 20 ms step; at most 12 changes a minute; sustained stalls (excess 20-80 ms every 2-8 s, an underrun with every second one) for 12 h: reserve at most 50 ms, mean 38.0 ms, worst hour 38.6 ms, first / last hour 38.2 / 37.8 ms (was 93 ms mean for an hour with the 100 ms cap); the reviewer's 12 h of periodic disturbances: mean 22.4 ms, worst hour 22.5 ms; no ratchet (tested: `smartlink_audio_reserve_capped_and_no_ratchet`) |
| `networkLimited` | 2+ **link** stalls within 20 s, or an interruption; held 10 s. A link stall: a frame gap >= 10 ms above the observed cadence (the second highest frame count of the last ~5 s) whose frames arrived late, i.e. the gap window and the next one together reach the cadence's frame count, or packets were lost / frames given up. A window one frame short with nothing lost is a frame the console skipped, not the link | never while the decoder queue was >= 3 frames in the last second; `packetGapMaxMs` is not needed (the app does not fill it); a 30 fps game with a dropped frame every 8 s and a 57 fps console cadence are not network-limited; the governor case and stalls of only 20-24 ms are (tested) |
| `linkHeadroom` | 120 s without stall, loss or late packet while the link **delivered** >= 80 % of the request (`measuredKbps`) and the console's target, when known, stayed >= 95 % of it | false on any trouble; a calm picture (25-60 % of the request) never gives headroom: a console target alone proves nothing |
| `suggestedStartKbps` (next session) | below the request when the console held its target under 85 % of it for a minute **and** the link lost packets or stalled in that minute (its own target, rounded down to 500), or loss stayed above 1 % for a minute (75 %); above it only after 5 minutes of headroom | a low rate alone gives no suggestion and no "start lower" advice (a calm menu on wired or Wi-Fi: none, tested); a raise is at most 1.25 x the request and never above 50 Mbit/s. GCC's loss reference points (< 2 % fine, > 10 % too high; research doc E, I2 #5) are an idea for the PS5, not validated; there is no live bitrate control in the protocol (research doc A1), so this is only a suggestion the user accepts |
| `reasons` | bit set (`reason::` in `SmartLinkTypes.h`) saying why; in observe mode this is the "what would change" explanation | - |

Differences from the research doc's recommendation I2 #1 ("the smallest step that catches ~99 % of late packets"):
SmartLink widens on frames that would have been saved, not on late packets alone, because late packets that FEC
covers cost nothing and the sprint-3 study (`ReorderWindow.h`) found that widening on lateness alone kept waits
up for no gain.

## Statistics against machine learning (directive section 1)

A logistic regression and a small boosted forest (30 trees of depth 2) over the **same 10 features** were trained
offline (`smartlink_sim --study`) on simulated windows of 8 scenario families (seeds 1-3, SmartLink observing the
`slipstream_rc4` policy), then evaluated on held-out seeds of those families and on 5 families never seen in
training (periodic spikes, the governor case, bandwidth drop, interruptions, reordering + loss). Only the logistic weights are shipped
(`SmartLinkModel.h`, generated), and only behind `VEYRA_EXP_RP_SMARTLINK_MODEL=learned`.

Model result (Linux simulator), not a measurement

| evaluation set | model | AUC | hit rate at 5 % false alarms (actual) | hit rate at 10 % false alarms (actual) | stall onsets alarmed in the window before | median lead (ms) |
|---|---|---|---|---|---|---|
| training (seen families, seeds 1..N) | statistics | 0.898 | 60.0 % (5.0 %) | 75.2 % (10.0 %) | 75.3 % of 162 | 2000 |
| training (seen families, seeds 1..N) | logistic | 0.948 | 76.4 % (5.0 %) | 81.3 % (10.0 %) | 81.5 % of 162 | 2000 |
| training (seen families, seeds 1..N) | stump forest (30 x depth 2) | 0.973 | 89.8 % (5.0 %) | 95.8 % (9.9 %) | 97.5 % of 162 | 2000 |
| held-out seeds, seen families | statistics | 0.919 | 61.2 % (5.0 %) | 77.3 % (10.0 %) | 75.2 % of 161 | 2000 |
| held-out seeds, seen families | logistic | 0.945 | 78.1 % (5.0 %) | 82.7 % (10.0 %) | 83.9 % of 161 | 2000 |
| held-out seeds, seen families | stump forest (30 x depth 2) | 0.962 | 80.6 % (4.8 %) | 95.3 % (9.6 %) | 97.5 % of 161 | 2000 |
| unseen families (periodic, governor case, bandwidth drop, interruptions, reordering + loss) | statistics | 0.758 | 30.7 % (5.0 %) | 39.4 % (10.0 %) | 32.7 % of 104 | 2000 |
| unseen families (periodic, governor case, bandwidth drop, interruptions, reordering + loss) | logistic | 0.749 | 15.5 % (5.0 %) | 22.3 % (10.0 %) | 14.4 % of 104 | 2000 |
| unseen families (periodic, governor case, bandwidth drop, interruptions, reordering + loss) | stump forest (30 x depth 2) | 0.803 | 14.8 % (4.9 %) | 39.8 % (9.9 %) | 36.5 % of 104 | 2000 |

Calibration of the statistics' stallRisk (unseen families; share of windows followed by a stall within 2 s):

| risk | windows | stall share |
|---|---|---|
| 0.0-0.1 | 11621 | 2.3 % |
| 0.1-0.2 | 2076 | 8.8 % |
| 0.2-0.3 | 1613 | 13.5 % |
| 0.3-0.4 | 716 | 8.5 % |
| 0.4-0.5 | 270 | 19.3 % |
| 0.5-0.6 | 801 | 11.5 % |
| 0.6-0.7 | 187 | 21.9 % |
| 0.7-0.8 | 245 | 42.0 % |
| 0.8-0.9 | 212 | 21.2 % |
| 0.9-1.0 | 259 | 41.3 % |

**Verdict: no measurable benefit where it matters, so the statistics stay the default.** On held-out seeds of the
families it was trained on, the learned models rank windows better (AUC 0.945 logistic / 0.962 forest against
0.919; hit rate at 5 % false alarms 78 % / 81 % against 61 %). On families they had never seen they do not
generalise: the logistic model's hit rate at 5 % false alarms falls to 16 % against the statistics' 31 %, and it
alarms before only 14 % of stall onsets against 33 %; the forest is no better than the statistics at 10 % false
alarms (40 % against 39 %) and worse at 5 %. When the controller uses the learned risk (`smartlink_apply_learned`
against `smartlink_apply_lan` in the tables), the reorder decisions are identical and the audio decisions trade
2-11 ms less average reserve for slightly more audio gaps (e.g. weak signal 1.08 against 0.33 gaps/min, governor case
1.08 against 0.83): no measurable improvement of decision quality. Cost per sample on this Linux machine (shared
2-core VM, nice 10; not the owner's PC): about 2.5 microseconds for either predictor and 3.4 for `Engine::update` as
a whole, i.e. the model costs nothing worth mentioning, but it buys nothing either. These are simulator results:
the decisive test, as research doc I3 says, would be recorded traces from real links (replay below).

"Median lead" is capped at 2000 ms by the look-back of the evaluation (10 windows): the risk is raised for whole
regimes, not for single spikes, as described above.


## Simulator (`tools/smartlink_sim`, `include/veyra/remoteplay/SmartLinkSim.h`)

A deterministic, seeded model in virtual time: the stream of `ReorderStudy.h` (60 fps, 24 source + 3 FEC packets
per frame), a FIFO path that can wander in delay, stall, pass a bottleneck, lose and displace packets; the library's
reorder queue exactly as `ReorderQueueModel.h` restates it; frame completion and hand-over; keyframe requests spaced
500 ms as PushFPS does; a simplified audio reserve model (docs/AUDIO.md findings 2, 4 and 6: the 2 s lower envelope
of arrivals as the reference, 20 ms floor, 30 ms starvation rule, 60 ms hard reset, excess decaying at 5 ms/s under
today's rule or cut at once under the fast rule, reserve growing at most 5 ms/s, lateness under 2 ms absorbed by the
device period, frames more than 100 ms late discarded as AudioIngress does). `AudioPathModel.h` is the complete
audio model, but it has no input for an external reserve target and this work may not change it, so SmartLink's
audio decision is judged in the simpler model. Not modelled: decoding and presentation, the console's encoder and
its own rate adaptation (except a lowered target in `bandwidth_drop`), keyframe sizes, Wi-Fi itself.

**Apply lag.** SmartLink's decisions take effect late, as in the app: the library applies a new reorder wait at its
next look (every 250 ms), so a decision made at the end of a window is applied at the first look or window boundary
at least `RunOptions::applyLagUs` later (default 250 ms, i.e. one to two 200 ms windows; `smartlink_sim --lag-ms L`
changes it, 0 = at once). The model before the review applied decisions at once and so hid blocker 1. The tables
below are with the default lag; with 0 and 400 ms the acceptance checks also pass.

Policies (where each constant comes from):

| policy | reorder wait | audio |
|---|---|---|
| `original_pushfps` | 16 ms fixed: the library default (`TransportTuning::kLibraryReorderUs`, chiaki-ng's takion reorder timeout) | today's rule, 20 ms floor (docs/AUDIO.md) |
| `slipstream_rc4` | the same: with every switch unset nothing on this path differs (so its rows equal the original's) | the same |
| `slipstream_rc4_switches` | `VEYRA_EXP_RP_LAN_PROFILE=1` (2 ms) + `VEYRA_EXP_RP_REORDER_ADAPTIVE=1` (`ReorderWindow.h`, fed the model's per-frame evidence, which the app does not count yet) | `VEYRA_EXP_RP_AUDIO_FAST_RECOVER=1` (fast rule) |
| `smartlink_apply` | SmartLink on top of `slipstream_rc4` (configured 16 ms) | fast rule with SmartLink's target |
| `smartlink_apply_lan` | SmartLink with the LAN profile (configured 2 ms) | fast rule with SmartLink's target |
| `smartlink_apply_learned` | as `smartlink_apply_lan`, risk from the learned model | as above |
| `early_loss_lan` / `early_loss_16` | 2 ms / 16 ms plus "early loss": a frame missing more units than its FEC units is given up at once while no out-of-order packet was seen for 300 s (Moonlight's speculative RFI, research doc C) | today's rule |

Metrics: added delay = hand-over minus the moment the frame was complete at the socket (the wait the policy adds:
the input latency indicator), percentiles over all delivered frames; stutter = a gap between delivered frames above
1.5 intervals; recovery requests = keyframe requests after given-up frames; audio gaps and the mean / P95 audio
reserve beyond the arrival envelope; time at each wait; setting changes (reorder and audio) and the most reorder-wait changes in any
minute; "back to leave" = seconds after the disturbance window (30-120 s) until every output is "leave".
Run time: 13 scenarios x 8 policies x 3 seeds in about 20 s (single thread, nice 10).

### What the tables say (model results)

All with SmartLink's decisions applied 250 ms late (see "Apply lag").

- **Wired, clean:** every policy identical; SmartLink adds exactly 0 ms and makes 0 changes (also a unit test).
- **Waiting 16 ms for packets that never come is the largest avoidable cost in the model.** With random loss
  (`wired_rare_loss`, `weak_signal`) the 16 ms wait of `original_pushfps` / `slipstream_rc4` turns each lost packet
  into a visible hitch: 10 and 144 stutters a minute against 0 and 7.75 at 2 ms. SmartLink on the 16 ms configuration
  trims the wait (0.42 and 9.67 a minute) without losing frames; with the LAN profile it stays at 2 ms. That gain is
  the 2 ms LAN profile's, not SmartLink's: from 2 ms SmartLink equals the existing switches there.
- **Reordering (5 % of packets 1-12 ms late):** this is the "frequent reordering" regime, where SmartLink no longer
  widens. From 2 ms it now has 13.6 stutters a minute and P95 / P99 added delay 3.3 / 5.0 ms, against 38.2 and
  6.2 / 9.8 ms for the existing switches, but it delivers 31 frames per run fewer (14338 against 14369; FEC completes
  most frames at 2 ms). Before this rule it widened to 16 ms: 71 stutters a minute, P95 8.3 ms.
- **Reordering + loss (2 % late 1-12 ms, 0.2 % lost; the reviewer's variant, now a permanent scenario):** from 2 ms
  SmartLink 2.75 stutters a minute, P99 3.85 ms, against 2.33 and 4.50 ms for the existing switches, one frame
  fewer per run (before the rule: 70 stutters a minute and P99 12 ms).
- **Wi-Fi droughts (random, periodic, governor case):** no receive-side policy changes video stutter: the pictures
  are late on the path. SmartLink's audio reserve cuts audio gaps from about 3-4 a minute to about 1.3-2.8 (2.3 under
  the weak signal against 5.6 for the fast rule alone), at about 5-7 ms more average audio reserve while the trouble
  lasts; it returns to the floor afterwards (back to leave within 30 s). `networkLimited` is set throughout the
  governor case.
- **Interruptions:** nothing helps the 0.2-5 s outages; SmartLink does not inflate the reserve for them.
- **Early loss:** gives up a frame sooner (P95 time to give up 15.0 ms against 26.3 ms at 16 ms in `burst_loss`), with
  the same frames delivered and requests; it does not fire once reordering has been seen. A small effect at 2 ms.
- **Bounded:** at most 3 reorder-wait changes in any minute in every scenario (the controller allows 4); audio-target changes are bounded at 12
  a minute by the controller and counted in the 'setting changes' column; every output back to "leave" within the
  stated 150 s (longest: 90 s, `burst_loss` / `weak_signal` on the 16 ms configuration).

### Where SmartLink is worse in the model

Model results, all of them; "the existing switches" = `slipstream_rc4_switches` (LAN profile 2 ms + adaptive reorder
wait + fast audio rule).

| scenario | SmartLink apply | better policy | what is worse |
|---|---|---|---|
| `congestion` | 2.50 audio gaps a minute, reserve mean 28.2 ms (both configurations) | existing switches: 1.25 gaps a minute, 22.9 ms | twice the audio gaps with 5 ms more reserve: a loss, not "no gain". SmartLink's raise after an underrun follows a queue that grows faster than a reserve can (the sink stretches at most 5 ms/s). Video is equal. Cause not fixed in this round |
| `rtt_variation` | 0.42 audio gaps a minute | `slipstream_rc4` (today's audio rule): 0.25 | the fast audio rule's cost (the existing switches have 0.42 too), not a SmartLink decision |
| `reordering` | from 2 ms: 14338 frames delivered | existing switches: 14369 (fixed 16 ms: 14384) | 31 fewer frames per run, for 13.6 instead of 38.2 stutters a minute and less added delay (P95 3.3 against 6.2 ms) |
| `reordering_loss` | from 2 ms: 2.75 stutters a minute, 14379 frames | existing switches: 2.33, 14380 | slightly more stutter (lower P99 delay: 3.85 against 4.50 ms) |
| `wifi_random_spikes` | on the 16 ms configuration: added-delay P99 3.27 ms | existing switches: 1.88 ms, equal delivery | the 16 ms configuration's cost; from 2 ms SmartLink equals the switches (1.88) |
| `weak_signal` | on the 16 ms configuration: P99 2.23 ms, 9.67 stutters a minute | existing switches: 2.16 ms, 7.75 | as above: from 2 ms SmartLink equals the switches |

Which existing policy wins each scenario in the model (fewest stutters, then least added delay, then most frames):
`wired_clean`: all equal. `wired_rare_loss`: the existing switches (0 stutters; SmartLink from 2 ms equal).
`rtt_variation`: `slipstream_rc4` (fewest audio gaps; video equal). `wifi_random_spikes`, `wifi_periodic_spikes`,
`governor_case`: video equal; SmartLink has the fewest audio gaps, the existing switches the smallest reserve.
`burst_loss`: the existing switches and SmartLink from 2 ms, equal. `reordering`: `early_loss_lan` (12.4 stutters,
P95 3.2 ms; 31 frames fewer than the switches). `congestion`: the existing switches (half SmartLink's audio gaps).
`bandwidth_drop` and `interruptions`: all equal. `weak_signal`: the existing switches on video, SmartLink on audio
gaps (2.3 against 5.6 a minute). `reordering_loss`: `early_loss_lan` (1.5 stutters, P99 3.6 ms).

No scenario is left where SmartLink apply is worse than the existing switches on both added delay and stutter; in
the frequent-reordering regime, where it was, it now leaves the wait as configured.

### Tables (per scenario; mean of 3 seeds; added-delay percentiles over the frames of all seeds)
#### wired_clean

wired, no impairment (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | 0.0 |
| smartlink_apply_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | 0.0 |
| smartlink_apply_learned | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | 0.0 |
| early_loss_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |

#### wired_rare_loss

wired, 0.02 % of packets lost at random for 90 s (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 10.00 | 0.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 10.00 | 0.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.42 | 0.0 | 0.00 | 20.0 / 20.0 | 103/35/35/67 | 6.0 (3) | 89.6 |
| smartlink_apply_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | 0.0 |
| smartlink_apply_learned | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | 0.0 |
| early_loss_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.00 | 0.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 10.00 | 0.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |

#### rtt_variation

90 s: one-way delay wanders 0-48 ms (round trip ~3-100 ms), period 20 s, plus a 0-20 ms step every ~5 s (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.92 | 0.0 | 0.25 | 20.9 / 29.5 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.92 | 0.0 | 0.25 | 20.9 / 29.5 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.92 | 0.0 | 0.42 | 19.3 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.92 | 0.0 | 0.42 | 20.3 / 23.8 | 0/0/0/240 | 5.0 (0) | 0.0 |
| smartlink_apply_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.92 | 0.0 | 0.42 | 20.3 / 23.8 | 240/0/0/0 | 5.0 (0) | 0.0 |
| smartlink_apply_learned | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.92 | 0.0 | 0.42 | 20.3 / 23.8 | 240/0/0/0 | 5.0 (0) | 0.0 |
| early_loss_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.92 | 0.0 | 0.25 | 20.9 / 29.5 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 0.92 | 0.0 | 0.25 | 20.9 / 29.5 | 0/0/0/240 | 0.0 (0) | - |

#### wifi_random_spikes

90 s: droughts of 20-80 ms at random, mean gap 5 s; 0.3 % of packets 1-6 ms late (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 3.27 | 14384.7 | 0.3 | 4.67 | 0.3 | 3.58 | 26.0 / 56.6 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 3.27 | 14384.7 | 0.3 | 4.67 | 0.3 | 3.58 | 26.0 / 56.6 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 1.88 | 14384.7 | 0.3 | 4.67 | 0.3 | 4.25 | 20.1 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 3.27 | 14384.7 | 0.3 | 4.67 | 0.3 | 2.83 | 27.1 / 46.7 | 0/0/0/240 | 12.3 (0) | 24.2 |
| smartlink_apply_lan | 0.00 / 0.00 / 1.88 | 14384.7 | 0.3 | 4.67 | 0.3 | 2.83 | 27.1 / 46.7 | 240/0/0/0 | 12.3 (0) | 24.2 |
| smartlink_apply_learned | 0.00 / 0.00 / 1.88 | 14384.7 | 0.3 | 4.67 | 0.3 | 2.83 | 27.1 / 46.7 | 240/0/0/0 | 12.3 (0) | 24.2 |
| early_loss_lan | 0.00 / 0.00 / 1.88 | 14384.7 | 0.3 | 4.67 | 0.3 | 3.58 | 26.0 / 56.6 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 3.27 | 14384.7 | 0.3 | 4.67 | 0.3 | 3.58 | 26.0 / 56.6 | 0/0/0/240 | 0.0 (0) | - |

#### wifi_periodic_spikes

90 s: droughts of 30-70 ms every 10 s (+-0.3 s), as background scans do (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 2.25 | 0.0 | 1.92 | 23.6 / 47.6 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 2.25 | 0.0 | 1.92 | 23.6 / 47.6 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 2.25 | 0.0 | 2.08 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 2.25 | 0.0 | 1.33 | 25.3 / 40.0 | 0/0/0/240 | 15.3 (0) | 29.0 |
| smartlink_apply_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 2.25 | 0.0 | 1.33 | 25.3 / 40.0 | 240/0/0/0 | 15.3 (0) | 29.0 |
| smartlink_apply_learned | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 2.25 | 0.0 | 1.33 | 25.0 / 40.0 | 240/0/0/0 | 13.3 (0) | 29.0 |
| early_loss_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 2.25 | 0.0 | 1.92 | 23.6 / 47.6 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 2.25 | 0.0 | 1.92 | 23.6 / 47.6 | 0/0/0/240 | 0.0 (0) | - |

#### governor_case

90 s: stalls of 20-80 ms every 2-8 s (uniform), mean jitter under 3 ms (the governor review's case) (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 4.33 | 0.0 | 3.08 | 25.6 / 55.4 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 4.33 | 0.0 | 3.08 | 25.6 / 55.4 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 4.33 | 0.0 | 3.75 | 20.1 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 4.33 | 0.0 | 2.67 | 26.9 / 43.3 | 0/0/0/240 | 10.0 (0) | 29.2 |
| smartlink_apply_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 4.33 | 0.0 | 2.67 | 26.9 / 43.3 | 240/0/0/0 | 10.0 (0) | 29.2 |
| smartlink_apply_learned | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 4.33 | 0.0 | 2.67 | 26.9 / 43.3 | 240/0/0/0 | 10.0 (0) | 29.2 |
| early_loss_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 4.33 | 0.0 | 3.08 | 25.6 / 55.4 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 4.33 | 0.0 | 3.08 | 25.6 / 55.4 | 0/0/0/240 | 0.0 (0) | - |

#### burst_loss

90 s: Gilbert-Elliott loss bursts, mean 5 packets, one every ~2.5 s; 3 FEC packets per frame (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 0.00 | 14365.3 | 19.7 | 9.83 | 17.0 | 0.08 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 0.00 | 14365.3 | 19.7 | 9.83 | 17.0 | 0.08 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 0.00 | 14365.3 | 19.7 | 4.75 | 17.3 | 0.08 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 0.00 | 14365.3 | 19.7 | 5.08 | 17.3 | 0.08 | 20.3 / 23.3 | 104/35/35/65 | 7.0 (3) | 90.0 |
| smartlink_apply_lan | 0.00 / 0.00 / 0.00 | 14365.3 | 19.7 | 4.75 | 17.3 | 0.08 | 20.3 / 23.3 | 240/0/0/0 | 1.0 (0) | 29.6 |
| smartlink_apply_learned | 0.00 / 0.00 / 0.00 | 14365.3 | 19.7 | 4.75 | 17.3 | 0.08 | 20.3 / 23.3 | 240/0/0/0 | 1.0 (0) | 29.6 |
| early_loss_lan | 0.00 / 0.00 / 0.00 | 14365.3 | 19.7 | 4.75 | 17.7 | 0.08 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 0.00 | 14365.3 | 19.7 | 9.75 | 17.7 | 0.08 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |

#### reordering

90 s: 5 % of packets 1-12 ms late (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 9.55 / 10.92 | 14384.0 | 1.0 | 127.33 | 1.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 9.55 / 10.92 | 14384.0 | 1.0 | 127.33 | 1.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 6.20 / 9.76 | 14368.7 | 16.3 | 38.17 | 14.0 | 0.00 | 20.0 / 20.0 | 80/30/0/0 | 6.0 (3) | - |
| smartlink_apply | 0.00 / 9.55 / 10.92 | 14384.0 | 1.0 | 127.33 | 1.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | 19.6 |
| smartlink_apply_lan | 0.00 / 3.32 / 5.00 | 14338.0 | 47.0 | 13.58 | 38.0 | 0.00 | 20.0 / 20.0 | 239/0/0/1 | 1.3 (2) | 26.8 |
| smartlink_apply_learned | 0.00 / 3.32 / 5.00 | 14338.0 | 47.0 | 13.58 | 38.0 | 0.00 | 20.0 / 20.0 | 239/0/0/1 | 1.3 (2) | 26.8 |
| early_loss_lan | 0.00 / 3.23 / 5.00 | 14337.7 | 47.3 | 12.42 | 38.3 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 9.55 / 10.92 | 14384.0 | 1.0 | 127.33 | 1.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |

#### congestion

90 s: every 20 s a 3 s bottleneck at 95 % of the stream's rate, 100 ms buffer (queue grows, then drains) (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 1.30 | 14385.0 | 0.0 | 1.25 | 0.0 | 2.50 | 23.6 / 39.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 1.30 | 14385.0 | 0.0 | 1.25 | 0.0 | 2.50 | 23.6 / 39.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 1.25 | 0.0 | 1.25 | 22.9 / 32.1 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 1.25 | 0.0 | 2.50 | 28.2 / 40.0 | 91/45/35/69 | 21.0 (3) | 83.0 |
| smartlink_apply_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 1.25 | 0.0 | 2.50 | 28.2 / 40.0 | 240/0/0/0 | 15.0 (0) | 8.0 |
| smartlink_apply_learned | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 1.25 | 0.0 | 2.50 | 28.2 / 40.0 | 240/0/0/0 | 15.0 (0) | 8.0 |
| early_loss_lan | 0.00 / 0.00 / 0.00 | 14385.0 | 0.0 | 1.25 | 0.0 | 2.50 | 23.6 / 39.0 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 1.30 | 14385.0 | 0.0 | 1.25 | 0.0 | 2.50 | 23.6 / 39.0 | 0/0/0/240 | 0.0 (0) | - |

#### bandwidth_drop

90 s: capacity 85 % of the stream (30 ms buffer); the console lowers its target to 75 % after 2 s (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 0.00 | 14273.7 | 111.3 | 1.17 | 4.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 0.00 | 14273.7 | 111.3 | 1.17 | 4.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 0.00 | 14273.7 | 111.3 | 1.17 | 4.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 0.00 | 14273.7 | 111.3 | 1.17 | 4.0 | 0.00 | 20.0 / 20.0 | 0/30/35/175 | 4.0 (3) | 0.0 |
| smartlink_apply_lan | 0.00 / 0.00 / 0.00 | 14273.7 | 111.3 | 1.17 | 4.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | 0.0 |
| smartlink_apply_learned | 0.00 / 0.00 / 0.00 | 14273.7 | 111.3 | 1.17 | 4.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | 0.0 |
| early_loss_lan | 0.00 / 0.00 / 0.00 | 14273.7 | 111.3 | 1.17 | 4.0 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 0.00 | 14273.7 | 111.3 | 1.17 | 4.0 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |

#### interruptions

outages of 0.2, 1 and 5 s at 40, 70 and 100 s (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 0.00 | 14014.0 | 371.0 | 0.75 | 3.0 | 0.75 | 23.3 / 50.8 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 0.00 | 14014.0 | 371.0 | 0.75 | 3.0 | 0.75 | 23.3 / 50.8 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 0.00 | 14014.0 | 371.0 | 0.75 | 3.0 | 0.75 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 0.00 | 14014.0 | 371.0 | 0.75 | 3.0 | 0.75 | 23.1 / 40.0 | 34/55/35/115 | 15.0 (3) | 75.0 |
| smartlink_apply_lan | 0.00 / 0.00 / 0.00 | 14014.0 | 371.0 | 0.75 | 3.0 | 0.75 | 23.1 / 40.0 | 240/0/0/0 | 9.0 (0) | 14.2 |
| smartlink_apply_learned | 0.00 / 0.00 / 0.00 | 14014.0 | 371.0 | 0.75 | 3.0 | 0.75 | 23.1 / 40.0 | 240/0/0/0 | 9.0 (0) | 14.2 |
| early_loss_lan | 0.00 / 0.00 / 0.00 | 14014.0 | 371.0 | 0.75 | 3.0 | 0.75 | 23.3 / 50.8 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 0.00 | 14014.0 | 371.0 | 0.75 | 3.0 | 0.75 | 23.3 / 50.8 | 0/0/0/240 | 0.0 (0) | - |

#### weak_signal

90 s: signal falls 70 -> 25 %; droughts of 20-60 ms (mean gap 3 s) and 0.5 % random loss (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 0.00 / 15.83 | 14385.0 | 0.0 | 144.25 | 0.0 | 4.00 | 22.6 / 37.5 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 0.00 / 15.83 | 14385.0 | 0.0 | 144.25 | 0.0 | 4.00 | 22.6 / 37.5 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 0.00 / 2.16 | 14385.0 | 0.0 | 7.75 | 0.0 | 5.58 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| smartlink_apply | 0.00 / 0.00 / 2.23 | 14385.0 | 0.0 | 9.67 | 0.0 | 2.42 | 26.4 / 40.0 | 110/35/35/60 | 18.0 (3) | 90.0 |
| smartlink_apply_lan | 0.00 / 0.00 / 2.16 | 14385.0 | 0.0 | 7.75 | 0.0 | 2.33 | 26.3 / 40.0 | 240/0/0/0 | 11.7 (0) | 28.4 |
| smartlink_apply_learned | 0.00 / 0.00 / 2.16 | 14385.0 | 0.0 | 7.75 | 0.0 | 2.33 | 26.3 / 40.0 | 240/0/0/0 | 11.7 (0) | 28.4 |
| early_loss_lan | 0.00 / 0.00 / 2.16 | 14385.0 | 0.0 | 7.75 | 0.0 | 4.00 | 22.6 / 37.5 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 0.00 / 15.83 | 14385.0 | 0.0 | 144.25 | 0.0 | 4.00 | 22.6 / 37.5 | 0/0/0/240 | 0.0 (0) | - |

#### reordering_loss

90 s: 2 % of packets 1-12 ms late and 0.2 % lost at random (clean before 30 s and after 120 s; 240 s per run, 3 seeds).

Model result (Linux simulator), not a measurement

| policy | added delay P50 / P95 / P99 ms | frames delivered | given up | stutter /min | recovery requests | audio gaps /min | audio reserve mean / P95 ms | time at 2/4/8/16 ms wait (s) | setting changes (max /min) | back to leave after (s) |
|---|---|---|---|---|---|---|---|---|---|---|
| original_pushfps | 0.00 / 9.13 / 15.37 | 14383.7 | 1.3 | 151.67 | 1.3 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4 | 0.00 / 9.13 / 15.37 | 14383.7 | 1.3 | 151.67 | 1.3 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |
| slipstream_rc4_switches | 0.00 / 2.17 / 4.50 | 14380.0 | 5.0 | 2.33 | 5.0 | 0.00 | 20.0 / 20.0 | 203/10/0/0 | 1.3 (2) | - |
| smartlink_apply | 0.00 / 7.26 / 9.44 | 14383.3 | 1.7 | 39.42 | 1.7 | 0.00 | 20.0 / 20.0 | 0/0/118/122 | 2.0 (1) | 29.8 |
| smartlink_apply_lan | 0.00 / 2.15 / 3.85 | 14379.0 | 6.0 | 2.75 | 5.7 | 0.00 | 20.0 / 20.0 | 239/0/0/1 | 0.7 (2) | 17.4 |
| smartlink_apply_learned | 0.00 / 2.15 / 3.85 | 14379.0 | 6.0 | 2.75 | 5.7 | 0.00 | 20.0 / 20.0 | 239/0/0/1 | 0.7 (2) | 17.4 |
| early_loss_lan | 0.00 / 2.15 / 3.62 | 14379.0 | 6.0 | 1.50 | 5.7 | 0.00 | 20.0 / 20.0 | 240/0/0/0 | 0.0 (0) | - |
| early_loss_16 | 0.00 / 9.13 / 15.37 | 14383.7 | 1.3 | 151.67 | 1.3 | 0.00 | 20.0 / 20.0 | 0/0/0/240 | 0.0 (0) | - |

Acceptance checks: all passed

## What only a hardware run can show

- Whether the Windows app can fill the `Sample` fields at all, and how often CONNECTIONQUALITY arrives (round trip and
  console target): until then `rttMs`, `consoleTargetKbps` and the trend detector work on partial data.
- Whether real Wi-Fi droughts look like the model's (20-80 ms, order kept) and whether the owner's link shows
  periodic spikes; a replay of recorded traces (`smartlink_sim --replay`) is the first step.
- Whether `lateRescueMs` can be counted per frame in the patched library (stream "net"); without it SmartLink only
  trims and relaxes, never widens.
- Whether a larger audio reserve actually removes the pops the owner hears, and at what audible delay.
- Whether `networkLimited` keeps the governor from lowering graphics on Wi-Fi spikes without hiding a real GPU limit.
- Whether the statistics' risk ranks real windows as it ranks simulated ones, and whether a learned model trained on
  real traces would beat it (the only test that could change the verdict above).
- Wired: SmartLink apply against off with identical `pushfpsMs`, present-to-scanout and audio delay (it should do
  nothing there).

## For the app-side streams: the API

```cpp
#include "veyra/remoteplay/SmartLinkController.h"   // Engine, Config, Decision, Sample, toLogLine
#include "veyra/remoteplay/SmartLinkTrace.h"        // trace CSV helpers
namespace sl = veyra::remoteplay::smartlink;
sl::Engine engine(sl::parseModel(env("VEYRA_EXP_RP_SMARTLINK_MODEL")));   // or sl::Engine(sl::Config{...})
const sl::Mode mode = sl::parseMode(env("VEYRA_EXP_RP_SMARTLINK"));      // Off / Observe / Apply
sl::Decision d = engine.update(sample);       // once per ~200 ms window, one owner thread
if (every 5 s) log(sl::toLogLine(d, mode).view());
if (mode == sl::Mode::Apply) {
    // 0 = RESTORE the configured value; it follows a raised one. Never "keep what is set now".
    setReorderWait(d.reorderWindowMs ? d.reorderWindowMs : configuredWaitMs);
    setAudioReserve(d.audioTargetMs ? d.audioTargetMs : sinkOwnTargetMs);
}
// and engine.reset() (or a new Engine) at every stream start
```

`Config::configuredReorderMs` is the reorder wait the session was configured with (16 ms library default,
2 ms with `VEYRA_EXP_RP_LAN_PROFILE`); 0 means "take `Sample::reorderWaitMs`, else 16". Every output that is
0 / false means "the configured value": that is what a clean link gets. **In apply mode a 0 means restore the
configured reorder wait / audio target**: after a widening or a raise the output returns to 0, and wiring that only
applies non-zero values would keep the raised value for the rest of the session.

**Clock.** Every time given to SmartLink and to its shared state (`Sample::endMs`, `publishShared(..., nowMs)`,
`isFresh(state, nowMs)`) must be milliseconds of the same monotonic clock: `std::chrono::steady_clock` (in the app
`remoteplay::monotonic100ns() / 10000`, which is steady_clock). Checked 20 Nov 2026 by code reading: the publisher
(`RemotePlaySmartLink.h` `tick`, `monotonic100ns`), the audio reader (`CaptureAudioSession.cpp` `hostTime`) and the
interface reader (`PushSlipstream.cpp` `steady100ns`) all use steady_clock; a wall clock (system_clock) would make
`isFresh` wrong after a clock change.

**Advice.** `Decision::advice` carries one advice id for the interface: `None` for the first minute and whenever
there is not enough evidence, `LinkIsFine` only after 2 minutes without loss, late packets or link stalls,
`RaiseRateNextTime` / `LowerPictureRateNextTime` only under the `suggestedStartKbps` rules above, on unstable Wi-Fi a
specific cause (`MoveCloserOrFasterBand`, `BackgroundActivity`) before `UseCable`; it changes at most once a minute.
It is not in the log line (the 400-byte line has no room for it at every field's maximum width). **`reset()` the engine at every
stream start** (a reconnect inside one session may keep it). Fill `Sample::reorderWaitMs` with the wait the library
actually used during the window: SmartLink adds the rescue evidence to that wait, not to its own newer decision.

## Trace file (replay format, version 1)

Integers are written as at most 2^53 (9007199254740992) and larger values are clamped to it: the reader parses
through a double, which holds integers exactly only up to 2^53 (no count or millisecond time comes near it).

One CSV line per `Sample` window, written and read only through `include/veyra/remoteplay/SmartLinkTrace.h`
(`writeTraceHeader`, `formatTraceLine`, `parseTraceLine`), so the recorder and the replay cannot drift apart.
Numbers only (no names, addresses or network names). Unknown = `-1`, except `wifiRssiDbm`, where unknown is `0`.
Decimal point `.`, no thousands separators, no quotes. Lines starting with `#` and the header line are skipped.
Columns may only ever be appended; a reader fills missing trailing columns with "unknown".

Header line (exactly):

```
endMs,windowMs,route,packetsReceived,packetsLost,packetsReordered,packetsLate,packetGapMeanMs,packetGapMaxMs,framesArrived,framesIncomplete,framesFecRecovered,frameGapMaxMs,frameGapExcessMs,lateRescueMs,reorderWaitMs,rttMs,consoleTargetKbps,measuredKbps,requestedKbps,videoQueueDepth,audioBufferedMs,audioUnderruns,wifiSignalPercent,wifiRssiDbm,wifiLinkMbps,wifiBandGhz,frameLateMeanMs
```

| # | column | unit | meaning |
|---|---|---|---|
| 1 | endMs | ms (integer) | host monotonic time at the end of the window |
| 2 | windowMs | ms | length of the window (nominally 200) |
| 3 | route | 0/1/2 | 0 unknown, 1 wired, 2 Wi-Fi |
| 4-7 | packetsReceived, packetsLost, packetsReordered, packetsLate | count in the window | late = arrived after the reorder wait gave it up |
| 8-9 | packetGapMeanMs, packetGapMaxMs | ms | time between consecutive packet arrivals |
| 10-12 | framesArrived, framesIncomplete, framesFecRecovered | count | complete / given up / completed only by FEC |
| 13-14 | frameGapMaxMs, frameGapExcessMs | ms | largest gap between complete frames; its excess over the nominal interval |
| 15 | lateRescueMs | ms | smallest extra reorder wait that would have completed every frame given up in the window; 0 none |
| 16 | reorderWaitMs | ms | the reorder wait in effect |
| 17-20 | rttMs, consoleTargetKbps, measuredKbps, requestedKbps | ms, kbit/s | console-reported round trip and bitrate target, measured video rate, the launch request |
| 21 | videoQueueDepth | frames | waiting for the decoder at the end of the window |
| 22-23 | audioBufferedMs, audioUnderruns | ms, count | PCM queued; playout gaps in the window |
| 24-27 | wifiSignalPercent, wifiRssiDbm, wifiLinkMbps, wifiBandGhz | %, dBm, Mbit/s, GHz | Wi-Fi only |
| 28 | frameLateMeanMs | ms (>= 0, any fixed origin) | mean over the window's complete frames of arrival minus nominal cadence; only its trend is used (queue building) |

Replay: `smartlink_sim --replay trace.csv` runs the lines through the predictor and controller and prints
one decision line per 5 s plus a summary (see "Simulator" below).
