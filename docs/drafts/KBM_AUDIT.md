# Keyboard and mouse audit (stream "kbm", owner's sixteenth message)

Base: `exp/rel-9` (Package 9, 71e49426). Method: code reading of every file on the input path, the pinned chiaki-ng
source (`/home/claude/vendor/chiaki-ng`, 0e169501, plus our patch 0003), and two Linux models written for this audit
(scratch, not shipped; their numbers are a model, not a measurement). Nothing here ran on Windows, a PS5 or the
owner's PC. **CONFIRMED** = read in the code (or the model reproduces it from the code); **HYPOTHESIS** = needs a
measurement on his PC or the console.

## 1. The path a mouse movement takes, and its cadence

| Step | Where | Thread | When |
|---|---|---|---|
| Raw Input report | `RemotePlayKeyboardMouse::nativeEventFilter` WM_INPUT (`include/veyra/ui/RemotePlayKeyboardMouse.h:259`): `GetRawInputData` for the message, then `GetRawInputBuffer` drains every queued report (`drainRawInput`) | interface (Qt GUI) | as the message loop dispatches it |
| Counts added | `KeyboardMousePad::mouseMove` -> `KbmImmediateAim::add` (int64 sums) and `MouseAim::add` | interface | same call |
| Pad poll | `QTimer` `Qt::PreciseTimer`, `ps5PadPollMs()` = 4 ms (1..8 with `VEYRA_EXP_RP_PAD_POLL_MS`) (`src/ui/bridge/RemotePlay.cpp:21,689`) | interface | every 4 ms, independent of the display rate |
| Translation | `RemotePlayKeyboardMouse::apply` -> `KeyboardMousePad::apply` -> `KbmImmediateAim::step` (profiles) or `MouseAim::step` (legacy) | interface | in the poll |
| Hand-off | `EngineController::remotePlayController` (engine `mutex_`, short) -> `RemotePlaySessionSource::controller` (session `mutex_`, `ControllerQueue::push`, `wake_->notify`) (`src/engine/EngineController.cpp:226`, `src/source/RemotePlaySessionSource.cpp:611`) | interface | in the poll |
| Submit | session owner loop sends everything pending at the top of each iteration (`RemotePlaySessionSource.cpp:364-381`), then `source.read` (video decode), then waits on `wake_` at most to the next 4 ms tick (`:459-467`) | session owner | at once when the owner waits; after the current decode when it is decoding |
| UDP | `chiaki_feedback_sender_set_controller_state` signals the feedback thread, which sends a feedback-state packet (16-bit sticks, `lib/src/feedback.c:73-76`) on every stick change; the `FEEDBACK_STATE_TIMEOUT_MIN_MS 8` constant is NOT enforced (`lib/src/feedbacksender.c` "TODO: FEEDBACK_STATE_TIMEOUT_MIN_MS"); buttons go as history packets | chiaki feedback | at once (CONFIRMED in source) |

Input send cadence: one pad state per 4 ms poll, changed states only, independent of display FPS (CONFIRMED). No
queue holds stick values: `ControllerQueue` (Immediate policy) replaces a pending analog state with the newest and
keeps only button edges (CONFIRMED, `include/veyra/remoteplay/ControllerQueue.h`). The poll and the send never wait
for the render or frame-generation thread: the engine `mutex_` is held only for snapshot assignments
(`EngineController.cpp` 440, 742, 980, 1543, 1606: no GPU wait under it), and the owner publishes decoded frames into a
one-slot mailbox that never blocks (`publishDecoded`, `RemotePlaySessionSource.cpp:530`) (CONFIRMED).

Indirect couplings (HYPOTHESIS until his counters show them):
- **The poll runs on the interface thread.** A busy QML frame, a layout or a long JavaScript call delays the poll.
  The `[slipstream-input-poll]` line (`pollGapMsP95/P99/Max`, `latePolls`) measures exactly this; Package 9's evidence
  had no keyboard-and-mouse run with it.
