# PS5 Remote Play audio path (Slipstream)

Code reading of `exp/dev` @ `13cf3b1f` and the pinned chiaki-ng `lib/` (`/home/claude/vendor/chiaki-ng`);
round 2 (model, tests, three fixes) on `exp/rc3`, 19 November 2026; round 3 (pops after the first real
session, counters, two switches) on `exp/rc4`, 20 November 2026.
Nothing in this section was built, run or measured: every delay below is an **estimate** from the code.
Unverified on Windows.

> **Read first: audio delay builds up over hours on the DEFAULT path (model result, existing behaviour).**
> With every switch unset, the audio clock follows the console's sample count while the video follows
> arrival (finding 1). This is the behaviour inherited from the upstream app, not something added here.
> If the console's clock runs faster than the PC's, the delay of the sound behind the picture grows until the
> 1.5 s limit. In the Linux model: at +100 ppm the 1.5 s limit is reached after **about 4 hours**; at +50 ppm
> the delay is **about 360 ms after 2 hours**. The real clock difference between the owner's console and PC
> is **unmeasured**. `VEYRA_EXP_RP_AUDIO_FAST_RECOVER=1` corrects it (it follows the arrival clock), but test
> it **only together with `VEYRA_EXP_RP_AUDIO_SMOOTH=1` and `VEYRA_EXP_RP_AUDIO_ADAPTIVE=1`**: alone, on Wi-Fi
> bursts, the model shows 2.6x the clicks (9.0 against 3.5 per minute). A session of 30 minutes or more with
> fast recovery gives the first real measurement in its `driftPpm=` lines.
> Fixed (review audio2 item 1): a raise of the reserve asked by SmartLink or ADAPTIVE could on its own trigger
> the "error above 60 ms" fade-and-restart (model: an ask of 80 ms gave 4 resets per hour on the default rule).
> The planned part of the error is now left out of that test (see round 4 below).

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

## Round 3 (sprint 4, 20 November 2026): pops on the first real session

