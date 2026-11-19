# First hardware session: what to run, in what order, what to send back

One procedure for the first session on the real PC (RTX 5070, 180 Hz screen, PS5 on wired Ethernet).
Stopping after any step still gives a usable result. Times are estimates. The kit has run on GitHub's
Windows runner only as a dry run (Windows PowerShell 5.1, made-up log lines, no PushFPS, no PS5); it has
never started PushFPS or measured anything on a real PC. If a step fails, send the text of the
PowerShell window.

The kit installs nothing, loads no driver and changes no Windows, driver, router or PS5 setting.

| Step | What | Your time | Hands-on |
|---|---|---|---|
| 0 | Preparation | 15 min | yes |
| 1 | Check and main comparison (3 arms) | 40 min | yes |
| 2 | Camera: button to screen | 15 min filming + 30 min counting, any time later | yes |
| 3 | Cable and network recovery | 8 min | yes |
| 4 | Long run (soak) | 2 h | no: start it and leave |
| 5 | Send back | 5 min | yes |

Optional afterwards: the breakdown of the three timing changes and the experiments (last section).

## 0. Preparation (15 min)

1. Both copies installed and paired: `SETUP.md` sections 2 and 3. Same stream quality, codec and bitrate
   in both, frame generation **off**. The kit ignores `slipstream.ini` and gives every run its own
   switches, so the file can stay.
2. Download PresentMon from its GitHub releases page (GameTechDev/PresentMon) and put the console
   program (`PresentMon-<version>-x64.exe`) into `scripts\remoteplay\testkit\` (in the test package:
   `testkit\scripts\remoteplay\testkit\`, or simply next to `Run-First-Test.cmd`). Without it the kit
   still runs; frame times, dropped frames and the present mode are then missing.
3. Screen at 180 Hz, PC and PS5 on cable, nothing else heavy running (browser video, downloads, overlays).
4. GTA 6 (or the game you play) loaded and standing in a **calm place you can return to**: every run
   must show the same scene. In-game settings untouched for the whole session.
5. Open PowerShell with **Run as administrator** (PresentMon needs it) and go to the kit:
   `cd <repository>\scripts\remoteplay\testkit` (in the test package, `<repository>` is the `testkit`
   folder of the package: every command below works from there unchanged).

## 1. Check and main comparison (40 min)

```powershell
powershell -ExecutionPolicy Bypass -File .\run-tests.ps1 -Quick
```

From the test package the same thing is one double-click: **`Run-First-Test.cmd`** (right-click, *Run as
administrator* when PresentMon is there). It checks that the original and the stable candidate are
installed and then starts exactly this command; `Run-First-Test.cmd -Build experimental` compares the
experimental build with the original instead.

The kit first prints the hardware it found. **Stop and fix** if it warns about Wi-Fi, a refresh rate
below 180 Hz or a missing NVIDIA adapter; then start again.

It then starts PushFPS nine times: three arms, three rounds, in the order A B C, C B A, A B C.

| Arm | What it is |
|---|---|
| `original` | the original build |
| `mod-legacy` | the modified build with the original timing switched back on (the baseline) |
| `mod-all` | the modified build as installed |

Each time, do exactly this (about 4 minutes):

1. Connect to the PS5 and wait in the game, at your calm place. Put the controller down.
2. PS5 streaming dialog > latency test > set **20** presses > start. When it has finished, start it a
   second time. The test opens and closes the PS5 control centre by itself.
3. Close PushFPS. In the PowerShell window press Enter for the next run (`q` stops and still makes the zip).

After three rounds every arm has 120 presses; the kit offers one more round for any arm below that.
At the end it prints the path of a zip. **This zip alone is already worth sending.**

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

Count every press, discard none. Then pack the folder again:
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
values and host names masked; the PresentMon captures; the once-a-second samples; the hardware report
without the computer name and connection names; `camera.csv`. **Never** in it: the settings folder
(`runtime_local`), the full log, console registration keys, PSN account ids, tokens, Wi-Fi names, your
folder paths or user name. The exact rules are in the header of `run-tests.ps1`; the kit checks its own
files before packing and leaves out (and names) any file that fails. You can open the zip and look.

## Optional, after the first results

- Breakdown of the three timing changes, all six standard arms (about 80 min): `.\run-tests.ps1`.
- Experiments, only when asked: `.\run-tests.ps1 -Arms mod-all,exp-poll2` and so on; the arm names and
  notes are in `arms.csv`. For `exp-fg` and `exp-fg-adaptive-phase` set frame generation to 3X in both.

## How the results are read

`python tools\remoteplay_latency_bench\report.py <zip>` writes `report.html` and `report.md`: per arm the
median, P95 and P99 of the built-in test, the mean difference to the baseline with its 95 % interval,
displayed frame times, dropped frames, present mode, GPU, CPU and memory. A difference is called real
only when the interval excludes zero and both arms have at least 120 presses. Only the camera table is
input to photon. Frames PushFPS generates are counted apart from pictures the console sent.
