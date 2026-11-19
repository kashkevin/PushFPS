# Keyboard and mouse on a PS5 stream

Experimental and off by default (`VEYRA_EXP_RP_KBM`). The console only ever sees a DualSense: the PC
turns keys and mouse movement into the buttons, triggers and sticks of a controller. **Unverified on
Windows and on a PS5**: everything below was written and unit-tested on Linux; nothing was built with
MSVC, run with a mouse or played. No value in it is tuned for any game.

This is input translation and nothing else. Every pad state is what your hands are doing at that moment:
no aim assistance, no recoil compensation, no macros, no repeated or timed input, nothing that reads
the picture. Many online games forbid playing with a mouse presented as a controller; whether you may
use this in a given game is your responsibility.

## Switching it on

Set the variables, then start PushFPS from the same window (PowerShell shown; `set NAME=value` in cmd):

```
$env:VEYRA_EXP_RP_KBM = "1"                              # mouse on the right stick; "gyro": mouse as the pad's motion sensor
$env:VEYRA_EXP_RP_KBM_PROFILE = "C:\Games\gta6-kbm.txt"   # optional: your profile file (below)
& "C:\Path\PushFPS.exe"
```

| Variable | Meaning |
|---|---|
| `VEYRA_EXP_RP_KBM` | `1` stick aiming, `gyro` motion aiming. Unset or `0`: the feature does not exist. |
| `VEYRA_EXP_RP_KBM_PROFILE` | path of a profile file. Unset: the built-in profiles. |
| `VEYRA_EXP_RP_KBM_SENS`, `_YSCALE`, `_FULL`, `_DEADZONE`, `_CURVE`, `_SMOOTH_MS`, `_GYRO_DEG`, `_INVERT_X`, `_INVERT_Y`, `_BINDS` | the first round's quick settings; each applies to all three profiles, the profile file wins |
| `VEYRA_EXP_RP_KBM_RAW_DRAIN=0` | read one mouse report per message instead of the buffered read (for comparison) |
| `VEYRA_EXP_RP_PAD_POLL_MS=1..8` | pad, keyboard and mouse poll interval, default 4 (see Rates) |

While a PS5 stream (not view-only) is in front, keyboard and mouse belong to the game: the cursor is
hidden and kept inside the window, and PushFPS's own interface does not get the keys.
**Ctrl+Alt+Shift+Z** gives them back until you leave the window and come back (on the way, Ctrl, Alt and
Shift act as whatever they are bound to).

Nothing stays pressed on the console. Every key and button is released, the triggers go up and the
sticks go back to the controller (or the centre) at once when the window loses the focus or leaves the
foreground, on Ctrl+Alt+Shift+Z, when the stream ends, when a keyboard or mouse you were using is
unplugged, and when you switch between the walking and the driving profile.

A controller can stay connected. Buttons add up, a trigger takes the deeper value, and each stick
follows whichever device acted last: push the stick and it has it, press a movement key or move the
mouse and they have it. A stick resting off-centre (drift) does not take over.

The log (`[remoteplay-kbm]` lines) says what is in use: the profile set, each profile's numbers
including what its smoothing window costs in milliseconds, every profile-file line that could not be
read, each capture and release with its reason, and the walk / drive switches.

## Built-in profiles (untuned guesses)

Used when no file is given, and the starting point a file changes. The layout follows the keyboard and
controller layouts of earlier GTA games from memory. **GTA 6 was not available to check it, and no
number was tuned on a console.**

| Key | Walking and aiming | Driving |
|---|---|---|
| W A S D | left stick | W throttle (R2), S brake (L2), A / D steer |
| Mouse | right stick (camera) | right stick (camera); `drive.mouse=steer` makes it a steering wheel |
| Left / right / middle mouse | R2 / L2 (aim) / R3 | L1 / none / R3 |
| Mouse 4 / 5 | L1 / R1 | none |
| Wheel up / down | D-pad left / right (60 ms per notch) | D-pad left / right |
| Shift, Space, F, R | Cross, Square, Triangle, Circle | Cross, R1, Triangle, Circle |
| Q, Tab, Ctrl, C, V, E | R1, L1, L3, R3, touchpad click, D-pad right | D-pad left, L1, Square, R3, touchpad click, L3 |
| H | none | D-pad right |
| Arrow keys | D-pad | D-pad |
| Esc, Backspace, Home | Options, Create, PS | the same |
| Alt | walk (half tilt) | none |
| X | fine aim: slower mouse while held | the same |
| Caps Lock | switch walking / driving | the same |

