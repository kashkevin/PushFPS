# Setup: the original build and the low-latency build side by side

"Slipstream" is the working name of the low-latency PS5 Remote Play build of PushFPS (built on Veyra by
Likely7).

**What has been checked, and what has not.** The builds were compiled by MSVC on GitHub's Windows
runner, installed there with the installer in the zip, and started (the interface loaded without errors;
that machine has no graphics card, no PS5 and no display to measure). Nothing here has run on your PC:
no build has decoded a PS5 stream, been paired, or been measured. Read each step as "what is supposed to
happen" and report what does not.

## 1. What you need

- The test package: one zip named `Slipstream-test-package-<date>-<run>.zip`. It holds up to four
  builds, the measuring kit and these documents; `START-HERE.txt` in it says which builds it has.
  - **original**: commit `ed317efb`, PushFPS 3.2.1 as published, built by the same workflow. The baseline.
  - **stable**: the stable candidate of the low-latency work. Use this one first.
  - **experimental**: newer work that has had less checking. For later.
  - **previous** (since Package 8): the experimental build of the package before, unchanged, installed into its
    own folder (`PushFPS-slipstream-p7`). It is the copy to go back to, and what the kit's `-Session builds`
    compares the new experimental build with.
- PS5 and PC on wired Ethernet, Remote Play enabled on the PS5 (*Settings > System > Remote Play*).
- About 4 GB of free disk space and one download of about 750 MB. No administrator rights for the install.

## 2. Install (10 to 20 minutes)

Right-click the zip, *Extract All*, then in the extracted folder double-click **`Install-Slipstream.cmd`**.
It runs each build's own installer with `-NoShortcuts -NoLaunch` into

| Build | Folder | What the kit calls it |
|---|---|---|
| original | `%LOCALAPPDATA%\Programs\PushFPS-original` | `original` |
| stable candidate | `%LOCALAPPDATA%\Programs\PushFPS-slipstream` | `modified` |
| experimental | `%LOCALAPPDATA%\Programs\PushFPS-slipstream-exp` | `modified` with `Run-First-Test.cmd -Build experimental` |

and prints what it did. Each copy has its own settings and logs inside its folder; none touches another.
The runtime package is downloaded once and reused for the other copies. A build that is not in the
package is skipped and named. Running it again changes nothing in a copy that already holds the same
build (`-Force` installs it again; settings, logs and pairing survive).

In each copy it also writes `Slipstream-INSTALL.txt` (which build this is) and protects the copy against
the in-app updater (next section). In the stable candidate only, when there is no `slipstream.ini` yet,
it writes one with `mode = ultra-low-latency` (section 4); an existing file is never changed.

If you already use PushFPS: that install (`%LOCALAPPDATA%\Programs\PushFPS`), its shortcuts and its
settings are never touched. Do **not** start an `Install-PushFPS` file of a build by hand without
`-InstallDir`: it would install the development build over your PushFPS. The package leaves the builds'
own double-click installer out for that reason.

