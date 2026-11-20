# First hardware session: what to run, in what order, what to send back

One procedure for the first session on the target PC.
Stopping after any step still gives a usable result. Times are estimates. The kit has run on GitHub's
Windows runner only as a dry run (Windows PowerShell 5.1, made-up log lines, no PushFPS, no PS5); it has
never started PushFPS or measured anything on a real PC. If a step fails, send the text of the
PowerShell window.

The kit installs nothing, loads no driver and changes no Windows, driver, router or PS5 setting.

| Step | What | Your time | Hands-on |
|---|---|---|---|
| 0 | Preparation | 15 min | yes |
| 1 | Check and main comparison (3 arms) | 72 min | yes |
| 2 | Camera: button to screen | 15 min filming + 30 min counting, any time later | yes |
| 3 | Cable and network recovery | 8 min | yes |
| 4 | Long run (soak) | 2 h | no: start it and leave |
| 5 | Send back | 5 min | yes |

Optional afterwards: the breakdown of the three timing changes and the experiments (last section).

## First: the short presentation session (this sprint's main question)

`Run-First-Test.cmd -Session presentation` (or `run-tests.ps1 -Session presentation`): **about 32 minutes**, no clicks.
Experimental build only; arms `p-window`, `p-fullscreen`, `p-lowqueue-off`, `p-test-uncovered` from `arms.csv` (each with
`VEYRA_EXP_PRESENT_WINDOW_FACTS=1`), 2 rounds, interleaved, each start automatic (connect, settle, one PS-button
latency test of 12 presses, close): 8 starts of about 4 minutes (2 minutes to start and close, about 6.3 s per press
plus 10 s per test). PresentMon is used only when it can run without an administrator prompt (your account in
"Performance Log Users"); otherwise the session prints "PresentMon not used: this session then cannot tell whether
Windows composes the video window; the app's own lines are still collected". RESULT.txt section SHORT SESSION gives,
per arm, measured values only (24 presses per arm are too few for a verdict word): present to screen P50 / P95
(`presentToScanoutP50/P95` of `[slipstream-latency]`), the share of frames on screen within 1 refresh, 1 to 2 and over
2 refreshes (`scanoutUnder1Refresh`, `scanout1To2Refresh`, `scanoutOver2Refresh`), and the window facts of
`[slipstream-present] window ...` (`regionKind`, `coversMonitor`, `ownOverlaps`, `flipBlockers`, each as its most common
value and share of lines); report.md adds PresentMon's present mode share when PresentMon ran.

Right after it, the audio pair: `Run-First-Test.cmd -Session audio`, **about 16 minutes**, no clicks: `s-base` against
`s-audio-smooth` (`VEYRA_EXP_RP_AUDIO_SMOOTH=1`), 2 rounds of the same automatic start; RESULT.txt gives how much
`clicks=` of `[remoteplay-audio]` rose per arm, per minute (measured values only).

## In short (test package 7, kit version 4)

What you do: **install** (`Install-Slipstream.cmd`), **pair with the PS5 once** in any copy (the pairing is kept
by Windows for your account and shared by every copy), **start the game scene**, **double-click one file**
(`Run-First-Test.cmd`). What is still manual, per build:

| Build | Manual steps left | Why |
|---|---|---|
| original, stable candidate | first start of each copy: connect by hand (later starts reconnect by themselves); per start: the PS5 latency test, 12 presses, as many tests as the kit says; frame generation off once in the kit's profile | these builds have no test automation and accept at most 12 presses per test |
| experimental (`-Session switches`, `quality`, `dedup-probe`) | none; if the automation cannot run (no console, a failed step, a build without it) the kit says so and shows the manual steps | started with `VEYRA_EXP_RP_STATS=1`, `VEYRA_TEST_RP_AUTOCONNECT=1` and `VEYRA_TEST_RP_AUTORUN=settle:8;latency:<n>;...;exit` |

