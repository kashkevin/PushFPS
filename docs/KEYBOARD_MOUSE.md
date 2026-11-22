# Keyboard and mouse on a PS5 stream

Experimental and off by default (`VEYRA_EXP_RP_KBM`, or the switch on the Mouse & Keyboard page). The
console only ever sees a DualSense: the PC turns keys and mouse movement into the buttons, triggers and
sticks of a controller. **Never used with a PS5**: it was written and unit-tested on Linux, and the part
that existed before sprint 5 has compiled with MSVC and its tests pass on GitHub's Windows runner, but it
has never been run with a mouse against a console or played. No value in it is tuned for any game. The
sprint 5 additions (input performance profiles, statistics, the GTA VI layout, leading-edge transmission)
are marked where they appear: Linux-tested maths, Windows code written without a Windows compiler.

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
| `VEYRA_EXP_RP_KBM_PROFILE` | path of a profile file. Unset: the calibrated profile of the game named in `slipstream-kbm.ini` if there is one (see Calibrating a game), else the built-in profiles. |
| `VEYRA_EXP_RP_KBM_SENS`, `_YSCALE`, `_FULL`, `_DEADZONE`, `_CURVE`, `_SMOOTH_MS`, `_GYRO_DEG`, `_INVERT_X`, `_INVERT_Y`, `_BINDS` | the first round's quick settings; each applies to all three profiles, the profile file wins |
| `VEYRA_EXP_RP_KBM_RAW_DRAIN=0` | read one mouse report per message instead of the buffered read (for comparison) |
| `VEYRA_EXP_RP_KBM_INPUT_PROFILE` | `legacy` (the default), `ultra-low-latency`, `adaptive-precision`, `custom`: see Input performance profiles |
| `VEYRA_EXP_RP_KBM_INPUT` | `key=value;key=value`: single values of the input profile; both win over the page's choice |
| `VEYRA_EXP_RP_KBM_EVENT_SEND=1` | leading-edge transmission of button presses (Where input time goes); off by default |
| `VEYRA_EXP_RP_STATS=1` | with keyboard and mouse on: the `[slipstream-input]` and `[slipstream-input-poll]` lines (Where input time goes) |
| `VEYRA_EXP_RP_PAD_POLL_MS=1..8` | pad, keyboard and mouse poll interval, default 4 (see Rates) |

While a PS5 stream (not view-only) is in front, keyboard and mouse belong to the game: the cursor is
hidden and kept inside the window, and PushFPS's own interface does not get the keys.
**Ctrl+Alt+Shift+Z** gives them back until you leave the window and come back (on the way, Ctrl, Alt and
Shift act as whatever they are bound to).

Nothing stays pressed on the console. Every key and button is released, the triggers go up and the
sticks go back to the controller (or the centre) at once when the window loses the focus or leaves the
foreground, on Ctrl+Alt+Shift+Z, when the stream ends, and when a keyboard or mouse you were using is
unplugged. When you switch between the walking and the driving profile, every held key whose meaning
changes is released (W stops walking instead of becoming the throttle) and ignored until you press it
again; a key that means the same in both stays held, and mouse movement on its way keeps turning the
camera, so the camera does not jump.

A controller can stay connected. Buttons add up, a trigger takes the deeper value, and each stick
follows whichever device acted last: push the stick and it has it, press a movement key or move the
mouse and they have it. A stick resting off-centre (drift) does not take over.

The log (`[remoteplay-kbm]` lines) says what is in use: the profile set, each profile's numbers
including what its smoothing window costs in milliseconds, every profile-file line that could not be
read, each capture and release with its reason, and the walk / drive switches.

## Input performance profiles

How mouse movement becomes a stick position. **Unverified on Windows and never used with a PS5**: the maths is
unit-tested on Linux; whether any of it feels right can only be judged by playing. Nothing is switched on by
itself: without a choice the mouse is translated exactly as before (`legacy`).

| Profile | What it does | Smoothing | Faster than the game can turn |
|---|---|---|---|
| `legacy` (default) | the translation as it was: mouse speed over a window (8 ms built in, `smooth_ms`), whole 8-bit stick steps with the remainder carried | the profile file's window | carried for `carry_ms`, the rest dropped |
| `ultra-low-latency` | the stick value of a pad poll comes from the mouse counts of that poll and nothing older; straight line; the proven 4 ms schedule | none, cannot be switched on | dropped |
| `adaptive-precision` | the same immediate transform with your own numbers per context (walk, aim, sprint, drive): sensitivity per axis, ADS factor, a curve per context, a tilt cap | none, cannot be switched on | dropped (carry is a choice in `custom`) |
| `custom` | every value editable, including an optional averaging window and a bounded carry | `smoothingMs`, 0 to 50, 0 = off | `drop` or `carry` for at most `carryMs` (0 to 50) |

Choose one on the Mouse & Keyboard page, with `VEYRA_EXP_RP_KBM_INPUT_PROFILE`, or with `input.profile=<id>` in the
profile file. In the three new profiles:

- **Immediate** means: no window, no waiting for later samples. When the hand stops, the next pad state has the
  stick at rest (with a 125 Hz mouse at a 4 ms poll: one report interval later at most, because a poll that falls
  between two reports repeats the last value instead of reading a stop; 250 Hz and faster: no repeat). What that
  costs: with few reports per poll the tilt is coarser (a 1000 Hz mouse gives 4 reports per 4 ms poll, so the speed is
  known in steps of a quarter of the counts; an 8000 Hz mouse or more DPI with lower sensitivity gives finer steps
  without any delay; derived from the arithmetic, not tested on hardware). The legacy window hides that coarseness
  by adding delay; here you see it instead.
- **Stick aiming only.** In gyro mode (`VEYRA_EXP_RP_KBM=gyro`, `mode=gyro`) the new profiles change nothing in the
  camera: the mouse still goes through `MouseAim`, with the profile file's smoothing window (8 ms built in), whichever
  input profile is chosen. Keys, contexts and the rest of the profile apply as in stick mode.
- **The game calibration is used** (`<ctx>.game=1`, the default of all three): dead zone, full-turn speed, response
  curve and vertical / horizontal ratio come from the profile file of the way of playing in use (`walk.` / `aim.` /
  `drive.` `deadzone`, `full`, `curve` or `curve_25/50/75`, `sens_y / sens_x`), exactly the numbers Calibrating a game
  writes: `tilt = deadzone + (1 - deadzone) x inverse(your curve(speed / full))`. The smallest movement leaves the
  game's dead zone at once. With `<ctx>.game=0` the context's own `deadzone` and `fullSpeed` are used and the game's
  response is taken as straight.
- **Contexts** switch by themselves: `aim` while the aim button is held, `drive` after your on-foot / car key,
  `sprint` while the sprint key (`sprint_while`, Shift in both built-in layouts) is held with a movement key, else
  `walk`. `context=walk|aim|sprint|drive` (or the page) overrides that by hand; `auto` gives it back. A change acts on
  the very next pad state and never lets go of a held key (unit-tested, also for a change of the whole profile in the
  middle of a movement).