- **The owner decodes video between sends.** A pad state that arrives while `source.read` decodes waits for that
  decode (typ. 1-4 ms with hardware decoding; longer under GPU contention from frame generation, which shares the GPU
  but not a lock). `stateToSendMs*` (with `VEYRA_EXP_RP_STATS=1`) measures it.
- **Capture-card mode** (section 5) sends on a fixed 4 ms tick and never wakes for a new state.

## 2. Section 2 of the message: "make mouse camera movement substantially smoother"

| Item | What the code does now | Defects (location) | Status |
|---|---|---|---|
| Raw mouse input | Raw Input, relative only, absolute devices ignored, buffered drain, no `RIDEV_NOLEGACY` (Qt needs the legacy messages); WM_MOUSEMOVE is swallowed so nothing double-counts (`moonlight/InputRouter.h:47`) | none found | CONFIRMED |
| Accurate relative movement | 64-bit sums per batch, clamped only at INT_MAX per batch (`KbmCapture.h:121-139`) | none: no delta is lost or rescaled between WM_INPUT and the transform | CONFIRMED |
| High-frequency sampling | every report is read; the transform sees the SUM per 4 ms poll and divides by the poll interval (`KbmInputProfile.h:353`) | **D1 per-poll speed noise from report quantisation**: a poll holds 4R/1000 +-1 reports, so the stick value jitters even for a perfectly steady hand. Model (poll jitter +-0.7 ms, steady 6000 counts/s): 250 Hz 27 % (and 4 % of polls exactly zero), 500 Hz 19 %, 1000 Hz 11 %, 4000 Hz 3 %, 8000 Hz 2 % coefficient of variation of the per-poll speed. A console game reads the stick once per game frame (33 ms at 30 FPS), so every frame turns by whatever one 4 ms value said: this is camera judder that depends on the mouse's polling rate | CONFIRMED (code + model) |
| Low-latency mouse-to-stick | immediate transform: no window, output = this poll's counts (`KbmImmediateAim::step`) for the three new profiles; **legacy (the default for everyone who does not choose) keeps an 8 ms box window** (`KbmProfile.h:47`, `KeyboardMousePad.h:146-159`): 2 ms mean, 4 ms last part at a 4 ms poll; with a 125 Hz mouse the window floor is 16 ms (`kbmAccumulationFor`) | legacy delay as stated; immediate adds none | CONFIRMED |
| X/Y sensitivity | `sensX`/`sensY` per context (walk, aim, sprint, drive) plus the calibration's vertical ratio | none | CONFIRMED |
| Hip / ADS sensitivity | Aim context has its own `sensX/sensY` and `adsFactor` (0.6 in the presets); the aim context follows the aim button (L2) held on keys or pad | the factor applies at the poll, so counts gathered under the hip context in the poll where the aim button went down are scaled as ADS (at most one poll) | CONFIRMED, negligible |
| Dead-zone compensation | `tilt = deadzone + (1-deadzone)*f(u)`, radial, from the calibration when `game` is on | **D2 8-bit truncation**: the immediate path sends exact 16-bit values (`stick()`, `:430`); the console's pad data is 8-bit per axis (ScePadData sticks are bytes in the public homebrew SDK headers; Remote Play carries 16 bits, `feedback.c:73`). A tilt just above a dead zone that is not a whole 8-bit step (0.12 = 15.24 steps) is cut to 15 steps = 0.118, inside the dead zone: the smallest motions are lost. The legacy path avoids this (`stickBits=8`, whole multiples of 256 with the remainder carried); the immediate path does not | CONFIRMED in our code; the console's truncation is HYPOTHESIS |
| Controller acceleration compensation | `accel` (legacy, off) is OUR acceleration, not the game's; the calibration measures dead zone, full-turn time and the response at 25/50/75 % (static), not the game's time ramp | the game's turn ramp (speed rising while the stick is held) is neither measured nor compensated | CONFIRMED (missing) |
| Nonlinear response curve | linear / power / precision-speed per context, applied before the game's inverse response | none | CONFIRMED |
| Small-movement precision | below one count per poll: a poll with a count turns, the next none | D1 and D2 above; with a 125 Hz mouse the hold covers one report interval | CONFIRMED |
| Fast turning without overshoot | Drop (presets) clips a flick at the game's fastest turn: no motion after the hand stops, but a flick faster than full stick turns less than the hand. Carry (Custom, and **legacy: `carryMs=40` built in**, `KeyboardMousePad.h:62`, `:137-145`) keeps the surplus and turns on after the hand stops | **D3 legacy overshoot**: after a flick beyond full stick the camera keeps turning at full speed for up to 40 ms plus the 8 ms window after the mouse stopped (the "accumulation after stop" the message forbids). The presets are clean | CONFIRMED |
| Smooth diagonals | radial: the length decides the tilt, the direction is kept; a diagonal at full tilt is 0.707 per axis | none in our maths. A game with an axial (per-axis) dead zone would lose the small axis of a shallow diagonal: we cannot know which the game uses | CONFIRMED / game side HYPOTHESIS |
| Stable motion across polling rates | mean speed is right at every rate (model: within 0.2 % from 250 to 8000 Hz); the per-poll noise is not (D1); 125 Hz and 250 Hz: the hold `kbmHoldUsFor` covers only a mouse SLOWER than the poll, so at 250 Hz with a 4 ms poll a jittered poll finds no report and sends zero (4 % of polls in the model) | **D4 250 Hz zero polls** (`KbmInputProfile.h:446`: `interval > poll`) | CONFIRMED (model) |
| Cursor capture / release | `ClipCursor` to the client rect, `SetCursor(nullptr)` on WM_SETCURSOR, raw input keeps reporting at the clip edge; released on focus loss, foreground change, Ctrl+Alt+Shift+Z, device removal, session end, with every key and stick neutral in the next state (`KbmCaptureState`, tested) | the clip is re-applied only on WM_SIZE/WM_MOVE and at capture: if Windows drops it (another process calling `ClipCursor`, a secure desktop) the hidden cursor can leave the window on a second monitor and a click there takes the focus (which then releases cleanly) | CONFIRMED code; the trigger is HYPOTHESIS |
| No drift when stationary | immediate: the output is zero at the first poll without counts (after the hold for slow mice); legacy: zero once the window empties, the 8-bit remainder is kept, not sent. A physical controller connected alongside passes its own right stick through once the mouse has been still for 250 ms (`kMouseHoldUs`): its drift is the pad's, and the game's dead zone applies as with the pad alone | none found | CONFIRMED |

