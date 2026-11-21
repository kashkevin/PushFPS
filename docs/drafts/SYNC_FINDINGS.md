# Package 9 evidence: clock drift, audio sync, decoder resets, presentation numbers (stream "sync", 21 Nov 2026)

Source: the owner's Package 9 evidence (8 runs of `-Session visual`, arms c-15 / c-30 / c-50 / c-100, wired, H.265
SDR 1080p60, everything off: no FG, no SR, no clean-up). The run order was c-15, c-30, c-50, c-100, c-100, c-50,
c-30, c-15 (13:13 to 13:23 local). Every number below comes from the app's own log lines in `bench\measure-*.txt`
(software counters; none is input-to-photon), or it is labelled a model or a code reading. Branch `exp/s6-sync`
(from `exp/rel-9`, built as 9c008f40). Nothing here was built for Windows or run on a PS5.

Words used: **CONFIRMED** = shown by the logs and the code together. **CODE** = follows from reading the code.
**HYPOTHESIS** = consistent with the evidence, not shown.

## 1. Clock drift and audio synchronisation

### How the timestamps are built (CODE)

- Video: Chiaki hands over one access unit per console frame with its 16-bit wire frame index
  (`ChiakiBackend.cpp` videoCallback, patch 0002). `sample.arrival100ns` is `steady_clock` at that callback.
  `RemotePlaySource::read` extends the index (`Sequence16Extender`) and `makePacket` stamps the frame through
  `RemotePlayClock::video` (`src/remoteplay/Timeline.cpp`): `pts = anchor + (index - firstIndex) / 60 + phase`. The phase
  follows the 1 s lower envelope of `arrival - nominal` but moves at most 0.5 % of a frame per frame (83 µs). So the
  PTS advances 16.583-16.75 ms per index, whatever the arrival does. The clock re-anchors only after a 200 ms arrival
  gap, an index jump of more than 30, or a decoder reset (`clock_.reset` in `RemotePlaySource::read`).
- Audio: Opus PCM gets `pts = arrival of the first block + samples / 48000` on the same origin
  (`RemotePlaySource::pullAudio`). `CaptureAudioSession::push` keeps `ingress` = the 2 s lower envelope of
  `arrival - pts` (`ArrivalClockMapping`).
- The audio schedule's target (automatic sync, mode 0; `CaptureAudioSession.cpp` lines 365-376) is
  `present host time - video PTS - ingress` of the newest shown frame. For Remote Play no arrival is passed, so
  `CaptureSyncTarget::observe` returns each frame's value unfiltered (its median of 5 is only for sources that pass an
  arrival); it is clamped to 0-1500 ms, playback is trimmed by at most ±5000 ppm, and an error above 60 ms (checked
  every 250 ms) fades and restarts the sound ("hard reset").
- Video presentation does not use the absolute PTS: a stream re-anchors its deadline on every real frame
  (`EngineController.cpp` 2943-2963, `liveTimeline.resetPair(..., presentAnchor=liveInputReady, ...)`;
  `PresentationScheduler::deadline = host + (pts - source) + delay`). Only PTS differences reach generated frames.

### What the logs show

| run (order) | wire indexes/s | lag slope ms/min | lag at end ms | audio target at end ms | hard resets/min* |
|---|---|---|---|---|---|
| c-15@131353 (1) | 58.60 | 1103 | 1118 | 1115 | 11.1 |
| c-30@131516 (2) | 57.46 | 2257 | 2010 | 1500 (cap) | 13.7 |
| c-50@131634 (3) | 59.03 | 673 | 707 | 744 | 8.0 |
| c-100@131752 (4) | 59.90 | 0 | 2.5 | 32 | 1.0 |
| c-100@131909 (5) | 59.84 | -4 | 2.4 | 33 | 1.0 |
| c-50@132027 (6) | 59.37 | 338 | 347 | 378 | 4.0 |
| c-30@132145 (7) | 57.94 | 1768 until the reset at 66 s | 133 (after the reset) | 162 (after the reset) | 15.4 |
| c-15@132311 (8) | 57.69 | 2019 | 2475 | 1500 (cap) | 13.7 |

\* The sum of the `hardResets=` of the 10 s `[remoteplay-audio]` windows per minute. The kit's "hard resets 6.86
/min" (c-15) read that per-window count as a running counter. Its `pops` (12.43 /min, a real running total of underrun
starts + hard resets) was right. The kit rows now give the median per 10 s window (c-15: 2.0) and the worst 10 s
window (c-100: 1, which a median hides), and "underrun starts" gives the median too.

