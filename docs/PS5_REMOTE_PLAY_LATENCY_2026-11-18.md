# PS5 Remote Play: button-to-screen latency (2026-11-18)

Branch `perf/ps5-low-latency`, based on `f306f570`. Target machine: RTX 5070, 180 Hz OLED.

**Verification status: unverified on Windows.** Nothing here has run against a PS5, on an NVIDIA
GPU or under MSVC. What was built, run and measured, and where, is listed under "What was verified".

## What sits between a button press and the picture

| Stage | Original | Now |
|---|---|---|
| Pad state read by the UI thread | every 8 ms | every 4 ms |
| UI thread to session owner | FIFO, one state per 4 ms owner tick | owner woken, sends everything pending |
| Owner to console | Chiaki sends at once | unchanged |
| Console: game, encode, network | not ours | not ours |
| Frame complete (Chiaki) to decoder owner | owner sleeps 1 ms between polls | owner woken by the video callback |
| Decode | D3D12VA on the presentation device, async | unchanged |
| Decoded frame to engine loop | engine polls in timer slices of up to 1 ms | engine woken by an event |
| Colour conversion and effects | decoder texture sampled in place, GPU-side fence wait | unchanged |
| GPU finished to Present | 0.2 ms poll | engine woken by the GPU fence |
| Present to light | display sync setting | unchanged, see "Settings" |

Already as it should be, checked and left alone:

- **Zero-copy.** `EnhanceGraph` creates luma and chroma views on the decoder's own NV12/P010 texture and
  waits for the decode fence on the GPU queue. No CPU read-back and no texture copy.
- **NVIDIA hardware decode.** D3D12VA is the NVDEC path on an RTX card and the only one that shares the
  D3D12 device with the presenter. CUVID would need a CUDA copy to reach the same pipeline.
- **Decoder buffering.** The headers a console sends (from chiaki-ng `test/bitstream.c`) carry
  `bitstream_restriction_flag=1`, `max_num_reorder_frames=0` (H.264) and `sps_max_num_reorder_pics=0`,
  `sps_max_latency_increase_plus1=0` (H.265). libavcodec holds no frame with such streams, with or
  without `AV_CODEC_FLAG_LOW_DELAY`, also after a lost frame (`decoder_delay.py`). No flag was added.
- **Queues.** Compressed frames: FIFO of at most 4, emptied by the owner as fast as it decodes.
  Decoded frames: one slot, the newest wins. Neither holds a frame for pacing.
- **Chiaki networking.** A frame is flushed to us when its last unit arrives or FEC can complete it,
  and controller changes wake the feedback sender at once. Nothing to gain without changing the pinned library.

## chiaki-ng issue #807

The reported 50 ms regression is in chiaki-ng's own window, `gui/src/qmlmainwindow.cpp` (the frame pacing
rework from `45237ac` on). PushFPS pins chiaki-ng at `0e16950`, which contains all of those commits, but
builds only `lib/` (`CHIAKI_ENABLE_GUI OFF`) and presents through its own engine. Between v1.10.0 and the
pin, `lib/` changed in `feedbacksender.c` (order of touch events) and `holepunch.c` only. The regression
does not exist in PushFPS. PushFPS already does what the issue asks for: without frame generation a
stream frame is presented when it is ready, not on a schedule.

## Changes

1. **Session owner wakes on events** (`WakeSignal`, `SessionInbox::wakeOn`, `RemotePlaySessionSource::run`).
   A video sample, a pad state, a login PIN or stop ends the wait. Before, the owner slept 1 ms between
   polls: 1-2 ms per sleep on Windows with a raised timer resolution, up to 15.6 ms without one.
2. **Pad states go out at once** (`ControllerQueue`). Button, trigger-edge and touch-contact changes keep
   their own entry and order. A state that differs only in sticks, trigger depth, touch position or
   motion replaces the newest pending one. Side effect: an owner stall no longer overflows the queue
   into a forced release (the original overflowed after 16 polls, 128 ms).