Aiming is active while the aim button (L2: right mouse, or the controller's L2) is held; it has the
walking keys and its own mouse numbers (sensitivity 0.6 instead of 1.0). Caps Lock still toggles
Windows' capitals state; bind another key (`all.bind.Caps=None`, `all.bind.F1=Drive`) if that bothers you.

## Profile file

Plain text, one `key=value` per line. A line starting with `#` or `;` is a comment (comments cannot
follow a value on the same line). Names are not case-sensitive. A line that cannot be read is logged
with its number and ignored; a value outside its range is refused, not adjusted. Limits: 64 KiB, 2000
lines, 512 characters per line. The file is read once, when the first stream of a run starts.

```
# Slipstream keyboard and mouse profile for one game.
name=GTA 6 first try

# On foot
walk.sens_x=1.0
walk.sens_y=0.8
walk.full=4000
walk.deadzone=0.12
walk.curve=1.0
walk.smooth_ms=33

# While the aim button (right mouse) is held
aim.sens_x=0.6
aim.sens_y=0.5
aim.deadzone=0.12
aim.fine=0.4

# In a vehicle (Caps Lock switches)
drive.trigger_rise_ms=150
drive.trigger_fall_ms=80
drive.move_rise_ms=120
drive.mouse=camera

# Keys
walk.bind.G=DLeft
aim.bind.Mouse2=Fine
drive.bind.Space=R1
```

Settings for the whole set:

| Key | Values | Built-in |
|---|---|---|
| `name` | up to 64 characters; shown in the log | built-in GTA-style layout (untuned guess) |
| `mode` | `stick`, `gyro` | from `VEYRA_EXP_RP_KBM` |
| `stick_bits` | 8 to 16: stick steps the output uses; 16 switches the carry of the rounding error off | 8 |
| `aim_while` | `none`, or the button or trigger that selects the aiming profile while held | `L2` |

Settings per profile: prefix `walk.`, `aim.`, `drive.`, or `all.` for the three of them.

| Key | Range | Built-in | Meaning |
|---|---|---|---|
| `sens_x`, `sens_y` | 0.01 to 100 | 1 (aim 0.6) | horizontal and vertical sensitivity |
| `full` | 100 to 1000000 | 4000 | mouse counts per second that mean a fully tilted stick |
| `deadzone` | 0 to 0.5 | 0.12 | the game's right-stick dead zone; the output starts just above it |
| `curve` | 0.25 to 4 | 1 | the game's response: turning speed grows with tilt to this power |
| `accel` | 0 to 4 | 0 (off: linear) | acceleration: counts of a poll are scaled by 1 + accel x speed / `full` |
| `accel_cap` | 1 to 8 | 2 | the most that scaling may reach |
| `smooth_ms` | 0 to 50 | 33 | window the mouse speed is taken over. Cost at a 4 ms poll if the game reads every pad state: 0 ms up to 7 ms, 2 ms on average for 8 to 11, 6 ms for 16 to 19, 14 ms for 32 to 35 (28 ms for the last part of a movement); nothing if the game reads one state per picture and the window is no longer than a picture |
| `carry_ms` | 0 to 250 | 40 | movement faster than a full tilt is kept this long (at full speed) and played out, the rest is dropped |
| `fine` | 0.05 to 1 | 0.5 | sensitivity factor while the Fine key is held |
| `invert_x`, `invert_y` | 0, 1 | 0 | |
| `gyro_deg` | 0.0005 to 1 | 0.03 | gyro mode: degrees of turn per mouse count |
| `mouse` | `camera`, `steer`, `none` | `camera` | what the mouse moves: the right stick, the steering wheel (left stick, left / right), nothing |
| `move_deadzone` | 0 to 0.5 | 0 (drive 0.10) | the game's left-stick dead zone; keys and wheel start above it |
| `move_rise_ms`, `move_fall_ms` | 0 to 2000 | 0, 0 (drive 120, 80) | time the keys take from centre to full tilt and back; 0: at once |
| `walk_tilt` | 0.1 to 1 | 0.5 | tilt while the Walk key is held |
| `trigger_rise_ms`, `trigger_fall_ms` | 0 to 2000 | 0, 0 (drive 150, 80) | time a key takes to press a trigger fully and to let it go; 0: at once |
| `trigger_start` | 0 to 0.9 | 0 (drive 0.15) | where a key press starts on the trigger: past the part the game ignores |
| `trigger_step` | 1 to 64 | 1 (drive 15) | trigger values are multiples of this; every new value is a packet to the console |
| `steer_range` | 10 to 100000 | 800 | mouse counts from the centre to full lock (`mouse=steer`) |
| `steer_return_ms` | 0 to 5000 | 0 | time from full lock back to the centre while the mouse rests; 0: the wheel stays |
| `steer_curve` | 0.25 to 4 | 1 | the game's steering response, as `curve` |

Keys: `<profile>.bind.<key>=<action>`. The aiming profile uses the walking profile's keys;
`aim.bind.<key>` makes one key differ while aiming. One key has one action per profile.

- Keys: `A` to `Z`, `0` to `9`, `F1` to `F12`, `Space`, `Shift`, `Ctrl`, `Alt` (left and right are the
  same key), `Tab`, `Esc`, `Enter`, `Back`, `Home`, `End`, `Insert`, `Delete`, `PageUp`, `PageDown`, `Up`,
  `Down`, `Left`, `Right`, `Caps`, `Grave`, `Minus`, `Plus`, `Comma`, `Period`, `Slash`, `Semicolon`, `Quote`,
  `LBracket`, `RBracket`, `Backslash`, `Num0` to `Num9`, `Mouse1` (left), `Mouse2` (middle), `Mouse3`
  (right), `Mouse4`, `Mouse5`, `WheelUp`, `WheelDown`.
- Actions: `Cross`, `Circle`, `Square`, `Triangle`, `L1`, `R1`, `L2`, `R2`, `L3`, `R3`, `Options`, `Share`
  (the Create button), `Touchpad` (click), `PS`, `DUp`, `DDown`, `DLeft`, `DRight`, `LUp`, `LDown`, `LLeft`,
  `LRight` (left stick), `Walk`, `Fine`, `Drive` (switch walking / driving), `None` (unbind).

## Tuning a new game

All of this needs the PS5 and your own eyes; none of it was done here. Change one value at a time,
restart PushFPS after editing the file, and keep the file per game.

1. **In the game**: stick sensitivity to maximum (a stick caps the turning speed; the mouse cannot turn
   faster than the game lets a stick turn), aim acceleration and smoothing off if offered, dead zone
   to minimum, aim assist off if offered. Write these into the file as comments.
2. **Dead zone** (`deadzone`): start at 0.02. Move the mouse as slowly as you can. Raise the value in
   steps of 0.02 until the slowest steady movement just moves the camera. Too low: a dead spot before
   the camera starts. Too high: the camera jumps or creeps when you barely touch the mouse.
3. **Full speed** (`full`): the game's fastest turn, expressed in mouse counts per second. It sets how
   far the mouse travels for a full turn. Push the controller's right stick fully to one side and time
   one complete turn: T seconds. Decide how many mouse counts a full turn should take: N (mouse DPI
   times the inches you want to move). Set `full` to N / T and leave `sens_x` at 1. Example: 1.5 s per
   turn and 800 DPI x 10 inches = 8000 counts give `full=5333`. Then N counts turn once around at any
   hand speed below the cap; `sens_x` and `sens_y` scale that afterwards.
4. **Curve** (`curve`): turn through the same mouse distance slowly and fast. If the fast one turns
   further, raise `curve`; if the slow one turns further, lower it. Equal distance, equal turn is the
   aim.
5. **Smoothing** (`smooth_ms`): make the same short flick twenty times with `smooth_ms=33` and with
   `smooth_ms=8`. If the turn varies clearly more with 8, the game reads one pad state per picture:
   keep the window at one picture (33 for 30 fps, 16 for 60 fps). If both are equally steady, the game
   reads every state: use 8, or 0 with a 1000 Hz mouse, and save the delay.
6. **Vertical** (`sens_y`), then the **aiming profile**: repeat steps 2 to 4 while holding the aim
   button with the `aim.` values. Games often use another dead zone and turn rate while aiming.
7. **Driving**: `drive.trigger_start` up until a tap on W just moves the car; `trigger_rise_ms` and
   `move_rise_ms` longer for gentler throttle and steering, shorter for sharper. Try `drive.mouse=steer`
   with `steer_range` to taste.
8. Try `VEYRA_EXP_RP_KBM=gyro` if the game has motion aiming: it has no dead zone, curve or speed cap
   and usually needs only `gyro_deg` (and `invert_x` / `invert_y`: the axis signs are untested).

## What cannot be fixed from the PC

- **The turning speed cap.** A stick at full tilt turns at the game's maximum; faster mouse movement is
  played out over the next 40 ms (`carry_ms`) and the rest is dropped. Only the in-game sensitivity
  raises the cap.
- **The game's own aim behaviour**: acceleration over time, smoothing, aim assist slow-down and the exact
  shape of its dead zone. Dead zone and curve here undo a simple model of it, not the game's real code.
  It will not feel like the PC version of a game.
- **The game's picture rate.** A 30 fps game looks at the pad every 33 ms at best. No setting on the PC
  makes it react sooner.
- **The delay of Remote Play itself**: console capture and encoding, network, decoding, display. See
  `docs/PS5_REMOTE_PLAY_EXPERIMENTS.md`.
- **Stick resolution.** A game reads 8 bits per axis; movement between two steps is carried and adds up,
  but the slowest steady turn is one step above the dead zone.
- **What the PC cannot know**: whether you are on foot, in a vehicle or in a menu. Walking / driving is
  your key press; menus are used with the D-pad keys and Cross / Circle, there is no mouse pointer.
- **Touchpad swipes, the microphone, text entry** are not mapped.

## Rates: how often a pad state leaves the PC

Read in the pinned chiaki-ng source (commit `0e169501`). Line numbers are those of the unpatched files;
`scripts/remoteplay/patches/0003-chiaki-feedback-history.patch` shifts the lines of `feedbacksender.c`
(the `TODO` at `:314` is at `:294` afterwards) and changes how button events are packed, which is noted
where it matters, not the timing described here.

| Step | Where | What limits it |
|---|---|---|
| Pad poll on the interface thread | `src/ui/bridge/RemotePlay.cpp:15` (`ps5PadPollMs`), `:561`, `:588` | a Qt precise timer: 4 ms by default, 1 to 8 with `VEYRA_EXP_RP_PAD_POLL_MS`. **This is the limit.** |
| Queue to the session thread | `include/veyra/remoteplay/ControllerQueue.h:33`, `src/source/RemotePlaySessionSource.cpp:304` | none: an unchanged state is dropped, a changed one wakes the owner at once; stick-only changes replace each other, button changes each keep their place |
| Owner hands it to Chiaki | `src/source/RemotePlaySessionSource.cpp:172-181`, `src/remoteplay/ChiakiBackend.cpp:250-278` | none: everything pending is submitted when it wakes |
| `chiaki_session_set_controller_state` | `lib/src/session.c:339-349` | none: copies the state under a mutex and calls the sender |
| `chiaki_feedback_sender_set_controller_state` | `lib/src/feedbacksender.c:77-99` | an unchanged state returns without waking anything (`:83-87`, comparison in `lib/src/controller.c:75`); a changed one sets `controller_state_changed` and signals the sender thread (`:93-96`) |
| Sender thread | `lib/src/feedbacksender.c:274-354` | **no minimum interval.** `FEEDBACK_STATE_TIMEOUT_MIN_MS` (8 ms, `:8`) is defined and never used: `:314` is `// TODO: FEEDBACK_STATE_TIMEOUT_MIN_MS`. The thread only waits (up to 200 ms, `:9`, `:286-296`) while nothing changed |
| One pass of the sender | `lib/src/feedbacksender.c:305-339` | at most one state packet (sticks and motion; sent when they differ from the last one sent, `:101-121`, `:319`, or after 200 ms, `:306`) and at most one history packet (buttons, triggers, touches) |
| Packet out | `lib/src/takion.c:753-773`, `:721-751` | none: encrypted and written to the UDP socket in the same call (`:746`), no queue, no pacing, no acknowledgement |

So a pad state can leave the PC as often as the poll produces a changed one: with a moving mouse that is
every poll, 250 packets a second at 4 ms and 1000 at 1 ms. Details that matter for mouse and keys:

- The sender reads the newest state when it runs (`feedbacksender.c:307`). If two states arrive before it
  wakes, the console gets the newer stick position only. That is right for a stick, which is a level.
- Buttons and triggers are events. With patch 0003 every change is a history event with a packet of its
  own (the console applies one new event per packet), 64 packets can queue
  (`lib/include/chiaki/feedbacksender.h:12`) and each packet repeats the last 8 events. **Every new
  trigger value is such an event**: this is why a key ramping a trigger (driving profile) moves in steps
  (`trigger_step`) instead of 255 values.
- Stick values travel as 16 bits (`lib/src/feedback.c:73-76`). What the console hands the game is not
  visible from here; a game reads 8 bits per axis according to the public homebrew SDK headers.

### Mouse polling against the pad poll (Linux model)

`tools/remoteplay_latency_bench/input_sim.cpp --mode mouse --table`: a hand moving at a steady 1500 counts
a second, mouse reports at four rates against four poll intervals, through the real `MouseAim` code, 20
simulated seconds per row, timer up to 0.3 ms late. **A Linux model in virtual time: no Windows timer,
no USB stack, no console.** It shows what the two clocks do to each other and nothing about feel.

- *Wait*: from a mouse report to the poll that sends it. This is the delay the poll interval adds.
- *States sent*: packets a second with the built-in profile (33 ms window, 8-bit steps).
- *Polls without a new report*: timer work that found nothing (wasted on the PC side).
- *Reports per poll*: reports merged into one state; their separate timing never reaches the console
  (wasted on the mouse side). At this hand speed an 8000 Hz mouse has movement in 1500 reports a second.
- *Tilt error*: how far the stick tilt is from the hand's true speed (RMS, per cent of the tilt above the
  dead zone), without smoothing and with an 8 ms and a 33 ms window.