- **CONFIRMED: the lag is the clock's 0.5 % limit.** In every 10 s window between two `[remoteplay-clock]` lines in
  which the console sent fewer than 59.6 indexes a second (30 windows, all runs), the PTS advanced exactly
  16.750 ms per index (`ptsMs` difference / 600): pinned at the 0.5 % limit, while the frames arrived 16.8-17.7 ms
  apart. (The lag growth then equals `1000 - 16.75 x indexes/s` by arithmetic; it is the pinned step that shows the
  cause.) Independently of the clock line, `[slipstream-net] fps=` (frames received per 5 s) shows the same lower
  rates, and the audio really was held: `delayMs` (PCM queued plus the endpoint, real time) reached 1484 ms in
  c-30@131516 and 1482 ms in c-15@132311. Reproduced by the production clock in `tests/remoteplay/SyncClockTests.inc` and
  `AudioPictureTermTests.inc` (58.60 /s: 1297.7 ms after 70 s).
- **CONFIRMED: the console sent fewer frames; nothing was lost.** No index gap: `skipped=0`, `framesLost=0`,
  `missing=0`, `fecFailed=0`, and Chiaki numbers its frames contiguously (an index gap would count as lost in
  `videoreceiver.c`). `[slipstream-net] fps=` (frames received per 5 s) agrees: 55.2-60.0 at 15-50 Mbit/s against
  59.4-60.0 at 100 (the first 5 s window, 48-53 in every run, left out).
- **CONFIRMED: the audio delay is the audio waiting for that lagging PTS. It is not drift between the console's and
  the PC's audio clocks.** `targetMs` follows `arrivalMinusPtsMs` line by line in every run (for example c-15@131353:
  lag 313 / 499 / 693 ms, target 333 / 533 / 690 ms ten seconds apart), and `delayMs` (PCM held plus the endpoint)
  trails the target (299 / 489 / 651 ms at the same times). The target is capped at 1500 ms; runs 2 and 8 sat at the cap. The resets come from a
  target that grows faster than the 5000 ppm trim (5 ms/s) can follow, so the 60 ms rule restarts the sound every few
  seconds.
- **CONFIRMED: the end values are artefacts of a two-run median.** "Delay at end" 1292.8 = (1107.2 + 1478.3) / 2, and
  "video clock lag at end" 1796.3 = (1118.1 + 2474.5) / 2. The c-30 figures (799.2 / 1071.7) mix run 2 (lag 2010,
  audio at the cap) with run 7, whose decoder reset at 66 s re-anchored the clock (lag 133). That is why c-30 looks
  better than c-15. For the same reason the kit's "video clock lag growth" for c-30 is (2257 - 232) / 2: a slope
  taken across a reset is not a drift.
- **CONFIRMED: bitrate is not the cause; the console's frame rate is.** Below 59.7 indexes a second the lag grows at
  any bitrate, at the rate the relation gives. At 100 Mbit/s the console sent 59.84-59.90 a second, so the clock
  followed. Even there one window of c-100@131909 at 59.65/s added 4 ms/s. Run order cannot explain it: the order is
  symmetric, and the last run (c-15, 57.69/s) was worse than the first (c-15, 58.60/s), while the second c-50 was
  better than the first. Run length scales only the end value: the lag grows linearly, and the last two runs were
  77-78 s against 70 s. Receive pacing: the arrival jitter (`frameJitterPeakMs` 14-17 ms, `gapP95` 26-30 ms) is the
  same at every bitrate, including 100 Mbit/s where the clock was fine.
- **HYPOTHESIS: why the console sends fewer frames at 15-50 Mbit/s.** Its encoder probably drops frames under rate
  control on large frames. It sent 63-67 % of its own target at 15-50 Mbit/s and 46 % at 100 (9.72 / 14.56,
  19.04 / 29.13, 30.44 / 48.54, 44.75 / 97.09 Mbit/s). The scene was not recorded ("scene not asked"), so a scene difference between
  runs cannot be excluded.
- **CODE: the picture was not delayed by this lag** (per-frame re-anchoring, above). The 1.3-1.5 s was added to the
  sound only.

### Fixes (commit d0a4341f; unverified on Windows)