- **Curves** shape hand speed before the game's inverse: `linear` (equal distance, equal turn: the default
  everywhere except `adaptive-precision`'s aiming and driving), `power` (`out = u^exponent`; note the direction: to
  cancel by hand a game that turns with tilt^c the exponent is 1/c, and with the game calibration on that is already
  done for you), `precision-speed` (two straight segments: below `kneeIn` of full speed the output rises to
  `kneeOut`, then straight on to full). `adaptive-precision` starts with a mild `precision-speed` (0.25 -> 0.20) while
  aiming and `power` 1.25 in a car: untuned guesses. Any curve that is not straight gives up "equal distance, equal
  turn" (the model below shows what that costs a player who expects it); `aim.curve=linear` takes it back.
- **`maxTilt`** (0.1 to 1, default 1 = off) caps the stick: some games add a turn that ramps up over time in the
  outer ring of the stick. With a cap, what counts as "faster than the stick can show" starts at the cap. (The idea of
  an output cap is xcontrol's; see Compared with xcontrol and PadForge.)
- **A tap between two polls is not lost**: a key pressed and let go before the next pad poll (a late poll) is held
  for exactly that one poll. (`legacy` keeps its old behaviour: such a tap is not seen.)
- **Self-check** (`adaptive-precision`, `custom`): every 16th poll with mouse movement times the transform in use and
  the simple one on copies of the same counts. Three 5 s windows in a row in which the one in use is slower by more
  than max(250 us, the simple one's cost) at P95, or (statistics on) in which pad states wait more than 2 ms longer for
  the sender than they did under `legacy` / `ultra-low-latency` in this session, switch to the simple path
  (`ultra-low-latency` with your sensitivities, dead zone and cap). Held keys stay held; the log and
  `fallback=translate-slower|queue-older` say why; choosing a profile again ends it.

Values (`VEYRA_EXP_RP_KBM_INPUT`, the page, or `input.<key>=<value>` lines in the profile file; `<ctx>` is `walk`, `aim`,
`sprint`, `drive` or `all`; a value outside its range is refused, not adjusted):

| Key | Range | Built-in | Meaning |
|---|---|---|---|
| `profile` | the four ids | `legacy` | |
| `ads` | 0.05 to 4 | 0.6 | sensitivity factor in the aim context |
| `smoothingMs` | 0 to 50 | 0 | `custom` only: averaging window; the two named profiles force 0 |
| `context` | `auto`, `walk`, `aim`, `sprint`, `drive` | `auto` | manual context |
| `<ctx>.sensX`, `<ctx>.sensY` | 0.01 to 100 | 1 | horizontal / vertical sensitivity |
| `<ctx>.curve`, `.exponent`, `.kneeIn`, `.kneeOut` | `linear` `power` `precision-speed`; 0.25 to 4; 0.05 to 0.95; 0.01 to 0.99 | `linear` | your own shaping |
| `<ctx>.game` | 0, 1 | 1 in the three new profiles | use the game calibration of the profile file |
| `<ctx>.deadzone`, `<ctx>.fullSpeed` | 0 to 0.5; 100 to 1000000 | 0.12; 4000 | used with `game=0` |
| `<ctx>.saturation`, `<ctx>.carryMs` | `drop`, `carry`; 0 to 50 | `drop`; 0 | `ultra-low-latency` always drops |
| `<ctx>.maxTilt` | 0.1 to 1 | 1 | tilt cap |
| `transmit` | `proven`, `leading-edge` | `proven` | `custom` only: see leading-edge transmission under Where input time goes |

**Fitting to your hand** (the page's wizard; `KbmPreferenceFit.h`): a quarter turn the way you want it, your fastest
comfortable flicks and your slowest steady movement, each 3 to 10 times while the camera stands still. From them:
counts per turn = 4 x the median quarter turn; full stick = counts per turn / the game's full-turn time (from the game
calibration; without it, your fast-flick speed, and the result says that the distance cannot be honoured); a
`precision-speed` knee at twice your slowest steady speed, or a straight line when slow and fast are too close.
The same motions always give the same numbers, the result explains each one in words, nothing is applied until
you confirm, and nothing adapts during play. It measures your hand, never the picture.

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

## GTA VI layout (from press tables, untuned, unverified against the game)

`kbmGtaProfiles()`, offered on the Mouse & Keyboard page: PC-style keys on the game's PS5 controller layout (default
control scheme). The controller layout is taken from two press tables that agree (Shacknews, 18 November 2026;
GTABase, 20 November 2026), not from the publisher, whose settings article has no button table, and **not from the
game: nobody here has played it**. Every key can be changed on the page or in the profile file.

| Key | On foot | While aiming (right mouse held) | Driving |
|---|---|---|---|
| W A S D | move (left stick) | move | W accelerate (R2), S brake (L2), A / D steer |
| Mouse | camera | camera | camera |
| Left / right mouse | shoot (R2) / aim (L2) | the same | shoot (R1) / none |
| Shift | run (L3) | run | none |
| Space | jump (Cross) | jump | handbrake (Cross) |
| Ctrl, R, F | crouch (Circle), melee (Square), interact (Triangle) | F: quick swap (Triangle) | F: exit (Triangle) |
| Q, Tab | cover (R1), holster / hold: loadout (L1) | the same | none |
| E | none | switch shoulder (D-pad left) | horn (L3) |
| Wheel up / down | none | scope zoom (D-pad up / down, 60 ms per notch) | none |
| Arrow keys | D-pad (up: phone) | D-pad | D-pad |
| Esc or P, Backspace, Home, M or V | Options, Create, PS, touchpad click | the same | the same |
| C or middle mouse | R3 | R3 | R3 |
| Alt, X, Caps Lock | walk (half tilt), fine aim, on foot / car | the same | fine aim, on foot / car |

Actions the tables do not name (reload, weapon wheel details, radio, headlights, camera) are not bound: bind them
once you know the game's button. The built-in set above (what `VEYRA_EXP_RP_KBM=1` without a file uses) is the earlier
layout from memory of the previous games and stays as it was; this one is a choice.

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
walk.smooth_ms=8

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
| `game` | up to 64 characters: your name for the game (written by the calibration; never detected) | none |
| `counts_per_360` | 100 to 1000000: mouse counts for one full turn, the calibration's target | 8000 when calibrating |
| `sprint_while` | `none`, or the button or trigger that means sprinting while held with a movement key (only names the `sprint` context of the input profiles) | `Cross` (GTA VI layout: `L3`) |
| `input.<key>` | the input performance profile's values (see Input performance profiles) | `legacy` |

A file written before these two keys existed loads exactly as before (unit-tested), and `kbmProfileText` (the page's
Save) writes every line, the built-in ones included, so a saved file reads back as the same set.

Settings per profile: prefix `walk.`, `aim.`, `drive.`, or `all.` for the three of them.

| Key | Range | Built-in | Meaning |
|---|---|---|---|
| `sens_x`, `sens_y` | 0.01 to 100 | 1 (aim 0.6) | horizontal and vertical sensitivity |
| `full` | 100 to 1000000 | 4000 | mouse counts per second that mean a fully tilted stick |
| `deadzone` | 0 to 0.5 | 0.12 | the game's right-stick dead zone; the output starts just above it |
| `curve` | 0.25 to 4 | 1 | the game's response: turning speed grows with tilt to this power |
| `accel` | 0 to 4 | 0 (off: linear) | acceleration: counts of a poll are scaled by 1 + accel x speed / `full` |
| `accel_cap` | 1 to 8 | 2 | the most that scaling may reach |
| `smooth_ms` | 0 to 50 | 8 | window the mouse speed is taken over. Cost at a 4 ms poll if the game reads every pad state: 0 ms up to 7 ms, 2 ms on average for 8 to 11, 6 ms for 16 to 19, 14 ms for 32 to 35 (28 ms for the last part of a movement); nothing if the game reads one state per picture and the window is no longer than a picture |
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
| `curve_25`, `curve_50`, `curve_75` | 0 to 1 | 0 (off) | the game's measured turning speed at 25, 50 and 75 % of the tilt above the dead zone, as a share of its fastest turn (written by the calibration). Used instead of `curve` when all three are set and rise; between them the response is taken as straight. A set that does not rise is reported and left out |

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

The Mouse & Keyboard page (`src/ui/bridge/PushKbm.cpp`) reads and writes these files on the interface
thread, which is also the thread of the pad poll, and only when you act on the page (load, save, reset,
"Finish and apply" after a calibration); a file is at most 64 KB and written atomically. Nothing reads or
writes a profile file while you play unless you asked for it there.

## Calibrating a game

A guided measurement of how the game answers the right stick, a few minutes per game, that writes the
profile numbers for you. **Unverified on Windows and on a PS5**: the steps, the maths and the file
were tested on Linux against a simulated game (below); no real game was calibrated.

Be clear about what it is: the console only ever sees an analog stick. The calibration cannot make a
stick into a mouse; it measures the game's dead zone, response curve, fastest turn and vertical speed
so that the mouse speed is placed on the stick to turn the camera as evenly as those measurements
allow. It sends only plain stick deflections (and, in the aiming steps, holds the aim button once you
agreed), each only after you tapped to start that step while watching the screen. It reads nothing
from the picture: every measurement is your own tap.

Before you start: set the in-game stick sensitivity the way you will play, aim acceleration, smoothing
and aim assist off if the game offers it, and stand somewhere open with a landmark ahead (a lamp
post, a corner). Optional: in the folder of `settings.ini` (`%LOCALAPPDATA%\Veyra\remoteplay`), create
`slipstream-kbm.ini` with one line `game=GTA 6` (your own name for the game), and put
`counts_per_360=<counts>` in your profile if you want another distance per turn than 8000 counts
(800 DPI and 10 inches).

Keys (only while keyboard and mouse belong to the stream):

| Key | What it does |
|---|---|
| Ctrl+Alt+Shift+K | start; again: stop without writing anything |
| Space or left mouse | tap: start the step the log asks for, then mark what it asks for |
| Enter | skip this step (its value stays as it was); at the aiming or vehicle question: skip that profile |
| Esc | stop without writing anything |

Between steps (where the aim button may be held for the game while you agree to the aiming profile)
the calibration waits at most 60 s: with no tap or Enter for a minute it stops, releases every button
and writes nothing (unit-tested).

Every other key keeps working, so you can walk to a better spot between steps. The log
(`[remoteplay-kbm] calibration ...` lines) says what each step wants; read it on a second screen or
with a log viewer. **The prompts appear only as log lines**, nowhere in the picture: keep the log
visible on a second screen (or in a window beside the stream) before you start, for example with
`Get-Content logs\veyra-qml.log -Wait -Tail 20` in PowerShell, run in the PushFPS folder.

With the built-in layout, Ctrl, Alt and Shift are game keys (Ctrl = L3, Alt = Walk, Shift = Cross).
While you press Ctrl+Alt+Shift+K they reach the game for a few polls before the chord is recognised:
expect a crouch/walk/jump twitch when starting or stopping the calibration, and start it standing
somewhere safe.

The steps, for walking, then for aiming, then (only if you tap while sitting in a
vehicle) for driving:

1. **Dead zone, rising**: the stick tilts slowly from the centre (5 % a second). Tap the moment the
   camera starts to turn.
2. **Dead zone, falling**: the stick jumps to 60 % and slowly returns. Tap the moment the camera stops.
   Both taps are late by the same reaction and stream delay, in opposite directions: their mean
   cancels it.
3. **Full turn**: the stick is held fully right. Tap each time your landmark crosses the centre of the
   screen, three times: two turns, averaged. Constant delay cancels in the differences.
4. **Response at 25, 50 and 75 %** of the tilt above the measured dead zone: the same three taps, at
   every full turn, or at every quarter turn when a full turn would take longer than about 4 s (the
   log says which; use four landmarks or the compass).
5. **Vertical, growing and shrinking**: level the camera on the horizon with a mark ahead. The camera
   swings right and back, then up and back, again and again. The up swing grows a little each time:
   tap when it looks as large as the right swing. Then the same with the up swing shrinking. The mean
   of the two cancels the delay.

A step without a tap in time (or two taps that disagree: dead-zone taps more than 0.12 apart, turn
intervals more than 20 % apart, vertical taps more than a factor 1.3 apart) is asked once more; the
second miss keeps the profile's own value. Without the full turn, the response steps are left out
(they are read against it).

At the end the profile is written and used at once (every key released):

- into the `VEYRA_EXP_RP_KBM_PROFILE` file if that is set, else into `slipstream-kbm-<game>.txt` next to
  `settings.ini` (`slipstream-kbm-default.txt` without a `game=` line). The log gives the path.
- Every line the file had stays; the measured numbers go into one block between two marker lines at
  the end, which a later calibration replaces. The previous file is kept as `<file>.bak` first; if
  that copy fails nothing is written. The FIRST backup is kept: when `<file>.bak` already exists (your
  file from before the first calibration) it is not replaced, and the log says the earlier backup was
  kept. Delete it yourself to take a new one.
- A profile file larger than the size limit (64 KiB) is not rewritten: only its beginning could be
  read, and writing that back would cut it short. The log says so; the new numbers are used until
  PushFPS closes.
- A network path in `VEYRA_EXP_RP_KBM_PROFILE` (`\\server\share\...` or `//server/...`) is refused.
  A mapped drive letter that points at a share is not recognised: keep the profile on a local disk.
- At the next start, with `VEYRA_EXP_RP_KBM_PROFILE` unset, `slipstream-kbm-<game>.txt` is read when
  `slipstream-kbm.ini` names that game. A file without a `game=` line is never loaded by itself.

What the numbers become, per profile (only what was measured; everything else stays):

| Measured | Written |
|---|---|
| dead zone | `deadzone` |
| full turn T | `full` = `counts_per_360` / T, `sens_x=1`; `carry_ms` = the time the game takes for 45 degrees at full speed, at most 50 ms: a flick faster than the game's fastest turn is played out for that long, the rest is dropped. (Until 19 November 2026 the cap was 250 ms, so any game with a full turn of 2 s or more kept the stick fully tilted about 284 ms after the hand stopped: a ghost turn. Now the stick is centred within 50 ms + one poll + the smoothing window; unit-tested with model games of 2 s and 4 s per turn) |
| vertical ratio | `sens_y`: equal turn for equal mouse counts up and sideways (lower it afterwards if you prefer slower vertical aim) |
| response | `curve`: the power that fits the three points best; `curve_25`, `curve_50`, `curve_75`: the points themselves when they rise |

Then `counts_per_360` counts turn the camera once around at any hand speed the stick can follow, sideways
and up, whether you aim or not. Lower `aim.sens_x` / `aim.sens_y` afterwards if you want slower aiming.

### Calibration against a simulated game (Linux model)

`tools/remoteplay_latency_bench/input_sim --mode calibrate --table`: four simulated games with known dead
zone, response, fastest turn and vertical speed (a separate set while aiming), read in 8 bits at every
4 ms poll; a simulated player who taps 250 ms late with an independent error of up to +/- 80 ms per tap
and notices camera movement above 1 degree a second; 20 runs per game. *Linearity error*: steady mouse
speeds from 2 to 90 % of the profile's full speed through the real `MouseAim`; RMS deviation of the turn
per mouse count from its median, before (the built-in profile) and after (the calibrated one). *Scale
error*: the turn per count against the 8000 counts per turn asked for. **A Linux model: how a real game
reads the stick and shapes its response, and how a real player sees and taps, are assumptions. It shows
what the calibration can recover under them, nothing about any real game.**

| Game (walk / aim) | Profile | Dead zone true / measured | Full turn true / measured | Vertical ratio true / measured | Linearity error before | after (mean, worst) | after, 15-90 % only | Scale error after (mean, worst) | Minutes |
|---|---|---|---|---|---|---|---|---|---|
| power 1.8, dead zone 0.20 | walk | 0.20 / 0.240 | 1.50 s / 1.482 s | 1.67 / 1.651 | 89 % | 10.1 %, 11.8 % | 2.1 % | 2.2 %, 4.2 % | 3.5 |
| power 1.8, dead zone 0.20 | aim | 0.15 / 0.177 | 3.00 s / 2.986 s | 1.33 / 1.337 | 51 % | 4.6 %, 5.7 % | 1.1 % | 1.1 %, 2.1 % | 3.5 |
| power 1.0, dead zone 0.10 | walk | 0.10 / 0.106 | 1.20 s / 1.182 s | 1.25 / 1.244 | 31 % | 9.2 %, 15.8 % | 1.6 % | 1.2 %, 3.1 % | 2.8 |
| power 1.0, dead zone 0.10 | aim | 0.10 / 0.106 | 2.40 s / 2.385 s | 1.25 / 1.247 | 31 % | 9.7 %, 16.4 % | 1.2 % | 1.1 %, 3.4 % | 2.8 |
| power 2.5, dead zone 0.25 | walk | 0.25 / 0.342 | 2.00 s / 1.982 s | 2.00 / 2.042 | 245 % | 7.3 %, 8.0 % | 2.6 % | 4.0 %, 5.4 % | 4.5 |
| power 2.5, dead zone 0.25 | aim | 0.20 / 0.288 | 4.00 s / 3.986 s | 1.67 / 1.667 | 107 % | 5.7 %, 7.6 % | 2.0 % | 2.1 %, 4.3 % | 4.5 |
| two zones, dead zone 0.15 | walk | 0.15 / 0.153 | 1.33 s / 1.316 s | 1.67 / 1.677 | 50 % | 5.0 %, 6.8 % | 5.1 % | 0.9 %, 2.6 % | 3.8 |
| two zones, dead zone 0.15 | aim | 0.12 / 0.138 | 2.67 s / 2.653 s | 1.43 / 1.416 | 33 % | 12.3 %, 13.0 % | 6.4 % | 0.9 %, 2.1 % | 3.8 |

Reading it:

- The full turn and the vertical ratio come out within about 2 % and the turn per count within 6 % in
  the worst run: the taps' fixed delay cancels, and +/- 80 ms of tap error averages out.
- The dead zone comes out high when the game's response is curved (0.24 for 0.20, 0.34 for 0.25):
  the player cannot see the camera move slower than about a degree a second, so both taps land
  where the camera turns visibly. That is most of the remaining error, and it sits at the slowest
  speeds: from 15 % of full speed upwards the error is 1 to 3 % for power-shaped games.
- A game whose response is not a power (the "two zones" rows: slow inner zone, fast outer zone) keeps
  5 to 7 % even at higher speeds: the three points cannot place the kink exactly.
- Without the calibration the same games are 31 to 245 % off.

## Mouse report rate

While keyboard and mouse are captured, the mouse's report rate is measured from when Raw Input
reports are read (Raw Input has no time per report): reports with movement are counted in 100 ms bins,
only bins inside a continuous movement count, and the median of the five busiest of the last 64 is
snapped to 125, 250, 500, 1000, 2000, 4000 or 8000 Hz. Move the mouse briskly for a second or two; a slow
hand gives fewer reports than the mouse can send, so the rate can only be read too low, never too high.
One log line gives the class, the poll interval, whether the poll is shorter than one report interval
(then some polls find nothing new: wasted work, and `VEYRA_EXP_RP_PAD_POLL_MS` could be longer), and the
shortest smoothing window: two report intervals (16 ms at 125 Hz, 2 ms at 1000 Hz). That floor is used
whatever `smooth_ms` says, so a poll between two reports of a slow mouse does not read as a stop
(unit-tested: a 125 Hz mouse at a 4 ms poll with `smooth_ms=0` sends a zero tilt on every other poll
without it, on none with it). With the built-in 8 ms window the floor decides for mice slower than 250 Hz; with a 33 ms window it changes nothing.

## Tuning a new game by hand

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

## Where input time goes

Read in the code of this branch and in the pinned chiaki-ng source; **nothing here is a measurement**. The
`[slipstream-input]` lines exist so that the owner's PC can put numbers on each stage (kit session `input`).

| Stage | Thread | Triggered by | Can wait for | Queue, bound | Locks |
|---|---|---|---|---|---|
| Mouse report to `WM_INPUT` | Windows, then the interface (Qt) thread | the device's report (125 to 8000 a second) | the interface thread's message loop: whatever that thread is doing (QML, a dialog, a bridge call) | the thread's Raw Input queue (Windows') | none of ours |
| Read: `RemotePlayKeyboardMouse::nativeEventFilter` -> `drainRawInput` (`GetRawInputData`, then `GetRawInputBuffer`: every pending report in one wake, 64 per call, 16 calls at most) | interface | the message (event-driven) | nothing | counts are added up into one pending movement (two integers), no list | none |
| Keys and buttons: `moonlight::InputRouter::message` -> `KeyboardMousePad::key` / `mouseButton` | interface | `WM_KEYDOWN` etc. (event-driven) | nothing | the held-key list (64 keys at most) | none |
| Ordinary mouse-move messages while captured | interface | Windows still produces them (the mouse is registered without `RIDEV_NOLEGACY`, which Qt and the button / wheel handling need) | | swallowed in `InputRouter::message` before Qt sees them: no QML hover work per report | none |
| Translation: `RemotePlayKeyboardMouse::apply` -> `KeyboardMousePad::apply` | interface | **the pad poll**: a Qt precise timer, 4 ms (`ps5Connect` in `src/ui/bridge/RemotePlay.cpp`; `ps5PadPollMs`, 1 to 8 with `VEYRA_EXP_RP_PAD_POLL_MS`) | the timer: 2 ms on average, 4 ms at most, more when the interface thread is busy | none (`legacy`: the smoothing window; new profiles: nothing) | none |
| Physical pad: `ControllerInput::poll` (SDL) | interface | the same poll | | SDL's event queue, drained every poll | SDL's |
| Hand-off: `EngineController::remotePlayController` -> `RemotePlaySessionSource::controller` | interface | each poll | two short locks | `ControllerQueue`: 16 states; stick-only changes replace the newest entry, button edges each keep theirs; an unchanged state is dropped; 100 ms without a poll sends neutral | `EngineController::mutex_` (also taken by the engine thread for its status updates), the session's `mutex_` (also taken by the owner and the decoder hand-off); then `WakeSignal::notify` |
| Take and submit: `RemotePlaySessionSource::run` -> `ChiakiBackend::submitController` -> `chiaki_session_set_controller_state` | session owner | the wake (event-driven), or its 4 ms tick, which only repeats the current state | **the rest of the owner's loop**: one `RemotePlaySource::read` (taking a video sample and handing it to the decoder) when a state arrives while that runs | everything pending is submitted in one wake | the session's `mutex_`, Chiaki's feedback mutex |
| Pack and send: `feedback_sender_thread_func` (chiaki-ng `lib/src/feedbacksender.c`) | Chiaki's feedback sender | a condition variable signalled on a changed state (event-driven) | nothing: no minimum interval (see Rates) | the newest stick state; 64 button-event packets (patch 0003) | its state mutex |
| `sendto` on the stream's UDP socket (`lib/src/takion.c`) | feedback sender | the same call | nothing: no pacing, no acknowledgement | the socket's send buffer | none |