## 3. Section 3: advanced mouse-to-stick translation

| Item | Now | Defect / gap | Status |
|---|---|---|---|
| Lost or inconsistently scaled deltas | none lost on the way in; the transform's speed is counts / poll interval | D1 (noise), D2 (8-bit truncation of the smallest motion), Drop by design beyond full stick; a poll gap over 50 ms is divided by 50 ms (`:353`), which saturates and drops (only after a stall) | CONFIRMED |
| Accumulation after the mouse stops | immediate: none (counts cleared each poll; Carry is opt-in, Custom only); legacy: D3 (40 ms carry + 8 ms window) | D3 is in the default (legacy) profile | CONFIRMED |
| Small / medium / fast movements | precision-speed curve in Adaptive Precision (aim), straight elsewhere; flicks clip at the game's fastest turn | whether the curve helps has not been shown by any test on the console; keep it optional | CONFIRMED |
| Aiming vs free camera | Aim context with its own tuning, chosen by the aim button | none | CONFIRMED |
| Game dead zone and acceleration | dead zone and static response from the calibration (Ctrl+Alt+Shift+K or the page wizard) | time ramp not measured (see section 2) | CONFIRMED (gap) |
| Game-enforced maximum rotation speed | `fullSpeedCounts` = counts per second at full stick, from the calibration's full-turn time and the player's counts per 360 | none | CONFIRMED |
| Low-latency mode | Ultra Low Latency: immediate, Drop, no smoothing, proven schedule | is NOT the default: an untouched install uses legacy (8 ms window + 40 ms carry) | CONFIRMED |

