# Frame-generation scheduling: the fragile spots and what could replace today's wait (stream "lagfix", 23 Nov 2026)

Scope: Directive 25 (the owner's 25th message): improve the frame-generation scheduling itself, in his order: (1) no lag
or stutter, (2) a stable 90 FPS from a 30 FPS game, (3) full 4K picture quality, (4) delay below Package 12A's ~30 ms.

**Labels.** Every number in this file is **SIMULATED**: the program's own decision code (or, in section 2, a generic
schedule model) fed with made-up 4K-like GPU costs (`tests/remoteplay/FgScheduleSim.h`; cost sets: comfortable 1-3 ms
base + 6.6 ms per generated frame; tight 3-5 + 10 ms; spikes 1-3 + 6.6 ms with 2 % of frames +15 ms; drift 6.6 -> 12.6 ms
over a minute; near-limit 3-5 + 11.5 ms with 2 % +10 ms; overload 3-5 + 13.5 ms). Nothing here is **hardware-verified**.
The engine changes of section 1 are **tested** (core tests on Linux and MSVC) and compiled by CI's checks; not started on
a GPU. The predicted-frame schedules of section 2 exist only in the simulator: nothing of them is in the engine.

## 1. The four fragile spots of the default path (both Package 12A and 13)

All four are built as switches in `include/veyra/engine/PushFgSchedule.h` (EngineController reads
`VEYRA_EXP_RP_FG_SCHED=<all|list>`, `VEYRA_TEST_RP_FG_SCHED_LEGACY=<list>`, logs `[fg-sched] ...` at every start).
**Every one is OFF by default**, because none is never-worse in every cost set (the orchestrator's rule (a)): each
trades one of the owner's priorities against another. The core tests print the full table ("Directive 25 candidates").

| Part | What it changes | Near-limit, today -> with it | Comfortable / tight / spikes / drift | Why not default |
|---|---|---|---|---|
| robustAdmit | admission also admits when min(P95, 1.2 x P75) of the FG cost fits, so a few slow groups cannot lock generation out for 2 s | card: skipped pairs 11.2 -> 0/s, generated 34.8 -> 59.7/s, off-slot intervals 25.5 -> 12.4 %, longest gap 62.4 -> 33.4 ms, real frame P95 61.0 -> 46.9 ms; stream: generated 29.1 -> 58.6/s | identical (no lock-out there) | real frame MEAN 35.6 -> 42.5 ms on the card: the 30 FPS stretches it removes showed real frames early |
| phaseBehindWarmup | no phase sample from a pair queued behind a warm-up group | card: real frame P95 61.0 -> 45.2 ms, mean 35.6 -> 33.1 ms | identical | 0.02 fewer generated frames/s, +1.4 % off-slot intervals |
| streamSmooth | a PS5 stream's present anchor through LiveArrivalSmoother (PTS line) | stream: off-slot intervals 61.3 -> 59.1 % | off-slot 21.4 -> 0.1 % (comfortable), 24.4 -> 3.9 % (spikes), 30.7 -> 16.5 % (drift); mean -0.5 ms | P95 counted from decode +1.5 to +2.6 ms (bounded by the jitter it removes); longest interval +0.3 ms in drift |
| holdCardsOnly | the card's 1 s rejection hold not on a stream with a learned phase | stream: generated 27.8 -> 29.1/s, longest gap 62.4 -> 40.8 ms | identical | mean real frame 28.5 -> 38.5 ms (the hold's 30 FPS stretches are gone) |

Admission designs compared and not taken: "max of the last 8 groups" (near-limit card: 46.7 generated/s, still 5.9
skips/s) and "admit after 3 skipped pairs" (49.1/s, 5.4 skips/s). robustAdmit is the only one that ends the lock-out.

**Recommendation (for the orchestrator / owner).** robustAdmit, phaseBehindWarmup and holdCardsOnly together serve
priority 1 (no 30 FPS stretches, no 60 ms gaps) and cost priority 4 only where the GPU is at its limit: there today's
"lower" mean delay is the delay of a 30 FPS picture. streamSmooth serves priority 1 on every PS5 stream for a -0.5 ms
mean. By the owner's order of priorities all four would be on; by rule (a) they stay off. One line flips the default
(`kPushFgSchedDefault`), and `VEYRA_TEST_RP_FG_SCHED_LEGACY` then restores today per part.

**FG_ULL's dead-phase idea, re-examined.** Showing a skipped pair's real frame early is one short interval on entry and
one long interval on exit, by construction; the simulator showed every hold variant (FG_ULL hold, the card's 1 s hold on
a stream) losing generated frames and lengthening gaps. With robustAdmit the skip runs it was meant for do not occur.
Not rebuilt.