| Mouse | Poll | Wait mean | Wait P95 | Wait max | States sent | Polls without a new report | Reports per poll | Tilt error, no window | 8 ms window | 33 ms window |
|---|---|---|---|---|---|---|---|---|---|---|
| 125 Hz | 1 ms | 0.50 ms | 0.98 ms | 1.24 ms | 658 /s | 88 % | 1.0 | 144 % | 14 % | 3.0 % |
| 125 Hz | 2 ms | 1.00 ms | 1.91 ms | 2.24 ms | 226 /s | 75 % | 1.0 | 129 % | 28 % | 2.8 % |
| 125 Hz | 4 ms | 2.01 ms | 3.81 ms | 4.15 ms | 92 /s | 50 % | 1.0 | 120 % | 85 % | 3.1 % |
| 125 Hz | 8 ms | 3.70 ms | 7.52 ms | 8.15 ms | 46 /s | 1 % | 1.0 | 15 % | 15 % | 3.4 % |
| 500 Hz | 1 ms | 0.51 ms | 1.00 ms | 1.26 ms | 739 /s | 50 % | 1.0 | 121 % | 11 % | 2.4 % |
| 500 Hz | 2 ms | 1.00 ms | 1.92 ms | 2.26 ms | 196 /s | 3 % | 1.0 | 27 % | 6 % | 1.4 % |
| 500 Hz | 4 ms | 2.00 ms | 3.80 ms | 4.22 ms | 97 /s | 0 % | 2.0 | 11 % | 9 % | 1.5 % |
| 500 Hz | 8 ms | 3.99 ms | 7.58 ms | 8.23 ms | 50 /s | 0 % | 4.0 | 5 % | 5 % | 1.4 % |
| 1000 Hz | 1 ms | 0.51 ms | 1.00 ms | 1.26 ms | 544 /s | 5 % | 1.1 | 49 % | 5 % | 1.2 % |
| 1000 Hz | 2 ms | 1.00 ms | 1.91 ms | 2.27 ms | 211 /s | 0 % | 2.0 | 18 % | 5 % | 1.2 % |
| 1000 Hz | 4 ms | 2.00 ms | 3.80 ms | 4.26 ms | 102 /s | 0 % | 4.0 | 6 % | 5 % | 0.9 % |
| 1000 Hz | 8 ms | 4.00 ms | 7.59 ms | 8.25 ms | 51 /s | 0 % | 8.0 | 3 % | 3 % | 0.9 % |
| 8000 Hz | 1 ms | 0.51 ms | 0.97 ms | 1.21 ms | 483 /s | 0 % | 1.5 | 34 % | 4 % | 0.9 % |
| 8000 Hz | 2 ms | 0.85 ms | 1.61 ms | 2.27 ms | 188 /s | 0 % | 3.0 | 8 % | 2 % | 0.5 % |
| 8000 Hz | 4 ms | 1.86 ms | 3.55 ms | 4.28 ms | 111 /s | 0 % | 6.0 | 6 % | 4 % | 0.8 % |
| 8000 Hz | 8 ms | 3.85 ms | 7.46 ms | 8.28 ms | 52 /s | 0 % | 12.0 | 3 % | 3 % | 0.7 % |

