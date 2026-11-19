# PS5 Remote Play audio path (Slipstream)

Code reading of `exp/dev` @ `13cf3b1f` and the pinned chiaki-ng `lib/` (`/home/claude/vendor/chiaki-ng`);
round 2 (model, tests, three fixes) on `exp/rc3`, 19 November 2026.
Nothing in this section was built, run or measured: every delay below is an **estimate** from the code.
Unverified on Windows.

## Stages

| # | Stage | Where | Capacity | Added delay, steady | Added delay, after a stall |
|---|---|---|---|---|---|
| 1 | Chiaki audio jitter buffer | `lib/src/audioreceiver.c:9-10,171-246` | 8 Opus frames; prefill 3 once per session | 0: in-order frames are delivered in a loop as soon as they arrive (only the first 2 frames of a session wait for the 3rd) | a missing frame is concealed (Opus PLC) once 3 newer frames are buffered: about 3 frames (frame size comes from the console's audio header; 10 ms is typical, not verified). After N lost frames the loop emits N concealed frames in one burst |
| 2 | Opus decode, `PcmBlock` stamped with arrival time | `ChiakiBackend.cpp:209-219` (Takion thread) | 1 frame | decode time | - |
| 3 | `AudioIngress` (inbox) | `VideoIngress.cpp:137-163`, `VideoIngress.h:88` | 100 ms, drop oldest | 0 (feeder drains it) | a burst over 100 ms drops the oldest PCM and marks a discontinuity |
| 4 | Feeder thread | `RemotePlaySessionSource.cpp:229-240`, `RemotePlaySource.cpp:467-508` | 480 frames per pull | <= 2 ms polling sleep + scheduler | - |
| 5 | `CaptureAudioSession` input deque + swresample (48 kHz -> 48 kHz, drift compensation) | `CaptureAudioSession.cpp:104-127,193-277` | 2 s for deque + PCM (`maxQueuedMs`, :60) | resampler filter delay (`swr_get_delay`, small); session waits <= 2 ms (:172) | overflow at 2 s: everything cleared, reset |
| 6 | PCM playout queue (A/V sync) | `CaptureAudioSession.cpp:287-392` | <= 1500 ms target (`maxAutoDelayMs`) | the video's presentation delay minus the audio path's own delay; floor = 20 ms reserve (startup prefill :314, reservoir rule `captureSafeCorrectionPpm`) | see findings 2-4 |
| 7 | WASAPI endpoint | `CaptureAudioSession.cpp:154`, `WasapiAudioSink.cpp:755-852,878-993` | shared mode, event-driven, `AUTOCONVERTPCM` + `SRC_DEFAULT_QUALITY`, 20 ms requested (actual logged as `actualBufferMs`) | the pump fills all free space each event, so up to the whole buffer (about 20 ms) + Windows engine and driver latency (unmeasured) | an empty endpoint plays silence; live sources never pad (`ReleaseBuffer(0)`), a 5 ms fade-in follows each gap |
| 8 | Device | - | - | USB: driver dependent; Bluetooth: codec and radio latency, not reported by `IAudioClock` (finding 9) | - |

Never exclusive mode. Reconnection stops and restarts the whole audio session per attempt
(`RemotePlaySessionSource.cpp:226,361`).

## How A/V sync works today

- Audio PTS: anchored to the arrival time of the first block after a discontinuity, then counted in samples
  (`RemotePlaySource.cpp:479-491`). Video PTS: arrival times, smoothed by a lower-envelope PLL
  (`Timeline.cpp:29-55`).
- The engine reports every presented frame, generated ones included, as `videoPresented(pts, hostNow)`
  without an arrival time (`EngineController.cpp:3115`). The video delay `host - pts` therefore already
  contains decoding, frame generation's pair delay (a generated frame is shown about half an interval after its
  pts, so is the real frame after it) and the present queue, but not scan-out.
- Auto mode (default): `target = (vHost - vPts) - ingress`, `mapping = ingress + target`
  (`CaptureAudioSession.cpp:293-302`), so a sample with PTS `p` should be playing at host time `p + mapping`.