Evidence (owner's PC on Wi-Fi, one session, software counters): pops heard and micro-stutter seen with every
audio experiment off; no packet lost; 13,559 audio callbacks in about 135 s (100 per second: 10 ms Opus
frames at the nominal rate); `[remoteplay-clock] arrivalMinusPtsMs` growing 40-120 ms per 1000 video frames
(a video timestamp rule, not audio); video frames overwritten before pickup 0.7-3.4 % (frames arriving in
clumps). No audio line was logged, so what follows is code reading of `exp/rc4` plus the Linux model.

### What can make a click on the default path (code reading, `exp/rc4` line numbers)

| # | Event | Where | Edge | How often on Wi-Fi-like arrival |
|---|---|---|---|---|
| 1 | **Endpoint underrun: start** | `WasapiAudioSink.cpp:974-1008` (live: `written=got`, `ReleaseBuffer(got)`; the engine plays zeros after the last frame) | **step**: the waveform stops on whatever value the last real sample has (up to full scale); nothing fades it out | every arrival gap longer than what is queued: the queue + endpoint hold about the 20 ms reserve (`:368`, reservoir rule `:480-483`) when the video delay is small, so gaps of about 10-20 ms and more. Model: 14-37 per minute with 20-80 ms bursts, 0 wired |
| 2 | Underrun: resume | `:918-926` (`liveGap_.observe` arms `fadeInRemaining_=240`), `AudioGain.h:21-29` | 5 ms linear fade-in: smooth, **if** the gap was detected | detected through the device clock. Drivers whose clock stops while the endpoint is empty need two empty pumps a period apart (`CaptureAudioDsp.h:50-60`): a shorter gap there resumes **with a step** |
| 3 | 30 ms starvation reset | `CaptureAudioSession.cpp:397-415` | the endpoint is already empty; `fadeAndReset` writes silence (`lastRaw_` path), then a fresh prefill and the gain ramp from 0 (`stopAndReset` sets `smoothedGain_=0`, `:1048`): smooth, but the clump that ends the gap is partly thrown away as expired (`:369-375`): a skip | each gap over 30 ms with no input for 30 ms |
| 4 | 60 ms hard reset (late or early by 60 ms, default 250 ms correction) | `:467-474`, `fadeAndReset` `WasapiAudioSink.cpp:1053-1084` | 5 ms linear fade-out after the queued endpoint data, silence until due, gain ramp in: no step, a dropout | gaps under 30 ms leave audio late by the gap (nothing removes it faster than 5 ms/s, finding 2), so lateness piles up across bursts until 60 ms: a reset every few seconds in heavy bursts (model: 10 per minute at 20-80 ms every 0.5 s) |
| 5 | Expired-audio discard | `:369-375` | before the endpoint starts: inaudible | at every (re)start |
| 6 | PTS jump over 50 ms, discontinuity, overflow | `:227-236`, `:306-310`, `:314-316` | `fadeAndReset` on queued PCM: faded | a loss burst over 100 ms (`AudioIngress`), reconnects; not seen (0 lost packets) |
| 7 | swresample compensation | `rateCorrection.set` `:486`, `CaptureAudioDsp.h:11-27` | polyphase resampling with a ratio up to 5000 ppm: no step | continuous |
| 8 | Opus concealment | chiaki `audioreceiver.c:200-246` (a NULL frame when 3 newer are buffered), `opusdecoder.c:93-95` (`opus_decode(NULL)` = Opus PLC) | PLC: no zero-filled gap. A late frame is never concealed while fewer than 3 newer ones wait; with fewer than 3 buffered and one newer frame, it conceals at once and then drops the late frame (reordering) | only on loss or reordering; the session had no loss |
| 9 | Device period vs block | 10 ms Opus blocks, 2 ms feeder poll, 20 ms requested endpoint, one pump per engine period; input is converted one chunk per loop until the converted queue holds the endpoint capacity + 10 ms (`:330`), the rest waits in `input` | not a step by itself; it is why the cushion is about one period smaller than it looks. The model converted everything at once; `Config::productDrain` now drains as the product does and changes the Wi-Fi rows by under 10 % (test `audio_pop_review_product_drain_is_not_a_second_cause_in_the_model`): not a second cause in the model | - |
| 10 | Sound profile EQ | `WasapiAudioSink.cpp:938-946` | state is kept across a gap and processed before the fade-in: no step | - |
| 11 | Volume and mute | `applyPcmGain` `AudioGain.h:7-10` | 5 ms ramp | user action |
| 12 | Recording tap | `:953-965` | a copy, never the played signal | - |

**Most likely cause of the owner's pops: #1** (with #2 on some drivers): Wi-Fi delivers audio in clumps with
gaps longer than the about 20 ms the default path keeps queued, the endpoint runs dry, and it stops on a
non-zero sample. #3 and #4 add dropouts and skips but are faded. Concealment (#8) is unlikely with 0 lost
packets; the `concealed=` counter now says.

**Video micro-stutter and audio pops share a cause, not a mechanism.** The same clump that empties the audio
endpoint also delivers two video frames within one pickup (the 0.7-3.4 % overwritten frames): both are starved
by the network. Audio does not delay video: the A/V rule only moves audio to match the video's delay
(`:352-355`), never the reverse; Opus decoding runs on Chiaki's receive thread (`ChiakiBackend.cpp:221-231`,
one 10 ms frame, small, with one vector allocation per frame); the presentation path takes the session mutex
only to store the latest video timestamp (`:584`), and the audio owner holds it for copies of at most a few
ms of PCM. By reading, none of these is long enough to cost a video frame; not measured.

### Review of round 3 (independent reviewer, commit `b4f85ac1`): fixes