Reading it:

- The poll adds half its interval on average: 2 ms at the default 4 ms, 0.5 ms at 1 ms. A 30 fps game
  shows a picture every 33 ms; the whole difference between 4 ms and 1 ms is 1.5 ms.
- Polling faster than the mouse reports is wasted: a 125 Hz mouse leaves 88 % of 1 ms polls empty, a
  500 Hz mouse half of them.
- Without a smoothing window the stick tilt is noisy whenever a poll holds only a few reports (49 % at
  1000 Hz and 1 ms). The window, not the poll rate, is what makes the tilt steady.

**Recommended default: keep the 4 ms poll.** With a 1000 Hz or faster mouse `VEYRA_EXP_RP_PAD_POLL_MS=2`
is a reasonable experiment (1 ms less wait on average for twice the timer work and packets); 1 ms only
if a measurement on the real machine shows a gain. Do not poll faster than the mouse reports.

### Why the built-in smoothing window is 33 ms (Linux model)

`input_sim --mode stroke --table`: a short stroke (20 counts in 10 ms, 1000 Hz mouse, 4 ms poll) read by
a game that takes the newest pad state once per picture at 30 fps and turns at that speed for the whole
picture; 4000 strokes at random moments. **A model, and how GTA 6 reads the pad is an assumption.**