**Same settings in every copy (the test profile).** The first hardware run compared an original copy with the
low-latency queue off against a stable copy with it on (`[pacing] requested=false/0/1` against
`requested=true/0/1`): each copy keeps its own settings beside its exe. The kit now starts every copy, the
original included (its `--data-dir` option exists in the original build too), with its own settings folder
`%LOCALAPPDATA%\slipstream-kit\profile-<build>` holding `qml-preferences.v1.json` with: low-latency queue ON and
NVIDIA low latency ON (the driver's low-latency mode with its per-frame sleep calls; on an NVIDIA GPU the queue follows
that choice, in the original build too), display sync V-Sync, output cap following the display with a variable refresh
request (smooth pacing), full screen on connect, reconnect to the last console at start, no intro. **This differs
from your everyday settings of the original copy**: in your first run the original had the low-latency queue off
(`[pacing] requested=false`). The test compares the builds' code with the same settings, so the original's numbers here
are not the numbers of the original as you normally use it. Your own
settings are not read or changed (an explicit data folder is never filled from them), and the console
pairing (`%LOCALAPPDATA%\Veyra\remoteplay`) is shared. Frame generation, super resolution and enhancement
keep that folder's defaults. The experimental build logs them ([slipstream-config]); the original and the stable build do not, so a comparison that needs them is reported as not comparable (NOT CHECKED). `--pref` is not used: it writes into the
saved settings and the presentation settings are read only at start. `-NoTestProfile` uses your own settings.