- **Blocker, fixed:** SMOOTH could make a click. If the owner woke late, after the engine had already run dry,
  the pump saw an empty endpoint and nothing new, and wrote a tail starting at the last sample's value
  straight out of silence (reviewer: a step of 0.92 on a full-scale tone). Now, with no new frames, a tail is
  due only while the endpoint still holds something (`AudioEdgeSmoother::due`). In the model, the same rule
  leaves 0-0.34 clicks/min with SMOOTH (the cut had already happened). The renderer counts gaps that began on
  a real sample with no fade (`unfadedGaps`), printed as `clicks=`.
- **ADAPTIVE grew on any underrun:** a 3 s pause plus one underrun gave 80 ms for 49 s, and an underrun every
  10 s that no reserve could fix held 80 ms for 11 h. Now gaps above 80 ms are ignored, and it grows only when
  the underrun comes within 1 s of an arrival gap that a larger reserve would have covered. The gap is often
  known only after the underrun, when its late packet arrives. Both reviewer cases are tests
  (`audio_pop_review_adaptive_ignores_pauses_and_unexplained_underruns`). The reviewer's 12 h model, before
  this fix: continuous "Wi-Fi typical" mean offset 32.6 ms today against 48.4 ms adaptive (+15.8, flat);
  underruns 14,505 against 668; no ratchet seen; the cap was never exceeded (model result).
- **`pops=` could not show SMOOTH:** `pops=` stays the contract's count (underrun starts + hard resets);
  `clicks=` is new (gaps that began with no fade). Arm A against A+SMOOTH differs in `clicks=` and `tails=`.
- The tail length is now time-based (4 ms at the endpoint rate, `setFadeMs`); the history holds 512 frames.
- The `[remoteplay-audio]` line is formatted and logged after the session mutex is released (values copied
  under it).
- Model input draining: see #9.

### Changes (all off by default; switches in `docs/PS5_REMOTE_PLAY_EXPERIMENTS.md`)

- `VEYRA_EXP_RP_STATS=1`: the `[remoteplay-audio]` line every 10 s with every audio experiment off, with
  `pops=`, `concealed=`, `underrunStarts= hardResets= tails=`, queue P50/P95/min, arrival-gap P95/max,
  endpoint padding min, `playoutTargetMs=` (`AudioStatsWindow`, `include/veyra/sink/AudioPopGuard.h`).
  No new lock on the render path: the window is the audio owner's own; arrival gaps go into a histogram under
  the mutex `push()` already holds.