By hand, for one build from its own zip instead of the package (PowerShell, in the unzipped folder):

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-PushFPS.ps1 -InstallDir "$env:LOCALAPPDATA\Programs\PushFPS-slipstream" -NoShortcuts -NoLaunch
```

A copy installed that way has no updater protection: read the next section.

### The in-app updater

Every build here is a development build and reports a version such as `2.1.5-pushfps.<run>`, while the
official project publishes 3.x releases. An unprotected copy therefore shows "new version" a few seconds
after it starts, and one click on that banner downloads the official release and installs it **over the
test copy**, which ends the comparison. Two independent protections:

1. **In the program** (builds made from a branch that has the guard). The updater is compiled out of
   use: no request, no banner, no install. The window title and *Settings > About* show a version ending
   in `-slipstream`, and `logs\veyra-qml.log` has `[update-check] disabled: Slipstream development build`.
   In any build with this code, `VEYRA_EXP_RP_NO_UPDATE = 1` in `slipstream.ini` (or in the environment)
   turns the check off as well.
2. **In the installed copy** (every build, including the original, which has no such code).
   `Install-Slipstream.cmd` puts the line `PushFPS 999.0.0` first in the copy's `PushFPS-VERSION.txt`. The
   program takes its installed version from that file when it is higher than its own, so the check
   answers "already the latest": no banner, nothing to click. The log then has
   `[update-check] current installed=999.0.0 ...`. The line is not a version; the real one is in
   `Slipstream-INSTALL.txt` and in *About*. Installing a copy again by hand removes the line; running
   `Install-Slipstream.cmd` again puts it back.

In a protected copy, *Settings > About > check for updates* says that nothing newer exists (or does
nothing, for protection 1). That is intended. How to tell that a copy was replaced after all: *About*
shows a plain `3.x.y` version. Run `Install-Slipstream.cmd -Force`.

Without either protection (a copy installed by hand): do not click the update banner, it goes away after
8 seconds; `PushFPS.exe --no-update-check` skips the check for that start.

Protection 2 was run against the program's update code on Linux (`tests/unit/UpdateCheckerTests.cpp`).
Both are checked on GitHub's Windows runner by the package workflow, which starts every installed copy and
reads its log; neither has run on your PC.

## 3. First launch and pairing (5 minutes per copy)

1. Start `PushFPS-slipstream\PushFPS.exe`. Windows SmartScreen may ask: *More info > Run anyway*.
2. Open **PS5 streaming**. On the PS5: *Settings > System > Remote Play > Link Device* shows an 8-digit code.
   Enter your PSN account ID and that code in PushFPS, connect once, check picture, sound and controller.
3. Start `PushFPS-original\PushFPS.exe` and open **PS5 streaming**. The PS5 sign-in is kept in your
   Windows user profile, so the console may already be listed; if it is not, pair this copy the same way.
4. In **both** copies set the same stream quality, codec and bitrate, and frame generation **off**.
   Run only one copy at a time.

### The first-launch card and "Recommended settings"

The home page's first-launch card (on a new install, until you press *Got it, don't show again*; an install
that already has PushFPS settings from an earlier version never shows it) and the **Recommended
settings** button beside the performance mode row show what PushFPS would set on this PC, with a reason for
each choice and a list of what it could not find out. Nothing changes until you press **Apply**; the card
then lists every setting it changed, and **Undo** puts back each one you have not changed yourself since.
**Keep my settings** changes nothing and stops the card offering it. Without measurements the recommendation
is always Ultra Low Latency with frame generation off. When a measurement is the only thing missing (a
60 fps game on a 120 Hz or faster screen), **Calibrate** runs for about 16 seconds on what is playing, briefly
changing frame generation and enhancement, and restores your settings when it finishes, is cancelled, or at
the next start if PushFPS stopped halfway. Only then can it recommend Competitive (2X). The recommendations
are estimates from reported capabilities and a short GPU-time sample; they have not been checked against
latency measured on real hardware. The measured path itself still needs checking on Windows: the GPU time it
reads leaves out the colour and blit passes, so with everything off it may read 0, which counts as "not
measured" (never as fast), and a calibration then ends without numbers. Only one calibration runs at a time,
and none while *Optimise for my PC*, the frame quality test or the latency test runs. Frame generation is
recommended without VRR only at outputs that divide the refresh rate evenly; with VRR only when the driver
reports it active.

## 4. Choosing a mode with `slipstream.ini`

Create a text file named `slipstream.ini` next to `PushFPS-slipstream\PushFPS.exe`. It is read once when
the program starts. No file means the program behaves exactly as without this feature. Example:

```ini
# lowest latency for a 30 fps game such as GTA 6
mode = ultra-low-latency
lan = 0
```

| `mode =` | 30 fps game | 60 fps game | Delay the generated frames add (at least) |
|---|---|---|---|
| `ultra-low-latency` | no frame generation | no frame generation | none |
| `competitive` | no frame generation | 2X, 120 shown | none / 8.3 ms |
| `balanced` | 3X, 90 shown | 2X, 120 shown | 22.2 ms / 8.3 ms |
| `maximum-smoothness` | 4X, 120 shown | 3X, 180 shown | 25.0 ms / 11.1 ms |
| `automatic` | moves between the four, never above `ceiling` | | as the mode in use |

The delay is arithmetic, not a measurement: an in-between picture cannot be shown before the next real
one has arrived, so a multiplier M holds every real picture back by (M-1)/M of a source frame, plus the
generator's own time. 30 to 180 (6X) and 60 to 90 (1.5X) are not available.

**What the file does today, and what it does not.** `mode` switches on the experiment switches that mode
asks for: a 2 ms controller poll for `ultra-low-latency` and `competitive`, the adaptive pair phase for
every mode but `ultra-low-latency` (that one no longer comes from the file since stream "lagfix", see
section 5; the same mode chosen in the interface still asks for it). It does **not** yet set frame generation, its multiplier or variable
refresh: set those in the interface to the values in the table (quick setup, *Target FPS*; *Smooth frame
pacing* on for anything with frame generation on a 180 Hz screen). `automatic` and `ceiling` are accepted
and logged, and `automatic` sets the adaptive pair phase like the generating modes, but nothing in the
program changes mode by itself because of the file: the interface's **Automatic** row exists only when
PushFPS is started with `VEYRA_EXP_AUTO_MODE=1` in its environment, which `slipstream.ini` cannot set (it is
read before the file, and the file only sets the experiment switches of a mode). For GTA 6 start with
`ultra-low-latency`.

**The same mode chosen in the interface.** Picking a mode in the quick setup's *Performance mode* row (or
through **Recommended settings**) also asks for that mode's switches: the 2 ms controller poll for Ultra
Low Latency and Competitive, the adaptive pair phase for the other modes, and, while **Automatic** is on,
connection health (`VEYRA_EXP_RP_HEALTH=1`; without it Automatic cannot see the network grade of a PS5 stream
and never steps back up). They are put in the program's environment and read when the next PS5 connection
starts, not by a stream already running; the row says so. A switch already in the environment (yours, or
one `slipstream.ini` set) is never changed or removed; the ones the interface set are removed again when
the mode changes or is turned off. With `VEYRA_TEST_RP_NO_SETTINGS_FILE` set (the test kit) nothing is set.
The log line is `[push-mode] switches ...`. A mode chosen before this version keeps running without these
switches until you choose it (or turn Automatic on) again, so an update changes nothing by itself.

## 5. Every key

Lines are `key = value`; a line starting with `#` is a comment. One line the program cannot read makes it
ignore the whole file (the log says which line: `[remoteplay-settings]` in `logs\veyra-qml.log`). Unknown
keys are ignored and listed once in the same log line. A switch already set in the environment of the
program wins over the file. A switch written in the file wins over what `mode` would set; an empty value
(`KEY =`) keeps that switch off even if the mode asks for it.

