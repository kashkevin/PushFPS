# PS5 Remote Play audio path (Slipstream)

Code reading of `exp/dev` @ `13cf3b1f` and the pinned chiaki-ng `lib/` (`/home/claude/vendor/chiaki-ng`).
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

Wiring (`CaptureAudioSession.cpp`, PS5 Remote Play only, switches in `docs/PS5_REMOTE_PLAY_EXPERIMENTS.md`):
the controller replaces the 250 ms correction in auto mode; drops go through `dropHeadLocked` (quiet
check on the removed window plus the crossfade). Device changes: the existing re-open paths (default-device
notification, chosen device leaving or returning, any write error) were checked and kept; only the first
reopen delay changes. Cuts are clamped to the PCM actually queued beyond the fade; the quiet scan covers
only the cut and its fade (at most about 600 frames, under the session mutex). Low buffer:
`AudioRenderer::requestLowLatencyPeriod`; abandoned for the rest of the stream (one log line) when a step
after creating the low-latency stream fails, or after 8 endpoint underruns while PCM was arriving.

## Linux model (not a measurement)

`audio_sync_linux_model_new_rule_is_never_worse_and_bounds_drift` in `tests/remoteplay/AudioSyncTests.inc`
models the console clock (10 ms Opus frames, sound with quiet gaps: loud runs averaging 400 ms, quiet 80
ms), 2-3.5 ms network delay, Chiaki concealment bursts, `AudioIngress`, the feeder's PTS anchoring and
2 ms polling, the playout queue, and a 20 ms / 10 ms-period endpoint; video delay 35 ms unless stated.
A swresample compensation ends 1 s after it was last set, as in the library. It
does not model Windows engine or device latency, swresample's filter delay or what anything sounds like.
Latency = play time minus the frame's lower-envelope arrival; |A/V| = distance from arrival + video delay.
ms, median / P95 / max after the first 3 s; "silent" = ms with nothing playing.

| Scenario | Today latency | Today \|A/V\| | Today silent, resets | New latency | New \|A/V\| | New silent, resets, dropped |
|---|---|---|---|---|---|---|
| steady | 37.9 / 38.0 / 38.5 | 2.9 / 3.0 / 3.5 | 0, 1 | 38.0 / 38.0 / 38.0 | 3.0 / 3.0 / 3.0 | 0, 1, 0 |
| steady, video 15 ms | 19.0 / 19.0 / 19.0 | 4.0 / 4.0 / 4.0 | 0, 1 | 18.0 / 18.0 / 18.0 | 3.0 / 3.0 / 3.0 | 0, 1, 0 |
| 25 ms hold x6 | 37.4 / 38.0 / 58.0 | 2.4 / 3.0 / 23.0 | 0, 1 | 38.0 / 38.0 / 58.0 | 3.0 / 3.0 / 23.0 | 0, 1, 0 |
| 200 ms stall (loss) | 126.0 / 126.8 / 127.0 | 91.0 / 91.8 / 92.0 | 309, 3 | 37.2 / 38.0 / 103.0 | 3.0 / 8.5 / 68.0 | 176, 3, 0 |
| 200 ms stall (held) | 37.8 / 40.5 / 318.0 | 2.8 / 5.5 / 283.0 | 182, 3 | 38.0 / 38.0 / 245.0 | 3.0 / 3.0 / 210.0 | 181, 2, 173 |
| 2 s stall (loss) | 126.0 / 126.9 / 127.4 | 91.0 / 91.9 / 92.4 | 2110, 4 | 37.0 / 38.0 / 102.0 | 2.3 / 10.0 / 67.0 | 1975, 3, 0 |
| 60 ms bursts every 5 s | 56.7 / 68.0 / 118.8 | 21.7 / 33.0 / 83.8 | 328, 1 | 37.0 / 38.0 / 119.0 | 2.0 / 3.0 / 84.0 | 472, 1, 475 |
| +200 ppm drift, 300 s | 67.8 / 94.6 / 97.9 | 32.8 / 59.6 / 62.9 | 0, 1 | 38.0 / 38.8 / 39.0 | 3.0 / 3.8 / 4.0 | 0, 1, 0 |
| -200 ppm drift, 300 s | 20.3 / 33.6 / 37.2 | 14.7 / 20.8 / 21.6 | 0, 1 | 36.8 / 37.0 / 37.3 | 1.8 / 2.0 / 2.3 | 0, 1, 0 |
| device switch | 37.5 / 38.0 / 38.5 | 2.5 / 3.0 / 3.5 | 540, 2 | 38.0 / 38.0 / 38.0 | 3.0 / 3.0 / 3.0 | 90, 2, 0 |

Reading it: the new rule matters after loss stalls (finding 11), with clock drift (finding 1) and with
repeated late bursts; in steady state both are the same. The price shows in the burst row: removing the
delay a burst adds means the next burst finds less audio queued, so more silence (472 vs 328 ms).
With the ratio set only when it changed (the reviewed defect), the +200 ppm row fails the model's bound:
the test catches it. With the previous controller (before the pending-cut and 5 ms loud-cut fixes) the
bursts row removed 657 ms (now 475) and P95 |A/V| was
12 ms (now 3). The
device row uses the shorter first reopen (50 ms instead of 500 ms). The low-buffer arm
(`VEYRA_EXP_RP_AUDIO_LOW_BUFFER`) changes nothing in this model, because its gain is Windows engine and
device latency, which the model does not contain; the test prints it as a third column. The constant 2-4 ms
|A/V| is the first block's arrival jitter in the PTS anchor, the same for both.