## 1b. Capture cards: what is ON by default now (the owner's scope and priority order, 23 Nov 2026 22:51 ET)

The owner plays through his HDMI capture card (4K, a 30 FPS game, 3X = 90 FPS) and asked for nothing more on PS5
streams. The orchestrator's rule, refined by his priority ORDER: per card cost set, a part may be on when it is not
worse on (1)+(2) (generated frames shown, share of intervals more than 1 ms off the 11.1 ms grid, longest gap, skipped
pairs), equal on those is decided by (4) delay, and it changes nothing where no pair is skipped (tested:
`fgsched_card_defaults_pass_the_priority_order_in_every_card_set`). Four card sets were added that bracket a 4K VFG pair:
vfg-4k (base 2-4 ms, 7 ms per generated frame, 1 % +5..+15 ms), heavy (base 3-5 ms, 9.5 ms, 1 % +5..+15 ms), thermal
(7 ms rising by 3 ms over two minutes), hiccup (+20 ms base work every 3 s).

- **robustAdmit: ON for capture cards** (`VEYRA_TEST_RP_FG_SCHED_LEGACY=robustAdmit` restores today). Identical in
  comfortable, tight, spikes, drift, vfg-4k, thermal, hiccup; better on (1)+(2) in near-limit, overload, heavy. Its real
  frames in pairs shown WITH generation are earlier than today's generating pairs (near-limit mean 47.1 -> 42.5 ms, P95
  63.5 -> 46.8 ms; heavy 40.2 -> 40.2; overload 39.5 -> 39.0): the higher overall mean in near-limit (35.6 -> 42.5 ms)
  only reflects that generation no longer drops out to 30 FPS, where today's real frames are shown early.
- **gridEarly: ON for capture cards** (`VEYRA_TEST_RP_FG_SCHED_LEGACY=gridEarly`). Item 2 of the owner's scope, the dead
  phase: a pair whose generated frames are skipped (and not held) shows its real frame on the earliest slot of the
  11.1 ms grid that its base cost P95 + 1 ms reaches (`pushGridEarlyDelay`), instead of the full phase. The empty slots
  are empty either way, so the long gap moves from before the real frame to after it; the grid is never left (a late
  real frame is shown when ready, as today). Better on (1)+(2) alone in near-limit, overload, heavy; with robustAdmit on
  it still helps in overload (real frame mean 29.3 -> 24.7 ms) and is identical elsewhere.
- **phaseBehindWarmup: stays OFF.** Alone it breaks the rule in near-limit (generated 34.83 -> 34.81 a second, uneven
  25.5 -> 26.9 %) and overload (18.27 -> 18.21, 89.0 -> 89.7 %); with robustAdmit on it changes nothing in any set.
- streamSmooth and holdCardsOnly: PS5 streams only, out of scope, off. Streams and every other source run exactly as
  before (kPushFgSchedDefault, all off).

SIMULATED, capture card, 29.97 -> 3X, 120 s per set; "gen-pair" = real frames of pairs that showed a generated frame:

| card set | today: real mean/P95, gen-pair mean/P95 | shown/s | skips/s | off % | longest | default now (robustAdmit+gridEarly): real mean/P95, gen-pair mean/P95 | shown/s | skips/s | off % | longest |
|---|---|---|---|---|---|---|---|---|---|---|
| comfortable | 33.4/34.1, 33.4/34.1 | 59.9 | 0.0 | 0.0 | 12.7 | 33.4/34.1, 33.4/34.1 | 59.9 | 0.0 | 0.0 | 12.7 |
| tight | 40.4/41.6, 40.4/41.6 | 59.9 | 0.0 | 0.3 | 15.9 | 40.4/41.6, 40.4/41.6 | 59.9 | 0.0 | 0.3 | 15.9 |
| spikes | 40.0/47.6, 40.0/47.6 | 59.6 | 0.0 | 3.2 | 33.3 | 40.0/47.6, 40.0/47.6 | 59.6 | 0.0 | 3.2 | 33.3 |
| drift | 38.9/43.3, 38.9/43.3 | 59.9 | 0.0 | 0.1 | 13.8 | 38.9/43.3, 38.9/43.3 | 59.9 | 0.0 | 0.1 | 13.8 |
| near-limit | 35.6/61.0, 47.1/63.5 | 34.8 | 11.2 | 25.5 | 62.4 | 42.5/46.8, 42.5/46.8 | 59.7 | 0.0 | 12.4 | 33.4 |
| overload | 27.8/39.5, 39.5/39.6 | 18.3 | 16.8 | 89.0 | 72.7 | 24.7/39.4, 39.0/39.5 | 24.7 | 14.3 | 87.8 | 69.7 |
| vfg-4k | 36.3/41.2, 36.3/41.2 | 59.9 | 0.0 | 1.7 | 30.8 | 36.3/41.2, 36.3/41.2 | 59.9 | 0.0 | 1.7 | 30.8 |
| heavy | 39.8/44.6, 40.2/44.6 | 58.7 | 0.5 | 2.6 | 45.4 | 40.1/44.6, 40.2/44.6 | 59.8 | 0.0 | 1.9 | 33.4 |
| thermal | 36.5/38.1, 36.5/38.1 | 59.9 | 0.0 | 0.0 | 13.7 | 36.5/38.1, 36.5/38.1 | 59.9 | 0.0 | 0.0 | 13.7 |
| hiccup | 35.8/38.5, 35.9/38.5 | 59.3 | 0.0 | 1.2 | 33.5 | 35.8/38.5, 35.9/38.5 | 59.3 | 0.0 | 1.2 | 33.5 |