- Every 250 ms the renderer's device-clock media time is compared with that (`:366-392`): error over 60 ms ->
  fade (<= 80 ms) and restart; otherwise swresample compensation of at most 5000 ppm, slewed 250 ppm per step.
- Video frames dropped: no effect while frames keep coming. No presentation for 500 ms: corrections stop
  (`fresh`, :290); a graph rebuild calls `videoReset`, which resets audio in auto mode.

## Findings

1. **Console/PC clock drift accumulates in auto mode** (code reading; size depends on the real drift,
   unmeasured). Audio PTS are sample-counted and `ingress` cancels out of `mapping`, which stays `vHost - vPts`.
   A relative drift `d` therefore moves the audio queue by `d x t` until a discontinuity: at 100 ppm, 6 ms
   per minute. Console faster: audio delay grows until the 2 s overflow resets it. Console slower: the queue
   reaches the 20 ms floor, the correction is blocked by the reservoir rule, and once the error passes 60 ms
   the session fades and restarts about once a second. Manual and Off modes follow `ingress` and do not drift.
2. **Slow recovery from small excess delay.** A stall shorter than the 30 ms starvation grace (or a late
   burst) leaves audio up to 60 ms late; it decays at <= 5 ms/s, and the 250 ppm-per-250 ms slew needs about
   5 s to reach that rate.
3. **Hard reset above 60 ms:** fade, endpoint stop, silence until the next block is due: audible.
4. **Starvation >= 30 ms** fades, clears and re-anchors with expired PCM discarded: no accumulation, but an
   audible gap and a restart (`:345-356`).
5. **Loss bursts become resets.** Chiaki conceals every lost frame and delivers them in one burst; a burst
   over 100 ms is cut by `AudioIngress`, which marks a discontinuity and resets the session.
6. **Audio floor about 20 ms + endpoint.** If the video's own delay is lower, audio plays later than the
   picture; this is not flagged as `limited`, only logged in `capture-audio-reserve`.
7. **No filtering of the video delay for Remote Play:** without an arrival time `CaptureSyncTarget` resets
   on every observation (`CaptureSyncTarget.h:16`), so the target is the last frame's delay.
8. **Device changes recover, slowly.** Default-device change (eConsole), the chosen device leaving or
   returning, and any write error invalidate the endpoint (`WasapiAudioSink.cpp:707-742`); the session
   waits 500 ms before reopening (`CaptureAudioSession.cpp:131`), dropping PCM meanwhile, then prefills again.
9. **Bluetooth headsets play late by their link latency** (general knowledge: often 100-200 ms for A2DP;
   not measured here). `IAudioClock` does not include it, so auto sync cannot see it; only the manual
   offset compensates.
10. **Underrun:** a single missed period stays silent (no synthetic PCM), with a 5 ms fade-in after the gap;
    persistent starvation follows finding 4.
11. **A loss stall of 100 ms or more leaves audio about 100 ms late for good** (code reading, reproduced in
    the Linux model below). Chiaki conceals the lost frames and delivers them in one burst; `AudioIngress`
    keeps the newest 100 ms and marks a discontinuity; `pullAudio` anchors the PTS of the oldest kept block
    to the burst's arrival time (`RemotePlaySource.cpp:480-486`). Every later block is then stamped up to
    100 ms after its real arrival, and auto mode cannot see it (finding 1: `ingress` cancels). It lasts
    until the next discontinuity.

## Controller (`include/veyra/sink/AudioLatencyController.h`, experiment `VEYRA_EXP_RP_AUDIO_FAST_RECOVER`)

- `AudioVideoDelayFilter`: median of the last 9 presented frames' `host - pts` (generated frames included,
  so frame generation's pair delay is followed), entries older than 500 ms ignored.