1. **Default (a latency fix):** `RemotePlaySessionSource::videoPresented` now hands the audio schedule
   `max(PTS, arrival)` of the shown real frame on the session axis (`include/veyra/remoteplay/AudioPictureTerm.h`).
   Generated frames carry the last real frame's correction. Arrivals that cannot belong to the frame (not finite,
   before the session's origin, after the present) are ignored; the lag itself is not bounded (a 5 s bound in the
   first version switched the fix off after 133-899 s: review, fixed; test: 1200 s at the four rates, target <= 6.5 ms
   throughout). It acts at every console rate, also at 60 (there audio becomes the few ms of the clock's residual lag
   earlier), and `[live-audio-sync] videoPtsMs` / `skewMs` now carry the corrected time.
   - It never makes audio later than before: the time handed over is never below the PTS, so the target is never
     above the old one. Test: 200 random sequences through the real `CaptureSyncTarget`, new target <= old target at
     every observation.
   - With the PTS behind the arrival, the target is the local picture delay (present - arrival). Audio PTS is
     arrival-anchored on the same axis, so sound keeps the relation it arrived with.
   - Tests: the production clock at the Package 9 rates for 70 s and for 1200 s (old target at the cap, new target
     5-6.5 ms, the picture's own delay); the Linux audio model (`AudioPathModel.h`, `pictureFix`) at 57.46-59.84 frames/s: 0 restarts instead of
     9-22, latency and A/V offset never above the old rule's; with a true timestamp the run is identical. These are
     models, not measurements.
   - `VEYRA_TEST_RP_LEGACY_AUDIO_PICTURE=1` gives the old term back for A/B. The own-clock experiment is unchanged.
2. **Experiment `VEYRA_EXP_RP_CLOCK_RATE=1` (off):** the clock follows its envelope at 10 % of a frame per frame, so the
   PTS lag stays bounded for any console rate within 10 % of nominal (tests: 21-37 ms at 58.6 /s, 89-104 ms at
   54.6 /s, 0-16 ms at 59.94 /s; the PTS step stays within 10 %). It is off by default because the PTS reaches frame
   generation, duplicate removal and pacing repair. It is not needed for the audio fix.
3. **Always on, diagnostics:** `[remoteplay-clock] wireFps= clockRate=`, the console's own frame rate per line (about
   10 s), and `[remoteplay-audio] pictureTerm=arrival|pts|own-clock` once per stream start.

## 2. Decoder resets at 30 Mbit/s

Both runs have the same signature in `[remoteplay-transport]`: `callbackRejected` 0 → 9, `upstreamWarnings` 2 → 31,
`assemblyErrors` 0 → 29, then a keyframe (`idrRequests` +1) and `[frame-flow] epoch=2`. There was no packet loss, no FEC
failure, no decoder warning or error line (the only WARN lines of all 8 logs are the haptics line), and
`fallback=false`.

**CODE: what the signature is.** A wait for a keyframe opened by the video queue (`VideoIngress::recoverLocked`): the
queue is dropped and every later non-keyframe is refused (`PushStatus::WaitingForIdr`). The callback then returns false
for those frames (`callbackRejected`), and Chiaki answers each refusal with "Video callback did not process frame
successfully", "Detected missing or corrupt frame(s)" and a reference change ("Missing reference frame ... changed
to"). That is about 3 warnings per refused frame, which `ChiakiBackend` counts as assembly errors, plus corrupt-frame
reports to the console, until the keyframe arrives (`waitingIdrMs=177`, 9 frames at about 58/s). So the 29 "assembly
errors" are a consequence, not a cause.

- **c-30@131516, at the start (13 dropped). CONFIRMED:** the episode opened in the first second (epoch 2 at 22.546,
  1.0 s after `session started`). 13 = 22 received - 9 decoded at 22.538. The first D3D12VA output took 209 ms
  (`decoder opened` 22.177 → `[remoteplay-decode] actual=` 22.386), against 38-41 ms in the other seven runs.
  **HYPOTHESIS (strong):** the 4-frame ingress queue overflowed while the owner was inside that first decode: 13 = 4
  queued + 9 refused, which is exactly a full queue. The test replays it on the real queue: `overflow=1 depthAtLast=4
  refusedWaiting=9 dropped=13`. Not fixed: a start-up-only cost of about 150 ms, and keeping more frames would add
  delay. The new counters will show whether it recurs.