3. **Pad polled at 250 Hz** (`bridge/RemotePlay.cpp`), as the PC and Xbox streaming paths already do. The
   session check and rumble / trigger / haptics feedback keep their 8 ms cadence.
4. **Engine loop wakes on the decoded frame and on the GPU fence** (`EngineController.cpp`), as it
   already does for a capture card. A frame that waits for its presentation time under frame generation
   is not interrupted by these events: that pacing is unchanged.

Not changed: the pinned Chiaki library, the decoder, the effect graph, presentation scheduling under
frame generation, the controller-only mode beside a capture card (it shares the queue, still on its tick).

### A/B switches

Any value restores the original timing of that part, on the same build:

| Variable | Restores |
|---|---|
| `VEYRA_TEST_RP_LEGACY_TIMING` | all of the below |
| `VEYRA_TEST_RP_LEGACY_INPUT` | 8 ms pad poll and one state per 4 ms tick |
| `VEYRA_TEST_RP_LEGACY_OWNER_WAIT` | the owner's 1 ms sleep poll |
| `VEYRA_TEST_RP_LEGACY_ENGINE_WAKE` | the engine's timer-slice and 0.2 ms polls |

The log line `[remoteplay-timing]` at session start states which timing is active.
`scripts/remoteplay/latency-ab.ps1` starts PushFPS in either arm and saves the result of the built-in
latency test afterwards. Experiments added later are listed in `PS5_REMOTE_PLAY_EXPERIMENTS.md`.

## Measurements

Run on a 2-vCPU Linux VM (`tools/remoteplay_latency_bench/run.sh`). They cover the parts that are
portable C++. They are not Windows numbers and not button-to-screen numbers.

**Frame hand-off, inbox to decoder owner** (real `SessionInbox`, 59.94 fps producer, 1500 frames, three runs;
the baseline is built from the original sources):

| | mean | p50 | p95 | p99 |
|---|---:|---:|---:|---:|
| Original, 1 ms sleep poll | 0.55-0.57 ms | 0.54-0.57 | 1.05-1.07 | 1.11-1.14 |
| Event wake | 0.11-0.14 ms | 0.10-0.11 | 0.17-0.19 | 0.23-0.43 |

Linux sleeps 1 ms as about 1.06 ms. With the sleep set to what Windows does (a parameter, not a Windows
measurement): 1.6 ms gives mean 0.90 / p95 1.66 ms; 15.6 ms (the default when nothing in the
process has raised the timer resolution) gives mean 8.0 / p95 15.3 ms. Whether that case occurs in
practice was not established.

**Input dispatch, pad state readable to Chiaki call** (model in virtual time around the real
`ControllerQueue`; 20 000 button edges, moving stick, 59.94 fps decode load of 1 ms per frame, owner
sleep 1-2 ms):

| | mean | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|---:|
| Original | 6.33 ms | 6.29 | 10.82 | 11.98 | 13.5 |
| Now | 2.09 ms | 2.10 | 3.91 | 4.16 | 5.0 |

Of the 4.2 ms mean difference, 2.0 ms is the faster poll and 2.3 ms the immediate dispatch. Every edge
was delivered exactly once and in order in both.

**Engine wake**: not measurable off Windows. By construction the original waits for the rest of a
timer slice of up to 1 ms for the decoded frame and for a 0.2 ms poll (about 0.5 ms with Windows' timer
granularity) for the GPU. The application already records the first as the `DecodedQueue` CPU stage,
so the difference will show in its own telemetry.

Expected in total, frame generation off: about 4 ms less on the input side and 1-2 ms less on the
picture side on average, more in the tail. The rest of a typical 60-120 ms button-to-screen time is the
game, the console's encoder and the network, which this application does not control.

## Settings for an RTX 5070 and a 180 Hz OLED