- `AudioArrivalDrift`: change of the ingress lower envelope against a reference learned in the session's
  first 2 s, added to the schedule. Slewed at 2 ms/s in steady state (fixes finding 1); taken at once for
  250 ms after a discontinuity (fixes finding 11). Logs a drift estimate (ppm) every 10 s window.
- `AudioLatencyController`, once per endpoint period: swresample ratio up to the original 5000 ppm
  (no larger pitch change), 500 ppm per ms of error, slewed 20000 ppm/s, never speeding up into the 20 ms
  reserve; set again every 250 ms while not zero (`AudioRatioArming`: swresample ends a compensation after
  its 48000-sample distance). Excess delay (late audio above a 2 ms dead band, limited to what the reserve
  can give) is removed from the head of the queue with a 2.5 ms crossfade: the whole excess at once within
  150 ms of an endpoint gap (masked by the dropout and fade-in that just played); otherwise windows of up
  to 10 ms only where the head is quiet (RMS under about -49 dBFS); and, when 15 ms or more has lasted
  150 ms, a 5 ms cut in sound at most every 100 ms (at most 50 ms/s). A cut is heard only after the
  endpoint has played its padding, so each cut stays "pending" and is added to the measured error until
  then (padding + 12 ms margin); without this the controller cut again every period and ended early.
  Fade-and-restart only when 400 ms late or 60 ms early (inserting silence is the only way to delay
  audio quickly), at most once a second.
- Round 2 (found by the end-to-end model below): a device clock that stands still while the endpoint is
  full (the raw error falls with time, by 80-120 % of it, for 30 ms or more, judged against an anchor
  with 3 ms slack) holds the controller: no cut, no ratio change, and pending cuts' deadlines move with
  it. When the clock runs again (less than half the elapsed time came off the error over 10 ms) the
  added delay is removed at once, as after an endpoint gap (the listener has just heard the dropout).
  `AudioArrivalDrift::rebase` inside the first 2 s ends the learning, so a re-anchor there is never taken
  as the reference.

Wiring (`CaptureAudioSession.cpp`, PS5 Remote Play only, switches in `docs/PS5_REMOTE_PLAY_EXPERIMENTS.md`):
the controller replaces the 250 ms correction in auto mode; drops go through `dropHeadLocked` (quiet
check on the removed window plus the crossfade). Device changes: the existing re-open paths (default-device
notification, chosen device leaving or returning, any write error) were checked and kept; only the first
reopen delay changes. Every reset in the fast path rebases `AudioArrivalDrift` (round 2: a burst re-anchor that arrives while
the endpoint is being reopened is dropped by `push()` with its discontinuity flag, so the reset after the
reopen did not snap to it). Cuts are clamped to the PCM actually queued beyond the fade; the quiet scan covers
only the cut and its fade (at most about 600 frames, under the session mutex). Low buffer:
`AudioRenderer::requestLowLatencyPeriod`; abandoned for the rest of the stream (one log line) when a step
after creating the low-latency stream fails, or after 8 endpoint underruns while PCM was arriving.

## Linux model (not a measurement)

Two models, both run by `tests/remoteplay/CoreTests.cpp`:

- `include/veyra/sink/AudioPathModel.h` (round 2, the one the tables below come from): virtual time in
  1 ms steps, driving the real decision code (`AudioLatencyController`, `AudioArrivalDrift`,
  `AudioVideoDelayFilter`, `AudioRatioArming`, `CaptureSyncTarget`, `ArrivalClockMapping`,
  `captureSafeCorrectionPpm`) with a model of everything around it: console clock (ppm offset plus a
  sinusoidal wander), 10 ms Opus frames, sound as loud/quiet per ms (tone = always loud, speech, music,
  sparse); network base delay, jitter, random loss, loss stalls (Chiaki conceals when the 3rd later frame
  arrives and delivers the run in one burst), held stalls, reconnects (a new audio session); the 100 ms
  `AudioIngress`, the feeder's 2 ms poll and PTS anchoring, the session's 2 s overflow, reset, prefill,
  expired-PCM discard, 30 ms starvation rule and 50 ms PTS-jump rule; libswresample compensation (whole
  samples per 48000, so 20.8 ppm steps, ending after 48000 output samples unless set again); the
  endpoint (20 ms / 10 ms period, or 6 ms / 3 ms with the low buffer), its own crystal in ppm and latency
  after its clock that nothing can see (Bluetooth); device switch, unplug until replug (reopen every
  500 ms, the first after 50 ms with fast recovery), format change to 44.1 kHz (no low-latency period),
  channel count change, device clock stall, audio thread stall; the video delay (60 presentations/s, 120
  with frame generation, whose +17 ms is switched with a 200 ms rebuild pause and `videoReset(false)`).
  Not modelled: sample values (the splice itself is tested on a tone, a chirp and noise), Windows engine
  scheduling beyond the explicit stalls, swresample's filter delay (about 0.3 ms), and anything real
  hardware does that is not in this list.