## 1c. The content cadence of a 60 Hz capture of a 30 FPS game (duplicate removal), and the phase after a spike

`ContentDedup` turns a 60 Hz capture of a 30 FPS game into 30 pictures a second with even content times; frame
generation spaces its frames by those times, so an error there is uneven output however well everything else runs.
Tested on made-up picture sequences (`tests/remoteplay/DedupCadenceTests.inc`; content-interval steps over 1 ms, 20 s):

| Sequence | old cadence | repaired |
|---|---|---|
| steady 24 / 25 / 29.97 / 30 / 40 / 45 / 50 / 60 FPS in 58, 59.94, 60 Hz; 30 in 120 Hz; 2,2,2,1,3 paced 30; a dropped game frame every 2 s | content times | identical to the 100 ns |
| 60 FPS menu, then the 30 FPS game | 25 uneven steps (16.7 / 50 ms alternation for ~1.5 s) | 2 |
| 30 FPS, then 20 FPS | 25 | 2 |
| 30 FPS, 3 s still picture, 30 FPS | 5 (content times then drift 0.3 ms a picture earlier until two misses) | 4, exact 33.37 ms after |

Two provable defects, repaired for capture cards only, on by default (`VEYRA_TEST_RP_DEDUP_LEGACY_CADENCE` restores the
old cadence; `[fg-sched] card content cadence repairs=1` at start): (1) when the content rate falls, the old period
still fits each new picture's window, so the fit placed every second picture one sample early until its slope caught
up; three pictures in a row spanning more than 1.4 x the fitted period now relearn the cadence (never three in a row for
any steady rate above, a 2,2,2,1,3 pattern or a single dropped frame); (2) after a still picture the fit restarted from
the held sample's time, so its first span was one transport period and the slope came out short; it now restarts from
the first new picture. Every steady sequence gives the same content times as before, so the priority-order rule holds
(identical where nothing changes, better on evenness in the transitions). The repairs do not touch the content-rate
readout (`contentFps`), which uses its own 0.75 s window and moved 59.9 -> 34 within 0.6 s of a 60 -> 30 switch in both.

Not changed, reported: a capture whose noise makes one repeat in twenty look new reads 33 FPS and puts a spurious
19.7 ms content step in each such pair; that is the picture comparison's noise band (`PictureCompare.h`, GPU/capture
side), not the cadence. After a GPU spike the learned phase rises at once (one longer interval) and comes down 0.5 ms
a pair; the deadline margin rises 0.1 ms per 250 ms and falls 0.04 ms: in the spikes card set the real frame's P95 is
47.6 ms against a 40.0 ms mean. Slowing the rise would make generated frames late (dropped), so it fails the
priority order; left as is.

## 1d. "99 % steady" (the owner, 23 Nov 2026 23:59 ET)

Acceptance: at least 99 % of Present intervals within 1 ms of the 11.1 ms grid (content interval / 3), and at least
99 % of the expected 90 frames a second shown. SIMULATED, capture card, 120 s per set (the simulator has no duplicate
removal: the content-cadence repairs of 1c act on top of this, in menu / pause transitions only):