| Key | Values | Effect |
|---|---|---|
| `mode` | see the table above | switches the mode asks for |
| `ceiling` | a mode other than `automatic` | highest-latency mode `automatic` may use (not used yet) |
| `lan` | `1` / `0` | LAN profile: sets `VEYRA_EXP_RP_LAN_PROFILE=1`. Only a build that contains the network work reads it |
| `VEYRA_EXP_RP_PAD_POLL_MS` | `1` to `8` | controller, keyboard and mouse poll interval in ms (default 4) |
| `VEYRA_EXP_RP_ADAPTIVE_PHASE` | `1` / `0` | **ignored in the file since stream "lagfix"** (environment or the interface's modes only): with frame generation, learn the pair phase instead of holding a full source frame |
| `VEYRA_TEST_RP_LEGACY_PACKAGE13` | `1` | **every change that Package 14 turned on by default behaves as in Package 13** (the six rows below at once) |
| `VEYRA_TEST_RP_LEGACY_ROBUST_ADMIT` | `1` | capture card: admission by the 2 s P95 rule alone |
| `VEYRA_TEST_RP_LEGACY_GRID_EARLY` | `1` | capture card: a skipped pair's real frame at the full phase |
| `VEYRA_TEST_RP_LEGACY_DEDUP_CADENCE` | `1` | capture card: duplicate removal's old content cadence |
| `VEYRA_TEST_RP_LEGACY_GPU_REAL_WAITS` | `1` | a real frame waits for its whole pair's GPU lists |
| `VEYRA_TEST_RP_LEGACY_UPLOAD_RELEASE` | `1` | the capture upload buffer is released after the pair's last list |
| `VEYRA_TEST_RP_LEGACY_NR_HOLD` | `1` | the neural layer's saved state is kept as it is (no hold marker) |
| `VEYRA_TEST_RP_LEGACY_TIMING` | `1` / `0` | the original timing of all three changed parts (the rollback switch); also restores the original answer to an expired controller state (as `_INPUT_EXPIRY`) |
| `VEYRA_TEST_RP_LEGACY_INPUT` | `1` / `0` | original controller dispatch and 8 ms poll only |
| `VEYRA_TEST_RP_LEGACY_OWNER_WAIT` | `1` / `0` | original 1 ms polling of the session loop only |
| `VEYRA_TEST_RP_LEGACY_ENGINE_WAKE` | `1` / `0` | original polling for a decoded frame only |
| `VEYRA_TEST_RP_LEGACY_INPUT_EXPIRY` | `1` / `0` | a controller state that waited more than 100 ms is answered with one neutral state (everything released) as before Veyra 9ff43a77, instead of being dropped alone |
| `VEYRA_EXP_RP_KBM` | `1`, `gyro`, `0` | keyboard and mouse as a controller: mouse on the right stick, or as motion aiming |
| `VEYRA_EXP_RP_KBM_SENS`, `_FULL`, `_DEADZONE`, `_CURVE`, `_YSCALE`, `_SMOOTH_MS`, `_GYRO_DEG` | numbers | mouse aim tuning (see `PS5_REMOTE_PLAY_EXPERIMENTS.md`) |
| `VEYRA_EXP_RP_KBM_INVERT_X`, `_INVERT_Y` | `1` / `0` | invert a mouse axis |
| `VEYRA_EXP_RP_KBM_BINDS` | `"F=Square;Mouse2=None"` | key and button overrides |
| `VEYRA_EXP_RP_REORDER_US`, `_RCVBUF`, `_THREAD_PRIORITY`, `_BACKLOG`, `_IDR_RETRY_MS`, `_HEALTH` | see the network notes | network experiments; only in a build that contains the network work |

Only keys beginning with `VEYRA_EXP_RP_` or `VEYRA_TEST_RP_` can become switches; the file cannot set any
other variable. Many online games forbid presenting a mouse as a controller: `VEYRA_EXP_RP_KBM` is your
responsibility. `VEYRA_TEST_RP_NO_SETTINGS_FILE=1` in the environment (not in the file) makes the program
skip the file; the test kit uses it so that every test arm has exactly its own switches.

To check what was applied: after a start, the newest `[remoteplay-settings]` line in
`PushFPS-slipstream\logs\veyra-qml.log` lists the mode, every switch set and every key ignored.

**Only input, network, audio and update switches come from the file** (stream "lagfix", 23 Nov 2026): the
keys in the table above except `VEYRA_EXP_RP_ADAPTIVE_PHASE`, the other `VEYRA_EXP_RP_KBM*` and
`VEYRA_EXP_RP_AUDIO_*` switches, `smartlink`, `_TRIGGER_FILTER`, `_LATENCY_BUTTONS`, `_REORDER_*`, `_EARLY_LOSS`,
`_RECONNECT_PATIENT`, `_WIFI_STREAMING_MODE`, `_RESUME_AFTER_DEVICE_LOSS`, `_NO_UPDATE` and the rollback switches
`VEYRA_TEST_RP_LEGACY_*`. Every other switch the file names (frame generation, pair phase, pacing, frame
delivery, GPU, statistics, other test switches) is listed once as `[remoteplay-settings] slipstream.ini
ignored=[...]` and not set, so a line left from a test cannot change normal play. The test kit sets its
switches in the environment and is unaffected. Every start also logs which switches are in force and from
where: `[experiment-switches] in force at start: NAME=value(ini|env) ...` (`none` without any).

## 6. Rollback

From the smallest step to the largest; each one is complete by itself. The original build is installed
beside the others the whole time and never reads `slipstream.ini`.

1. **One change off:** add `VEYRA_TEST_RP_LEGACY_TIMING = 1` to `slipstream.ini` (or one of the three
   per-part keys) and restart. The modified build then runs the original timing.
2. **All settings off:** double-click **`Rollback-Slipstream.cmd`** in the package. It renames
   `slipstream.ini` in the stable candidate and the experimental build to `slipstream.ini.off-<date>-<time>`
   (nothing is deleted); from the next start those builds behave as builds without a settings file. Doing
   the same by hand, deleting or renaming the file, has the same effect. To use the file again, rename
   it back. `Install-Slipstream.cmd` does not write a new one after a rollback.
3. **Use the original build:** start `PushFPS-original\PushFPS.exe`.
4. **Remove the modified builds, keep the original** (PowerShell, in the package folder):
   `powershell -ExecutionPolicy Bypass -File testkit\scripts\remoteplay\testkit\uninstall-slipstream.ps1 -Mode Modified`
5. **Remove everything the package installed:** double-click **`Uninstall-Slipstream.cmd`**.

Steps 4 and 5 list the folders, ask you to type `YES`, and then delete exactly the folders
`PushFPS-original`, `PushFPS-slipstream`, `PushFPS-slipstream-exp` under `%LOCALAPPDATA%\Programs`, with
each copy's own settings and logs inside. A folder is left alone when it has no `Slipstream-INSTALL.txt`
(you installed it by hand: add `-IncludeUnmarked`), when its PushFPS is running, or when it holds a link.
They never touch `%LOCALAPPDATA%\Programs\PushFPS` (an official install) or its settings, the Start menu
and desktop shortcuts, your PS5 pairing, or your test results. Do **not** use `Install-PushFPS.cmd
-Uninstall` for a test copy: besides the folder it deletes the shortcuts named PushFPS, which belong to
your normal PushFPS install if you have one.

## 7. What is stored where

The package holds programs, scripts and documents only. It contains no account, no console registration,
no settings, no logs and no keys; the tool that makes it refuses to write a package holding a settings
folder, a log, a `slipstream.ini`, a key file or anything that looks like a token.

| What | Where on your PC | Who writes it | Removed by `Uninstall-Slipstream.cmd` |
|---|---|---|---|
| Program files of a copy | `%LOCALAPPDATA%\Programs\PushFPS-original`, `-slipstream`, `-slipstream-exp` | the installer | yes |
| `PushFPS-VERSION.txt`, `Slipstream-INSTALL.txt` (versions, hashes, dates; nothing personal) | in each copy's folder | the installer, `Install-Slipstream` | yes |
| `slipstream.ini` (mode and switches) | in the stable candidate's folder | `Install-Slipstream`, you | yes |
| Settings of a copy, including stream keys for going live (encrypted for your Windows account) and a Discord webhook if you set one | `<copy>\runtime_local\user-data-2.0.0` | the program | yes, with the folder |
| Logs of a copy (may hold addresses and device names of your network) | `<copy>\logs` | the program | yes, with the folder |
| **PS5 pairing: console registration key and PSN account ID**, encrypted for your Windows account | `%LOCALAPPDATA%\Veyra\remoteplay` | the program | **no**: shared by every copy, also an official PushFPS |
| Moonlight and Xbox sign-ins, if you use them | `%LOCALAPPDATA%\Veyra\...` | the program | no |
| Downloaded runtime package | `%TEMP%\PushFPS-download` | the installer | no (delete it when you like) |
| Test results | `Documents\slipstream-evidence` | the test kit | no |

Because the pairing is kept per Windows account and not per copy, pairing one copy usually pairs all of
them, and removing the test copies does not unpair anything. To remove a pairing, use the delete button
in the PS5 streaming dialog of any copy, or delete that folder.

Calibrated mouse profiles of the keyboard and mouse mode (`slipstream-kbm.ini`, which names the game, and
`slipstream-kbm-<game>.txt`) are kept in the same shared folder, `%LOCALAPPDATA%\Veyra\remoteplay`. Every
copy reads the same files, an official PushFPS install too, and `Uninstall-Slipstream.cmd` leaves them
there; delete them by hand when you no longer want them.

Nothing from this table leaves the PC unless you send it. The only thing meant to be sent is the kit's
`evidence-*.zip`; what goes into it, and what never does (registration keys, account IDs, tokens, the
settings folder, the full log), is in `HARDWARE_TEST_PROCEDURE.md`, section 5. The install, rollback and
uninstall scripts read no log and no settings and send nothing anywhere; the installer contacts GitHub
only, to download the runtime package.

Next: `HARDWARE_TEST_PROCEDURE.md` for the first measuring session.