| Window | Turned, mean | Spread (standard deviation) | Least | Most | Delay it would add if the game read every state |
|---|---|---|---|---|---|
| 0 ms | 102 % | 139 % | 0 % | 413 % | 0.0 ms mean, 0 ms at most |
| 8 ms | 101 % | 130 % | 0 % | 413 % | 2.0 ms mean, 4 ms at most |
| 16 ms | 100 % | 91 % | 0 % | 284 % | 6.0 ms mean, 12 ms at most |
| 33 ms | 100 % | 11 % | 52 % | 105 % | 14.0 ms mean, 28 ms at most |
| 50 ms | 100 % | 26 % | 69 % | 140 % | 22.0 ms mean, 44 ms at most |

If the game reads one state per picture, a window shorter than a picture makes the same hand movement
turn anything between nothing and four times too much, depending on when it happened; a window of one
picture makes it turn about the same every time. In that case the window adds no delay the game could
see: the game would not have looked sooner. If the game instead reads every state, the last column is
what the window costs. Which of the two holds has to be found on the console (tuning step 5).

### To confirm on the PS5

1. Whether a shorter poll shortens button-to-screen time at all: `scripts/remoteplay/latency-ab.ps1`
   with `-Experiment Poll2ms` and `Poll1ms` against the default, arms alternating, 120 presses per arm.
2. Whether the console keeps up with 500 to 1000 state packets a second (no growing delay, no lost
   presses) and whether Qt's timer really fires every 1 or 2 ms on that PC.
3. Whether the game reads one pad state per picture (tuning step 5 below).
4. Whether the stick reaches the game in 8 bits and how the console rounds it.

## What was verified

- Linux, GCC 13: the translation, hand-over, profile switching, ramps, profile parser (malformed and
  random input included), capture state machine and Raw Input buffer walk are unit-tested
  (`tests/remoteplay/KbmProfileTests.inc`, 41 cases in `CoreTests.cpp`'s 145), also under
  ThreadSanitizer and AddressSanitizer + UBSan.
- The Windows-only capture (`include/veyra/ui/RemotePlayKeyboardMouse.h`) and the bridge passed a
  mingw-w64 syntax check. That is not a build.
- Not done: an MSVC build, any run on Windows, any mouse or keyboard input, any PS5, any game. The
  tables under Rates are a Linux model.