**Comparable or refused.** From each run's log the kit and `report.py` read every observable setting into the
same keys: the `[slipstream-config]` line when the build writes it (all keys), else what the existing lines
show: `lowQueue`, `pacingMode`, `displaySync`, `vrrRequested`, `displayQueuePaced` (`[pacing] revision=`),
`presentWaitable`, `presentMaxLatency`, `presentVsync`, `presentTearing` (`[pacing] DXGI`), `vrrActive`,
`vrrLevel`, `refreshHz` (`[display-vrr]`, `[display-refresh]`), `stream`, `codec`, `bitrateKbps` (session line),
`decoder` (`[remoteplay-decode]`), `fg` (`[frame-flow]`, `[fg-validity]`). Not visible in the original or
stable build: mode, fgMethod, fgMultiplier, fgTier, sr, srQuality, enhance, nvLowLatency, smoothPacing,
fullscreen, window, output, hdr, trialsMax. Two arms are compared only when every key both logged is equal,
except the keys the switch under test is expected to change (`arms.csv` column `differs`); otherwise RESULT.txt,
result.json and report.py say REFUSED, name each differing key with both values and give a one-line fix.
Re-scored on the first run's evidence: original against mod-legacy is refused (`lowQueue 0 in original, 1 in
mod-legacy`), mod-legacy against mod-all is accepted.

**Wi-Fi.** Allowed, never blocking. When the default route is Wi-Fi (`[slipstream-config] route=wifi`, the
health line's `link=wifi`, or the hardware report) the first line of RESULT.txt, every table and every verdict
say "Wi-Fi: diagnostic only, not a wired-LAN latency conclusion"; result.json has `network.route`; baseline.py
never compares a Wi-Fi baseline with a wired one.

**Presses.** The original and the stable build accept at most 12 presses per latency test. The kit asks for
exactly what each build accepts (12, or `trialsMax` of `[slipstream-config]` from a build's second start on)
and plans the tests per start from `-PressesPerArm` and `-Rounds` (default 120 and 3: four tests of 12 per
start); the plan is printed before the first run.

**Tables of RESULT.txt** (software counters only; camera and light sensor never enter them), each per arm with
P50 / P95 / P99, bootstrap 95 % intervals (1000 resamples, Park-Miller generator, seed 807, identical digits in
PowerShell and Python: `ci\bootstrap-golden.json`) and the minimum detectable difference for the sample:
PC pipeline (arrival to scanout, `[slipstream-press]`), console + network (send to arrival), total (PS-button
style, every build), frame delivery (seconds under 57 presents, replaced %, arrival-gap P95, clumps), audio
(pops, underruns; counts only), super resolution cost and frame generation (`-Session quality`; "not available
in this build" when the hook reports `reason=unsupported`), repeat pictures (`-Session dedup-probe`, diagnostic).
Verdict per table: at least 120 values per arm and non-overlapping P50 intervals (frame delivery: 300 seconds
and non-overlapping Wilson intervals). report.py computes the same tables and names any disagreement.

**PresentMon** (optional; MIT licence, Intel Corporation; the kit does not download or ship it): put
`PresentMon-<version>-x64.exe` from the GitHub page "Releases - GameTechDev/PresentMon"
(https://github.com/GameTechDev/PresentMon/releases) next to `Run-First-Test.cmd`. The kit uses it only with a
valid Intel Corporation signature, reads the option names from its own `--help`, captures only PushFPS.exe,
and starts only PresentMon elevated (one Windows prompt per run, and only when you start the kit with
`-PresentMonPrompt`; otherwise it is skipped with one line), or without a prompt when your account is in the Windows
group "Performance Log Users" (the kit never changes groups). `presentmon.py` reads the CSV by its
header names (2.x and 1.x) and report.md shows per arm: independent flip against composed share, tearing,
dropped, frame time P50/P95/P99, displayed interval P95/P99, present to display P50/P95. Its licence is at the
end of this document.

## Presentation and super resolution arms (from docs/PRESENTATION_LATENCY.md section 6 and docs/SUPER_RESOLUTION.md)

`-Session switches` runs one arm per experiment now in the app, each a single switch against `s-base` (statistics
only), all OFF elsewhere: `s-audio-fast` (`VEYRA_EXP_RP_AUDIO_FAST_RECOVER`), `s-audio-smooth` (`_AUDIO_SMOOTH`),
`s-audio-adaptive` (`_AUDIO_ADAPTIVE`), `s-delivery-catchup` (`VEYRA_EXP_RP_DELIVERY=catchup`; needs 120 Hz or more;
accept and reject rules in docs/FRAME_DELIVERY.md section 5), `s-early-loss`, `s-wrap-fix`, `s-expiry-fix`,
`s-idr-retry` (`VEYRA_EXP_RP_EARLY_LOSS`, `_REORDER_WRAP_FIX`, `_EXPIRY_FIX`, `_IDR_RETRY_MS=500`: only meaningful in a
build that contains the network work). `VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1` is used only by the reliability
session. `-Session switches` also runs six presentation arms of the experimental build (four of them are the short
presentation session above): `p-window` (windowed with the interface
visible, as the owner ran it; `pref:pushAutoFullscreen=false` in its test profile), `p-fullscreen` (the test profile),
`p-vrr` (`VEYRA_EXP_PRESENT_VRR_REQUEST=1`), `p-tearing` (`VEYRA_EXP_PRESENT_TEARING=1`), `p-lowqueue-off`
(`VEYRA_EXP_PRESENT_LOW_QUEUE_OFF=1`, windowed: presents as the original build did in the first test) and
`p-test-uncovered` (`VEYRA_EXP_PRESENT_TEST_UNCOVERED=1`, windowed: the latency test window is kept behind the picture).
Every presentation arm sets `VEYRA_EXP_PRESENT_WINDOW_FACTS=1`. The tearing arm can show
tearing lines in the picture when variable refresh is not really active for the window: that is expected for that arm.
Torn frames get no scan-out time from the app, so `p-tearing` is scored only with PresentMon (start with
`-PresentMonPrompt`); without it the arm says "not scored". The `VEYRA_EXP_PRESENT_*` and `VEYRA_EXP_SR_*` switches are
put into the environment of the started app (slipstream.ini cannot set them). report.md's table "Presentation: composed or
independent flip" puts `[slipstream-swapchain]` (flip, full screen, window covers monitor), the app's present-to-scanout
P50 and PresentMon's `MsUntilDisplayed` P50 side by side and says whether they agree within one refresh. `-Session
quality` reports each super resolution hold with its measured GPU time and in-app latency only, no recommendation.

## Network session (SmartLink, phase 2 of sprint 4)

`Run-First-Test.cmd -Session network` (or `run-tests.ps1 -Session network`; also part of `-Full` when the experimental
build is installed). **Play normally during the whole session: the stream must show real motion. A still menu gives a
result that only speaks for a still menu.** Works on whatever link the PC has; on Wi-Fi every line says "Wi-Fi:
diagnostic only, not a wired-LAN latency conclusion".

Four arms of the experimental build, all automatic (connect, `settle:8`, `wait:<minutes>`, close), the same video
settings: `n-off` (SmartLink switched off: current defaults with `VEYRA_EXP_RP_STATS=1` only, so the cost of observe mode
itself is measured), `n-observe` (`VEYRA_EXP_RP_SMARTLINK=1` + `_TRACE=1`: computes and logs, changes nothing; the
reference and the recording), `n-apply` (`=apply`), `n-audio` (apply + `VEYRA_EXP_RP_AUDIO_FAST_RECOVER=1`,
`_AUDIO_SMOOTH=1`, `_AUDIO_ADAPTIVE=1`, never `_AUDIO_LOW_BUFFER`). **Every arm runs twice** (`-Rounds`, default 2) for
`-Minutes` each (default 3), interleaved: the order starts at a random arm and is reversed in the second round
(A B C D D C B A), so slow drift of the network cancels. **Default length: 8 runs of 3 minutes plus about 2 minutes to
start and close each, about 40 minutes.** `-NetworkWithBuilds` adds the original and the stable build (two runs each,
about 20 minutes more): `Run-First-Test.cmd -Session network -NetworkWithBuilds`.

`-WifiStreamingMode` (only when you ask for it; never by default or in `-Full`) adds one more arm with
`VEYRA_EXP_RP_WIFI_STREAMING_MODE=1`: PushFPS asks the Wi-Fi adapter not to scan for other networks in the background
while the stream runs, and the request ends with the stream; other streaming programs use the same request. It changes
how the adapter behaves and has not been tested here, so choose it knowingly; if the adapter says no, the result says
"the adapter did not accept it", which is not an error.

Collected per run: the log lines, and every SmartLink trace file written during that run, copied whole from beside
the app log (`smartlink-trace.csv`, and `smartlink-trace.1.csv` when the stream reconnected inside the run: the app
starts a new file at every stream start and renames the older ones). A trace that is not exactly the app's trace (its
fixed header line, then plain decimal numbers in ASCII digits, no line longer than 618 bytes) is withheld from the zip
and named in `withheld.txt`.

Scored per arm (RESULT.txt section NETWORK SESSION, and report.py in more detail): round trip min / median / P95 / max
(SmartLink line, else the health line's `consoleRttMs`; "not known in this session" when the session's `consoleUnits`
line says the console's unit could not be decided, and the same for the console's rates);
the console's target rate, the measured and the requested rate; frame gap P95 and max; stutter events per minute (gaps
over 25 ms, about 1.5 frame intervals, from `[slipstream-delivery]`); frames replaced; audio pops, underruns and
concealed per minute; in-app latency P50 / P95 / P99 (`[slipstream-latency]`); SmartLink profile and regime time
shares; reorder-wait changes, time at each value and the added wait (time at each value times its size: the latency
cost). A plain "Connection" paragraph gives the detected connection and advice.

Verdict rule. The unit is the **run** (the mean of its 5 s windows; windows inside one run are strongly correlated, runs
are not). One primary metric per question gets a verdict word: **stability = stutter events per minute**, **cost =
in-app latency P95**; the reorder added wait is shown beside it as the latency-cost indicator. The difference of the
run means against `n-observe` gets a Welch interval at 97.5 % per metric (Bonferroni over the two primary metrics, so
the chance of a verdict word for an arm that really is the same stays at or below 5 %; a simulation of 200 sessions of
identical arms on drifting Wi-Fi gave one in 0.5 %). "better" or "WORSE" only when that interval excludes zero, the
configurations are comparable (`smartlink` allowed to differ) and both arms saw similar motion; with fewer than 2 runs
per arm: "indication only: needs more runs". When the interval contains zero, its half-width is compared with the
smallest effect worth finding (one table, the same numbers in `run-tests.ps1` `$networkEffect` and `report.py`
`NETWORK_EFFECT`):

| Verdict metric | Smallest effect worth finding | Why |
|---|---|---|
| stutter events per minute | 30 % of the reference arm's mean, at least 0.5 per minute | a change a player notices; below 0.5 per minute one event more or less in a 3-minute run decides |
| in-app latency P95 | 1 ms | about one sixth of a frame at 180 Hz; smaller changes are below what the session's clock and the PS-button test resolve |

The per-press tables of RESULT.txt use the same rule on their P50 (`$latencyEffect`, `LATENCY_EFFECT`): when the
two 95 % intervals overlap and the MDD is above the effect, the verdict is "cannot tell with N presses per arm: this
session would need about M presses per arm" (M grows with the square of MDD / effect, in whole tests of 12, at most 2000).

| Press table (P50) | Smallest effect worth finding | Why |
|---|---|---|
| PC pipeline (arrival to scanout) | 1 ms | as the network session's in-app P95 |
| console + network (send to arrival) | 2 ms | twice the PC side: the console's own steps are coarser |
| total (PS-button style) | 8.3 ms | half a console frame at 60 FPS: the test's answers come in 16.7 ms steps |

Narrower than the effect: "no difference shown". Wider: "cannot tell with N runs per arm: this session would need about M
runs per arm to show a difference of <effect>" (M from the observed run-to-run spread, for a 97.5 % interval with 80 %
power, at most 20; "more than 20" when the spread is larger still). "Cannot tell" is never evidence that the arms are
the same. **The default session (2 runs per arm, about 40 minutes) only screens for large effects**: with 2 runs per arm
the Welch interval finds an effect of 3 run-to-run standard deviations in under 5 % of sessions; with 6 runs per arm in
over 95 % (test `Power` in `test_smartlink_session.py`). A real verdict on one question needs about 6 runs per arm with
2 arms: `Run-First-Test.cmd -Session network -Arms n-observe,n-apply -Rounds 6` (12 runs of 3 minutes plus about 2
minutes each, about 60 minutes). Every other metric (P50, P99, frame gaps, replaced frames, audio counts,
audio delay) is a descriptive number without a verdict word. Motion: the median video rate against the console's own
target rate (at least 40 %; `targetKbps` of the SmartLink line, else `consoleTargetKbps` of the health line) or, without one, against the requested
bitrate (at least 20 %); the two arms at most 25 points apart. When that fails, RESULT.txt says: "The picture hardly
moved in this test: play something with movement and run it again", and still prints the numbers. An in-app latency
metric that gets worse beyond its interval is listed first. The original and stable builds are compared only on what
every build logs (seconds under 57 presents, replaced frames, present to scanout); the rest is listed as not compared,
with the reason.

## 0. Preparation (15 min)

1. Both copies installed and paired: `SETUP.md` sections 2 and 3. Same stream quality, codec and bitrate
   in both, frame generation **off**. The kit ignores `slipstream.ini` and gives every run its own
   switches, so the file can stay.
2. Download PresentMon from its GitHub releases page (GameTechDev/PresentMon) and put the console
   program (`PresentMon-<version>-x64.exe`) into `scripts\remoteplay\testkit\` (in the test package:
   `testkit\scripts\remoteplay\testkit\`, or simply next to `Run-First-Test.cmd`). Without it the kit
   still runs; frame times, dropped frames and the present mode are then missing.
3. Screen at its highest refresh rate, PC and PS5 on cable (Wi-Fi runs too, labelled diagnostic only), nothing else heavy running (browser video, downloads, overlays).
4. GTA 6 (or the game you play) loaded and standing in a **calm place you can return to**: every run
   must show the same scene. In-game settings untouched for the whole session.
5. Open PowerShell **normally** (not as administrator) and go to the kit:
   `cd <repository>\scripts\remoteplay\testkit` (in the test package, `<repository>` is the `testkit`
   folder of the package: every command below works from there unchanged). The kit refuses to run as
   administrator so that PushFPS, a network program, never runs elevated. PresentMon alone needs
   administrator rights: the kit starts it after checking its Intel signature, and Windows asks once per
   run.

## 1. Check and main comparison (72 min)

```powershell
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -Quick
```

From the test package the same thing is one double-click: **`Run-First-Test.cmd`** (a normal double-click).
Before anything starts it checks, each with a one-line fix: both copies installed (and their versions),
each copy has streamed from a PS5 before (from its own log), no PushFPS running (and which copy it is), not
running as administrator, PresentMon there or not, the network route a cable, the display at the highest
rate Windows reports, room for the evidence. When an unfinished session is found it offers to continue it
(the kit's `-Resume`: finished runs are kept, only the missing ones run). `Run-First-Test.cmd -Build
experimental` compares the experimental build with the original instead.

You type one thing: a few words about the place in the game (kept in every record, letters and digits
only). While a run goes on, the kit reads the program's log every second and prints what it finds once:
stream (resolution, frame rate, codec, bitrate), refresh rate, variable refresh, frame generation, and each
finished latency test with its median. It does not query processor or GPU counters while a latency test is
timing. After the last planned test (the kit prints how many) it closes PushFPS itself. If frame generation is on, or the stream settings,
refresh rate or variable refresh differ from the session's first run, it says so at once and, after the
run, lists every difference and offers to set the run aside and repeat it (Enter) or keep it (`k`; the
arm is then refused in the result).

The kit first prints the hardware it found. **Stop and fix** if it warns about a refresh rate below the
screen's highest refresh rate; then start again. Wi-Fi is allowed: the results are then labelled "Wi-Fi: diagnostic
only, not a wired-LAN latency conclusion".

It then starts PushFPS nine times: three arms, three rounds, in the order A B C, C B A, A B C.

| Arm | What it is |
|---|---|
| `original` | the original build |
| `mod-legacy` | the modified build with the original timing switched back on (the baseline) |
| `mod-all` | the modified build as installed |

Each time, do exactly this (about 8 minutes):

1. Connect to the PS5 and wait in the game, at your calm place. Put the controller down.
2. PS5 streaming dialog > latency test > set **12** presses (the most the original and the stable build accept) >
   start, as many times as the kit printed. The test opens and closes the PS5 control centre by itself.
3. Nothing else: the kit closes PushFPS after the last test. In the PowerShell window press Enter for the
   next run (`q` stops and still makes the zip; Run-First-Test can continue the session later).

After three rounds every arm has 120 presses; the kit offers one more round for any arm below that.
At the end it prints `RESULT.txt` (also in the zip) and the path of the zip. **This zip alone is already
worth sending.**

### The full session and stable against experimental

- **`Run-Full-Test.cmd`** (about 105 minutes): the same latency arms, then a 10-minute stability run and the
  reliability checks (section 3 of `RELIABILITY.md`), with a break before each part; the estimated time is
  printed first. Same as `run-tests.ps1 -Quick -Full`.
- **`Run-First-Test.cmd -Session versus`**: the stable candidate against the experimental build, one arm per
  named switch set (`arms.csv`, set `versus`: `stable`, `x-base`, then `x-lan`, `x-reorder`, `x-health`,
  `x-backlog`, `x-kbm`, `x-audio`, `x-governor`, `x-gpubudget`, each against `x-base`). Duplicate detection
  is in no arm. Same as `run-tests.ps1 -Versus`. Ten arms, three rounds: about 240 minutes (four hours); `-Arms` runs fewer.
  Between the builds the kit compares the per-press latency of the same 12-press test plan (started by the automation
  or from the menu, it is the same in-app test), with every setting both builds log checked equal; super resolution and
  enhancement off are taken from the test profile when one build does not log them, and the verdict says "assumed from
  the test profile" (the kit cannot set them: they are nodes of the effect chain, saved in `chain-session.v1` in a
  versioned format that differs between the builds and that a build refuses to overwrite once it cannot read it, so a
  written file could break a copy; the experimental build logs `sr=` and `enhance=` itself); full screen is checked from the stable build's own interface line; frame generation details do not
  count while it is off on both sides. RESULT.txt has a section "BETWEEN BUILDS" that says this in plain words.

### RESULT.txt

One page, written by the kit itself (no Python needed): per arm the presses, median, P95 and P99 of the
built-in test with 95 % intervals (order statistics), starts, and the verdict; the verdict rule (a
difference is called only with 120 presses per arm, the same settings, and medians whose intervals do not
overlap); refused arms with every reason; what each arm ran with; decoded and presented frame rates,
decode time, queue depth, network jitter, loss, reordering and bottleneck when logged, processor and GPU
load (sampled between tests), error and warning counts with the first lines; the camera and light sensor
row counts (those are analysed only by `report.py`, in their own tables); the missing app log fields; and
what to send. Everything in it is **software** latency, not input to photon.

## 2. Camera: button to screen (15 min filming)

The built-in test cannot see the controller, the display or the panel. A phone at 240 fps can
(4.2 ms per frame). This is the only measurement of the whole chain.

Phone in slow-motion mode on a stand; the controller's PS button and the screen in one picture; lights on.

```powershell
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -Camera
```

The kit starts `original`, then `mod-all`. In each: connect, start filming, press the PS button, wait
until the control centre is fully open, press again to close, wait two seconds; **30 times**; stop
filming, close PushFPS. Then film two arms the kit is not involved in (camera only, nothing else is
recorded for them):

- `official-app`: Sony's own PS Remote Play program for Windows, same PC, same screen, 30 presses.
- `direct-ps5`: the PS5's HDMI cable plugged straight into the same screen, 30 presses. This is the
  floor: no streaming at all.

Counting (later, without the PS5): play each clip frame by frame. For every press count the frames from
the first frame with the button fully down to the first frame in which the screen has changed. Enter
them in `camera.csv` in the evidence folder of this step, one row per press:

```
arm,press,frames,camera_fps,note
original,1,31,240,
mod-all,1,29,240,
```

Count every press, discard none. A light sensor (photodiode or latency tester) instead of a camera: one row
per press in `sensor.csv` (`arm,press,ms,device,note`; the template is in the evidence folder). Then pack the
folder again:
`powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -PackOnly <evidence folder>`

## 3. Cable and network recovery (8 min)

```powershell
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -SkipLatency -Impairment
```

No extra software. Connect and stand in the game, then follow the green lines in the PowerShell window.
Before each action press **Enter in the PowerShell window** (that writes the time marker), then act:

1. Pull the network cable out of the PC for about **1 second**, plug it back. Wait until the picture is normal.
2. Pull it for about **5 seconds**. Wait until the picture is normal.
3. Start heavy traffic on a **second device** on the same network (a 4K video or a large download) for a minute.
4. Stop the traffic, press Enter, close PushFPS.

Recorded: the markers, the longest time one picture stayed on screen after each, how long the decoded
frame rate took to return, keyframe requests, reconnects, lost packets. Write down what you **saw**
(freeze, blocks, sound drop, had to reconnect by hand) for each of the three.

## 4. Long run (2 h, unattended)

```powershell
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -SkipLatency -Soak -SoakMinutes 120
```

Connect, stand in the game (or play), leave it. The kit closes PushFPS after two hours and makes the zip.
Recorded once a second: PushFPS memory, processor, GPU and video decoder load, video memory; from the log:
reconnects, keyframe requests, errors, decoded frame rate; from PresentMon: the first ten minutes of frames.

## 5. What to send back

1. Every `evidence-*.zip` the kit made (in `Documents\slipstream-evidence\`; the kit prints the path).
2. Four lines of text: the game and the place you stood in; stream quality, codec and bitrate; what you
   saw in step 3; anything that looked or felt wrong in any run (stutter, tearing, a crash, a run you
   had to repeat).

**What is in a zip:** the log lines of the families `[push-latency]` and `[remoteplay...]` (not
`[remoteplay-discovery]`) with IP addresses cut to their private-range prefix, MAC addresses, long hex
values, GUIDs, e-mail addresses, folder paths (also `\\server\share` and `C:/...`), host, server, computer,
device and network names masked, and your computer and user name replaced by `[removed]`; `RESULT.txt`; the PresentMon captures; the once-a-second samples; the hardware report
without the computer name and connection names; `camera.csv`. **Never** in it: the settings folder
(`runtime_local`), the full log, console registration keys, PSN account ids, tokens, Wi-Fi names, your
folder paths or user name. The exact rules are in the header of `run-tests.ps1`; the kit checks its own
files before packing and leaves out (and names) any file that fails. You can open the zip and look.

## Optional, after the first results

- Breakdown of the three timing changes, all six standard arms (about 144 min): `.\run-tests.ps1`.
- Experiments, only when asked: `.\run-tests.ps1 -Arms mod-all,exp-poll2` and so on; the arm names and
  notes are in `arms.csv`. For `exp-fg` and `exp-fg-adaptive-phase` set frame generation to 3X in both.

## How the results are read

`python tools\remoteplay_latency_bench\report.py <zip>` writes `report.html` and `report.md`: per arm the
median, P95 and P99 of the built-in test, the mean difference to the baseline with its 95 % interval,
displayed frame times, dropped frames, present mode, GPU, CPU and memory. A difference is called real
only when the interval excludes zero and both arms have at least 120 presses. The camera and light sensor
tables are the only input to photon; they are never put into a table or a verdict with the software clock.
The section "One-page result" applies the rule of `RESULT.txt` again and says whether it agrees with the
kit's own (`DISAGREE` otherwise). Frames PushFPS generates are counted apart from pictures the console sent.
A comparison is refused when stream settings, present mode, variable refresh, refresh rate or frame
generation differ, or when an arm's frame generation is not what `arms.csv` asks for.

## Missing app log fields

Numbers the kit would use but no log line gives today (the line that should exist is in `RESULT.txt` and
in the report): the performance mode in effect, the frame generation setting when it is off, per-frame
decode time without the health switch, the swap chain's queue depth, a count of every error and warning
line, refresh and variable refresh state from GPUs other than NVIDIA, and the stream settings of the
original build.

## PresentMon licence

The kit can use PresentMon if you download it yourself; no PresentMon file is in the repository or the test
package, so this notice is the only part of it here.

```
Copyright (C) 2017-2024 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
OR OTHER DEALINGS IN THE SOFTWARE.
```