## 4. Section 4: frame generation and input

- Sampling and transmission are independent of displayed FPS (section 1). Generated frames cannot raise the game's own
  input rate: GTA VI samples the stick once per game frame (30 FPS), and an interpolated frame shows motion between two
  real frames that already exist, so it carries no newer input. Interpolation needs the NEXT real frame: it adds at
  least one source frame interval (33.3 ms of game content at 30 FPS, 16.7 ms per stream frame at 59.94) plus
  processing before the first generated frame can be shown (CONFIRMED by design; the size on his PC needs a camera).
- What can be measured inside the PC (software clock, `VEYRA_EXP_RP_STATS=1`): `rawToTranslateUs` (first input event
  after a poll to that poll), `translateUs` (transform cost), `stateToSendMs` (poll hand-off to the owner's submit),
  `eventToSendMs` (first input event to the submit), `sendGapMs`, `sendHz`, `pollGapMs` (interface-thread health).
  Not measured: chiaki's feedback thread to the network card, the network, the console, the game's frame, the encode,
  decode, frame generation and display. Only a camera (or a light sensor) on the screen measures mouse-to-photon.
- Missing: the capture-card mode's sends have no owner tap (no `stateToSend` there); there is no FG on/off camera
  procedure for mouse input.

## 5. Input with a capture card

A capture card carries no input (CONFIRMED: video and audio only). What exists: **PS5 手柄控制** ("PS5 controller
control", Veyra 2.0.8, `QmlPlayerBridge::startPs5Control`, `RemotePlay.cpp:891-948`): beside a playing capture card
the app opens a Remote Play session with `controlOnly` (`EngineController::startRemoteControl`), the backend counts and
drops the console's keepalive video and audio without decoding (`ChiakiBackend.cpp:221,241`), at a keepalive profile of
1280x720, 30 FPS, 5000 kbps H.264 (`RemotePlay.cpp:924`). Veyra's 2.0.8 acceptance notes say users reported it working on
real consoles (`docs/history/releases/RELEASE_2.0.8_ACCEPTANCE_2026-10-15.md:29`): that is the upstream's user report,
not our measurement, and it implies the PS5 kept its HDMI picture during that session.

Gaps (CONFIRMED in code):
1. **Keyboard and mouse are not wired into it**: its 8 ms timer sends `controller.poll()` only (`RemotePlay.cpp:936-944`);
   `ps5KeyboardMouse` is created and polled only for a PS5 stream.
2. Its poll is 8 ms on a coarse `QTimer` (no `PreciseTimer`), and its owner loop sends one state per 4 ms tick on a
   waitable timer and never wakes for a new state (`RemotePlaySessionSource.cpp:265-273,305-308`): up to 8 + 4 ms of
   waiting that the stream path does not have.
3. The keepalive is 720p at 5 Mbps; the protocol's lowest preset is 640x360 at 2000 kbps (chiaki-ng
   `lib/src/session.c` `CHIAKI_VIDEO_RESOLUTION_PRESET_360p`). Less keepalive traffic would cost the console and the PC
   less; whether the PS5 changes its HDMI output (resolution, HDR) while a Remote Play session runs is HYPOTHESIS until
   his capture card shows it.
4. No input statistics in that loop.

## 6. What this stream changed (each opt-in; nothing changes for a user who does not choose it)

| Defect / item | Change | Status |
|---|---|---|
| D1, D4 | `timing=report` (`KbmInputProfile.h`): counts / time between the last report of the previous poll and of this poll, when reports come back to back; else the poll interval. Stop rule: a poll without a report repeats only until the next report was due and never a whole poll interval | implemented; automatically tested (Linux g++ driver; CI ck) |
| D2 | `stickSteps=8`: whole 8-bit steps, the first step beyond the game's dead zone for a moving stick, the remainder above the dead zone carried (at most one step) and used only by a poll that carries counts | implemented; automatically tested |
| D3 | the GTA V-style preset uses Ultra Low Latency (no window, no carry); legacy unchanged for everyone else | implemented; tested |
| GTA preset, menu context | `kbmGtaPcProfiles`, `kbmUseGtaPcPreset`, `PadAction::Menu`, `KbmProfileSet::menu`, `menu.bind` lines; page button and menu tab (`PushKbm.cpp`, `PushKbmBindings.qml`); `VEYRA_EXP_RP_KBM_PRESET=gta-pc` | core tested; bridge / QML blind Windows code (syntax check, QML lint in CI) |
| capture card | `VEYRA_EXP_RP_KBM_CONTROL=1`: keyboard and mouse in PS5 手柄控制 at the 4 ms precise poll, the owner sends each new state at once, owner tap for `[slipstream-input]` | blind Windows code (`RemotePlay.cpp` `startPs5Control`, `RemotePlaySessionSource.cpp` controller-only loop) |
| measurement | `stickChangePctP50/P95`, `stickDips` appended to `[slipstream-input]`; kit arm `i-gta-pc`; input cues with slow circles and flicks that stop dead | implemented; kit Python tests pass |

Answers to the orchestrator (21 Nov):
- **Moving the poll off the interface thread: not done.** WM_INPUT and the key messages arrive on the thread that owns
  the window; a poll on another thread needs a second, message-only window registered with `RIDEV_INPUTSINK` for the
  mouse, keys forwarded across threads to the pad (or read as raw keyboard input with their own repeat and focus
  rules), and the capture's release rules re-proven on two threads. That is a redesign of the capture written blind,
  not a contained switch. The busy-interface delay is measured instead: the kit's input session shows the interface
  poll's gap P50 / P95 / P99 / max and late polls per run (`[slipstream-input-poll]`).
- **Controller-only mode and the decode:** that loop never decodes (the backend drops the keepalive video on receipt),
  so a send can never wait behind a decode there (CONFIRMED in code). Its delays were the 8 ms coarse poll and the
  one-state-per-4-ms tick; `VEYRA_EXP_RP_KBM_CONTROL=1` removes both.
- **GTA VI reload:** neither published table (Shacknews 18 Nov, GTABase 21 Nov) lists one; R is unbound in the preset
  and named "check in game" on the page, in its name and in the guide.

Not changed (reasons): the game's time ramp needs a measurement on the console (a calibration step can be designed only
once its shape is seen); an axial dead-zone option waits for evidence that the game uses one; re-applying `ClipCursor`
every poll waits for a log showing the clip lost (a release is already clean); a 360p keepalive needs `Validation.cpp`
and `ChiakiBackend.cpp` to accept it and a console to show it works; an FG-on arm in the input session is refused by
the kit's rules (input arms carry no picture path), so frame generation's effect on mouse-to-photon is the camera
procedure in `docs/KEYBOARD_MOUSE.md`.

## 7. What only his PC and PS5 can show

- Whether the console reads the stick in 8 bits (`stickSteps=8` is right either way; its benefit needs that).
- `i-gta-pc` against `i-ultra` in the input session: stick change P50 / P95 and stick dips lower, event-to-send equal.
- The interface poll's gap P99 and late polls with his overlays and frame generation running.
- In GTA VI: the camera stops with the hand after a flick; no turn after the stop; the reload button.
- PS5 手柄控制 with keyboard and mouse beside his capture card: whether the HDMI picture stays (resolution, HDR) and
  `[slipstream-input]` there; mouse-to-photon by camera with frame generation off, 2x and 3x.