- The round-1 model in `AudioSyncTests.inc` stays as a cross-check of the same claims.

`tools/remoteplay_latency_bench/audio_sim.cpp` prints `audio_sim_results.json` (the same bytes for the same
build; about 10 minutes on one shared core; `--quick` shortens the long runs, `--fuzz` runs 100 random
mixes). |A/V| = |play - (arrival lower envelope + true video delay)|, median / P95 / max in ms after the
first 3 s; "silent" = ms with nothing playing while stream and device were up; "recover" = longest time
from the end of an event until |A/V| <= 8 ms for a whole second ("never": not within 20 s; windows
shorter than 20 s, before the next event, are not judged); "cut" = ms
the controller removed. Today = switches unset; fast = `VEYRA_EXP_RP_AUDIO_FAST_RECOVER=1`; low = fast +
`VEYRA_EXP_RP_AUDIO_LOW_BUFFER=1` (its gain is engine and device latency, which the model lacks, so it
only shows where the shorter buffer hurts or its 14 ms reserve helps).

| Scenario | Today \|A/V\| | Today silent, resets, recover | Fast \|A/V\| | Fast silent, resets, cut, recover | Low \|A/V\| |
|---|---|---|---|---|---|
| steady, video 35 ms | 1.0 / 1.1 / 1 | 0, 0, 0 | 1.0 / 1.0 / 1 | 0, 0, 0, 0 | 1.0 / 1.0 / 1 |
| steady, video 15 ms | 1.0 / 1.1 / 1 | 0, 0, 0 | 1.0 / 1.0 / 1 | 0, 0, 0, 0 | 1.0 / 1.0 / 1 |
| steady, video 10 ms (below the 20 ms reserve) | 5.0 / 5.0 / 5 | 0, 0, 0 | 5.0 / 5.0 / 5 | 0, 0, 0, 0 | 1.0 / 1.6 / 2 |
| steady, frame generation on (+17 ms, 120 presentations/s) | 1.0 / 1.1 / 1 | 0, 0, 0 | 1.0 / 1.0 / 1 | 0, 0, 0, 0 | 1.0 / 1.0 / 1 |
| console clock +50 ppm (wander 10), 2 h | 180.8 / 343.2 / 361 | 0, 0, 0 | 1.1 / 2.0 / 2 | 0, 0, 92, 0 | - |
| console clock -50 ppm (wander 10), 2 h | 19.5 / 24.2 / 25 | 0, 0, 0 | 0.8 / 1.5 / 2 | 0, 0, 0, 0 | - |
| console clock +200 ppm (wander 40), 2 h | 721.2 / 1369.8 / 1440 | 0, 0, 0 | 1.4 / 2.1 / 2 | 0, 0, 470, 0 | - |
| console clock -200 ppm (wander 40), 2 h | 20.0 / 24.4 / 25 | 0, 0, 0 | 0.3 / 0.6 / 2 | 0, 0, 0, 0 | - |
| console clock +500 ppm (wander 100), 2 h | 1466.6 / 1467.2 / 1467 | 0, 0, 0 | 2.0 / 2.5 / 3 | 0, 0, 1130, 0 | - |
| console clock -500 ppm (wander 100), 2 h | 20.4 / 24.6 / 26 | 0, 0, 0 | 1.5 / 2.1 / 3 | 0, 0, 0, 0 | - |
| console clock +200 ppm (wander 40), 6 h | 1465.8 / 1466.5 / 1467 | 0, 0, 0 | 1.4 / 2.1 / 2 | 0, 0, 1450, 0 | - |
| console clock -500 ppm (wander 100), 6 h | 20.4 / 24.6 / 26 | 0, 0, 0 | 1.5 / 2.1 / 3 | 0, 0, 0, 0 | - |
| device clock +300 ppm | 0.9 / 1.1 / 2 | 0, 0, 0 | 0.3 / 0.4 / 1 | 0, 0, 0, 0 | 0.3 / 0.4 / 1 |
| device clock -300 ppm | 2.0 / 2.1 / 2 | 0, 0, 0 | 1.1 / 1.1 / 1 | 0, 0, 0, 0 | 1.1 / 1.1 / 1 |
| Bluetooth-like device, 150 ms after its clock | 1.0 / 1.1 / 1 | 0, 0, 0 | 1.0 / 1.0 / 1 | 0, 0, 0, 0 | 1.0 / 1.0 / 1 |
| loss stall 100 ms every 30 s, 1 h | 91.1 / 92.0 / 93 | 4350, 120, never | 1.0 / 1.1 / 9 | 11607, 232, 568, 435 | - |
| loss stall 200 ms every 30 s, 1 h | 91.1 / 92.0 / 93 | 15655, 195, never | 1.0 / 1.1 / 9 | 23602, 232, 568, 56 | - |
| loss stall 500 ms every 30 s, 1 h | 91.1 / 92.0 / 93 | 51657, 194, never | 1.0 / 1.1 / 9 | 59602, 234, 571, 449 | - |
| loss stall 1000 ms every 30 s, 1 h | 91.1 / 92.0 / 93 | 111649, 194, never | 1.0 / 1.1 / 9 | 119604, 237, 565, 185 | - |
| loss stall 5000 ms every 30 s, 1 h | 91.1 / 92.0 / 93 | 591660, 191, never | 1.0 / 1.1 / 9 | 599603, 230, 574, 125 | - |
| held stall 25 ms every 30 s, 1 h | 1.0 / 1.1 / 1 | 0, 0, 0 | 1.0 / 1.0 / 1 | 0, 0, 0, 0 | - |
| held stall 60 ms every 30 s, 1 h | 8.6 / 43.1 / 43 | 3453, 0, 15263 | 1.0 / 1.1 / 31 | 3561, 0, 3516, 89 | - |
| held stall 200 ms every 30 s, 1 h | 98.2 / 99.0 / 100 | 12047, 187, never | 1.0 / 1.1 / 8 | 23602, 237, 541, 35 | - |
| held stall 1000 ms every 30 s, 1 h | 98.2 / 99.0 / 100 | 108054, 187, never | 1.0 / 1.1 / 7 | 119601, 237, 556, 35 | - |
| held stall 5000 ms every 30 s, 1 h | 98.2 / 99.0 / 100 | 588059, 185, never | 1.0 / 1.1 / 9 | 599607, 230, 570, 145 | - |
| random loss 1 % | 1.1 / 14.2 / 22 | 53, 0, 0 | 0.9 / 1.1 / 11 | 80, 0, 79, 0 | 1.0 / 1.1 / 8 |
| random loss 5 % | 10.6 / 23.6 / 35 | 381, 0, - | 1.0 / 3.0 / 31 | 1897, 0, 1915, 0 | 1.0 / 2.0 / 28 |
| loss stall 100 ms every 3 s, 10 min | 91.1 / 92.1 / 93 | 6984, 194, - (events 3 s apart) | 1.0 / 4.6 / 9 | 18794, 330, 914, 455 | 0.8 / 2.1 / 9 |
| reconnect 3 s | 0.8 / 1.1 / 1 | 38, 2, 38 | 1.0 / 1.0 / 1 | 38, 2, 0, 38 | 1.0 / 1.0 / 1 |
| reconnect, loss stall 1 s later | 90.6 / 91.1 / 92 | 259, 4, never | 2.0 / 2.1 / 7 | 235, 3, 4, 35 | 2.0 / 2.1 / 7 |
| default device switch (tone) | 0.2 / 0.6 / 1 | 38, 1, 538 | 0.2 / 1.0 / 1 | 37, 1, 0, 87 | 0.2 / 1.0 / 1 |
| unplug 4 s, replug (tone) | 0.2 / 0.4 / 1 | 37, 1, 37 | 0.3 / 1.0 / 1 | 38, 1, 0, 88 | 0.2 / 1.0 / 1 |
| format change to 44.1 kHz, no low-latency period (tone) | 1.0 / 1.2 / 1 | 38, 1, 538 | 1.0 / 1.0 / 1 | 37, 1, 0, 87 | 1.0 / 1.0 / 1 |
| channel count change (reopen, tone) | 0.4 / 1.0 / 1 | 38, 1, 538 | 1.0 / 1.0 / 1 | 37, 1, 0, 87 | 1.0 / 1.0 / 1 |
| device switch mid-cut, after a clock stall (tone) | 0.2 / 0.6 / 200 | 38, 1, 538 | 0.3 / 1.1 / 201 | 38, 1, 200, 88 | 0.2 / 1.1 / 201 |
| device switch mid-ratio, FG on (tone) | 1.1 / 1.9 / 17 | 56, 1, 556 | 1.0 / 1.1 / 17 | 55, 1, 0, 105 | 1.0 / 1.1 / 17 |
| device switch during a 1 s network stall (tone) | 91.3 / 91.7 / 92 | 521, 3, never | 0.3 / 1.9 / 7 | 946, 3, 0, 35 | 0.3 / 1.9 / 7 |
| unplug during a held 2 s stall (tone) | 99.0 / 99.1 / 99 | 498, 3, never | 1.0 / 2.1 / 6 | 446, 3, 0, 35 | 1.0 / 2.1 / 6 |
| device clock stall 300 ms (tone) | 0.8 / 13.4 / 17 | 1, 1, 4687 | 1.0 / 1.1 / 301 | 0, 0, 300, 29 | 1.0 / 1.1 / 301 |
| device clock stall 100 ms every 30 s, 1 h (tone) | 1.0 / 15.1 / 101 | 120, 120, 10450 | 1.0 / 1.1 / 101 | 0, 0, 11939, 29 | 1.0 / 1.1 / 101 |
| device clock stall 100 ms every 3 s, 20 min (tone) | 5.8 / 41.8 / 105 | 394, 394, 1538 | 1.1 / 2.0 / 101 | 0, 0, 39243, 29 | 1.1 / 1.9 / 101 |
| audio thread stalls 25 ms every 200 ms for 10 s | 1.0 / 45.9 / 76 | 486, 7, 11357 | 1.0 / 1.1 / 12 | 479, 0, 496, 20 | 1.0 / 1.1 / 23 |
| audio thread stalls 8 ms every 50 ms for 10 s | 1.0 / 1.1 / 1 | 0, 0, 0 | 1.0 / 1.0 / 1 | 0, 0, 0, 0 | 1.0 / 1.1 / 6 |
| FG toggled every minute, 1 h (tone) | 1.0 / 12.8 / 18 | 0, 0, 4943 | 1.0 / 4.1 / 18 | 0, 0, 145, 1821 | 1.0 / 4.1 / 18 |
| FG toggled every minute, 1 h (speech) | 1.0 / 12.8 / 18 | 0, 0, 4943 | 1.0 / 1.4 / 18 | 0, 0, 453, 1821 | 1.0 / 1.3 / 18 |
| everything: +200 ppm, 0.5 % loss, stalls, FG, device events, 1 h | 96.1 / 104.7 / 116 | 33842, 147, - (events 15 s apart) | 1.1 / 2.6 / 28 | 33883, 142, 3301, 2735 | 1.1 / 2.5 / 28 |