- `VEYRA_EXP_RP_AUDIO_SMOOTH=1`: `AudioEdgeSmoother` in `AudioRenderer::pumpOnce`. A 4 ms continuation
  (mirror image of the last written frames under a raised-cosine fade-out) is written once per run of real
  data, only when the padding after the write is below one engine period (a gap is then certain: the next
  pump comes a period later), and the next real data is faded in. Real samples are never changed and the
  tail is not in the media timeline. Resets were already faded (#3, #4, #6) and are unchanged.
- `VEYRA_EXP_RP_AUDIO_ADAPTIVE=1`: `AudioAdaptivePlayout` (auto sync only). Starts at today's reserve; after
  an underrun, at least +10 ms and at least the largest arrival gap of the last 10-20 s + 5 ms, at most
  80 ms; after 10 s with no underrun, down at 2 ms/s towards today's reserve (or recent gaps + 5 ms). The
  extra is added to the schedule and to the prefill after a re-anchor (inaudible: it is taken in silence
  that is already there); the rate correction keeps today's reserve so lateness piled up by gaps is still
  removed. With no underrun the extra is 0: a clean link keeps today's delay exactly.

### Linux model, round 3 (not a measurement)

`AudioPathModel.h` gained Wi-Fi-like arrival (windows with nothing delivered, then one clump), clicks (an
underrun starting right after a loud real millisecond), the smooth tail and the adaptive reserve; earlier
rows are unchanged with the switches unset. `audio_sim --pops` -> `tools/remoteplay_latency_bench/audio_pop_results.json`
(today's rule, video delay 15 ms, music-like sound, 600 s, 3 seeds; per minute; latency = play minus
lower-envelope arrival, i.e. the delay audio adds; |A/V| P95):

| Arrival | Rule | clicks/min | underruns/min | resets/min | silence ms/min | latency P50 / P95 ms | \|A/V\| P95 ms | reserve max ms |
|---|---|---|---|---|---|---|---|---|
| wired-like, jitter 1.5 ms | today | 0 | 0 | 0 | 0 | 16.0 / 16.1 | 1.1 | 20 |
| | smooth, adaptive, both | 0 | 0 | 0 | 0 | 16.0 / 16.1 | 1.1 | 20 |
| wired-like, jitter 4 ms | all four | 0 | 0 | 0 | 0 | 16.3 / 16.4 | 1.4 | 20 |
| Wi-Fi light: 20-40 ms bursts every 2 s | today | 14.2 | 17.0 | 0 | 133 | 30.3 / 44.7 | 29.7 | 20 |
| | smooth | 0.03 | 11.7 | 0 | 85 | 30.6 / 43.9 | 28.9 | 20 |
| | adaptive | 0.8 | 1.0 | 0 | 5 | 43.1 / 49.9 | 34.9 | 59 |
| | both | 0.03 | 0.7 | 0 | 3 | 41.9 / 48.5 | 33.5 | 57 |
| Wi-Fi typical: 20-60 ms every 1 s | today | 16.9 | 20.7 | 0.13 | 209 | 47.1 / 65.0 | 50.0 | 20 |
| | smooth | 0 | 14.0 | 0.10 | 145 | 48.5 / 64.7 | 49.7 | 20 |
| | adaptive | 1.0 | 1.3 | 0 | 6 | 63.5 / 71.1 | 56.1 | 79 |
| | both | 0 | 0.9 | 0 | 4 | 62.2 / 69.5 | 54.5 | 75 |
| Wi-Fi heavy: 20-80 ms every 0.5 s | today | 37.0 | 44.6 | 10.0 | 700 | 61.5 / 73.7 | 58.7 | 20 |
| | smooth | 0.34 | 35.1 | 11.3 | 628 | 61.1 / 74.3 | 59.3 | 20 |
| | adaptive | 7.8 | 9.7 | 0.10 | 22 | 77.1 / 83.5 | 68.5 | 80 |
| | both | 0 | 2.1 | 0.10 | 7 | 77.5 / 86.7 | 71.7 | 80 |

Reading it (model):
- On a clean link neither switch changes anything (also a test: equal latency, A/V, zero tails).
- Today's rule on Wi-Fi already plays audio 15-45 ms later than on a wired link: each gap under 30 ms leaves
  audio late by the gap and the correction removes at most 5 ms/s (finding 2). That is a crude adaptive
  buffer with clicks.
- SMOOTH removes almost every modelled click (0-0.34 per minute remain: the endpoint was already empty at the
  pump, the review's late-wake case, where no tail can help) at no steady-state cost (median latency within
  1.5 ms of today's). Each tail can delay the next real data by up to 4 ms: a small latency cost, but not
  zero, paid only at a gap. It shortens the silence (the tail fills part of the gap) but does not prevent
  underruns: a dropout is still a dropout, now without the click.
- ADAPTIVE removes most underruns (5-17x fewer) at +13 to +16 ms median latency and audio behind the
  picture by up to the reserve (|A/V| P95 +5 to +10 ms against today's already-late audio). ADAPTIVE can raise
  the audio/video offset by up to 60 ms (the 80 ms bound minus today's 20 ms reserve). It stays at 80 ms
  under the heavy row; it is the choice for "no dropouts", not for lowest delay. Both together: no clicks
  and the fewest underruns (the tail itself adds a little cushion).
- Not modelled: the video stalling with the same bursts (the model keeps presenting video), drivers whose
  clock stops while empty (#2), the real burst statistics of the owner's Wi-Fi (the rows are guesses), and
  anything on the Windows side beyond the code as read.

Tests (`tests/remoteplay/AudioPopTests.inc`): on a full-scale tone, today's underrun onset is a step of the
last sample's value and SMOOTH leaves no step above the tone's own; the resume after a gap the clock did not
show steps today and not with SMOOTH; today's reset and volume paths are step-free; the tail is due only
when a gap is certain and once per run; the adaptive reserve stays at today's value without underruns,
grows fast and bounded, shrinks at most 2 ms/s, never below the base; the statistics window; and on the
model: the clean link is unchanged by both switches, Wi-Fi bursts click today and never with SMOOTH,
ADAPTIVE cuts underruns at least 4x within the 80 ms bound, and the extra drains back after bursts stop.

### Next session: Wi-Fi vs wired listening checklist

Start the experimental build with `VEYRA_EXP_RP_STATS=1` in every arm (it changes no behaviour), same game
scene, 2-3 minutes per arm, music or speech playing, headphones on a wired output:

| Arm | Switches | Listen for | Counters to read |
|---|---|---|---|
| A | STATS only | the pops as before | `pops=`, `underrunStarts=`, `gapMax=`, `queueMin=`, `paddingMin=`, `concealed=` |
| B | + `VEYRA_EXP_RP_AUDIO_SMOOTH=1` | pops replaced by soft dropouts | `tails=` close to `underrunStarts=` |
| C | + `VEYRA_EXP_RP_AUDIO_ADAPTIVE=1` | fewer dropouts, lip sync slightly later | `playoutTargetMs=` rising after bursts, falling when quiet |
| D | SMOOTH + ADAPTIVE | neither pops nor many dropouts | as B and C |

What confirms each suspected cause:
- **#1 (underrun onset)**: in arm A, `underrunStarts=` above 0 in the windows where pops were heard, with
  `gapMax=` 25 ms or more, `paddingMin=0` and a low `queueMin=`, and `clicks=` rising with it; in arm B the
  pops are gone and `clicks=` nearly stops rising while `underrunStarts=` stays similar. Wired LAN (when the cable arrives): `underrunStarts=0`, `gapMax=` about
  10-15 ms, no pops.
- **#2 (resume without fade-in)**: arm B removes the remaining clicks while `capture-audio-underrun` lines
  show few `events=`; or `clockStalledGaps`/`audio-gap persistent empty endpoint` lines appear.
- **#3/#4 (resets)**: `hardResets=` above 0 and `capture-audio-reset` / `capture-audio-underrun reanchor`
  lines at the moments of skips; heard as short silences, not clicks.
- **#8 (loss)**: `concealed=` rising together with `packetWindowLost`; if pops come with `concealed=` growing
  but `underrunStarts=0`, the cause is loss, not arrival gaps.
- **None of these**: pops with `underrunStarts=0`, `hardResets=0`, `concealed=0` point outside this path:
  output device or driver, Bluetooth, the sound profile, or another program. Then try another output device.

## Round 4 (SmartLink phase 2, 20 November 2026): audio continuity

Owner's directive, section 5: handle audio interruptions caused by network jitter; adaptive buffering,
packet-loss concealment, clock synchronisation, recovery after stalls; latency must not accumulate. Code reading
of `exp/rc4` + the pinned chiaki-ng, and the Linux model. Nothing here was measured; unverified on Windows.

### Concealment of missing audio (code reading)

- **Lost frame:** concealed by the Opus decoder's own loss concealment. Chiaki's audio receiver hands a NULL
  frame to the decoder once 3 newer frames are buffered (`lib/src/audioreceiver.c:217-246`), and the decoder
  calls `opus_decode(NULL)` (`lib/src/opusdecoder.c:93-95`). It is already used; nothing to add.
- **Redundancy:** each audio packet carries N source frames plus M copies of earlier frames (the "FEC units",
  frame index `packet->frame_index - fec_units_count + i`, `audioreceiver.c:117-131`). They are used: a
  frame lost in one packet is normally taken from the next packet's copy before concealment is needed.
  This is packet-level redundancy. Opus in-band FEC is not decoded (`decode_fec=0`, `opusdecoder.c:95`).
  Whether the console encodes any cannot be settled from the client source: the encoder is on the console.
- **Frame size, settled:** the decoder calls the frame callback once per decoded packet (`opusdecoder.c:95-99`)
  and `ChiakiBackend.cpp` counts one `audioCallbacks` per call. The owner's session logged 13,559 callbacks in
  about 135 s, which is 100 per second, so 480 samples = 10 ms per frame at 48 kHz (code + one session's counter).
- **Late frame:** the receiver is driven by packets and has no timer, so a late frame is waited for, not
  concealed. The playout side then runs dry. Today that is silence with a click (round 3, #1); with
  `VEYRA_EXP_RP_AUDIO_SMOOTH` it is a 4 ms faded tail and a faded resume. A frame that arrives after it was
  concealed is dropped (`audioreceiver.c:180-181`).
- **Decision: no `VEYRA_EXP_RP_AUDIO_CONCEAL`.** Loss concealment is already used. Concealing *late* frames
  on time would need a timer inside the library's receiver; its hunk (stream "net" owns patch 0004) would be a
  tick that hands a NULL frame for `next_frame_index` once it is overdue by more than the playout reserve.
  That turns a late frame into a lost one plus concealment (the frame is dropped when it arrives), so it only
  helps when the reserve is already exhausted. The sink's reserve (ADAPTIVE) absorbs the same lateness without
  discarding real audio. Model view, in "gaps filled / not filled": with SMOOTH, 4 ms of each gap is filled by
  the tail and the rest is silent; with ADAPTIVE most gaps never reach the endpoint (round 3 table: underruns
  5-17x fewer).

### Clock synchronisation (code reading + model)

The default rule (auto sync) never drops or inserts samples. Every 250 ms it sets a libswresample
compensation of at most 5000 ppm (polyphase, no step; `CaptureAudioSession.cpp` 250 ms correction). So drift
does not click: in the model at plus or minus 100 ppm for 10 min it gives 0 resets, and 12 h at +100 ppm
gives 0.03 clicks/min, all from underruns. But it follows the video delay, not the arrival clock (finding 1),
so a faster console clock piles up delay. At +100 ppm, 12 h in the model, today's audio sits at the 1500 ms
bound (`audio_continuity_results.json`); review audio2 ran the same model shorter: the 1.5 s limit is reached
after about 4 hours at +100 ppm, and +50 ppm gives about 360 ms after 2 hours (model; the real clock
difference is unmeasured). `VEYRA_EXP_RP_AUDIO_FAST_RECOVER` already fixes this:
`AudioArrivalDrift` follows the arrival clock and trims the ratio within 5000 ppm. Model, 12 h at +100 ppm:
latency 16.6-16.9 ms median, end-of-cycle offset within 0.5 ms of the floor in the first and the last cycle.
**No new `VEYRA_EXP_RP_AUDIO_DRIFT` switch:** the trim exists and is inaudible by construction; the kit
should set FAST_RECOVER, but only together with SMOOTH and ADAPTIVE (alone on Wi-Fi bursts the model shows
2.6x the clicks: 9.0 against 3.5 per minute). The ratio in force is logged as `trimPpm=` (and `ratioPpm=`).

### Recovery after a stall

Today, after a loss or held stall of 100 ms or more, audio stays about 90-98 ms late until the next
discontinuity (finding 11). In the model, 200 ms loss stalls every 30 s give an |A/V| median of 91 ms today.
`VEYRA_EXP_RP_AUDIO_FAST_RECOVER` removes it: 1.0 ms with fast alone, 2.6 ms with fast + SMOOTH + ADAPTIVE
(adaptive counts each stall's underrun as a disturbance and holds up to 30 ms for a while). Test:
`audio_pop_model_continuity_arm_recovers_from_stalls_and_drift`.

### Adaptive reserve: own statistics and SmartLink

`AudioAdaptivePlayout` (`include/veyra/sink/AudioPopGuard.h`) now has three inputs, and the reserve is the
largest of them, between today's value and 80 ms:

1. **Underruns** (round 3): at least +10 ms, and at least the largest recent arrival gap + 5 ms.
2. **Forgetting quantile of relative arrival delay** (new; the shape of WebRTC NetEQ's delay manager,
   re-implemented from its public description, no code copied). Each pushed block's arrival minus PTS is
   measured above its 2 s lower envelope (`ArrivalClockMapping`). The 0.995 quantile with a forgetting factor
   of 0.9995 per block (about 20 s) + 5 ms raises the reserve before any underrun. Delays up to 15 ms keep
   today's 20 ms. In the model this changes little: underrun growth already reacts within the first bursts.
   It matters in the first seconds and on links whose lateness grows without underruns.
3. **SmartLink** (`VEYRA_EXP_RP_SMARTLINK`): only while `latestShared()` is fresh (`isFresh`, 2 s) and in apply
   mode, `decision.audioTargetMs > 0` raises the reserve's target at once (bounded by 80 ms) and releases it at
   2 ms/s. What physically happens is slower: the schedule moves at once, but the queue grows only through
   the rate trim, by at most about 5 ms per second (5000 ppm), or through a reset. The switch is read as
   SmartLink reads it (`parseMode(...) != Off`: `=off` is off for audio too).
   It never lowers the reserve this class keeps itself. Stale, observe mode or 0: exactly the round-3
   behaviour (test `audio_pop_model_smartlink_unset_stale_or_zero_is_exactly_phase_one`). With the switch set
   in any mode, the sink reports `publishAudioFeed(queued + endpoint ms, underruns)` every 250 ms.

**A raise is not trouble (review audio2 item 1, fixed).** Because the schedule moves at once, a raise of X ms
makes the measured error jump by X. Before the fix, a raise of about 55 ms or more (an ask of 80 over the
default 20) tripped the default rule's "error above 60 ms" fade-and-restart (and the controller's early-reset
limit with FAST_RECOVER) by itself. `ReserveChangeAllowance` (`AudioLatencyController.h`) now leaves the
change since the last anchor out of those reset tests only; it shrinks with the error as the trim works the
raise off and never grows with it, so real trouble on top still resets. With the reserve never changed (every
switch unset) it is always 0 and the test is the original one. Model (tests
`audio_pop_model_reserve_raise_never_forces_a_reset`, `audio_pop_reserve_change_allowance_keeps_real_trouble`):
asks of 40/60/70/80 ms with no audio trouble for 1 simulated hour give 0 resets on the default rule and with
FAST_RECOVER (before: 4 and 1 at 80 ms); 12 h of Wi-Fi bursts (20-60 ms, 2 min of every 10) on the default
rule: 11 resets without SmartLink, 2 with an ask of 80 (before the fix: 16). Model results, not measurements.

Returning to the floor uses only what existed: after 10 s without an underrun the reserve falls at 2 ms/s,
and the extra delay is removed by the rate trim (5000 ppm at most) or, with FAST_RECOVER, by cuts in quiet
sound. No new time-stretch was added: a pitch-preserving stretch is new signal processing, and its audibility
cannot be checked here.

**Latency does not accumulate (model, `audio_sim --continuity`, `audio_continuity_results.json`):** 12 simulated
hours, with 2 min of Wi-Fi-like bursts (20-60 ms, every 1 s) at the start of every 10 min:

| Rule | console | return to floor after bursts end (worst of 72) | end-of-cycle offset vs floor, first / last / max (ms) | clicks/min | underruns/min | latency P50 / P95 (ms) |
|---|---|---|---|---|---|---|
| today | 0 ppm | 15 s, 44 cycles never within 1.5 ms | -0.19 / 2.10 / 4.32 | 3.53 | 4.22 | 18.1 / 55.1 |
| fast | 0 ppm | 71 cycles never within 1.5 ms (about 2 ms above) | 0.07 / 2.07 / 2.07 | 9.02 | 10.72 | 18.0 / 20.5 |
| fast + smooth + adaptive | 0 ppm | **42 s**, every cycle | -0.00 / -0.00 / 0.00 | 0 | 0.36 | 16.0 / 65.0 |
| + SmartLink asking 60 ms during bursts | 0 ppm | 42 s, every cycle | -0.00 / -0.00 / 0.00 | 0 | 0.32 | 16.0 / 65.1 |
| today | +100 ppm | (its floor is itself 1500 ms: drift) | - | 0.03 | 0.04 | 1499.9 / 1501.0 |
| fast | +100 ppm | 30 s | -0.41 / -0.43 / -0.32 | 9.38 | 11.21 | 16.6 / 19.3 |
| fast + smooth + adaptive | +100 ppm | 49 s, every cycle | 0.00 / -0.00 / 0.05 | 0 | 0.33 | 17.0 / 64.9 |
| + SmartLink | +100 ppm | 49 s, every cycle | 0.00 / -0.00 / 0.05 | 0 | 0.29 | 17.0 / 65.0 |

So, in the model (after the review fixes), the continuity arm returns to the clean-link floor within 42-49 s
after every disturbance. The first and the last of 72 cycles end within 0.05 ms of each other: no ratchet over
12 h. The price is the P95
latency during disturbances (reserve up to 80 ms). The SmartLink ask here is a model of the decision's timing
(asks 60 ms from 1 s after bursts start until 3 s after they end), not SmartLink itself. It adds little on top
of the arm's own statistics. Its value is acting earlier when the network sees trouble that audio has not seen
yet, which this model cannot show.

### Counters (with `VEYRA_EXP_RP_STATS=1`, in `[remoteplay-audio]`)

Round 3's keys plus `clicks=` (endpoint gaps that began on a real sample with no fade, since the stream
started; with SMOOTH only late wakes remain), `playoutTargetMs=` (reserve in force), `playoutSource=base|own|smartlink`,
`smartlinkAskMs=` (0 = none / stale / observe), `aboveTargetPct=` (share of the 10 s window's queue samples
more than 5 ms above the larger of the schedule and the reserve: piled-up delay), `trimPpm=` (rate trim in
force), `concealed=` (Opus frames Chiaki concealed since the stream started).

### Kit: the "audio continuity" arm

`VEYRA_EXP_RP_STATS=1 VEYRA_EXP_RP_AUDIO_FAST_RECOVER=1 VEYRA_EXP_RP_AUDIO_SMOOTH=1 VEYRA_EXP_RP_AUDIO_ADAPTIVE=1`,
plus `VEYRA_EXP_RP_SMARTLINK=apply` in the SmartLink arm. Not `VEYRA_EXP_RP_AUDIO_LOW_BUFFER` (round 2: riskier,
gains not shown). The run must show, against the STATS-only arm on the same link: `pops=` growing much
slower, `underrunStarts=` per window down, `aboveTargetPct=` near 0 in quiet windows, and `playoutTargetMs=`
back at its starting value within a minute after trouble ends. Over 30 min, the `delayMs=` of quiet windows
must not grow.

## On real hardware (when the owner is back)

(Round 3: on Wi-Fi, or to find pops, use the checklist above; `VEYRA_EXP_RP_STATS=1` adds the counters to every
arm below without changing behaviour.) Same game scene each time, 2 minutes per arm, wired LAN. Run once with nothing set (the stable
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
   lets console-clock drift build up unseen over a long session (model: 1.5 s after about 4 h at +100 ppm,
   about 360 ms after 2 h at +50 ppm). This is the shipping behaviour, upstream too; repeat with fast
   recovery (with SMOOTH and ADAPTIVE) to compare.
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