What that means:

- **GPU work cannot hold input.** Enhancement, the neural layer, frame generation and presentation run on the engine
  thread (`EngineController`'s dispatch thread and `EnhanceGraph`); input never runs there. The only thing shared is
  `EngineController::mutex_`, which that thread takes for short status updates (122 lock sites; the ones read copy or
  assign fields; one writes a log line under the lock). A long hold there would show as `stateToSendMs`.
- **Network congestion cannot hold input on the PC.** Receiving is Chiaki's takion thread; sending is the feedback
  sender's own `sendto`, with no queue behind it. A congested network delays or drops the packet after it left.
- **The interface thread is shared.** Raw Input, key messages, the pad poll and the translation all run on the Qt
  interface thread, with QML, dialogs and bridge calls. A busy interface delays the next poll and the reading of
  reports. `[slipstream-input-poll]` shows it (`pollGapMsP95/P99/Max`, `latePolls`), and so do `sendGapMsMax` and
  `stateToSendMs*`. While the stream is in front and nothing is open, that thread is idle between polls.
- **The owner thread is shared with decoding.** A pad state that arrives while the owner hands a video sample to the
  decoder waits for that call. `stateToSendMsP95/P99` is exactly this wait (plus the wake).

Avoidable delays found, and their state:

| Delay | Where | State |
|---|---|---|
| The smoothing window in front of the stick (8 ms built in: 2 ms mean, 4 ms last part at a 4 ms poll; more with a longer window) | `MouseAim::step` (`KeyboardMousePad.h`) | **removed in the new input profiles** (`KbmImmediateAim::step`); `legacy` keeps it; `custom` may switch one on |
| The carry of movement faster than the game can turn (up to 40 or 50 ms of turning after the hand stopped) | `MouseAim::step` | **dropped by default in the new profiles**; a bounded choice in `custom`. The model below shows the price: a flick beyond the game's fastest turn loses distance |
| A window floor of two report intervals for slow mice (16 ms at 125 Hz) | `MouseAim::setWindowFloorMs` | **replaced** in the new profiles by repeating the last value for at most one report interval (8 ms at 125 Hz, none from 250 Hz) |
| One mouse report per message | `nativeEventFilter` | fixed earlier (buffered read); unchanged |
| A tap wholly between two late polls never reaching the console | `KeyboardMousePad::set` | **fixed in the new profiles** (held for one poll); `legacy` unchanged |
| Waiting for the 4 ms pad poll (2 ms mean, 4 ms at most) | the Qt timer in `ps5Connect` | **the poll itself is not changed**: it is the proven schedule, and `VEYRA_EXP_RP_PAD_POLL_MS=2` or `1` remains the experiment for the mouse. **For button presses there is a new experiment, off by default: leading-edge transmission** (`VEYRA_EXP_RP_KBM_EVENT_SEND=1`, or `transmit=leading-edge` in `custom`). The press of a plain button, or of a trigger that has no ramp, is laid over the last poll's state and handed to the sender when its key or mouse-button message arrives (`RemotePlayKeyboardMouse::sendEarly`, `KeyboardMousePad::earlyPress`), at most once between two polls, so never more than two states per poll interval. Sticks, key ramps, releases, the wheel and the mouse wait for the poll as before; a key whose meaning could change by the next poll (aiming layout, on-foot / car switch pending) waits too. It needs one line in the bridge that gives it the sender; without that line it does nothing. Expected (reasoned, not measured): a fire or jump press leaves the PC up to 4 ms, on average about 2 ms, earlier |
| The interface thread doing other work | QML and the bridge on the same thread | **instrumented, not changed**: moving the poll and the translation to their own thread means reading keys through Raw Input as well, a thread-safe hand-off of the physical pad's state and of every setting, and a second owner of the capture state: too large to write blind in this sprint. Recommended only if `[slipstream-input-poll]` shows late polls during play (`pollGapMsP99` well above the poll interval, `latePolls` > 0); then: an input thread with its own message-only window and an absolute-deadline timer |
| A pad state waiting for the owner's decoder hand-off | `RemotePlaySessionSource::run` | **instrumented, not changed**: submitting from another thread needs `ChiakiBackend`'s owner-thread rule lifted (Chiaki's own call is mutex-protected). Recommended only if `stateToSendMsP99` is above about 1 ms |
| Statistics, the self-check and its shadow timing in front of the hand-off (`closeWindow` every 5 s: percentiles, strings, two log lines; two extra transforms every 16th moving poll with `adaptive-precision` / `custom`, which run the self-check without any switch) | `RemotePlayKeyboardMouse::apply` | **fixed** (review 8B): `apply` only copies the mouse counts and queues one call on the interface thread's event loop, which runs after the bridge has handed the state off (`afterHandOff`); the hand-off time recorded for the statistics is taken right after the translation, as before |
| `EngineController::mutex_` on the hand-off (and `notify` under it) | `EngineController::remotePlayController` | **not changed** (shared file). The Moonlight path copies the session pointer under the lock and calls outside it; that one-line form would shorten the hold here too, but the copy can then be the session's last owner, so a session ended at that moment would be destroyed on the interface thread (Chiaki's teardown, thread joins) instead of the engine's: worth doing only together with a rule that the engine always drops the last owner itself, and only if `stateToSendMsP99` shows this lock |

The transmission schedule, as a limit and not a target: the pinned Chiaki code sends one state packet for every
changed stick state it is given, with no minimum interval, and one history packet per button event (Rates below).
At the 4 ms poll that is at most 250 state packets a second while the mouse moves. By default nothing in this sprint
sends more often than the poll. With leading-edge transmission switched on, up to two states per poll interval are
handed to the sender (one early button press, then the poll), so at most 500 a second at the 4 ms poll; the extra one
always carries a button edge, which the patched sender sends as its own history packet anyway. States that change
faster than the sender wakes are merged into the newest one, which is right for a stick.

Leading edge and the on-foot / car switch: if the switch key (Caps) is pressed after an early state went out and
before the next poll, that poll switches profiles and lets go of every key whose meaning changes, the one just sent
included. The press then reaches the console for less than one poll interval; without the early send (exp/rc5) the
console never saw it. This is inherent in sending before the poll and is left so: about 1 % of early sends in the
reviewer's random key script (review 8B), only when both keys go down within one poll interval.

### The `[slipstream-input]` lines

Only with `VEYRA_EXP_RP_STATS=1` and keyboard and mouse on, once per 5 s. They are **software times inside the PC**.
They are not input-to-photon and cannot be: the mouse's own delay, USB, the network, the console, the game, the
stream back and the screen are not in them. The target "less than 1 ms of added mouse processing" can only be
confirmed on the owner's PC with these lines; nothing here confirms it.

| Field | Meaning |
|---|---|
| `source` | `kbm`, `pad`, `mixed` or `none`: which device was in use in the window |
| `inputProfile`, `context`, `curve`, `smoothingMs`, `profile` | what is in use (`profile`: the profile file's name without folder, or `builtin`) |
| `mouseEvents`, `mouseHz` | reports with movement in the window; the measured report-rate class |
| `rawToTranslateUs*` | from reading the first event after a poll to the poll that translated it: the wait for the poll. (Raw Input has no time stamp of its own: a report that waited in Windows' queue before being read is not seen here; `[slipstream-input-poll]` shows a late interface thread instead) |
| `translateUs*` | the cost of one translation (`KeyboardMousePad::apply`) |
| `stateToSendMs*` | from the hand-off of a changed state to the moment the owner gave it to Chiaki: both locks, the wake, the owner's loop |
| `eventToSendMs*` | from the first event in that state to the same moment: the whole way through the PC that we can see |
| `queueAgeMsP95`, `queueDepthMax` | the controller queue when the owner took a state |
| `reports`, `sendHz` | changed states given to Chiaki, and per second |
| `sendGapMs*` | time between two changed states less than 100 ms apart: the packet interval while a hand moves; a late poll shows as a gap |
| `saturatedPct` | share of moving polls in which the hand asked for more than the stick's limit (new profiles; -1 in `legacy`) |
| `switches` | context changes in the window |
| `stuckReleases`, `heldAtLoss` | since the start: releases that had something to let go of; inputs held when the session went away |
| `fallback` | `none` or why the self-check fell back |

`[slipstream-input-poll] polls= pollMs= pollGapMsP50= pollGapMsP95= pollGapMsP99= pollGapMsMax= latePolls=
rawBacklogMax=`: the interface thread's own timing (`latePolls`: gaps above twice the interval; `rawBacklogMax`: the
most reports read in one wake). For the comparison with a DualSense, run the pad arm with keyboard and mouse
switched on and simply use the controller: the lines then say `source=pad` for the same path.

### Input profiles against legacy (Linux model)

`tools/remoteplay_latency_bench/kbm_profile_bench.cpp` (build and run lines in its header; the output is the two
tables below). The legacy column is the Package 7 behaviour. **A model in virtual time; no PC, no console, no game.**
Assumptions: 8000 counts per turn; pad poll 4 ms with a timer up to 0.15 ms early or late; a simulated game with an
8-bit stick, dead zone 0.20, turning speed ~ tilt^1.8 and a full turn in 1.5 s (240 degrees a second at most); every
profile holds a perfect calibration of that game; the owner sends a queued state 50 microseconds after the poll (so
queue age is the same for all: the queue is not what differs); the model player makes minimum-jerk strokes (100 ms
plus 1.2 ms per degree, at most 300 ms), looks 150 ms after each stroke and corrects until the camera is within 0.5
degrees. The player has no tremor and expects equal distance, equal turn; a real player adapts to a curve, this one
does not.

Table 1: one small stroke (2 degrees in 100 ms) and a slow pursuit (+/- 30 degrees a second, 0.5 Hz, 4 s); the game
reads every state. *Delay of the turn*: how much later the middle of the camera's turn is than the middle of the
hand's movement (it contains the half poll and the game's read).

| Mouse | Profile | Delay of the turn (centroid), ms | Against legacy, ms | Turned of 2 degrees | Pursuit error RMS, deg | States sent /s | Queue age max, ms |
|---|---|---|---|---|---|---|---|
| 125 Hz | legacy (8 ms window, carry 50 ms) | 11.2 | +0.0 | 1.93 | 0.40 | 187 | 0.05 |
| 125 Hz | ultra-low-latency | 6.7 | -4.5 | 2.07 | 0.39 | 105 | 0.05 |
| 125 Hz | adaptive-precision, its aiming curve | 6.7 | -4.4 | 1.68 | 2.50 | 105 | 0.05 |
| 125 Hz | custom, carry 50 ms | 6.7 | -4.5 | 2.07 | 0.39 | 105 | 0.05 |
| 125 Hz | custom, 8 ms window | 6.7 | -4.5 | 2.07 | 0.42 | 105 | 0.05 |
| 1000 Hz | legacy (8 ms window, carry 50 ms) | 6.5 | +0.0 | 1.97 | 0.27 | 178 | 0.05 |
| 1000 Hz | ultra-low-latency | 4.7 | -1.8 | 2.00 | 0.24 | 206 | 0.05 |
| 1000 Hz | adaptive-precision, its aiming curve | 4.7 | -1.8 | 1.60 | 2.30 | 206 | 0.05 |
| 1000 Hz | custom, carry 50 ms | 4.7 | -1.8 | 2.00 | 0.24 | 206 | 0.05 |
| 1000 Hz | custom, 8 ms window | 6.3 | -0.2 | 1.99 | 0.30 | 191 | 0.05 |
| 8000 Hz | legacy (8 ms window, carry 50 ms) | 6.2 | +0.0 | 1.99 | 0.20 | 180 | 0.05 |
| 8000 Hz | ultra-low-latency | 4.8 | -1.4 | 1.99 | 0.23 | 205 | 0.05 |
| 8000 Hz | adaptive-precision, its aiming curve | 4.8 | -1.4 | 1.59 | 2.32 | 205 | 0.05 |
| 8000 Hz | custom, carry 50 ms | 4.8 | -1.4 | 1.99 | 0.23 | 205 | 0.05 |
| 8000 Hz | custom, 8 ms window | 6.1 | -0.1 | 2.00 | 0.22 | 192 | 0.05 |

Table 2: the model player turning to a target, 1000 Hz mouse.

| Target | Game reads | Profile | Time to target, ms | Strokes | First stroke off by, deg | Saturated polls |
|---|---|---|---|---|---|---|
| 2 deg | every state | legacy (8 ms window, carry 50 ms) | 93 | 1 | -0.08 | not counted |
| 2 deg | every state | ultra-low-latency | 91 | 1 | -0.00 | 0 % |
| 2 deg | every state | adaptive-precision, its aiming curve | 106 | 1 | -0.40 | 0 % |
| 2 deg | every state | custom, carry 50 ms | 91 | 1 | -0.00 | 0 % |
| 2 deg | every state | custom, 8 ms window | 92 | 1 | -0.06 | 0 % |
| 2 deg | 30 fps | legacy (8 ms window, carry 50 ms) | 336 | 2 | -0.53 | not counted |
| 2 deg | 30 fps | ultra-low-latency | 131 | 1 | -0.47 | 0 % |
| 2 deg | 30 fps | adaptive-precision, its aiming curve | 329 | 2 | -0.77 | 0 % |
| 2 deg | 30 fps | custom, carry 50 ms | 131 | 1 | -0.47 | 0 % |
| 2 deg | 30 fps | custom, 8 ms window | 131 | 1 | -0.47 | 0 % |
| 20 deg | every state | legacy (8 ms window, carry 50 ms) | 134 | 1 | -0.06 | not counted |
| 20 deg | every state | ultra-low-latency | 362 | 2 | -1.68 | 39 % |
| 20 deg | every state | adaptive-precision, its aiming curve | 381 | 2 | -2.10 | 39 % |
| 20 deg | every state | custom, carry 50 ms | 131 | 1 | +0.05 | 57 % |
| 20 deg | every state | custom, 8 ms window | 365 | 2 | -1.82 | 39 % |
| 20 deg | 30 fps | legacy (8 ms window, carry 50 ms) | 358 | 2 | -0.78 | not counted |
| 20 deg | 30 fps | ultra-low-latency | 385 | 2 | -1.27 | 39 % |
| 20 deg | 30 fps | adaptive-precision, its aiming curve | 394 | 2 | -1.89 | 39 % |
| 20 deg | 30 fps | custom, carry 50 ms | 385 | 2 | -1.27 | 57 % |
| 20 deg | 30 fps | custom, 8 ms window | 393 | 2 | -1.83 | 39 % |
| 90 deg | every state | legacy (8 ms window, carry 50 ms) | 744 | 3 | -38.88 | not counted |
| 90 deg | every state | ultra-low-latency | 1039 | 4 | -50.36 | 73 % |
| 90 deg | every state | adaptive-precision, its aiming curve | 1268 | 5 | -50.70 | 73 % |
| 90 deg | every state | custom, carry 50 ms | 730 | 3 | -38.36 | 88 % |
| 90 deg | every state | custom, 8 ms window | 1044 | 4 | -50.36 | 72 % |
| 90 deg | 30 fps | legacy (8 ms window, carry 50 ms) | 796 | 3 | -41.62 | not counted |
| 90 deg | 30 fps | ultra-low-latency | 1060 | 4 | -50.73 | 73 % |
| 90 deg | 30 fps | adaptive-precision, its aiming curve | 1083 | 4 | -50.92 | 73 % |
| 90 deg | 30 fps | custom, carry 50 ms | 793 | 3 | -41.23 | 88 % |
| 90 deg | 30 fps | custom, 8 ms window | 1057 | 4 | -50.33 | 72 % |
| 180 deg | every state | legacy (8 ms window, carry 50 ms) | 1266 | 4 | -108.32 | not counted |
| 180 deg | every state | ultra-low-latency | 1598 | 5 | -120.31 | 76 % |
| 180 deg | every state | adaptive-precision, its aiming curve | 1813 | 6 | -120.71 | 76 % |
| 180 deg | every state | custom, carry 50 ms | 1260 | 4 | -108.24 | 89 % |
| 180 deg | every state | custom, 8 ms window | 1602 | 5 | -120.34 | 75 % |
| 180 deg | 30 fps | legacy (8 ms window, carry 50 ms) | 1511 | 5 | -108.78 | not counted |
| 180 deg | 30 fps | ultra-low-latency | 1845 | 6 | -120.56 | 76 % |
| 180 deg | 30 fps | adaptive-precision, its aiming curve | 1862 | 6 | -120.90 | 76 % |
| 180 deg | 30 fps | custom, carry 50 ms | 1276 | 4 | -107.62 | 89 % |
| 180 deg | 30 fps | custom, 8 ms window | 1830 | 6 | -121.06 | 75 % |

Reading it (all of it model, none of it measured):

- The immediate transform puts the turn 1.4 to 1.8 ms earlier than legacy with a 1000 or 8000 Hz mouse and 4.5 ms
  earlier with a 125 Hz mouse (whose legacy window is 16 ms). With a 1000 or 8000 Hz mouse an 8 ms window in `custom` gives almost all of that back.
- A small correction lands in one stroke with the immediate profiles also when the game reads the pad only once per
  picture (131 ms against 336 ms for legacy in this model, which needs a second stroke there).
- **Dropping costs distance on flicks.** A 20 degree stroke of this player peaks above the game's 240 degrees a
  second; `ultra-low-latency` drops the surplus and needs a second stroke (362 ms against 134 ms), `custom` with a
  50 ms carry does not. For 90 and 180 degrees every profile falls far short on the first stroke: that is the
  console's turning-speed cap, which nothing on the PC removes. Dropping is still the default of the new profiles
  because a carry is turning that continues after the hand stopped; who flicks a lot can choose `custom` with
  `all.saturation=carry;all.carryMs=<10 to 50>` and should raise the in-game stick sensitivity first.
- `adaptive-precision`'s aiming curve turns a slow 2 degree stroke only 1.6 degrees for this player and costs him a
  stroke in some rows: the price of a curve for someone who expects a straight line. On foot it is the straight
  line (the same numbers as `ultra-low-latency`).
- Pursuit error is the same within 0.1 degrees for all straight profiles: the stick's 8-bit steps and the dead
  zone dominate it, not the transform.

## Compared with xcontrol and PadForge

Two MIT-licensed open-source mouse-to-stick programs were read for this sprint (xcontrol at commit `c1921818b709`,
`src/XControl.Core/MouseStick.cs` and `Engine.cs`; PadForge at `fa8a76ebebae`, `src/PadForge.Core/Mapping/StickCurve.cs`
and `Runtime/PadEngine.cs`). Nothing was built or run; the numbers are their source constants. **No code was
ported.** One idea was re-implemented in our own code: an output cap (`maxTilt`), after xcontrol's `MaxOutput`.

| | xcontrol | PadForge | Ours, new profiles | Ours, legacy |
|---|---|---|---|---|
| Model | speed, exponential average, shape | speed, clamp, "spring" low-pass, shape (or position hold) | this poll's speed, shape | speed over a window |
| Smoothing by default | always on: about 8.7 ms time constant, cannot be switched off | always on: 38 ms | none | 8 ms box (2 ms mean at a 4 ms poll) |
| After the hand stops | fades over 56 ms, at or above the dead-zone offset throughout | fades over 108 ms | at rest on the next poll | at rest after the window |
| Dead zone | offset | an inner cut that drops slow aim, then an offset | offset, from the game calibration | offset, from the game calibration |
| Curves | power, speed gain | power | linear, power, two-segment, on the inverse of the game's measured response | the inverse of the game's measured response |
| Faster than full stick | clamped (lost) | clamped (lost) | dropped, or a bounded carry by choice | carried for `carry_ms` |
| Tick | 8 ms | 4 ms | 4 ms poll (1 to 8) | the same |

Why we keep ours: both add a filter before the stick by default, which is the delay this sprint removes, and both
have a behaviour we test against: xcontrol keeps the stick at or above the dead-zone offset while its filter fades
after the hand stopped (ours: `kbm_input_immediate_output_depends_on_this_poll_only`), and PadForge's inner dead zone
drops slow aim (ours: `kbm_input_smallest_motion_leaves_the_dead_zone_at_once`). Neither measures the game; our
calibration does, and the new profiles now use it. Neither solves what no PC program can: the console's turning
cap, the game's own aim acceleration and smoothing, and the stick's 8-bit steps. A note derived from the arithmetic
(not tested): more mouse DPI with a lower sensitivity gives the immediate transform finer speed steps per poll at no
delay. Neither project's claims about feel or latency were checked and none is repeated here.

## Limits of analog emulation (what cannot be fixed from the PC)

The console only ever sees an analog stick. Everything here translates a mouse into stick positions;
neither the calibration nor any setting turns the stick into a mouse.

- **1:1 mouse feel is impossible.** A mouse reports a distance, a stick is read as a turning speed, and the game
  decides what a tilt means. The best any translation can do is make a hand distance turn the same angle at every
  speed the stick can show, as far as the game's response was measured. It will not feel like the PC version of a
  game, with any profile.
- **The console's maximum turning speed.** A stick at full tilt turns at the game's maximum and no faster. A flick
  quicker than that loses distance: the new profiles drop the surplus (`saturatedPct` counts how often), `legacy`
  plays it out over the next 40 to 50 ms (`carry_ms`) and drops the rest, `custom` lets you choose. Only the in-game
  stick sensitivity raises the cap; set it to maximum before calibrating.
- **Stick resolution in time.** The stick is a speed, and the speed of one 4 ms poll is known only as coarsely as
  the reports in it: slow aiming is a few counts per poll. The new profiles send that as it is; the legacy window
  averages it at the price of delay.
- **The game's own aim behaviour**: acceleration over time, smoothing, aim assist slow-down and the exact
  shape of its dead zone. Dead zone and curve here undo a simple model of it, not the game's real code. Nothing here
  uses, counters or imitates aim assist: what the game does with a controller's stick it does with this one.
- **The game's picture rate.** A 30 fps game looks at the pad every 33 ms at best. No setting on the PC
  makes it react sooner.
- **The delay of Remote Play itself**: console capture and encoding, network, decoding, display. See
  `docs/PS5_REMOTE_PLAY_EXPERIMENTS.md`.
- **Stick resolution.** A game reads 8 bits per axis; movement between two steps is carried and adds up,
  but the slowest steady turn is one step above the dead zone.
- **What the PC cannot know**: whether you are on foot, in a vehicle or in a menu. Walking / driving is
  your key press; menus are used with the D-pad keys and Cross / Circle, there is no mouse pointer.
- **Touchpad swipes, the microphone, text entry** are not mapped.
- **What a calibration can measure**: a radial dead zone, a response along one axis at three points, the
  fastest turn and one vertical / horizontal ratio, through your eyes and taps. A dead zone shaped like
  a square or a cross, a response that differs by direction, turn acceleration over time, aim slow-down
  near targets and a vertical response shaped differently from the horizontal one are not measured.
  The slowest aim stays the least accurate (see the model above).
- **In a vehicle** only the camera is calibrated, and only if you agree; steering and triggers keep
  their hand-set numbers.
- **What the test kit can and cannot measure.** It can read the `[slipstream-input]` software times (processing,
  the wait for the poll, the wait for the sender, packet intervals, late polls) and compare mouse arms with a
  DualSense arm on the same path, and it can time a press to the picture's answer with the latency test. It cannot
  measure the mouse's own delay or USB, cannot see what the console and the game do with a stick value, and cannot
  judge feel: turning evenness, overshoot and comfort are the player's verdict on the real console.
- **GTA VI and the publisher.** The publisher has made no statement on mouse and keyboard for GTA VI on console
  (press of 18 November 2026 reports none confirmed), and its general policy is that hardware not licensed by the
  console maker is unsupported. This feature presents itself to the console as a controller; nothing here is
  supported or endorsed by the publisher or by Sony, and whether you may use it in a given game or mode is your
  responsibility.

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
- *States sent*: packets a second with the profile used before the default became 8 ms (33 ms window, 8-bit steps).
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

### The built-in smoothing window: 8 ms, with 33 ms as the opt-in smoother camera (Linux model)

Input delay comes first, so the built-in window is 8 ms: at a 4 ms poll it adds 2 ms on average and
4 ms at most if the game reads every pad state, and the stick is back in the centre within two polls
plus 8 ms after the mouse stops (unit-tested, `kbm_profile_builtin_default_adds_little_delay`). Until
19 November 2026 it was 33 ms (14 ms on average, 28 ms at most, the stick centred about 32 ms after the
mouse stopped). 33 ms is the opt-in "smoother camera" value for a game that reads one state per
picture at 30 fps: `VEYRA_EXP_RP_KBM_SMOOTH_MS=33` or `smooth_ms=33` in the profile file. The table
below is why it can be worth it there.

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
  (`tests/remoteplay/KbmProfileTests.inc`), also under ThreadSanitizer and AddressSanitizer + UBSan.
- Linux, GCC 13: the calibration (keys, steps, missed and disagreeing taps, the numbers derived, a whole
  run against simulated games with +/- 80 ms tap error), the response table, the report-rate meter and
  the window floor, the profile block and its backup, and profile switches in mid-movement (no camera
  jump larger than one poll of movement, keys that keep their meaning stay held) are unit-tested
  (`tests/remoteplay/KbmCalibrationTests.inc`, 20 cases).
- The Windows-only capture (`include/veyra/ui/RemotePlayKeyboardMouse.h`), with the calibration keys,
  the report-rate meter and the profile writing, and the bridge compiled with MSVC on GitHub's Windows
  runner, where the tests also pass. Later changes to that file (the `DefWindowProcW` call for
  `WM_INPUT`, the first-backup and size-limit rules) passed a mingw-w64 syntax check only until the next
  Windows CI run.
- Sprint 5 (20 November 2026), Linux, GCC 13: the input performance profiles, the immediate transform with the
  game calibration and the tilt cap, the profile text (old files included), the GTA VI layout, the preference fit,
  the statistics and their lock-free probe (also under ThreadSanitizer), the self-check and its switch, and the
  matrix of interruptions (5760 random sequences over four profiles, six mouse rates from 125 Hz to 8 kHz and six
  kinds of interruption; rapid, fine and reversing movement; controller and mouse on one stick; a starved input
  thread; connection interruption through the controller queue; an 8-hour session in model time) are unit-tested
  (`tests/remoteplay/KbmInputTests.inc`, `KbmInputStatsTests.inc`, `KbmInputMatrixTests.inc`). The Windows side of
  it (`RemotePlayKeyboardMouse.h`: switches, measurements, the page's methods; the owner's tap in
  `RemotePlaySessionSource::run`) passed a mingw-w64 syntax check and nothing more: **unverified on Windows**.
- Not done: any use with a real mouse and keyboard against a PS5, any game, any calibration by a person.
- The stable candidate (the build without these review fixes) still has the 33 ms smoothing default. To
  use 8 ms there, add `VEYRA_EXP_RP_KBM_SMOOTH_MS=8` to `slipstream.ini` beside `PushFPS.exe` (or set
  the environment variable). The tables under Rates and Calibrating a game are a Linux model.
