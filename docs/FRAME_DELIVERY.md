# Frame delivery for a PS5 stream: where a frame can be lost, late or repeated

Branch `exp/s4-stutter`, 20 November 2026. Code reading (line numbers are this branch's), the owner's first
hardware benchmark of 19 November 2026 (**software counters, Wi-Fi, one session**) and a Linux model
(`tools/remoteplay_latency_bench/delivery_sim.cpp`, **a model, not a measurement**). Nothing here was run on
Windows, an RTX card or a PS5.

## 1. The path, stage by stage

```
takion thread ──► SessionInbox/VideoIngress ──► owner thread: decode ──► mailbox ──► engine thread ──► Present ──► scanout
 (arrival stamp)     FIFO, max 4, 100 ms          (D3D12VA, serial)      1 slot      read, GPU, wait     DXGI queue 1
```

| # | Stage (file:line) | What can happen to a frame | Cost | Counted where | What today's logs can / cannot tell |
|---|---|---|---|---|---|
| 1 | Arrival stamp: `ChiakiBackend.cpp:197` stamps `arrival100ns` when the whole frame has been reassembled, just before `Token::video` takes the inbox lock (`SessionInbox.cpp:71-83`). | Network jitter, reorder wait and FEC delay it; Wi-Fi holds release several frames together (a *clump*). | delay, and the cause of most mailbox losses (row 5) | `[remoteplay-progress] receiveAgeMs`, `inputFps`; `frameJitterMs` only with `VEYRA_EXP_RP_HEALTH` | Per-second counts only: a clump of 5 frames inside one second is invisible. **New:** `[slipstream-delivery] gap*/clumps/longGaps` (`VEYRA_EXP_RP_STATS=1`). |
| 2 | Ingress queue: `VideoIngress::push` (`VideoIngress.cpp:50-98`): a frame older than 100 ms on arrival (`:79`), a sequence gap or lost frame (`:76-78`), a full queue (4 frames, `:86`) or a frame while waiting for a keyframe (`:87`) drops the queue and requests a keyframe. | **Dropped**, plus every frame until the next keyframe. A burst, not one frame. | frames, then a frozen picture until the IDR | `video.dropped`, `recoveries`, `idrRequests` (`[remoteplay-progress] idrRequests`, `[remoteplay-lan]` with a LAN switch) | Seen: 1 keyframe request per run, the one at start. So in the benchmark nothing was dropped here after start. |
| 3 | **Expiry race** (upstream): the owner reads the clock (`RemotePlaySource.cpp:399`) and only then takes the inbox lock (`SessionInbox.cpp:47`). A sample pushed into an *empty* queue in between has `arrival > now`, and `expiredLocked` (`VideoIngress.cpp:41`) calls it expired: recovery, queue dropped, keyframe request. Fixed only under `VEYRA_EXP_RP_BACKLOG`. | Same as row 2 (a recovery). | a keyframe round trip of frozen picture | as row 2 | Seen: no extra keyframe request in any run, so it did not fire in ~40 minutes. See section 4. |
| 4 | Decode: owner loop `RemotePlaySessionSource.cpp:330` → `RemotePlaySource::read` decodes one access unit per call, serially; more than 4 decoded-but-unread pictures make `drainDecoder` fail (`RemotePlaySource.cpp:334`). Event-driven owner: woken by the sample (`:394`); legacy: `sleep_for(1 ms)` (`:396`), which lasts until the next system tick (15.6 ms) unless the process raised the timer resolution. | Delay (decode time; with polling, the wait for the next poll). Decoded frames come out **back to back** when several samples were queued (a *decode burst*). | delay; feeds row 5 | `decodeMeanMs`, `decodeP95Ms` (`SessionInbox` snapshot), `[remoteplay-progress] decodeAgeMs` | Mean/P95 decode time, not how close together the outputs were. **New:** `decodedGapP95`, `decodedClumps`. |
| 5 | **Mailbox** `latest_` (one slot): `publishDecoded` (`RemotePlaySessionSource.cpp:449-462`) overwrites a frame the engine has not taken (`:456`, `++skipped_`, flags carried, `Drop` set) and sets the auto-reset `frameEvent_`. | **Dropped unseen** when two frames are published between two engine pickups. The newest is shown, so the newest frame's latency is not hurt; the motion step doubles (a visible skip). | a frame | `skipped()` → `captureStats.dropped` (`EngineController.cpp:3212`) → `[frame-flow] mailboxOverwritten` (`:3248`) | Count only: not whether the cause was a clump (row 1), a decode burst (row 4) or a blind engine (row 6). **New:** the `[slipstream-delivery]` extras separate the three. |
| 6 | **Engine pickup**: the engine reads the mailbox only at the top of its loop (`EngineController.cpp:2129`). Before that, with a live source, the low-latency queue on, VSync and no frame generation, it **waits until the previous frame is out of the scheduler and the DXGI queue has room** (`:2025`, wait `:2042` on the frame-latency object or `waitLive` 1 ms slices `:1527-1536`). With NVIDIA low latency on, `beginSourceInput` (`:2119`) may also sleep in the driver before the read. So after each pickup the engine is *blind* for: GPU processing + the wait until the frame leaves the queue at a vblank (up to one refresh, 5.56 ms at 180 Hz) [+ the Reflex sleep]. | A frame that arrives while the engine is blind **waits** (delay up to the blind window); a second one in the same window **replaces** it (row 5). | delay; with a clump, a frame | `[push-latency] pushfpsMs` (arrival → Present returned, mean), `[capture-timing] schedulingWaitP95Ms` | Mean arrival→Present only. **New:** `pickupDelayP50/P95/Max` (decoded → taken). |
| 7 | `cancelledBeforePresent` (`EngineController.cpp:3168`): frames of a batch still unhandled when its presentation step is finished early: `LiveGpuScheduler::cancel()` (`LiveGpuScheduler.h:36`) on stop, pause, seek, a reset or a failed step. | Dropped after processing. | a frame | `[frame-flow] cancelledBeforePresent` | Seen: they appear only in the last `[frame-flow]` line of a run (1 and 21 in two runs checked), i.e. at teardown. Not a cause of in-play stutter. |
| 8 | Presenter replacement (`PresentSink.cpp:488`): more presents than refreshes between two shown frames means a present was replaced unseen in the DXGI queue (Present(0,0) / Automatic display sync can do this; with VSync and queue 1 it should not). | Dropped after Present. | a frame | `[display-latency] replacedAtLeast` | Present-count arithmetic from DXGI frame statistics; a lower bound. |
| 9 | Present → scanout: `[display-latency] presentToScanoutP50Ms` 9.1-9.9 ms at a 5.56 ms refresh in every arm, `compositionMode=-1`. | Delay of ~1.7 refreshes; the window was probably composed, not independent flip. | delay | as named | Owned by stream "present". |

Duplicates: the engine never shows a stream frame twice on purpose; when no new frame arrives the display keeps
the last one (a *repeat*, seen as a long shown interval). Repeats come from late arrivals (row 1), the engine
being blind (row 6) or a frame dropped before it (rows 2, 5, 7, 8).

## 2. The measured numbers (software counters, Wi-Fi, one session)

| | event-driven (stable, no switch) | legacy polling (`VEYRA_TEST_RP_LEGACY_TIMING=1`) | original build |
|---|---|---|---|
| mailbox overwritten / received | 0.71 % | 3.40 % | 1.93 % |
| seconds with < 57 presents | 4.2 % (21/501) | 23.3 % (124/533) | 19.2 % (125/650) |
| seconds with < 58 frames **arrived** (coordinator's join, approximate) | 20/490 | 17/521 | 15/640 |
| low-present seconds that had full arrivals | 14 of 21 | 119 of 123 | 115 of 124 |
| overwrites inside the low-present seconds | 113 of 260 (about 5 per low second); the other 147 spread over 469 s (0.3/s) | | 56 of 513 |

Per-second shape of the overwrites (this analysis, `[frame-flow]` deltas after the first 600 frames):
event-driven 354 s with none, 90 with one, 27 with two, 15 with three or more; legacy 142 / 133 / 106 / 138.
Seconds that received more than 60 frames (a backlog arriving) carry more overwrites (event-driven 0.54 per
second against 0.38 in seconds with 58-60).

What this says:
- About 3-4 % of seconds are short of **arrivals** in every arm: that is the console and the network, and no
  PC-side policy can show a frame that never arrived.
- **Legacy polling** loses frames that *did* arrive (119 of 123 low seconds had full arrivals). Code reading
  gives two mechanisms: the owner's `sleep_for(1 ms)` lasts up to a 15.6 ms tick, so queued samples are
  decoded back to back (decode bursts, row 4) and the engine sees them published 1-3 ms apart; and the engine
  polls in 1 ms slices instead of waking on the event. The model (section 3) reproduces only a small part of
  the 3.4 % from these two (0.71-0.88 % at an otherwise identical engine; 2.6 % only if the engine also stays
  blind two extra refreshes). So the larger part of the legacy loss is **not explained** by the model; the
  `[slipstream-delivery]` line in a legacy arm would show which: `decodedClumps` far above `clumps` = decode
  bursts (owner); a large `pickupDelayP95` = blind engine.
- **Original build**: most of its missing presents are not mailbox overwrites (56 of 513 overwrites in its
  124 low seconds). Its presentation differs (`[pacing] requested=false`: low-latency queue off), so the
  candidates are rows 8 and 6 (presents replaced in the DXGI queue under Automatic display sync, frames
  waiting behind a full DXGI queue). `[display-latency] replacedAtLeast` per second in the original's runs is
  the next thing to read.
- **What remains with the event-driven hand-off** (0.71 %): overwrites in rare bursts (~5 in one second)
  plus a low steady rate. In the code, a single late frame cannot cause an overwrite with an event-driven
  engine: the engine takes the late frame at once and the next one waits alone during the blind window. An
  overwrite needs **two frames published while the engine is blind**, i.e. a clump of three or more arrivals
  (a link hold of ~20-50 ms releasing 2-4 frames for the steady rate, a hold of ~100 ms releasing 5-9 frames
  for the bursts), or a long blind window (slow GPU work, Reflex sleep, a later queue signal). Decode
  bursts are not needed to explain it. The separating measurement is exactly the new line: `clumps` and
  `longGaps` (network) against `decodedClumps` (decoder) against `pickupDelayP95/Max` (engine), per 5 s,
  next to `replaced`.

## 3. Model: policies compared (MODEL, `delivery_sim.cpp`, 3 seeds x 600 s per row)

Parameters (stated in the source): 59.94 fps; LAN = 0.25 ms jitter; Wi-Fi = 1 ms jitter + short link holds
0.32/s of 22-48 ms + long holds 1 per 20 s of 80-150 ms (fitted by hand to the event-driven numbers above:
the model gives 0.67 % replaced, 4.35 % low seconds, 43 % of replacements in low seconds); 0.3 % of frames
never sent; decode 1-3 ms serial; engine processing 1-2.5 ms; queue-1 VSync with the picture on screen one
refresh after the first vblank after Present (as the field's 9-10 ms present→scanout suggests) and the engine
blind until that first vblank; or immediate (tearing/VRR) presentation.
"fresh" = time from a frame's arrival until it **or a newer frame** is on screen (the input-relevant latency
of the newest information); "a2s" = arrival → scanout of shown frames.

Wi-Fi, event-driven, queue-1 VSync:

| Hz | policy | replaced % | low seconds % | a2s P50 / P99 ms | fresh P50 / P99 ms |
|---|---|---|---|---|---|
| 180 | (a) newest wins (today) | 0.67 | 4.35 | 12.52 / 16.39 | 12.54 / 18.25 |
| 180 | (b) catch-up | **0.48** | 4.01 | 12.52 / 16.92 | 12.53 / 19.55 |
| 180 | (c) newest unless the newer misses the vblank anyway | 0.67 | 4.35 | identical to (a) | identical to (a) |
| 180 | (d) de-jitter 4 ms | 0.61 | 3.90 | 14.88 / 18.66 | 14.89 / 21.18 |
| 180 | (d) de-jitter 8 ms | 0.52 | 3.79 | 18.84 / 22.52 | 18.84 / 22.78 |
| 120 | (a) = (b): catch-up off at 120 Hz since the delta2 fix (before: (b) 0.62 / 4.52 / 16.74 / 24.22 / 16.77 / 27.69) | 0.85 | 4.91 | 16.72 / 22.02 | 16.76 / 25.57 |
| 144 | (a) / (b) | 0.74 / 0.53 | 4.57 / 4.29 | 14.62 / 19.20 vs 14.64 / 20.51 | 14.65 / 21.81 vs 14.66 / 23.41 |
| 240 | (a) / (b) | 0.50 / 0.32 | 3.01 / 2.40 | 10.43 / 13.66 vs 10.44 / 13.95 | 10.44 / 14.58 vs 10.44 / 15.42 |
| 60 | all | 1.17 (catch-up never allowed) | 6.13 | | |

- Wired LAN: **0 replaced** in every policy and rate; all policies identical except (d), which only adds its
  delay. Catch-up is a Wi-Fi measure.
- Immediate (tearing/VRR) presentation: 0.01 % replaced for every policy (the engine is blind only for its
  processing time); catch-up almost never triggers.
- (c) cannot differ from (a) in this engine: both waiting frames are already decoded at the pickup, so the
  newer one always makes the same vblank the older one would. Kept in the model to show it.
- (d) costs its delay on every frame (P50 +2.4 ms for 4 ms, +6.3 ms for 8 ms at 180 Hz): **fails** the
  no-added-latency rule, as expected. Reference only.
- In the model, (b) leaves the P50 of both latency measures unchanged (±0.02 ms) and raises the newest
  information's P99 by 1.3 ms at 180 Hz (0.23 of a refresh), 1.6 ms at 144 Hz, 0.8 ms at 240 Hz: the cost
  falls only on the rare frames held back one refresh. In the model it removes about 28 % (180 Hz) to 36 %
  (240 Hz) of the replaced frames and 0.3-0.6 points of low seconds. At 120 Hz it is now off (delta2 fix,
  below); before, the model gave 27 % fewer replaced frames there for +2.1 ms of newest-information P99. It cannot recover long-hold bursts (at most one extra
  frame per clump, by design: showing more stale frames would build latency).
- Sensitivity: an engine blind one or two refreshes longer raises today's replaced to 1.12 / 1.33 %; a slow
  engine (6-9 ms processing, e.g. heavy super resolution) to 1.12 %. There catch-up now gives 1.11 % (it
  fires on 0.03 % of frames): its catch-ups cost about two refreshes (11.1 ms at 180 Hz), above the bound of
  8.5 ms, so the guard keeps it off. With the earlier bound of two refreshes it fired on 0.46 % and gave
  0.94 %, at the price of the newest information's P99 rising from 28.6 to 29.9 ms and a2s P99 from 23.0 to
  26.2 ms: exactly the latency the bound now refuses. The other rows of the table are unchanged by the new
  bound (re-run 20 November 2026, `delivery_sim_results.json`): with a normal engine a catch-up costs one
  refresh, inside the bound at every rate. Re-run again after the delta2 fix (one refresh at most 0.9 x the
  bound): only the two 120 Hz Wi-Fi catch-up rows changed (vsync and immediate), now equal to newest wins;
  every other row is identical (MODEL; the model's source is 59.94 fps, at 60/120/144/180/240 Hz only).
- Robustness (independent reviewer, MODEL, 20 November 2026): other fits of the Wi-Fi parameters that match
  the same three field numbers (0.71 % replaced, 4.2 % low seconds, 43 % of replacements in low seconds)
  gave 22 %, 27 % and 31 % fewer replaced frames with catch-up (this fit: about 28 % at 180 Hz). The
  direction holds; the size depends on the fit. P50 unchanged in every fit; latency P99 +0.2 to +0.7 ms,
  newest information ("fresh") P99 +0.3 to +1.5 ms.

### What a catch-up costs (code reading, `FrameDelivery.h`)

The newer of two waiting frames is handed out at the pickup **after** the older one, so it is late by the
pickup-to-pickup time. Bound: `catchUpCostBound100ns` = **min(2 refreshes, 0.5 x source interval) x 1.02**
(the 2 % covers NTSC rates and timer resolution). Catch-up is allowed only when **one refresh is at most
0.9 x the bound** (`catchUpCanMeetBound`, delta2 review): before, 60 fps at 120 Hz had 0.17 ms of margin
(8.33 vs 8.5 ms), so a pickup jitter of 0.2 ms tripped the guard about 48 times an hour, and 30 fps at
60 Hz had 0.33 ms. The refresh is the display's nominal refresh (`displayRefreshHz`), also with variable
refresh; an unknown refresh (or source rate) means catch-up is off. Every row is checked by the test
`delivery_catchup_cost_bound` (code, not a measurement):

| display | 60 fps: one refresh / bound / 0.9 x bound | catch-up | 30 fps: one refresh / bound / 0.9 x bound | catch-up |
|---|---|---|---|---|
| 59.94 Hz | 16.68 / 8.50 / 7.65 ms | off | 16.68 / 17.00 / 15.30 ms | off (was on) |
| 60 Hz | 16.67 / 8.50 / 7.65 ms | off | 16.67 / 17.00 / 15.30 ms | off (was on) |
| 100 Hz | 10.00 / 8.50 / 7.65 ms | off | 10.00 / 17.00 / 15.30 ms | on |
| 119.88 Hz | 8.34 / 8.50 / 7.65 ms | off (was on) | 8.34 / 17.00 / 15.30 ms | on |
| 120 Hz | 8.33 / 8.50 / 7.65 ms | off (was on) | 8.33 / 17.00 / 15.30 ms | on |
| 143.86 Hz | 6.95 / 8.50 / 7.65 ms | on | 6.95 / 14.18 / 12.76 ms | on |
| 144 Hz | 6.94 / 8.50 / 7.65 ms | on | 6.94 / 14.17 / 12.75 ms | on |
| 165 Hz | 6.06 / 8.50 / 7.65 ms | on | 6.06 / 12.36 / 11.13 ms | on |
| 180 Hz | 5.56 / 8.50 / 7.65 ms | on | 5.56 / 11.33 / 10.20 ms | on |
| 240 Hz | 4.17 / 8.50 / 7.65 ms | on | 4.17 / 8.50 / 7.65 ms | on |
| 360 Hz | 2.78 / 5.67 / 5.10 ms | on | 2.78 / 5.67 / 5.10 ms | on |

30 fps at 100 Hz stays on under this rule (10.0 ms against 15.3 ms: a whole refresh of margin to spare); a
30 fps stream at 50 Hz is off as before. Where it stays on with a 60 fps source the margin is at least
0.85 ms (143.86 Hz), and the test `delivery_catchup_jitter_margin` (the review's jitter model) sees no trip
in an hour with 0.5 ms of pickup jitter at any of those rates.

Worst case, in order:
- **Normally about one refresh** for the held-back newest frame (with a queue-1 vsync presenter the next
  pickup comes at the next vblank).
- **Above the bound, once**: the guard measures each catch-up's cost after the fact and cannot cap that one
  event (a GPU hiccup during it); any catch-up above the bound switches the rule off. The hold is 5 s, and
  doubles (10, 20, 40, up to 80 s) for every further trip within 80 s of the previous hold's end, so a GPU
  that hiccups regularly pays at most once per 80 s; after 80 quiet seconds the hold starts at 5 s again. A
  reconnect ends a hold but keeps its level.
- **The frame after the held one** can wait up to about one more refresh (it arrives while the held frame
  is being processed).
- The rule never fires twice in a row and never hands out a frame older than one source interval.

### Not modelled

- The PS5 path paces presentation to a smoothed source-PTS anchor even with frame generation off
  (`livePacesSourcePts`, `EngineController.cpp` near :2884). The model presents as soon as the engine is
  free; a held-back frame whose PTS slot has passed may be treated differently by that pacing.
- Catch-up holds one more decoded surface (D3D12VA) while two frames wait; the model has no surface
  pressure. More than 4 decoded-but-unread pictures make `drainDecoder` fail (row 4).

**Recommendation:** in the model, (b) catch-up is the only policy that reduces replaced frames without raising
P50 latency or the newest frame's latency by more than a fraction of a refresh; its benefit in the model is
modest (about a quarter of the replaced frames, Wi-Fi only, at 144 Hz and above for a 60 fps stream; nothing
measured). It is implemented behind `VEYRA_EXP_RP_DELIVERY=catchup`. The larger lever
for the remaining stutter is upstream of the PC (Wi-Fi holds) and the engine's blind window (stream
"present": independent flip, a waitable that signals at the first vblank).

## 4. VideoIngress expiry race (stream "net")

Rate = empty-queue reads per second x arrivals per second x the window between the owner's clock read and its
inbox lock. Empty-queue reads: about 60/s (after each decode) + 250/s (4 ms controller ticks) ≈ 310/s. A
window of 0.1 µs gives ~7 recoveries per hour, 1 µs ~67 per hour, 10 µs (the owner preempted between the two
calls) ~670 per hour. The benchmark showed none beyond the start in ~40 minutes of streaming, so the effective
window on that PC was well under 0.1 µs **or** preemption there was rare; it contributed nothing measurable
to the 0.71 %. But each occurrence costs a keyframe round trip of frozen picture (a large stutter), and its
rate rises with load (preemption) and with the controller tick rate.

**Proposal for stream "net"** (not changed here): in `RemotePlaySource::read` (`RemotePlaySource.cpp:399-400`)
read the clock *inside* the inbox lock, i.e. give `SessionInbox::tryVideo` no `now` parameter and call
`monotonic100ns()` after `std::lock_guard l(s.mutex)` (`SessionInbox.cpp:47`). Every queued sample was stamped
(`ChiakiBackend.cpp:197`) before it was pushed under that same lock, so `now >= arrival` then holds for every
queued sample and `expiredLocked`'s `now<arrival` branch becomes unreachable. Equivalent to what `VEYRA_EXP_RP_BACKLOG` does for its own path, without its other changes. Test:
two threads, pusher and popper, 10^6 iterations, assert `recoveries == 0` (expected to fail with today's order).

## 5. What the next hardware run must show

Arms (same exe, same preferences, `[slipstream-config]` equal except the switch): `VEYRA_EXP_RP_STATS=1`
alone, and `VEYRA_EXP_RP_STATS=1 VEYRA_EXP_RP_DELIVERY=catchup`; frame generation off; 144 Hz or more for a 60 fps
stream (catch-up is off below that, see the table in section 3).

**`VEYRA_EXP_RP_STATS=1` alone does not run the stable build's literal code.** Any of the two switches
selects the two-slot path (`publishDelivery` / `readDelivery`) with the newest-wins rule. By reading it is
equivalent to the one-slot `latest_` (same lock, sequence counter, `skipped_` count, flag merge, `SetEvent`;
the test `delivery_newest_matches_one_slot_mailbox` checks the hand-off against a copy of the one-slot code),
but the STATS arms measure that path, not the stable one. Only an arm with both switches unset runs the
stable code. With both unset, what the experiments' code still does (code reading): one branch on a constant
bool in each publish and each read; two relaxed atomic stores per engine loop (`remote->deliveryContext`,
`EngineController.cpp` near :1484, in `applyPresentation`); and `slots_.clear()` / `catchUpGuard_.reset()`
on empty objects at reconnect and close (`RemotePlaySessionSource.cpp` :444, :564). No lock, no allocation,
nothing measurable; behaviour identical to exp/rc4.

1. Diagnosis (STATS arm): per 5 s window, `replaced` against `clumps`/`longGaps` (network), `decodedClumps`
   (decoder) and `pickupDelayP95/Max` (engine). Expected by this reading: replaced windows coincide with
   `longGaps` and clumps; `decodedClumps ≈ clumps`; `pickupDelayMax` ≤ one refresh + GPU time. If instead
   `pickupDelayMax` is often above two refreshes, the engine's blind window is the cause (stream "present").
   Feed the line to `delivery_sim --fit "arrived=.. clumps=.. longGaps=.."` to check the model.
2. Accept catch-up only if, against the STATS-only arm: `mailboxOverwritten/captureReceived` falls (the model
   expects about a quarter less), seconds under 57 presents do not rise, `[push-latency] pushfpsMs` and
   `[display-latency] presentToScanoutP50Ms` move by less than 0.5 ms, `caughtUp` is > 0, and no
   `[remoteplay-timing]` warning or new error appears. Reject if the P50 latency rises by a quarter refresh or
   more, if seconds under 57 presents rise, or if `caughtUp` stays 0 (then the rule never fires on that PC).
3. Wired LAN (when the cable arrives): `replaced` near 0 in both arms; catch-up then changes nothing, which is
   the expected and acceptable result.