Reading it (model, not measurement):

- **Today's rule, documented, unchanged:** clock drift accumulates (finding 1): +50 ppm reaches 360 ms
  late after 2 h, +200 / +500 ppm stop near 1.47 s (the 1500 ms target bound); a slower console leaves
  audio 20-25 ms early (the reserve floor). Every loss or held stall of 100 ms or more leaves audio 91-98
  ms late until the next discontinuity (finding 11: "never" recovers). 60 ms held bursts every 30 s give
  P95 43 ms (finding 2). Frame generation toggles take about 5 s to follow. None of this was changed.
- **Fast recovery:** P95 |A/V| at or below 3 ms in every row except the video-10 ms row (the 20 ms
  reserve floor, as today; only the low buffer's 14 ms reserve gets closer), a 100 ms loss stall every 3 s
  (4.6 ms) and the two FG-toggle rows
  (4.1 ms with a tone, where only 5 ms cuts in sound and the 5000 ppm ratio can follow), no ratchet over 1 h of repeated
  stalls (signed median of the first and last quarter equal within 0.1 ms) and none over 6 h of drift.
  Prices: more silence after loss stalls (today's late audio is a 90 ms cushion that fast recovery does
  not keep, so the next stall finds less queued: 11.6 s against 4.4 s per hour at 100 ms stalls every
  30 s); ratio up to 5000 ppm (8.7 cents, the same bound as today's rule) for up to about 3.5 s after
  frame generation is switched on; masked cuts of the whole excess after an underrun or a device clock
  stall (up to 300 ms in one go here).
- Properties checked on every fast and low run in the table, the tests and 100 random mixes (`--fuzz`): no
  sample played before it arrived, controller state finite, never more than 50 ms per second cut into sound
  outside a masked gap (the budget is pushed to its limit in a unit test), ratio within 5000 ppm, every
  event recovered from.
- **What the random mixes do and do not show.** Fast has the lower P95 |A/V| in 100 of 100 mixes, but
  that is one comparison and mostly one mechanism: every mix holds a stall of 100 ms or more, after which
  today's rule stays 90-98 ms late (finding 11). The costs in the same 100 mixes: fast has more silence in
  25, more endpoint underruns in 75, and single masked cuts up to 308 ms (after an
  underrun or a device clock stall). Frequent trouble costs most: a 100 ms loss stall every 3 s gives
  fast 2.7 times today's silence (18.8 s against 7.0 s in 10 min), and 5 % random loss 5.0 times (1.9 s
  against 0.4 s, 1059 underruns against 108), because fast keeps no late cushion. The `--fuzz` output ends
  with these counts.
- **Low buffer stays a separate, riskier switch.** Its 6 ms endpoint leaves less margin for the audio
  thread (thread-stall rows: worst |A/V| 23 ms against 12, and silence where fast has none at 8 ms
  stalls every 50 ms), and its gain is engine/device latency this model does not contain. Before the
  round-2 review fix (4 below) a 100 ms device clock stall every 3 s gave it 391 resets and 2.1 s of
  silence in 20 min where fast gave none; now 0 and 0 in that row.
- **Not modelled:** Chiaki's start-of-session prefill (the first 2 frames wait for the 3rd, stage 1): a
  possible standing offset of about 20 ms the model cannot show.

Defects the round-2 model found (all fixed, all inside `VEYRA_EXP_RP_AUDIO_FAST_RECOVER`; each has a test
that fails without its fix):

1. `AudioArrivalDrift` learned a discontinuity in its first 2 s as the reference: a reconnect followed
   by a loss stall left audio 90 ms late for the whole session (`audio_arrival_drift_discontinuity_while_
   learning_keeps_the_earlier_reference`, `audio_model_reconnect_then_loss_inside_the_learning_window`).
2. A device clock stall (endpoint full, clock frozen) was recovered with 5 ms cuts in sound over 5 s; and
   quiet cuts made in its first 30 ms expired unheard by the wall clock and were cut again, leaving audio
   up to 55 ms early for 10 s (`audio_latency_controller_holds_while_the_device_clock_stands_still_then_
   cuts_once`, "device clock stall" rows).
3. A burst re-anchor while the endpoint was being reopened lost its discontinuity in `push()`, so the
   drift followed it at 2 ms/s, about 45 s (`CaptureAudioSession.cpp`, one-line hunk; "held stall ending
   while unplugged" in `audio_model_device_events_at_awkward_moments`).
4. (Review of round 2.) With the low buffer the masked cut after a device clock stall is played before
   its pending deadline (6 ms endpoint), is counted twice for a few ms, reads as audio 60 ms early and
   resets after every stall. An early reset now needs the raw error early too
   (`audio_model_low_buffer_survives_device_clock_stalls_every_3_s`). Two tests added for mutations the
   reviewer found surviving: pending deadlines moving with a stall (`audio_model_short_device_clock_
   stalls_recover_at_once`) and the 50 ms/s loud-cut budget (`audio_latency_controller_loud_cut_budget_
   is_reached_and_never_exceeded`).

## On real hardware (when the owner is back)

Same game scene each time, 2 minutes per arm, wired LAN. Run once with nothing set (the stable
behaviour, also upstream Veyra's shipping rule), then with `VEYRA_EXP_RP_AUDIO_FAST_RECOVER=1`, then with
that and `VEYRA_EXP_RP_AUDIO_LOW_BUFFER=1`. What each arm logs:

- nothing set: no `[remoteplay-audio]` lines at all; send `live-audio-sync` (every 2 s),
  `capture-audio-reserve`, `capture-audio-underrun` and `capture-audio-reset` lines;
- fast recovery: the same plus one `[remoteplay-audio]` line every 10 s, whose `driftPpm=` (after 10 s)
  is the only console/PC clock estimate; it is printed only in this arm and the next;
- fast + low buffer: the same plus `low-latency period request ... opened=` and, if it gives up,
  `low-latency endpoint period abandoned`.

None of these lines contain device names or ids.

1. **Wired USB headset.** Lip sync on a cutscene and gunshots against muzzle flash; any crackle, clicks
   or a "warble" (pitch going up and down). Unplug for 3 s and plug back mid-game: how long the silence is
   and whether sync is right afterwards. Switch frame generation off and on twice: does sound drift out of
   sync for seconds?
2. **Lip sync after a network hiccup, on the stable build (nothing set).** Pull the network cable for
   about one second, or wait for a visible stall, then check lip sync for a minute. From the code (PTS =
   arrival of the anchor block + sample count, `RemotePlaySource.cpp:470,479-491`) today's rule should
   leave audio about 90-98 ms late after any stall of 100 ms or more, until the next discontinuity, and
   lets console-clock drift build up unseen over a long session. This is the shipping behaviour, upstream
   too; repeat with fast recovery to compare.
3. **The long masked cut.** With fast recovery, after a USB or Bluetooth stall (a short dropout without
   the network stalling) the whole delay is removed in one cut right after the dropout (up to about 300
   ms in the model): listen for a jump or a click on resume instead of a smooth return.
4. **Bluetooth headset.** Expect it late by its link (often 100-200 ms; auto sync cannot see it, the model
   shows the same). Note whether the manual offset fixes it, and whether the low buffer reports `opened=1`
   or gives up. Walk away until it stutters, come back: clicks, how long until sync returns.
5. **HDMI / monitor speakers.** Many run at a fixed format: look for `opened=0` with the low buffer and any
   reopen. Change the default output device in Windows while playing: silence length, sync after.
6. **Long session, fast recovery arm.** 30 min or more with the `[remoteplay-audio]` lines: their
   `driftPpm=` is the first measurement of the real clock difference the whole model is guessing at.