| card set | 12A / 13 default: within 1 ms / within half a slot, shown, longest gap, longest off run | this branch's default |
|---|---|---|
| comfortable | 99.98 / 100.00 %, 100.00 %, 12.7 ms, 1 | identical |
| tight | 99.68 / 100.00 %, 100.00 %, 15.9 ms, 3 | identical |
| drift | 99.94 / 100.00 %, 100.00 %, 13.8 ms, 2 | identical |
| thermal | 99.98 / 100.00 %, 100.00 %, 13.7 ms, 1 | identical |
| vfg-4k (1 % of generated frames +5..15 ms) | 98.33 / 99.48 %, 99.92 %, 30.8 ms, 4 | identical |
| hiccup (+20 ms every 3 s) | 98.79 / 99.63 %, 99.26 %, 33.5 ms, 3 | identical |
| spikes (2 % +15 ms) | 96.79 / 98.34 %, 99.64 %, 33.3 ms, 5 | identical |
| heavy | 97.41 / 98.68 %, 98.63 %, 45.4 ms, 68 | 98.05 / 99.34 %, 99.87 %, 33.4 ms, 6 |
| near-limit | 74.52 / 79.19 %, 72.07 %, 62.4 ms, 200 | 87.64 / 97.74 %, 99.69 %, 33.4 ms, 11 |
| overload | 11.02 / 48.33 %, 53.65 %, 72.7 ms, 298 | 12.21 / 58.04 %, 60.82 %, 69.7 ms, 250 |

Frames shown meet 99 % in every set but heavy (today) and overload. On-grid within 1 ms meets 99 % in comfortable,
tight, drift and thermal and misses it where the GPU has spikes: a generated frame finishing 5-15 ms after its deadline
is shown late (inside the 10 ms grace), which is two off-grid intervals (the late one and the short one after it); a
real frame whose base work spikes (hiccup) is late itself. The deadline margin covers at most 1 ms (the owner's
earlier setting). Explored and NOT taken: dropping a generated frame later than 1 ms instead of showing it late lifts
vfg-4k to 99.08 % on grid / 99.12 % shown but lowers frames shown in every spiky set (near-limit 99.69 -> 93.90 %), so
it fails the priority order; a larger phase would add delay to every frame. What limits near-limit and overload: the
GPU work of a pair (base + two generated frames) approaches or exceeds 33.4 ms.

From his real play, without any switch: `[capture-timing]` (always on with a capture card, once a second) now carries
`gridIntervals=`, `onGrid1msPct=`, `onGridHalfPct=`, `shownPct=`, `gridLongestMs=`, `gridOffRunMax=` (PushGridWindow:
Present call spacing against content interval / M; -1 without frame generation). Present calls, not photons.

## 2. Other schedules (the 22.2 ms wait belongs to interpolation, not to the GPU)

Interpolation must have the next real frame B before it can show anything between A and B, so B waits (M-1)/M of a
content interval: 22.2 ms at 3X from 30 FPS, plus its own and the first generated frame's GPU time. A schedule that
shows B earlier must fill the slots after B with frames that do not need the future: predicted (extrapolated) frames.

Simulated on equal terms (same costs, jitter and presentation model; phase learned as LivePairLatency does; no
admission; a predicted frame costs an assumed 3.0 ms for the first of a pair (optical flow A->B at 4K plus a forward
warp) and 1.0 ms for a second (warp only)). "predicted" = share of all shown frames that are predicted.

| Schedule (display order per pair) | card comfortable: real mean / P95, off % | card near-limit | card spikes | stream comfortable | predicted |
|---|---|---|---|---|---|
| interpolate 3X (today): I(1/3), I(2/3), B | 33.0 / 33.5, 0.1 % | 45.5 / 50.5, 4.2 % | 39.3 / 46.8, 3.2 % | 33.5 / 33.8, 21.3 % | 0 % |
| I(2/3), B, P(B+1/3) | 21.9 / 22.4, 0.1 % | 31.3 / 37.5, 2.0 % | 26.2 / 35.6, 1.9 % | 22.4 / 22.7, 21.3 % | 33 % |
| B, P(B+1/3), P(B+2/3) | 4.2 / 4.4, 0.0 % | 6.2 / 6.4, 0.0 % | 4.8 / 12.5, 1.0 % | 4.6 / 4.5, 21.4 % | 67 % |
| interpolate in 8 ms slots (content re-spacing) | 26.7 / 27.3, 100 % | 42.3 / 47.2, 99.5 % | 34.0 / 40.6, 99.9 % | 27.2 / 27.6, 99.8 % | 0 % |
| interpolate 2X (60 FPS, reference only) | 27.4 / 28.0, 0.1 % | 36.8 / 43.0, 3.0 % | 31.8 / 41.1, 2.8 % | 27.9 / 28.3, 31.9 % | 0 % |