- **c-30@132145, at 66 s (10 dropped). CONFIRMED:** `frameDrops=10`, `ingressDropped=10`, `recoveries=1`,
  `gapMaxMs=188.3`, `waitingIdrMs=177`, network `frameJitterPeakMs` 17-31 ms, `framesLost=0`. 10 = 1 frame waiting + 9
  refused, so one frame was queued when the wait opened. **HYPOTHESIS (strong):** the stale-clock rule. The owner read
  its clock (`RemotePlaySource::read`, `now`), then took the inbox lock (`SessionInbox::tryVideo`) only after a frame
  stamped later had been pushed. `VideoIngress::expiredLocked` calls such a frame expired (`now < arrival`, the
  original rule; the "expiry race" of the experiments doc) and opens the wait. The other rules are excluded:
  - a real 100 ms age or a full queue needs 4+ frames waiting (about 67 ms of a stalled owner), and only 1 was;
  - a gap or a lost frame needs Chiaki to report one (`framesLost=0`, `fecFailed=0`);
  - a decoder failure logs a warning (none).

  A refused malformed frame (one callback-level refusal + 2 queued) fits the counts too but needs a PS5 frame that
  the NAL check refuses; nothing points to it.
- **Fix (commit 2343d6db):** `RemotePlaySessionSource` sets `QueueLimits::expiryFix` by default, so a frame stamped
  after the owner read its clock is decoded instead of opening a keyframe wait. Both upstreams (Veyra, PushFPS) ship
  this rule.
  - It cannot add delay: the only case it changes is the one where the old rule dropped the queue and waited about
    180 ms for a keyframe. A frame really older than 100 ms is still dropped.
  - `VEYRA_TEST_RP_LEGACY_EXPIRY=1` keeps the old rule for A/B.
  - Test: the c-30@132145 sequence on the real queue: old rule `staleClock=1 depthAtLast=1 refusedWaiting=9
    dropped=10`, new rule no episode and every frame decoded.
- **Diagnostics (always on, one line only when a count changes):** `[remoteplay-ingress] causes` names what opened
  each wait: malformed, config, gap, lost, tooOld, tooLarge, overflow, expired, staleClock, backlogOverdue, decoder.
  It also gives `depthAtLast`, `refusedWaiting`, `staleClockTolerated` (looks that the old rule would have turned
  into a reset) and `callbackRejected`. The next run names the trigger instead of leaving it to inference.
- Not bitrate-related on this evidence: one start-up stall and one race, both in 30 Mbit/s runs, out of 8 runs.

## 3. Presentation measurement

**CONFIRMED (code + logs): "presents dated 100 %" did not mean dated.** The scan-out audit's `namedPct` is the share of
presents whose id the DXGI frame statistics named (`PushScanoutAudit.h`). They named every one (`stepP50=1`), but the
statistics' vblank time stood still: `statsAgeMsP50` 100-500 ms (up to 2 s) in the steady state, with
`SyncRefreshCount == PresentRefreshCount` (`exact = named`). A present named with a vblank time older than its own
Present call gives a negative value, which `PresentSink::sampleDisplayLatency` discards. So only presents followed at
once by a statistics update keep a value.

In that state `[slipstream-latency] scanoutKnown` was 0-30 of about 295 presents per 5 s, that is 0-16 % per window and
1-4 % typical
(c-50@131634 after its first window: 28, 8, 10, 9, 8, 11, 30, 7, 5, 8, 9, 15 of 293-298). `[display-latency] samples` was 1-12 a second, the
kit's "dated frames/s 2-3".

Two regimes are visible in the same runs:
- 49-60 dated presents a second with P50 2-15 ms: the first 2-4 s of every run, and c-15@131353 from 18 s to 40 s;
- 1-12 dated a second with P50 0.1-1.5 ms: the rest.

So the steady "present to screen P50 0.43-0.92 ms" describes a 1-4 % sample selected by when the statistics updated.
It is not screen latency: DXGI dates the vblank, not light (no panel processing, no pixel response). The composition
mode was unknown (`IDXGISwapChainMedia` E_NOINTERFACE, `compositionMode=-1`) and PresentMon was absent, so neither the
regime nor the bias can be settled from these logs.

**HYPOTHESIS:** the second regime is the variable-refresh independent-flip path, in which the statistics' vblank
counter does not advance per refresh. Its sub-millisecond values may be right for the dated presents, but they are
not shown to represent the others.