- **Frame generation off** for the lowest latency. Interpolation shows a real frame one source interval
  (16.7 ms at 60 fps) plus its processing allowance after it was decoded; no setting removes that.
- **Variable refresh on** (G-SYNC enabled for windowed and fullscreen in the NVIDIA control panel). A
  60 fps stream is inside the panel's range, so each frame starts a refresh when it arrives. Without it
  the default (`Automatic`, `Present(0,0)`) shows the frame at the next 180 Hz refresh: up to 5.6 ms,
  2.8 ms on average. `Tearing` also removes the wait, with visible tear lines.
- **NVIDIA low latency on** (DXGI maximum frame latency 1).
- **Hardware decode** (Automatic), H.265, wired network on both ends.
- If smoothness matters more than latency: 3X (60 to 180) fills the panel's refresh rate at the cost above.

## Not done

- **Predictive (extrapolating) frame generation.** Every backend here interpolates between two decoded
  frames. Showing the real frame at once and synthesising the following ones from past motion needs a
  new warp pass, a disocclusion fill and its own schedule in the engine. It cannot be written responsibly
  without a GPU to look at the result, so it was not attempted.
- **Chiaki library changes** (socket buffer size, thread priorities): no evidence that they cost latency
  on a wired LAN, and the library is pinned and provenance-checked.
- **A dedicated input thread.** The owner still decodes on the thread that sends the pad; a press that
  lands inside a decode submit waits for it (about 1 ms, 6 % of presses in the model).
  *Correction, later the same day:* an independent review of the library found one timed wait this
  note missed. Video packets pass a reorder queue, and a missing packet holds later ones for up to
  16 ms (`TAKION_AV_REORDER_TIMEOUT_US`). It costs nothing on a clean link and up to one frame per
  loss event. See `PS5_REMOTE_PLAY_NETWORK_REVIEW_2026-11-18.md`.

## What was verified

- Linux, GCC 13: `tests/remoteplay/CoreTests.cpp` 88/88 (77 existing, 11 new for `WakeSignal`,
  the inbox wake and both `ControllerQueue` policies). The benchmarks above.
- mingw-w64 GCC 13 `-fsyntax-only` against FFmpeg 9.0.1 headers: `RemotePlaySessionSource.cpp`,
  `RemotePlaySource.cpp`, `SessionInbox.cpp` clean. `EngineController.cpp` (with the public NVIDIA / AMD
  headers) reports the same 7 mingw-versus-MSVC diagnostics as the original and none new.
- `src/ui/bridge/RemotePlay.cpp` was syntax-checked later with Qt 6.4 headers (see
  `PS5_REMOTE_PLAY_EXPERIMENTS.md`). Not compiled at all: `tests/remoteplay/ProductBoundaryTests.cpp`.
  `scripts/remoteplay/latency-ab.ps1` was not parsed by PowerShell.
- Not run: anything on Windows, on a GPU, or against a console.

## Needs a PS5

1. The stream connects, plays and reconnects with the new owner loop; login PIN entry still works.
2. Pad: taps and quick double taps register, held buttons stay held, sticks and gyro are smooth, touchpad
   swipes work, rumble / adaptive triggers / haptics unchanged. Alt-tab away releases everything.
3. Log: no `Controller queue overflow`, `[remoteplay-timing]` shows `event / immediate / event`.
4. Telemetry with and without `VEYRA_TEST_RP_LEGACY_TIMING`: inbox wait (`ingressWaitMeanMs`) and the
   `DecodedQueue` stage should fall; frame pacing and 1 % low with frame generation on should not change.
5. Built-in latency test, alternating arms with `latency-ab.ps1`, at least 120 presses per arm, then
   `tools/remoteplay_latency_bench/analyze.py`. It starts its clock when the press is handed to the
   session, so it sees the dispatch and picture-side changes but not the faster pad poll.
6. True button-to-screen: a 240 fps phone camera on pad and screen, or an LDAT-style sensor, both arms.
