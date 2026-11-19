# Setup: the original build and the low-latency build side by side

"Slipstream" is the working name of the low-latency PS5 Remote Play build of PushFPS (built on Veyra by
Likely7). Everything here is **unverified on Windows**: the build has not run on a real PC yet, so read
each step as "what is supposed to happen" and report what does not.

## 1. What you need

- The two zips from the build run: **original** (commit `f306f570`) and **modified** (the Slipstream branch).
- PS5 and PC on wired Ethernet, Remote Play enabled on the PS5 (*Settings > System > Remote Play*).
- A few GB of free disk space. No administrator rights for the install.

## 2. Install both copies (10 minutes)

Each copy lives in its own folder with its own settings and logs; neither touches the other.
Unzip each zip into its own temporary folder, then in a PowerShell window in that folder:

```powershell
# in the unzipped ORIGINAL folder
.\Install-PushFPS.cmd -InstallDir "$env:LOCALAPPDATA\Programs\PushFPS-original" -NoShortcuts -NoLaunch
# in the unzipped MODIFIED folder
.\Install-PushFPS.cmd -InstallDir "$env:LOCALAPPDATA\Programs\PushFPS-slipstream" -NoShortcuts -NoLaunch
```

The installer downloads the runtime package once per copy (internet needed). These two folder names are
the ones the test kit looks for by itself. If you already use PushFPS, leave that install alone.

## 3. First launch and pairing (5 minutes per copy)

1. Start `PushFPS-slipstream\PushFPS.exe`. Windows SmartScreen may ask: *More info > Run anyway*.
2. Open **PS5 streaming**. On the PS5: *Settings > System > Remote Play > Link Device* shows an 8-digit code.
   Enter your PSN account ID and that code in PushFPS, connect once, check picture, sound and controller.
3. Start `PushFPS-original\PushFPS.exe` and open **PS5 streaming**. The PS5 sign-in is kept in your
   Windows user profile, so the console may already be listed; if it is not, pair this copy the same way.
4. In **both** copies set the same stream quality, codec and bitrate, and frame generation **off**.
   Run only one copy at a time.

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
every mode but `ultra-low-latency`. It does **not** yet set frame generation, its multiplier or variable
refresh: set those in the interface to the values in the table (quick setup, *Target FPS*; *Smooth frame
pacing* on for anything with frame generation on a 180 Hz screen). `automatic` and `ceiling` are accepted
and logged, but nothing in the program changes mode by itself yet. For GTA 6 start with `ultra-low-latency`.

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
| `VEYRA_EXP_RP_ADAPTIVE_PHASE` | `1` / `0` | with frame generation: learn the pair phase instead of holding a full source frame |
| `VEYRA_TEST_RP_LEGACY_TIMING` | `1` / `0` | the original timing of all three changed parts (the rollback switch) |
| `VEYRA_TEST_RP_LEGACY_INPUT` | `1` / `0` | original controller dispatch and 8 ms poll only |
| `VEYRA_TEST_RP_LEGACY_OWNER_WAIT` | `1` / `0` | original 1 ms polling of the session loop only |
| `VEYRA_TEST_RP_LEGACY_ENGINE_WAKE` | `1` / `0` | original polling for a decoded frame only |
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

## 6. Rollback

From the smallest step to the largest; each one is complete by itself.

1. **One change off:** add `VEYRA_TEST_RP_LEGACY_TIMING = 1` to `slipstream.ini` (or one of the three
   per-part keys) and restart. The modified build then runs the original timing.
2. **All settings off:** delete or rename `slipstream.ini` and restart.
3. **Use the original build:** start `PushFPS-original\PushFPS.exe`. It never reads `slipstream.ini`.
4. **Remove the modified build:** in its unzipped folder run
   `.\Install-PushFPS.cmd -Uninstall -InstallDir "$env:LOCALAPPDATA\Programs\PushFPS-slipstream"`,
   or delete that folder. Your original copy and your PS5 pairing stay.

Next: `HARDWARE_TEST_PROCEDURE.md` for the first measuring session.