**Fix (commit 6e10d6e0): the numbers say what they are.**
- `[slipstream-latency] scanoutKnownPct=` gives the dated share of each window.
- The kit (one `measures.csv`, both readers) now says "present to vblank P50 (dated presents)", "presents dated per
  second", "share of presents dated", "presents named (not a date)" (formerly "presents dated") and "arrival to
  vblank P50 (dated presents)", in groups "vblank first 3 s" / "vblank steady".
- The footnote (identical in `run-tests.ps1` and `report.py`) says: only the dated presents, not screen latency, not
  input-to-photon, and that sub-millisecond values are not a proven screen delay; PresentMon (MsUntilDisplayed) or a
  camera decides.
- The presentation session's text says "present to vblank ... dated presents only, DXGI statistics, not screen
  latency".
- The sampling itself is unchanged: DXGI gives no date for the other presents, and inventing one would be a model.

## What the retest must show (for stream "clarity"'s session: 50 and 100 Mbit/s, 3+ runs each)

Arms, all with `VEYRA_EXP_RP_STATS=1`: the default (this branch), `VEYRA_TEST_RP_LEGACY_AUDIO_PICTURE=1` (Package 9's
audio rule), `VEYRA_EXP_RP_CLOCK_RATE=1`, and `VEYRA_TEST_RP_LEGACY_EXPIRY=1`. Log lines needed:
- `[remoteplay-clock] wireIndex arrivalMinusPtsMs wireFps clockRate`;
- `[remoteplay-audio]` (`delayMs targetMs hardResets`) and its one `pictureTerm=` line;
- `[remoteplay-ingress] expiryRule=` and `causes ...`;
- `[remoteplay-health] decoderResets frameDrops`;
- `[slipstream-net] fps recoveries ingressDropped waitingIdrMs gapMaxMs`;
- `[remoteplay-transport] callbackRejected assemblyErrors`;
- `[slipstream-latency] scanoutKnown scanoutKnownPct`;
- `[slipstream-scanout] statsAgeMsP50`;
- PresentMon `PresentMode` / `MsUntilDisplayed`.

1. Item 1 fix: in the default arm, `targetMs` and `delayMs` stay near the picture's own delay plus about 20 ms (tens of
   ms) for the whole run whatever `wireFps` is, with `hardResets` near 0 after the first window. The legacy arm
   repeats Package 9 wherever `wireFps` < 59.7. Lip sync is judged by ear on a cutscene in both arms. One run of at
  least 10 minutes in the default arm shows that the target stays flat however large `arrivalMinusPtsMs` grows.
  `VEYRA_TEST_RP_LEGACY_TIMING` does not restore Package 9's audio term or expiry rule: use the two legacy switches.
2. The relation: `arrivalMinusPtsMs` slope = `1000 - 16.75 x wireFps` per window in the default and legacy arms
   (bounded with `CLOCK_RATE=1`). Whether `wireFps` < 59.7 occurs at 50 and 100 Mbit/s in this scene is the
   bitrate question; the scene must be the same moving scene in every run.
3. Item 2 fix: `causes` shows `staleClock=0` in the default arm and `staleClockTolerated` > 0 whenever the race occurred
   (each one a keyframe wait avoided). Any remaining episode is named by its cause. In the legacy-expiry arm a
   `staleClock` episode should appear only rarely: about 1 in 600 s was seen in Package 9.
4. Item 3: PresentMon's `MsUntilDisplayed` and `PresentMode` over the same seconds as `scanoutKnownPct`. If PresentMon
   shows independent flip with values near the dated sample's, the sample is representative; if not, the DXGI
   present-to-vblank figures stay labelled as a sample and are not used as latency.

## Not done

- The startup overflow (c-30@131516) is not changed: no fix that keeps more frames can avoid adding delay.
- Nothing in Chiaki was changed. The 29 warnings follow from our refusals; they are not a library fault.
- The kit's MEASURED VALUES gain "stream rate: console frames sent per second" (`wireFps`, median). It is n/a for the
  Package 9 logs, which predate the field; the new table gives the kit's own Package 9 values for every other row
  (checked with `report.py`'s `measure_cell` on the eight logs).
- The kit's `slope-min` of the clock lag still spans a decoder reset. The default audio rule no longer depends on
  that lag, but the row's meaning across a reset is not fixed.