(Every cost set and both sources: `fgsim_prints_the_schedule_comparison` in the core tests' output. Stream rows keep
today's decode-time anchor, so their 21 % off-slot share is decode jitter; streamSmooth removes it for every schedule.)

Reading it by the owner's priorities:
- **Content re-spacing** (compressing a pair's frames into shorter slots) saves 6 ms but makes every interval uneven
  (8/8/17 ms): fails priority 1. Rejected. **2X** saves 5.6 ms but is 60 FPS: fails priority 2. Reference only.
- **B + 2 predicted** removes the whole wait (about 5 ms from callback to Present at 4K) and is the most even, but two of
  three frames are guesses: at every change of motion (a turn starting or stopping, an explosion, a cut) both are wrong
  for a whole content interval, and disoccluded areas behind moving objects have no source at all. Priority 3 ranks
  above the delay: rejected as a default, kept as a measured comparison.
- **I(2/3), B, P(B+1/3)** ("one predicted"): ~11.1 ms less delay in every cost set (32.95 -> 21.88 ms comfortable card,
  45.5 -> 31.3 ms near-limit), equal or better evenness (its GPU work per pair is one interpolation and one cheap warp
  instead of two interpolations, so it also has more headroom), and one frame in three predicted only ONE slot (11.1 ms)
  ahead, from motion just measured between A and B. **This is the candidate.** Its picture risk is real: a predicted
  frame overshoots when motion stops or reverses and leaves holes at disocclusions; the design below falls back to
  today's schedule wherever that risk is detected.

What each needs from the GPU side (the GPU specialist's area; not changed here):
- NVIDIA's VFG, DLSS frame generation, FSR and XeSS frame generation, AMD FRC and PushFlow (RIFE) only interpolate
  between two frames they already have. None of them can produce P(B+1/3).
- A predicted frame is our own: forward-warp the enhanced B along the A->B optical flow scaled by 1/3. The graph already
  computes optical flow for NR / SR guidance (`guidance::AmdOpticalFlow`, `GpuDisOpticalFlow`, NVOF; `flowWidth()`),
  has a warp (`shaders/NrReuseWarp.hlsl`), flow fill (`FlowFill.hlsl`) and a shot-change detector (`VfgShotChange.hlsl`).
  A forward splat with a depth-free occlusion rule (keep the larger motion on collision) and hole fill from B is new work:
  `shaders/PushPredict.hlsl`, one pass at 4K (assumed ~1 ms; flow at the existing flow size ~2 ms if not already run).

## 3. Engine design for "one predicted" (NOT built in this package)

1. **Schedule mode**, per pair, decided at admission (EngineController `admitFg`): `Interpolate` (today) or
   `OnePredicted`. The graph is asked for one interpolated frame at t = 2/3 (`FgDecision` with a target time; VFG and
   PushFlow accept a time, DLSS MFG gives fixed indices: with DLSS only today's schedule) and one predicted frame.
2. **Deadlines.** `PresentationScheduler` keeps one anchor per pair; slot offsets become per frame: I(2/3) at
   phase - T/3, B at phase, P at phase + T/3 (today: -2T/3, -T/3, 0). `LivePairLatency::observe` already takes the
   largest "ready - anchor + (B - pts)"; with the predicted frame's pts beyond B the same formula holds (its term is
   ready - anchor - T/3). The legacy phase becomes T/3 + base P95 (today T + base P95), so every bound moves by 2T/3.
3. **Presentation.** The scheduler step presents in display order as today. The predicted frame is a "generated" frame
   for expiry (`kLiveGeneratedGrace100ns`), the deadline margin and the counters, plus a `predicted` counter.
4. **Fallback to today's schedule** (one pair at a time, decided before submission, so no frame is shown twice or out of
   order): a shot change (`VfgShotChange`), flow confidence below a threshold (forward/backward flow disagreement over
   more than x % of the picture), a motion change between the last two pairs (the A->B flow against the previous pair's
   flow), a history reset, or the predicted frame's own GPU time over its budget. A switch between schedules moves B by
   2T/3 (22.2 ms): it is ONE uneven interval each way, so the fallback needs hysteresis (enter after 8 calm pairs, leave
   at once) and a log line per switch; the simulator must count those switches before anything is turned on.
5. **Switch and default.** `VEYRA_EXP_RP_FG_SCHEDULE=onepredicted` (off by default), never from slipstream.ini; a kit
   set comparing it with today's schedule at 4K 3X with the readout, `[slipstream-cadence] offPct` and a recorded clip of
   motion changes for the picture check (priority 3 decides).
6. **What only the owner's PC can tell:** the real cost of flow + warp at 4K, how often the fallback fires in a real
   game, and whether a predicted frame one slot ahead is visible in motion. None of it is known today.
