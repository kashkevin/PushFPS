<#
.SYNOPSIS
  Slipstream hardware test kit: one command that runs the PS5 Remote Play latency arms on the real PC,
  collects the evidence in one folder and packs it into one zip to send back.

.DESCRIPTION
  For each arm (see arms.csv) the kit starts PushFPS with that arm's environment switches, records while
  you use it, and saves:
    - the log lines of that run that belong to the whitelisted families (below), with addresses masked;
    - a PresentMon capture, when PresentMon is found (parameter, this folder, or PATH);
    - once-a-second samples of processor load, PushFPS memory, handles and threads; every other second the
      GPU load, video decoder load and video memory of the PushFPS process itself (Windows' own GPU
      counters, any GPU maker); and, when nvidia-smi exists, the whole first GPU's load and video memory.
  Arms are interleaved (A B C, then C B A ...) and the kit counts the presses of the built-in latency test
  until every arm has 120.

  WHAT GOES INTO THE ZIP - the whitelist
    Log lines are copied ONLY when their component (the second bracket of a line) is
        [push-latency] [push-quality] [push-benchmark] [push-mode] [push-hardware] [push-profile]
        [slipstream-governor] [slipstream-gpu-budget] [slipstream-watch]
        [remoteplay] or [remoteplay-<anything>]   except [remoteplay-discovery]
        [frame-flow] [frame-rate] [pacing] [present-deviation] [fg-validity] [fg-drops]
        [display-vrr] [display-refresh] [display-latency] [display-query]
        [gpu-failure] [gpu-reconnect] [reset-lifecycle] [engine-hang] [engine-test] [crash]
        [overlay-hooks] [log-backpressure], and [qml] lines that start with [update-check]
    (numbers about frames, the display, the GPU and the program's own version; no other family)
    and the line holds none of these words: registration, regist_key, rp_key, account, psn (except in
    psnr=, a picture quality value), token, secret, passw, ssid, morning, nickname. In the lines that are copied:
        folder and file paths (C:\...) become [path]
        IPv4 addresses keep only their private-range prefix (192.168.x.x, 10.x.x.x, 172.16.x.x ... 172.31.x.x,
        169.254.x.x, 127.x.x.x); any other address becomes x.x.x.x
        IPv6 addresses become [ipv6], MAC addresses [mac], runs of 16 or more hex digits [hex]
        host=<name> and name=<name> become host=[name] / name=[name]
    Lines older than the start of the run are left out (the log file also holds earlier sessions).
    From the hardware report the computer name and the network connection names are removed.
    Never copied: the settings store (runtime_local), the whole log, console registration keys, PSN account
    ids, tokens, Wi-Fi names, the paths of your folders, your Windows user name.

  WHAT THE KIT DOES NOT DO
    It installs nothing, loads no driver and changes no Windows, driver, router or PS5 setting. It sets
    environment variables for the PushFPS processes it starts and for nothing else. Only with -Reliability
    does it end a PushFPS process on purpose (the one it started, once, for the crash recovery check).

  Windows PowerShell 5.1 or newer. Start it normally, NOT as administrator: PushFPS is always started with your
  normal rights. PresentMon needs administrator rights to read frame times, so only PresentMon is started elevated:
  Windows asks once per run (a long run: once, for its first capture window). PresentMon is used only when its
  file carries a valid Authenticode signature of Intel Corporation; otherwise the kit names the problem and goes on
  without it (the program's own frame lines are still recorded).

.PARAMETER OriginalExe
  PushFPS.exe of the original build. Default: %LOCALAPPDATA%\Programs\PushFPS-original\PushFPS.exe.
.PARAMETER ModifiedExe
  PushFPS.exe of the modified build. Default: %LOCALAPPDATA%\Programs\PushFPS-slipstream\PushFPS.exe.
.PARAMETER PresentMon
  Path of the PresentMon console program. Default: this folder, then PATH.
.PARAMETER Ps5Address
  Optional IP address of the PS5: 20 pings are recorded (the address itself is masked in the zip).
.PARAMETER OutRoot
  Where the evidence folders and zips go. Default: Documents\slipstream-evidence.
.PARAMETER Quick
  Only the three main arms: original, mod-legacy, mod-all.
.PARAMETER Experimental
  Also the experimental arms. -Arms name,name runs exactly the named arms instead.
.PARAMETER Rounds
  Rounds of all arms (default 3). The tests per start are planned so that the rounds give -PressesPerArm presses.
.PARAMETER Health
  Also sets VEYRA_EXP_RP_HEALTH=1 in every run of the modified build (the same in every arm), so that the
  program logs its network health line every 5 s: loss, jitter, late packets, recoveries, bitrate, route.
  Observation only. Soak and impairment runs always set it.
.PARAMETER Soak
  Long run of one arm (-SoakArm, default mod-all): 2 hours unless -SoakHours (for example 6) or -SoakMinutes
  says otherwise. Sampled once a second: memory, GPU memory, handles and threads of PushFPS; from its log:
  reconnects, drops, processing times; from PresentMon: -SoakPresentMonSeconds (60) of frames every
  -SoakPresentMonEveryMinutes (15). The report fits a trend to each and flags growth.
  KILL-SWITCH: press Q or Ctrl+C in the kit's window, or create a file named stop.txt in the evidence folder
  (the kit prints its path). The kit then closes PushFPS, saves everything recorded so far and makes the zip.
.PARAMETER Impairment
  Guided recovery test: cable out for 1 s, for 5 s, and a second device loading the network.
.PARAMETER Reliability
  Guided reliability checks of one arm (-ReliabilityArm, default mod-all), each ending in a PASS / FAIL line:
  cable out for 1 s and for 5 s, display mode change, display unplugged and plugged back, focus lost with keys
  held (needs -ReliabilityKbm, which starts that run with VEYRA_EXP_RP_KBM=1), PushFPS ended abruptly and
  started again (the kit ends the PushFPS it started, once, on purpose), then a start with no switches and no
  slipstream.ini. -ReliabilityGpuLoss adds a run in which PushFPS removes its own graphics device once
  (VEYRA_TEST_GPU_FAILURE: only PushFPS is affected, no driver is touched). See docs/RELIABILITY.md.
.PARAMETER Camera
  Camera session: starts each chosen arm once (default original and mod-all) so that you can film your own
  presses; no latency test is asked for. See docs/HARDWARE_TEST_PROCEDURE.md.
.PARAMETER SkipLatency
  Skip the latency arms (to run only -Soak or -Impairment).
.PARAMETER PackOnly
  An existing evidence folder: check it and make the zip again (after camera.csv was filled in).
.PARAMETER DryRun
  Starts no PushFPS and asks nothing: runs every step on made-up data. Used by the checks workflow.
  -DryRunStopAfter N makes a dry soak write its own stop file after N samples, to exercise the kill-switch.
  -DryRunPresses N (default 20) makes every dry latency run log N presses, 'original' 20 ms slower and 'mod-all'
  20 ms faster than the rest, so that the LOWER, HIGHER and overlap verdicts of RESULT.txt all run.
  -DryRunMismatchArm NAME gives that arm's made-up log another bitrate, to exercise the refusal of an arm whose
  settings differ from its pair. -DryRunQueueArm NAME logs the low-latency queue off in that arm (refused, key lowQueue).
  -DryRunWithout a,b leaves the lines of those log components out (a build without that feature: for example
  slipstream-pipeline for a build without VEYRA_TEST_RP_PIPELINE, slipstream-input for a build before the input core).
  -DryRunNoNeural makes the neural keys unknown to the build (8A / 8B: refused:unknown-key, no [nr-runtime-status]).
  -DryRunWifi makes every made-up log say the network is Wi-Fi. Arms of the experimental build get the sprint 4 lines
  ([slipstream-config], -press, -latency, -delivery, -sr, -fg, -autorun, [remoteplay-audio] with pops); the other builds
  do not, as a real original or stable build.
.PARAMETER ExperimentalExe
  PushFPS.exe of the experimental build (arms whose exe is 'experimental' in arms.csv). Default:
  %LOCALAPPDATA%\Programs\PushFPS-slipstream-exp\PushFPS.exe when it exists; otherwise those arms are skipped.
.PARAMETER PreviousExe
  PushFPS.exe of Package 7's own experimental build (arms whose exe is 'previous' in arms.csv: -Session builds). Default:
  %LOCALAPPDATA%\Programs\PushFPS-slipstream-p7\PushFPS.exe when it exists; otherwise those arms are skipped with a line.
.PARAMETER Versus
  The stable candidate against the experimental build: the arms of the set 'versus' in arms.csv, one named switch
  set per arm (LAN profile, adaptive reorder, health, backlog, keyboard and mouse, audio fast recovery, governor
  observe, GPU budget observe). Duplicate detection is in no arm.
.PARAMETER Full
  One long session: the latency arms, then a 10-minute stability run (-Soak, -SoakMinutes 10 unless given) and the
  reliability checks (-Reliability), with a break before each part. The estimated time is shown first.
.PARAMETER Clip
  Stream clarity: a recorded clip (absolute or relative path to a video file) that every measured run plays instead of
  connecting to the PS5 (VEYRA_TEST_RP_CLIP), so the arms of a picture comparison process the same frames, for example
  -Session visual -Arms cl-a-raw,cl-b-nis,cl-c-vsr,cl-d-neural,cl-e-vsr-neural,cl-f-clean-nis -Clip D:\clips\foliage.mp4.
  The clip should last at least 90 seconds; every arm starts it at 0. The kit records '<clip>', not the path; the app's
  hook logs no path. Not with -NoTestProfile, -NoAutomation or arms that ask for their own bitrate or codec.
.PARAMETER SceneNote
  A few words about the place in the game where every run is made. Asked once when not given; kept in kit.json
  and every run.json (letters, digits and simple punctuation only, 80 characters at most).
.PARAMETER Resume
  An unfinished evidence folder (no zip beside it yet), or 'last' for the newest one under -OutRoot: the kit counts
  the runs it already holds and goes on from there.
.PARAMETER ExpectRefreshHz
  Refresh rate the hardware report should warn below. Default 0: no fixed expectation (the PC's own highest rate
  is shown by Run-First-Test's checks).
.PARAMETER AllowElevated
  The kit refuses to run as administrator, so that PushFPS (a network program) never starts elevated. Only where
  every program runs elevated anyway (User Account Control off) does this let it go on.
.PARAMETER TestsPerStart
  Built-in latency tests per start. Default: planned from -PressesPerArm, -Rounds and the most presses one test of
  that build accepts (12 in the original and the stable build; trialsMax of [slipstream-config] in a build that logs
  it), and shown on screen. When the log shows that many finished tests the kit closes PushFPS by itself;
  -NoAutoClose leaves that to you.
.PARAMETER Pass
  Only with -Session smoothness or capture: the Windows refresh rate this pass is for, 120 or 180 (stream fps). The kit asks you to
  set Windows to it before the first run, and a run whose app reports another refresh rate is not used (the kit offers
  to set it aside and repeat it). 0 (the default): no refresh rate is asked for.
.PARAMETER Session
  switches: every arm of the set 'switches' in arms.csv, all inside the experimental build, one switch per arm,
  interleaved (A B, B A ...), started without clicks (below). quality: super resolution cost, frame generation and the
  frame quality test, automatic, one start. dedup-probe: OPTIONAL diagnostic, 3 minutes of a game running at 30 FPS
  with duplicate detection measuring (it removes nothing in this build); never compared for latency.
  Kit version 5 (each automatic, no question, 2 rounds interleaved, wired or Wi-Fi labelled; RESULT.txt section MEASURED
  VALUES; the arms of a session are the lines of arms.csv whose set is the session's name):
    input     mouse and keyboard against the pad, one arm per input performance profile; the kit prints what to do
    visual    the picture paths (original, upscaling, clean-up, neural layer, combinations, the owner's reference)
    neural    the neural layer at internal sizes 720, 900, 1080 and 1440, without and with the upscaler
    framegen  every frame generation method at 2x and, experimental, 3x
    audiofix  s-base against the three audio continuity switches together (2 minutes per run)
    pacing    s-base against one arm per presentation / pacing switch
    builds    original, stable, Package 7's experimental build (-PreviousExe) and this experimental build, same test profile
    capture   a capture card instead of Remote Play: native passthrough against frame generation 2x-6x, per method, duplicate
              removal off, HUD Lock, one buffer (VEYRA_TEST_RP_CAPTURE: the card remembered in the kit's capture settings
              folder; the first time the kit asks you to choose it once). Once with a 30 FPS game, once with a 60 FPS game.
.PARAMETER PresentMonPrompt
  Allows the Windows (UAC) prompt that PresentMon needs once per run when you are not in the group "Performance Log
  Users". Without it a found PresentMon is skipped with one line.
.PARAMETER Minutes
  -Session network: minutes per run (default 3). Every arm runs twice (-Rounds, default 2) in an interleaved order
  (A B B A; A B C D D C B A with four arms): about 40 minutes for the four default arms. Play normally during the session: the stream must show real motion; a
  still menu gives a result that only speaks for a still menu. -NetworkWithBuilds adds the original and the stable build
  for the same time (compared only on what every build logs). -Full also runs the network session when the
  experimental build is installed.
.PARAMETER WifiStreamingMode
  -Session network only, never by default or in -Full: adds an arm with VEYRA_EXP_RP_WIFI_STREAMING_MODE=1, which asks the
  Wi-Fi adapter not to scan in the background while the stream runs (the request ends with the stream; other streaming
  programs use the same request). It changes how the adapter behaves and is untested here: choose it knowingly.
.PARAMETER NoTestProfile
  Start every copy with your own saved settings. By default the kit starts every copy, the original included, with
  its own settings folder (--data-dir %LOCALAPPDATA%\slipstream-kit\profile-<build>) holding a fixed test profile:
  low-latency queue on (with the NVIDIA low latency choice, which it follows on an NVIDIA GPU), display sync V-Sync, smooth pacing (variable refresh request, output
  cap following the display), full screen on connect, reconnect to the last console at start. Your own settings are
  never changed. The console pairing is shared by every copy, so nothing has to be paired again. Frame generation,
  super resolution and enhancement keep the defaults of that folder; the log of every run is checked.
.PARAMETER NoAutomation
  Guided manual steps also in arms of the experimental build (otherwise those are started with
  VEYRA_EXP_RP_STATS=1, VEYRA_TEST_RP_AUTOCONNECT=1 and VEYRA_TEST_RP_AUTORUN=<plan>, and need no click).
.PARAMETER AutorunPlan
  The plan of an automatic latency start (default settle:8;latency:<presses>;...;exit). -AutorunTimeoutMinutes
  (15) ends an automatic start that has not finished by then.

  WHILE A RUN IS GOING ON the kit reads the new log lines every second and prints what it finds once: the stream
  (resolution, frame rate, codec, bitrate), refresh rate, variable refresh and frame generation, and each finished
  latency test. Processor and GPU counters are NOT sampled while a latency test is timing (only the cheap process
  values are), so the sampling cannot add to what is measured. After each latency run its settings are compared
  with the session's first run and with what the arm needs (arms.csv column fg): a run that differs is named with
  every difference, and you can set it aside and repeat it. At the end RESULT.txt and result.json give the result
  on one page: per arm median, P95 and P99 with 95 % intervals and press counts, the verdict rule, refused arms with
  their reasons, and what to send.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File run-tests.ps1 -Quick
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File run-tests.ps1 -SkipLatency -Soak -SoakMinutes 120
#>
param(
    [string]$OriginalExe = '',
    [string]$ModifiedExe = '',
    [string]$PresentMon = '',
    [string]$Ps5Address = '',
    [string]$OutRoot = '',
    [int]$Rounds = 3,
    [int]$PressesPerArm = 120,
    [string[]]$Arms = @(),
    [switch]$Quick,
    [switch]$Experimental,
    [switch]$Soak,
    [int]$SoakMinutes = 120,
    [double]$SoakHours = 0,
    [string]$SoakArm = 'mod-all',
    [int]$SoakPresentMonSeconds = 60,
    [int]$SoakPresentMonEveryMinutes = 15,
    [switch]$Impairment,
    [string]$ImpairmentArm = 'mod-all',
    [switch]$Reliability,
    [string]$ReliabilityArm = 'mod-all',
    [switch]$ReliabilityKbm,
    [switch]$ReliabilityGpuLoss,
    [switch]$ResumeAfterDeviceLoss,
    [switch]$Camera,
    [switch]$SkipLatency,
    [string]$PackOnly = '',
    [int]$ExpectRefreshHz = 0,
    [switch]$Health,
    [int]$DryRunStopAfter = 0,
    [string]$DryRunMismatchArm = '',
    [int]$DryRunPresses = 20,
    [string]$ExperimentalExe = '',
    [string]$PreviousExe = '',
    [switch]$Versus,
    [switch]$Full,
    [string]$SceneNote = '',
    [string]$Clip = '',
    [string]$Resume = '',
    [int]$TestsPerStart = 2,
    [switch]$NoAutoClose,
    [switch]$AllowElevated,
    [string]$Session = '',
    [int]$Pass = 0,
    [switch]$NoTestProfile,
    [switch]$NoAutomation,
    [string]$AutorunPlan = '',
    [int]$AutorunTimeoutMinutes = 15,
    [string]$SelfTestBootstrap = '',
    [string]$DryRunQueueArm = '',
    [int]$Minutes = 3,
    [switch]$PresentMonPrompt,
    [switch]$NetworkWithBuilds,
    [switch]$WifiStreamingMode,
    [string]$DryRunBadTraceArm = '',
    [switch]$DryRunWifi,
    [string]$DryRunWithout = '',
    [switch]$DryRunNoNeural,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$kitVersion = '5'
$script:useTestProfile = (-not $NoTestProfile)
$script:trialsMax = @{}          # most presses one latency test accepts, per build, once a [slipstream-config] line said it
$script:sessionTrials = 12       # presses per test in this session: the same in every arm (set after the arms are chosen)
$script:automationAllowed = $false   # latency arms run automatically only when every chosen arm is of the experimental build
$testsPerStartGiven = $PSBoundParameters.ContainsKey('TestsPerStart')
$script:roundsGiven = $PSBoundParameters.ContainsKey('Rounds')
$script:dryBase = $null      # base time of the dry run's made-up timeline (Add-DryRunTimeline)
$script:referenceConfig = $null   # settings of the session's first kept latency run that logged its stream (Get-RunProblems)
$script:referenceArm = ''         # the arm of that run
$script:armReference = @{}        # arm name -> settings of that arm's first kept latency run (Get-RunProblems compares an arm with its pair)
$script:lastRun = $null           # the record and paths of the run Invoke-ArmRun finished last
$script:measuredNow = $null       # the MEASURED VALUES tables of the folder Write-ResultFiles is writing (Get-MeasuredTables)
$kitFolder = Split-Path -Parent $MyInvocation.MyCommand.Path
$sessionName = 'SlipstreamKit'

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host ('== ' + $Text) -ForegroundColor Cyan
}

function Get-UtcStamp {
    return (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
}

# --- redaction -----------------------------------------------------------------------------------
# The same rules are in tools/remoteplay_latency_bench/report.py (redact_line), which is tested on Linux
# and checks every received evidence folder again. Change both together.
$allowedLine = '\[(INFO |WARN |ERROR|TRACE)\] \[(push-(latency|quality|benchmark|mode|hardware|profile|adaptive-fg)|remoteplay(-[a-z0-9-]+)?|slipstream-(governor|gpu-budget|watch|config|latency|latency-test|press|delivery|sr|fg|autorun|smartlink|swapchain|present|pipeline|nr|clean|quality|input|input-poll|scanout|cadence|net|smooth|capture)|capture-(driver-age|buffer)|nr-runtime-status|frame-(flow|rate)|pacing|present-deviation|present-contract|qml-window|ui-(dialog|fullscreen|window|resume)|fg-(validity|drops)|display-(vrr|refresh|latency|query)|gpu-(failure|reconnect)|reset-lifecycle|engine-(hang|test)|crash|overlay-hooks|log-backpressure|qml\] \[update-check)\] '
$refusedLine = '\[remoteplay-discovery\]|registration|regist[_ -]?key|rp[_ -]?key|account|psn(?!r=)|token|secret|passw|ssid|morning|nickname'

function Protect-Text([string]$Line) {
    $t = $Line
    $t = $t -replace '(?i)[A-Za-z]:\\+Users\\+[^\\\r\n"]*', '[path]'
    $t = $t -replace '[A-Za-z]:\\(?:[^\\\r\n"<>|]*\\)*[^\s",;)<>|]*', '[path]'
    $t = $t -replace '\\{2,}[^\\\s"]+\\+[^\s",;)<>|]*', '[path]'
    $t = $t -replace '(?i)(?<![A-Za-z])[A-Za-z]:/+Users/+[^/\r\n"]*', '[path]'
    $t = $t -replace '(?<![A-Za-z])[A-Za-z]:/(?:[^/\r\n"<>|]*/)*[^\s",;)<>|]*', '[path]'
    $t = $t -replace '(?<![/\\\w.%+-])[A-Za-z0-9._%+-]+@(?:[A-Za-z0-9-]*[A-Za-z][A-Za-z0-9-]*\.)+[A-Za-z]{2,}\b', '[email]'
    $t = $t -replace '\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\b', '[guid]'
    $t = $t -replace '\b[0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}\b', '[mac]'
    $t = $t -replace '\b[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\b', '[mac]'
    $t = $t -replace '(?<![Vv]ersion=)\b10\.\d{1,3}\.\d{1,3}\.\d{1,3}\b', '10.x.x.x'
    $t = $t -replace '(?<![Vv]ersion=)\b192\.168\.\d{1,3}\.\d{1,3}\b', '192.168.x.x'
    $t = $t -replace '(?<![Vv]ersion=)\b172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}\b', '172.$1.x.x'
    $t = $t -replace '(?<![Vv]ersion=)\b169\.254\.\d{1,3}\.\d{1,3}\b', '169.254.x.x'
    $t = $t -replace '(?<![Vv]ersion=)\b127\.\d{1,3}\.\d{1,3}\.\d{1,3}\b', '127.x.x.x'
    $t = $t -replace '(?<![Vv]ersion=)\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b', 'x.x.x.x'
    $t = $t -replace '(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){3,7}[0-9A-Fa-f]{1,4}(?![0-9A-Za-z:])', '[ipv6]'
    $t = $t -replace '(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){1,6}:(?:[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4}){0,5})?(?![0-9A-Za-z:])', '[ipv6]'
    $t = $t -replace '\[ipv6\]%[0-9A-Za-z]+', '[ipv6]'
    $t = $t -replace '(?:0[xX])?[0-9A-Fa-f]{16,}', '[hex]'
    $t = $t -replace '\b(?=[0-9A-Fa-f]*[A-Fa-f])[0-9A-Fa-f]{12}\b', '[mac]'
    $t = $t -replace '\b(?=[0-9A-Fa-f]*[A-Fa-f])[0-9A-Fa-f]{13,15}\b', '[hex]'
    $t = $t -replace '\b(?-i:(host|name|server|computer|device|network|Host|Name|Server|Computer|Device|Network))=(?!\[name\]|(?:\d{1,3}|x)(?:\.(?:\d{1,3}|x)){3}(?![0-9A-Za-z.]))[^\s,;)]+', '$1=[name]'
    return $t
}

function Select-SafeLines([string[]]$Lines) {
    $kept = New-Object System.Collections.Generic.List[string]
    foreach ($line in $Lines) {
        if (-not $line) { continue }
        if ($line -cnotmatch $allowedLine) { continue }
        if ($line -match $refusedLine) { continue }
        $kept.Add((Protect-Text $line))
    }
    return $kept.ToArray()
}

# The log file also holds earlier sessions, and a long run can roll it over: only lines stamped at or after
# $FromUtc (yyyy-MM-ddTHH:mm:ss.fffZ, which sorts as text) belong to the run. Lines without a stamp are kept.
function Select-SinceStart([string[]]$Lines, [string]$FromUtc) {
    $kept = New-Object System.Collections.Generic.List[string]
    foreach ($line in $Lines) {
        if (-not $line) { continue }
        if (($line.Length -ge 24) -and ($line -match '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z ')) {
            if ([string]::CompareOrdinal($line.Substring(0, 24), $FromUtc) -lt 0) { continue }
        }
        $kept.Add($line)
    }
    return $kept.ToArray()
}

# True when a text file written by the kit still holds something the rules above would change or refuse.
function Test-FileLeaks([string]$Path) {
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        if ($line -match 'registration|regist[_ -]?key|rp[_ -]?key|account|psn(?!r=)|token|secret|passw|ssid|morning|nickname') { return $true }
        if ((Protect-Text $line) -cne $line) { return $true }
        if ((Hide-OwnWords $line) -cne $line) { return $true }
    }
    return $false
}

# PresentMon captures and samples.csv are numbers, process names and swap chain pointers (0x + 16 hex digits, which
# the [hex] mask would change and which the report needs to tell swap chains apart). Checked the same way with only
# those pointer values set aside; a file that still holds anything else the rules would change is withheld.
function Test-TableLeaks([string]$Path) {
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        $plain = $line -replace '\b0[xX][0-9A-Fa-f]{16}\b', '0x0'
        if ($plain -match 'registration|regist[_ -]?key|rp[_ -]?key|account|psn(?!r=)|token|secret|passw|ssid|morning|nickname') { return $true }
        if ((Protect-Text $plain) -cne $plain) { return $true }
        if ((Hide-OwnWords $plain) -cne $plain) { return $true }
    }
    return $false
}

# Beyond the shared rules (as collect-diagnostics.ps1): the computer name and the Windows user name, wherever they
# stand as a word of their own, become [removed].
$ownWords = @()
foreach ($word in @($env:COMPUTERNAME, $env:USERNAME)) { if ($word -and ($word.Length -ge 3)) { $ownWords += $word } }
function Hide-OwnWords([string]$Text) {
    $t = $Text
    foreach ($word in $ownWords) {
        $t = [regex]::Replace($t, ('(?i)(?<![A-Za-z0-9])' + [regex]::Escape($word) + '(?![A-Za-z0-9])'), '[removed]')
    }
    return $t
}

# --- small helpers -------------------------------------------------------------------------------
function Read-ArmTable([string]$Path) {
    $rows = Get-Content -LiteralPath $Path | Where-Object { $_ -and ($_ -notmatch '^\s*#') } | ConvertFrom-Csv
    $list = @()
    foreach ($row in $rows) {
        if ($row.name -notmatch '^[A-Za-z0-9-]+$') { throw "arms.csv: bad arm name '$($row.name)'" }
        if (@('original', 'modified', 'experimental', 'previous') -notcontains $row.exe) { throw "arms.csv: arm '$($row.name)' needs exe original, modified, experimental or previous" }
        # every arm's switches and preferences by the same rules as at its start, once here: a bad line fails before any run
        [void](Get-ArmSwitches $row)
        [void](Get-ArmPrefs $row)
        $list += $row
    }
    return $list
}

function Get-ArmSwitches($Arm) {
    $table = @{}
    if (-not $Arm.switches) { return $table }
    foreach ($pair in ($Arm.switches -split ';')) {
        if (-not $pair) { continue }
        if ($pair.StartsWith('pref:')) { continue }   # a preference of the arm's test profile (Get-ArmPrefs)
        if ($pair.StartsWith('pipeline:')) {
            # kit 5: the picture path of the arm, key=value|key=value (S5_CONTRACT.md interface 1); test-only, environment only
            $pipeline = $pair.Substring(9).Replace('|', ';')
            if ($pipeline -notmatch '^[A-Za-z0-9]+=[A-Za-z0-9.-]+(;[A-Za-z0-9]+=[A-Za-z0-9.-]+)*$') { throw "arms.csv: arm '$($Arm.name)': '$pair' is not pipeline:key=value|key=value" }
            $table['VEYRA_TEST_RP_PIPELINE'] = $pipeline
            continue
        }
        $at = $pair.IndexOf('=')
        if ($at -lt 1) { throw "arms.csv: arm '$($Arm.name)': '$pair' is not NAME=VALUE" }
        $name = $pair.Substring(0, $at).Trim()
        if ($name -notmatch '^VEYRA_(EXP|TEST)_(RP|PRESENT|SR|PIPE)_[A-Z0-9_]+$') { throw "arms.csv: arm '$($Arm.name)': only VEYRA_EXP_RP_*, VEYRA_TEST_RP_*, VEYRA_EXP_PRESENT_*, VEYRA_EXP_SR_* and VEYRA_EXP_PIPE_* switches are allowed" }
        # the alternative latency buttons (sham presses, not accepted by review) are never part of a kit run
        if ($name -eq 'VEYRA_EXP_RP_LATENCY_BUTTONS') { throw "arms.csv: arm '$($Arm.name)': VEYRA_EXP_RP_LATENCY_BUTTONS is never used by the kit (the PS button only)" }
        $table[$name] = $pair.Substring($at + 1).Trim()
    }
    return $table
}

# 'pref:NAME=VALUE' entries of an arm's switches: preferences written into its test profile (pushAutoFullscreen, and for the
# smoothness session vrr (the test profile's variable refresh request), fgAdaptive and pacingRepair; a smoothness arm
# writes the last two as off unless it names them, so no arm inherits another's from the shared settings folder).
function Get-ArmPrefs($Arm) {
    $table = @{}
    # pushSmoothTarget 0: the fixed arms never inherit a Smoothness Mode chosen earlier in this folder (Package 9 integration
    # review); the sm-mode-* arms set it through VEYRA_EXP_RP_SMOOTH_TARGET.
    if ([string]$Arm.set -eq 'smoothness') { $table['fgAdaptive'] = $false; $table['pacingRepair'] = $false; $table['pushSmoothTarget'] = 0 }
    # the capture session (stream capture): automatic buffering unless the arm names it; Smoothness Mode, the adaptive multiplier
    # and pacing repair off, so no arm inherits them from the shared capture settings folder
    if ([string]$Arm.set -eq 'capture') { $table['captureBufferMode'] = 0; $table['fgAdaptive'] = $false; $table['pacingRepair'] = $false; $table['pushSmoothTarget'] = 0 }
    if (-not $Arm.switches) { return $table }
    foreach ($pair in ($Arm.switches -split ';')) {
        if ($pair -match '^pref:(pushAutoFullscreen)=(true|false)$') { $table[$Matches[1]] = ($Matches[2] -eq 'true') }
        elseif ($pair -match '^pref:(fgAdaptive|pacingRepair)=(true|false)$') { $table[$Matches[1]] = ($Matches[2] -eq 'true') }
        elseif ($pair -match '^pref:vrr=(true|false)$') { $table['presentationVrr'] = ($Matches[1] -eq 'true') }
        elseif ($pair -match '^pref:captureBufferMode=([012])$') { $table['captureBufferMode'] = [int]$Matches[1] }
        elseif ($pair.StartsWith('pref:')) { throw "arms.csv: arm '$($Arm.name)': only pref:pushAutoFullscreen, pref:vrr, pref:fgAdaptive or pref:pacingRepair (=true|false), or pref:captureBufferMode (=0|1|2), is allowed" }
    }
    return $table
}

function Clear-KitSwitches {
    $names = @()
    foreach ($item in Get-ChildItem Env:) {
        if (($item.Name -like 'VEYRA_EXP_RP_*') -or ($item.Name -like 'VEYRA_TEST_RP_*') -or ($item.Name -like 'VEYRA_EXP_PRESENT_*') -or ($item.Name -like 'VEYRA_EXP_SR_*') -or ($item.Name -like 'VEYRA_EXP_PIPE_*')) { $names += $item.Name }
    }
    foreach ($name in $names) { Remove-Item -Path ('Env:' + $name) -ErrorAction SilentlyContinue }
    return $names
}

function Get-LogSizes([string]$LogFolder) {
    $sizes = @{}
    if (Test-Path -LiteralPath $LogFolder) {
        foreach ($file in Get-ChildItem -LiteralPath $LogFolder -Filter '*.log') { $sizes[$file.FullName] = $file.Length }
    }
    return $sizes
}

# The text every *.log file gained since Get-LogSizes (a file that shrank was rotated: read all of it).
function Read-NewLogLines([string]$LogFolder, $Before) {
    $text = New-Object System.Text.StringBuilder
    if (-not (Test-Path -LiteralPath $LogFolder)) { return @() }
    foreach ($file in Get-ChildItem -LiteralPath $LogFolder -Filter '*.log' | Sort-Object LastWriteTime) {
        $from = 0
        if ($Before.ContainsKey($file.FullName) -and ($file.Length -ge $Before[$file.FullName])) { $from = $Before[$file.FullName] }
        if ($file.Length -le $from) { continue }
        $stream = [System.IO.File]::Open($file.FullName, 'Open', 'Read', 'ReadWrite')
        try {
            [void]$stream.Seek($from, 'Begin')
            $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
            [void]$text.Append($reader.ReadToEnd())
            [void]$text.Append("`n")
        } finally { $stream.Dispose() }
    }
    return ($text.ToString() -split "`r?`n")
}

function Count-Presses([string[]]$Lines) {
    $count = 0
    foreach ($line in $Lines) {
        if ($line -match '\[push-latency\] trials answerMs=\[([^\]]*)\]') {
            foreach ($value in ($Matches[1] -split ',')) { if ($value.Trim()) { $count++ } }
        } elseif ($line -match '\[push-latency\] done answered=(\d+)/') {
            $script:summaryPresses += [int]$Matches[1]
        }
    }
    return $count
}

function Find-RunningCopy([string]$Exe) {
    $leaf = [System.IO.Path]::GetFileNameWithoutExtension($Exe)
    foreach ($p in @(Get-Process -Name $leaf -ErrorAction SilentlyContinue)) {
        $path = $null
        try { $path = $p.Path } catch { $path = $null }
        if ($path -and ($path -ieq $Exe)) { return $p }
    }
    return $null
}

function Wait-NoPushFps([string]$Exe) {
    $leaf = [System.IO.Path]::GetFileNameWithoutExtension($Exe)
    $told = $false
    while (@(Get-Process -Name $leaf -ErrorAction SilentlyContinue).Count -gt 0) {
        if (-not $told) { Write-Host "Close every running $leaf window first. Waiting ..." -ForegroundColor Yellow; $told = $true }
        Start-Sleep -Seconds 2
    }
}

# One line of nvidia-smi values for the first GPU ('' when it fails). $gpuFields is chosen once at start.
function Get-GpuSample {
    if (-not $nvidiaSmi) { return '' }
    $ErrorActionPreference = 'Continue'
    $out = $null
    try { $out = & $nvidiaSmi ('--query-gpu=' + $gpuFields) '--format=csv,noheader,nounits' 2>$null } catch { $out = $null }
    if ($LASTEXITCODE -ne 0) { return '' }
    $first = [string](@($out)[0])
    if (-not $first) { return '' }
    return ($first -replace '\s', '')
}

# GPU load and video memory of ONE process, from Windows' own GPU counters (Windows 10 1709 and later, any
# GPU maker): '3D %,video decode %,dedicated MB,shared MB'. Empty cells when Windows has no counters for it.
# A query that once took longer than 400 ms is asked only every fifth time from then on.
$appGpuColumns = 'appGpu3dPercent,appGpuDecodePercent,appGpuDedicatedMb,appGpuSharedMb'
$script:appGpuAvailable = $true
$script:appGpuEvery = 2
function Get-AppGpuSample([int]$ProcessId) {
    $engine3d = ''; $decode = ''; $dedicated = ''; $shared = ''
    if (-not $script:appGpuAvailable) { return ',,,' }
    $prefix = 'pid_' + $ProcessId + '_'
    $filter = "Name LIKE '" + $prefix + "%'"
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    try {
        $sum3d = 0.0; $sumDecode = 0.0; $seen = $false
        foreach ($item in @(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine -Filter $filter -ErrorAction Stop)) {
            $name = [string]$item.Name
            if (-not $name.StartsWith($prefix)) { continue }   # '_' in LIKE matches any character
            $seen = $true
            if ($name -like '*engtype_3D*') { $sum3d += [double]$item.UtilizationPercentage }
            elseif ($name -like '*engtype_VideoDecode*') { $sumDecode += [double]$item.UtilizationPercentage }
        }
        if ($seen) { $engine3d = [string][math]::Round($sum3d, 1); $decode = [string][math]::Round($sumDecode, 1) }
        $sumDedicated = 0.0; $sumShared = 0.0; $seen = $false
        foreach ($item in @(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory -Filter $filter -ErrorAction Stop)) {
            $name = [string]$item.Name
            if (-not $name.StartsWith($prefix)) { continue }
            $seen = $true
            $sumDedicated += [double]$item.DedicatedUsage
            $sumShared += [double]$item.SharedUsage
        }
        if ($seen) { $dedicated = [string][math]::Round($sumDedicated / 1MB, 1); $shared = [string][math]::Round($sumShared / 1MB, 1) }
    } catch {
        # The counter classes do not exist on this Windows: stop asking.
        $script:appGpuAvailable = $false
    }
    if ($watch.ElapsedMilliseconds -gt 400) { $script:appGpuEvery = 5 }
    return ($engine3d + ',' + $decode + ',' + $dedicated + ',' + $shared)
}

# Writes an object as JSON through the same masking as the log lines, so the final check finds nothing in it.
function Save-Json($Object, [string]$Path, [int]$Depth = 6) {
    $safe = @()
    foreach ($line in (($Object | ConvertTo-Json -Depth $Depth) -split "`r?`n")) { $safe += (Hide-OwnWords (Protect-Text $line)) }
    Set-Content -LiteralPath $Path -Value $safe -Encoding UTF8
}

function Read-KeyIfAny {
    try {
        if ([Console]::KeyAvailable) { return [Console]::ReadKey($true) }
    } catch { return $null }
    return $null
}

# --- guided steps (impairment, reliability) -----------------------------------------------------------
# watch: seconds the kit keeps watching after Enter (0 = the step is only a time marker). dryAt: where the step
# sits on the dry run's made-up timeline. action: '' | 'kill' (end PushFPS abruptly and start it again) |
# 'auto' (starts by itself). question: asked when the step ends; the answer becomes a marker.
function New-Step([string]$Name, [string]$Prompt, [int]$Watch, [int]$DryAt, [string]$Action, [string]$Question) {
    return [pscustomobject]@{ name = $Name; prompt = $Prompt; watch = $Watch; dryAt = $DryAt; action = $Action; question = $Question }
}

function Get-Steps([string]$Purpose, [bool]$Resume = $false) {
    $list = @()
    if ($Purpose -eq 'impairment') {
        $list += New-Step 'cable-out-1s' 'Connect to the PS5 and wait until the game picture is steady. Then press Enter HERE and at once pull the network cable out of the PC for about 1 second and put it back.' 0 0 '' ''
        $list += New-Step 'cable-out-5s' 'Wait until the picture is back to normal. Then press Enter and pull the cable for about 5 seconds.' 0 0 '' ''
        $list += New-Step 'second-device-load-start' 'Wait until the picture is back to normal. Then press Enter and start heavy traffic on a second device on the same network (a 4K video or a large download) for one minute.' 0 0 '' ''
        $list += New-Step 'second-device-load-end' 'Stop the traffic on the second device, press Enter, then close PushFPS.' 0 0 '' ''
    } elseif ($Purpose -eq 'reliability') {
        $list += New-Step 'cable-out-1s' 'Connect to the PS5 and wait until the game picture has been steady for half a minute. Then press Enter HERE and at once pull the network cable out of the PC for about 1 second and put it back.' 30 40 '' ''
        $list += New-Step 'cable-out-5s' 'Press Enter and at once pull the network cable for about 5 seconds, then put it back.' 75 80 '' ''
        $list += New-Step 'display-mode-change' 'Press Enter, then change the refresh rate (or the resolution) of the screen PushFPS is on in Windows Settings > System > Display > Advanced display, wait 5 seconds and change it back.' 60 190 '' ''
        $list += New-Step 'display-reconnect' 'Press Enter, then pull the monitor cable out of the PC (or the monitor), wait 5 seconds and put it back. The kit beeps when this step is over.' 60 260 '' ''
        $list += New-Step 'focus-loss-keys-held' 'Keyboard and mouse mode only. Press Enter HERE; then, within 10 seconds, click into the game picture, hold W and the left mouse button, press Alt+Tab while holding them, release them in the other window, wait 5 seconds and come back to this window.' 30 330 '' 'Did the character stop moving and firing when PushFPS lost the focus (y/n)?'
        $list += New-Step 'crash-recovery' 'Press Enter: the kit ends PushFPS abruptly, as a crash would, and starts it again. Then connect to the PS5 again (the console can keep the old session for about 20 seconds: connect again if it refuses).' 120 370 'kill' ''
    } elseif ($Purpose -eq 'gpuloss') {
        if ($Resume) {
            # VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1: connecting by hand during the wait CANCELS the resume (reason=other-source).
            $list += New-Step 'gpu-device-loss' 'Connect to the PS5. About 15 seconds after the picture appears PushFPS removes its own graphics device (only PushFPS; nothing else on the PC). Then WAIT AND TOUCH NOTHING: PushFPS reconnects by itself; connecting by hand would cancel that. With this test the device fails again in every new session, so PushFPS tries up to three times and then stops: that end is expected. The step ends by itself.' 150 0 'auto' ''
        } else {
            $list += New-Step 'gpu-device-loss' 'Connect to the PS5. About 15 seconds after the picture appears PushFPS removes its own graphics device once (only PushFPS; nothing else on the PC). When it says so, connect to the PS5 again and leave it. The step ends by itself.' 150 0 'auto' ''
        }
    }
    return $list
}

function Add-Marker([string]$Path, [datetime]$UtcTime, [string]$Name) {
    Add-Content -LiteralPath $Path -Value ($UtcTime.ToString('yyyy-MM-ddTHH:mm:ss.fffZ') + ',' + $Name) -Encoding ASCII
}

# --- pass / fail lines of the reliability checks ----------------------------------------------------------
# The rules are in docs/RELIABILITY.md, section 4. tools/remoteplay_latency_bench/reliability.py judges the same
# evidence again by the same rules; the report shows both. Times are compared as text: every log line and every
# marker starts with yyyy-MM-ddTHH:mm:ss.fffZ, which sorts as text.
function Get-Stamp([string]$Line) {
    if (($Line.Length -ge 24) -and ($Line -match '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z')) { return $Line.Substring(0, 24) }
    return ''
}

function ConvertTo-Time([string]$Stamp) {
    return [datetime]::ParseExact($Stamp.Substring(0, 23), 'yyyy-MM-ddTHH:mm:ss.fff', [System.Globalization.CultureInfo]::InvariantCulture)
}

function Add-StampSeconds([string]$Stamp, [double]$Seconds) {
    return (ConvertTo-Time $Stamp).AddSeconds($Seconds).ToString('yyyy-MM-ddTHH:mm:ss.fff') + 'Z'
}

function Get-StampSeconds([string]$From, [string]$To) {
    return [math]::Round(((ConvertTo-Time $To) - (ConvertTo-Time $From)).TotalSeconds, 0)
}

function Test-InWindow([string]$Line, [string]$From, [string]$To) {
    $stamp = Get-Stamp $Line
    if (-not $stamp) { return $false }
    return (([string]::CompareOrdinal($stamp, $From) -ge 0) -and ([string]::CompareOrdinal($stamp, $To) -lt 0))
}

# The number after Name= in a log line, or $null.
function Get-LogNumber([string]$Line, [string]$Name) {
    if ($Line -match ('(?<![A-Za-z])' + $Name + '=(-?[0-9]+(\.[0-9]+)?)')) {
        return [double]::Parse($Matches[1], [System.Globalization.CultureInfo]::InvariantCulture)
    }
    return $null
}

# First line of $Lines inside [From, To) that matches $Pattern: its time stamp, or ''.
function Find-FirstStamp([string[]]$Lines, [string]$Pattern, [string]$From, [string]$To) {
    foreach ($line in $Lines) {
        if (($line -match $Pattern) -and (Test-InWindow $line $From $To)) { return (Get-Stamp $line) }
    }
    return ''
}

# Median decoded rate in the 30 s before $Start; -1 when there was no steady stream (fewer than 3 lines, or below 5).
function Get-NormalRate([string[]]$Lines, [string]$Start) {
    $from = Add-StampSeconds $Start -30
    $values = @()
    foreach ($line in $Lines) {
        if (($line -match '\] \[remoteplay-progress\] ') -and (Test-InWindow $line $from $Start)) {
            $fps = Get-LogNumber $line 'decodeFps'
            if ($null -ne $fps) { $values += $fps }
        }
    }
    if ($values.Count -lt 3) { return -1 }
    $sorted = @($values | Sort-Object)
    $middle = [int][math]::Floor($sorted.Count / 2)
    $median = $sorted[$middle]
    if (($sorted.Count % 2) -eq 0) { $median = ($sorted[$middle - 1] + $sorted[$middle]) / 2 }
    if ($median -lt 5) { return -1 }
    return $median
}

$fatalPattern = '\] \[crash\] .*unhandled exception|\] \[engine-hang\] owner=|\] \[remoteplay-settings\] .*slipstream\.ini'

function Get-StreamVerdict([string[]]$Lines, [string]$Start, [string]$End, [double]$Limit) {
    $normal = Get-NormalRate $Lines $Start
    if ($normal -lt 0) { return @('NOT-EXERCISED', 'no steady stream in the 30 s before the step') }
    $lastBad = ''
    foreach ($line in $Lines) {
        if (-not (Test-InWindow $line $Start $End)) { continue }
        $bad = $false
        if ($line -match '\] \[remoteplay-progress\] ') {
            $fps = Get-LogNumber $line 'decodeFps'
            if (($null -ne $fps) -and ($fps -lt (0.9 * $normal))) { $bad = $true }
        } elseif (($line -match '\] \[remoteplay-recovery\] ') -and ($line -notmatch 'retry allowance renewed')) { $bad = $true }
        if ($bad) {
            $stamp = Get-Stamp $line
            if ([string]::CompareOrdinal($stamp, $lastBad) -gt 0) { $lastBad = $stamp }
        }
    }
    if (-not $lastBad) { return @('NOT-EXERCISED', 'the log shows no disturbance of the stream: was the cable pulled?') }
    if (Find-FirstStamp $Lines '\] \[remoteplay-recovery\] .*recovery stopped' $Start $End) { return @('FAIL', 'the program gave up reconnecting') }
    if (Find-FirstStamp $Lines $fatalPattern $Start $End) { return @('FAIL', 'a crash or engine hang line, or the program started again') }
    $back = ''
    foreach ($line in $Lines) {
        if ($back) { break }
        if (($line -match '\] \[remoteplay-progress\] ') -and (Test-InWindow $line $Start $End)) {
            $stamp = Get-Stamp $line
            $fps = Get-LogNumber $line 'decodeFps'
            if (([string]::CompareOrdinal($stamp, $lastBad) -gt 0) -and ($null -ne $fps) -and ($fps -ge (0.9 * $normal))) { $back = $stamp }
        }
    }
    if (-not $back) { return @('FAIL', 'the stream was not back when the step ended') }
    $seconds = Get-StampSeconds $Start $back
    $detail = 'back after ' + $seconds + ' s (limit ' + $Limit + ' s)'
    if ($seconds -le $Limit) { return @('PASS', $detail) }
    return @('FAIL', $detail)
}

function Get-DisplayVerdict([string[]]$Lines, [string]$Start, [string]$End) {
    if (-not (Find-FirstStamp $Lines '\] \[display-refresh\] |\] \[display-query\] invalidate' $Start $End)) { return @('NOT-EXERCISED', 'the log shows no display change in the step') }
    if (Find-FirstStamp $Lines $fatalPattern $Start $End) { return @('FAIL', 'a crash or engine hang line, or the program started again') }
    if (Find-FirstStamp $Lines '\] \[gpu-failure\] operation=' $Start $End) { return @('FAIL', 'a GPU failure line') }
    $normal = Get-NormalRate $Lines $Start
    $lastFps = $null
    $lastPresented = $null
    foreach ($line in $Lines) {
        if (-not (Test-InWindow $line $Start $End)) { continue }
        if ($line -match '\] \[remoteplay-progress\] ') {
            $value = Get-LogNumber $line 'decodeFps'
            if ($null -ne $value) { $lastFps = $value }
        } elseif ($line -match '\] \[frame-rate\] ') {
            $value = Get-LogNumber $line 'presentSubmitFps'
            if ($null -ne $value) { $lastPresented = $value }
        }
    }
    if (($normal -ge 0) -and (($null -eq $lastFps) -or ($lastFps -lt (0.9 * $normal)))) { return @('FAIL', 'the stream was not back when the step ended') }
    if (($null -ne $lastPresented) -and ($lastPresented -lt 1)) { return @('FAIL', 'nothing was being presented when the step ended') }
    if ($normal -ge 0) { return @('PASS', 'the program kept running and the stream was at its rate again') }
    return @('PASS', 'the program kept running; no stream was running, so only survival was checked')
}

function Get-FocusVerdict([string[]]$Lines, [string]$Start, [string]$End, [string]$Answer) {
    if (-not (Find-FirstStamp $Lines '\] \[remoteplay-kbm\] keyboard and mouse captured' '0000' $End)) { return @('SKIPPED', 'keyboard and mouse mode was not on in this run (kit parameter -ReliabilityKbm)') }
    $released = Find-FirstStamp $Lines '\] \[remoteplay-kbm\] keyboard and mouse released \(focus lost\)' $Start $End
    if ($released -and ($Answer -eq 'no')) { return @('FAIL', 'the log shows every key released on focus loss, but you reported input still held on the console') }
    if ($released) { return @('PASS', 'every key was released when the window lost focus') }
    if ($Answer -eq 'no') { return @('FAIL', 'no release on focus loss in the log, and you reported input still held') }
    return @('NOT-EXERCISED', 'the log shows no loss of focus while keyboard and mouse were captured')
}

function Get-CrashRecoveryVerdict([string[]]$Lines, [string]$Start, [string]$End) {
    $again = Find-FirstStamp $Lines '\] \[remoteplay-settings\] .*slipstream\.ini' $Start $End
    if (-not $again) { return @('FAIL', 'PushFPS did not start again after it was ended') }
    if (Find-FirstStamp $Lines '\] \[crash\] .*unhandled exception' $again $End) { return @('FAIL', 'a crash line after the new start') }
    $session = Find-FirstStamp $Lines '\] \[remoteplay\] session started' $again $End
    if (-not $session) { return @('NOT-EXERCISED', 'PushFPS started again; no connection to the console was made in the step') }
    foreach ($line in $Lines) {
        if (($line -match '\] \[remoteplay-progress\] ') -and (Test-InWindow $line $session $End)) {
            $fps = Get-LogNumber $line 'decodeFps'
            if (($null -ne $fps) -and ($fps -ge 5)) { return @('PASS', ('started again ' + (Get-StampSeconds $Start $again) + ' s after it was ended, pictures decoded ' + (Get-StampSeconds $Start (Get-Stamp $line)) + ' s after')) }
        }
    }
    return @('FAIL', 'PushFPS started again and connected, but no pictures were decoded in the step')
}

$resumeReasonText = @{
    'budget' = 'it stopped after its allowed attempts (budget) without a picture'
    'other-source' = 'a connection started by hand during the wait cancelled the automatic resume (wait and touch nothing next time)'
    'user-stop' = 'the stream was stopped by hand'; 'app-exit' = 'PushFPS was closed during the wait'
    'device-unavailable' = 'no usable graphics device came back'; 'engine-error' = 'an engine error'; 'session-ended' = 'the session had ended'
    'recovery-running' = 'another recovery was running'; 'not-remote' = 'the source was not a PS5 stream'; 'no-description' = 'the stream could not be described again'
}

function Get-GpuLossVerdict([string[]]$Lines, [string]$Start, [string]$End) {
    $lost = Find-FirstStamp $Lines '\] \[gpu-failure\] operation=|\] \[engine-test\] .*GPU' $Start $End
    if (-not $lost) { return @('NOT-EXERCISED', 'the log shows no GPU device loss in the step') }
    if (Find-FirstStamp $Lines '\] \[crash\] .*unhandled exception' $lost $End) { return @('FAIL', 'a crash line after the device was lost') }
    if (Find-FirstStamp $Lines '\] \[remoteplay-settings\] .*slipstream\.ini' $lost $End) { return @('FAIL', 'the program ended and started again after the device was lost') }
    # Resume after device loss (VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1): reliability.py gpu_loss_check, the same rule. With the
    # test hook attempts 1 to 3 and then reason=budget is the expected end.
    $started = @(); $reasons = @()
    foreach ($line in $Lines) {
        if (-not (Test-InWindow $line $lost $End)) { continue }
        if ($line -match '\] \[remoteplay-recovery\] device-loss resume attempt=(\d+) started') { $started += [pscustomobject]@{ at = (Get-Stamp $line); n = $Matches[1] } }
        elseif ($line -match '\] \[remoteplay-recovery\] device-loss not resumed reason=([a-z-]+)') { $reasons += $Matches[1] }
    }
    if (($started.Count -gt 0) -or ($reasons.Count -gt 0)) {
        foreach ($item in $started) {
            $after = Add-StampSeconds $item.at 2
            foreach ($line in $Lines) {
                if (($line -match '\] \[frame-rate\] ') -and (Test-InWindow $line $after $End)) {
                    $value = Get-LogNumber $line 'presentSubmitFps'
                    if (($null -ne $value) -and ($value -ge 1)) {
                        $tail = ''
                        if ($reasons -contains 'budget') { $tail = ' Later attempts stopped at the budget, the expected end with the test hook.' }
                        return @('PASS', ('PushFPS resumed by itself (attempt ' + $item.n + '); picture back ' + (Get-StampSeconds $lost (Get-Stamp $line)) + ' s after the device was lost.' + $tail))
                    }
                }
            }
        }
        $reason = 'none logged'
        if ($reasons.Count -gt 0) { $reason = $reasons[$reasons.Count - 1] }
        $text = 'reason ' + $reason
        if ($resumeReasonText.ContainsKey($reason)) { $text = $resumeReasonText[$reason] }
        return @('FAIL', ('PushFPS did not resume with a picture: ' + $text))
    }
    $from = Add-StampSeconds $lost 2
    $anyFrameRate = $false
    foreach ($line in $Lines) { if ($line -match '\] \[frame-rate\] ') { $anyFrameRate = $true; break } }
    $pattern = '\] \[frame-rate\] '
    $field = 'presentSubmitFps'
    $least = 1
    if (-not $anyFrameRate) { $pattern = '\] \[remoteplay-progress\] '; $field = 'decodeFps'; $least = 5 }
    foreach ($line in $Lines) {
        if (($line -match $pattern) -and (Test-InWindow $line $from $End)) {
            $value = Get-LogNumber $line $field
            if (($null -ne $value) -and ($value -ge $least)) {
                $how = 'after a reconnect by hand: the program does not reconnect a PS5 session by itself'
                if (Find-FirstStamp $Lines '\] \[gpu-reconnect\] .*automatic=true' $lost $End) { $how = 'reconnected by the program' }
                return @('PASS', ('picture back ' + (Get-StampSeconds $lost (Get-Stamp $line)) + ' s after the device was lost (' + $how + ')'))
            }
        }
    }
    return @('FAIL', 'the program survived the device loss, but no picture came back in the step (a PS5 session is not reconnected automatically)')
}

function Get-SafeStartVerdict([string[]]$Lines, [bool]$LimitReached, [bool]$Forced, [int]$Seconds) {
    $started = $false; $broken = $false; $experiment = $false
    foreach ($line in $Lines) {
        if ($line -match '\] \[remoteplay-settings\] .*slipstream\.ini') { $started = $true }
        if ($line -match '\] \[crash\] .*unhandled exception|\] \[engine-hang\] owner=') { $broken = $true }
        if ($line -match '\] \[remoteplay-(lan|kbm|cadence|timing)\] .*experiment') { $experiment = $true }
    }
    if (-not $started) { return @('FAIL', 'no start line of the program in the log') }
    if ($broken) { return @('FAIL', 'a crash or engine hang line') }
    if ($experiment) { return @('FAIL', 'an experiment was active in a start that should have none') }
    if ($Forced) { return @('FAIL', 'the program did not close when asked') }
    if (-not $LimitReached) { return @('FAIL', 'the program ended before the kit asked it to close') }
    return @('PASS', ('started without slipstream.ini and without switches, stayed up ' + $Seconds + ' s, closed when asked'))
}

# Judges every finished step of a run, prints one line each and writes reliability.csv (check,result,detail).
function Write-ReliabilityVerdicts([string[]]$Lines, [string]$RunFolder, [string]$Purpose, [bool]$LimitReached, [bool]$Forced, [int]$Seconds) {
    $marks = @{}
    $markerPath = Join-Path $RunFolder 'markers.csv'
    if (Test-Path -LiteralPath $markerPath) {
        foreach ($row in @(Get-Content -LiteralPath $markerPath | Select-Object -Skip 1)) {
            $at = $row.IndexOf(',')
            if ($at -lt 1) { continue }
            $name = $row.Substring($at + 1).Trim()
            if (-not $marks.ContainsKey($name)) { $marks[$name] = $row.Substring(0, $at).Trim() }
        }
    }
    $results = @()
    if ($Purpose -eq 'safestart') {
        $verdict = Get-SafeStartVerdict $Lines $LimitReached $Forced $Seconds
        $results += [pscustomobject]@{ check = 'safe-start'; result = $verdict[0]; detail = $verdict[1] }
    } else {
        foreach ($check in @('cable-out-1s', 'cable-out-5s', 'display-mode-change', 'display-reconnect', 'focus-loss-keys-held', 'crash-recovery', 'gpu-device-loss')) {
            if (-not $marks.ContainsKey($check)) { continue }
            $verdict = @('NOT-EXERCISED', 'the step was not finished (no end marker)')
            if ($marks.ContainsKey($check + '-end')) {
                $from = $marks[$check]; $to = $marks[$check + '-end']
                $answer = ''
                if ($marks.ContainsKey($check + '-answer-yes')) { $answer = 'yes' } elseif ($marks.ContainsKey($check + '-answer-no')) { $answer = 'no' }
                if ($check -eq 'cable-out-1s') { $verdict = Get-StreamVerdict $Lines $from $to 10 }
                elseif ($check -eq 'cable-out-5s') { $verdict = Get-StreamVerdict $Lines $from $to 60 }
                elseif (($check -eq 'display-mode-change') -or ($check -eq 'display-reconnect')) { $verdict = Get-DisplayVerdict $Lines $from $to }
                elseif ($check -eq 'focus-loss-keys-held') { $verdict = Get-FocusVerdict $Lines $from $to $answer }
                elseif ($check -eq 'crash-recovery') { $verdict = Get-CrashRecoveryVerdict $Lines $from $to }
                elseif ($check -eq 'gpu-device-loss') { $verdict = Get-GpuLossVerdict $Lines $from $to }
            }
            $results += [pscustomobject]@{ check = $check; result = $verdict[0]; detail = $verdict[1] }
        }
    }
    $rows = @('check,result,detail')
    Write-Host ''
    foreach ($item in $results) {
        $colour = 'Yellow'
        if ($item.result -eq 'PASS') { $colour = 'Green' } elseif ($item.result -eq 'FAIL') { $colour = 'Red' }
        Write-Host ('  {0,-14} {1,-22} {2}' -f $item.result, $item.check, $item.detail) -ForegroundColor $colour
        $rows += ($item.check + ',' + $item.result + ',"' + ($item.detail -replace '"', '') + '"')
    }
    Set-Content -LiteralPath (Join-Path $RunFolder 'reliability.csv') -Value $rows -Encoding ASCII
}

# PresentMon, started with administrator rights (one Windows prompt) unless the kit already has them. $null when
# it did not start (the prompt declined, or another error, which is printed).
# PresentMon's option spelled as its own --help prints it ('--name' in 2.x, '-name' in 1.x); '--name' when unknown.
$script:pmOptions = @{}
function Get-PmOption([string]$Name) {
    if ($script:pmOptions.ContainsKey($Name)) { return $script:pmOptions[$Name] }
    return ('--' + $Name)
}

# Reads the help text of PresentMon once and keeps the spelling of every option the kit uses. '' when all the needed
# ones are there, else the names that are missing.
function Read-PresentMonOptions([string]$Path) {
    $ErrorActionPreference = 'Continue'
    $text = ''
    try { $text = (& $Path '--help' 2>&1 | Out-String) } catch { $text = '' }
    if (-not $text) { try { $text = (& $Path '-?' 2>&1 | Out-String) } catch { $text = '' } }
    # No help text at all (it could not be started from here): the 2.x spelling is used and the capture shows the rest.
    if (-not $text) { return '' }
    $missing = @()
    foreach ($name in @('process_name', 'output_file', 'no_console_stats', 'terminate_on_proc_exit', 'session_name', 'stop_existing_session', 'terminate_existing_session', 'timed', 'terminate_after_timed', 'track_gpu_video')) {
        if ($text -match ('(?m)(^|\s)(--?)' + $name + '\b')) { $script:pmOptions[$name] = $Matches[2] + $name }
        elseif (@('process_name', 'output_file', 'session_name', 'terminate_on_proc_exit') -contains $name) { $missing += $name }
    }
    return ($missing -join ', ')
}

function Start-PresentMon([string]$Arguments) {
    try {
        if ($elevated -or $script:perfLogUser) { return (Start-Process -FilePath $presentMonExe -ArgumentList $Arguments -WindowStyle Hidden -PassThru) }
        return (Start-Process -FilePath $presentMonExe -ArgumentList $Arguments -WindowStyle Hidden -PassThru -Verb RunAs)
    } catch {
        Write-Host ('  PresentMon did not start (' + $_.Exception.Message + '): this run has no frame times from it.') -ForegroundColor Yellow
        return $null
    }
}

# --- test profile, press plan and automation (sprint 4) ------------------------------------------------------------
# The test profile: its own settings folder per build (--data-dir; the user's saved settings are never read or changed;
# the console pairing lives in %LOCALAPPDATA%\Veyra\remoteplay and is shared by every copy). Only the presentation
# keys below are written; every other setting of that folder is kept between runs.
function Write-TestProfile([string]$ExeKind, [bool]$AutoResume, $Extra) {
    $root = Join-Path $env:LOCALAPPDATA 'slipstream-kit'
    if ($DryRun) { $root = Join-Path $evidence '_dryrun' }
    $folder = Join-Path $root ('profile-' + $ExeKind)
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    $file = Join-Path $folder 'qml-preferences.v1.json'
    $prefs = $null
    if (Test-Path -LiteralPath $file) { try { $prefs = [System.IO.File]::ReadAllText($file) | ConvertFrom-Json } catch { $prefs = $null } }
    if (-not $prefs) { $prefs = New-Object PSObject }
    # Super resolution and enhancement are NOT written here and stay "assumed from the test profile" in the verdicts: they
    # are not preferences of qml-preferences.v1.json but nodes of the effect chain (QmlPlayerBridge::srEnabled / nrEnabled
    # read impl_->chain), saved in chain-session.v1 by ChainSessionStore (PresetLibrary.cpp): a versioned format
    # ("VEYRA_CHAIN_SESSION <n>", file renamed per version: .dlaa, .output-controls, ...) whose version differs between the
    # original, stable and experimental builds, and a file a build cannot decode is marked corrupt and never overwritten.
    # The kit cannot write it safely for all three builds; the experimental build logs sr= and enhance= itself.
    # Low-latency queue on for every GPU maker: on an NVIDIA main GPU the queue follows the NVIDIA low-latency choice
    # (PresentationSettings.h syncNvidiaLowLatencyQueue, the same in the original build), so that choice is On too; on
    # AMD and Intel it is kept unused. Every build then runs with the queue on (review of 16a2bfec, blocker 1).
    $presentation = [pscustomobject]@{ lowQueue = $true; nvidiaLowLatency = 1; display = 1; outputRate = 1; customFps = 60; vrr = $true }
    # an arm's pref:vrr=false (smoothness session): the app is asked for no variable refresh; Windows settings are never changed
    if ($Extra -and $Extra.ContainsKey('presentationVrr')) { $presentation.vrr = [bool]$Extra['presentationVrr'] }
    $values = [ordered]@{ presentation = $presentation; pushSmoothPacing = $true; pushSmoothPacing2 = $false; autoResume = $AutoResume; pushAutoFullscreen = $true; introAnimation = $false; introMusic = $false }
    if ($Extra) { foreach ($key in $Extra.Keys) { if ($key -ne 'presentationVrr') { $values[$key] = $Extra[$key] } } }
    foreach ($key in $values.Keys) { $prefs | Add-Member -NotePropertyName $key -NotePropertyValue $values[$key] -Force }
    # Qt reads JSON without a byte order mark.
    [System.IO.File]::WriteAllText($file, ($prefs | ConvertTo-Json -Depth 32), (New-Object System.Text.UTF8Encoding($false)))
    return $folder
}

# Latency tests per start: what -TestsPerStart says, else enough tests of $Trials presses for -PressesPerArm over -Rounds.
function Get-TestsPerStart([int]$Trials) {
    if ($testsPerStartGiven) { return $TestsPerStart }
    if ($Trials -lt 1) { $Trials = 12 }
    $rounds = [math]::Max(1, $Rounds)
    return [int][math]::Max(1, [math]::Ceiling($PressesPerArm / ($rounds * $Trials)))
}

function Get-AutorunPlan([string]$Purpose, [int]$Trials, [int]$Tests) {
    if ($Purpose -eq 'quality') { return 'settle:8;sr:15;fg:15;quality;exit' }
    if ($Purpose -eq 'probe') { return 'settle:8;wait:180;exit' }
    if ($Purpose -eq 'network') { return ('settle:8;wait:' + ([math]::Max(1, $Minutes) * 60) + ';exit') }
    if ($Purpose -eq 'measure') { return $script:measurePlan }
    if ($AutorunPlan) { return $AutorunPlan }
    $steps = @('settle:8')
    for ($i = 0; $i -lt $Tests; $i++) { $steps += ('latency:' + $Trials) }
    $steps += 'exit'
    return ($steps -join ';')
}

function Write-ManualSteps([string]$Purpose, [int]$Trials, [int]$Tests, [bool]$Profiled) {
    if ($Purpose -eq 'latency') {
        if ($Profiled) { Write-Host '  1. Connect to the PS5 (after the first start of this copy the kit''s profile reconnects by itself) and wait in the game, in the same place as in the other runs.' }
        else { Write-Host '  1. Connect to the PS5 and wait in the game, in the same place as in the other runs.' }
        Write-Host ('  2. PS5 streaming dialog -> latency test -> ' + $Trials + ' presses (the most this build accepts). Run it ' + $Tests + ' time(s). Do not touch the controller meanwhile.')
        if ($NoAutoClose -or ($Tests -lt 1)) { Write-Host '  3. Close PushFPS. The kit continues by itself.' }
        else { Write-Host ('  3. Nothing else: after test ' + $Tests + ' the kit closes PushFPS itself. (Or close it yourself; the kit continues.)') }
    } elseif (($Purpose -eq 'quality') -or ($Purpose -eq 'probe')) {
        Write-Host '  Connect to the PS5, wait in the game, and close PushFPS after a few minutes. (The automatic plan did not run.)'
    } elseif ($Purpose -eq 'measure') {
        Write-Host '  This build has no automation: connect to the PS5 if it does not connect by itself, then leave the game running as in the other runs. The kit closes PushFPS itself.' -ForegroundColor Green
    } elseif ($Purpose -eq 'network') {
        Write-Host '  Connect to the PS5 if it does not connect by itself, then PLAY NORMALLY until the kit closes PushFPS.' -ForegroundColor Green
        Write-Host '  The picture must move: a still menu gives a result that only speaks for a still menu.'
    }
}

# --- SmartLink trace (smartlink-trace.csv beside the app log, written with VEYRA_EXP_RP_SMARTLINK_TRACE=1) ------------
$traceName = 'smartlink-trace.csv'
$script:withheldTraces = @()

# True when the file is the app's trace: exactly its header line (SmartLinkTrace.h kHeader), then numbers only (ASCII
# digits; nan / inf allowed), no line longer than kMaxLine (28*22+2 = 618 bytes): report.py trace_problem.
$traceHeader = 'endMs,windowMs,route,packetsReceived,packetsLost,packetsReordered,packetsLate,packetGapMeanMs,packetGapMaxMs,' +
    'framesArrived,framesIncomplete,framesFecRecovered,frameGapMaxMs,frameGapExcessMs,lateRescueMs,reorderWaitMs,' +
    'rttMs,consoleTargetKbps,measuredKbps,requestedKbps,videoQueueDepth,audioBufferedMs,audioUnderruns,' +
    'wifiSignalPercent,wifiRssiDbm,wifiLinkMbps,wifiBandGhz,frameLateMeanMs'
$traceMaxLine = 28 * 22 + 2
function Test-TraceNumbers([string]$Path) {
    # Bytes read one to one (Latin-1), so a byte order mark or any byte outside printable ASCII refuses the file.
    $text = [System.Text.Encoding]::GetEncoding(28591).GetString([System.IO.File]::ReadAllBytes($Path))
    if ($text -cmatch '[^\x20-\x7E\r\n]') { return $false }
    $first = $true
    foreach ($line in ($text -split "\r?\n")) {
        if ($line.Length -gt $traceMaxLine) { return $false }
        if ($first) { $first = $false; if ($line.TrimEnd() -cne $traceHeader) { return $false }; continue }
        if (-not $line.Trim()) { continue }
        foreach ($field in ($line.Trim() -split ',')) {
            $f = $field.Trim()
            if ($f -and ($f -cnotmatch '^-?[0-9]+(\.[0-9]+)?$')) { return $false }
        }
    }
    return (-not $first)
}

# The app starts a new smartlink-trace.csv at every stream start and renames the older ones (s4-net
# RemotePlaySmartLink.h openTrace: .1.csv -> .2.csv, .csv -> .1.csv, a new .csv). A run's traces are every one of these
# files written after the run started, each copied WHOLE (never from a byte offset): the current file, and the rotated
# ones when the stream reconnected inside the run. report.py select_run_traces is the same rule.
# A file that is not the app's exact trace goes to _withheld (never zipped) and is named in withheld.txt.
# Returns none | kept | withheld (withheld when any file was withheld).
$traceFiles = @('smartlink-trace.2.csv', 'smartlink-trace.1.csv', 'smartlink-trace.csv')
function Save-RunTrace([string]$LogFolder, [datetime]$SinceUtc, [string]$RunFolder, [string]$RunName) {
    $state = 'none'
    foreach ($file in $traceFiles) {
        $source = Join-Path $LogFolder $file
        if (-not (Test-Path -LiteralPath $source)) { continue }
        $item = Get-Item -LiteralPath $source
        if ($item.LastWriteTimeUtc -lt $SinceUtc) { continue }
        $target = Join-Path $RunFolder $file
        try {
            $stream = [System.IO.File]::Open($source, 'Open', 'Read', 'ReadWrite')
            try { $out = [System.IO.File]::Create($target); try { $stream.CopyTo($out) } finally { $out.Dispose() } } finally { $stream.Dispose() }
        } catch { continue }
        if (Test-TraceNumbers $target) { if ($state -eq 'none') { $state = 'kept' }; continue }
        $hold = Join-Path $evidence '_withheld'
        New-Item -ItemType Directory -Force -Path $hold | Out-Null
        Move-Item -LiteralPath $target -Destination (Join-Path $hold ($RunName + '_' + $file)) -Force
        $script:withheldTraces += ($RunName + ' ' + $file + ' (not the app trace: other header, a field that is not a number or a line too long)')
        Write-Host ('  ' + $file + ' of this run is not the plain number trace of the app: withheld, not packed.') -ForegroundColor Yellow
        $state = 'withheld'
    }
    return $state
}

# --- one run of one arm ----------------------------------------------------------------------------
# Purpose: latency | camera | soak | impairment. Returns the number of per-press values the run logged.
function Invoke-ArmRun($Arm, [string]$Exe, [string]$Purpose, [int]$LimitSeconds, [string]$Title) {
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $runName = $Arm.name + '@' + $stamp
    if ($Purpose -ne 'latency') { $runName = $Purpose + '-' + $runName }
    $runFolder = Join-Path (Join-Path $evidence 'runs') $runName
    New-Item -ItemType Directory -Force -Path $runFolder | Out-Null
    $exeFolder = Split-Path -Parent $Exe
    $logFolder = Join-Path $exeFolder 'logs'
    $switches = Get-ArmSwitches $Arm
    $stopFile = Join-Path $evidence 'stop.txt'
    $stopAsked = $false
    $stopAskedAt = Get-Date
    $stopReason = 'closed'       # closed (by the user or by itself) | time | kill-switch (key) | kill-switch (stop file)
    $forcedStop = $false
    $controlCAsKey = $false

    Write-Step $Title
    Write-Host $Arm.note
    if (-not $DryRun) { Wait-NoPushFps $Exe }

    [void](Clear-KitSwitches)
    Set-Item -Path 'Env:VEYRA_TEST_RP_NO_SETTINGS_FILE' -Value '1'   # slipstream.ini must not add switches to an arm
    foreach ($name in $switches.Keys) { Set-Item -Path ('Env:' + $name) -Value $switches[$name] }
    # Switches the kit adds itself, the same way in every arm: the network health line (observation only).
    # Latency and camera runs get it only with -Health; every other kind of run always.
    $kitSwitches = @()
    $wantHealth = [bool]$Health
    if (($Purpose -ne 'latency') -and ($Purpose -ne 'camera')) { $wantHealth = $true }
    if ($Purpose -eq 'safestart') { $wantHealth = $false }   # a safe start has no switch at all
    if ($Purpose -eq 'measure') { $wantHealth = [bool]$Health }   # kit 5 sessions: nothing an arm does not name (the builds session compares builds with everything off)
    if ($wantHealth -and ($Arm.exe -ne 'original') -and (-not $switches.ContainsKey('VEYRA_EXP_RP_HEALTH'))) {
        Set-Item -Path 'Env:VEYRA_EXP_RP_HEALTH' -Value '1'
        $kitSwitches += 'VEYRA_EXP_RP_HEALTH=1'
    }
    if (($Purpose -eq 'reliability') -and $ReliabilityKbm -and (-not $switches.ContainsKey('VEYRA_EXP_RP_KBM'))) {
        Set-Item -Path 'Env:VEYRA_EXP_RP_KBM' -Value '1'
        $kitSwitches += 'VEYRA_EXP_RP_KBM=1'
    }
    # stream clarity: -Clip plays the recorded clip in every measured run instead of connecting to the PS5
    # (VEYRA_TEST_RP_CLIP), so the arms of a picture comparison process the same frames. run.json says '<clip>', never the path.
    if ($script:clipPath -and ($Purpose -eq 'measure')) {
        Set-Item -Path 'Env:VEYRA_TEST_RP_CLIP' -Value $script:clipPath
        $kitSwitches += 'VEYRA_TEST_RP_CLIP=<clip>'
    }
    if ($Purpose -eq 'gpuloss') {
        # After 900 frames of a live session PushFPS treats one frame as lost on the GPU and, half a second later,
        # removes its own Direct3D device. Only this process is affected; the variable is removed after the run.
        Set-Item -Path 'Env:VEYRA_TEST_GPU_FAILURE' -Value '900:500'
        $kitSwitches += 'VEYRA_TEST_GPU_FAILURE=900:500'
        if ($ResumeAfterDeviceLoss -and (-not $switches.ContainsKey('VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS'))) {
            Set-Item -Path 'Env:VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS' -Value '1'
            $kitSwitches += 'VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS=1'
        }
    }
    # Presses per test and tests per start for this build; automation in the experimental build; the test profile.
    # The same presses per test in every arm of the session (review of 16a2bfec, blocker 2): 12 when the session crosses
    # builds (the original and the stable build accept at most 12), else what -PressesPerArm and -Rounds need (at most
    # 100), never more than the build says it accepts.
    $trials = $script:sessionTrials
    if ($script:trialsMax.ContainsKey([string]$Arm.exe) -and ([int]$script:trialsMax[[string]$Arm.exe] -lt $trials)) { $trials = [int]$script:trialsMax[[string]$Arm.exe] }
    $testsWanted = Get-TestsPerStart $trials
    $automatic = ($Arm.exe -eq 'experimental') -and (-not $NoAutomation) -and ((@('quality', 'probe', 'network', 'measure') -contains $Purpose) -or (($Purpose -eq 'latency') -and $script:automationAllowed))
    if (($Arm.exe -eq 'previous') -and (-not $NoAutomation) -and ($Purpose -eq 'measure')) { $automatic = $true }   # Package 7's experimental build has the automation too
    $autorunLimitSeconds = [math]::Max($AutorunTimeoutMinutes * 60, 8 + $testsWanted * $trials * 5 + 120)
    if ($Purpose -eq 'network') { $autorunLimitSeconds = [math]::Max(1, $Minutes) * 60 + 180 }
    if ($Purpose -eq 'measure') { $autorunLimitSeconds = [int](Get-PlanSeconds $script:measurePlan) + 180 }
    $plan = ''
    if ($automatic) {
        $plan = Get-AutorunPlan $Purpose $trials $testsWanted
        # the capture session starts the remembered capture card instead of connecting to the PS5 (stream capture)
        $startSwitch = 'VEYRA_TEST_RP_AUTOCONNECT'
        if ([string]$Arm.set -eq 'capture') { $startSwitch = 'VEYRA_TEST_RP_CAPTURE' }
        foreach ($pair in @(('VEYRA_EXP_RP_STATS', '1'), ($startSwitch, '1'), ('VEYRA_TEST_RP_AUTORUN', $plan))) {
            if (-not $switches.ContainsKey($pair[0])) { Set-Item -Path ('Env:' + $pair[0]) -Value $pair[1]; $kitSwitches += ($pair[0] + '=' + $pair[1]) }
        }
    }
    $profiled = $script:useTestProfile -and ($Purpose -ne 'safestart')
    $exeArgs = ''
    # A pipeline arm (kit 5) changes picture settings through the app's own setters, which the app may save: it gets its own
    # settings folder, so the plain test profile of the other sessions never inherits them.
    $profileKind = [string]$Arm.exe
    if ($switches.ContainsKey('VEYRA_TEST_RP_PIPELINE')) { $profileKind += '-pipeline' }
    if ([string]$Arm.set -eq 'smoothness') { $profileKind += '-smoothness' }   # its prefs (vrr, adaptive, repair) stay out of the other sessions' folders
    if ([string]$Arm.set -eq 'capture') { $profileKind = [string]$Arm.exe + '-capture' }   # the folder that remembers the capture card (Invoke-CaptureSetup)
    if ($profiled) { $exeArgs = '--data-dir "' + (Write-TestProfile $profileKind (-not $automatic) (Get-ArmPrefs $Arm)) + '"' }
    $before = Get-LogSizes $logFolder
    $traceSince = (Get-Date).ToUniversalTime().AddSeconds(-2)   # trace files written after this belong to this run
    $live = New-LiveState
    $liveOffsets = $before.Clone()
    $watchLive = (@('latency', 'camera', 'quality', 'probe', 'network', 'measure') -contains $Purpose)
    $lastNews = Get-Date

    # PresentMon first, so that it sees the program start.
    $presentMonCsv = Join-Path $runFolder 'presentmon.csv'
    $presentMonProcess = $null
    $presentMonStart = ''
    $pmBase = (Get-PmOption 'process_name') + ' ' + [System.IO.Path]::GetFileName($Exe) + ' ' + (Get-PmOption 'terminate_on_proc_exit') + ' ' + (Get-PmOption 'stop_existing_session') + ' ' +
        (Get-PmOption 'session_name') + ' ' + $sessionName + ' ' + (Get-PmOption 'no_console_stats')
    if ($script:pmOptions.ContainsKey('track_gpu_video')) { $pmBase += ' ' + (Get-PmOption 'track_gpu_video') }
    if (($Purpose -eq 'soak') -and ($SoakPresentMonSeconds -gt 0)) { $pmBase += ' ' + (Get-PmOption 'timed') + ' ' + $SoakPresentMonSeconds + ' ' + (Get-PmOption 'terminate_after_timed') }
    # A long run records frames in short windows; presentmon-windows.csv says when each began.
    $windowFile = Join-Path $runFolder 'presentmon-windows.csv'
    $windowCount = 1
    $lastWindow = Get-Date
    if ($Purpose -eq 'soak') { Set-Content -LiteralPath $windowFile -Value 'file,startUtc' -Encoding ASCII }
    if ($presentMonExe -and (-not $DryRun)) {
        $pmArgs = $pmBase + ' ' + (Get-PmOption 'output_file') + ' "' + $presentMonCsv + '"'
        try {
            $presentMonStart = Get-UtcStamp
            $presentMonProcess = Start-PresentMon $pmArgs
            if ($Purpose -eq 'soak') { Add-Content -LiteralPath $windowFile -Value ('presentmon.csv,' + $presentMonStart) -Encoding ASCII }
        } catch {
            Write-Host ('PresentMon did not start: ' + $_.Exception.Message) -ForegroundColor Yellow
            $presentMonProcess = $null
        }
    }

    $resumeOn = ($switches.ContainsKey('VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS') -and ($switches['VEYRA_EXP_RP_RESUME_AFTER_DEVICE_LOSS'] -eq '1')) -or (($Purpose -eq 'gpuloss') -and $ResumeAfterDeviceLoss)
    $steps = @(Get-Steps $Purpose $resumeOn)
    $dryArgs = '-NoProfile -Command "Start-Sleep -Seconds 4"'
    if ($steps.Count -gt 0) { $dryArgs = '-NoProfile -Command "Start-Sleep -Seconds 12"' }   # time for every step of the dry run
    $startUtc = Get-UtcStamp
    if ($DryRun) {
        $process = Start-Process -FilePath 'powershell.exe' -ArgumentList $dryArgs -WindowStyle Hidden -PassThru
        Add-DryRunLog $logFolder $Purpose $Arm.name ([string]$Arm.exe) ([string]$Arm.switches)
    } else {
        if ($exeArgs) { $process = Start-Process -FilePath $Exe -ArgumentList $exeArgs -WorkingDirectory $exeFolder -PassThru }
        else { $process = Start-Process -FilePath $Exe -WorkingDirectory $exeFolder -PassThru }
    }
    if ($profiled) { Write-Host '  Started with the kit''s test profile (low-latency queue, V-Sync, smooth pacing, full screen on connect); your own settings are untouched.' }

    if ($automatic) {
        if ([string]$Arm.set -eq 'capture') { Write-Host ('  Automatic: PushFPS starts your capture card (no PS5 connection) and runs ' + $plan + ' by itself. Keep the game moving as the session asks.') -ForegroundColor Green }
        elseif ($Purpose -eq 'network') { Write-Host ('  Automatic: PushFPS connects to the last console and runs ' + $plan + ' by itself.') -ForegroundColor Green }
        else { Write-Host ('  Automatic: PushFPS connects to the last console and runs ' + $plan + ' by itself. Do not touch the controller.') -ForegroundColor Green }
        Write-Host '  If it cannot (no console paired in this copy, or a build without automation), the kit says so and shows the steps to do by hand.'
        if ($Purpose -eq 'network') { Write-Host '  PLAY NORMALLY until the kit closes PushFPS: the picture must move (a still menu speaks only for a still menu).' -ForegroundColor Green }
        if (($Purpose -eq 'measure') -and (@($script:measureCues).Count -gt 0)) { Write-Host '  The kit tells you what to do, step by step, once the picture is there. Wait for each line that starts with >>>.' -ForegroundColor Green }
    } elseif (@('latency', 'quality', 'probe', 'network', 'measure') -contains $Purpose) {
        Write-ManualSteps $Purpose $trials $testsWanted $profiled
    } elseif ($Purpose -eq 'camera') {
        Write-Host '  1. Connect to the PS5 and wait in the game. Start the phone camera at 240 fps: controller and screen in one picture.'
        Write-Host '  2. Press the PS button, wait until the control centre is fully open, press it again to close, wait 2 seconds. 30 times.'
        Write-Host '  3. Stop the camera, close PushFPS. Count the frames later and fill in camera.csv.'
    } elseif ($Purpose -eq 'soak') {
        Write-Host ("  Connect to the PS5 and play or leave the game running for {0} minutes." -f [int]($LimitSeconds / 60))
        Write-Host '  To stop early and keep everything recorded so far: press Q (or Ctrl+C) in THIS window, or create the file' -ForegroundColor Green
        Write-Host ('    ' + $stopFile) -ForegroundColor Green
    } elseif ($Purpose -eq 'safestart') {
        Write-Host ("  Nothing to do: PushFPS starts with default settings and the kit closes it after {0} seconds." -f $LimitSeconds)
    }
    # Guided steps: Enter starts a step (a time marker), the kit watches for its length, then names the next one.
    $markerFile = Join-Path $runFolder 'markers.csv'
    if ($steps.Count -gt 0) {
        Set-Content -LiteralPath $markerFile -Value 'utc,marker' -Encoding ASCII
        Write-Host ('  NEXT: ' + $steps[0].prompt) -ForegroundColor Green
    }
    $stepIndex = 0
    $stepState = 'wait'
    $stepEnds = Get-Date
    $stepTold = Get-Date
    $markerCount = 0
    $tickMs = 1000
    if ($DryRun -and ($steps.Count -gt 0)) { $tickMs = 200 }

    # Once-a-second samples until the program is closed (or the time limit is reached).
    $sampleFile = Join-Path $runFolder 'samples.csv'
    $sampleHeader = 'utc,cpuTotalPercent,appCpuPercent,appWorkingSetMb,appPrivateMb,appThreads,appHandles,' + $appGpuColumns + ',sampling'
    if ($gpuColumns) { $sampleHeader += ',' + $gpuColumns }
    Set-Content -LiteralPath $sampleFile -Value $sampleHeader -Encoding ASCII
    $cores = [Environment]::ProcessorCount
    $lastCpu = $null
    $lastTick = Get-Date
    $began = Get-Date
    $limitReached = $false
    $restarts = 0
    $sampleRows = 0
    if ($Purpose -eq 'soak') { Remove-Item -LiteralPath $stopFile -Force -ErrorAction SilentlyContinue }   # one left by an earlier run
    if (($Purpose -eq 'soak') -and (-not $DryRun)) {
        # Ctrl+C then arrives as a key and ends the run like Q does, instead of ending the kit without a zip.
        try { [Console]::TreatControlCAsInput = $true; $controlCAsKey = $true } catch { $controlCAsKey = $false }
    }
    while ($true) {
        # A sample is taken before the program is looked at, so that every run has at least one row.
        $tick = Get-Date
        if ($watchLive) {
            $news = @()
            foreach ($kept in @(Select-SafeLines @(Read-LiveLines $logFolder $liveOffsets))) { $news += (Hide-OwnWords $kept) }
            if ($news.Count -gt 0) { Update-LiveState $live $news $Arm; $lastNews = Get-Date }
        }
        # While the built-in latency test is timing, only the process's own cheap values are read: no WMI query,
        # no GPU counter, no nvidia-smi, so the kit cannot add to what is measured.
        $light = [bool]$live.inTrial
        if ($light) { $live.lightSamples++ }
        $cpuTotal = ''
        if (-not $light) {
            try {
                $perf = Get-CimInstance -ClassName Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'" -ErrorAction Stop
                $cpuTotal = [string]$perf.PercentProcessorTime
            } catch { $cpuTotal = '' }
        }
        $appCpu = ''; $ws = ''; $private = ''; $threads = ''; $handles = ''; $appGpu = ',,,'
        try {
            $process.Refresh()
            $cpuNow = $process.TotalProcessorTime.TotalMilliseconds
            if ($null -ne $lastCpu) {
                $wall = ($tick - $lastTick).TotalMilliseconds
                if ($wall -gt 0) { $appCpu = [string][math]::Round(($cpuNow - $lastCpu) / $wall / $cores * 100, 1) }
            }
            $lastCpu = $cpuNow
            $ws = [string][math]::Round($process.WorkingSet64 / 1MB, 1)
            $private = [string][math]::Round($process.PrivateMemorySize64 / 1MB, 1)
            $threads = [string]$process.Threads.Count
            $handles = [string]$process.HandleCount
            if ((-not $light) -and (($sampleRows % $script:appGpuEvery) -eq 0)) { $appGpu = Get-AppGpuSample $process.Id }
        } catch { $appCpu = '' }
        $lastTick = $tick
        $sampling = 'full'
        if ($light) { $sampling = 'light' }
        $row = (@((Get-UtcStamp), $cpuTotal, $appCpu, $ws, $private, $threads, $handles) -join ',') + ',' + $appGpu + ',' + $sampling
        if ($gpuColumns) {
            if ($light) { $row += (',' * ($gpuColumns.Split(',').Count)) } else { $row += ',' + (Get-GpuSample) }
        }
        Add-Content -LiteralPath $sampleFile -Value $row -Encoding ASCII
        $sampleRows++

        if (($steps.Count -gt 0) -and ($stepIndex -lt $steps.Count)) {
            $step = $steps[$stepIndex]
            if ($stepState -eq 'wait') {
                $go = ($DryRun -or ($step.action -eq 'auto'))
                if (-not $go) {
                    $key = Read-KeyIfAny
                    if ($key -and ($key.Key -eq 'Enter')) { $go = $true }
                }
                if ($go) {
                    $stepStart = (Get-Date).ToUniversalTime()
                    if ($DryRun -and $script:dryBase) { $stepStart = $script:dryBase.AddSeconds($step.dryAt) }
                    Add-Marker $markerFile $stepStart $step.name
                    $markerCount++
                    Write-Host ('  marked: ' + $step.name)
                    if ($step.action -eq 'kill') {
                        Write-Host '  Ending PushFPS abruptly and starting it again ...' -ForegroundColor Yellow
                        try { Stop-Process -Id $process.Id -Force -ErrorAction Stop } catch { Write-Host ('  could not end it: ' + $_.Exception.Message) -ForegroundColor Yellow }
                        try { [void]$process.WaitForExit(10000) } catch { }
                        if (-not $DryRun) { Start-Sleep -Seconds 2 }
                        if ($DryRun) { $process = Start-Process -FilePath 'powershell.exe' -ArgumentList $dryArgs -WindowStyle Hidden -PassThru }
                        elseif ($exeArgs) { $process = Start-Process -FilePath $Exe -ArgumentList $exeArgs -WorkingDirectory $exeFolder -PassThru }
                        else { $process = Start-Process -FilePath $Exe -WorkingDirectory $exeFolder -PassThru }
                        $lastCpu = $null
                        $restarts++
                    }
                    if ($step.watch -gt 0) {
                        $stepState = 'watch'
                        $stepEnds = (Get-Date).AddSeconds($step.watch)
                        if ($DryRun) { $stepEnds = Get-Date }
                        $stepTold = Get-Date
                        Write-Host ("  watching for {0} seconds ..." -f $step.watch)
                    } else {
                        $stepIndex++
                        if ($stepIndex -lt $steps.Count) { Write-Host ('  NEXT: ' + $steps[$stepIndex].prompt) -ForegroundColor Green }
                    }
                }
            } elseif ((Get-Date) -ge $stepEnds) {
                $stepEnd = (Get-Date).ToUniversalTime()
                if ($DryRun -and $script:dryBase) { $stepEnd = $script:dryBase.AddSeconds($step.dryAt + $step.watch) }
                Add-Marker $markerFile $stepEnd ($step.name + '-end')
                $markerCount++
                if (-not $DryRun) { try { [Console]::Beep(880, 300) } catch { } }
                if ($step.question -and (-not $DryRun)) {
                    $answer = (Read-Host ('  ' + $step.question)).Trim().ToLower()
                    if ($answer.StartsWith('y')) { Add-Marker $markerFile (Get-Date).ToUniversalTime() ($step.name + '-answer-yes') }
                    elseif ($answer.StartsWith('n')) { Add-Marker $markerFile (Get-Date).ToUniversalTime() ($step.name + '-answer-no') }
                }
                $stepState = 'wait'
                $stepIndex++
                if ($stepIndex -lt $steps.Count) { Write-Host ('  NEXT: ' + $steps[$stepIndex].prompt) -ForegroundColor Green }
                else { Write-Host '  All steps are done. Close PushFPS now.' -ForegroundColor Green }
            } elseif (((Get-Date) - $stepTold).TotalSeconds -ge 10) {
                $stepTold = Get-Date
                Write-Host ("  {0:N0} seconds left in this step" -f ($stepEnds - (Get-Date)).TotalSeconds)
            }
        }
        if ($Purpose -eq 'soak') {
            # Kill-switch: Q or Ctrl+C in this window, or the stop file. Everything recorded so far is kept.
            if ($DryRun -and ($DryRunStopAfter -gt 0) -and ($sampleRows -ge $DryRunStopAfter) -and (-not $stopAsked)) {
                Set-Content -LiteralPath $stopFile -Value 'stop' -Encoding ASCII
            }
            if (-not $stopAsked) {
                $why = ''
                if (Test-Path -LiteralPath $stopFile) { $why = 'stop file' }
                elseif (-not $DryRun) {
                    $key = Read-KeyIfAny
                    if ($key -and (($key.Key -eq 'Q') -or (($key.Key -eq 'C') -and ($key.Modifiers.ToString() -match 'Control')))) { $why = 'key' }
                }
                if ($why) {
                    $stopAsked = $true; $stopAskedAt = Get-Date; $stopReason = 'kill-switch (' + $why + ')'
                    Write-Host ('  Stop asked (' + $why + '): closing PushFPS and saving what was recorded.') -ForegroundColor Green
                    try { [void]$process.CloseMainWindow() } catch { }
                }
            }
            if (($sampleRows % 600) -eq 0) { Write-Host ("  {0:N0} of {1:N0} minutes; PushFPS memory {2} MB" -f ((Get-Date) - $began).TotalMinutes, ($LimitSeconds / 60), $ws) }
            # The next PresentMon window.
            # Without administrator rights every window would be one more Windows prompt in an unattended run: only the first.
            if ($presentMonExe -and $elevated -and (-not $DryRun) -and (-not $stopAsked) -and ($SoakPresentMonSeconds -gt 0) -and ($SoakPresentMonEveryMinutes -gt 0) -and
                (((Get-Date) - $lastWindow).TotalMinutes -ge $SoakPresentMonEveryMinutes)) {
                $lastWindow = Get-Date
                $windowCount++
                $windowName = 'presentmon-{0:D3}.csv' -f $windowCount
                try {
                    $windowStart = Get-UtcStamp
                    $presentMonProcess = Start-PresentMon ($pmBase + ' ' + (Get-PmOption 'output_file') + ' "' + (Join-Path $runFolder $windowName) + '"')
                    Add-Content -LiteralPath $windowFile -Value ($windowName + ',' + $windowStart) -Encoding ASCII
                } catch { Write-Host ('  PresentMon window did not start: ' + $_.Exception.Message) -ForegroundColor Yellow }
            }
        }
        # Kit 5: the guided steps of a measured session (-Session input), timed from the first presented frame the kit sees.
        if (($Purpose -eq 'measure') -and ($live.cuesTold -lt @($script:measureCues).Count)) {
            $cue = @($script:measureCues)[$live.cuesTold]
            $cueDue = [bool]$DryRun
            if ((-not $cueDue) -and ($null -ne $live.streamAt)) { $cueDue = (((Get-Date) - $live.streamAt).TotalSeconds -ge $cue.at) }
            if ($cueDue) {
                Write-Host ('  >>> ' + $cue.text) -ForegroundColor Green
                if (-not $DryRun) { try { [Console]::Beep(880, 200) } catch { } }
                $live.cuesTold++
            }
        }
        if ($automatic -and (-not $DryRun) -and (-not $stopAsked)) {
            $elapsed = ((Get-Date) - $began).TotalSeconds
            $why = ''
            if ($live.autorunFailed) { $why = 'the automatic plan stopped: ' + (Get-StopReasonText $live.autorunFailed) }
            elseif ((-not $live.autorunSeen) -and ($elapsed -ge 120)) { $why = 'no automation line in 2 minutes (this build may not have the automation)' }
            if ($why -and (-not $live.fallbackTold)) {
                $live.fallbackTold = $true
                # The automatic plan is cancelled before the manual steps are offered (review of 16a2bfec, item 9): PushFPS
                # is closed and started again without the automation switches, so no pending step can start a test too.
                Write-Host ('  ' + $why + '. PushFPS starts again without the automation; then do it by hand:') -ForegroundColor Yellow
                foreach ($name in @('VEYRA_TEST_RP_AUTORUN', 'VEYRA_TEST_RP_AUTOCONNECT', 'VEYRA_TEST_RP_CAPTURE')) { Remove-Item -Path ('Env:' + $name) -ErrorAction SilentlyContinue }
                try { [void]$process.CloseMainWindow() } catch { }
                try { if (-not $process.WaitForExit(20000)) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; [void]$process.WaitForExit(10000) } } catch { }
                if ($exeArgs) { $process = Start-Process -FilePath $Exe -ArgumentList $exeArgs -WorkingDirectory $exeFolder -PassThru }
                else { $process = Start-Process -FilePath $Exe -WorkingDirectory $exeFolder -PassThru }
                $lastCpu = $null; $restarts++
                $automatic = $false
                $live.autorunFinished = $null; $live.finishedAt = $null
                # A measured session (kit 5) is closed by the kit after the plan's time also when the plan could not run, so that
                # an unattended session goes on to the next arm instead of waiting for a click.
                if (($Purpose -eq 'measure') -and ($LimitSeconds -le 0)) { $LimitSeconds = [int](Get-PlanSeconds $script:measurePlan) + 60; $began = Get-Date }
                Write-ManualSteps $Purpose $trials $testsWanted $profiled
            }
            if ($automatic -and ($null -ne $live.autorunFinished) -and ($null -eq $live.finishedAt)) { $live.finishedAt = Get-Date }
            if (($null -ne $live.finishedAt) -and (((Get-Date) - $live.finishedAt).TotalSeconds -ge 30)) {
                $stopAsked = $true; $stopAskedAt = Get-Date; $stopReason = 'autorun finished'
                try { [void]$process.CloseMainWindow() } catch { }
            } elseif ($automatic -and (-not $live.fallbackTold) -and ($elapsed -ge $autorunLimitSeconds)) {
                $stopAsked = $true; $stopAskedAt = Get-Date; $stopReason = 'autorun timeout'
                Write-Host ('  The automatic plan did not finish within ' + [int]($autorunLimitSeconds / 60) + ' minutes: closing PushFPS.') -ForegroundColor Yellow
                try { [void]$process.CloseMainWindow() } catch { }
            }
        }
        if (($Purpose -eq 'latency') -and (-not $NoAutoClose) -and ($testsWanted -gt 0) -and (-not $stopAsked) -and ($live.testsDone -ge $testsWanted) -and (-not $live.inTrial)) {
            $stopAsked = $true; $stopAskedAt = Get-Date; $stopReason = 'tests done'
            Write-Host ('  ' + $live.testsDone + ' latency tests done: closing PushFPS.') -ForegroundColor Green
            try { [void]$process.CloseMainWindow() } catch { }
        }
        if ($watchLive -and (-not $DryRun) -and (-not $stopAsked) -and (((Get-Date) - $lastNews).TotalSeconds -ge 60)) {
            $lastNews = Get-Date
            $waited = [int]((Get-Date) - $began).TotalMinutes
            if ($Purpose -eq 'latency') { Write-Host ('  still waiting (' + $waited + ' min): ' + $live.testsDone + ' of ' + $testsWanted + ' latency tests done. The test is in the PS5 streaming menu of PushFPS.') }
            else { Write-Host ('  still running (' + $waited + ' min). Close PushFPS when you are done.') }
        }
        if (($LimitSeconds -gt 0) -and (-not $limitReached) -and (-not $stopAsked) -and (((Get-Date) - $began).TotalSeconds -ge $LimitSeconds)) {
            $limitReached = $true; $stopAsked = $true; $stopAskedAt = Get-Date; $stopReason = 'time'
            Write-Host '  Time is up: closing PushFPS.' -ForegroundColor Green
            try { [void]$process.CloseMainWindow() } catch { }
        }
        if ($stopAsked -and (-not $forcedStop) -and (-not $process.HasExited) -and (((Get-Date) - $stopAskedAt).TotalSeconds -ge 30)) {
            # Asked to close 30 s ago and still there: end it, so that an unattended run still gives its evidence.
            $forcedStop = $true
            Write-Host '  PushFPS did not close within 30 s: ending it. This is recorded.' -ForegroundColor Yellow
            try { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue } catch { }
        }
        if ($process.HasExited) {
            # PushFPS can restart itself once (overlay compatibility): follow the new copy.
            $other = $null
            if ((-not $DryRun) -and (-not $stopAsked)) { Start-Sleep -Milliseconds 1500; $other = Find-RunningCopy $Exe }
            if ($other) { $process = $other; $lastCpu = $null; $restarts++ } else { break }
        }
        $spent = ((Get-Date) - $tick).TotalMilliseconds
        if ($spent -lt $tickMs) { Start-Sleep -Milliseconds ([int]($tickMs - $spent)) }
    }
    # The loop above is the sampler: it runs in this script, so when it ends nothing is still writing.
    if ($controlCAsKey) { try { [Console]::TreatControlCAsInput = $false } catch { } }
    if ($Purpose -eq 'soak') { Remove-Item -LiteralPath $stopFile -Force -ErrorAction SilentlyContinue }
    try { $process.WaitForExit() } catch { }
    if ($DryRun) {
        # The dry run always leaves every marker, however few samples its short stand-in allowed.
        while ($stepIndex -lt $steps.Count) {
            $step = $steps[$stepIndex]
            $stepStart = (Get-Date).ToUniversalTime()
            if ($script:dryBase) { $stepStart = $script:dryBase.AddSeconds($step.dryAt) }
            if ($stepState -eq 'wait') { Add-Marker $markerFile $stepStart $step.name; $markerCount++ }
            if ($step.watch -gt 0) { Add-Marker $markerFile $stepStart.AddSeconds($step.watch) ($step.name + '-end'); $markerCount++ }
            $stepState = 'wait'
            $stepIndex++
        }
    }
    if ($Purpose -eq 'gpuloss') { Remove-Item -Path 'Env:VEYRA_TEST_GPU_FAILURE' -ErrorAction SilentlyContinue }
    $endUtc = Get-UtcStamp
    $exitCode = $null
    try { $exitCode = $process.ExitCode } catch { $exitCode = $null }

    if ($presentMonProcess) {
        if (-not $presentMonProcess.WaitForExit(15000)) {
            $ender = Start-PresentMon ((Get-PmOption 'session_name') + ' ' + $sessionName + ' ' + (Get-PmOption 'terminate_existing_session'))
            if ($ender) { try { [void]$ender.WaitForExit(15000) } catch { } }
            if (-not $presentMonProcess.WaitForExit(10000)) { try { Stop-Process -Id $presentMonProcess.Id -Force -ErrorAction SilentlyContinue } catch { } }
        }
    }
    [void](Clear-KitSwitches)

    $hasCapture = (Test-Path -LiteralPath $presentMonCsv) -and ((Get-Item -LiteralPath $presentMonCsv).Length -gt 200)
    if ($presentMonExe -and (-not $DryRun) -and (-not $hasCapture)) {
        Write-Host '  PresentMon wrote no capture for this run (was its Windows prompt declined?).' -ForegroundColor Yellow
    }

    $script:summaryPresses = 0
    # Two seconds of slack: the program's clock stamps a line when it is made, the kit's start time is taken
    # just before the program starts.
    $fromUtc = ([datetime]::ParseExact($startUtc.Substring(0, 23), 'yyyy-MM-ddTHH:mm:ss.fff', [System.Globalization.CultureInfo]::InvariantCulture)).AddSeconds(-2).ToString('yyyy-MM-ddTHH:mm:ss.fff') + 'Z'
    $lines = @()
    foreach ($kept in @(Select-SinceStart @(Select-SafeLines (Read-NewLogLines $logFolder $before)) $fromUtc)) { $lines += (Hide-OwnWords $kept) }
    $presses = Count-Presses $lines
    $benchFolder = Join-Path $evidence 'bench'
    New-Item -ItemType Directory -Force -Path $benchFolder | Out-Null
    $benchName = $runName + '.txt'
    Set-Content -LiteralPath (Join-Path $benchFolder $benchName) -Value $lines -Encoding UTF8

    $traceState = Save-RunTrace $logFolder $traceSince $runFolder $runName
    $config = Get-RunConfig $lines
    $config.from = ("run '" + $runName + "'")
    $trialsSeen = Read-Number ([string]$config.trialsMax)
    if (($null -ne $trialsSeen) -and ($trialsSeen -ge 1)) { $script:trialsMax[[string]$Arm.exe] = [int]$trialsSeen }
    $problems = @()
    if (($Purpose -eq 'latency') -or ($Purpose -eq 'camera')) { $problems = @(Get-RunProblems $Arm $config) }
    # -Pass (smoothness session, stream fps): the run must have run at the refresh rate this pass is for.
    if (($Pass -gt 0) -and ($Purpose -eq 'measure')) {
        $ranHz = Read-Number ([string]$config.refreshHz)
        if (($null -eq $ranHz) -or ([math]::Abs($ranHz - $Pass) -gt 3)) {
            $problems += ('the display ran at ' + [string]$config.refreshHz + ' Hz, this pass is for ' + $Pass + ' Hz: set Windows to ' + $Pass + ' Hz (Settings > System > Display > Advanced display > Choose a refresh rate)')
        }
    }
    $switchList = @()
    foreach ($name in ($switches.Keys | Sort-Object)) { $switchList += ($name + '=' + $switches[$name]) }
    # VEYRA_EXP_RP_CHIAKI_SEND_BUFFER is read inside the streaming library: no app line shows it, so the kit records what
    # this run's process was started with (patch 0009: exactly 1 = 1024 kept messages, anything else = the pin's 16).
    $chiakiSendBuffer = 'unset: 16 (the pin)'
    if ($switches.ContainsKey('VEYRA_EXP_RP_CHIAKI_SEND_BUFFER')) {
        if ([string]$switches['VEYRA_EXP_RP_CHIAKI_SEND_BUFFER'] -ceq '1') { $chiakiSendBuffer = '1: 1024' } else { $chiakiSendBuffer = 'set but not 1: 16 (the pin)' }
    }
    $run = [pscustomobject]@{
        chiakiSendBuffer = $chiakiSendBuffer
        arm = $Arm.name; exe = $Arm.exe; compare = $Arm.compare; purpose = $Purpose; switches = $switchList; kitSwitches = $kitSwitches
        startUtc = $startUtc; endUtc = $endUtc; presentMonStartUtc = $presentMonStart; presentMonCapture = $hasCapture
        exitCode = $exitCode; restarts = $restarts; limitSeconds = $LimitSeconds; limitReached = $limitReached
        stopReason = $stopReason; forcedStop = $forcedStop; presentMonWindows = $windowCount
        presses = $presses; pressesAnsweredInSummaries = $script:summaryPresses; logLines = $lines.Count; log = ('bench/' + $benchName)
        sampleRows = $sampleRows; markers = $markerCount; dryRun = [bool]$DryRun
        appGpuCounters = $script:appGpuAvailable; logFrom = $fromUtc
        config = $config; configText = (Get-ConfigText $config); pairProblems = $problems; sceneNote = $SceneNote; fgExpected = [string]$Arm.fg
        latencyTestsSeen = $live.testsDone; samplesWhileTestTimed = $live.lightSamples; setAside = $false; setAsideReason = ''
        trialsAsked = $trials; testsPerStart = $testsWanted; automatic = $automatic; autorunPlan = $plan; autorunFinished = $live.autorunFinished
        autorunFailed = $live.autorunFailed; testProfile = $profiled; smartlinkTrace = $traceState
        completed = $true   # written last: a run folder without this file, or without this field, did not finish
    }
    if (($Purpose -eq 'reliability') -or ($Purpose -eq 'gpuloss') -or ($Purpose -eq 'safestart')) {
        Write-ReliabilityVerdicts $lines $runFolder $Purpose $limitReached $forcedStop $LimitSeconds
    }
    Save-Json $run (Join-Path $runFolder 'run.json')
    $script:lastRun = [pscustomobject]@{ record = $run; folder = $runFolder; bench = (Join-Path $benchFolder $benchName); name = $runName }
    if ($presses -eq 0 -and $script:summaryPresses -gt 0) { $presses = $script:summaryPresses }
    Write-Host ("  saved: {0} log lines, {1} presses" -f $lines.Count, $presses)
    return $presses
}

function New-DryLine([double]$At, [string]$Text) {
    return [pscustomobject]@{ at = $At; text = $Text }
}

# Made-up log of a reliability or GPU-loss run on a fixed timeline (seconds after $script:dryBase). The dry run puts
# its markers on the same timeline (New-Step dryAt), so the verdicts are known in advance; the checks workflow
# expects: cable-out-1s PASS (back after 4 s), cable-out-5s FAIL (back after 71 s), display-mode-change PASS,
# display-reconnect NOT-EXERCISED (no display line), focus-loss-keys-held SKIPPED (no keyboard and mouse line),
# crash-recovery PASS, gpu-device-loss PASS.
function Add-DryRunTimeline([string]$LogFolder, [string]$Purpose) {
    New-Item -ItemType Directory -Force -Path $LogFolder | Out-Null
    $script:dryBase = (Get-Date).ToUniversalTime()
    $progress = '[INFO ] [remoteplay-progress] state=3 received=600 decoded=600 inputFps={0} decodeFps={0} videoMbps=28.100 receiveAgeMs=7.0 decodeAgeMs=5.0 decodeBusyMs=1.2 queue=0 waitingIdr=false idrRequests=0 errors=0'
    $frameRate = '[INFO ] [frame-rate] revision=1 gpuCompletedFps=59.90 completedReal=60 capture60To30=false rateSkipped=0 presentSubmitFps=59.90 windowMs=1000 vramMiB=1200 budgetMiB=11000 (not scanout FPS)'
    $started = '[INFO ] [remoteplay-settings] slipstream.ini not read (VEYRA_TEST_RP_NO_SETTINGS_FILE is set)'
    $session = '[INFO ] [remoteplay] session started host=192.168.1.77 profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000'
    $items = @()
    $items += New-DryLine 1.25 $started
    $items += New-DryLine 2.25 $session
    $items += New-DryLine 2.5 '[INFO ] [remoteplay-control] start host=PlantedHostName restart=false background=false'
    $items += New-DryLine 2.75 '[INFO ] [remoteplay-discovery] host-name=PlantedConsoleName host-id=AABBCCDDEEFF'
    if ($Purpose -eq 'reliability') {
        for ($i = 3; $i -lt 500; $i++) {
            if (($i -ge 371) -and ($i -le 391)) { continue }     # PushFPS is down: the kit ended it at 370
            $fps = '59.9'
            if ((($i -ge 41) -and ($i -le 43)) -or (($i -ge 81) -and ($i -le 150))) { $fps = '0.0' }
            $items += New-DryLine $i ($progress -f $fps)
            if (($i % 5) -eq 0) { $items += New-DryLine ($i + 0.1) $frameRate }
        }
        $items += New-DryLine 43.5 '[WARN ] [remoteplay-recovery] request keyframe; received=900 decoded=880 videoCallbacks=900 rejected=0 packetReceived=4000 packetLost=37'
        $items += New-DryLine 85.5 '[WARN ] [remoteplay-recovery] old session joined; reconnect=1 using existing in-memory pairing'
        $items += New-DryLine 150.5 '[INFO ] [remoteplay-recovery] reconnected attempt=1 first decoded frame; reset video history and audio clock'
        $items += New-DryLine 195.5 '[INFO ] [display-query] invalidate reason=display-change generation=2 (stored states are stale; each monitor in use is queried once more)'
        $items += New-DryLine 196.5 '[INFO ] [display-refresh] monitor refresh=120.000 Hz (was 180.000; window moved or display mode changed)'
        $items += New-DryLine 205.5 '[INFO ] [display-refresh] monitor refresh=180.000 Hz (was 120.000; window moved or display mode changed)'
        $items += New-DryLine 375.5 $started
        $items += New-DryLine 390.5 $session
    } else {
        for ($i = 3; $i -lt 170; $i++) {
            if (($i -ge 21) -and ($i -le 42)) { continue }       # no picture between the device loss and the reconnect by hand
            $items += New-DryLine $i ($progress -f '59.9')
            $items += New-DryLine ($i + 0.1) $frameRate
        }
        $items += New-DryLine 20.25 '[WARN ] [engine-test] simulating a GPU frame-ready timeout; device removal after 500 ms'
        $items += New-DryLine 20.75 '[WARN ] [engine-test] device removed'
        $items += New-DryLine 21.25 '[ERROR] [gpu-failure] operation=frame-ready alive=false removedReason=0x887A0006 requestedFence=10 completedFence=9 sentinel=false adapter=MadeUpAdapter device=0x1 luid=1 driver=1'
        $items += New-DryLine 21.5 '[WARN ] [gpu-reconnect] session=1 kind=ps5 automatic=false'
        $items += New-DryLine 41.5 $session
    }
    $out = @()
    foreach ($item in @($items | Sort-Object -Property at)) {
        $out += ($script:dryBase.AddSeconds($item.at).ToString('yyyy-MM-ddTHH:mm:ss.fffZ') + ' t=1234 ' + $item.text)
    }
    Add-Content -LiteralPath (Join-Path $LogFolder 'veyra-qml.log') -Value $out -Encoding UTF8
}

# Made-up log lines for -DryRun: what a run writes, plus lines and values that must NOT reach the zip.
function Add-DryRunLog([string]$LogFolder, [string]$Purpose, [string]$ArmName, [string]$ArmExe, [string]$ArmSwitches = '') {
    $script:dryBase = $null
    if (($Purpose -eq 'reliability') -or ($Purpose -eq 'gpuloss')) { Add-DryRunTimeline $LogFolder $Purpose; return }
    New-Item -ItemType Directory -Force -Path $LogFolder | Out-Null
    $now = (Get-Date).ToUniversalTime()
    $values = @()
    $offset = 0.0
    if ($DryRunPresses -gt 20) { if ($ArmName -eq 'original') { $offset = 20.0 } elseif ($ArmName -eq 'mod-all') { $offset = -20.0 } }
    $count = [math]::Max(1, $DryRunPresses)
    for ($i = 0; $i -lt $count; $i++) { $values += (83.3 + $offset + 16.7 * ($i % 3) + ($i % 20) * 0.1).ToString('F1', [System.Globalization.CultureInfo]::InvariantCulture) }
    # -DryRunMismatchArm: that arm streams at another bitrate, so the kit and the report must refuse it.
    $bitrate = '30000'
    # A clarity arm asks for its own bitrate and codec (VEYRA_TEST_RP_BITRATE_KBPS / _CODEC): the made-up session streams at
    # them, as the app does; -DryRunMismatchArm on such an arm then plays a build that did not apply the override.
    if ($ArmSwitches -match 'VEYRA_TEST_RP_BITRATE_KBPS=(\d+)') { $bitrate = [string]$Matches[1] }
    $sessionCodec = 'hevc'
    if ($ArmSwitches -match 'VEYRA_TEST_RP_CODEC=(h264|hevc-hdr|hevc)') { $sessionCodec = [string]$Matches[1] }
    if ($DryRunMismatchArm -and ($ArmName -eq $DryRunMismatchArm)) { $bitrate = '15000' }
    $made = @(
        '[INFO ] [app] PushFPS QML session started (based on Veyra)',
        '[INFO ] [app] data folder C:\Users\PlantedUserName\AppData\Local\Programs\PushFPS\runtime_local',
        '[INFO ] [remoteplay-discovery] host-name=PlantedConsoleName host-id=AABBCCDDEEFF',
        '[INFO ] [remoteplay-ui] registration regist_key=0123456789abcdef0123 account=PlantedAccountId=',
        '[INFO ] [wlan] ssid=PlantedWifiName',
        '[INFO ] [remoteplay-settings] made-up values VEYRA_EXP_RP_KBM_PROFILE=\\PLANTEDSRV\share\planted-kbm.txt and C:/Users/Planted User/AppData/Local/kbm.ini end',
        '[INFO ] [remoteplay-kbm] made-up ids: mail planted.user@example.com mac 001A2B3C4D5E dotted 001a.2b3c.4d5e guid 0F8FAD5B-D9CB-469F-A165-70867728950E id 1A2B3C4D5E6F7A8 server=PlantedServer computer=PLANTED-PC device=PlantedDevice network=PlantedNet peer fe80::1ff:fe23:4567:890b%12 end',
        '[INFO ] [remoteplay-ui] regist key=PlantedSpaceKey',
        '[INFO ] [remoteplay-control] start host=192.168.1.77 restart=false background=false',
        '[INFO ] [remoteplay-control] start host=PlantedHostName restart=false background=false',
        ('[INFO ] [remoteplay] session started host=203.0.113.9 profile=1920x1080@60 codec=hevc requestedBitrateKbps=' + $bitrate),
        '[INFO ] [remoteplay] peer fe80::1ff:fe23:4567:890a mac 00:1A:2B:3C:4D:5E key 00112233445566778899aabbccddeeff',
        '[INFO ] [remoteplay-timing] ownerWait=event input=immediate engineWake=event',
        '[INFO ] [remoteplay-settings] slipstream.ini not read (VEYRA_TEST_RP_NO_SETTINGS_FILE is set)',
        '[INFO ] [remoteplay-decode] actual=D3D12VA pixelFormat=172 1920x1080 fallback=false',
        '[INFO ] [qml] [update-check] current installed=3.2.1 build=3.2.1 latest=3.2.1',
        '[INFO ] [qml] PlantedQmlText that is not an update check',
        '[INFO ] [overlay-hooks] RTSS hook module: activeServer=false path=C:\Program Files (x86)\PlantedFolder\RTSSHooks64.dll',
        '[WARN ] [engine-test] made-up line with a folder C:\Users\Planted User Name\AppData\Local\Programs\PushFPS and nothing else',
        '[INFO ] [display-vrr] level=2 known=true monitorSupportsVrr=true currentlyCapable=true trueGsync=false capsKnown=true possible=true requested=true active=true displayInVrrMode=true indicator=false simulated=false status=0 refreshHz=180.000 (made up)',
        '[INFO ] [display-latency] presentToScanoutP50Ms=2.100 presentToScanoutP95Ms=4.800 presentToScanoutMaxMs=6.000 samples=180 replacedAtLeast=0 compositionMode=2 refreshHz=180.000 (made up)',
        '[INFO ] [frame-rate] revision=1 gpuCompletedFps=59.90 completedReal=60 capture60To30=false rateSkipped=0 presentSubmitFps=59.90 windowMs=1000 vramMiB=1200 budgetMiB=11000 (not scanout FPS)',
        '[INFO ] [frame-flow] state=active session=1 revision=1 epoch=1 source=600 batch=600 captureReceived=600 mailboxOverwritten=0 sourceAccepted=600 realSubmitted=600 fgEvaluated=0 fgReadyValid=0 realPresented=600 generatedPresented=0 generatedExpiredAfterEval=0 cancelledBeforePresent=0 presentSubmitFps=59.90',
        '[INFO ] [remoteplay-health] route=direct-lan link=ethernet linkMbps=1000 (the side of this PC only)',
        '[INFO ] [remoteplay-health] grade=stable interval=stable seconds=5.0 lossPercent=0.000 lost=0 received=9000 frameJitterMs=0.40 frameJitterPeakMs=1.20 reordered=0 late=0 reorderTimeouts=0 skipped=0 fecRecovered=0 fecFailed=0 keyframeRequests=0 decoderResets=0 frameDrops=0 ingressDepth=0 ingressPeak=1 mailboxOverwrites=0 mbps=28.10 fps=59.9 rtt=0.45ms(at-connect) route=direct-lan link=ethernet linkMbps=1000 bottleneck=none signs=none overshootMs=0.00 decodeP95Ms=1.80',
        '[INFO ] [push-hardware] vendor=0x10de decodeKnown=true h264=true hevc=true hevc10=true fgBackends=4 displayHz=180.00 vrrState=3 networkKnown=true ethernet=true wifi=false routeKnown=true directLan=true link=ethernet',
        '[INFO ] [slipstream-governor] mode=observe recommend=hold applied=0 sourceFps=59.94 jitterMs=0.40 lossPercent=0.000 framesLostPerSecond=0 decodeMs=1.20 gpuMs=2.10 gpuHeadroomPercent=70 queueDepth=1 schedulingMs=0.10 presentLateMs=0.00 knobs reorder=0 backlog=0 pollMs=4 fgStep=0 tier=0 unavailable=[] rollbacks=0 declined=0 invalid=0 session=1',
        '[INFO ] [remoteplay-progress] state=3 received=600 decoded=600 inputFps=59.9 decodeFps=59.9 videoMbps=28.100 receiveAgeMs=3.0 decodeAgeMs=5.0 decodeBusyMs=1.2 queue=0 waitingIdr=false idrRequests=0 errors=0',
        '[INFO ] [push-latency] remote play latency test started trials=20',
        ('[INFO ] [push-latency] done answered=20/20 meanMs=100.9 medianMs=100.0 minMs=83.3 maxMs=118.6 pushfpsMs=6.1'),
        ('[INFO ] [push-latency] trials answerMs=[' + ($values -join ',') + ']')
    )
    # the clarity arm's own codec in its session line (the made-up text above stays the default one)
    if ($sessionCodec -ne 'hevc') { for ($i = 0; $i -lt $made.Count; $i++) { if ($made[$i] -match '\[remoteplay\] session started ') { $made[$i] = $made[$i] -replace ' codec=hevc ', (' codec=' + $sessionCodec + ' ') } } }
    $queue = 'true'
    if ($DryRunQueueArm -and ($ArmName -eq $DryRunQueueArm)) { $queue = 'false' }
    # An arm that turns the low queue off (VEYRA_EXP_PRESENT_LOW_QUEUE_OFF=1) or runs windowed (pref:pushAutoFullscreen=false)
    # logs that as the app does: the request stays, the effective queue is off; a windowed picture has no active variable refresh.
    $queueOffArm = ($ArmSwitches -match 'VEYRA_EXP_PRESENT_LOW_QUEUE_OFF=1')
    $windowedArm = ($ArmSwitches -match 'pref:pushAutoFullscreen=false')
    $effectiveQueue = $queue
    if ($queueOffArm) { $effectiveQueue = 'false' }
    if ($windowedArm) { for ($i = 0; $i -lt $made.Count; $i++) { if ($made[$i] -match '\[display-vrr\]') { $made[$i] = $made[$i] -replace ' active=true ', ' active=false ' } } }
    # the smoothness arm that does not ask for variable refresh (pref:vrr=false): the app reports it neither requested nor active
    if ($ArmSwitches -match 'pref:vrr=false') { for ($i = 0; $i -lt $made.Count; $i++) { if ($made[$i] -match '\[display-vrr\]') { $made[$i] = $made[$i] -replace ' requested=true active=true ', ' requested=false active=false ' } } }
    $made += ('[INFO ] [pacing] revision=2 requested=' + $queue + '/0/1 effective=' + $effectiveQueue + '/0/1 displayQueuePaced=true variableRefresh=true vrrLevel=2 vrrSync=false')
    $made += '[INFO ] [pacing] DXGI enabled=true maximumLatency=1 vsync=true tearing=false hr=0x0 staleLatencyCounts=0'
    for ($i = 0; $i -lt 12; $i++) { $made += '[INFO ] [frame-rate] revision=1 gpuCompletedFps=59.90 completedReal=60 capture60To30=false rateSkipped=0 presentSubmitFps=59.90 windowMs=1000 vramMiB=1200 budgetMiB=11000 (not scanout FPS)' }
    if ($DryRunWifi) { for ($i = 0; $i -lt $made.Count; $i++) { $made[$i] = $made[$i] -replace 'link=ethernet', 'link=wifi' } }
    if (($ArmExe -eq 'experimental') -or ($ArmExe -eq 'previous')) {
        # The sprint 4 lines of the experimental build (S4_CONTRACT.md); a real original or stable build has none of them.
        # Package 7's experimental build ('previous') writes them too, but none of the kit 5 lines below.
        $route = 'wired'
        if ($DryRunWifi) { $route = 'wifi' }
        $queueFlag = '1'
        if (($queue -eq 'false') -or $queueOffArm) { $queueFlag = '0' }
        $nvidiaFlag = '1'
        if ($queueOffArm) { $nvidiaFlag = '0' }
        $fullFlag = '1'; $windowText = '2560x1440'
        if ($windowedArm) { $fullFlag = '0'; $windowText = '1600x900' }
        $hdrFlag = '0'
        if ($ArmSwitches -match '\|hdr=1') { $hdrFlag = '1' }   # kit 5: an arm that asks for HDR logs it (HDR and SDR arms are never compared)
        # kit 5: the keys S5_CONTRACT.md interface 2 adds at the end of the line; an arm that asks for the upscaler or the clean-up logs them
        $srTargetText = 'off'; $srModelText = 'none'; $cleanText = '0'
        if ($ArmSwitches -match '\|srTarget=qhd') { $srTargetText = '2560x1440' } elseif ($ArmSwitches -match '\|srTarget=4k') { $srTargetText = '3840x2160' }
        if ($ArmSwitches -match '\|srModel=([A-Za-z0-9]+)') { $srModelText = [string]$Matches[1] }
        if ($ArmSwitches -match '\|clean=([0-9]+)') { $cleanText = [string]$Matches[1] }
        # The line exactly as the app writes it (src/ui/bridge/PushAutorun.cpp, its format string).
        $made += ('[INFO ] [slipstream-config] mode=ultra-low-latency fg=0 fgMethod=0 fgMultiplier=2 fgTier=1 sr=0 srQuality=0 enhance=0 lowQueue=' + $queueFlag + ' nvLowLatency=' + $nvidiaFlag + ' displaySync=1 smoothPacing=1 vrrRequested=1 vrrLevel=2 fullscreen=' + $fullFlag + ' window=' + $windowText + ' output=2560x1440 refreshHz=180.00 hdr=' + $hdrFlag + ' stream=1920x1080@60.00 codec=hevc bitrateKbps=' + $bitrate + ' decoder=hardware route=' + $route + ' trialsMax=500 modeSource=ini fgActive=0 srActive=0 smartlink=off nrRuntime=3 nrSize=1080 srTarget=' + $srTargetText + ' srModel=' + $srModelText + ' fgBackend=none clean=' + $cleanText + ' profile=none')
        $made += '[INFO ] [slipstream-autorun] step=0 name=connect state=start reason=none'
        $made += '[INFO ] [slipstream-autorun] step=0 name=PlantedConsoleName state=done reason=none'
        for ($i = 0; $i -lt $count; $i++) {
            $pc = (9.0 + ($i % 7) * 0.5 + $offset / 10).ToString('F2', [System.Globalization.CultureInfo]::InvariantCulture)
            $console = (40.0 + ($i % 11) * 0.7).ToString('F2', [System.Globalization.CultureInfo]::InvariantCulture)
            $made += ('[INFO ] [slipstream-press] n=' + $i + ' sendToArrivalMs=' + $console + ' arrivalToScanoutMs=' + $pc + ' totalMs=60.00 phaseMs=3.10')
        }
        $made += '[INFO ] [slipstream-latency-test] trials=20 answered=20 button=ps consoleP50=43.0 consoleP95=46.5 pcP50=10.5 pcP95=12.0 totalP50=60.0 totalP95=63.0'
        # kit 5: the audio delay of the arm with the three audio switches is twice the base arm's, so the comparison says 'differs'
        $audioDelay = '30.0'
        if ($ArmName -eq 's-audio-all') { $audioDelay = '60.0' }
        # kit 5 (S5_PRESENT_API.md): an own-clock arm logs its fields; in s-audio-own-fast the arrival lookup 'fails' (ownFrames stays 0),
        # so the dry run shows a failed arm that is not compared.
        $ownClockArm = ($ArmSwitches -match 'VEYRA_EXP_RP_AUDIO_OWN_CLOCK=1')
        $ownFramesStep = 300
        if ($ArmName -eq 's-audio-own-fast') { $ownFramesStep = 0 }
        for ($i = 0; $i -lt 3; $i++) {
            $made += '[INFO ] [slipstream-latency] frames=300 arrivalToDecodedP50=1.2 arrivalToDecodedP95=1.9 decodedToPickupP50=0.4 decodedToPickupP95=0.9 pickupToPresentCallP50=0.3 pickupToPresentCallP95=0.6 presentCallP50=0.2 presentCallP95=0.5 presentToScanoutP50=6.1 presentToScanoutP95=9.0 arrivalToScanoutP50=8.4 arrivalToScanoutP95=11.6 arrivalToScanoutP99=13.0 arrivalToScanoutMax=15.0 scanoutKnown=290 syncInterval=1 tearing=0 queueLatency=1 waitable=1 flip=independent'
            $made += '[INFO ] [slipstream-delivery] arrived=300 shown=298 replaced=2 gapP50=16.6 gapP95=21.0 gapMax=40.0 clumps=3 longGaps=1 shownGapP95=17.0 shownGapMax=33.0 policy=newest'
            $ownText = ''
            if ($ownClockArm) { $ownText = ' ownClock=1 ownTargetMs=28.0 ownMedianMs=6.0 ownFrames=' + ($ownFramesStep * ($i + 1)) + ' ownRejected=0' }
            $made += ('[INFO ] [remoteplay-audio] queueMs=20.0 endpointMs=10.0 delayMs=' + $audioDelay + ' targetMs=30.0 errorMs=0.0 driftPpm=0 ratioPpm=0 underruns=' + $i + ' resets=0 deviceChanges=0 pops=' + $i + ' concealed=0 hardResets=0 clicks=0 underrunStarts=0 queueP95=24.0' + $ownText)
        }
        # stream clarity: an arm with its own bitrate request logs the override, the console's target (health line) and what
        # arrived. The made-up console follows a request up to 30 Mbit/s and sends half its target: fixture numbers, not a PS5.
        if ($ArmSwitches -match 'VEYRA_TEST_RP_BITRATE_KBPS=(\d+)') {
            $askedKbps = [int]$Matches[1]
            $targetKbps = [math]::Min($askedKbps, 30000)
            $made += ('[INFO ] [remoteplay-clarity] test-override bitrateKbps=' + $askedKbps + ' savedBitrateKbps=15000 codec=hevc savedCodec=hevc (made up)')
            # the run's own health and progress lines with the console's target and a delivered rate added (no new made-up text)
            $healthLine = @($made | Where-Object { $_ -match '\[remoteplay-health\] grade=' })[0]
            $progressLine = @($made | Where-Object { $_ -match '\[remoteplay-progress\] state=3 ' })[0]
            $deliveredText = ($targetKbps / 2000.0).ToString('F3', [System.Globalization.CultureInfo]::InvariantCulture)
            for ($i = 0; $i -lt 3; $i++) {
                $made += ($healthLine + ' lateArrivals=0 routeClass=wired consoleRttMs=0.6 consoleTargetKbps=' + $targetKbps + ' consoleMeasuredKbps=-1 consoleReports=' + (12 * ($i + 1)) + ' consoleAgeMs=400')
                $made += ($progressLine -replace 'videoMbps=[0-9.]+', ('videoMbps=' + $deliveredText))
            }
        }
        if ($Purpose -eq 'quality') {
            $made += '[INFO ] [slipstream-sr] choice=2 quality=4 in=1920x1080 out=2560x1440 gpuP50=1.80 gpuP95=2.40 frames=900 generatedToo=0'
            $made += '[INFO ] [slipstream-fg] method=7 multiplier=3 tier=1 sourceFps=59.9 outputFps=179.6 generatedShown=1790 generatedExpired=4 skippedPairs=2 costP50=2.1 costP95=3.0 shownGapP95=5.9 shownGapMax=11.0 pairDelayMs=16.8'
        }
        if ($Purpose -eq 'network') {
            $mode = 'observe'
            if ($ArmName -ne 'n-observe') { $mode = 'apply' }
            if ($ArmName -eq 'n-off') { $mode = 'off' }
            for ($i = 0; $i -lt 16; $i++) {
                $reorder = 0
                if (($mode -eq 'apply') -and (($i % 3) -eq 0)) { $reorder = 4 }
                if ($mode -ne 'off') { $made += ('[INFO ] [slipstream-smartlink] profile=unstable-wifi regime=periodic-spikes risk=0.40 rttMs=' + (5 + $i) + ' rttMaxMs=79 targetKbps=15000 measuredKbps=9000 gapP95Ms=21.0 gapMaxMs=45.0 late=1 lost=0 reorderMs=' + $reorder + ' audioTargetMs=0 netLimited=1 headroom=0 signal=-1 mode=' + $mode + ' reasons=spikes') }
                $made += '[INFO ] [remoteplay-progress] state=3 received=300 decoded=300 inputFps=59.9 decodeFps=59.9 videoMbps=12.500 receiveAgeMs=3.0 decodeAgeMs=2.0 decodeBusyMs=1.0 queue=0 waitingIdr=false idrRequests=0 errors=0'
                $made += ('[INFO ] [slipstream-delivery] arrived=300 shown=298 replaced=2 gapP50=16.6 gapP95=21.0 gapMax=45.0 clumps=2 longGaps=' + ($i % 3) + ' longGapsEnd=0 shownGapP95=17.0 shownGapMax=33.0 policy=newest')
                $made += ('[INFO ] [slipstream-latency] frames=300 arrivalToScanoutP50=' + (10 + ($i % 4)) + '.5 arrivalToScanoutP95=14.0 arrivalToScanoutP99=16.0 arrivalToScanoutMax=30.0 scanoutKnown=290')
            }
            # SmartLink trace beside the app log (numbers only; with -DryRunBadTraceArm a planted word, which must be withheld)
            # Rotated like the app does at every stream start (openTrace): .1 -> .2, the current file -> .1, a new file.
            if ($ArmName -eq 'n-observe') {
                $trace = Join-Path $LogFolder 'smartlink-trace.csv'
                $one = Join-Path $LogFolder 'smartlink-trace.1.csv'; $two = Join-Path $LogFolder 'smartlink-trace.2.csv'
                if (Test-Path -LiteralPath $two) { Remove-Item -LiteralPath $two -Force }
                if (Test-Path -LiteralPath $one) { Move-Item -LiteralPath $one -Destination $two -Force }
                if (Test-Path -LiteralPath $trace) { Move-Item -LiteralPath $trace -Destination $one -Force }
                $row = '200,200,2,120,0,1,0,1.6,9,12,0,0,16.6,0,0,0,5,15000,9000,15000,0,20,0,80,-55,866,5,0.4'
                $rows = @($traceHeader, $row, $row.Replace(',16.6,', ',-1,'), $row.Replace(',5,15000,', ',79,15000,'))
                if ($DryRunBadTraceArm -eq $ArmName) { $rows += $row.Replace(',16.6,', ',PlantedTraceWord,') }
                Set-Content -LiteralPath $trace -Value $rows -Encoding ASCII
            }
        }
        if ($Purpose -eq 'probe') {
            for ($i = 0; $i -lt 3; $i++) { $made += '[INFO ] [remoteplay-cadence] state=locked windowS=10.0 streamFps=59.94 sourceFps=30.00 repeatsFound=290 repeatsRemoved=0 keptUnsure=0 unmeasured=0 exactVetoes=10 suspendedKeeps=0 (made up; duplicate removal off)' }
        }
        $made += '[INFO ] [remoteplay-clock] wireIndex=600 ptsMs=11241.417 arrivalMinusPtsMs=58.931 nominalFps=60 (arrival-disciplined local estimate, not PS5 native PTS)'
        $made += '[INFO ] [remoteplay-clock] wireIndex=1200 ptsMs=21291.397 arrivalMinusPtsMs=165.675 nominalFps=60 (arrival-disciplined local estimate, not PS5 native PTS)'
        if ($ArmExe -eq 'experimental') {
            # Kit 5: the lines of S5_CONTRACT.md (interfaces 1 to 7), exactly as the contract writes them. Made up so that every
            # verdict of the MEASURED VALUES section runs: v-clean-dlss better, v-clean-neural-dlss worse, v-clean-nis cleaner but
            # over the processing budget, v-neural with a missing runtime (refused keys, no processing time).
            $runtimeLine = '[INFO ] [nr-runtime-status] original=present originalVersion=310.8.0.0 originalSigned=1 sf=absent selected=nr-original selectedAvailable=1 gpuSupportsOriginal=1 reason=none originalTested=1 resolvedDefault=0'
            if ($ArmName -eq 'v-neural') { $runtimeLine = '[INFO ] [nr-runtime-status] original=absent originalVersion=none originalSigned=0 sf=absent selected=nr-original selectedAvailable=0 gpuSupportsOriginal=1 reason=runtime-missing originalTested=0 resolvedDefault=0' }
            $made += $runtimeLine
            for ($i = 0; $i -lt 3; $i++) { $made += '[INFO ] [slipstream-scanout] presents=300 samples=300 failed=0 disjoint=0 noSync=0 unchanged=250 named=50 exact=40 gapRejected=0 passedVsync=250 passedSync0=0 refreshShort=3 unsettled=0 stepP50=6 stepMax=9 statsAgeMsP50=30.0 statsAgeMsMax=45.0 namedPct=16.7 syncInterval=1' }
            # Stream network: the [slipstream-net] line (NetDiagnostics.h; VEYRA_EXP_RP_STATS=1, which every kit arm sets), made up.
            for ($i = 0; $i -lt 3; $i++) { $made += '[INFO ] [slipstream-net] window=5.0 codec=hevc resolution=1920x1080 resolutionChanges=0 fps=59.9 requestedKbps=30000 kbps=28100 kbpsMin1s=24000 kbpsMax1s=31000 consoleTargetKbps=-1 packets=9000 missing=0 late=0 missingNotLate=0 lossPct=0.000 reordered=0 dropOther=0 reorderTimeouts=0 fecRecovered=0 fecFailed=0 fecRecoveryPct=-1.0 framesLost=0 recoveries=0 keyframeRequests=0 ingressDropped=0 ingressPeak=1 mailboxOverwrites=0 frames=300 jitterMs=0.40 jitterPeakMs=1.20 gapMaxMs=17.0 burstMax=1 netToDecodeP50Ms=1.10 netToDecodeP95Ms=1.80 netToDecodeP99Ms=2.40 decodeP95Ms=1.80 waitingIdrMs=0 udpInDatagrams=9100 udpInErrors=0 nicPackets=9400 nicDiscards=0 nicErrors=0 route=on-link routeClass=wired family=ipv4 bottleneck=none signs=none counterWrap=0 (software counters; udp*: whole PC; nic*: the console''s adapter; -1 = not available)' }
            if ($ArmSwitches -match 'VEYRA_EXP_PRESENT_WINDOW_FACTS=1') {
                $regionKind = '3'; $regionRects = '56'; $rectFlag = '0'
                if ($ArmSwitches -match 'VEYRA_EXP_PRESENT_RECT_REGION=1') { $regionKind = '2'; $regionRects = '1'; $rectFlag = '1' }
                $made += ('[INFO ] [slipstream-present] window event=change known=1 host=640,296,1280x720 monitor=0,0,2560x1440 coversMonitor=0 regionKind=' + $regionKind + ' regionRects=' + $regionRects + ' overlay=1 ownOverlaps=1 rootPopup=1 rootCaption=0 rootMaximized=0 rootTopmost=0 rootLayered=0 hostLayered=0 hostVisible=1 foreground=0 flipBlockers=0xF')
                $made += ('[INFO ] [slipstream-present] covers count=2 inPicture=1 rectRegion=' + $rectFlag + ' items=PushModeRow/modeRow,PushLatencyBadge/latencyBadge')
            }
            $pipelineKeys = @(); $pipelineText = ''
            foreach ($pair in ($ArmSwitches -split ';')) { if ($pair.StartsWith('pipeline:')) { $pipelineText = $pair.Substring(9) } }
            if ($pipelineText) { foreach ($item in ($pipelineText -split '\|')) { $pipelineKeys += ($item -split '=')[0] } }
            if ($pipelineKeys.Count -gt 0) {
                $detail = @(); $refusedKeys = 0
                foreach ($key in $pipelineKeys) {
                    if (($ArmName -eq 'v-neural') -and (@('nr', 'nrRuntime', 'nrLook') -contains $key)) { $detail += ($key + ':refused:runtime-missing'); $refusedKeys++ }
                    else { $detail += ($key + ':ok') }
                }
                $made += ('[INFO ] [slipstream-pipeline] applied=' + ($pipelineKeys.Count - $refusedKeys) + ' refused=' + $refusedKeys + ' detail=' + ($detail -join ',') + ' streamHdr=0 videoHdr=' + $hdrFlag)
                $graphP95 = '5.00'
                if ($ArmName -eq 'v-clean-nis') { $graphP95 = '9.50' }
                if ($ArmName -ne 'v-neural') {
                    for ($i = 0; $i -lt 3; $i++) { $made += ('[INFO ] [slipstream-nr] frames=300 layers=1 internal=1920x1080 output=2560x1440 nrMsP50=3.10 nrMsP95=4.20 srMsP50=1.20 srMsP95=1.80 fgMsP50=-1 fgMsP95=-1 graphMsP50=4.60 graphMsP95=' + $graphP95 + ' copyCount=1 vramMiB=3100 budgetMiB=11000 runtime=nr-original version=310.8.0.0') }
                }
                if ($pipelineKeys -contains 'clean') { for ($i = 0; $i -lt 2; $i++) { $made += '[INFO ] [slipstream-clean] frames=300 strength=50 size=1920x1080 gpuMsP50=0.40 gpuMsP95=0.60 hudProtect=1 hdr=0' } }
                if ($ArmName -like 'v-*') {
                    # as real runs: v-original (and the neural layer alone) shows the stream's own 1920x1080 and a HUD delta of
                    # about 0; the upscaled arms 2560x1440, where blockiness is not comparable (the scaler blurs the grid)
                    $outBlock = '0.200'; $outRing = '0.050'; $outBand = '0.100'; $outSize = '2560x1440'; $outHud = '0.012'
                    if (($ArmName -eq 'v-original') -or ($ArmName -eq 'v-neural')) { $outSize = '1920x1080'; $outHud = '0.000' }
                    if ($ArmName -eq 'v-clean') { $outSize = '1920x1080'; $outHud = '0.004' }
                    if (($ArmName -eq 'v-clean') -or ($ArmName -eq 'v-clean-dlss') -or ($ArmName -eq 'v-clean-nis')) { $outBlock = '0.120'; $outBand = '0.080' }
                    if ($ArmName -eq 'v-clean-neural-dlss') { $outRing = '0.080' }
                    $made += '[INFO ] [slipstream-quality] frames=20 size=1920x1080 blockiness=0.200 banding=0.100 ringing=0.050 detail=0.500 hudDelta=0.010 flicker=0.020 meanLuma=0.400 hdr=0 stage=raw'
                    $made += ('[INFO ] [slipstream-quality] frames=20 size=' + $outSize + ' blockiness=' + $outBlock + ' banding=' + $outBand + ' ringing=' + $outRing + ' detail=0.500 hudDelta=' + $outHud + ' flicker=0.020 meanLuma=0.400 hdr=0 stage=output')
                }
                if ($pipelineText -match '\|fg=(vfg|dlss|pushflow|fsr)') {
                    # frame generation on: the frame counters rise for real and generated frames alike
                    $flowLine = [string]@($made | Where-Object { $_ -match '\[frame-flow\]' })[0]
                    $made += ($flowLine -replace 'generatedPresented=0', 'generatedPresented=600')
                    $made += (($flowLine -replace 'realPresented=600', 'realPresented=1200') -replace 'generatedPresented=0', 'generatedPresented=1200')
                    $made += '[INFO ] [slipstream-fg] method=7 multiplier=2 tier=1 sourceFps=59.9 outputFps=119.8 generatedShown=1190 generatedExpired=4 skippedPairs=2 costP50=2.1 costP95=3.0 shownGapP95=8.9 shownGapMax=17.0 pairDelayMs=16.8'
                }
            }
            if ($Purpose -eq 'measure') {
                $inputValues = ' context=walk profile=builtin mouseEvents=5000 mouseHz=1000 rawToTranslateUsP50=12.0 rawToTranslateUsP95=30.0 rawToTranslateUsP99=55.0 translateUsP50=2.0 translateUsP95=4.0 translateUsP99=6.0 stateToSendMsP50=2.00 stateToSendMsP95=3.90 stateToSendMsP99=4.20 eventToSendMsP50=2.10 eventToSendMsP95=4.00 eventToSendMsP99=4.40 eventToSendMsMax=6.00 queueAgeMsP95=0.20 queueDepthMax=1 reports=1250 sendHz=250 sendGapMsP50=4.00 sendGapMsP95=4.30 sendGapMsMax=7.00 smoothingMs=0.0 saturatedPct=1.5 curve=linear switches=0 stuckReleases=0 heldAtLoss=0 fallback=none'
                $padValues = ' context=none profile=builtin mouseEvents=0 mouseHz=-1 rawToTranslateUsP50=-1 rawToTranslateUsP95=-1 rawToTranslateUsP99=-1 translateUsP50=-1 translateUsP95=-1 translateUsP99=-1 stateToSendMsP50=1.90 stateToSendMsP95=3.80 stateToSendMsP99=4.10 eventToSendMsP50=2.00 eventToSendMsP95=3.90 eventToSendMsP99=4.30 eventToSendMsMax=5.50 queueAgeMsP95=0.10 queueDepthMax=1 reports=1250 sendHz=250 sendGapMsP50=4.00 sendGapMsP95=4.20 sendGapMsMax=6.50 smoothingMs=0.0 saturatedPct=0.0 curve=none switches=0 stuckReleases=0 heldAtLoss=0 fallback=none'
                $inputProfile = 'legacy'
                if ($ArmSwitches -match 'VEYRA_EXP_RP_KBM_INPUT_PROFILE=([a-z-]+)') {
                    $inputProfile = [string]$Matches[1]
                    for ($i = 0; $i -lt 2; $i++) { $made += ('[INFO ] [slipstream-input] source=kbm inputProfile=' + $inputProfile + $inputValues) }
                    for ($i = 0; $i -lt 2; $i++) { $made += ('[INFO ] [slipstream-input] source=pad inputProfile=' + $inputProfile + $padValues) }
                    # the interface thread's own poll timing (S5_INPUT_API.md, change 11): written with [slipstream-input]
                    for ($i = 0; $i -lt 4; $i++) { $made += '[INFO ] [slipstream-input-poll] polls=1250 pollMs=4 pollGapMsP50=4.000 pollGapMsP95=4.400 pollGapMsP99=5.100 pollGapMsMax=9.000 latePolls=2 rawBacklogMax=3' }
                } else {
                    $made += ('[INFO ] [slipstream-input] source=none inputProfile=' + $inputProfile + ($padValues -replace '=[0-9.]+ ', '=-1 '))
                }
            }
        }
        $made += '[INFO ] [slipstream-autorun] finished ok=1 steps=4'
    }
    # the smoothness session: the present stream's 5 s cadence line (S5_PRESENT_API.md), three windows per run
    if ($ArmName -like 'sm-*') {
        $vrrWord = '1'; $offText = '0.5'; $h1 = '0'; $h2 = '0'; $h3 = '300'; $unevenText = '0.0'; $subFps = '59.90'; $genFps = '0.00'
        if ($ArmName -ne 'sm-native60') { $subFps = '119.80'; $genFps = '59.90'; $h2 = '600'; $h3 = '0'; $offText = '1.0' }
        if ($ArmName -eq 'sm-fg2-novrr') { $vrrWord = '0'; $h1 = '300'; $h2 = '300'; $unevenText = '50.0' }
        if ($ArmName -eq 'sm-fg3') { $subFps = '179.70'; $genFps = '119.80'; $h1 = '900'; $h2 = '0'; $offText = '0.8' }
        if ($ArmName -eq 'sm-fg2-cap90') { $offText = '5.0' }
        if ($ArmName -eq 'sm-mode-90') {
            # VRR changes once during the runs: the run's own [display-vrr] line again, saying not active
            $vrrWord = '-1'
            $vrrLine = [string]@($made | Where-Object { $_ -match '\[display-vrr\]' })[0]
            $made += (($vrrLine -replace ' active=true ', ' active=false ') -replace ' displayInVrrMode=true ', ' displayInVrrMode=false ')
        }
        # stream smooth's 5 s line (S5_PRESENT_API.md, Smoothness): the working size follows the arm's upscaler target
        # no upscaler 1920x1080 (-1 -1); 4K asked 3840x2160 (1 1), lowered to the monitor by SR_DISPLAY 2560x1440 (0 1)
        $srAsked = '-1'
        if ($ArmSwitches -match '\|sr=(dlss|vsr-stream|nis)') { if ($ArmSwitches -match 'srTarget=4k') { $srAsked = '1' } else { $srAsked = '0' } }
        $srUsed = $srAsked
        if (($srAsked -eq '1') -and ($ArmSwitches -match 'VEYRA_EXP_RP_SMOOTH_SR_DISPLAY=1')) { $srUsed = '0' }
        $work = '1920x1080'
        if ($srUsed -eq '0') { $work = '2560x1440' } elseif ($srUsed -eq '1') { $work = '3840x2160' }
        $smoothLine = '[INFO ] [slipstream-smooth] window=5.0 backend=vfg ceiling=3X chosen=3X chosenMin=3X chosenMax=3X changes=0 why=cap contentFps=59.94 capFps=179.95 refreshHz=179.950 vrr=1 source=1920x1080 work=' + $work + ' flow=1920x1080 display=2560x1440 srTarget=' + $srUsed + ' srTargetAsked=' + $srAsked + ' receivedFps=59.94 uniqueFps=59.94 evaluatedPairsFps=59.70 rejectedPairsFps=0.24 fastPairsFps=0.00 generatedFps=119.40 invalidFps=0.00 dropCapFps=0.00 dropDeadlineFps=0.40 dropCapacityFps=0.00 resets=0 holds=0 gpuColorP50Ms=0.200 gpuColorP95Ms=0.300 gpuCleanP50Ms=-1.000 gpuCleanP95Ms=-1.000 gpuSrP50Ms=2.000 gpuSrP95Ms=2.400 gpuNrP50Ms=-1.000 gpuNrP95Ms=-1.000 gpuFlowP50Ms=-1.000 gpuFlowP95Ms=-1.000 gpuFgP50Ms=6.000 gpuFgP95Ms=7.000 gpuBlitP50Ms=0.300 gpuBlitP95Ms=0.400 gpuSpanP50Ms=9.000 gpuSpanP95Ms=10.500 gpuFrames=299 pairBudgetMs=16.683 gpuLoadPct=54.0 smooth=0 smoothReason=off stepDowns=0 stepUps=0 srFollow=0 capFit=0 steady=0 realAgeP50Ms=26.500 realAgeP95Ms=29.000 realAgeP99Ms=31.000 addedEstMs=12.400 realHoldP95Ms=13.800 realHoldP99Ms=14.600 phaseMs=17.900 phaseLearned=0 firstReadyP50Ms=6.100 firstReadyP95Ms=7.400 presentLateP95Ms=0.300 presentLateP99Ms=0.600 deadPhaseFps=0.240 fallbackFps=0.000 (counts before Present; what reached the panel: [slipstream-cadence] / PresentMon; gpuLoadPct is a model)'
        for ($i = 0; $i -lt 3; $i++) { $made += $smoothLine }
        for ($i = 0; $i -lt 3; $i++) { $made += ('[INFO ] [slipstream-cadence] window=5 submittedFps=' + $subFps + ' realFps=59.90 generatedFps=' + $genFps + ' intervals=300 intervalP50Ms=8.33 intervalP95Ms=9.10 intervalP99Ms=10.20 intervalMaxMs=16.90 offPct=' + $offText + ' refreshHz=179.95 vrr=' + $vrrWord + ' fixedHold1=' + $h1 + ' fixedHold2=' + $h2 + ' fixedHold3=' + $h3 + ' fixedUnevenPct=' + $unevenText + ' fixedShownFps=119.80 evenFps=89.98 evenDivisor=2 overflow=0') }
    }
    # the capture session (stream capture): the capture card's own lines, made up (a 30 FPS game in a 60 Hz signal; dedup on:
    # 30 pairs a second; dedup off: 60, half of them two identical pictures; latency (M-1)/M x 33.3 ms + 5 ms with generation)
    if ($ArmName -like 'cap-*') {
        $inv = [System.Globalization.CultureInfo]::InvariantCulture
        $mult = 1
        $fgOn = ($ArmSwitches -match '\|fg=(vfg|dlss|pushflow|fsr)')
        if ($fgOn -and ($ArmSwitches -match '\|fgMultiplier=([0-9])')) { $mult = [int]$Matches[1] }
        $dedupOn = ($ArmSwitches -notmatch '\|dedup=off')
        $pairs = 60.0
        if ($dedupOn) { $pairs = 30.0 }
        $gen = $pairs * ($mult - 1); $still = 0.0; $stillPairs = 0.0
        if (-not $dedupOn) { $stillPairs = 30.0; $still = 30.0 * ($mult - 1) }
        $latMs = 1.1
        if ($mult -gt 1) { $latMs = 1000.0 / 30.0 * ($mult - 1) / $mult + 5.0 }
        $buffers = '3'; $bufferMode = 'auto'
        if ($ArmSwitches -match 'pref:captureBufferMode=1') { $buffers = '1'; $bufferMode = 'minimum' }
        $dedupFlag = '0'
        if ($dedupOn) { $dedupFlag = '1' }
        $made += '[INFO ] [slipstream-capture] event=card card=Made_Up_Card_4K modes=24 max60=3840x2160 max120=1920x1080 max60Any=3840x2160 max120Any=2560x1440 tenBit=1 maxFps=120.00 pixels=NV12+YUY2+P010+MJPEG (made up)'
        $made += ('[INFO ] [capture-buffer] mode=' + $bufferMode + ' requested=' + $buffers + ' actual buffers=' + $buffers + ' bytes=3110400 align=1 hr=0x00000000 (driver honored)')
        for ($i = 0; $i -lt 2; $i++) { $made += ('[INFO ] [capture-driver-age] frames=600 p50Ms=12.4 p95Ms=14.0 minMs=10.9 maxMs=17.2 driverBuffers=' + $buffers + ' (made up)') }
        $window = ' pictures=1 transportFps=59.94 readFps=59.90 newFps=29.97 repeatFps=29.93 missedNewFps=0.00 mailboxDropFps=0.04 pairsFps=' + $pairs.ToString('F2', $inv) +
            ' stillPairsFps=' + $stillPairs.ToString('F2', $inv) + ' genFps=' + $gen.ToString('F2', $inv) + ' stillGenFps=' + $still.ToString('F2', $inv) +
            ' motionGenFps=' + ($gen - $still).ToString('F2', $inv) + ' fgHeldPct=0.0 realLatN=150 realLatP50Ms=' + $latMs.ToString('F2', $inv) +
            ' realLatP95Ms=' + ($latMs + 1.2).ToString('F2', $inv) + ' realLatP99Ms=' + ($latMs + 2.0).ToString('F2', $inv) + ' (made up)'
        for ($i = 0; $i -lt 3; $i++) { $made += ('[INFO ] [slipstream-capture] window=5.0s mode=1920x1080@59.94 format=nv12 compressed=0 hdr=0 dedup=' + $dedupFlag + $window) }
        $real = 29.97
        if (-not $dedupOn) { $real = 59.94 }
        $shown = $real * $mult
        $interval = 1000.0 / $shown
        for ($i = 0; $i -lt 3; $i++) { $made += ('[INFO ] [slipstream-cadence] window=5 submittedFps=' + $shown.ToString('F2', $inv) + ' realFps=' + $real.ToString('F2', $inv) + ' generatedFps=' + ($shown - $real).ToString('F2', $inv) + ' intervals=300 intervalP50Ms=' + $interval.ToString('F2', $inv) + ' intervalP95Ms=' + ($interval + 0.8).ToString('F2', $inv) + ' intervalP99Ms=' + ($interval + 1.5).ToString('F2', $inv) + ' intervalMaxMs=' + ($interval + 4.0).ToString('F2', $inv) + ' offPct=0.5 refreshHz=179.95 vrr=1 fixedHold1=0 fixedHold2=0 fixedHold3=0 fixedUnevenPct=0.0 fixedShownFps=' + $shown.ToString('F2', $inv) + ' evenFps=' + $shown.ToString('F2', $inv) + ' evenDivisor=1 overflow=0 pauses=0 (made up)') }
        for ($i = 0; $i -lt 3; $i++) { $made += ('[INFO ] [slipstream-smooth] window=5.0 backend=vfg ceiling=' + $mult + 'X chosen=' + $mult + 'X chosenMin=' + $mult + 'X chosenMax=' + $mult + 'X changes=0 why=user contentFps=29.97 capFps=179.95 refreshHz=179.950 vrr=1 source=1920x1080 work=1920x1080 flow=1920x1080 display=2560x1440 srTarget=-1 srTargetAsked=-1 receivedFps=59.94 uniqueFps=' + $pairs.ToString('F2', $inv) + ' rejectedPairsFps=0.00 generatedFps=' + $gen.ToString('F2', $inv) + ' dropCapFps=0.00 dropDeadlineFps=0.10 holds=0 gpuSpanP95Ms=9.50 gpuFgP95Ms=7.20 gpuSrP95Ms=-1 pairBudgetMs=33.33 realAgeP50Ms=-1.000 realAgeP95Ms=-1.000 realAgeP99Ms=-1.000 addedEstMs=24.500 realHoldP95Ms=26.000 realHoldP99Ms=27.200 phaseMs=29.500 phaseLearned=1 firstReadyP50Ms=7.500 firstReadyP95Ms=9.000 presentLateP95Ms=0.200 presentLateP99Ms=0.400 deadPhaseFps=0.000 fallbackFps=0.000 (made up)') }
    }
    # kit 5: two more seconds of display timing well after the first one, so the steady state (after 3 s) has values too
    for ($i = 0; $i -lt 2; $i++) { $made += '[INFO ] [display-latency] presentToScanoutP50Ms=1.200 presentToScanoutP95Ms=3.900 presentToScanoutMaxMs=5.100 samples=9 replacedAtLeast=0 compositionMode=2 refreshHz=180.000 (made up)' }
    # The Windows user name in a line that is otherwise copied: it must come out as [removed] (Hide-OwnWords).
    if ($env:USERNAME -and ($env:USERNAME.Length -ge 3)) { $made += ('[INFO ] [remoteplay-kbm] made-up profile owner ' + $env:USERNAME + ' end') }
    if ($Purpose -eq 'impairment') { $made += '[WARN ] [remoteplay-recovery] request keyframe; received=900 decoded=880 videoCallbacks=900 rejected=0 packetReceived=4000 packetLost=37' }
    # kit 5, rolling releases: a build without a feature (-DryRunWithout components, -DryRunNoNeural)
    if ($DryRunWithout -or $DryRunNoNeural) {
        $dropped = @(@($DryRunWithout -split ',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        if ($DryRunNoNeural) { $dropped += 'nr-runtime-status' }
        $keep = @()
        foreach ($line in $made) {
            $drop = $false
            foreach ($component in $dropped) { if (([string]$line).IndexOf('] [' + $component + '] ') -ge 0) { $drop = $true } }
            if (-not $drop) {
                if ($DryRunNoNeural) { $keep += ([string]$line -replace '\b(nr|nrRuntime|nrLook|nrSize):(ok|refused:runtime-missing)', '$1:refused:unknown-key') }
                else { $keep += [string]$line }
            }
        }
        $made = $keep
    }
    $out = @()
    $n = 0
    foreach ($line in $made) {
        $out += ($now.AddMilliseconds(200 * $n).ToString('yyyy-MM-ddTHH:mm:ss.fffZ') + ' t=1234 ' + $line)
        $n++
    }
    Add-Content -LiteralPath (Join-Path $LogFolder 'veyra-qml.log') -Value $out -Encoding UTF8
}

# --- live log reading, the settings of a run, and the one-page result ----------------------------------------
$invariant = [System.Globalization.CultureInfo]::InvariantCulture

# A number with a dot whatever the language of the PC; 'n/a' for none.
function Format-Number($Value, [int]$Digits) {
    if ($null -eq $Value) { return 'n/a' }
    $number = [double]$Value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { return 'n/a' }
    return $number.ToString('F' + $Digits, $invariant)
}

# A JSON value: rounded, or null for none (Windows PowerShell writes NaN, which is not JSON).
function Get-JsonNumber($Value) {
    if ($null -eq $Value) { return $null }
    $number = [double]$Value
    if ([double]::IsNaN($number) -or [double]::IsInfinity($number)) { return $null }
    return [math]::Round($number, 2)
}

# The scene note as typed once by the tester: plain letters, digits and simple punctuation, no private word, at
# most 80 characters, and nothing the address masking would change.
function Protect-Note([string]$Text) {
    if (-not $Text) { return '' }
    $t = $Text -replace '[^A-Za-z0-9 ,.()/+-]', ' '
    $t = $t -replace $refusedLine, '*'
    $t = ($t -replace '\s{2,}', ' ').Trim()
    if ($t.Length -gt 80) { $t = $t.Substring(0, 80).Trim() }
    return (Hide-OwnWords (Protect-Text $t))
}

# Whole new lines of every *.log since the offsets (file -> bytes already read), which are moved on. A line that is
# still being written stays for the next call. At most 4 MB per file and call.
function Read-LiveLines([string]$LogFolder, $Offsets) {
    $found = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $LogFolder)) { return $found.ToArray() }
    foreach ($file in @(Get-ChildItem -LiteralPath $LogFolder -Filter '*.log' | Sort-Object LastWriteTime)) {
        $from = 0
        if ($Offsets.ContainsKey($file.FullName) -and ($file.Length -ge $Offsets[$file.FullName])) { $from = $Offsets[$file.FullName] }
        $count = $file.Length - $from
        if ($count -le 0) { continue }
        if ($count -gt 4194304) { $count = 4194304 }
        $stream = $null
        try {
            $stream = [System.IO.File]::Open($file.FullName, 'Open', 'Read', 'ReadWrite')
            [void]$stream.Seek($from, 'Begin')
            $buffer = New-Object byte[] ([int]$count)
            $read = $stream.Read($buffer, 0, [int]$count)
            if ($read -gt 0) {
                $end = [Array]::LastIndexOf($buffer, [byte]10, $read - 1)
                if ($end -ge 0) {
                    $text = [System.Text.Encoding]::UTF8.GetString($buffer, 0, $end + 1)
                    $Offsets[$file.FullName] = $from + $end + 1
                    foreach ($line in ($text -split "`r?`n")) { if ($line) { $found.Add($line) } }
                }
            }
        } catch {
            $found.Add('')
        } finally {
            if ($stream) { $stream.Dispose() }
        }
    }
    return $found.ToArray()
}

# --- every observable setting of a run (sprint 4) ---------------------------------------------------------------
# The same key names and rules as tools/remoteplay_latency_bench/report.py (infer_config, normal_config,
# config_refusal): change both together. A build that logs [slipstream-config] gives its keys directly; the original
# and the stable build give what their existing lines show ([pacing], [display-vrr], [display-refresh], the session
# line, [remoteplay-decode], [frame-flow]). Not inferable without that line: mode, fgMethod, fgMultiplier, fgTier, sr,
# srQuality, enhance, nvLowLatency, smoothPacing, fullscreen, window, output, hdr, trialsMax.
$wifiLabel = 'Wi-Fi: diagnostic only, not a wired-LAN latency conclusion'
$configIgnored = @('route', 'trialsMax')
$configFix = @{
    lowQueue = 'the low-latency queue differs: let the kit start every copy with its test profile (do not use -NoTestProfile), or set it the same in both copies'
    displaySync = 'display sync differs: use the kit''s test profile, or set display sync the same in both copies'
    vrrRequested = 'the variable refresh request differs: use the kit''s test profile, or set smooth frame pacing the same in both copies'
    vrrActive = 'variable refresh was active in one arm only: run both full screen on the same display (the test profile turns full screen on at connect)'
    presentWaitable = 'the present queue differs (DXGI waitable): use the kit''s test profile in both copies'
    presentMaxLatency = 'the DXGI frame latency differs: use the kit''s test profile in both copies'
    presentVsync = 'V-Sync differs: use the kit''s test profile in both copies'
    presentTearing = 'tearing differs: use the kit''s test profile in both copies'
    stream = 'the stream resolution or frame rate differs: choose the same quality in the PS5 connect dialog of both copies'
    codec = 'the codec differs: choose the same codec in the PS5 connect dialog of both copies'
    bitrateKbps = 'the bitrate differs: choose the same bitrate in the PS5 connect dialog of both copies'
    decoder = 'the decoder differs: choose the same decoder in the PS5 connect dialog of both copies'
    refreshHz = 'the refresh rate differs: keep the display at the same refresh rate for every run'
    fg = 'frame generation differs: turn it off (or on) in both copies before connecting'
    sr = 'super resolution differs: set it the same in both copies'
    enhance = 'picture enhancement differs: set it the same in both copies'
}

function ConvertTo-Flag([string]$Value) {
    $v = ([string]$Value).Trim().ToLowerInvariant()
    if (@('1', 'true', 'on', 'yes') -contains $v) { return '1' }
    if (@('0', 'false', 'off', 'no') -contains $v) { return '0' }
    return $v
}

function ConvertTo-Codec([string]$Value) {
    $v = ([string]$Value).Trim().ToLowerInvariant()
    if (($v -eq 'h265') -or ($v -eq 'hevc') -or ($v -eq 'h265_hdr')) { return 'hevc' }
    if (($v -eq 'h264') -or ($v -eq 'avc')) { return 'h264' }
    return $v
}

# key=value words of a log text (the last of a repeated key wins), as report.py words().
function Get-LineWords([string]$Text) {
    $table = @{}
    foreach ($m in [regex]::Matches($Text, '(\w+)=([^\s,;()]+)')) { $table[$m.Groups[1].Value] = $m.Groups[2].Value }
    return $table
}

function Get-HzText([string]$Value) {
    $hz = Read-Number $Value
    if (($null -eq $hz) -or ($hz -le 0)) { return '' }
    return [string][int][math]::Round($hz, 0)
}

# Updates the inferred keys from one line (component and text after it).
function Update-ConfigKeys($Keys, [string]$Component, [string]$Text) {
    if ($Component -eq 'pacing') {
        if ($Text -match '^revision=\d+ requested=(\w+)/(\d+)/(\d+)') {
            $Keys['lowQueue'] = ConvertTo-Flag $Matches[1]; $Keys['pacingMode'] = $Matches[2]; $Keys['displaySync'] = $Matches[3]
            $w = Get-LineWords $Text
            if ($w.ContainsKey('variableRefresh')) { $Keys['vrrRequested'] = ConvertTo-Flag $w['variableRefresh'] }
            if ($w.ContainsKey('displayQueuePaced')) { $Keys['displayQueuePaced'] = ConvertTo-Flag $w['displayQueuePaced'] }
        } elseif ($Text.StartsWith('DXGI ')) {
            $w = Get-LineWords $Text
            if ($w.ContainsKey('enabled')) { $Keys['presentWaitable'] = ConvertTo-Flag $w['enabled'] }
            if ($w.ContainsKey('maximumLatency')) { $Keys['presentMaxLatency'] = $w['maximumLatency'] }
            if ($w.ContainsKey('vsync')) { $Keys['presentVsync'] = ConvertTo-Flag $w['vsync'] }
            if ($w.ContainsKey('tearing')) { $Keys['presentTearing'] = ConvertTo-Flag $w['tearing'] }
        }
    } elseif ($Component -eq 'display-vrr') {
        $w = Get-LineWords $Text
        if ($w.ContainsKey('active')) { $Keys['vrrActive'] = ConvertTo-Flag $w['active'] }
        if ($w.ContainsKey('level')) { $Keys['vrrLevel'] = $w['level'] }
        $hz = Get-HzText ([string]$w['refreshHz'])
        if ($hz) { $Keys['refreshHz'] = $hz }
    } elseif (($Component -eq 'display-query') -and $Text.StartsWith('result ') -and ($Text -match ' hdr=(\S+)')) {
        # The monitor's HDR mode (every build logs it; the experimental build's hdr= is the stream's, a different thing).
        $Keys['displayHdr'] = ConvertTo-Flag $Matches[1]
    } elseif (($Component -eq 'display-refresh') -and $Text.StartsWith('monitor ')) {
        $w = Get-LineWords $Text
        $hz = Get-HzText ([string]$w['refresh'])
        if ($hz) { $Keys['refreshHz'] = $hz }
    } elseif (($Component -eq 'remoteplay') -and $Text.StartsWith('session started')) {
        $w = Get-LineWords $Text
        if ($w['profile']) { $Keys['stream'] = $w['profile'] }
        if ($w['codec']) {
            $Keys['codec'] = ConvertTo-Codec $w['codec']
            if (([string]$w['codec']).ToLowerInvariant() -eq 'h265_hdr') { $Keys['hdr'] = '1' } else { $Keys['hdr'] = '0' }   # the stream's HDR request, as hdr= of [slipstream-config]
        }
        if ($w['requestedBitrateKbps']) { $Keys['bitrateKbps'] = $w['requestedBitrateKbps'] }
    } elseif (($Component -eq 'remoteplay-decode') -and $Text.StartsWith('actual=')) {
        $w = Get-LineWords $Text
        if ([string]$w['fallback'] -eq 'true') { $Keys['decoder'] = 'software' } else { $Keys['decoder'] = ([string]$w['actual']).ToLowerInvariant() }
    } elseif (($Component -eq 'ui-fullscreen') -and $Text.StartsWith('enabled=')) {
        $w = Get-LineWords $Text
        $Keys['fullscreen'] = ConvertTo-Flag ([string]$w['enabled'])   # logged by the stable build's interface too: verifies the profile
    } elseif ($Component -eq 'frame-flow') {
        $generated = Get-LogNumber $Text 'generatedPresented'
        if (($null -ne $generated) -and (($generated -gt 0) -or (-not $Keys.ContainsKey('fg')))) { if ($generated -gt 0) { $Keys['fg'] = '1' } else { $Keys['fg'] = '0' } }
    } elseif ($Component -eq 'fg-validity') {
        $valid = Get-LogNumber $Text 'valid'
        if (($null -ne $valid) -and ($valid -gt 0)) { $Keys['fg'] = '1' }
    }
}

# The [slipstream-config] keys over the inferred ones, normalised as report.py normal_config.
# One value in the form every build agrees on, $null for unknown (-1, unknown, empty): report.py normal_value.
$hardwareDecoders = @('hardware', 'd3d12va', 'd3d11va', 'dxva2', 'dxva', 'nvdec', 'cuda', 'qsv', 'vulkan', 'amf', 'videotoolbox', 'mediafoundation')
$softwareDecoders = @('software', 'software-fallback', 'ffmpeg', 'libavcodec', 'cpu')
$flagKeys = @('fg', 'enhance', 'lowQueue', 'smoothPacing', 'vrrRequested', 'vrrActive', 'fullscreen', 'hdr', 'displayHdr', 'presentWaitable', 'presentVsync', 'presentTearing', 'displayQueuePaced')
$latencyKeys = @('lowQueue', 'nvLowLatency', 'displaySync', 'smoothPacing', 'vrrRequested', 'fullscreen', 'fg', 'fgMethod', 'fgMultiplier', 'sr', 'srQuality', 'enhance', 'hdr',
                 'presentWaitable', 'presentMaxLatency', 'presentVsync', 'presentTearing', 'stream', 'decoder')
$trackedKeys = $latencyKeys + @('codec', 'bitrateKbps')
$profileKeys = @('nvLowLatency', 'smoothPacing')
$configComponents = @('pacing', 'display-query', 'display-vrr', 'display-refresh', 'remoteplay', 'remoteplay-decode', 'frame-flow', 'fg-validity', 'slipstream-config', 'ui-fullscreen')
# report.py ASSUMED_KEYS / FG_DETAIL_KEYS / settings_that_matter: settings a build may not log, taken as the test profile
# sets them when both arms ran with it and the side that logs them shows that value ("assumed from the test profile").
$assumedKeys = [ordered]@{ fullscreen = '1'; sr = '0'; enhance = '0' }
$fgDetailKeys = @('fgMethod', 'fgMultiplier', 'fgTier')
function Get-SettingsThatMatter($Mine, $Theirs, [bool]$Profiled) {
    $a = @{}; foreach ($k in @($Mine.Keys)) { $a[$k] = $Mine[$k] }
    $b = @{}; foreach ($k in @($Theirs.Keys)) { $b[$k] = $Theirs[$k] }
    $assumed = @()
    if ($Profiled) {
        foreach ($k in @($assumedKeys.Keys)) {
            if ((-not $a.ContainsKey($k)) -and $b.ContainsKey($k) -and ([string]$b[$k] -eq $assumedKeys[$k])) { $a[$k] = $assumedKeys[$k]; if ($assumed -notcontains $k) { $assumed += $k } }
            if ((-not $b.ContainsKey($k)) -and $a.ContainsKey($k) -and ([string]$a[$k] -eq $assumedKeys[$k])) { $b[$k] = $assumedKeys[$k]; if ($assumed -notcontains $k) { $assumed += $k } }
        }
    }
    $fgOff = (([string]$a['fg'] -eq '0') -and ([string]$b['fg'] -eq '0'))
    $srOff = (([string]$a['sr'] -eq '0') -and ([string]$b['sr'] -eq '0'))
    foreach ($t in @($a, $b)) {
        if ($fgOff) { foreach ($k in $fgDetailKeys) { $t.Remove($k) } }
        if ($srOff) { $t.Remove('srQuality') }
    }
    return @{ mine = $a; theirs = $b; assumed = @($assumed | Sort-Object) }
}
$configSettleSeconds = 10.0
function ConvertTo-ConfigValue([string]$Key, [string]$Value) {
    $v = ([string]$Value).Trim()
    if ((@('', 'unknown', '-1', '-1.0', '-1.00', 'n/a', 'none') -contains $v.ToLowerInvariant()) -and ($Key -ne 'mode')) { return $null }
    if ($flagKeys -contains $Key) { return (ConvertTo-Flag $v) }
    if ($Key -eq 'codec') { return (ConvertTo-Codec $v) }
    if ($Key -eq 'decoder') {
        $low = $v.ToLowerInvariant()
        if ($hardwareDecoders -contains $low) { return 'hardware' }
        if ($softwareDecoders -contains $low) { return 'software' }
        return $low
    }
    if ($Key -eq 'refreshHz') { $hz = Get-HzText $v; if ($hz) { return $hz }; return $null }
    if (($Key -eq 'stream') -and ($v -match '^(\d+)x(\d+)@([0-9.]+)$')) {
        $w = $Matches[1]; $h = $Matches[2]; $fps = [double]::Parse($Matches[3], $invariant)
        if ([math]::Abs($fps - [math]::Round($fps)) -lt 0.01) { return ($w + 'x' + $h + '@' + [string][int][math]::Round($fps)) }
        return ($w + 'x' + $h + '@' + $fps.ToString('F2', $invariant))
    }
    return $v
}

function Merge-ConfigKeys($Inferred, $Explicit) {
    $keys = @{}
    foreach ($source in @($Inferred, $Explicit)) {
        foreach ($k in @($source.Keys)) {
            $v = ConvertTo-ConfigValue $k ([string]$source[$k])
            if ($null -ne $v) { $keys[$k] = $v }   # an explicit unknown (refreshHz=-1) never erases a value inferred from other lines
        }
    }
    return $keys
}

# Latency settings logged by one side only (not comparable); the two keys the test profile sets count as equal
# when both arms ran with it. report.py comparability.
function Get-OneSidedKeys($Mine, $Theirs, [string]$Allowed, [bool]$Profiled) {
    $list = @()
    $allowedKeys = @(([string]$Allowed) -split ';' | Where-Object { $_ })
    foreach ($k in (Get-SortedKeys $Mine $Theirs)) {
        if (($configIgnored -contains $k) -or ($allowedKeys -contains $k) -or ($latencyKeys -notcontains $k)) { continue }
        if ($Profiled -and ($profileKeys -contains $k)) { continue }
        if ($Mine.ContainsKey($k) -ne $Theirs.ContainsKey($k)) { $list += $k }
    }
    return $list
}

# A key table read back from run.json (an object) or kept in memory (a hashtable).
function Get-KeyTable($Value) {
    $table = @{}
    if ($null -eq $Value) { return $table }
    if ($Value -is [hashtable]) { foreach ($k in $Value.Keys) { $table[$k] = [string]$Value[$k] }; return $table }
    foreach ($p in $Value.PSObject.Properties) { $table[$p.Name] = [string]$p.Value }
    return $table
}

function Get-SortedKeys($A, $B) {
    $all = New-Object System.Collections.Generic.List[string]
    foreach ($k in @($A.Keys) + @($B.Keys)) { if (-not $all.Contains([string]$k)) { $all.Add([string]$k) } }
    $list = $all.ToArray()
    [Array]::Sort($list, [StringComparer]::Ordinal)
    return $list
}

# Keys known on both sides that differ ('key A in name, B in base'), leaving out route, trialsMax and the keys the
# switch under test may change (arms.csv column differs, separated by ';').
function Compare-ConfigKeys($Mine, [string]$Name, $Theirs, [string]$BaseName, [string]$Allowed) {
    $list = @()
    $allowedKeys = @(([string]$Allowed) -split ';' | Where-Object { $_ })
    foreach ($k in (Get-SortedKeys $Mine $Theirs)) {
        if (($configIgnored -contains $k) -or ($allowedKeys -contains $k)) { continue }
        if ($Mine.ContainsKey($k) -and $Theirs.ContainsKey($k) -and ($Mine[$k] -cne $Theirs[$k])) { $list += ($k + ' ' + $Mine[$k] + ' in ' + $Name + ', ' + $Theirs[$k] + ' in ' + $BaseName) }
    }
    return $list
}

function Get-ConfigFix([string]$Difference) {
    $key = ($Difference -split ' ')[0]
    if ($configFix.ContainsKey($key)) { return $configFix[$key] }
    return ($key + ' differs: set it the same in both arms')
}

# wifi | wired | unknown: [slipstream-config] route=, then the network health line's link, then hardware.json.
function Get-NetworkRoute([string]$Folder, $Configs) {
    foreach ($config in @($Configs)) { if ($config -and (@('wifi', 'wired') -contains [string]$config.route)) { return [string]$config.route } }
    foreach ($config in @($Configs)) { if ($config -and (@('wifi', 'wired') -contains [string]$config.healthLink)) { return [string]$config.healthLink } }
    $file = Join-Path $Folder 'hardware.json'
    if (Test-Path -LiteralPath $file) {
        try {
            $hardware = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
            foreach ($adapter in @($hardware.network)) {
                if ($adapter -and $adapter.defaultRoute) {
                    if ((([string]$adapter.media) + ' ' + ([string]$adapter.description)) -match '(?i)802\.11|wi-?fi|wireless|wlan') { return 'wifi' }
                    return 'wired'
                }
            }
        } catch { return 'unknown' }
    }
    return 'unknown'
}

# The settings a run actually ran with, as far as its (whitelisted) log lines tell. Empty text = not logged.
function Get-RunConfig([string[]]$Lines, [bool]$Track = $true) {
    $profiles = @(); $codecs = @(); $bitrates = @(); $refreshes = @(); $vrrStates = @()
    $generation = ''; $flowSeen = $false; $decoder = ''; $timing = ''; $build = ''; $frameSeen = $false
    $inferred = @{}; $explicit = @{}; $healthLink = ''
    $settle = $null; $changedKeys = @()
    foreach ($line in $Lines) {
        if (-not $line) { continue }
        if ($line -match '^(\S+Z) t=\d+ \[[A-Z ]+\] \[([^\]]+)\] (.*)$') {
            $stampText = $Matches[1]; $component = $Matches[2]; $text = $Matches[3]
            $when = $null
            try { $when = Get-StampSecondsOf $stampText } catch { $when = $null }
            if (($null -eq $settle) -and ($null -ne $when)) { $settle = $when + $configSettleSeconds }
            # Kit 5 (BENCH2): the settle time counts from the LAST 'session started' before the first presented frame; a connection
            # that was retried (the console still held the previous session) is start-up, not a change of setting.
            if (($component -eq 'remoteplay') -and $text.StartsWith('session started') -and ($null -ne $when) -and (-not $frameSeen)) { $settle = $when + $configSettleSeconds; $changedKeys = @() }
            if (($component -eq 'frame-rate') -and ($null -ne $when)) { $frameSeen = $true }
            $beforeKeys = $null
            if ($Track -and ($configComponents -contains $component)) { $beforeKeys = Merge-ConfigKeys $inferred $explicit }
            Update-ConfigKeys $inferred $component $text
            if ($component -eq 'slipstream-config') { $words = Get-LineWords $text; foreach ($k in $words.Keys) { $explicit[$k] = $words[$k] } }
            if (($null -ne $beforeKeys) -and ($null -ne $when) -and ($null -ne $settle) -and ($when -gt $settle)) {
                $afterKeys = Merge-ConfigKeys $inferred $explicit
                foreach ($k in $trackedKeys) {
                    if ($beforeKeys.ContainsKey($k) -and $afterKeys.ContainsKey($k) -and ($beforeKeys[$k] -cne $afterKeys[$k]) -and ($changedKeys -notcontains $k)) { $changedKeys += $k }
                }
            }
            elseif (($component -eq 'remoteplay-health') -and ($text -match ' link=(wifi|ethernet)')) { if ($Matches[1] -eq 'wifi') { $healthLink = 'wifi' } else { $healthLink = 'wired' } }
        }
        if ($line -match '\] \[remoteplay\] session started .*profile=(\S+) codec=(\S+) requestedBitrateKbps=(\d+)') {
            $p = $Matches[1]; $c = $Matches[2].ToUpperInvariant(); $b = $Matches[3]
            if ($profiles -notcontains $p) { $profiles += $p }
            if ($codecs -notcontains $c) { $codecs += $c }
            if ($bitrates -notcontains $b) { $bitrates += $b }
        } elseif ($line -match '\] \[frame-flow\] ') {
            $flowSeen = $true
            $generated = Get-LogNumber $line 'generatedPresented'
            if (($null -ne $generated) -and ($generated -gt 0)) { $generation = 'on' }
        } elseif ($line -match '\] \[fg-validity\] ') {
            $valid = Get-LogNumber $line 'valid'
            if (($null -ne $valid) -and ($valid -gt 0)) { $generation = 'on' }
        } elseif ($line -match '\] \[display-vrr\] ') {
            $state = ''
            if ($line -match ' active=(true|false) ') { $state = $Matches[1] }
            $hz = Get-LogNumber $line 'refreshHz'
            if (($null -ne $hz) -and ($hz -gt 0)) { $refreshes += [string][int][math]::Round($hz, 0) }
            if ($state) { $vrrStates += $state }
        } elseif ($line -match '\] \[display-refresh\] monitor ') {
            $hz = Get-LogNumber $line 'refresh'
            if (($null -ne $hz) -and ($hz -gt 0)) { $refreshes += [string][int][math]::Round($hz, 0) }
        } elseif ($line -match '\] \[remoteplay-decode\] actual=([A-Za-z0-9_]+)') {
            $decoder = $Matches[1]
        } elseif ($line -match '\] \[remoteplay-timing\] (ownerWait=\S+ input=\S+ engineWake=\S+)') {
            $timing = $Matches[1]
        } elseif ($line -match '\] \[qml\] \[update-check\] .*build=([0-9A-Za-z_.+-]+)') {
            $build = $Matches[1]
        }
    }
    if ((-not $generation) -and $flowSeen) { $generation = 'off' }
    # Refresh rate and variable refresh: the LAST state the run logged counts. [display-vrr] is logged at the first
    # query and on every change (the window going full screen after connecting turns VRR on), so a change during a
    # run is normal and never a refusal by itself.
    $changed = @()
    if ($profiles.Count -gt 1) { $changed += 'resolution and frame rate' }
    if ($codecs.Count -gt 1) { $changed += 'codec' }
    if ($bitrates.Count -gt 1) { $changed += 'bitrate' }
    $lastRefresh = ''
    if ($refreshes.Count -gt 0) { $lastRefresh = $refreshes[$refreshes.Count - 1] }
    $lastVrr = ''
    if ($vrrStates.Count -gt 0) { $lastVrr = $vrrStates[$vrrStates.Count - 1] }
    return [pscustomobject]@{
        profile = ($profiles -join ' / '); codec = ($codecs -join ' / '); bitrateKbps = ($bitrates -join ' / ')
        frameGeneration = $generation; refreshHz = $lastRefresh; vrr = $lastVrr; decoder = $decoder; timing = $timing; build = $build
        changedDuringRun = $changed; from = ''
        keys = (Merge-ConfigKeys $inferred $explicit); configLine = ($explicit.Count -gt 0); route = [string]$explicit['route']; healthLink = $healthLink
        trialsMax = [string]$explicit['trialsMax']; changedKeys = $changedKeys
    }
}

$configKeys = @(
    [pscustomobject]@{ key = 'profile'; label = 'resolution and frame rate'; unit = '' },
    [pscustomobject]@{ key = 'codec'; label = 'codec'; unit = '' },
    [pscustomobject]@{ key = 'bitrateKbps'; label = 'bitrate'; unit = ' kbit/s' },
    [pscustomobject]@{ key = 'refreshHz'; label = 'refresh rate'; unit = ' Hz' },
    [pscustomobject]@{ key = 'vrr'; label = 'variable refresh active'; unit = '' }
)

function Get-ConfigText($Config) {
    $parts = @()
    if ($Config.profile) { $parts += ($Config.profile + ' ' + $Config.codec + ' ' + $Config.bitrateKbps + ' kbit/s') } else { $parts += 'stream settings not logged' }
    if ($Config.refreshHz) { $parts += ('refresh ' + $Config.refreshHz + ' Hz') } else { $parts += 'refresh rate not logged' }
    if ($Config.vrr -eq 'true') { $parts += 'variable refresh active' } elseif ($Config.vrr -eq 'false') { $parts += 'variable refresh off' } else { $parts += 'variable refresh not logged' }
    if ($Config.frameGeneration) { $parts += ('frame generation ' + $Config.frameGeneration) } else { $parts += 'frame generation not logged' }
    if ($Config.decoder) { $parts += ('decoder ' + $Config.decoder) }
    return ($parts -join ', ')
}

# Every setting known on both sides that differs, one text each.
function Compare-RunConfig($Config, $Reference, [string[]]$Skip = @()) {
    $list = @()
    if (-not $Reference) { return $list }
    foreach ($item in $configKeys) {
        if ($Skip -contains $item.key) { continue }
        $mine = [string]$Config.($item.key)
        $theirs = [string]$Reference.($item.key)
        if ($mine -and $theirs -and ($mine -ne $theirs)) { $list += ($item.label + ': ' + $mine + $item.unit + ' in this run, ' + $theirs + $item.unit + ' in ' + $Reference.from) }
    }
    return $list
}

# The keys an arm is expected to change against its pair (arms.csv column differs); none for a pair's own baseline.
function Get-ArmAllowedKeys($Arm) {
    if ([string]$Arm.compare -eq [string]$Arm.name) { return @() }
    return @(([string]$Arm.differs) -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}

# Why a run cannot be compared: frame generation not as its arm needs it (arms.csv column fg), a setting that changed
# while it ran, a stream or refresh rate that differs from the session's first run, or a setting that differs from the
# arm's pair (column compare) and is not one the arm is expected to change (column differs).
function Get-RunProblems($Arm, $Config) {
    $list = @()
    if (($Arm.fg -eq 'off') -and ($Config.frameGeneration -eq 'on')) { $list += 'frame generation was ON; this arm needs it OFF' }
    if (($Arm.fg -eq 'on') -and ($Config.frameGeneration -eq 'off')) { $list += 'frame generation was OFF; this arm needs it ON' }
    foreach ($what in @($Config.changedDuringRun)) { if ($what) { $list += ($what + ' changed while the run went on') } }
    foreach ($key in @($Config.changedKeys)) { if ($key) { $list += ('the setting ' + $key + ' changed while the run went on (after its first 10 seconds): the run cannot be compared') } }
    # What every run of a session shares whatever its arm: the stream and the refresh rate (the session's first run).
    # An arm that changes the stream on purpose (arms.csv column differs: the clarity arms' bitrate and codec) is not refused for it.
    foreach ($difference in @(Compare-RunConfig $Config $script:referenceConfig (@('vrr') + @(Get-ArmAllowedKeys $Arm)))) { if ($difference) { $list += $difference } }
    # Every other setting is compared with the arm's pair (a pair's baseline with its own first run). One session holds
    # arms that differ on purpose (windowed and full screen), so the session's first run is not the measure for these.
    $allowed = @(Get-ArmAllowedKeys $Arm)
    $pairName = [string]$Arm.compare
    if (-not $pairName) { $pairName = [string]$Arm.name }
    $pair = $null
    $pairAllowed = @($allowed)
    if ($script:armReference.ContainsKey($pairName)) { $pair = $script:armReference[$pairName] }
    elseif ($script:referenceConfig -and $script:referenceArm) {
        # The pair has not run yet (round 1 of a baseline that is listed after its other arm): the session's first run
        # stands in when it belongs to the same pair, with what that arm is expected to change left out as well.
        $first = @($table | Where-Object { $_.name -eq $script:referenceArm })
        if ($first.Count -gt 0) {
            $firstPair = [string]$first[0].compare
            if (-not $firstPair) { $firstPair = [string]$first[0].name }
            if ($firstPair -eq $pairName) { $pair = $script:referenceConfig; $pairAllowed = @($allowed) + @(Get-ArmAllowedKeys $first[0]) }
        }
    }
    if ($pair) {
        if ($pairAllowed -notcontains 'vrrActive') {
            $mineVrr = [string]$Config.vrr
            $theirVrr = [string]$pair.vrr
            if ($mineVrr -and $theirVrr -and ($mineVrr -ne $theirVrr)) { $list += ('variable refresh active: ' + $mineVrr + ' in this run, ' + $theirVrr + ' in ' + [string]$pair.from) }
        }
        $mine = Get-KeyTable $Config.keys
        $theirs = Get-KeyTable $pair.keys
        foreach ($difference in @(Compare-ConfigKeys $mine 'this run' $theirs ([string]$pair.from) ($pairAllowed -join ';'))) {
            # The stream, refresh and frame generation are named above already.
            $key = ($difference -split ' ')[0]
            if (@('stream', 'codec', 'bitrateKbps', 'refreshHz', 'fg') -notcontains $key) { $list += ($difference + ' (fix: ' + (Get-ConfigFix $difference) + ')') }
        }
    }
    # The kit's test profile asks for the low-latency queue and V-Sync: a run whose log says otherwise did not get it
    # (unless the arm itself turns that setting off: column differs).
    if ($script:useTestProfile) {
        $keys = Get-KeyTable $Config.keys
        if (($allowed -notcontains 'lowQueue') -and $keys.ContainsKey('lowQueue') -and ($keys['lowQueue'] -ne '1')) { $list += 'the kit''s test profile did not take effect: the low-latency queue was off' }
        if (($allowed -notcontains 'displaySync') -and $keys.ContainsKey('displaySync') -and ($keys['displaySync'] -ne '1')) { $list += ('the kit''s test profile did not take effect: display sync was ' + $keys['displaySync'] + ', not V-Sync (1)') }
    }
    return $list
}

# Plain words for the stop reasons of the app's automation ([slipstream-autorun] state=failed reason=...) and latency test
# ([push-latency] stopped reason=...); report.py STOP_REASONS is the same table.
$stopReasonText = @{
    'no-console' = 'PushFPS found no console to connect to (is the PS5 switched on or in rest mode, and paired with this copy?)'
    'stream-lost' = 'the stream ended while the plan ran (a disconnect, a reconnect or Stop)'
    'cancelled' = 'the latency test was cancelled in PushFPS'
    'invalid-plan' = 'this build did not understand the kit''s plan (a kit and build mismatch: send the zip as it is)'
    'picture-moving' = 'the picture moved too much for the PS-button test: stand still in a calm place, or open the PS5 menu, and run it again'
    'no-capture' = 'PushFPS found no capture card remembered in the kit''s capture settings folder (the kit asks you to choose it once before the capture session)'
}
function Get-StopReasonText([string]$Reason) {
    if ($stopReasonText.ContainsKey($Reason)) { return ($stopReasonText[$Reason] + ' (' + $Reason + ')') }
    return $Reason
}

function New-LiveState {
    return [pscustomobject]@{ lines = (New-Object System.Collections.Generic.List[string]); inTrial = $false; testsDone = 0; presses = 0; announced = ''; fgWarned = $false; lightSamples = 0
        autorunSeen = $false; autorunFinished = $null; autorunFailed = ''; fallbackTold = $false; finishedAt = $null; trialsTold = $false; streamAt = $null; cuesTold = 0 }
}

# New (whitelisted, masked) lines of a running run: prints what is new, once.
function Update-LiveState($State, [string[]]$Lines, $Arm) {
    $any = $false
    foreach ($line in $Lines) {
        if (-not $line) { continue }
        $any = $true
        $State.lines.Add($line)
        if (($null -eq $State.streamAt) -and ($line -match '\] \[frame-rate\] ')) { $State.streamAt = Get-Date }
        if ($line -match '\] \[push-latency\] remote play latency test started') {
            $State.inTrial = $true
            Write-Host '  latency test running: the kit samples only cheap values until it ends'
        } elseif ($line -match '\] \[push-latency\] done answered=(\d+)/(\d+)') {
            $answered = [int]$Matches[1]; $trials = [int]$Matches[2]
            $State.inTrial = $false
            $State.testsDone++
            $State.presses += $answered
            $median = Get-LogNumber $line 'medianMs'
            Write-Host ('  latency test ' + $State.testsDone + ' finished: ' + $answered + ' of ' + $trials + ' presses answered, median ' + (Format-Number $median 1) + ' ms (software clock, not input to photon)') -ForegroundColor Green
        } elseif ($line -match '\] \[push-latency\] stopped reason=(\S+)') {
            $State.inTrial = $false
            Write-Host ('  latency test stopped early: ' + (Get-StopReasonText $Matches[1])) -ForegroundColor Yellow
        } elseif ($line -match '\] \[push-latency\] (cancelled|.*no answer seen)') {
            $State.inTrial = $false
        } elseif ($line -match '\] \[slipstream-autorun\] finished ok=(\d+)') {
            $State.autorunSeen = $true; $State.autorunFinished = [int]$Matches[1]
            if ($State.autorunFinished -eq 0) { if (-not $State.autorunFailed) { $State.autorunFailed = 'the plan finished with ok=0' } }
            else { Write-Host ('  automatic plan finished, ok=' + $Matches[1]) -ForegroundColor Green }
        } elseif ($line -match '\] \[slipstream-autorun\] (.*)$') {
            $State.autorunSeen = $true
            $text = $Matches[1]
            if ($text -match 'state=failed reason=(\S+)') { $State.autorunFailed = $Matches[1]; Write-Host ('  automatic step failed: ' + $text) -ForegroundColor Yellow }
        } elseif (($line -match '\] \[slipstream-config\] .*trialsMax=(\d+)') -and (-not $State.trialsTold)) {
            $State.trialsTold = $true
            Write-Host ('  this build accepts at most ' + $Matches[1] + ' presses per latency test') -ForegroundColor Cyan
        }
    }
    if (-not $any) { return }
    if ($State.inTrial) { return }   # no parsing while the latency test is timing
    $config = Get-RunConfig $State.lines.ToArray() $false
    if ($config.profile) {
        $text = Get-ConfigText $config
        if ($text -ne $State.announced) {
            $State.announced = $text
            Write-Host ('  detected: ' + $text) -ForegroundColor Cyan
        }
    }
    if (($Arm.fg -eq 'off') -and ($config.frameGeneration -eq 'on') -and (-not $State.fgWarned)) {
        $State.fgWarned = $true
        Write-Host '  FRAME GENERATION IS ON in this run, and this arm needs it OFF. Turn it off in PushFPS now; the kit offers to repeat the run at the end.' -ForegroundColor Red
    }
}

# --- RESULT.txt and result.json ----------------------------------------------------------------------------
# Software latency only (the built-in test's clock). Camera and light sensor measurements are counted here but
# analysed only by report.py, in their own table: the two are never in one table or one verdict.
$resultMinimum = 120
$resultRule = 'A difference is called only when both arms have at least ' + $resultMinimum + ' presses with per-press values, the two arms ran with the same settings, and the 95 % intervals of the two medians do not overlap. This is conservative: a real difference can stay not shown. report.py (difference of means, bootstrap interval) can resolve smaller ones.'
$missingAppFields = @(
    'performance mode in effect at session start (set in the interface or by slipstream.ini): [remoteplay-settings] mode=ultra-low-latency source=ui|ini|none',
    'frame generation setting at session start, also when it is off: [remoteplay-settings] frameGeneration=off multiplier=1 backend=none',
    'per-frame decode time percentiles without the health switch: [remoteplay-decode] window=5s frames=300 decodeMsP50=1.10 decodeMsP95=1.90 decodeMsMax=3.20',
    'present queue depth at the swap chain: [pacing] queueDepth=1 queueDepthMax=2 window=5s',
    'count of error and warning lines of every family since start (the kit copies only some families): [log-summary] errors=0 warnings=3 since=start',
    'refresh rate and variable refresh state from every GPU maker (display-vrr reads NVIDIA only): [display-refresh] at-start refresh=180.000 Hz vrr=active source=dxgi',
    'stream settings in the original build (3.2.1 logs no session line the kit reads): [remoteplay] session started profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000'
)

# Sorted values: linear interpolation between ranks (as report.py's percentile).
function Get-Quantile([double[]]$Sorted, [double]$P) {
    $n = $Sorted.Count
    if ($n -eq 0) { return [double]::NaN }
    $k = ($n - 1) * $P
    $lo = [int][math]::Floor($k)
    $hi = $lo + 1
    if ($hi -gt ($n - 1)) { $hi = $n - 1 }
    return ($Sorted[$lo] + ($Sorted[$hi] - $Sorted[$lo]) * ($k - $lo))
}

# 95 % interval of a quantile without assumptions about the distribution: the order statistics at the 1-based ranks
# floor(np - 1.96 sqrt(np(1-p))) and ceil(np + 1.96 sqrt(np(1-p))) + 1. NaN on a side whose rank is outside the data.
function Get-QuantileInterval([double[]]$Sorted, [double]$P) {
    $n = $Sorted.Count
    $low = [double]::NaN
    $high = [double]::NaN
    if ($n -eq 0) { return @($low, $high) }
    $spread = 1.96 * [math]::Sqrt($n * $P * (1 - $P))
    $lowRank = [int][math]::Floor($n * $P - $spread)
    $highRank = [int][math]::Ceiling($n * $P + $spread) + 1
    if ($lowRank -ge 1) { $low = $Sorted[$lowRank - 1] }
    if ($highRank -le $n) { $high = $Sorted[$highRank - 1] }
    return @($low, $high)
}

function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) { return [double]::NaN }
    $sorted = [double[]]@($Values | Sort-Object)
    return (Get-Quantile $sorted 0.5)
}

function Get-Mean([double[]]$Values) {
    if ($Values.Count -eq 0) { return [double]::NaN }
    $sum = 0.0
    foreach ($value in $Values) { $sum += $value }
    return ($sum / $Values.Count)
}

# Completed runs of an evidence folder that were not set aside: name and run.json.
function Read-KeptRuns([string]$Folder) {
    $list = @()
    $runsFolder = Join-Path $Folder 'runs'
    if (-not (Test-Path -LiteralPath $runsFolder)) { return $list }
    foreach ($dir in @(Get-ChildItem -LiteralPath $runsFolder -Directory | Sort-Object Name)) {
        $file = Join-Path $dir.FullName 'run.json'
        if (-not (Test-Path -LiteralPath $file)) { continue }
        $record = $null
        try { $record = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json } catch { $record = $null }
        if ((-not $record) -or (-not $record.completed) -or $record.setAside) { continue }
        $list += [pscustomobject]@{ name = $dir.Name; folder = $dir.FullName; record = $record }
    }
    return $list
}

function Read-Number([string]$Text) {
    $value = 0.0
    if ($Text -and [double]::TryParse($Text, [System.Globalization.NumberStyles]::Float, $invariant, [ref]$value)) { return $value }
    return $null
}

# Every value of one column of samples.csv files that has a number.
function Read-SampleColumn([string[]]$Files, [string]$Column) {
    $values = New-Object System.Collections.Generic.List[double]
    foreach ($file in $Files) {
        if (-not (Test-Path -LiteralPath $file)) { continue }
        foreach ($row in @(Get-Content -LiteralPath $file | ConvertFrom-Csv)) {
            $number = Read-Number ([string]$row.$Column)
            if ($null -ne $number) { $values.Add($number) }
        }
    }
    return $values.ToArray()
}

# --- precision tables (sprint 4): the same definitions as report.py precision_tables ---------------------------------
# Bootstrap: values sorted, $bootRounds resamples drawn with Park-Miller x = 16807 x mod (2^31 - 1) from $bootSeed
# (index = x mod n); 95 % interval = the 2.5 and 97.5 percentiles of the resampled quantile. Same digits as Python.
$bootSeed = 807
$bootRounds = 1000
$tableMinimum = 120
$deliveryMinimum = 300
$quantileNames = @('p50', 'p95', 'p99')
$quantileLevels = @(0.5, 0.95, 0.99)

function Get-BootstrapQuantiles([double[]]$Values, [int]$RoundCount, [long]$Seed) {
    $result = @{}
    $n = $Values.Count
    if ($n -lt 2) { foreach ($k in $quantileNames) { $result[$k] = @([double]::NaN, [double]::NaN) }; return $result }
    $sorted = [double[]]($Values.Clone())
    [Array]::Sort($sorted)
    $draws = @{}
    foreach ($k in $quantileNames) { $draws[$k] = New-Object 'double[]' $RoundCount }
    $sample = New-Object 'double[]' $n
    $x = [long]$Seed
    for ($r = 0; $r -lt $RoundCount; $r++) {
        for ($i = 0; $i -lt $n; $i++) {
            $x = ($x * 16807) % 2147483647
            $sample[$i] = $sorted[[int]($x % $n)]
        }
        [Array]::Sort($sample)
        for ($j = 0; $j -lt 3; $j++) { $draws[$quantileNames[$j]][$r] = Get-Quantile $sample $quantileLevels[$j] }
    }
    foreach ($k in $quantileNames) {
        $d = $draws[$k]
        [Array]::Sort($d)
        $result[$k] = @((Get-Quantile $d 0.025), (Get-Quantile $d 0.975))
    }
    return $result
}

function New-LatencyRow([double[]]$Values) {
    $sorted = [double[]]@($Values | Sort-Object)
    $row = @{ n = $sorted.Count; verdict = ''; mdd = [double]::NaN; windowP95 = [double]::NaN }
    $intervals = Get-BootstrapQuantiles $sorted $bootRounds $bootSeed
    for ($j = 0; $j -lt 3; $j++) {
        $k = $quantileNames[$j]
        $row[$k] = Get-Quantile $sorted $quantileLevels[$j]
        $row[$k + 'Low'] = $intervals[$k][0]; $row[$k + 'High'] = $intervals[$k][1]
    }
    return $row
}

function Get-Mdd([double]$WidthA, [double]$WidthB) {
    if ([double]::IsNaN($WidthA) -or [double]::IsNaN($WidthB)) { return [double]::NaN }
    return (($WidthA + $WidthB) / 2.0)   # the two 95 % intervals must not overlap: the sum of the half-widths
}

# The smallest P50 difference worth finding per press table (ms): report.py LATENCY_EFFECT.
$latencyEffect = @{ 'pipeline' = 1.0; 'console' = 2.0; 'total' = 8.3 }
$pressesCap = 2000
function Get-LatencyVerdict($Row, $Base, [string]$BaseName, [string]$Key = 'total') {
    if (($Row.n -eq 0) -or ($Base.n -eq 0)) { return 'no verdict: no values in one of the two arms' }
    if (($Row.n -lt $tableMinimum) -or ($Base.n -lt $tableMinimum)) { return ('no verdict: fewer than ' + $tableMinimum + ' values (' + $Row.n + ' and ' + $Base.n + ')') }
    foreach ($v in @($Row.p50Low, $Row.p50High, $Base.p50Low, $Base.p50High)) { if ([double]::IsNaN($v)) { return 'no verdict: no interval' } }
    if ($Row.p50High -lt $Base.p50Low) { return ('LOWER than ' + $BaseName + ' by ' + (Format-Number ($Base.p50 - $Row.p50) 2) + ' ms (P50)') }
    if ($Row.p50Low -gt $Base.p50High) { return ('HIGHER than ' + $BaseName + ' by ' + (Format-Number ($Row.p50 - $Base.p50) 2) + ' ms (P50)') }
    # report.py latency_verdict: overlapping intervals whose MDD is larger than the table's effect say "cannot tell"
    $effect = [double]$latencyEffect[$Key]
    $need = Get-Mdd ($Row.p50High - $Row.p50Low) ($Base.p50High - $Base.p50Low)
    if ($need -le $effect) { return ('no difference shown: the 95 % intervals of the P50 overlap and could have shown ' + (Format-Number $need 2) + ' ms, below ' + (Format-Number $effect 1) + ' ms (vs ' + $BaseName + ')') }
    $n = [math]::Min([int]$Row.n, [int]$Base.n)
    $m = [int]([math]::Ceiling($n * ($need / $effect) * ($need / $effect) / 12.0)) * 12
    $more = 'about ' + $m
    if ($m -gt $pressesCap) { $more = 'more than ' + $pressesCap }
    return ('cannot tell with ' + $n + ' presses per arm: this session would need ' + $more + ' presses per arm to show a difference of ' + (Format-Number $effect 1) + ' ms (vs ' + $BaseName + ')')
}

# 95 % interval of sum(k) / sum(n) in % when whole runs are resampled (report.py run_block_interval): blocks sorted by
# (k, n), Park-Miller as above, the same index for k and n; NaN with fewer than 3 runs.
function Get-RunBlockInterval($Blocks) {
    $b = @($Blocks | Sort-Object k, n)
    if ($b.Count -lt 3) { return @([double]::NaN, [double]::NaN) }
    $draws = New-Object System.Collections.Generic.List[double]
    $x = [long]$bootSeed
    for ($r = 0; $r -lt $bootRounds; $r++) {
        $k = 0.0; $n = 0.0
        for ($i = 0; $i -lt $b.Count; $i++) { $x = ($x * 16807) % 2147483647; $pick = $b[[int]($x % $b.Count)]; $k += $pick.k; $n += $pick.n }
        if ($n -gt 0) { $draws.Add(100.0 * $k / $n) }
    }
    $d = $draws.ToArray(); [Array]::Sort($d)
    return @((Get-Quantile $d 0.025), (Get-Quantile $d 0.975))
}

function Get-Wilson([double]$K, [double]$N) {
    if ($N -le 0) { return @([double]::NaN, [double]::NaN) }
    $z = 1.96; $p = $K / $N
    $centre = ($p + $z * $z / (2 * $N)) / (1 + $z * $z / $N)
    $half = $z * [math]::Sqrt($p * (1 - $p) / $N + $z * $z / (4 * $N * $N)) / (1 + $z * $z / $N)
    return @((100.0 * ($centre - $half)), (100.0 * ($centre + $half)))
}

# A counter logged per window or as a running total: the last value when the values never fall, else the sum.
function Get-CounterTotal([double[]]$Values) {
    if ($Values.Count -eq 0) { return $null }
    $rising = ($Values.Count -gt 1)
    for ($i = 1; $i -lt $Values.Count; $i++) { if ($Values[$i] -lt $Values[$i - 1]) { $rising = $false } }
    if ($rising) { return $Values[$Values.Count - 1] }
    $sum = 0.0
    foreach ($v in $Values) { $sum += $v }
    return $sum
}

# Everything the precision tables need from the log lines of one run.
function Read-PrecisionLines([string[]]$Lines) {
    $r = @{ pipeline = (New-Object System.Collections.Generic.List[double]); console = (New-Object System.Collections.Generic.List[double]); total = (New-Object System.Collections.Generic.List[double])
        windowP95 = (New-Object System.Collections.Generic.List[double]); fps = (New-Object System.Collections.Generic.List[double]); arrived = 0.0; replaced = 0.0; deliveryLines = 0
        gaps = (New-Object System.Collections.Generic.List[double]); clumps = 0.0; capture = 0.0; overwritten = 0.0
        pops = (New-Object System.Collections.Generic.List[double]); underruns = (New-Object System.Collections.Generic.List[double]); concealed = (New-Object System.Collections.Generic.List[double]); audioLines = 0
        first = ''; last = ''; sr = @(); fg = @(); unsupported = @(); cadence = @(); holds = @(); swapchain = @{}; scanoutP50 = (New-Object System.Collections.Generic.List[double]) }
    $prev = $null
    # Kit 5 (BENCH2): the swap chain facts as they stood during the measurement. Lines after the last '[push-latency] done'
    # (else after the first '[slipstream-autorun] finished') are left out: the app writes one at exit, windowed again.
    $chainLines = New-Object System.Collections.Generic.List[object]; $testDone = ''; $planDone = ''
    foreach ($line in $Lines) {
        if (-not $line) { continue }
        if ($line -notmatch '^(\S+Z) t=\d+ \[[A-Z ]+\] \[([^\]]+)\] (.*)$') { continue }
        $when = $Matches[1]; $component = $Matches[2]; $text = $Matches[3]
        if (-not $r.first) { $r.first = $when }
        $r.last = $when
        if (($component -eq 'push-latency') -and ($text -match '^done answered=(\d+)/(\d+) meanMs=([\d.]+) medianMs=([\d.]+).*pushfpsMs=([\d.]+)')) { $testDone = $when }
        if (($component -eq 'slipstream-autorun') -and $text.StartsWith('finished') -and (-not $planDone)) { $planDone = $when }
        if ($component -eq 'slipstream-press') {
            $v = Get-LogNumber $text 'arrivalToScanoutMs'; if (($null -ne $v) -and ($v -ge 0)) { $r.pipeline.Add($v) }
            $v = Get-LogNumber $text 'sendToArrivalMs'; if (($null -ne $v) -and ($v -ge 0)) { $r.console.Add($v) }
        } elseif (($component -eq 'push-latency') -and ($text -match '^trials answerMs=\[([^\]]*)\]')) {
            foreach ($value in ($Matches[1] -split ',')) { $number = Read-Number $value.Trim(); if ($null -ne $number) { $r.total.Add($number) } }
        } elseif ($component -eq 'slipstream-latency') {
            $v = Get-LogNumber $text 'arrivalToScanoutP95'; if (($null -ne $v) -and ($v -ge 0)) { $r.windowP95.Add($v) }
            $v = Get-LogNumber $text 'presentToScanoutP50'; if (($null -ne $v) -and ($v -ge 0)) { $r.scanoutP50.Add($v) }
        } elseif ($component -eq 'slipstream-swapchain') {
            $chainLines.Add([pscustomobject]@{ when = $when; words = (Get-LineWords $text) })
        } elseif ($component -eq 'frame-rate') {
            $v = Get-LogNumber $text 'presentSubmitFps'; if ($null -ne $v) { $r.fps.Add($v) }
        } elseif ($component -eq 'slipstream-delivery') {
            $r.deliveryLines++
            $v = Get-LogNumber $text 'arrived'; if ($null -ne $v) { $r.arrived += $v }
            $v = Get-LogNumber $text 'replaced'; if ($null -ne $v) { $r.replaced += $v }
            $v = Get-LogNumber $text 'gapP95'; if ($null -ne $v) { $r.gaps.Add($v) }
            $v = Get-LogNumber $text 'clumps'; if ($null -ne $v) { $r.clumps += $v }
        } elseif ($component -eq 'frame-flow') {
            # Counters restart with a session: the last line before a restart counts (as report.py parse_log).
            $now = @{ session = (Get-LogNumber $text 'session'); capture = (Get-LogNumber $text 'captureReceived'); over = (Get-LogNumber $text 'mailboxOverwritten') }
            if ($null -eq $now.capture) { $now.capture = 0.0 }
            if ($null -eq $now.over) { $now.over = 0.0 }
            if ($prev -and (($now.session -ne $prev.session) -or ($now.capture -lt $prev.capture) -or ($now.over -lt $prev.over))) { $r.capture += $prev.capture; $r.overwritten += $prev.over }
            $prev = $now
            if ($text -match '^state=closed') { $r.capture += $now.capture; $r.overwritten += $now.over; $prev = $null }
        } elseif (($component -eq 'remoteplay-audio') -and (($text -match 'underruns=') -or ($text -match 'pops='))) {
            $r.audioLines++
            $v = Get-LogNumber $text 'pops'; if ($null -ne $v) { $r.pops.Add($v) }
            $v = Get-LogNumber $text 'underruns'; if ($null -ne $v) { $r.underruns.Add($v) }
            $v = Get-LogNumber $text 'concealed'; if ($null -ne $v) { $r.concealed.Add($v) }
        } elseif (($component -eq 'slipstream-sr') -and $text.StartsWith('event=hold-end')) {
            $w = Get-LineWords $text
            $r.holds += [pscustomobject]@{ label = [string]$w['label']; seconds = (Get-LogNumber $text 'seconds'); p50 = (Get-LogNumber $text 'latencyP50Ms'); p95 = (Get-LogNumber $text 'latencyP95Ms') }
        } elseif (($component -eq 'slipstream-sr') -and (-not $text.StartsWith('event='))) {
            $w = Get-LineWords $text
            $r.sr += [pscustomobject]@{ choice = [int](Read-Number ([string]$w['choice'])); quality = [string]$w['quality']; inSize = [string]$w['in']; outSize = [string]$w['out']; gpuP50 = (Get-LogNumber $text 'gpuP50'); gpuP95 = (Get-LogNumber $text 'gpuP95'); frames = (Get-LogNumber $text 'frames') }
        } elseif ($component -eq 'slipstream-fg') {
            $r.fg += [pscustomobject]@{ method = [int](Get-LogNumber $text 'method'); multiplier = [int](Get-LogNumber $text 'multiplier'); outputFps = (Get-LogNumber $text 'outputFps'); generatedShown = (Get-LogNumber $text 'generatedShown')
                generatedExpired = (Get-LogNumber $text 'generatedExpired'); skippedPairs = (Get-LogNumber $text 'skippedPairs'); costP50 = (Get-LogNumber $text 'costP50'); costP95 = (Get-LogNumber $text 'costP95')
                shownGapP95 = (Get-LogNumber $text 'shownGapP95'); shownGapMax = (Get-LogNumber $text 'shownGapMax'); pairDelayMs = (Get-LogNumber $text 'pairDelayMs') }
        } elseif (($component -eq 'slipstream-autorun') -and ($text -match 'state=failed reason=unsupported')) {
            $w = Get-LineWords $text
            $r.unsupported += [string]$w['name']
        } elseif (($component -eq 'remoteplay-cadence') -and ($text -match '^state=')) {
            $c = @{}
            foreach ($key in @('repeatsFound', 'repeatsRemoved', 'exactVetoes', 'suspendedKeeps', 'keptUnsure', 'unmeasured')) { $v = Get-LogNumber $text $key; if ($null -eq $v) { $v = 0.0 }; $c[$key] = $v }
            $r.cadence += ,$c
        }
    }
    if ($prev) { $r.capture += $prev.capture; $r.overwritten += $prev.over }
    $chainEnd = $testDone
    if (-not $chainEnd) { $chainEnd = $planDone }
    foreach ($item in $chainLines) {
        if ($chainEnd -and ([string]::CompareOrdinal([string]$item.when, [string]$chainEnd) -gt 0)) { continue }
        foreach ($k in $item.words.Keys) { $r.swapchain[$k] = $item.words[$k] }
    }
    return $r
}

function Get-MedianOrNaN($List) {
    $values = [double[]]@($List)
    if ($values.Count -eq 0) { return [double]::NaN }
    return (Get-Median $values)
}

# The tables per latency arm (and the quality / probe runs), as report.py precision_tables. $Arms: the arm entries of
# Write-ResultFiles (name, compare, refused). Adds the text to $Out and returns the values for result.json.
function Add-PrecisionTables($Out, [string]$Folder, $Arms, $Kept, [string]$Route) {
    $label = ''
    if ($Route -eq 'wifi') { $label = $wifiLabel }
    $read = @{}
    $other = @()
    foreach ($run in $Kept) {
        $benchFile = Join-Path (Join-Path $Folder 'bench') ($run.name + '.txt')
        $lines = @()
        if (Test-Path -LiteralPath $benchFile) { $lines = @(Get-Content -LiteralPath $benchFile) }
        $parsed = Read-PrecisionLines $lines
        if ($run.record.purpose -eq 'latency') {
            $arm = [string]$run.record.arm
            if (-not $read.ContainsKey($arm)) { $read[$arm] = @() }
            $read[$arm] += ,$parsed
        }
        $other += [pscustomobject]@{ purpose = [string]$run.record.purpose; parsed = $parsed }
    }
    $tables = [ordered]@{ route = $Route; label = $label; seed = $bootSeed; rounds = $bootRounds }
    $titles = [ordered]@{ pipeline = 'PC PIPELINE (arrival to scanout), software'; console = 'CONSOLE + NETWORK (send to arrival), software'; total = 'TOTAL (PS-button style), software' }
    foreach ($key in $titles.Keys) {
        $rows = [ordered]@{}
        foreach ($entry in $Arms) {
            $values = New-Object System.Collections.Generic.List[double]
            $windows = New-Object System.Collections.Generic.List[double]
            foreach ($parsed in @($read[$entry.name])) { if ($parsed) { foreach ($v in $parsed[$key]) { $values.Add($v) }; foreach ($v in $parsed.windowP95) { $windows.Add($v) } } }
            $row = New-LatencyRow $values.ToArray()
            if ($key -eq 'pipeline') { $row.windowP95 = Get-MedianOrNaN $windows.ToArray() }
            $rows[$entry.name] = $row
        }
        foreach ($entry in $Arms) {
            $row = $rows[$entry.name]
            $base = @($Arms | Where-Object { $_.name -eq $entry.compare })
            if ($entry.compare -eq $entry.name) { $row.verdict = 'baseline' }
            elseif ($base.Count -eq 0) { $row.verdict = "no verdict: its pair '" + $entry.compare + "' was not run" }
            elseif ($entry.refused -or $base[0].refused) { $row.verdict = 'REFUSED: the two arms ran with different settings (see the result above)' }
            else {
                $other0 = $rows[$entry.compare]
                $row.verdict = Get-LatencyVerdict $row $other0 $entry.compare $key
                $row.mdd = Get-Mdd ($row.p50High - $row.p50Low) ($other0.p50High - $other0.p50Low)
            }
            if ($label) { $row.verdict = $label + '; ' + $row.verdict }
        }
        $tables[$key] = $rows
        $Out.Add($titles[$key])
        if ($label) { $Out.Add('  ' + $label) }
        $Out.Add('  arm             values  P50 ms [95 %]            P95 ms [95 %]            P99 ms [95 %]            MDD ms  verdict')
        foreach ($name in $rows.Keys) {
            $r = $rows[$name]
            $Out.Add(('  {0,-15} {1,6}  {2} [{3}, {4}]  {5} [{6}, {7}]  {8} [{9}, {10}]  {11}  {12}' -f $name, $r.n, (Format-Number $r.p50 2), (Format-Number $r.p50Low 2), (Format-Number $r.p50High 2),
                (Format-Number $r.p95 2), (Format-Number $r.p95Low 2), (Format-Number $r.p95High 2), (Format-Number $r.p99 2), (Format-Number $r.p99Low 2), (Format-Number $r.p99High 2), (Format-Number $r.mdd 2), $r.verdict))
            if (($key -eq 'pipeline') -and (-not [double]::IsNaN($r.windowP95))) { $Out.Add('      continuous: median of the 5 s window P95 ' + (Format-Number $r.windowP95 2) + ' ms') }
        }
        $Out.Add('  Per press; bootstrap 95 % intervals (' + $bootRounds + ' resamples, seed ' + $bootSeed + '). Verdict: at least ' + $tableMinimum + ' values per arm and the P50 intervals do not overlap. MDD = the smallest P50 difference that rule can call (sum of the two half-widths); overlapping intervals with an MDD above the smallest difference worth finding (pipeline 1 ms, console + network 2 ms, total 8.3 ms) say "cannot tell" and how many presses would be needed, never "no difference shown". Software counters, never mixed with camera or light sensor data.')
        if ($key -eq 'total') { $Out.Add('  The PS-button test times the console''s own menu answer (about half a second, about 26 ms apart from press to press): it cannot resolve a few milliseconds. The in-app split ([slipstream-latency], the PC pipeline table) and PresentMon can.') }
        $Out.Add('')
    }
    # frame delivery
    $rows = [ordered]@{}
    foreach ($entry in $Arms) {
        $seconds = 0; $under = 0; $arrived = 0.0; $replaced = 0.0; $deliveryLines = 0; $capture = 0.0; $overwritten = 0.0; $clumps = 0.0; $blocks = @()
        $gaps = New-Object System.Collections.Generic.List[double]
        foreach ($parsed in @($read[$entry.name])) {
            if (-not $parsed) { continue }
            $runUnder = 0; $runSeconds = 0
            for ($i = 3; $i -lt ($parsed.fps.Count - 2); $i++) { $seconds++; $runSeconds++; if ($parsed.fps[$i] -lt 57) { $under++; $runUnder++ } }
            if ($runSeconds -gt 0) { $blocks += [pscustomobject]@{ k = $runUnder; n = $runSeconds } }
            $arrived += $parsed.arrived; $replaced += $parsed.replaced; $deliveryLines += $parsed.deliveryLines; $clumps += $parsed.clumps
            $capture += $parsed.capture; $overwritten += $parsed.overwritten
            foreach ($v in $parsed.gaps) { $gaps.Add($v) }
        }
        $replacedPercent = [double]::NaN; $replacedFrom = ''
        if ($arrived -gt 0) { $replacedPercent = 100.0 * $replaced / $arrived; $replacedFrom = 'slipstream-delivery' }
        elseif ($capture -gt 0) { $replacedPercent = 100.0 * $overwritten / $capture; $replacedFrom = 'frame-flow' }
        $wilson = Get-RunBlockInterval $blocks
        $percent = [double]::NaN
        if ($seconds -gt 0) { $percent = 100.0 * $under / $seconds }
        $clumpValue = $null
        if ($deliveryLines -gt 0) { $clumpValue = $clumps }
        $rows[$entry.name] = @{ seconds = $seconds; under57 = $under; under57Percent = $percent; under57Low = $wilson[0]; under57High = $wilson[1]; replacedPercent = $replacedPercent
            replacedFrom = $replacedFrom; gapP95 = (Get-MedianOrNaN $gaps.ToArray()); clumps = $clumpValue; mdd = [double]::NaN; verdict = ''; runs = $blocks.Count }
    }
    foreach ($entry in $Arms) {
        $row = $rows[$entry.name]
        $base = @($Arms | Where-Object { $_.name -eq $entry.compare })
        if ($entry.compare -eq $entry.name) { $row.verdict = 'baseline' }
        elseif ($base.Count -eq 0) { $row.verdict = "no verdict: its pair '" + $entry.compare + "' was not run" }
        elseif ($entry.refused -or $base[0].refused) { $row.verdict = 'REFUSED: the two arms ran with different settings (see the result above)' }
        else {
            $b = $rows[$entry.compare]
            if (($row.seconds -lt $deliveryMinimum) -or ($b.seconds -lt $deliveryMinimum)) { $row.verdict = 'no verdict: fewer than ' + $deliveryMinimum + ' seconds (' + $row.seconds + ' and ' + $b.seconds + ')' }
            elseif (($row.runs -lt 3) -or ($b.runs -lt 3)) { $row.verdict = 'no verdict: fewer than 3 runs (' + $row.runs + ' and ' + $b.runs + ')' }
            elseif ($row.under57High -lt $b.under57Low) { $row.verdict = 'FEWER short seconds than ' + $entry.compare + ' (' + (Format-Number $row.under57Percent 1) + ' % vs ' + (Format-Number $b.under57Percent 1) + ' %)' }
            elseif ($row.under57Low -gt $b.under57High) { $row.verdict = 'MORE short seconds than ' + $entry.compare + ' (' + (Format-Number $row.under57Percent 1) + ' % vs ' + (Format-Number $b.under57Percent 1) + ' %)' }
            else { $row.verdict = 'no difference shown: the 95 % intervals overlap (vs ' + $entry.compare + ')' }
            $row.mdd = Get-Mdd ($row.under57High - $row.under57Low) ($b.under57High - $b.under57Low)
        }
        if ($label) { $row.verdict = $label + '; ' + $row.verdict }
    }
    $tables['delivery'] = $rows
    # The tearing arm (VEYRA_EXP_PRESENT_TEARING): torn frames get no scan-out time from the app; scored only with PresentMon.
    foreach ($entry in $Arms) {
        if ([string]$entry.switches -notmatch 'VEYRA_EXP_PRESENT_TEARING') { continue }
        $captured = $false
        foreach ($run in @($Kept | Where-Object { ($_.record.arm -eq $entry.name) -and ($_.record.purpose -eq 'latency') })) { if (Test-Path -LiteralPath (Join-Path $run.folder 'presentmon.csv')) { $captured = $true } }
        if (-not $captured) {
            foreach ($key in @('pipeline', 'console', 'total', 'delivery')) {
                if ($tables[$key].Contains($entry.name)) { $tables[$key][$entry.name].verdict = 'not scored: the tearing arm needs PresentMon (torn frames get no scan-out time from the app)' }
            }
        }
    }
    $Out.Add('FRAME DELIVERY (seconds with fewer than 57 presents submitted, 95 % interval by resampling whole runs; replaced = overwritten before the engine took them)')
    if ($label) { $Out.Add('  ' + $label) }
    foreach ($name in $rows.Keys) {
        $r = $rows[$name]
        $clumpText = 'n/a'
        if ($null -ne $r.clumps) { $clumpText = [string][int]$r.clumps }
        $Out.Add(('  {0,-15} {1,5} s  under 57: {2} % [{3}, {4}]  replaced {5} % ({6})  arrival gap P95 {7} ms  clumps {8}  MDD {9} pp  {10}' -f $name, $r.seconds, (Format-Number $r.under57Percent 1), (Format-Number $r.under57Low 1), (Format-Number $r.under57High 1),
            (Format-Number $r.replacedPercent 2), $r.replacedFrom, (Format-Number $r.gapP95 2), $clumpText, (Format-Number $r.mdd 1), $r.verdict))
    }
    $Out.Add('  Verdict: at least ' + $deliveryMinimum + ' seconds and 3 runs per arm and the intervals do not overlap. First 3 and last 2 [frame-rate] lines of each run left out. MDD = sum of the two half-widths.')
    $Out.Add('')
    # audio
    $rows = [ordered]@{}
    $Out.Add('AUDIO (counts from [remoteplay-audio]; counts only, no verdict)')
    if ($label) { $Out.Add('  ' + $label) }
    foreach ($entry in $Arms) {
        $pops = $null; $underruns = $null; $concealed = $null; $lineCount = 0
        foreach ($parsed in @($read[$entry.name])) {
            if (-not $parsed) { continue }
            $lineCount += $parsed.audioLines
            $t = Get-CounterTotal $parsed.pops.ToArray(); if ($null -ne $t) { $pops = [double]$pops + $t }
            $t = Get-CounterTotal $parsed.underruns.ToArray(); if ($null -ne $t) { $underruns = [double]$underruns + $t }
            $t = Get-CounterTotal $parsed.concealed.ToArray(); if ($null -ne $t) { $concealed = [double]$concealed + $t }
        }
        $rows[$entry.name] = @{ lines = $lineCount; pops = $pops; underruns = $underruns; concealed = $concealed }
        $Out.Add('  ' + $entry.name + ': ' + $lineCount + ' lines, pops ' + (Format-Number $pops 0) + ', underruns ' + (Format-Number $underruns 0) + ', concealed ' + (Format-Number $concealed 0))
        if ($script:measuredNow -and (@($script:measuredNow.names) -contains [string]$entry.name) -and ($script:measuredNow.groups.Contains('audio'))) {
            $Out.Add('      delay, target, per minute, video clock lag: ' + (Get-MeasuredGroupText $script:measuredNow ([string]$entry.name) 'audio'))
        }
    }
    $tables['audio'] = $rows
    $Out.Add('')
    # super resolution, frame generation, repeated pictures (quality and probe sessions; any run)
    $srRows = @(); $fgRows = @(); $unsupported = @(); $cadence = @(); $probeRuns = 0
    foreach ($item in $other) {
        $srRows += @($item.parsed.sr); $fgRows += @($item.parsed.fg); $unsupported += @($item.parsed.unsupported)
        if ($item.purpose -eq 'probe') { $probeRuns++; $cadence += @($item.parsed.cadence) }
    }
    $Out.Add('SUPER RESOLUTION COST (per choice, medians of 5 s windows)')
    $srRows = @($srRows | Where-Object { $_ })
    if ($srRows.Count -eq 0) {
        if (($unsupported -contains 'sr') -or ($unsupported -contains '[name]')) { $Out.Add('  not available in this build') } else { $Out.Add('  no [slipstream-sr] line (run -Session quality)') }
    }
    $srKeys = @()
    foreach ($e in $srRows) { $k = [string]$e.choice + '|' + [string]$e.quality + '|' + [string]$e.outSize; if ($srKeys -notcontains $k) { $srKeys += $k } }
    foreach ($srKey in $srKeys) {
        $these = @($srRows | Where-Object { ([string]$_.choice + '|' + [string]$_.quality + '|' + [string]$_.outSize) -eq $srKey })
        $choice = $these[0].choice
        $frames = 0.0; foreach ($e in $these) { if ($null -ne $e.frames) { $frames += $e.frames } }
        $Out.Add('  choice ' + $choice + ' quality ' + $these[-1].quality + ' ' + $these[-1].inSize + ' -> ' + $these[-1].outSize + ': GPU P50 ' + (Format-Number (Get-MedianOrNaN @($these | ForEach-Object { $_.gpuP50 } | Where-Object { $null -ne $_ })) 2) +
            ' ms, P95 ' + (Format-Number (Get-MedianOrNaN @($these | ForEach-Object { $_.gpuP95 } | Where-Object { $null -ne $_ })) 2) + ' ms, ' + [int]$frames + ' frames')
    }
    $Out.Add('')
    $holdTexts = @()
    foreach ($item in $other) {
        foreach ($h in @($item.parsed.holds)) {
            if (-not $h) { continue }
            $p50 = $h.p50; if (($null -ne $p50) -and ($p50 -lt 0)) { $p50 = $null }
            $p95 = $h.p95; if (($null -ne $p95) -and ($p95 -lt 0)) { $p95 = $null }
            $holdTexts += ($h.label + ': in-app latency P50 ' + (Format-Number $p50 2) + ' ms, P95 ' + (Format-Number $p95 2) + ' ms over ' + (Format-Number $h.seconds 0) + ' s')
        }
    }
    if ($holdTexts.Count -gt 0) { $Out.Add('  Holds (measured values only, no recommendation): ' + ($holdTexts -join '; ')) }
    $Out.Add('')
    $Out.Add('PRESENTATION (composed or independent flip; PresentMon''s MsUntilDisplayed is compared in report.md)')
    $anyChain = $false
    foreach ($entry in $Arms) {
        $chain = @{}; $scan = New-Object System.Collections.Generic.List[double]
        foreach ($parsed in @($read[$entry.name])) { if ($parsed) { foreach ($k in $parsed.swapchain.Keys) { $chain[$k] = $parsed.swapchain[$k] }; foreach ($v in $parsed.scanoutP50) { $scan.Add($v) } } }
        if (($chain.Count -eq 0) -and ($scan.Count -eq 0)) { continue }
        $anyChain = $true
        $Out.Add('  ' + $entry.name + ': flip ' + [string]$chain['flip'] + ', fullscreen ' + [string]$chain['fullscreen'] + ', window covers monitor ' + [string]$chain['windowCoversMonitor'] + ', app present to scanout P50 ' + (Format-Number (Get-MedianOrNaN $scan.ToArray()) 2) + ' ms')
    }
    if (-not $anyChain) { $Out.Add('  no [slipstream-swapchain] or [slipstream-latency] line (experimental build with VEYRA_EXP_RP_STATS=1 only)') }
    $Out.Add('')
    $Out.Add('FRAME GENERATION (per method and multiplier, medians of 5 s windows)')
    $fgRows = @($fgRows | Where-Object { $_ })
    if ($fgRows.Count -eq 0) {
        if (($unsupported -contains 'fg') -or ($unsupported -contains '[name]')) { $Out.Add('  not available in this build') } else { $Out.Add('  no [slipstream-fg] line (run -Session quality)') }
    }
    $fgKeys = @()
    foreach ($e in $fgRows) { $k = [string]$e.method + 'x' + [string]$e.multiplier; if ($fgKeys -notcontains $k) { $fgKeys += $k } }
    foreach ($fgKey in $fgKeys) {
        $these = @($fgRows | Where-Object { ([string]$_.method + 'x' + [string]$_.multiplier) -eq $fgKey })
        $shown = 0.0; $expired = 0.0; $skipped = 0.0
        foreach ($e in $these) { if ($null -ne $e.generatedShown) { $shown += $e.generatedShown }; if ($null -ne $e.generatedExpired) { $expired += $e.generatedExpired }; if ($null -ne $e.skippedPairs) { $skipped += $e.skippedPairs } }
        $Out.Add('  method ' + $these[0].method + ' x' + $these[0].multiplier + ': output ' + (Format-Number (Get-MedianOrNaN @($these | ForEach-Object { $_.outputFps } | Where-Object { $null -ne $_ })) 1) + ' FPS, generated shown ' + [int]$shown + ', expired ' + [int]$expired +
            ', skipped pairs ' + [int]$skipped + ', cost P50 ' + (Format-Number (Get-MedianOrNaN @($these | ForEach-Object { $_.costP50 } | Where-Object { $null -ne $_ })) 2) + ' / P95 ' + (Format-Number (Get-MedianOrNaN @($these | ForEach-Object { $_.costP95 } | Where-Object { $null -ne $_ })) 2) +
            ' ms, shown gap P95 ' + (Format-Number (Get-MedianOrNaN @($these | ForEach-Object { $_.shownGapP95 } | Where-Object { $null -ne $_ })) 2) + ' ms, pair delay ' + (Format-Number (Get-MedianOrNaN @($these | ForEach-Object { $_.pairDelayMs } | Where-Object { $null -ne $_ })) 2) + ' ms')
    }
    $Out.Add('')
    if ($probeRuns -gt 0) {
        $sums = [ordered]@{ repeatsFound = 0.0; repeatsRemoved = 0.0; exactVetoes = 0.0; suspendedKeeps = 0.0; keptUnsure = 0.0; unmeasured = 0.0 }
        foreach ($c in $cadence) { foreach ($key in @($sums.Keys)) { $sums[$key] += $c[$key] } }
        $found = $sums['repeatsFound']; $inexact = $sums['exactVetoes']
        if ($cadence.Count -eq 0) { $sentence = 'no [remoteplay-cadence] line: the probe did not run (needs the experimental build and a PS5 stream)' }
        elseif ($found -le 0) { $sentence = 'exact repeats seen: no repeated pictures were found at all (was the game running at 30 FPS?)' }
        else {
            $exact = [math]::Max(0, $found - $inexact)
            if ($exact -gt 0) { $sentence = 'exact repeats seen: yes, ' + [int]$exact + ' of ' + [int]$found + ' repeated pictures (' + (Format-Number (100.0 * $exact / $found) 0) + ' %) decoded exactly' }
            else { $sentence = 'exact repeats seen: no, none of the ' + [int]$found + ' repeated pictures decoded exactly' }
        }
        $Out.Add('REPEAT PICTURES IN THIS STREAM (diagnostic only, never a latency comparison; duplicate detection measured and removed nothing)')
        $parts = @(); foreach ($key in $sums.Keys) { $parts += ($key + ' ' + [int]$sums[$key]) }
        $Out.Add('  ' + $probeRuns + ' run(s), ' + $cadence.Count + ' windows of 10 s: ' + ($parts -join ', '))
        $Out.Add('  ' + $sentence + '.')
        $Out.Add('')
        $tables['dedup'] = [pscustomobject]@{ runs = $probeRuns; windows = $cadence.Count; repeatsFound = $found; exactVetoes = $inexact; sentence = $sentence }
    }
    return $tables
}

# The precision tables as JSON values: NaN becomes null, numbers are rounded as Get-JsonNumber does.
function ConvertTo-JsonTables($Value) {
    if ($null -eq $Value) { return $null }
    if ($Value -is [System.Collections.IDictionary]) {
        $o = [ordered]@{}
        foreach ($k in $Value.Keys) { $o[[string]$k] = ConvertTo-JsonTables $Value[$k] }
        return [pscustomobject]$o
    }
    if (($Value -is [double]) -or ($Value -is [single])) { return (Get-JsonNumber $Value) }
    if (($Value -is [string]) -or ($Value -is [int]) -or ($Value -is [long]) -or ($Value -is [bool])) { return $Value }
    if ($Value -is [System.Management.Automation.PSCustomObject]) {
        $o = [ordered]@{}
        foreach ($p in $Value.PSObject.Properties) { $o[$p.Name] = ConvertTo-JsonTables $p.Value }
        return [pscustomobject]$o
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        $list = @()
        foreach ($item in $Value) { $list += ,(ConvertTo-JsonTables $item) }
        return ,$list
    }
    return $Value
}

# --- the network session (-Session network, SmartLink): the same definitions as report.py network_tables -------------
# The unit of analysis is the RUN (the mean of its 5 s windows); verdict words only for the two primary metrics, each with a
# Welch 97.5 % interval (Bonferroni over 2). Fewer than 2 runs per arm: "indication only: needs more runs". report.py the same.
$networkMinimumWindows = 6
$networkMinimumRuns = 2
$motionTargetShare = 0.40
$motionRequestedShare = 0.20
$motionTolerance = 0.25
$motionAdvice = 'The picture hardly moved in this test: play something with movement and run it again.'
$networkPrimary = @{ 'stutterPerMinute' = 'stability'; 'arrivalToScanoutP95' = 'cost' }
$t9875 = @(25.452, 6.205, 4.177, 3.495, 3.163, 2.969, 2.841, 2.752, 2.685, 2.634, 2.593, 2.560, 2.533, 2.510, 2.490,
    2.473, 2.458, 2.445, 2.433, 2.423, 2.414, 2.405, 2.398, 2.391, 2.385, 2.379, 2.373, 2.368, 2.364, 2.360)
$networkMetrics = @(
    @('arrivalToScanoutP50', 'in-app latency P50 (arrival to scanout) ms', $true),
    @('arrivalToScanoutP95', 'in-app latency P95 ms', $true),
    @('arrivalToScanoutP99', 'in-app latency P99 ms', $true),
    @('stutterPerMinute', 'stutter events per minute (gaps over 25 ms)', $false),
    @('gapP95', 'frame gap P95 ms', $false),
    @('replacedPercent', 'frames replaced %', $false),
    @('audioDelayMs', 'audio delay ms (delayMs; a cost, shown before any gain in dropouts)', $false),
    @('popsPerMinute', 'audio pops per minute', $false),
    @('clicksPerMinute', 'audio clicks per minute (dropouts without a fade)', $false),
    @('underrunsPerMinute', 'audio underruns per minute', $false),
    @('underrunStartsPerMinute', 'audio underrun starts per minute (underrunStarts=, per window)', $false),
    @('tailsPerMinute', 'audio tails per minute (tails=, per window)', $false),
    @('concealedPerMinute', 'audio concealed per minute', $false)
)
$networkAdvice = @{
    'wired' = 'Wired link: SmartLink leaves every setting as it is.'
    'strong-wifi' = 'Strong Wi-Fi: fine for play. A cable still gives the lowest delay.'
    'unstable-wifi' = 'Unstable Wi-Fi: move the PC or the router closer, keep other devices from big downloads or videos while playing, or connect a cable.'
    'unknown' = 'The connection type was not recognised.'
}
$regimeText = @{ 'periodic-spikes' = 'short delays that come back regularly (often another device or a background scan)'; 'congested' = 'the network was busy'; 'weak-signal' = 'the signal was weak'; 'interrupted' = 'the connection dropped out for a moment' }

# --- the short sessions (-Session presentation, -Session audio): measured values per arm, no verdict word ----------------
# report.py window_facts_paragraphs writes the same text. Keys from the app's format strings: [slipstream-latency]
# (PushPresentTiming.h formatSlipstreamLatency) and [slipstream-present] window ... (formatSlipstreamWindow, only with
# VEYRA_EXP_PRESENT_WINDOW_FACTS=1).
$windowFactKeys = @('regionKind', 'coversMonitor', 'ownOverlaps', 'flipBlockers')
function Get-MostCommonText($Values) {
    if ($Values.Count -eq 0) { return 'not logged' }
    $counts = @{}; $order = @()
    foreach ($v in $Values) { if (-not $counts.ContainsKey($v)) { $counts[$v] = 0; $order += $v }; $counts[$v]++ }
    $best = $order[0]; foreach ($v in $order) { if ($counts[$v] -gt $counts[$best]) { $best = $v } }
    return ($best + ' in ' + (Format-Number (100.0 * $counts[$best] / $Values.Count) 0) + ' % of ' + $Values.Count + ' lines')
}
function Add-ShortSessionSection($Out, [string]$Folder, $Kept, [string]$Route) {
    $wanted = @()
    if ($Session -eq 'presentation') { $wanted = $presentationArms } elseif ($Session -eq 'audio') { $wanted = $audioArms } else { return }
    $Out.Add('SHORT SESSION: ' + $Session.ToUpperInvariant() + ' (measured values only, no verdict word: ' + $shortSessionRounds + ' rounds of ' + $shortSessionPresses + ' presses are too few for one)')
    if ($Route -eq 'wifi') { $Out.Add('  ' + $wifiLabel) }
    foreach ($name in $wanted) {
        $runs = @($Kept | Where-Object { $_.record.arm -eq $name })
        if ($runs.Count -eq 0) { $Out.Add('  ' + $name + ': no run'); continue }
        $p50 = New-Object System.Collections.Generic.List[double]; $p95 = New-Object System.Collections.Generic.List[double]
        $under = 0.0; $oneTwo = 0.0; $over = 0.0
        $facts = @{}; foreach ($key in $windowFactKeys) { $facts[$key] = @() }
        $clicks = 0.0; $clickSeconds = 0.0
        foreach ($run in $runs) {
            $file = Join-Path (Join-Path $Folder 'bench') ($run.name + '.txt')
            if (-not (Test-Path -LiteralPath $file)) { continue }
            $firstClicks = $null; $lastClicks = $null; $firstT = $null; $lastT = $null
            foreach ($line in @(Get-Content -LiteralPath $file)) {
                if ($line -notmatch '^(\S+Z) t=\d+ \[[A-Z ]+\] \[([^\]]+)\] (.*)$') { continue }
                $when = $Matches[1]; $component = $Matches[2]; $text = $Matches[3]
                if ($component -eq 'slipstream-latency') {
                    $v = Get-LogNumber $text 'presentToScanoutP50'; if (($null -ne $v) -and ($v -ge 0)) { $p50.Add($v) }
                    $v = Get-LogNumber $text 'presentToScanoutP95'; if (($null -ne $v) -and ($v -ge 0)) { $p95.Add($v) }
                    $v = Get-LogNumber $text 'scanoutUnder1Refresh'; if ($null -ne $v) { $under += $v }
                    $v = Get-LogNumber $text 'scanout1To2Refresh'; if ($null -ne $v) { $oneTwo += $v }
                    $v = Get-LogNumber $text 'scanoutOver2Refresh'; if ($null -ne $v) { $over += $v }
                } elseif (($component -eq 'slipstream-present') -and ($text -match '^window event=')) {
                    $words = Get-LineWords $text
                    foreach ($key in $windowFactKeys) { if ($words.ContainsKey($key)) { $facts[$key] += [string]$words[$key] } }
                } elseif (($component -eq 'remoteplay-audio') -and ($text -match 'underruns=|pops=') -and ($text -match '(?<![A-Za-z])clicks=')) {
                    $v = Get-LogNumber $text 'clicks'; $t = Get-StampSecondsOf $when
                    if (($null -ne $v) -and ($null -ne $t)) { if ($null -eq $firstClicks) { $firstClicks = $v; $firstT = $t }; $lastClicks = $v; $lastT = $t }
                }
            }
            if (($null -ne $firstClicks) -and ($lastT -gt $firstT)) { $clicks += [math]::Max(0.0, $lastClicks - $firstClicks); $clickSeconds += ($lastT - $firstT) }
        }
        $buckets = $under + $oneTwo + $over
        if ($Session -eq 'presentation') {
            $text = '  ' + $name + ' (' + $runs.Count + ' runs): present to screen P50 ' + (Format-Number (Get-MedianOrNaN $p50.ToArray()) 2) + ' ms, P95 ' + (Format-Number (Get-MedianOrNaN $p95.ToArray()) 2) + ' ms (medians of ' + $p50.Count + ' windows of [slipstream-latency])'
            if ($buckets -gt 0) { $text += '; frames on screen within 1 refresh ' + (Format-Number (100.0 * $under / $buckets) 1) + ' %, 1 to 2 refreshes ' + (Format-Number (100.0 * $oneTwo / $buckets) 1) + ' %, over 2 refreshes ' + (Format-Number (100.0 * $over / $buckets) 1) + ' %' }
            else { $text += '; refresh buckets not logged' }
            $parts = @(); foreach ($key in $windowFactKeys) { $parts += ($key + ' ' + (Get-MostCommonText $facts[$key])) }
            $Out.Add($text + '. Window facts: ' + ($parts -join ', ') + '.')
        } else {
            if ($clickSeconds -gt 0) { $Out.Add('  ' + $name + ' (' + $runs.Count + ' runs): clicks= rose by ' + [int]$clicks + ' in ' + (Format-Number ($clickSeconds / 60.0) 1) + ' min (' + (Format-Number ($clicks * 60.0 / $clickSeconds) 2) + ' per minute)') }
            else { $Out.Add('  ' + $name + ' (' + $runs.Count + ' runs): no [remoteplay-audio] clicks= line') }
        }
    }
    if ($Session -eq 'presentation') {
        $Out.Add('  regionKind 0 = no window region; coversMonitor 1 = the video window covers the whole monitor; ownOverlaps = PushFPS windows over the picture; flipBlockers 0x0 = nothing the app knows of stops an independent flip. Whether Windows really composed the window: PresentMon''s present mode in report.md (presentmon.py), only when PresentMon ran.')
    }
    $Out.Add('')
}

# Welch interval at 97.5 % of mean(A) - mean(B) over per-run values: report.py welch_interval (df rounded down, 1..30).
function Get-WelchInterval([double[]]$A, [double[]]$B) {
    if (($A.Count -lt 2) -or ($B.Count -lt 2)) { return @([double]::NaN, [double]::NaN, [double]::NaN) }
    $ma = 0.0; foreach ($v in $A) { $ma += $v }; $ma /= $A.Count
    $mb = 0.0; foreach ($v in $B) { $mb += $v }; $mb /= $B.Count
    $va = 0.0; foreach ($v in $A) { $va += ($v - $ma) * ($v - $ma) }; $va /= ($A.Count - 1)
    $vb = 0.0; foreach ($v in $B) { $vb += ($v - $mb) * ($v - $mb) }; $vb /= ($B.Count - 1)
    $qa = $va / $A.Count; $qb = $vb / $B.Count
    $diff = $ma - $mb
    if (($qa + $qb) -le 0) { return @($diff, $diff, $diff) }
    $den = 0.0
    if ($qa -gt 0) { $den += $qa * $qa / ($A.Count - 1) }
    if ($qb -gt 0) { $den += $qb * $qb / ($B.Count - 1) }
    $df = [double]($A.Count + $B.Count - 2)
    if ($den -gt 0) { $df = ($qa + $qb) * ($qa + $qb) / $den }
    $index = [int][math]::Floor($df)
    if ($index -lt 1) { $index = 1 }
    if ($index -gt 30) { $index = 30 }
    $half = $t9875[$index - 1] * [math]::Sqrt($qa + $qb)
    return @($diff, ($diff - $half), ($diff + $half))
}

# The smallest effect worth finding per verdict metric: report.py NETWORK_EFFECT (the same numbers; the procedure's table).
# @(kind, value, floor): 'relative' = share of the reference arm's mean (at least floor), 'absolute' = value. When the
# interval's half-width is larger than the effect the verdict is "cannot tell", never "no difference shown".
$networkEffect = @{ 'stutterPerMinute' = @('relative', 0.30, 0.5); 'arrivalToScanoutP95' = @('absolute', 1.0, 0.0) }
$effectUnit = @{ 'stutterPerMinute' = 'stutter events per minute'; 'arrivalToScanoutP95' = 'ms' }
$networkRunsCap = 20
$unitsUnknown = 'not known in this session'
$powerZ = 0.842

function Get-EffectSize([string]$Key, [double[]]$Reference) {
    $e = $networkEffect[$Key]
    if ($e[0] -eq 'absolute') { return [double]$e[1] }
    $m = 0.0; if ($Reference.Count -gt 0) { foreach ($v in $Reference) { $m += $v }; $m /= $Reference.Count }
    return [math]::Max([double]$e[1] * $m, [double]$e[2])
}

# Runs per arm for a 97.5 % interval to find the effect with 80 % power: report.py runs_needed.
function Get-RunsNeeded([double[]]$A, [double[]]$B, [double]$Effect) {
    if (($A.Count -lt 2) -or ($B.Count -lt 2) -or ($Effect -le 0)) { return ($networkRunsCap + 1) }
    $ma = 0.0; foreach ($v in $A) { $ma += $v }; $ma /= $A.Count
    $mb = 0.0; foreach ($v in $B) { $mb += $v }; $mb /= $B.Count
    $va = 0.0; foreach ($v in $A) { $va += ($v - $ma) * ($v - $ma) }; $va /= ($A.Count - 1)
    $vb = 0.0; foreach ($v in $B) { $vb += ($v - $mb) * ($v - $mb) }; $vb /= ($B.Count - 1)
    $s = [math]::Sqrt(($va + $vb) / 2.0)
    for ($n = 2; $n -le $networkRunsCap; $n++) {
        $index = [math]::Min(30, 2 * $n - 2)
        if ((($t9875[$index - 1] + $powerZ) * $s * [math]::Sqrt(2.0 / $n)) -le $Effect) { return $n }
    }
    return ($networkRunsCap + 1)
}

# The verdict when the interval contains zero: report.py no_difference_text.
function Get-NoDifferenceText([string]$Key, [double[]]$A, [double[]]$B, [double]$Low, [double]$High) {
    $effect = Get-EffectSize $Key $B
    $half = ($High - $Low) / 2.0
    $what = (Format-Number $effect 2) + ' ' + $effectUnit[$Key]
    if ($half -le $effect) { return ('no difference shown (interval +-' + (Format-Number $half 2) + ', narrower than ' + $what + ')') }
    $n = [math]::Min($A.Count, $B.Count)
    $m = [math]::Max((Get-RunsNeeded $A $B $effect), $n + 1)
    $more = 'about ' + $m
    if ($m -gt $networkRunsCap) { $more = 'more than ' + $networkRunsCap }
    return ('cannot tell with ' + $n + ' runs per arm: this session would need ' + $more + ' runs per arm to show a difference of ' + $what)
}

# Per-run means of one metric (runs with at least $networkMinimumWindows windows); 'addedWaitMs' from the run's facts.
function Get-RunValues([string[]]$BenchFiles, [string]$Key) {
    $out = New-Object System.Collections.Generic.List[double]
    foreach ($file in $BenchFiles) {
        $one = Get-NetworkWindows @($file)
        if ($Key -eq 'addedWaitMs') { if (-not [double]::IsNaN($one.facts.addedWaitMs)) { $out.Add($one.facts.addedWaitMs) }; continue }
        $values = $one.windows[$Key]
        if ($values.Count -ge $networkMinimumWindows) { $sum = 0.0; foreach ($v in $values) { $sum += $v }; $out.Add($sum / $values.Count) }
    }
    return ,($out.ToArray())
}

# '' when both arms saw similar motion, else the reason: report.py motion_gate.
function Get-MotionGate($Fa, $Fb) {
    if (($null -eq $Fa.motion) -or ($null -eq $Fb.motion)) { return 'motion not known (no video rate, no console target and no requested bitrate)' }
    if (($Fa.motion -eq $false) -or ($Fb.motion -eq $false)) { return ('the picture hardly moved in one arm (' + $motionAdvice + ')') }
    if ([math]::Abs($Fa.motionShare - $Fb.motionShare) -gt $motionTolerance) {
        return ('the arms saw different motion (video rate ' + (Format-Number ($Fa.motionShare * 100) 0) + ' % and ' + (Format-Number ($Fb.motionShare * 100) 0) + ' % of the reference rate; at most ' + (Format-Number ($motionTolerance * 100) 0) + ' points apart)')
    }
    return ''
}

function Get-StampSecondsOf([string]$Stamp) {
    return ([datetime]::ParseExact($Stamp.Substring(0, 23), 'yyyy-MM-ddTHH:mm:ss.fff', $invariant) - [datetime]'2000-01-01').TotalSeconds
}

# Per-window rate of a counter: increments when the values never fall (a running total), else the values themselves.
function Add-RatePerMinute($List, $Points, [bool]$PerWindow = $false) {
    $p = @($Points)
    if ($p.Count -lt 2) { return }
    $rising = (-not $PerWindow)
    for ($i = 1; $i -lt $p.Count; $i++) { if ($p[$i][1] -lt $p[$i - 1][1]) { $rising = $false } }
    for ($i = 1; $i -lt $p.Count; $i++) {
        $dt = $p[$i][0] - $p[$i - 1][0]
        if ($dt -le 0) { continue }
        if ($rising) { $List.Add(($p[$i][1] - $p[$i - 1][1]) * 60.0 / $dt) } else { $List.Add($p[$i][1] * 60.0 / $dt) }
    }
}

# Every network metric of one arm as per-window values, and its facts.
function Get-NetworkWindows([string[]]$BenchFiles) {
    $w = @{}
    foreach ($m in $networkMetrics) { $w[$m[0]] = New-Object System.Collections.Generic.List[double] }
    $rtt = New-Object System.Collections.Generic.List[double]; $target = New-Object System.Collections.Generic.List[double]; $measured = New-Object System.Collections.Generic.List[double]
    $video = New-Object System.Collections.Generic.List[double]; $requested = New-Object System.Collections.Generic.List[double]
    $profiles = @{}; $regimes = @{}; $wifiModes = @{}; $reorder = @(); $netLimited = 0; $netLines = 0; $headroom = 0; $headLines = 0; $smartLines = 0
    $rttUndecided = $false; $rateUndecided = $false
    foreach ($file in $BenchFiles) {
        if (-not (Test-Path -LiteralPath $file)) { continue }
        $pops = @(); $clicks = @(); $starts = @(); $tails = @(); $underruns = @(); $concealed = @(); $healthRtt = @(); $healthTarget = @(); $healthMeasured = @(); $runSmart = 0
        foreach ($line in @(Get-Content -LiteralPath $file)) {
            if ($line -notmatch '^(\S+Z) t=\d+ \[[A-Z ]+\] \[([^\]]+)\] (.*)$') { continue }
            $when = $Matches[1]; $component = $Matches[2]; $text = $Matches[3]
            if ($component -eq 'slipstream-latency') {
                foreach ($key in @('arrivalToScanoutP50', 'arrivalToScanoutP95', 'arrivalToScanoutP99')) { $v = Get-LogNumber $text $key; if (($null -ne $v) -and ($v -ge 0)) { $w[$key].Add($v) } }
            } elseif ($component -eq 'slipstream-delivery') {
                $v = Get-LogNumber $text 'longGaps'; if ($null -ne $v) { $w['stutterPerMinute'].Add($v * 12.0) }
                $v = Get-LogNumber $text 'gapP95'; if ($null -ne $v) { $w['gapP95'].Add($v) }
                $arrived = Get-LogNumber $text 'arrived'
                if (($null -ne $arrived) -and ($arrived -ne 0)) { $replaced = Get-LogNumber $text 'replaced'; if ($null -eq $replaced) { $replaced = 0.0 }; $w['replacedPercent'].Add(100.0 * $replaced / $arrived) }
            } elseif (($component -eq 'remoteplay-audio') -and (($text -match 'underruns=') -or ($text -match 'pops='))) {
                $t = Get-StampSecondsOf $when
                $v = Get-LogNumber $text 'pops'; if ($null -ne $v) { $pops += ,@($t, $v) }
                $v = Get-LogNumber $text 'underruns'; if ($null -ne $v) { $underruns += ,@($t, $v) }
                $v = Get-LogNumber $text 'concealed'; if ($null -ne $v) { $concealed += ,@($t, $v) }
                $v = Get-LogNumber $text 'clicks'; if ($null -ne $v) { $clicks += ,@($t, $v) }
                $v = Get-LogNumber $text 'underrunStarts'; if ($null -ne $v) { $starts += ,@($t, $v) }
                $v = Get-LogNumber $text 'tails'; if ($null -ne $v) { $tails += ,@($t, $v) }
                $v = Get-LogNumber $text 'delayMs'; if (($null -ne $v) -and ($v -ge 0)) { $w['audioDelayMs'].Add($v) }
            } elseif ($component -eq 'slipstream-smartlink') {
                $runSmart++; $smartLines++
                $v = Get-LogNumber $text 'rttMs'; if (($null -ne $v) -and ($v -ge 0)) { $rtt.Add($v) }
                $v = Get-LogNumber $text 'targetKbps'; if (($null -ne $v) -and ($v -ge 0)) { $target.Add($v) }
                $v = Get-LogNumber $text 'measuredKbps'; if (($null -ne $v) -and ($v -ge 0)) { $measured.Add($v) }
                $words = Get-LineWords $text
                $profile = [string]$words['profile']; if (-not $profile) { $profile = 'unknown' }
                $regime = [string]$words['regime']; if (-not $regime) { $regime = 'unknown' }
                $profiles[$profile] = 1 + [int]$profiles[$profile]; $regimes[$regime] = 1 + [int]$regimes[$regime]
                $v = Get-LogNumber $text 'reorderMs'; if ($null -ne $v) { $reorder += $v }
                $v = Get-LogNumber $text 'netLimited'; if ($null -ne $v) { $netLines++; if ($v -gt 0) { $netLimited++ } }
                $v = Get-LogNumber $text 'headroom'; if ($null -ne $v) { $headLines++; if ($v -gt 0) { $headroom++ } }
                $mode = [string]$words['wifiStreamingMode']; if ($mode) { $wifiModes[$mode] = 1 + [int]$wifiModes[$mode] }
            } elseif (($component -eq 'remoteplay-health') -and ($text -match '^grade=')) {
                # the console fields of ConnectionHealth.h formatHealthLine (-1 = unknown): report.py network_windows
                $v = Get-LogNumber $text 'consoleRttMs'; if (($null -ne $v) -and ($v -ge 0)) { $healthRtt += $v }
                $v = Get-LogNumber $text 'consoleTargetKbps'; if (($null -ne $v) -and ($v -ge 0)) { $healthTarget += $v }
                $v = Get-LogNumber $text 'consoleMeasuredKbps'; if (($null -ne $v) -and ($v -ge 0)) { $healthMeasured += $v }
            } elseif (($component -eq 'remoteplay-health') -and ($text -match '^consoleUnits rtt=(\S+) rate=(\S+)')) {
                # once per session (ConsoleQuality.h takeLogLine): an undecided unit makes the figures "not known in this session"
                if ($Matches[1] -eq 'undecided') { $rttUndecided = $true }
                if ($Matches[2] -eq 'undecided') { $rateUndecided = $true }
            } elseif ($component -eq 'remoteplay-progress') {
                $v = Get-LogNumber $text 'videoMbps'; if (($null -ne $v) -and ($v -gt 0)) { $video.Add($v) }
            } elseif (($component -eq 'remoteplay') -and ($text -match '^session started .*requestedBitrateKbps=(\d+)')) {
                $requested.Add([double]$Matches[1])
            }
        }
        Add-RatePerMinute $w['popsPerMinute'] $pops
        Add-RatePerMinute $w['underrunsPerMinute'] $underruns
        Add-RatePerMinute $w['concealedPerMinute'] $concealed
        Add-RatePerMinute $w['clicksPerMinute'] $clicks
        Add-RatePerMinute $w['underrunStartsPerMinute'] $starts $true
        Add-RatePerMinute $w['tailsPerMinute'] $tails $true
        if ($runSmart -eq 0) { foreach ($v in $healthRtt) { $rtt.Add($v) }; foreach ($v in $healthTarget) { $target.Add($v) }; foreach ($v in $healthMeasured) { $measured.Add($v) } }
    }
    if ($rttUndecided) { $rtt.Clear() }
    if ($rateUndecided) { $target.Clear(); $measured.Clear() }
    $changes = 0
    for ($i = 1; $i -lt $reorder.Count; $i++) { if ($reorder[$i] -ne $reorder[$i - 1]) { $changes++ } }
    $added = [double]::NaN
    if ($reorder.Count -gt 0) { $sum = 0.0; foreach ($v in $reorder) { $sum += $v }; $added = $sum / $reorder.Count }
    $facts = [ordered]@{ rttMin = [double]::NaN; rttMedian = [double]::NaN; rttP95 = [double]::NaN; rttMax = [double]::NaN
        targetKbps = (Get-MedianOrNaN $target.ToArray()); measuredKbps = (Get-MedianOrNaN $measured.ToArray()); requestedKbps = (Get-MedianOrNaN $requested.ToArray())
        videoMbps = (Get-MedianOrNaN $video.ToArray()); profiles = $profiles; regimes = $regimes; reorderChanges = $changes; addedWaitMs = $added
        netLimitedPercent = $(if ($netLines -gt 0) { 100.0 * $netLimited / $netLines } else { [double]::NaN }); headroomPercent = $(if ($headLines -gt 0) { 100.0 * $headroom / $headLines } else { [double]::NaN })
        smartlinkLines = $smartLines; motion = $null; motionBasis = 'unknown'; motionShare = [double]::NaN; wifiStreamingMode = $wifiModes
        rttUndecided = $rttUndecided; rateUndecided = $rateUndecided }
    if ($rtt.Count -gt 0) {
        $sorted = $rtt.ToArray(); [Array]::Sort($sorted)
        $facts.rttMin = $sorted[0]; $facts.rttMedian = Get-Quantile $sorted 0.5; $facts.rttP95 = Get-Quantile $sorted 0.95; $facts.rttMax = $sorted[$sorted.Count - 1]
    }
    if (-not [double]::IsNaN($facts.videoMbps)) {
        if ((-not [double]::IsNaN($facts.targetKbps)) -and ($facts.targetKbps -gt 0)) {
            $facts.motionBasis = 'console target'; $facts.motionShare = $facts.videoMbps * 1000.0 / $facts.targetKbps; $facts.motion = ($facts.motionShare -ge $motionTargetShare)
        } elseif ((-not [double]::IsNaN($facts.requestedKbps)) -and ($facts.requestedKbps -gt 0)) {
            $facts.motionBasis = 'requested'; $facts.motionShare = $facts.videoMbps * 1000.0 / $facts.requestedKbps; $facts.motion = ($facts.motionShare -ge $motionRequestedShare)
        }
    }
    return @{ windows = $w; facts = $facts }
}

function Get-TopKey($Table) {
    $best = ''; $count = -1
    foreach ($k in $Table.Keys) { if ([int]$Table[$k] -gt $count) { $best = [string]$k; $count = [int]$Table[$k] } }
    return $best
}

function Get-ConnectionText($Facts, [string]$Route) {
    $parts = @()
    $total = 0; foreach ($k in $Facts.profiles.Keys) { $total += [int]$Facts.profiles[$k] }
    if ($total -gt 0) {
        $profile = Get-TopKey $Facts.profiles
        $parts += ('Detected connection: ' + $profile + ' (' + (Format-Number (100.0 * $Facts.profiles[$profile] / $total) 0) + ' % of the time).')
    } else {
        $profile = 'unknown'; if ($Route -eq 'wired') { $profile = 'wired' }
        $parts += 'No SmartLink line: the connection type comes from the network route only.'
    }
    if ($networkAdvice.ContainsKey($profile)) { $parts += $networkAdvice[$profile] } else { $parts += $networkAdvice['unknown'] }
    $regime = Get-TopKey $Facts.regimes
    if ($regimeText.ContainsKey($regime)) { $parts += ('Most of the time: ' + $regimeText[$regime] + '.') }
    if ((-not [double]::IsNaN($Facts.netLimitedPercent)) -and ($Facts.netLimitedPercent -gt 50)) { $parts += 'Stutter came from the connection, so lowering graphics settings would not help.' }
    if ((-not [double]::IsNaN($Facts.headroomPercent)) -and ($Facts.headroomPercent -gt 50)) { $parts += 'The connection carried the requested quality with room to spare.' }
    if ($Facts.wifiStreamingMode.ContainsKey('refused')) { $parts += 'Wi-Fi streaming mode: the adapter did not accept it (not an error).' }
    elseif ($Facts.wifiStreamingMode.ContainsKey('requested')) { $parts += 'Wi-Fi streaming mode: requested from the adapter for the stream.' }
    if ($Route -eq 'wifi') { $parts += ($wifiLabel + '.') }
    return ($parts -join ' ')
}

# The network session's part of RESULT.txt; returns the values for result.json (networkSession), $null without a network run.
function Add-NetworkSection($Out, [string]$Folder, $Kept, $ArmTable, [string]$Route) {
    $runs = @($Kept | Where-Object { $_.record.purpose -eq 'network' })
    if ($runs.Count -eq 0) { return $null }
    $names = @()
    foreach ($run in $runs) { if ($names -notcontains [string]$run.record.arm) { $names += [string]$run.record.arm } }
    $label = ''; if ($Route -eq 'wifi') { $label = $wifiLabel }
    $data = @{}; $keys = @{}; $keysDiffer = @{}; $exe = @{}; $armFiles = @{}; $gates = @()
    foreach ($name in $names) {
        $files = @(); $one = @{}; $differ = @()
        foreach ($run in @($runs | Where-Object { $_.record.arm -eq $name })) {
            $file = Join-Path (Join-Path $Folder 'bench') ($run.name + '.txt')
            $files += $file
            $lines = @(); if (Test-Path -LiteralPath $file) { $lines = @(Get-Content -LiteralPath $file) }
            $runKeys = Get-KeyTable (Get-RunConfig $lines).keys
            foreach ($k in $runKeys.Keys) { if ($one.ContainsKey($k)) { if (($one[$k] -cne $runKeys[$k]) -and ($differ -notcontains $k)) { $differ += $k } } else { $one[$k] = $runKeys[$k] } }
        }
        $data[$name] = Get-NetworkWindows $files
        $armFiles[$name] = $files
        $keys[$name] = $one; $keysDiffer[$name] = $differ
        $row = @($ArmTable | Where-Object { $_.name -eq $name })
        $exe[$name] = 'experimental'; if ($row.Count -gt 0) { $exe[$name] = [string]$row[0].exe }
    }
    $reference = $names[0]
    foreach ($name in $names) { $row = @($ArmTable | Where-Object { $_.name -eq $name }); if (($row.Count -gt 0) -and ($row[0].compare -eq $name) -and ($exe[$name] -eq 'experimental')) { $reference = $name; break } }
    $json = [ordered]@{ route = $Route; label = $label; reference = $reference; arms = [ordered]@{}; builds = [ordered]@{} }
    $Out.Add('NETWORK SESSION (SmartLink; per 5 s window; software counters)')
    if ($label) { $Out.Add('  ' + $label) }
    $Out.Add('  Connection: ' + (Get-ConnectionText $data[$reference].facts $Route))
    $worse = @()
    $lines = New-Object System.Collections.Generic.List[string]
    foreach ($name in $names) {
        $f = $data[$name].facts
        $motionText = 'unknown'; if ($f.motion -eq $true) { $motionText = 'yes' } elseif ($f.motion -eq $false) { $motionText = 'LITTLE (a still scene speaks only for a still scene)' }
        if ($null -ne $f.motion) { $motionText += (' (' + (Format-Number ($f.motionShare * 100) 0) + ' % of the ' + $f.motionBasis + ' rate)') }
        $rttText = (Format-Number $f.rttMin 1) + ' / ' + (Format-Number $f.rttMedian 1) + ' / ' + (Format-Number $f.rttP95 1) + ' / ' + (Format-Number $f.rttMax 1) + ' ms'
        if ($f.rttUndecided) { $rttText = $unitsUnknown }
        $rateText = (Format-Number $f.targetKbps 0) + ' / ' + (Format-Number $f.measuredKbps 0) + ' / ' + (Format-Number $f.requestedKbps 0)
        if ($f.rateUndecided) { $rateText = $unitsUnknown + ' / ' + $unitsUnknown + ' / ' + (Format-Number $f.requestedKbps 0) }
        $lines.Add('  ' + $name + ': round trip min/median/P95/max ' + $rttText +
            '; target/measured/requested ' + $rateText + ' kbit/s; motion ' + $motionText +
            '; reorder changes ' + $f.reorderChanges + ', added wait ' + (Format-Number $f.addedWaitMs 2) + ' ms')
        $armJson = [ordered]@{ exe = $exe[$name]; runs = @($armFiles[$name]).Count; facts = $f; metrics = [ordered]@{}; verdicts = @(); motionGate = '' }
        $gate = ''
        if (($name -ne $reference) -and ($exe[$name] -eq 'experimental')) {
            $gate = Get-MotionGate $data[$name].facts $data[$reference].facts
            $armJson.motionGate = $gate
            if ($gate -and ($gates -notcontains $gate)) { $gates += $gate }
            $wa = Get-RunValues $armFiles[$name] 'addedWaitMs'; $wb = Get-RunValues $armFiles[$reference] 'addedWaitMs'
            $waitDiff = [double]::NaN
            if (($wa.Count -gt 0) -and ($wb.Count -gt 0)) { $waitDiff = (Get-Mean $wa) - (Get-Mean $wb) }
            $armJson.addedWait = [ordered]@{ difference = $waitDiff; runs = @($wa.Count, $wb.Count) }
            $lines.Add('    reorder added wait (latency-cost indicator, descriptive): ' + (Format-Number $waitDiff 2) + ' ms against ' + $reference)
        }
        foreach ($m in $networkMetrics) {
            $values = $data[$name].windows[$m[0]].ToArray()
            $mean = [double]::NaN; if ($values.Count -gt 0) { $mean = Get-Mean $values }
            $armJson.metrics[$m[0]] = [ordered]@{ n = $values.Count; mean = $mean }
            $text = '    ' + $m[1] + ': mean ' + (Format-Number $mean 2) + ' (' + $values.Count + ' windows)'
            if (($name -ne $reference) -and ($exe[$name] -eq 'experimental')) {
                $ra = Get-RunValues $armFiles[$name] $m[0]; $rb = Get-RunValues $armFiles[$reference] $m[0]
                $d = Get-WelchInterval $ra $rb
                if ([double]::IsNaN($d[0]) -and ($ra.Count -gt 0) -and ($rb.Count -gt 0)) { $d = @(((Get-Mean $ra) - (Get-Mean $rb)), [double]::NaN, [double]::NaN) }
                $primary = ''; if ($networkPrimary.ContainsKey($m[0])) { $primary = $networkPrimary[$m[0]] }
                $row = @($ArmTable | Where-Object { $_.name -eq $name })
                $allowed = ''; if ($row.Count -gt 0) { $allowed = [string]$row[0].differs }
                $differ = @(Compare-ConfigKeys $keys[$name] $name $keys[$reference] $reference $allowed)
                if (($keysDiffer[$name].Count + $keysDiffer[$reference].Count) -gt 0) { $differ += ('the runs of one arm differ in ' + ((@($keysDiffer[$name]) + @($keysDiffer[$reference])) -join ', ')) }
                if (-not $primary) { $verdict = 'descriptive (no verdict)' }
                elseif ($differ.Count -gt 0) { $verdict = 'not comparable: ' + ((@($differ) | Select-Object -First 3) -join '; ') }
                elseif ($gate) { $verdict = 'not comparable: ' + $gate }
                elseif (($ra.Count -lt $networkMinimumRuns) -or ($rb.Count -lt $networkMinimumRuns)) { $verdict = 'indication only: needs more runs (' + $ra.Count + ' and ' + $rb.Count + ' runs; at least ' + $networkMinimumRuns + ' per arm)' }
                elseif ([double]::IsNaN($d[1])) { $verdict = 'indication only: no interval' }
                elseif ($d[2] -lt 0) { $verdict = 'better than ' + $reference + ' by ' + (Format-Number (-$d[0]) 2) }
                elseif ($d[1] -gt 0) { $verdict = 'WORSE than ' + $reference + ' by ' + (Format-Number $d[0] 2); if ($m[2]) { $worse += ($name + ': ' + $m[1] + ': ' + $verdict) } }
                else { $verdict = Get-NoDifferenceText $m[0] $ra $rb $d[1] $d[2] }
                if ($label) { $verdict = $label + '; ' + $verdict }
                if ($primary) { $text += '; difference of run means ' + (Format-Number $d[0] 2) + ' [' + (Format-Number $d[1] 2) + ', ' + (Format-Number $d[2] 2) + '] (' + $ra.Count + ' and ' + $rb.Count + ' runs): ' + $verdict }
                else { $text += '; difference of run means ' + (Format-Number $d[0] 2) + ': ' + $verdict }
                $armJson.verdicts += [pscustomobject]@{ metric = $m[0]; primary = $primary; runs = @($ra.Count, $rb.Count); difference = $d[0]; low = $d[1]; high = $d[2]; verdict = $verdict }
            }
            $lines.Add($text)
        }
        $json.arms[$name] = $armJson
    }
    if ($worse.Count -gt 0) { $Out.Add('  LATENCY WORSE (read this first): ' + ($worse -join '; ')) }
    if (@($gates | Where-Object { $_ -match 'hardly moved' }).Count -gt 0) { $Out.Add('  ' + $motionAdvice + ' (The numbers below are still shown, without verdicts.)') }
    foreach ($line in $lines) { $Out.Add($line) }
    $Out.Add('  Verdict: the unit is the RUN (the mean of its 5 s windows; a run needs at least ' + $networkMinimumWindows + ' windows). Verdict words only for the primary metrics (stability = stutter events per minute, cost = in-app latency P95): difference of the run means against ' + $reference + ' with a Welch 97.5 % interval (Bonferroni over 2 primary metrics); "better" or "WORSE" only when the interval excludes zero, the settings are comparable, both arms saw similar motion and each arm has at least ' + $networkMinimumRuns + ' runs; with fewer: "indication only: needs more runs". Every other metric is descriptive.')
    $Out.Add('  What the audio arms should show (expectations from the audio work, not results): base arm: clicks= rising when pops are heard, with gapMax= at or above 25 ms and paddingMin=0; base + SMOOTH: clicks= flat; continuity arm (n-audio): fewer underrunStarts=, playoutTargetMs= back to its starting value within a minute after trouble ends, and the delayMs= of quiet windows not growing. Audio delay is a cost: the model predicts +13 to +16 ms on Wi-Fi while trouble lasts.')
    $Out.Add('  Motion: the median video rate against the console target rate (at least ' + (Format-Number ($motionTargetShare * 100) 0) + ' %) or, without one, the requested bitrate (at least ' + (Format-Number ($motionRequestedShare * 100) 0) + ' %); the two arms at most ' + (Format-Number ($motionTolerance * 100) 0) + ' points apart.')
    $builds = @($names | Where-Object { $exe[$_] -ne 'experimental' })
    if ($builds.Count -gt 0) {
        $Out.Add('  Against the original and stable builds (only what every build logs: seconds under 57 presents, replaced frames, present to scanout):')
        foreach ($name in @($builds) + @($reference)) {
            $under = 0; $seconds = 0; $p50 = New-Object System.Collections.Generic.List[double]; $p95 = New-Object System.Collections.Generic.List[double]
            foreach ($run in @($runs | Where-Object { $_.record.arm -eq $name })) {
                $file = Join-Path (Join-Path $Folder 'bench') ($run.name + '.txt')
                $parsed = Read-PrecisionLines @(Get-Content -LiteralPath $file)
                for ($i = 3; $i -lt ($parsed.fps.Count - 2); $i++) { $seconds++; if ($parsed.fps[$i] -lt 57) { $under++ } }
                foreach ($line in @(Get-Content -LiteralPath $file)) {
                    if ($line -match '\] \[display-latency\] ') {
                        $v = Get-LogNumber $line 'presentToScanoutP50Ms'; if ($null -ne $v) { $p50.Add($v) }
                        $v = Get-LogNumber $line 'presentToScanoutP95Ms'; if ($null -ne $v) { $p95.Add($v) }
                    }
                }
            }
            $percent = [double]::NaN; if ($seconds -gt 0) { $percent = 100.0 * $under / $seconds }
            $json.builds[$name] = [ordered]@{ seconds = $seconds; under57Percent = $percent; presentToScanoutP50 = (Get-MedianOrNaN $p50.ToArray()); presentToScanoutP95 = (Get-MedianOrNaN $p95.ToArray()) }
            $Out.Add('    ' + $name + ': ' + $seconds + ' s, under 57: ' + (Format-Number $percent 1) + ' %, present to scanout P50 ' + (Format-Number $json.builds[$name].presentToScanoutP50 2) + ' ms, P95 ' + (Format-Number $json.builds[$name].presentToScanoutP95 2) + ' ms')
        }
        $Out.Add('    Not compared: in-app latency split, stutter events, frame gaps and audio counts per minute, round trip, console target rate and SmartLink state (those builds write none of these lines).')
    }
    $Out.Add('')
    return $json
}

# RESULT.txt (one page to read) and result.json (the same as values) in the evidence folder.
# --- kit version 5: MEASURED VALUES (measures.csv) --------------------------------------------------------------------
# One table (measures.csv) read here and by tools/remoteplay_latency_bench/report.py (measured_tables), so the kit and the
# report cannot list different things; result.json 'measured' is compared with the report cell by cell (DISAGREE).
# Software counters only. Change both together.
$measurePurposes = @('latency', 'measure', 'network', 'quality', 'probe')
$measurePhaseSeconds = 3.0     # 'start' = within 3 s of the run's first line of the component (the slow regime of 20 Nov lasted 1 to 3 s)
$measureStats = @('median', 'p01', 'max', 'last', 'word', 'rate-min', 'rate-s', 'slope-min', 'first-s', 'last-word')
$measureSampleStats = @('median', 'p01', 'max')
$measureSessions = @('input', 'visual', 'neural', 'framegen', 'audiofix', 'pacing', 'builds', 'smoothness', 'capture')
# The measured sessions (kit 5): one automatic start of 'plan' per arm and round; 'also' = arms of another set that join
# (the pair every arm of the session is compared with); 'intro' is printed once; 'cues' are the guided steps, in seconds
# after the first presented frame. The arms themselves are the lines of arms.csv whose set is the session's name.
$measureSessionTable = @{
    'input' = @{ plan = 'settle:8;wait:100;exit'; rounds = 2; also = @()
        intro = @('You need: the mouse and keyboard of this PC, and the DualSense controller connected to this PC (USB or Bluetooth).',
                  'Stand in the game where turning the camera is safe. Each run takes about 2 minutes; the kit prints every step. Do the SAME motions in every run.')
        cues = @(@{ at = 10; text = 'MOUSE: move the mouse in steady circles for 20 seconds (click into the picture first if the mouse is not captured).' },
                 @{ at = 30; text = 'KEYBOARD: tap W, A, S, D and Space in turn, about two taps a second, for 15 seconds.' },
                 @{ at = 45; text = 'Hands off mouse and keyboard. DUALSENSE: move the right stick in steady circles for 20 seconds.' },
                 @{ at = 65; text = 'DUALSENSE: tap Cross and the D-pad in turn, about two taps a second, for 15 seconds.' },
                 @{ at = 80; text = 'DUALSENSE: hold L2 and R2 fully, release ONE of them, press it again; repeat for 10 seconds.' },
                 @{ at = 92; text = 'Done: hands off. PushFPS closes by itself in a few seconds.' }) }
    'visual' = @{ plan = 'settle:8;probe:20;wait:40;exit'; rounds = 2; also = @(); cues = @()
        intro = @('Stand in the game in ONE place with a picture that shows detail (vegetation, text of the HUD, something dark) and do not move during the session:',
                  'the picture numbers of two arms can only be compared when both saw the same scene. Do not touch the controller.',
                  'Every arm runs on the SAME stream: the codec saved with your PS5 pairing (the kit cannot change it). The reference arm (v-reference) needs the H.265 HDR stream:',
                  'for it, connect once with codec H.265 HDR in the PS5 connect dialog BEFORE this session (then every arm is HDR). With an SDR stream RESULT.txt says "reference could not be reproduced: stream was SDR".') }
    'neural' = @{ plan = 'settle:8;wait:45;exit'; rounds = 2; also = @(); cues = @()
        intro = @('Stand in the game in ONE place and do not touch the controller: the same scene in every run.') }
    'framegen' = @{ plan = 'settle:8;wait:45;exit'; rounds = 2; also = @(); cues = @()
        intro = @('Let the game show steady motion (for example drive or walk in a straight line) in every run: frame generation has nothing to do in a still picture.') }
    'audiofix' = @{ plan = 'settle:8;wait:120;exit'; rounds = 2; also = @('s-base'); cues = @()
        intro = @('Leave the game running with sound (music or traffic). Each run takes about 2.5 minutes. Listen: clicks, drop-outs or an echo are worth a note.') }
    'pacing' = @{ plan = 'settle:8;wait:60;exit'; rounds = 2; also = @('p-window'); cues = @()
        intro = @('Let the game show steady motion in every run. Do not touch the controller unless the game needs it.') }
    'smoothness' = @{ plan = 'settle:8;wait:60;exit'; rounds = 2; also = @(); cues = @()
        intro = @('Keep the SAME steady motion in every run: drive or turn the camera slowly at a constant speed for the whole minute (each run is about 1.5 minutes).',
                  'The kit never changes Windows settings. For a FIXED 120 Hz case set Windows to 120 Hz yourself (Settings > System > Display > Advanced display), run this session again, then set it back: RESULT.txt shows the refresh in effect per run.',
                  'Watch each run: whether it LOOKS smooth only you can tell; note which arm looked uneven.') }
    'capture' = @{ plan = 'settle:8;wait:60;exit'; rounds = 2; also = @(); cues = @()
        intro = @('A capture card instead of Remote Play: the PS5 picture over HDMI into the card, the DualSense plugged into (or paired with) the PS5, PS5 HDCP off (Settings > System > HDMI).',
                  'PushFPS never connects to the PS5 in this session. Run it once with a 30 FPS game (GTA-class) and once with a 60 FPS game: new pictures/s shows which one ran.',
                  'Keep the SAME steady motion in every run (drive or turn the camera slowly). For the 120 and 180 targets add -Pass 120 or -Pass 180 (Windows at that rate).',
                  'Watch each run: ghosting, broken HUD text and uneven motion are worth a note. The numbers are the app''s own counters, not input-to-photon (docs/drafts/CAPTURE_FG.md section 9: the camera procedure).') }
    'builds' = @{ plan = 'settle:8;wait:60;exit'; rounds = 2; also = @(); cues = @()
        intro = @('Four copies with the same test profile, one minute each, twice. The original and the stable copy have no automation:',
                  'connect them by hand if they do not connect by themselves; the kit closes every copy itself.') }
}
$script:measurePlan = ''          # the plan of the measured session that runs (Get-AutorunPlan)
$script:measureCues = @()         # its guided steps (Invoke-ArmRun)
$sessionFirstLine = @{
    'input' = 'These are times inside the PC (raw input to the pad report handed to the network), not input-to-photon; how fast the camera turns on screen needs a camera.'
    'visual' = 'Every picture path against v-original, the original stream untouched (each arm''s ''against'' line, v-dlss-k against v-dlss, and a verdict against v-original only by the rule below): software numbers, processing times of the app''s own counters and no-reference picture metrics of the probe; not a judgement of how it looks on your screen and not input-to-photon.'
    'neural' = 'GPU cost of the neural layer at each internal size, with and without the upscaler to the display: the app''s own counters, not input-to-photon.'
    'framegen' = 'Submitted FPS counts generated frames too and is NOT motion: ''real frames'' are the stream''s frames (a 30 FPS game repeats each picture inside the 60 FPS stream; no line here detects that). The real / generated split comes from the app''s own counters ([frame-flow], [slipstream-fg]), not from an outside tool: PresentMon marks generated frames only in an application instrumented for it.'
    'audiofix' = 'Audio delay is a cost: a lower pop count bought with a longer delay is not a gain. Software estimates of the audio owner, not a measurement at the speaker.'
    'pacing' = 'One presentation switch per arm against p-window (windowed, the interface visible): software counters of presentation and frame pacing, not input-to-photon.'
    'smoothness' = 'The result is how EVENLY frames were PRESENTED ([slipstream-cadence]: the spacing of the Present calls, and with VRR off the fixed-refresh model of what the panel holds), not the submitted FPS the counter shows. What your eye sees only you can judge: watch every run.'
    'capture' = 'Capture card: the game''s own pictures (new pictures/s) against what was shown; generated frames between two identical pictures are copies and never count as motion; latency is a real frame''s capture callback to its Present return (software, not photons). With these counters on, the card''s callback compares every picture before handing it over, in every arm (about 0.3 ms at 1440p, more at 4K; estimate), so the passthrough arm''s latency reads slightly high and the added delay of frame generation slightly low. Generated frames are never native PS5 frames.'
    'builds' = 'Regression proof: every build with the SAME test profile. A build that does not log a value shows n/a and is ''cannot tell'' for it.'
}
$spreadWords = @('differs', 'same within run-to-run spread', 'cannot tell')
$spreadRule = 'Comparison rule (per value, each arm against its pair; a run = one start): ''same within run-to-run spread'' when the ranges of the per-run values of the two arms overlap; ''differs'' when they do not overlap AND the gap between the two ranges is more than twice the larger range; ''cannot tell'' otherwise, and whenever an arm has fewer than 2 runs with that value. With 2 runs per arm only large differences can show, and identical arms would still be called ''differs'' for about 1 value in 25 by chance (a model of independent normal noise, not a measurement); ''same'' is not proof of equality. No better / worse word comes from this rule.'
$visualBase = 'v-original'
$visualReference = 'v-reference'
$visualDefects = @('blockiness', 'banding', 'ringing', 'flicker')   # hudDelta: never against v-original (about 0 there)
$visualCleaned = @('blockiness', 'banding', 'ringing')
$visualRule = 'Verdict rule (software picture metrics of the shown picture, against v-original; no word of beauty): ''better'' only when blockiness, banding or ringing is at least 10 % lower, no defect metric (blockiness, banding, ringing, flicker) is 10 % or more higher, detail stays at 95 % or more, AND the graph processing P95 is at most 8.33 ms (half a 60 FPS frame); ''worse'' when a defect metric is 10 % or more higher or detail falls below 90 % (processing time measured); ''cannot tell'' otherwise; ''no verdict'' when an arm has fewer than 2 probes or no processing time. HUD delta is not compared with v-original (it measures the output against the raw picture, so it is about 0 there), and blockiness counts only when the arm''s output size is v-original''s (a 1080 to 1440 scaler blurs the 8-pixel grid it looks for).'
$visualSizeNote = '; blockiness not comparable: different output size'
$processingBudgetMs = 1000.0 / 120.0
$neuralKeys = @('nr', 'nrRuntime', 'nrSize', 'nrLook')
$neuralWithout = '; the session went on, and the values of this arm are WITHOUT the neural layer'
# HDR or SDR: in the sessions that compare pictures every run says which it was, and an HDR arm is never compared with an SDR one.
$pictureSessions = @('visual', 'neural')
$pictureRule = 'HDR or SDR per run: the stream''s own picture, streamHdr= of the run''s last [slipstream-pipeline] line (the codec chosen before connecting; the pipeline key hdr= only switches the RTX Video HDR conversion), or hdr= of [slipstream-config] in a run without that line. An arm is compared with its pair (and gets a verdict) only when every run of both was the same of the two; the neural layer is undocumented on HDR video, so HDR and SDR numbers are never mixed.'
# The owner's reference (v-reference) is an HDR stream; its codec is chosen before connecting (saved with the PS5 pairing; no
# pipeline key can change it). A run whose stream was SDR did not reproduce it: said, never silent (report.py reference_lines).
# A feature the build does not have (rolling releases 8A / 8B / 8C; report.py feature_lines): said per arm, never compared or judged.
$pipelineMissing = 'pipeline keys not applied: this build has no VEYRA_TEST_RP_PIPELINE (no [slipstream-pipeline] line in any run), so this arm ran the build''s own settings; it is neither compared nor judged'
$overrideMissing = 'test override not applied: a run''s [remoteplay] session started request is not the arm''s VEYRA_TEST_RP_BITRATE_KBPS / VEYRA_TEST_RP_CODEC (a build without the clarity switches, or a refused value), so this arm ran the saved stream settings; it is neither compared nor judged'
# Stream clarity (report.py override_missing): true when a run of the arm logged a session request that is not the bitrate
# or codec its switches ask for. The app logs the codec as H264 / H265 / H265_HDR (the dry run as h264 / hevc / hevc-hdr).
function Test-OverrideMissing([string]$SwitchText, $Runs) {
    $wantKbps = ''; $wantCodec = ''
    foreach ($pair in ($SwitchText -split ';')) {
        if ($pair -match '^VEYRA_TEST_RP_BITRATE_KBPS=(\d+)$') { $wantKbps = [string]$Matches[1] }
        elseif ($pair -match '^VEYRA_TEST_RP_CODEC=(\S+)$') { $wantCodec = [string]$Matches[1] }
    }
    if ((-not $wantKbps) -and (-not $wantCodec)) { return $false }
    $codecWords = @{ 'h264' = @('h264', 'H264'); 'hevc' = @('hevc', 'H265'); 'hevc-hdr' = @('hevc-hdr', 'H265_HDR') }
    if ($null -eq $Runs) { return $false }
    foreach ($run in $Runs) {
        if (-not $run.byComponent.ContainsKey('remoteplay')) { continue }
        foreach ($item in $run.byComponent['remoteplay']) {
            if ([string]$item.text -notmatch '^session started .*codec=(\S+) requestedBitrateKbps=(\d+)') { continue }
            $codec = [string]$Matches[1]; $kbps = [string]$Matches[2]
            if ($wantKbps -and ($kbps -ne $wantKbps)) { return $true }
            if ($wantCodec -and $codecWords.ContainsKey($wantCodec) -and ($codecWords[$wantCodec] -cnotcontains $codec)) { return $true }
        }
    }
    return $false
}
$inputMissing = 'input: not in this build (no [slipstream-input] line in any run: the build has no input core, or the keyboard-and-mouse object did not start); the session went on'
$referenceSdr = 'reference could not be reproduced: stream was SDR'
# The smoothness session (report.py cadence_lines / CADENCE_RULE): a word about how evenly frames were PRESENTED.
$cadenceEvenPct = 2.0
$cadenceUnevenPct = 10.0
$cadenceRule = 'Cadence rule (per arm, the 5 s [slipstream-cadence] windows of all its runs; VRR on or off from the app''s own [display-vrr] active= lines of those runs, beside what the arm asked for): with VRR active throughout the share of uneven presents is offPct; with VRR off throughout it is the larger of offPct and fixedUnevenPct (the fixed-refresh model); ''even'' when the median share is 2 % or less, ''uneven'' when it is 10 % or more, ''cannot tell'' otherwise, and whenever VRR changed during the runs, no [display-vrr] line said it, or no window was logged. A word about presenting only: not about what the panel or your eye showed.'
$referenceHow = ' (the reference needs the H.265 HDR stream, chosen in the PS5 connect dialog before the session; the kit cannot change the codec saved with the pairing); its values are those of an SDR run'
$buttonSpreadMs = 50.0
$measuredFoot = 'screen = DXGI frame statistics (Present call to the vblank that showed it), software; ''first 3 s'' and ''steady'' are apart because the first 1 to 3 seconds ran slower in every run of 20 Nov. audio = the audio owner''s estimates beside the video clock lag ([remoteplay-clock] arrivalMinusPtsMs).'

# The text after Name= up to the next blank (report.py log_word): keeps commas, so detail=a:ok,b:refused:why stays whole.
function Get-LogWord([string]$Line, [string]$Name) {
    if ($Line -cmatch ('(?<![A-Za-z])' + $Name + '=(\S+)')) { return [string]$Matches[1] }
    return $null
}

function Read-MeasureSpec([string]$Path) {
    $rows = New-Object System.Collections.Generic.List[object]
    if (-not (Test-Path -LiteralPath $Path)) { return $rows }
    $seen = @{}
    foreach ($raw in @(Get-Content -LiteralPath $Path | Where-Object { $_ -and ($_ -notmatch '^\s*#') } | ConvertFrom-Csv)) {
        $stat = ([string]$raw.stat).Trim()
        $shareKind = ''; $shareLimit = 0.0; $firstLimit = $null
        if ($stat -cmatch '^share-(ge|lt):(-?[0-9]+(\.[0-9]+)?)$') { $shareKind = [string]$Matches[1]; $shareLimit = [double]::Parse($Matches[2], $invariant) }
        elseif ($stat -cmatch '^first-lt:(-?[0-9]+(\.[0-9]+)?)$') { $firstLimit = [double]::Parse($Matches[1], $invariant) }
        elseif ($measureStats -notcontains $stat) { throw ("measures.csv: unknown stat '" + $stat + "'") }
        $component = ([string]$raw.component).Trim()
        if (($component -eq 'samples') -and ($measureSampleStats -notcontains $stat)) { throw ("measures.csv: samples rows take median, p01 or max, not '" + $stat + "'") }
        $phase = ([string]$raw.phase).Trim()
        if (@('all', 'start', 'steady') -notcontains $phase) { throw ("measures.csv: unknown phase '" + $phase + "'") }
        $group = ([string]$raw.group).Trim(); $filter = ([string]$raw.filter).Trim(); $field = ([string]$raw.field).Trim()
        $key = $group + '|' + $component + '|' + $filter + '|' + $field + '|' + $stat
        if ($seen.ContainsKey($key)) { throw ('measures.csv: two rows are the same value: ' + $key) }
        $seen[$key] = $true
        $digits = 0; $skipFirst = 0; $skipLast = 0
        if (([string]$raw.digits).Trim()) { $digits = [int]([string]$raw.digits).Trim() }
        if (([string]$raw.skipFirst).Trim()) { $skipFirst = [int]([string]$raw.skipFirst).Trim() }
        if (([string]$raw.skipLast).Trim()) { $skipLast = [int]([string]$raw.skipLast).Trim() }
        $rows.Add([pscustomobject]@{ group = $group; sessions = ([string]$raw.sessions).Trim(); label = ([string]$raw.label).Trim(); component = $component; filter = $filter; phase = $phase
            field = $field; stat = $stat; digits = $digits; skipFirst = $skipFirst; skipLast = $skipLast; unit = ([string]$raw.unit).Trim(); compare = (([string]$raw.compare).Trim() -eq '1')
            key = $key; shareKind = $shareKind; shareLimit = $shareLimit; firstLimit = $firstLimit })
    }
    return $rows
}

# The stamped lines of one run's bench log by component, and what the statistics need beside them.
function Read-MeasureRun([string]$Folder, $Run) {
    $byComponent = @{}
    $firstLine = $null; $firstFrame = $null
    $file = Join-Path (Join-Path $Folder 'bench') ($Run.name + '.txt')
    if (Test-Path -LiteralPath $file) {
        foreach ($line in @(Get-Content -LiteralPath $file)) {
            if (-not $line) { continue }
            if ($line -notmatch '^\W?(\S+Z) t=\d+ \[\w+\s*\] \[([^\]]+)\] (.*)$') { continue }
            $stampText = [string]$Matches[1]; $component = [string]$Matches[2]; $text = [string]$Matches[3]
            $t = $null
            try { $t = Get-StampSecondsOf $stampText } catch { $t = $null }
            if ($null -eq $t) { continue }
            if ($null -eq $firstLine) { $firstLine = $t }
            if (($component -eq 'frame-rate') -and ($null -eq $firstFrame)) { $firstFrame = $t }
            if (-not $byComponent.ContainsKey($component)) { $byComponent[$component] = New-Object System.Collections.Generic.List[object] }
            $byComponent[$component].Add([pscustomobject]@{ t = [double]$t; text = $text })
        }
    }
    return [pscustomobject]@{ name = [string]$Run.name; arm = [string]$Run.record.arm; purpose = [string]$Run.record.purpose; byComponent = $byComponent
        firstLine = $firstLine; firstFrame = $firstFrame; samples = (Join-Path $Run.folder 'samples.csv') }
}

function Test-MeasureFilter([string]$Text, [string]$Filter) {
    if (-not $Filter) { return $true }
    foreach ($pair in ($Filter -split '&')) {
        $at = $pair.IndexOf('=')
        if ($at -lt 1) { return $false }
        $wanted = $pair.Substring(0, $at)
        $have = Get-LogWord $Text $wanted
        if (($null -eq $have) -or ($have -cne $pair.Substring($at + 1))) { return $false }
    }
    return $true
}

function Get-MostCommonWord($Values) {
    $counts = @{}; $order = @()
    foreach ($v in $Values) { $k = [string]$v; if (-not $counts.ContainsKey($k)) { $counts[$k] = 0; $order += $k }; $counts[$k]++ }
    $best = $order[0]
    foreach ($k in $order) { if ($counts[$k] -gt $counts[$best]) { $best = $k } }
    return $best
}

function Get-SortedDoubles($List) {
    $plain = New-Object System.Collections.Generic.List[double]
    foreach ($v in $List) { $plain.Add([double]$v) }
    $plain.Sort()
    return ,$plain.ToArray()
}

# One value of measures.csv over the runs of one arm (report.py measure_cell): value ($null = none or a word), n, text.
# Time spans are rounded to the millisecond of the log's stamps (report.py span_seconds), so a rate over a short span is the
# same number in both readers.
# The points (times, values) of one run for one row of measures.csv (report.py measure_cell, per run). Written without
# break / continue and with one loop per function: the round of 21 Nov 00:00 failed in Windows PowerShell 5.1 with
# "Argument types do not match" at the per-run loop that held all of this in one body (the interpreter's loop compiler).
function Get-MeasureRunPoints($Row, $Run, [string]$Stat) {
    $times = New-Object System.Collections.Generic.List[double]
    $values = New-Object System.Collections.Generic.List[object]
    if ($Row.component -eq 'samples') {
        foreach ($v in @(Read-SampleColumn @([string]$Run.samples) ([string]$Row.field))) { if ($v -ne -1) { $times.Add(0.0); $values.Add([double]$v) } }
    } elseif ($Run.byComponent.ContainsKey([string]$Row.component)) {
        Add-MeasureLinePoints $Row $Stat $Run.byComponent[[string]$Row.component] $times $values
    }
    if (($Stat -eq 'first-s') -or ($null -ne $Row.firstLimit)) {
        # seconds from the first presented frame to the first matching line (first-lt:X: the first whose value is under X)
        $firstAt = Get-FirstMeasureTime $Row $times $values
        $origin = $Run.firstFrame
        if ($null -eq $origin) { $origin = $Run.firstLine }
        $oneTimes = New-Object System.Collections.Generic.List[double]
        $oneValues = New-Object System.Collections.Generic.List[object]
        if (($Row.component -ne 'samples') -and ($null -ne $firstAt) -and ($null -ne $origin)) { $oneTimes.Add([double]$firstAt); $oneValues.Add([math]::Round(([double]$firstAt - [double]$origin), 3)) }
        if ($Row.component -ne 'samples') { return [pscustomobject]@{ times = $oneTimes; values = $oneValues } }
    }
    $keptTimes = New-Object System.Collections.Generic.List[double]
    $keptValues = New-Object System.Collections.Generic.List[object]
    $from = [int]$Row.skipFirst
    $to = $times.Count - [int]$Row.skipLast
    for ($i = $from; $i -lt $to; $i++) { $keptTimes.Add($times[$i]); $keptValues.Add($values[$i]) }
    return [pscustomobject]@{ times = $keptTimes; values = $keptValues }
}

# The matching lines of one component in one run, in time order, added to $Times / $Values (no break, no continue).
function Add-MeasureLinePoints($Row, [string]$Stat, $Lines, $Times, $Values) {
    if (($null -eq $Lines) -or ($Lines.Count -eq 0)) { return }
    $tFirst = $null
    $tFirst = [double]$Lines[0].t
    $isWord = (($Stat -eq 'word') -or ($Stat -eq 'last-word'))
    $field = [string]$Row.field
    $filter = [string]$Row.filter
    $phase = [string]$Row.phase
    foreach ($item in $Lines) {
        $take = Test-MeasureFilter ([string]$item.text) $filter
        if ($take -and ($phase -ne 'all')) {
            $since = [math]::Round(([double]$item.t - [double]$tFirst), 3)
            if (($phase -eq 'start') -and ($since -gt $measurePhaseSeconds)) { $take = $false }
            if (($phase -eq 'steady') -and ($since -le $measurePhaseSeconds)) { $take = $false }
        }
        if ($take) {
            if ($Stat -eq 'first-s') { $Times.Add([double]$item.t); $Values.Add([double]0) }
            elseif ($isWord) {
                $w = Get-LogWord ([string]$item.text) $field
                if ($null -ne $w) { $Times.Add([double]$item.t); $Values.Add([string]$w) }
            } else {
                $number = Get-LogNumber ([string]$item.text) $field
                if (($null -ne $number) -and ($number -ne -1)) { $Times.Add([double]$item.t); $Values.Add([double]$number) }
            }
        }
    }
}

# The time of the first point (first-lt:X: the first whose value is under X); $null when none (no break).
function Get-FirstMeasureTime($Row, $Times, $Values) {
    $firstAt = $null
    for ($i = 0; $i -lt $Times.Count; $i++) {
        if (($null -eq $firstAt) -and (($null -eq $Row.firstLimit) -or ([double]$Values[$i] -lt [double]$Row.firstLimit))) { $firstAt = $Times[$i] }
    }
    return $firstAt
}

function Get-MeasureCell($Row, $Runs) {
    $stat = [string]$Row.stat
    $perRun = New-Object System.Collections.Generic.List[object]
    $runCount = 0
    # never @($Runs): Windows PowerShell 5.1's array binder fails on a generic List ('Argument types do not match' in
    # PSToObjectArrayBinder / MaybeDebase, the kit dry run of 21 Nov); foreach enumerates a List and a single run alike
    foreach ($run in $Runs) {
        $runCount = $runCount + 1
        $perRun.Add((Get-MeasureRunPoints $Row $run $stat))
    }
    $value = $null; $n = 0; $text = 'n/a'
    $unit = ''
    if ($Row.unit) { $unit = ' ' + [string]$Row.unit }
    $pooled = New-Object System.Collections.Generic.List[object]
    foreach ($one in $perRun) { foreach ($v in $one.values) { $pooled.Add($v) } }
    if ($stat -eq 'word') {
        if ($pooled.Count -gt 0) { $n = $pooled.Count; $text = Get-MostCommonWord $pooled }
    } elseif ($stat -eq 'last-word') {
        # the text of each run's last line; the most common over the runs
        $lastWords = New-Object System.Collections.Generic.List[object]
        foreach ($one in $perRun) { if ($one.values.Count -gt 0) { $lastWords.Add($one.values[$one.values.Count - 1]) } }
        if ($lastWords.Count -gt 0) { $n = $lastWords.Count; $text = Get-MostCommonWord $lastWords }
    } elseif ((@('median', 'p01', 'max') -contains $stat) -or $Row.shareKind) {
        if ($pooled.Count -gt 0) {
            $sorted = Get-SortedDoubles $pooled
            if ($stat -eq 'median') { $value = Get-Quantile $sorted 0.5 }
            elseif ($stat -eq 'p01') { $value = Get-Quantile $sorted 0.01 }
            elseif ($stat -eq 'max') { $value = $sorted[$sorted.Count - 1] }
            else {
                $hits = 0
                foreach ($v in $sorted) { if ((($Row.shareKind -eq 'ge') -and ($v -ge $Row.shareLimit)) -or (($Row.shareKind -eq 'lt') -and ($v -lt $Row.shareLimit))) { $hits++ } }
                $value = 100.0 * $hits / $sorted.Count
            }
            $n = $sorted.Count
        }
    } elseif ($stat -eq 'last') {
        $lasts = New-Object System.Collections.Generic.List[object]
        foreach ($one in $perRun) { if ($one.values.Count -gt 0) { $lasts.Add($one.values[$one.values.Count - 1]) } }
        if ($lasts.Count -gt 0) { $value = Get-Quantile (Get-SortedDoubles $lasts) 0.5; $n = $lasts.Count }
    } elseif (($stat -eq 'rate-min') -or ($stat -eq 'rate-s')) {
        $rise = 0.0; $seconds = 0.0; $used = 0; $a = $null; $b = $null
        foreach ($one in $perRun) {
            $c = $one.values.Count
            if (($c -ge 2) -and ($one.times[$c - 1] -gt $one.times[0])) {
                for ($i = 1; $i -lt $c; $i++) {
                    $a = [double]$one.values[$i - 1]; $b = [double]$one.values[$i]
                    if ($b -ge $a) { $rise += ($b - $a) } else { $rise += $b }
                }
                $seconds += [math]::Round(($one.times[$c - 1] - $one.times[0]), 3); $used++
            }
        }
        if ($seconds -gt 0) {
            $value = $rise / $seconds
            if ($stat -eq 'rate-min') { $value = $value * 60.0 }
            $n = $used
        }
    } elseif ($stat -eq 'slope-min') {
        $slopes = New-Object System.Collections.Generic.List[object]
        foreach ($one in $perRun) {
            $c = $one.values.Count
            if (($c -ge 2) -and ($one.times[$c - 1] -gt $one.times[0])) { $slopes.Add(([double]$one.values[$c - 1] - [double]$one.values[0]) / ([math]::Round(($one.times[$c - 1] - $one.times[0]), 3) / 60.0)) }
        }
        if ($slopes.Count -gt 0) { $value = Get-Quantile (Get-SortedDoubles $slopes) 0.5; $n = $slopes.Count }
    } elseif (($stat -eq 'first-s') -or ($null -ne $Row.firstLimit)) {
        $firsts = New-Object System.Collections.Generic.List[object]
        foreach ($one in $perRun) { if ($one.values.Count -gt 0) { $firsts.Add($one.values[0]) } }
        if ($firsts.Count -gt 0) { $value = Get-Quantile (Get-SortedDoubles $firsts) 0.5; $n = $firsts.Count }
        else { $text = 'never (0 of ' + $runCount + ' runs)' }
    }
    if ($null -ne $value) {
        $text = (Format-Number $value ([int]$Row.digits)) + $unit
        if (($stat -eq 'first-s') -or ($null -ne $Row.firstLimit)) { $text += ' (in ' + $n + ' of ' + $runCount + ' runs)' }
    }
    return [pscustomobject]@{ value = $value; n = $n; text = $text }
}

# report.py spread_verdict.
function Get-SpreadVerdict($Mine, $Base) {
    $a = New-Object System.Collections.Generic.List[object]; $b = New-Object System.Collections.Generic.List[object]
    foreach ($v in $Mine) { if ($null -ne $v) { $a.Add($v) } }
    foreach ($v in $Base) { if ($null -ne $v) { $b.Add($v) } }
    if (($a.Count -lt 2) -or ($b.Count -lt 2)) { return 'cannot tell' }
    $mine = Get-SortedDoubles $a; $base = Get-SortedDoubles $b
    $mineLow = $mine[0]; $mineHigh = $mine[$mine.Count - 1]; $baseLow = $base[0]; $baseHigh = $base[$base.Count - 1]
    if (($mineLow -le $baseHigh) -and ($baseLow -le $mineHigh)) { return 'same within run-to-run spread' }
    $spread = [math]::Max($mineHigh - $mineLow, $baseHigh - $baseLow)
    $gap = [math]::Max($mineLow - $baseHigh, $baseLow - $mineHigh)
    if ($gap -gt (2.0 * $spread)) { return 'differs' }
    return 'cannot tell'
}

# report.py stream_hdr_words: streamHdr= of the run's last [slipstream-pipeline] line that says 0 or 1; $null without one.
function Get-StreamHdrWord($Run) {
    $word = $null
    if (-not $Run.byComponent.ContainsKey('slipstream-pipeline')) { return $null }
    foreach ($item in $Run.byComponent['slipstream-pipeline']) {
        $v = Get-LogWord ([string]$item.text) 'streamHdr'
        if (($v -eq '0') -or ($v -eq '1')) { $word = $v }
    }
    return $word
}

# report.py picture_of: 'HDR' | 'SDR' when every run was that, 'not logged' when none said, else 'mixed'.
function Get-PictureOf($States) {
    $kinds = @()
    foreach ($state in $States) { if ($kinds -notcontains [string]$state) { $kinds += [string]$state } }
    if ($kinds.Count -eq 1) { return $kinds[0] }
    if ($kinds.Count -eq 0) { return 'not logged' }
    return 'mixed'
}

# report.py picture_blocks: why an arm and its pair are not compared in a picture session ('' = they are).
function Get-PictureBlock([string]$SessionOfFolder, [string]$Mine, [string]$Theirs, [string]$BaseName) {
    if ($pictureSessions -notcontains $SessionOfFolder) { return '' }
    if (($Mine -ceq $Theirs) -and (@('HDR', 'SDR') -contains $Mine)) { return '' }
    return ('this arm: ' + $Mine + ', ' + $BaseName + ': ' + $Theirs + '; HDR and SDR runs are never mixed in one comparison, and runs that do not say which they were are not compared')
}

function Get-ArmPipeline($Arm) {
    if ((-not $Arm) -or (-not $Arm.switches)) { return '' }
    foreach ($pair in ([string]$Arm.switches -split ';')) { if ($pair.StartsWith('pipeline:')) { return $pair.Substring(9).Replace('|', ';') } }
    return ''
}

# report.py visual_verdict: $Mine / $Base = metric -> @(median, n) of the output picture.
# $MineSize / $BaseSize: output size= words; blockiness counts only when both are known and equal (report.py visual_verdict).
function Get-VisualVerdict($Mine, $Base, $ProcessingP95, [string]$MineSize, [string]$BaseSize) {
    foreach ($k in ($visualDefects + @('detail'))) {
        if ((-not $Mine.ContainsKey($k)) -or (-not $Base.ContainsKey($k)) -or ($Mine[$k][1] -lt 2) -or ($Base[$k][1] -lt 2)) { return 'no verdict: fewer than 2 probes with picture numbers in this arm or in v-original' }
    }
    if ($null -eq $ProcessingP95) { return 'no verdict: no processing time ([slipstream-nr] graphMsP95) in this arm' }
    $same = (($MineSize -ceq $BaseSize) -and $MineSize -and ($MineSize -ne 'n/a'))
    $note = ''
    if (-not $same) { $note = $visualSizeNote + ' (' + $MineSize + ' / ' + $BaseSize + ')' }
    $lower = @(); $higher = @()
    foreach ($k in $visualCleaned) { if (($same -or ($k -ne 'blockiness')) -and ($Mine[$k][0] -le ($Base[$k][0] * 0.9)) -and ($Mine[$k][0] -lt $Base[$k][0])) { $lower += $k } }
    foreach ($k in $visualDefects) { if (($same -or ($k -ne 'blockiness')) -and ($Mine[$k][0] -ge ($Base[$k][0] * 1.1)) -and ($Mine[$k][0] -gt $Base[$k][0])) { $higher += $k } }
    $detail = 1.0
    if ($Base['detail'][0] -gt 0) { $detail = $Mine['detail'][0] / $Base['detail'][0] }
    if (($higher.Count -gt 0) -or ($detail -lt 0.90)) {
        $why = @()
        foreach ($k in $higher) { $why += ($k + ' higher') }
        if ($detail -lt 0.90) { $why += 'detail lower' }
        return ('worse on these software picture metrics (' + ($why -join ', ') + '; processing P95 ' + (Format-Number $ProcessingP95 2) + ' ms' + $note + ')')
    }
    if (($lower.Count -gt 0) -and ($detail -ge 0.95)) {
        $names = @()
        foreach ($k in $lower) { $names += ($k + ' lower') }
        if ($ProcessingP95 -le $processingBudgetMs) { return ('better on these software picture metrics (' + ($names -join ', ') + '; processing P95 ' + (Format-Number $ProcessingP95 2) + ' ms, within 8.33 ms' + $note + ')') }
        return ('cannot tell: cleaner on the picture metrics (' + ($names -join ', ') + ') but processing P95 ' + (Format-Number $ProcessingP95 2) + ' ms is over 8.33 ms' + $note)
    }
    return ('cannot tell: the picture metrics differ by less than 10 % or in both directions' + $note)
}

# The text of a failure in the kit's own table code, precise enough to fix it from the dry run's annotation alone: the
# message, its .NET type, the statement (PositionMessage), the kit's function stack and the first .NET frames.
function Get-KitErrorText($Record) {
    $parts = @([string]$Record.Exception.Message + ' [' + $Record.Exception.GetType().FullName + ']')
    $parts += ('script line ' + $Record.InvocationInfo.ScriptLineNumber)
    $position = [string]$Record.InvocationInfo.PositionMessage
    if ($position) { $parts += ('at: ' + ((@($position -split "`r?`n") | Where-Object { $_ } | ForEach-Object { $_.Trim() }) -join ' | ')) }
    $stack = [string]$Record.ScriptStackTrace
    if ($stack) { $parts += ('kit stack: ' + ((@($stack -split "`r?`n") | Where-Object { $_ } | Select-Object -First 6 | ForEach-Object { $_.Trim() }) -join ' < ')) }
    $net = [string]$Record.Exception.StackTrace
    if ($net) { $parts += ('.NET: ' + ((@($net -split "`r?`n") | Where-Object { $_ } | Select-Object -First 4 | ForEach-Object { $_.Trim() }) -join ' < ')) }
    if ($Record.Exception.InnerException) { $parts += ('inner: ' + [string]$Record.Exception.InnerException.Message) }
    return ($parts -join '; ')
}

# report.py measured_tables + measured_compared + visual_verdicts + neural_runtime_lines + button_spread_notes.
function Get-MeasuredTables([string]$Folder, $Kept, $ArmTable, [string]$SessionOfFolder) {
    $specPath = Join-Path $Folder 'measures.csv'
    if (-not (Test-Path -LiteralPath $specPath)) { $specPath = Join-Path $kitFolder 'measures.csv' }
    $shown = New-Object System.Collections.Generic.List[object]
    $groups = New-Object System.Collections.Generic.List[string]
    foreach ($row in (Read-MeasureSpec $specPath)) {
        if (($row.sessions -ne '*') -and (@($row.sessions -split '\|') -notcontains $SessionOfFolder)) { continue }
        $shown.Add($row)
        if (-not $groups.Contains([string]$row.group)) { $groups.Add([string]$row.group) }
    }
    $runsOf = @{}; $names = @()
    foreach ($run in $Kept) {
        if ($measurePurposes -notcontains [string]$run.record.purpose) { continue }
        $name = [string]$run.record.arm
        if (-not $runsOf.ContainsKey($name)) { $runsOf[$name] = New-Object System.Collections.Generic.List[object] }
        $runsOf[$name].Add((Read-MeasureRun $Folder $run))
    }
    $pairOf = @{}; $pipelineOf = @{}; $switchesOf = @{}
    foreach ($arm in $ArmTable) { if ($runsOf.ContainsKey([string]$arm.name)) { $names += [string]$arm.name; $pairOf[[string]$arm.name] = [string]$arm.compare; $pipelineOf[[string]$arm.name] = Get-ArmPipeline $arm; $switchesOf[[string]$arm.name] = [string]$arm.switches } }
    foreach ($name in @($runsOf.Keys | Sort-Object)) { if ($names -notcontains $name) { $names += $name } }
    $arms = [ordered]@{}
    foreach ($name in $names) {
        $runs = $runsOf[$name]
        $cells = [ordered]@{}; $perRun = @{}
        foreach ($row in $shown) {
            $cells[[string]$row.key] = Get-MeasureCell $row $runs
            if ($row.compare) {
                $each = New-Object System.Collections.Generic.List[object]
                foreach ($run in $runs) { $one = New-Object System.Collections.Generic.List[object]; $one.Add($run); $each.Add((Get-MeasureCell $row $one).value) }
                $perRun[[string]$row.key] = $each
            }
        }
        $picture = New-Object System.Collections.Generic.List[string]
        foreach ($run in $runs) {
            $state = 'not logged'
            $stream = Get-StreamHdrWord $run
            if ($stream -eq '1') { $picture.Add('HDR'); continue }
            if ($stream -eq '0') { $picture.Add('SDR'); continue }
            if ($run.byComponent.ContainsKey('slipstream-config')) {
                foreach ($item in $run.byComponent['slipstream-config']) {
                    $hdrWord = Get-LogWord ([string]$item.text) 'hdr'
                    if ($hdrWord -eq '1') { $state = 'HDR' } elseif ($hdrWord -eq '0') { $state = 'SDR' }
                }
            }
            $picture.Add($state)
        }
        $arms[$name] = [pscustomobject]@{ runs = $runs.Count; cells = $cells; perRun = $perRun; picture = $picture; pictureOf = (Get-PictureOf $picture) }
    }
    # The audio own clock (S5_PRESENT_API.md, comparison A): ownFrames= must grow. An arm whose arrival lookup failed ran the
    # default rule: a failed arm, not a result, and not compared (report.py own_clock_lines).
    $ownClock = [ordered]@{}
    foreach ($name in $names) {
        if ((-not $switchesOf.ContainsKey($name)) -or (@($switchesOf[$name] -split ';') -notcontains 'VEYRA_EXP_RP_AUDIO_OWN_CLOCK=1')) { continue }
        $seen = 0; $frames = 0.0
        foreach ($run in $runsOf[$name]) {
            if (-not $run.byComponent.ContainsKey('remoteplay-audio')) { continue }
            foreach ($item in $run.byComponent['remoteplay-audio']) {
                if ((Get-LogWord ([string]$item.text) 'ownClock') -ne '1') { continue }
                $v = Get-LogNumber ([string]$item.text) 'ownFrames'
                if ($null -ne $v) { $seen++; if ($v -gt $frames) { $frames = $v } }
            }
        }
        if ($seen -eq 0) { $ownClock[$name] = 'own clock: NOT A RESULT, no [remoteplay-audio] line says ownClock=1 with ownFrames= (a build without the switch): the arm ran the default rule' }
        elseif ($frames -le 0) { $ownClock[$name] = 'own clock: FAILED ARM, not a result: ownFrames stayed 0 (the arrival of the shown frames was not found), so the arm ran the default rule' }
        else { $ownClock[$name] = 'own clock ran: ownFrames reached ' + (Format-Number $frames 0) }
    }
    # the owner's reference: an HDR stream in every run, or a line that says it was not (report.py reference_lines)
    $reference = [ordered]@{}
    if (($SessionOfFolder -eq 'visual') -and $runsOf.ContainsKey($visualReference) -and ($names -contains $visualReference)) {
        $total = 0; $sdr = 0; $missing = 0
        foreach ($run in $runsOf[$visualReference]) {
            $total++
            $stream = Get-StreamHdrWord $run
            if ($stream -eq '0') { $sdr++ } elseif ($null -eq $stream) { $missing++ }
        }
        if (($total -gt 0) -and ($sdr -eq $total)) { $reference[$visualReference] = $referenceSdr + $referenceHow }
        elseif ($sdr -gt 0) { $reference[$visualReference] = $referenceSdr + ' in ' + $sdr + ' of ' + $total + ' runs' + $referenceHow }
        elseif ($missing -gt 0) { $reference[$visualReference] = 'reference could not be reproduced: the stream''s HDR was not logged in ' + $missing + ' of ' + $total + ' runs (no streamHdr= in [slipstream-pipeline])' }
    }
    # how evenly each arm's frames were presented (report.py cadence_lines); VRR from the app's [display-vrr] active=
    $cadence = [ordered]@{}
    if ($SessionOfFolder -eq 'smoothness') {
        foreach ($name in $names) {
            $modes = @(); $windows = 0
            $shares = New-Object System.Collections.Generic.List[double]
            $rows = New-Object System.Collections.Generic.List[object]
            foreach ($run in $runsOf[$name]) {
                if ($run.byComponent.ContainsKey('display-vrr')) {
                    foreach ($item in $run.byComponent['display-vrr']) {
                        $active = Get-LogWord ([string]$item.text) 'active'
                        if ((@('true', 'false') -contains $active) -and ($modes -notcontains $active)) { $modes += $active }
                    }
                }
                if (-not $run.byComponent.ContainsKey('slipstream-cadence')) { continue }
                foreach ($item in $run.byComponent['slipstream-cadence']) {
                    $windows++
                    $rows.Add([pscustomobject]@{ off = (Get-LogNumber ([string]$item.text) 'offPct'); fixed = (Get-LogNumber ([string]$item.text) 'fixedUnevenPct') })
                }
            }
            $asked = 'yes'
            if ($switchesOf.ContainsKey($name) -and (@([string]$switchesOf[$name] -split ';') -contains 'pref:vrr=false')) { $asked = 'no' }
            if ($windows -eq 0) { $cadence[$name] = 'presented cadence: cannot tell (no [slipstream-cadence] line: a build without it, or the statistics switch was off)'; continue }
            if ($modes.Count -eq 0) { $cadence[$name] = 'presented cadence: cannot tell (no [display-vrr] line said whether VRR was active)'; continue }
            if ($modes.Count -gt 1) {
                $sortedModes = [string[]]$modes
                [Array]::Sort($sortedModes, [System.StringComparer]::Ordinal)   # as Python's sorted()
                $cadence[$name] = 'presented cadence: cannot tell (VRR changed during the runs: active=' + ($sortedModes -join '/') + '; asked for: ' + $asked + ')'; continue
            }
            $on = ($modes[0] -eq 'true')
            foreach ($row in $rows) {
                if (($null -eq $row.off) -or ($row.off -eq -1)) { continue }
                $share = [double]$row.off
                if ((-not $on) -and ($null -ne $row.fixed) -and ($row.fixed -ne -1) -and ([double]$row.fixed -gt $share)) { $share = [double]$row.fixed }
                $shares.Add($share)
            }
            if ($shares.Count -eq 0) { $cadence[$name] = 'presented cadence: cannot tell (no offPct in the [slipstream-cadence] lines)'; continue }
            $median = Get-Quantile (Get-SortedDoubles $shares) 0.5
            $word = 'cannot tell'
            if ($median -le $cadenceEvenPct) { $word = 'even' } elseif ($median -ge $cadenceUnevenPct) { $word = 'uneven' }
            $vrrText = 'off'
            if ($on) { $vrrText = 'on' }
            $cadence[$name] = 'presented cadence: ' + $word + ' (VRR ' + $vrrText + ', asked for: ' + $asked + '; uneven presents ' + (Format-Number $median 1) + ' % median of ' + $shares.Count + ' windows)'
        }
    }
    # the arms whose build lacked the feature they measure (report.py feature_lines)
    $features = [ordered]@{}
    foreach ($name in $names) {
        $hasPipeline = $false; $hasInput = $false
        foreach ($run in $runsOf[$name]) {
            if ($run.byComponent.ContainsKey('slipstream-pipeline')) { $hasPipeline = $true }
            if ($run.byComponent.ContainsKey('slipstream-input')) { $hasInput = $true }
        }
        $armPipeline = ''
        if ($pipelineOf.ContainsKey($name)) { $armPipeline = [string]$pipelineOf[$name] }
        if ($armPipeline -and (-not $hasPipeline)) { $features[$name] = $pipelineMissing }
        elseif ($switchesOf.ContainsKey($name) -and (Test-OverrideMissing ([string]$switchesOf[$name]) $runsOf[$name])) { $features[$name] = $overrideMissing }
        elseif (($SessionOfFolder -eq 'input') -and (-not $hasInput)) { $features[$name] = $inputMissing }
    }
    # each arm against its pair
    $compared = [ordered]@{}; $notCompared = [ordered]@{}
    foreach ($name in $names) {
        if (-not $pairOf.ContainsKey($name)) { continue }
        $base = $pairOf[$name]
        if ((-not $base) -or ($base -eq $name) -or (-not $arms.Contains($base))) { continue }
        $block = ''
        if ($features.Contains($name) -and ([string]$features[$name] -eq $pipelineMissing)) { $block = 'pipeline keys not applied in this arm (this build has no VEYRA_TEST_RP_PIPELINE)' }
        elseif ($features.Contains($base) -and ([string]$features[$base] -eq $pipelineMissing)) { $block = 'pipeline keys not applied in ' + $base + ' (that build has no VEYRA_TEST_RP_PIPELINE)' }
        elseif ($features.Contains($name) -and ([string]$features[$name] -eq $overrideMissing)) { $block = 'test override not applied in this arm (the request was not the arm''s bitrate / codec)' }
        elseif ($features.Contains($base) -and ([string]$features[$base] -eq $overrideMissing)) { $block = 'test override not applied in ' + $base + ' (the request was not its bitrate / codec)' }
        if (-not $block) { $block = Get-PictureBlock $SessionOfFolder ([string]$arms[$name].pictureOf) ([string]$arms[$base].pictureOf) $base }
        if ((-not $block) -and $ownClock.Contains($name) -and (-not ([string]$ownClock[$name]).StartsWith('own clock ran'))) { $block = [string]$ownClock[$name] }
        if ($block) { $notCompared[$name] = 'not compared with ' + $base + ': ' + $block; continue }
        $wordsOf = [ordered]@{}
        foreach ($row in $shown) {
            if (-not $row.compare) { continue }
            $theirs = @()
            if ($arms[$base].perRun.ContainsKey([string]$row.key)) { $theirs = $arms[$base].perRun[[string]$row.key] }
            $mineValues = $arms[$name].perRun[[string]$row.key]
            $wordsOf[[string]$row.key] = Get-SpreadVerdict $mineValues $theirs
        }
        $compared[$name] = $wordsOf
    }
    # the neural layer of every arm that asks for it: never a blank
    $runtime = [ordered]@{}
    foreach ($name in $names) {
        if ((-not $pipelineOf.ContainsKey($name)) -or (@($pipelineOf[$name] -split ';') -notcontains 'nr=1')) { continue }
        $status = $null; $detail = $null
        foreach ($run in $runsOf[$name]) {
            if ($run.byComponent.ContainsKey('nr-runtime-status')) { $list = $run.byComponent['nr-runtime-status']; $status = [string]$list[$list.Count - 1].text }
            if ($run.byComponent.ContainsKey('slipstream-pipeline')) {
                $list = $run.byComponent['slipstream-pipeline']
                $detail = Get-LogWord ([string]$list[$list.Count - 1].text) 'detail'
                if ($null -eq $detail) { $detail = '' }
            }
        }
        $word = @{}
        foreach ($k in @('selected', 'selectedAvailable', 'reason', 'original', 'originalSigned', 'gpuSupportsOriginal', 'originalVersion')) {
            $word[$k] = '?'
            if ($null -ne $status) { $w = Get-LogWord $status $k; if ($null -ne $w) { $word[$k] = $w } }
        }
        $refused = @(); $unknown = @()
        if ($detail) { foreach ($d in ($detail -split ',')) { if (($neuralKeys -contains ($d -split ':')[0]) -and ($d.IndexOf(':refused') -ge 0)) { $refused += $d } } }
        foreach ($d in $refused) { if ($d.IndexOf('unknown-key') -ge 0) { $unknown += $d } }
        if (($null -eq $status) -and ($null -eq $detail)) { $runtime[$name] = 'not in this build: skipped (no [slipstream-pipeline] and no [nr-runtime-status] line: a build without the test hook cannot be asked for the neural layer)' + $neuralWithout }
        elseif ($unknown.Count -gt 0) { $runtime[$name] = 'not in this build: skipped (the build does not know the neural keys: ' + ($unknown -join ', ') + ')' + $neuralWithout }
        elseif (($null -ne $status) -and ($word['selectedAvailable'] -eq '0')) {
            # The installer keeps the NVIDIA file only on an explicit yes: an arm without it still ran, and says so.
            $kind = 'not usable on this PC: skipped'
            if (($word['reason'] -eq 'runtime-missing') -or ($word['original'] -eq 'absent')) { $kind = 'not installed: skipped' }
            $runtime[$name] = $kind + ' (the app reports selected=' + $word['selected'] + ' selectedAvailable=0 reason=' + $word['reason'] + ', original=' + $word['original'] + ', originalSigned=' + $word['originalSigned'] + ', gpuSupportsOriginal=' + $word['gpuSupportsOriginal'] + ')' + $neuralWithout
        }
        elseif ($refused.Count -gt 0) { $runtime[$name] = 'refused by the app: skipped (' + ($refused -join ', ') + ')' + $neuralWithout }
        elseif ($null -eq $status) { $runtime[$name] = 'neural layer applied per [slipstream-pipeline]; runtime state not logged (no [nr-runtime-status] line)' }
        else { $runtime[$name] = 'neural runtime available: selected=' + $word['selected'] + ' version=' + $word['originalVersion'] + ' signed=' + $word['originalSigned'] + ' reason=' + $word['reason'] }
    }
    # the visual verdicts
    $verdicts = [ordered]@{}
    if (($SessionOfFolder -eq 'visual') -and $arms.Contains($visualBase)) {
        $metricsOf = @{}
        foreach ($name in $names) {
            $one = @{}
            foreach ($k in ($visualDefects + @('detail'))) {
                $key = 'quality output|slipstream-quality|stage=output|' + $k + '|median'
                if ($arms[$name].cells.Contains($key) -and ($null -ne $arms[$name].cells[$key].value)) { $one[$k] = @([double]$arms[$name].cells[$key].value, [int]$arms[$name].cells[$key].n) }
            }
            $metricsOf[$name] = $one
        }
        foreach ($name in $names) {
            if ($name -eq $visualBase) { $verdicts[$name] = 'reference of the comparison'; continue }
            if ($name -eq $visualReference) { $verdicts[$name] = 'the owner''s reference settings (4K output, HDR, frame generation): measured values only, no verdict'; continue }
            if (($features.Contains($name) -and ([string]$features[$name] -eq $pipelineMissing)) -or ($features.Contains($visualBase) -and ([string]$features[$visualBase] -eq $pipelineMissing))) {
                $verdicts[$name] = 'no verdict: the pipeline keys were not applied (this build has no VEYRA_TEST_RP_PIPELINE)'; continue
            }
            if ($runtime.Contains($name) -and (-not ([string]$runtime[$name]).StartsWith('neural runtime available')) -and (-not ([string]$runtime[$name]).StartsWith('neural layer applied'))) {
                $verdicts[$name] = 'no verdict: the neural layer did not run in this arm (the line above says why); its values are those of the path without it'; continue
            }
            $block = Get-PictureBlock $SessionOfFolder ([string]$arms[$name].pictureOf) ([string]$arms[$visualBase].pictureOf) $visualBase
            if ($block) { $verdicts[$name] = 'no verdict: ' + $block; continue }
            $processing = $null
            $pKey = 'processing|slipstream-nr||graphMsP95|median'
            if ($arms[$name].cells.Contains($pKey)) { $processing = $arms[$name].cells[$pKey].value }
            $sizeKey = 'quality output|slipstream-quality|stage=output|size|word'
            $mineSize = 'n/a'; $baseSize = 'n/a'
            if ($arms[$name].cells.Contains($sizeKey)) { $mineSize = [string]$arms[$name].cells[$sizeKey].text }
            if ($arms[$visualBase].cells.Contains($sizeKey)) { $baseSize = [string]$arms[$visualBase].cells[$sizeKey].text }
            $verdicts[$name] = Get-VisualVerdict ($metricsOf[$name]) ($metricsOf[$visualBase]) $processing $mineSize $baseSize
        }
    }
    # PS-button answer time of one arm's runs more than 50 ms apart
    $spreadNotes = [ordered]@{}
    foreach ($name in $names) {
        $medians = New-Object System.Collections.Generic.List[double]
        foreach ($run in $runsOf[$name]) {
            if (($run.purpose -ne 'latency') -or (-not $run.byComponent.ContainsKey('push-latency'))) { continue }
            $presses = New-Object System.Collections.Generic.List[object]; $summaries = New-Object System.Collections.Generic.List[object]
            foreach ($item in $run.byComponent['push-latency']) {
                if ([string]$item.text -match '^trials answerMs=\[([^\]]*)\]') {
                    foreach ($part in ($Matches[1] -split ',')) { $number = Read-Number $part.Trim(); if ($null -ne $number) { $presses.Add($number) } }
                } elseif ([string]$item.text -match '^done answered=(\d+)/(\d+) meanMs=([\d.]+) medianMs=([\d.]+).*pushfpsMs=([\d.]+)') {
                    $number = Read-Number $Matches[4]; if ($null -ne $number) { $summaries.Add($number) }
                }
            }
            if ($presses.Count -gt 0) { $medians.Add((Get-Quantile (Get-SortedDoubles $presses) 0.5)) }
            elseif ($summaries.Count -gt 0) { $medians.Add((Get-Quantile (Get-SortedDoubles $summaries) 0.5)) }
        }
        if ($medians.Count -ge 2) {
            $low = $medians[0]; $high = $medians[0]; $texts = @()
            foreach ($m in $medians) { if ($m -lt $low) { $low = $m }; if ($m -gt $high) { $high = $m }; $texts += (Format-Number $m 0) }
            if (($high - $low) -gt $buttonSpreadMs) {
                $spreadNotes[$name] = 'the PS-button answer time of ' + $name + ' differs by ' + (Format-Number ($high - $low) 0) + ' ms between its runs (run medians ' + ($texts -join ', ') + ' ms): the console answers in more than one state (about 450 and 550 ms were seen), so this test cannot compare arms at this size'
            }
        }
    }
    return [pscustomobject]@{ session = $SessionOfFolder; groups = $groups; spec = $shown; arms = $arms; names = $names; pairOf = $pairOf; compared = $compared; notCompared = $notCompared; ownClock = $ownClock; verdicts = $verdicts; runtime = $runtime; buttonSpread = $spreadNotes; reference = $reference; features = $features; cadence = $cadence }
}

# 'label value, label value' of one group of one arm (report.py measured_group_text).
function Get-MeasuredGroupText($Measured, [string]$Arm, [string]$Group) {
    $parts = @(); $any = $false
    foreach ($row in $Measured.spec) {
        if ($row.group -ne $Group) { continue }
        $cell = $Measured.arms[$Arm].cells[[string]$row.key]
        if ($cell.n -gt 0) { $any = $true }
        $parts += ([string]$row.label + ' ' + [string]$cell.text)
    }
    if (-not $any) { return 'not logged in this arm''s runs' }
    return ($parts -join ', ')
}

# RESULT.txt section MEASURED VALUES (report.py measured_paragraphs writes the same lines).
function Add-MeasuredSection($Out, $Measured, [string]$Route) {
    $heading = 'MEASURED VALUES'
    if ($Measured.session) { $heading += ': ' + ([string]$Measured.session).ToUpperInvariant() }
    $Out.Add($heading + ' (software counters per arm, medians of the app''s lines unless named; never input-to-photon)')
    if ($Route -eq 'wifi') { $Out.Add('  ' + $wifiLabel) }
    if (@($Measured.names).Count -eq 0) { $Out.Add('  no run'); $Out.Add(''); return }
    if ($sessionFirstLine.ContainsKey([string]$Measured.session)) { $Out.Add('  ' + $sessionFirstLine[[string]$Measured.session]) }
    foreach ($name in $Measured.names) {
        $Out.Add('  ' + $name + ' (' + $Measured.arms[$name].runs + ' runs)')
        if ($pictureSessions -contains [string]$Measured.session) { $Out.Add('    picture: ' + ($Measured.arms[$name].picture.ToArray() -join ', ') + ' (per run)') }
        if ($Measured.reference.Contains($name)) { $Out.Add('    ' + $Measured.reference[$name]) }
        if ($Measured.features.Contains($name)) { $Out.Add('    ' + $Measured.features[$name]) }
        foreach ($group in $Measured.groups) { $Out.Add('    ' + $group + ': ' + (Get-MeasuredGroupText $Measured $name $group)) }
        if ($Measured.runtime.Contains($name)) { $Out.Add('    neural layer: ' + $Measured.runtime[$name]) }
        if ($Measured.ownClock.Contains($name)) { $Out.Add('    ' + $Measured.ownClock[$name]) }
        if ($Measured.cadence.Contains($name)) { $Out.Add('    ' + $Measured.cadence[$name]) }
        if ($Measured.compared.Contains($name)) {
            $base = [string]$Measured.pairOf[$name]
            $parts = @()
            foreach ($word in $spreadWords) {
                $list = @()
                foreach ($row in $Measured.spec) {
                    if ((-not $Measured.compared[$name].Contains([string]$row.key)) -or ($Measured.compared[$name][[string]$row.key] -ne $word)) { continue }
                    $text = [string]$row.group + ' ' + [string]$row.label
                    if ($word -eq 'differs') { $text += ' (' + $Measured.arms[$name].cells[[string]$row.key].text + ' / ' + $Measured.arms[$base].cells[[string]$row.key].text + ')' }
                    $list += $text
                }
                if ($list.Count -gt 0) { $parts += ($word + ': ' + ($list -join ', ')) }
            }
            if ($parts.Count -gt 0) { $Out.Add('    against ' + $base + ': ' + ($parts -join '; ')) }
            else { $Out.Add('    against ' + $base + ': no value marked for comparison in this session') }
        }
        if ($Measured.notCompared.Contains($name)) { $Out.Add('    ' + $Measured.notCompared[$name]) }
        if ($Measured.verdicts.Contains($name)) { $Out.Add('    verdict: ' + $Measured.verdicts[$name]) }
    }
    if ($pictureSessions -contains [string]$Measured.session) { $Out.Add('  ' + $pictureRule) }
    if ($Measured.compared.Count -gt 0) { $Out.Add('  ' + $spreadRule) }
    if ($Measured.verdicts.Count -gt 0) { $Out.Add('  ' + $visualRule) }
    if ($Measured.cadence.Count -gt 0) { $Out.Add('  ' + $cadenceRule) }
    foreach ($name in $Measured.buttonSpread.Keys) { $Out.Add('  NOTE: ' + $Measured.buttonSpread[$name]) }
    $Out.Add('  ' + $measuredFoot)
    $Out.Add('')
}

# result.json 'measured' (report.py measured_json).
function ConvertTo-MeasuredJson($Measured) {
    $arms = [ordered]@{}
    $pictures = [ordered]@{}
    foreach ($name in $Measured.names) {
        $pictures[$name] = $Measured.arms[$name].picture.ToArray()
        $cells = [ordered]@{}
        foreach ($key in $Measured.arms[$name].cells.Keys) {
            $cell = $Measured.arms[$name].cells[$key]
            $cells[$key] = [pscustomobject]@{ value = (Get-JsonNumber $cell.value); n = [int]$cell.n; text = [string]$cell.text }
        }
        $arms[$name] = [pscustomobject]@{ runs = [int]$Measured.arms[$name].runs; cells = $cells }
    }
    return [pscustomobject]@{ session = [string]$Measured.session; arms = $arms; compared = $Measured.compared; notCompared = $Measured.notCompared; picture = $pictures; ownClock = $Measured.ownClock; verdicts = $Measured.verdicts; runtime = $Measured.runtime; buttonSpread = @($Measured.buttonSpread.Keys); reference = $Measured.reference; features = $Measured.features; cadence = $Measured.cadence }
}

# Time estimates (kit 5): from the measured runs of 20 Nov (12 starts of 'settle:8;latency:12;exit' took 53 to 75 s, median
# 54 s). Start, connect and close: 15 s; a press 3 s; a latency test 2 s more; every settle / wait / probe / sr / fg step its
# own seconds; a quality step 30 s; a start done by hand 60 s more. report.py plan_seconds holds the same numbers.
function Get-PlanSeconds([string]$Plan) {
    $total = 15.0
    foreach ($step in ($Plan -split ';')) {
        if (-not $step) { continue }
        $name = $step; $amount = 0.0
        $at = $step.IndexOf(':')
        if ($at -ge 0) { $name = $step.Substring(0, $at); $number = Read-Number ($step.Substring($at + 1)); if ($null -ne $number) { $amount = $number } }
        if (@('settle', 'wait', 'probe', 'sr', 'fg') -contains $name) { $total += $amount }
        elseif ($name -eq 'latency') { $total += ($amount * 3.0 + 2.0) }
        elseif ($name -eq 'quality') { $total += 30.0 }
    }
    return $total
}

function Get-AboutMinutes([double]$Seconds) {
    return [int][math]::Max(1, [math]::Floor($Seconds / 60.0 + 0.5))
}

function Write-ResultFiles([string]$Folder) {
    $tablePath = Join-Path $Folder 'arms.csv'
    if (-not (Test-Path -LiteralPath $tablePath)) { $tablePath = Join-Path $kitFolder 'arms.csv' }
    $armTable = @(Read-ArmTable $tablePath)
    $kept = @(Read-KeptRuns $Folder)
    $note = ''
    $kitFile = Join-Path $Folder 'kit.json'
    $dryNote = $false
    $sessionOfFolder = ''
    $kitRecord = $null
    if (Test-Path -LiteralPath $kitFile) {
        try {
            $kitRecord = Get-Content -LiteralPath $kitFile -Raw | ConvertFrom-Json
            $note = [string]$kitRecord.sceneNote
            $dryNote = [bool]$kitRecord.dryRun
            $sessionOfFolder = [string]$kitRecord.session
        } catch { $note = '' }
    }
    $setAside = 0
    if (Test-Path -LiteralPath (Join-Path $Folder 'setaside')) { $setAside = @(Get-ChildItem -LiteralPath (Join-Path $Folder 'setaside') -Recurse -Filter 'run.json').Count }

    # per arm: presses, configurations, facts
    $arms = @()
    $allConfigs = @()
    foreach ($arm in $armTable) {
        $runs = @($kept | Where-Object { ($_.record.arm -eq $arm.name) -and ($_.record.purpose -eq 'latency') })
        if ($runs.Count -eq 0) { continue }
        $presses = New-Object System.Collections.Generic.List[double]
        $testMedians = New-Object System.Collections.Generic.List[double]
        $answered = 0; $tests = 0; $errors = 0; $warnings = 0
        $firstProblems = @()
        $decodeFps = New-Object System.Collections.Generic.List[double]
        $shownFps = New-Object System.Collections.Generic.List[double]
        $decodeBusy = New-Object System.Collections.Generic.List[double]
        $decodeP95 = New-Object System.Collections.Generic.List[double]
        $jitter = New-Object System.Collections.Generic.List[double]
        $loss = New-Object System.Collections.Generic.List[double]
        $queueDepth = New-Object System.Collections.Generic.List[double]
        $reordered = 0; $late = 0; $keyframes = 0
        $bottlenecks = @{}
        $configs = @()
        $fgWrong = 0
        $sampleFiles = @()
        foreach ($run in $runs) {
            $sampleFiles += (Join-Path $run.folder 'samples.csv')
            $benchFile = Join-Path (Join-Path $Folder 'bench') ($run.name + '.txt')
            $lines = @()
            if (Test-Path -LiteralPath $benchFile) { $lines = @(Get-Content -LiteralPath $benchFile) }
            $config = Get-RunConfig $lines
            $config.from = ("run " + $run.name)
            $configs += $config
            $allConfigs += $config
            if ((($arm.fg -eq 'off') -and ($config.frameGeneration -eq 'on')) -or (($arm.fg -eq 'on') -and ($config.frameGeneration -eq 'off'))) { $fgWrong++ }
            foreach ($line in $lines) {
                if ($line -match '\] \[push-latency\] trials answerMs=\[([^\]]*)\]') {
                    foreach ($value in ($Matches[1] -split ',')) { $number = Read-Number $value.Trim(); if ($null -ne $number) { $presses.Add($number) } }
                } elseif ($line -match '\] \[push-latency\] done answered=(\d+)/') {
                    $count = [int]$Matches[1]
                    $tests++
                    $answered += $count
                    $median = Get-LogNumber $line 'medianMs'
                    if (($count -gt 0) -and ($null -ne $median)) { $testMedians.Add($median) }
                } elseif ($line -match '\] \[remoteplay-progress\] ') {
                    $value = Get-LogNumber $line 'decodeFps'; if ($null -ne $value) { $decodeFps.Add($value) }
                    $value = Get-LogNumber $line 'decodeBusyMs'; if ($null -ne $value) { $decodeBusy.Add($value) }
                } elseif ($line -match '\] \[frame-rate\] ') {
                    $value = Get-LogNumber $line 'presentSubmitFps'; if ($null -ne $value) { $shownFps.Add($value) }
                } elseif ($line -match '\] \[remoteplay-health\] grade=') {
                    $value = Get-LogNumber $line 'frameJitterMs'; if ($null -ne $value) { $jitter.Add($value) }
                    $value = Get-LogNumber $line 'lossPercent'; if ($null -ne $value) { $loss.Add($value) }
                    $value = Get-LogNumber $line 'decodeP95Ms'; if ($null -ne $value) { $decodeP95.Add($value) }
                    $value = Get-LogNumber $line 'reordered'; if ($null -ne $value) { $reordered += $value }
                    $value = Get-LogNumber $line 'late'; if ($null -ne $value) { $late += $value }
                    $value = Get-LogNumber $line 'keyframeRequests'; if ($null -ne $value) { $keyframes += $value }
                    if ($line -match ' bottleneck=([a-z-]+)') {
                        $name = $Matches[1]
                        if ($bottlenecks.ContainsKey($name)) { $bottlenecks[$name]++ } else { $bottlenecks[$name] = 1 }
                    }
                } elseif ($line -match '\] \[slipstream-governor\] ') {
                    $value = Get-LogNumber $line 'queueDepth'; if ($null -ne $value) { $queueDepth.Add($value) }
                }
                if ($line -match '\[(ERROR|WARN )\] ') {
                    if ($Matches[1] -eq 'ERROR') { $errors++ } else { $warnings++ }
                    if ($firstProblems.Count -lt 3) {
                        $short = $line
                        if ($short.Length -gt 160) { $short = Hide-OwnWords (Protect-Text $short.Substring(0, 160)) }   # a cut mask (host=[nam) must not look like a name
                        $firstProblems += $short
                    }
                }
            }
        }
        # settings: the same within the arm?
        $reasons = @()
        foreach ($item in $configKeys) {
            $seen = @()
            foreach ($config in $configs) { $value = [string]$config.($item.key); if ($value -and ($seen -notcontains $value)) { $seen += $value } }
            if ($seen.Count -gt 1) { $reasons += ('its runs differ in ' + $item.label + ': ' + ($seen -join ' / ') + $item.unit) }
        }
        foreach ($config in $configs) { foreach ($what in @($config.changedDuringRun)) { if ($what) { $reasons += ($what + ' changed during ' + $config.from) } } }
        # every observable key: the runs of one arm must agree (report.py arm_config)
        $armKeys = @{}
        $keysDiffer = @()
        foreach ($config in $configs) {
            $one = Get-KeyTable $config.keys
            foreach ($k in $one.Keys) {
                if ($armKeys.ContainsKey($k)) { if (($armKeys[$k] -cne $one[$k]) -and ($keysDiffer -notcontains $k)) { $keysDiffer += $k } }
                else { $armKeys[$k] = $one[$k] }
            }
        }
        if ($keysDiffer.Count -gt 0) { $reasons += ("the runs of '" + $arm.name + "' differ in " + (($keysDiffer | Sort-Object) -join ', ')) }
        foreach ($config in $configs) { foreach ($key in @($config.changedKeys)) { if ($key) { $reasons += ($key + ' changed during ' + $config.from) } } }
        $profiledAll = $true; $methods = @()
        foreach ($run in $runs) {
            if (-not $run.record.testProfile) { $profiledAll = $false }
            # The test method is the plan: presses per test. Started by the automation or from the menu it is the same
            # in-app latency test (report.py test_method), so a 12-press plan compares across builds.
            if ($run.record.trialsAsked) {
                $method = [string]$run.record.trialsAsked + ' presses per test'
                if ($methods -notcontains $method) { $methods += $method }
            }
        }
        if ($fgWrong -gt 0) { $reasons += ('frame generation was not ' + $arm.fg.ToUpperInvariant() + ' in ' + $fgWrong + ' run(s)') }
        $merged = [pscustomobject]@{ profile = ''; codec = ''; bitrateKbps = ''; refreshHz = ''; vrr = ''; frameGeneration = ''; decoder = ''; from = ("arm '" + $arm.name + "'") }
        foreach ($item in $configKeys + @([pscustomobject]@{ key = 'frameGeneration' }, [pscustomobject]@{ key = 'decoder' })) {
            foreach ($config in $configs) { if ((-not $merged.($item.key)) -and $config.($item.key)) { $merged.($item.key) = [string]$config.($item.key) } }
        }
        $sorted = [double[]]@($presses.ToArray() | Sort-Object)
        $medianInterval = Get-QuantileInterval $sorted 0.5
        $p95Interval = Get-QuantileInterval $sorted 0.95
        $p99Interval = Get-QuantileInterval $sorted 0.99
        $bottleneckText = ''
        foreach ($key in @($bottlenecks.Keys | Sort-Object)) { $bottleneckText += ($key + ' ' + $bottlenecks[$key] + '; ') }
        $arms += [pscustomobject]@{
            name = $arm.name; exe = $arm.exe; compare = $arm.compare; fg = [string]$arm.fg; switches = [string]$arm.switches; starts = $runs.Count
            pressValues = $sorted.Count; tests = $tests; answered = $answered; testMedians = $testMedians.ToArray()
            median = (Get-Quantile $sorted 0.5); medianLow = $medianInterval[0]; medianHigh = $medianInterval[1]
            p95 = (Get-Quantile $sorted 0.95); p95Low = $p95Interval[0]; p95High = $p95Interval[1]
            p99 = (Get-Quantile $sorted 0.99); p99Low = $p99Interval[0]; p99High = $p99Interval[1]
            config = $merged; reasons = $reasons; refused = $false; verdict = ''; keys = $armKeys; assumed = @(); differs = [string]$arm.differs
            profiled = ($profiledAll -and ($runs.Count -gt 0)); method = (($methods | Sort-Object) -join ' / ')
            decodeFps = (Get-Median $decodeFps.ToArray()); shownFps = (Get-Median $shownFps.ToArray()); decodeBusyMs = (Get-Median $decodeBusy.ToArray())
            decodeP95Ms = (Get-Median $decodeP95.ToArray()); jitterMs = (Get-Mean $jitter.ToArray()); lossPercent = (Get-Mean $loss.ToArray())
            healthIntervals = $jitter.Count; reordered = $reordered; late = $late; keyframeRequests = $keyframes; bottlenecks = $bottleneckText.Trim()
            queueDepthMax = $(if ($queueDepth.Count -gt 0) { ($queueDepth.ToArray() | Measure-Object -Maximum).Maximum } else { $null })
            cpuPercent = (Get-Mean (Read-SampleColumn $sampleFiles 'cpuTotalPercent')); appGpu3dPercent = (Get-Mean (Read-SampleColumn $sampleFiles 'appGpu3dPercent'))
            gpuPercent = (Get-Mean (Read-SampleColumn $sampleFiles 'gpuPercent'))
            errors = $errors; warnings = $warnings; firstProblems = $firstProblems
        }
    }
    # refusals against the pair, then verdicts
    foreach ($entry in $arms) {
        $base = @($arms | Where-Object { $_.name -eq $entry.compare })
        if (($entry.compare -ne $entry.name) -and ($base.Count -gt 0)) {
            $skipKeys = @()
            if (@(([string]$entry.differs) -split ';') -contains 'vrrActive') { $skipKeys = @('vrr') }   # going full screen may turn variable refresh on (arms.csv column differs)
            foreach ($difference in @(Compare-RunConfig $entry.config $base[0].config $skipKeys)) { if ($difference) { $entry.reasons += ($difference -replace ' in this run, ', ' here, ') } }
            $matter = Get-SettingsThatMatter $entry.keys $base[0].keys ($entry.profiled -and $base[0].profiled)
            $entry.assumed = @($matter.assumed)
            $keyDifferences = @(Compare-ConfigKeys $matter.mine $entry.name $matter.theirs $base[0].name $entry.differs)
            if ($keyDifferences.Count -gt 0) { $entry.reasons += ('the two arms ran with different settings: ' + ($keyDifferences -join '; ') + '. Fix: ' + (Get-ConfigFix $keyDifferences[0])) }
            else {
                $blind = @(Get-OneSidedKeys $matter.mine $matter.theirs $entry.differs ($entry.profiled -and $base[0].profiled))
                if ($blind.Count -gt 0) { $entry.reasons += ('not comparable: NOT CHECKED, logged by one side only: ' + ($blind -join ', ') + '. These settings change the latency; the build that does not log them cannot show that they were the same') }
                elseif ($entry.method -and $base[0].method -and ($entry.method -ne $base[0].method)) { $entry.reasons += ('the two arms used a different test method (' + $entry.name + ': ' + $entry.method + '; ' + $base[0].name + ': ' + $base[0].method + '): latency comparison refused') }
            }
            if ($base[0].fg -eq $entry.fg) {
                $mine = $entry.config.frameGeneration; $theirs = $base[0].config.frameGeneration
                if ($mine -and $theirs -and ($mine -ne $theirs)) { $entry.reasons += ('frame generation: ' + $mine + ' here, ' + $theirs + " in arm '" + $base[0].name + "'") }
            }
        }
        $entry.refused = (@($entry.reasons).Count -gt 0)
    }
    foreach ($entry in $arms) {
        $base = @($arms | Where-Object { $_.name -eq $entry.compare })
        if ($entry.compare -eq $entry.name) { $entry.verdict = 'baseline'; if ($entry.refused) { $entry.verdict = 'baseline, REFUSED: ' + (@($entry.reasons) -join '; ') } }
        elseif ($base.Count -eq 0) { $entry.verdict = "no verdict: its pair '" + $entry.compare + "' was not run" }
        elseif ($entry.refused) { $entry.verdict = 'REFUSED: ' + (@($entry.reasons) -join '; ') }
        elseif ($base[0].refused) { $entry.verdict = "REFUSED: its pair '" + $base[0].name + "' is refused" }
        elseif (($entry.pressValues -eq 0) -or ($base[0].pressValues -eq 0)) { $entry.verdict = 'no verdict: per-press values are not logged by one of the two builds (only test summaries)' }
        elseif (($entry.pressValues -lt $resultMinimum) -or ($base[0].pressValues -lt $resultMinimum)) { $entry.verdict = 'no verdict: fewer than ' + $resultMinimum + ' presses (' + $entry.pressValues + ' and ' + $base[0].pressValues + ')' }
        elseif ([double]::IsNaN($entry.medianHigh) -or [double]::IsNaN($base[0].medianLow) -or [double]::IsNaN($entry.medianLow) -or [double]::IsNaN($base[0].medianHigh)) { $entry.verdict = 'no verdict: no interval' }
        elseif ($entry.medianHigh -lt $base[0].medianLow) { $entry.verdict = 'LOWER than ' + $base[0].name + ' by ' + (Format-Number ($base[0].median - $entry.median) 1) + ' ms (median)' }
        elseif ($entry.medianLow -gt $base[0].medianHigh) { $entry.verdict = 'HIGHER than ' + $base[0].name + ' by ' + (Format-Number ($entry.median - $base[0].median) 1) + ' ms (median)' }
        else { $entry.verdict = 'no difference shown: the 95 % intervals of the medians overlap (vs ' + $base[0].name + ')' }
        # report.py result_summary: settings one build does not log, taken from the test profile, are named in the verdict.
        if ((-not $entry.refused) -and (@($entry.assumed).Count -gt 0)) { $entry.verdict += ('; compared between the builds with ' + (@($entry.assumed) -join ', ') + ' assumed from the test profile (not logged by one build)') }
    }
    # The route comes from every run of the folder (report.py network_route), so a session without a latency run
    # (network, quality, dedup-probe) says Wi-Fi when its own runs did. The first run that names a route is enough.
    $routeConfigs = @($allConfigs)
    if (@($routeConfigs | Where-Object { $_ -and ((@('wifi', 'wired') -contains [string]$_.route) -or (@('wifi', 'wired') -contains [string]$_.healthLink)) }).Count -eq 0) {
        foreach ($run in @($kept | Where-Object { $_.record.purpose -ne 'latency' })) {
            $benchFile = Join-Path (Join-Path $Folder 'bench') ($run.name + '.txt')
            if (-not (Test-Path -LiteralPath $benchFile)) { continue }
            $other = Get-RunConfig @(Get-Content -LiteralPath $benchFile) $false
            if ((@('wifi', 'wired') -contains [string]$other.route) -or (@('wifi', 'wired') -contains [string]$other.healthLink)) { $routeConfigs += $other; break }
        }
    }
    $route = Get-NetworkRoute $Folder $routeConfigs
    if ($route -eq 'wifi') { foreach ($entry in $arms) { $entry.verdict = $wifiLabel + '; ' + $entry.verdict } }

    # camera / light sensor rows: counted only
    $physical = @()
    foreach ($name in @('camera.csv', 'sensor.csv')) {
        $path = Join-Path $Folder $name
        $rows = 0
        if (Test-Path -LiteralPath $path) { $rows = @(Get-Content -LiteralPath $path | Where-Object { $_ -and ($_ -notmatch '^\s*#') } | Select-Object -Skip 1).Count }
        $physical += [pscustomobject]@{ file = $name; rows = $rows }
    }
    $otherRuns = @()
    foreach ($run in $kept) { if ($run.record.purpose -ne 'latency') { $otherRuns += $run } }

    # RESULT.txt
    $out = New-Object System.Collections.Generic.List[string]
    if ($route -eq 'wifi') { $out.Add('SLIPSTREAM TEST RESULT - ' + $wifiLabel + ' (the default route of this PC was Wi-Fi)') }
    else { $out.Add('SLIPSTREAM TEST RESULT (one page; the details are in report.md after report.py)') }
    if ($dryNote) { $out.Add('DRY RUN: made-up data, not a measurement.') }
    $out.Add('Written ' + (Get-UtcStamp) + ' by the kit (run-tests.ps1).')
    # Which download installed each copy (8A / 8B / 8C / 8, or Package 7), from kit.json (Get-ExeFacts).
    $copyLabels = @()
    if ($kitRecord) {
        foreach ($slot in @('experimental', 'previous', 'modified', 'original')) {
            $facts = $kitRecord.$slot
            if ($facts -and $facts.package) { $copyLabels += ($slot + ': ' + [string]$facts.package) }
        }
    }
    if ($copyLabels.Count -gt 0) { $out.Add('Copies: ' + ($copyLabels -join '; ')) }
    if ($note) { $out.Add('Scene: ' + $note) }
    $out.Add('')
    $out.Add('Network: ' + $route + ' (details are in report.md after report.py)')
    $out.Add('')
    $out.Add('SOFTWARE LATENCY (the built-in test: software, not input-to-photon)')
    if ($route -eq 'wifi') { $out.Add('  ' + $wifiLabel) }
    $out.Add('  arm             presses  median ms [95 % interval]  P95 ms [95 % interval]  P99 ms [95 % interval]  starts  verdict')
    foreach ($entry in $arms) {
        if ($entry.pressValues -gt 0) {
            $out.Add(('  {0,-15} {1,7}  {2,6} [{3}, {4}]  {5,6} [{6}, {7}]  {8,6} [{9}, {10}]  {11,6}  {12}' -f $entry.name, $entry.pressValues,
                (Format-Number $entry.median 1), (Format-Number $entry.medianLow 1), (Format-Number $entry.medianHigh 1),
                (Format-Number $entry.p95 1), (Format-Number $entry.p95Low 1), (Format-Number $entry.p95High 1),
                (Format-Number $entry.p99 1), (Format-Number $entry.p99Low 1), (Format-Number $entry.p99High 1), $entry.starts, $entry.verdict))
        } else {
            $out.Add(('  {0,-15} {1,7}  median of {2} test medians {3}; no per-press values in this build, so no P95/P99  {4,6}  {5}' -f $entry.name, $entry.answered,
                @($entry.testMedians).Count, (Format-Number (Get-Median ([double[]]@($entry.testMedians))) 1), $entry.starts, $entry.verdict))
        }
    }
    if ($arms.Count -eq 0) { $out.Add('  no latency run in this folder') }
    $out.Add('  Verdict rule: ' + $resultRule)
    $out.Add('')
    $out.Add('REFUSED ARMS')
    $anyRefused = $false
    foreach ($entry in $arms) { if ($entry.refused) { $anyRefused = $true; $out.Add('  ' + $entry.name + ': ' + (@($entry.reasons) -join '; ')) } }
    if (-not $anyRefused) { $out.Add('  none') }
    $crossBuild = @($arms | Where-Object { $e = $_; ($e.compare -ne $e.name) -and (@($arms | Where-Object { ($_.name -eq $e.compare) -and ($_.exe -ne $e.exe) }).Count -gt 0) })
    if ($crossBuild.Count -gt 0) {
        $out.Add('BETWEEN BUILDS (in plain words)')
        $out.Add('  Compared: the per-press latency of the same 12-press test plan, with the settings both builds log checked equal (low-latency queue, display sync, variable refresh, refresh rate, stream size, codec, bitrate and HDR request, decoder, frame generation on or off, full screen from the interface line).')
        $out.Add('  Taken from the kit''s test profile where one build does not log them (named "assumed from the test profile" in the verdict): NVIDIA low latency, smooth pacing, super resolution off, enhancement off. Frame generation details do not count while it is off on both sides.')
        $out.Add('  Not compared: anything a verdict lists as "NOT CHECKED, logged by one side only"; split latency, stutter and SmartLink numbers, which the stable build does not log.')
    }
    if ($setAside -gt 0) { $out.Add('  ' + $setAside + ' run(s) were set aside during the session because their settings differed (folder setaside, not used).') }
    $out.Add('')
    # Kit 5: the measured values of every arm (measures.csv). A failure here is written into the page, never hidden.
    $measured = $null; $measuredError = ''
    try { $measured = Get-MeasuredTables $Folder $kept $armTable $sessionOfFolder }
    catch { $measuredError = 'tables: ' + (Get-KitErrorText $_) }
    $script:measuredNow = $measured
    $tables = Add-PrecisionTables $out $Folder $arms $kept $route
    $networkSession = Add-NetworkSection $out $Folder $kept $armTable $route
    Add-ShortSessionSection $out $Folder $kept $route
    $measuredJson = $null
    if ($measured) {
        try { Add-MeasuredSection $out $measured $route; $measuredJson = ConvertTo-MeasuredJson $measured }
        catch { $measuredError = 'section: ' + (Get-KitErrorText $_) }
    }
    if ($measuredError) { $out.Add('MEASURED VALUES: not written, the kit failed here: ' + (Hide-OwnWords (Protect-Text $measuredError))); $out.Add('') }
    $out.Add('WHAT EACH ARM RAN WITH (from its own log lines)')
    foreach ($entry in $arms) {
        $switchText = $entry.switches
        if (-not $switchText) { $switchText = 'no switch' }
        $out.Add('  ' + $entry.name + ' (' + $entry.exe + ' build; ' + $switchText + '): ' + (Get-ConfigText $entry.config))
        $keyText = @()
        foreach ($k in (Get-SortedKeys $entry.keys @{})) { $keyText += ($k + '=' + $entry.keys[$k]) }
        if ($keyText.Count -gt 0) { $out.Add('      keys: ' + ($keyText -join ' ')) }
    }
    # the library switch no app line shows, from each run's own environment (run.json chiakiSendBuffer)
    $chiakiOf = [ordered]@{}; $chiakiSet = $false
    foreach ($run in $kept) {
        $value = [string]$run.record.chiakiSendBuffer
        if (-not $value) { continue }
        if (-not $value.StartsWith('unset')) { $chiakiSet = $true }
        $armName = [string]$run.record.arm
        if (-not $chiakiOf.Contains($armName)) { $chiakiOf[$armName] = @() }
        if (@($chiakiOf[$armName]) -notcontains $value) { $chiakiOf[$armName] = @($chiakiOf[$armName]) + @($value) }
    }
    if ($chiakiSet) {
        $out.Add('  chiaki send buffer (VEYRA_EXP_RP_CHIAKI_SEND_BUFFER from each run''s environment; no app line shows it):')
        foreach ($armName in $chiakiOf.Keys) { $out.Add('    ' + $armName + ': ' + (@($chiakiOf[$armName]) -join ' / ')) }
    }
    $out.Add('')
    $out.Add('STREAM, NETWORK AND PC DURING THE LATENCY RUNS (medians; processing times and rates, not latency)')
    foreach ($entry in $arms) {
        $out.Add('  ' + $entry.name + ': decoded ' + (Format-Number $entry.decodeFps 1) + ' fps, presented ' + (Format-Number $entry.shownFps 1) + ' fps (submitted, not scanout); decode busy ' +
            (Format-Number $entry.decodeBusyMs 2) + ' ms, decode P95 ' + (Format-Number $entry.decodeP95Ms 2) + ' ms; queue depth max ' + (Format-Number $entry.queueDepthMax 0))
        $healthText = 'no network health line (switch VEYRA_EXP_RP_HEALTH not set in this arm)'
        if ($entry.healthIntervals -gt 0) {
            $healthText = 'jitter ' + (Format-Number $entry.jitterMs 2) + ' ms, loss ' + (Format-Number $entry.lossPercent 3) + ' %, reordered ' + $entry.reordered + ', late ' + $entry.late +
                ', keyframe requests ' + $entry.keyframeRequests + ', bottleneck ' + $entry.bottlenecks + ' (' + $entry.healthIntervals + ' intervals of 5 s)'
        }
        $out.Add('      network: ' + $healthText)
        $out.Add('      PC: processor ' + (Format-Number $entry.cpuPercent 0) + ' %, PushFPS GPU 3D ' + (Format-Number $entry.appGpu3dPercent 0) + ' %, whole GPU ' + (Format-Number $entry.gpuPercent 0) +
            ' % (sampled between latency tests only); log: ' + $entry.errors + ' error and ' + $entry.warnings + ' warning lines')
        foreach ($problem in @($entry.firstProblems)) { if ($problem) { $out.Add('        ' + $problem) } }
    }
    $out.Add('')
    $out.Add('OTHER RUNS')
    foreach ($run in $otherRuns) {
        $line = '  ' + $run.name + ': ' + $run.record.stopReason
        $verdictFile = Join-Path $run.folder 'reliability.csv'
        if (Test-Path -LiteralPath $verdictFile) {
            foreach ($row in @(Get-Content -LiteralPath $verdictFile | ConvertFrom-Csv)) { $line += ('; ' + $row.check + ' ' + $row.result) }
        }
        $out.Add($line)
    }
    if ($otherRuns.Count -eq 0) { $out.Add('  none') }
    $out.Add('')
    $out.Add('PHYSICAL MEASUREMENTS (input to photon: camera or light sensor; never mixed with the table above)')
    foreach ($item in $physical) { $out.Add('  ' + $item.file + ': ' + $item.rows + ' row(s); analysed by report.py in its own table') }
    $out.Add('')
    $out.Add('MISSING APP LOG FIELDS (numbers no log line gives today; the line that should exist)')
    foreach ($item in $missingAppFields) { $out.Add('  - ' + $item) }
    $out.Add('')
    $out.Add('WHAT TO SEND')
    $out.Add('  The one zip the kit names at the end (evidence-<date>-<time>.zip, beside this folder). Nothing else is needed.')
    $out.Add('  Filmed presses: fill in camera.csv in this folder, then run-tests.ps1 -PackOnly <this folder> and send the new zip.')
    $out.Add('  Read RESULT.txt first if you like: it holds no names, addresses or ids.')
    $final = $out.ToArray()
    if ($route -eq 'wifi') {
        # Every table and verdict says it (review of 16a2bfec, item 6): the label under every heading that lacks it.
        $labelled = New-Object System.Collections.Generic.List[string]
        for ($i = 0; $i -lt $final.Count; $i++) {
            $labelled.Add($final[$i])
            $isHeading = ($i -gt 0) -and ($final[$i] -cmatch '^[A-Z][A-Z0-9 ,+()/.%-]{5,}')
            $next = ''; if (($i + 1) -lt $final.Count) { $next = $final[$i + 1] }
            if ($isHeading -and ($next.IndexOf($wifiLabel) -lt 0)) { $labelled.Add('  ' + $wifiLabel) }
        }
        $final = $labelled.ToArray()
    }
    Set-Content -LiteralPath (Join-Path $Folder 'RESULT.txt') -Value $final -Encoding ASCII

    # result.json
    $jsonArms = @()
    foreach ($entry in $arms) {
        $jsonArms += [pscustomobject]@{
            name = $entry.name; exe = $entry.exe; compare = $entry.compare; starts = $entry.starts; presses = $entry.pressValues; testsAnswered = $entry.answered; tests = $entry.tests
            median = (Get-JsonNumber $entry.median); medianLow = (Get-JsonNumber $entry.medianLow); medianHigh = (Get-JsonNumber $entry.medianHigh)
            p95 = (Get-JsonNumber $entry.p95); p95Low = (Get-JsonNumber $entry.p95Low); p95High = (Get-JsonNumber $entry.p95High)
            p99 = (Get-JsonNumber $entry.p99); p99Low = (Get-JsonNumber $entry.p99Low); p99High = (Get-JsonNumber $entry.p99High)
            refused = $entry.refused; reasons = @($entry.reasons); verdict = $entry.verdict; keys = $entry.keys
            config = [pscustomobject]@{ profile = $entry.config.profile; codec = $entry.config.codec; bitrateKbps = $entry.config.bitrateKbps; refreshHz = $entry.config.refreshHz; vrr = $entry.config.vrr; frameGeneration = $entry.config.frameGeneration; decoder = $entry.config.decoder }
            decodeFps = (Get-JsonNumber $entry.decodeFps); presentedFps = (Get-JsonNumber $entry.shownFps); decodeBusyMs = (Get-JsonNumber $entry.decodeBusyMs); decodeP95Ms = (Get-JsonNumber $entry.decodeP95Ms)
            jitterMs = (Get-JsonNumber $entry.jitterMs); lossPercent = (Get-JsonNumber $entry.lossPercent); healthIntervals = $entry.healthIntervals; errors = $entry.errors; warnings = $entry.warnings
        }
    }
    $result = [pscustomobject]@{
        kind = 'software latency (built-in test), not input-to-photon'; resultVersion = 2; dryRun = $dryNote; minimumPresses = $resultMinimum
        quantiles = 'linear interpolation between ranks; 95 % intervals from order statistics (ranks floor(np - 1.96 sqrt(np(1-p))) and ceil(np + 1.96 sqrt(np(1-p))) + 1)'
        verdictRule = $resultRule; arms = $jsonArms; setAsideRuns = $setAside; physicalRows = $physical; missingAppLogFields = $missingAppFields
        network = [pscustomobject]@{ route = $route; label = $(if ($route -eq 'wifi') { $wifiLabel } else { '' }) }
        tables = (ConvertTo-JsonTables $tables)
        networkSession = (ConvertTo-JsonTables $networkSession)
        measured = $measuredJson
    }
    Save-Json $result (Join-Path $Folder 'result.json') 9
    return (Join-Path $Folder 'RESULT.txt')
}

# --- checking and packing ----------------------------------------------------------------------------
function Publish-Evidence([string]$Folder) {
    Write-Step 'Checking the evidence and making the zip'
    $withheld = @()
    $names = @()
    foreach ($file in Get-ChildItem -LiteralPath $Folder -Recurse -File) {
        if ($file.FullName -like '*\_withheld\*') { continue }
        $isKitText = ($file.Extension -eq '.json') -or ($file.Extension -eq '.txt') -or ($file.Name -eq 'markers.csv') -or ($file.Name -eq 'camera.csv') -or ($file.Name -eq 'reliability.csv')
        $isTable = (($file.Extension -eq '.csv') -and ($file.Name -like 'presentmon*')) -or ($file.Name -eq 'samples.csv') -or ($file.Name -eq 'sensor.csv') -or ($file.Name -eq 'arms.csv') -or ($file.Name -eq 'measures.csv')
        if ($traceFiles -contains $file.Name) { if (-not (Test-TraceNumbers $file.FullName)) { $withheld += $file }; continue }
        if ($isKitText -and (Test-FileLeaks $file.FullName)) { $withheld += $file }
        elseif ($isTable -and (Test-TableLeaks $file.FullName)) { $withheld += $file }
    }
    if ($withheld.Count -gt 0) {
        $hold = Join-Path $Folder '_withheld'
        New-Item -ItemType Directory -Force -Path $hold | Out-Null
        foreach ($file in $withheld) {
            Write-Host ('  NOT packed (it may hold an address or a private word): ' + $file.Name) -ForegroundColor Yellow
            $names += $file.Name
            Move-Item -LiteralPath $file.FullName -Destination (Join-Path $hold ($file.Directory.Name + '_' + $file.Name)) -Force
        }
    }
    $names += @($script:withheldTraces)
    Set-Content -LiteralPath (Join-Path $Folder 'withheld.txt') -Value (@('files the kit left out of the zip: ' + ($withheld.Count + @($script:withheldTraces).Count)) + $names) -Encoding ASCII
    $zip = $Folder.TrimEnd('\') + '.zip'
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    $items = @(Get-ChildItem -LiteralPath $Folder | Where-Object { ($_.Name -ne '_withheld') -and ($_.Name -ne '_dryrun') } | ForEach-Object { $_.FullName })
    Compress-Archive -LiteralPath $items -DestinationPath $zip -CompressionLevel Optimal
    Write-Host ''
    Write-Host ('Send this file back: ' + $zip) -ForegroundColor Green
    Write-Host ('Size: {0:N1} MB' -f ((Get-Item -LiteralPath $zip).Length / 1MB))
    return $zip
}

# -SelfTestBootstrap <golden.json>: the bootstrap of this script against the numbers report.py gives (checks workflow).
if ($SelfTestBootstrap) {
    $golden = Get-Content -LiteralPath $SelfTestBootstrap -Raw | ConvertFrom-Json
    $got = Get-BootstrapQuantiles ([double[]]@($golden.values)) ([int]$golden.rounds) ([long]$golden.seed)
    $bad = 0
    foreach ($k in $quantileNames) {
        $want = @($golden.$k)
        for ($i = 0; $i -lt 2; $i++) {
            $ok = [math]::Abs([double]$got[$k][$i] - [double]$want[$i]) -lt 0.000001
            if (-not $ok) { $bad++ }
            Write-Host ($k + '[' + $i + '] kit ' + (Format-Number $got[$k][$i] 6) + ' report.py ' + (Format-Number $want[$i] 6) + $(if ($ok) { ' same' } else { ' DIFFERENT' }))
        }
    }
    if ($bad -gt 0) { exit 1 }
    exit 0
}

if ($PackOnly) {
    if (-not (Test-Path -LiteralPath $PackOnly)) { throw "Folder not found: $PackOnly" }
    $packFolder = (Resolve-Path -LiteralPath $PackOnly).Path
    try { [void](Write-ResultFiles $packFolder) } catch { Write-Host ('RESULT.txt could not be written again: ' + $_.Exception.Message) -ForegroundColor Yellow }
    [void](Publish-Evidence $packFolder)
    return
}

# Moves the run Invoke-ArmRun finished last out of the evidence that is analysed (into setaside\), with the reason.
function Set-RunAside([string]$Reason) {
    $last = $script:lastRun
    if (-not $last) { return }
    $last.record.setAside = $true
    $last.record.setAsideReason = $Reason
    Save-Json $last.record (Join-Path $last.folder 'run.json')
    $aside = Join-Path $evidence 'setaside'
    New-Item -ItemType Directory -Force -Path (Join-Path $aside 'runs') | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $aside 'bench') | Out-Null
    if (Test-Path -LiteralPath $last.bench) { Move-Item -LiteralPath $last.bench -Destination (Join-Path (Join-Path $aside 'bench') (Split-Path -Leaf $last.bench)) -Force }
    Move-Item -LiteralPath $last.folder -Destination (Join-Path (Join-Path $aside 'runs') $last.name) -Force
    Write-Host ('  set aside: ' + $last.name) -ForegroundColor Yellow
}

# True when kit.json of the folder says the session ran to its planned end (finished=true). A session stopped with
# q still gets its zip, but stays unfinished, so that Run-First-Test offers to continue it.
function Test-SessionFinished([string]$Folder) {
    try {
        $record = Get-Content -LiteralPath (Join-Path $Folder 'kit.json') -Raw | ConvertFrom-Json
        return [bool]$record.finished
    } catch { return $false }
}

# A break between the parts of a -Full session. True when the tester wants to stop.
function Wait-Break([string]$Next) {
    if ($DryRun) { return $false }
    Write-Host ''
    Write-Host ('BREAK. Next: ' + $Next) -ForegroundColor Cyan
    $answer = Read-Host 'Enter = go on when you are ready, q = stop here (the zip is still made)'
    return ($answer -eq 'q')
}

# --- start -------------------------------------------------------------------------------------------
$elevated = $false
try {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    $elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
} catch { $elevated = $false }
if ($elevated -and (-not $DryRun) -and (-not $AllowElevated)) {
    Write-Host 'This window runs as administrator. The kit does not start PushFPS that way (a network program should run with your normal rights).' -ForegroundColor Red
    Write-Host '  Close it and start the kit (or Run-First-Test.cmd) with a normal double-click. PresentMon asks for its own rights when it starts.' -ForegroundColor Red
    exit 1
}
if (-not $OutRoot) {
    # Not inside the repository or the program folders: your Documents folder.
    $documents = [Environment]::GetFolderPath('MyDocuments')
    if (-not $documents) { $documents = (Get-Location).Path }
    $OutRoot = Join-Path $documents 'slipstream-evidence'
}
if ($Full) {
    $Soak = $true
    $Reliability = $true
    if (-not $PSBoundParameters.ContainsKey('SoakMinutes')) { $SoakMinutes = 10 }
}
$resumed = $false
if ($Resume) {
    $target = $Resume
    if ($Resume -eq 'last') {
        $target = ''
        if (Test-Path -LiteralPath $OutRoot) {
            foreach ($folder in @(Get-ChildItem -LiteralPath $OutRoot -Directory -Filter 'evidence-*' | Sort-Object Name -Descending)) {
                if ((Test-Path -LiteralPath (Join-Path $folder.FullName 'kit.json')) -and (-not (Test-SessionFinished $folder.FullName))) { $target = $folder.FullName; break }
            }
        }
        if (-not $target) { throw ('-Resume last: no unfinished evidence folder (kit.json without finished=true) under ' + $OutRoot) }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $target 'kit.json'))) { throw ('-Resume: not an evidence folder of the kit (no kit.json): ' + $target) }
    $evidence = (Resolve-Path -LiteralPath $target).Path
    $resumed = $true
} else {
    $evidence = Join-Path $OutRoot ('evidence-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $evidence | Out-Null
    $evidence = (Resolve-Path -LiteralPath $evidence).Path
}
Write-Step ('Slipstream hardware test kit ' + $kitVersion)
Write-Host ('Evidence folder: ' + $evidence)
if ($resumed) { Write-Host 'Continuing an unfinished session: runs already in this folder are counted, not repeated.' -ForegroundColor Green }

if ($DryRun) {
    $dry = Join-Path $evidence '_dryrun'
    New-Item -ItemType Directory -Force -Path (Join-Path $dry 'logs') | Out-Null
    $OriginalExe = Join-Path $dry 'PushFPS.exe'
    $ModifiedExe = $OriginalExe
    $ExperimentalExe = $OriginalExe
    $PreviousExe = $OriginalExe
    Set-Content -LiteralPath $OriginalExe -Value 'not a program: stand-in for the dry run' -Encoding ASCII
    # the installer's marker beside it, as a labelled download writes it (RESULT.txt 'Copies:')
    Set-Content -LiteralPath (Join-Path $dry 'Slipstream-INSTALL.txt') -Value @('slot: experimental', 'package: Slipstream-test-package-8A-2026-11-21-1') -Encoding ASCII
    $Rounds = 1
} else {
    $base = Join-Path $env:LOCALAPPDATA 'Programs'
    if (-not $OriginalExe) {
        $guess = Join-Path $base 'PushFPS-original\PushFPS.exe'
        if (Test-Path -LiteralPath $guess) { $OriginalExe = $guess } elseif ((-not $Versus) -and (-not $Session)) { $OriginalExe = (Read-Host 'Path of the ORIGINAL PushFPS.exe (Enter = skip the original arm)').Trim('"', ' ') }
    }
    if (-not $ModifiedExe) {
        $guess = Join-Path $base 'PushFPS-slipstream\PushFPS.exe'
        # The short sessions use the experimental copy only and ask nothing: without a stable copy they go on.
        if (Test-Path -LiteralPath $guess) { $ModifiedExe = $guess } elseif ((@('presentation', 'audio') + $measureSessions) -notcontains $Session) { $ModifiedExe = (Read-Host 'Path of the MODIFIED PushFPS.exe').Trim('"', ' ') }
    }
    # The experimental copy is looked for only when a session uses it (-Versus, -Experimental or -Arms), and a guess
    # that is the modified copy itself (Run-First-Test -Build experimental) is dropped, not an error.
    $experimentalGuessed = $false
    if ((-not $ExperimentalExe) -and ($Versus -or $Experimental -or $Session -or $Full -or ($Arms.Count -gt 0))) {
        $guess = Join-Path $base 'PushFPS-slipstream-exp\PushFPS.exe'
        if (Test-Path -LiteralPath $guess) { $ExperimentalExe = $guess; $experimentalGuessed = $true }
    }
    if ($OriginalExe -and (-not (Test-Path -LiteralPath $OriginalExe))) { throw "Original PushFPS.exe not found: $OriginalExe" }
    $modifiedOptional = (((@('presentation', 'audio') + $measureSessions) -contains $Session) -and (-not $ModifiedExe))
    # Package 7's own experimental build (kit 5, -Session builds): looked for only by that session, never asked for.
    if ((-not $PreviousExe) -and ($Session -eq 'builds')) {
        $guess = Join-Path $base 'PushFPS-slipstream-p7\PushFPS.exe'
        if (Test-Path -LiteralPath $guess) { $PreviousExe = $guess }
    }
    if ($PreviousExe -and (-not (Test-Path -LiteralPath $PreviousExe))) { throw "Previous (Package 7) PushFPS.exe not found: $PreviousExe" }
    if ($PreviousExe) { $PreviousExe = (Resolve-Path -LiteralPath $PreviousExe).Path }
    if ($PreviousExe -and (($PreviousExe -ieq $ExperimentalExe) -or ($PreviousExe -ieq $ModifiedExe))) { throw 'The previous (Package 7) build must be its own folder, not the stable or the experimental copy.' }
    if ((-not $modifiedOptional) -and ((-not $ModifiedExe) -or (-not (Test-Path -LiteralPath $ModifiedExe)))) { throw "Modified PushFPS.exe not found: $ModifiedExe" }
    if ($ExperimentalExe -and (-not (Test-Path -LiteralPath $ExperimentalExe))) { throw "Experimental PushFPS.exe not found: $ExperimentalExe" }
    if ($OriginalExe) { $OriginalExe = (Resolve-Path -LiteralPath $OriginalExe).Path }
    if ($ModifiedExe) { $ModifiedExe = (Resolve-Path -LiteralPath $ModifiedExe).Path }
    if ($ExperimentalExe) { $ExperimentalExe = (Resolve-Path -LiteralPath $ExperimentalExe).Path }
    if ($OriginalExe -and ($OriginalExe -ieq $ModifiedExe)) { throw 'The original and the modified build must be two different folders.' }
    if ($ExperimentalExe -and ($ExperimentalExe -ieq $ModifiedExe)) {
        if ($experimentalGuessed) { $ExperimentalExe = '' }
        else { throw 'The modified (stable) and the experimental build must be two different folders.' }
    }
}

# PresentMon and nvidia-smi. A member of "Performance Log Users" (S-1-5-32-559) may trace without administrator rights.
$script:perfLogUser = $false
try {
    foreach ($group in @([Security.Principal.WindowsIdentity]::GetCurrent().Groups)) { if ($group -and ($group.Value -eq 'S-1-5-32-559')) { $script:perfLogUser = $true } }
} catch { $script:perfLogUser = $false }
$presentMonExe = ''
if ($PresentMon) {
    if (-not (Test-Path -LiteralPath $PresentMon)) { throw "PresentMon not found: $PresentMon" }
    $presentMonExe = (Resolve-Path -LiteralPath $PresentMon).Path
} else {
    # The kit folder first (that is where the procedure says to put the console program), then PATH. The
    # installed PresentMon application also has a service program of a similar name: never that one.
    $candidates = @()
    foreach ($item in @(Get-ChildItem -LiteralPath $kitFolder -Filter 'PresentMon-*-x64.exe' -ErrorAction SilentlyContinue | Sort-Object Name -Descending)) { $candidates += $item.FullName }
    foreach ($item in @(Get-ChildItem -LiteralPath $kitFolder -Filter 'PresentMon*.exe' -ErrorAction SilentlyContinue)) { $candidates += $item.FullName }
    foreach ($item in @(Get-Command -Name 'PresentMon*.exe' -CommandType Application -ErrorAction SilentlyContinue)) { $candidates += $item.Source }
    foreach ($candidate in $candidates) {
        if ((-not $presentMonExe) -and $candidate -and ([System.IO.Path]::GetFileName($candidate) -notmatch 'Service')) { $presentMonExe = $candidate }
    }
}
if (-not $presentMonExe) {
    Write-Host 'PresentMon not found: frame times, dropped frames and the present mode will be missing.' -ForegroundColor Yellow
    Write-Host '  To include them: download PresentMon from its GitHub releases page (GameTechDev/PresentMon), put the'
    Write-Host ('  console program (PresentMon-<version>-x64.exe) into ' + $kitFolder + ' and start the kit again.')
    Write-Host '  Without it the kit uses the program''s own frame lines ([frame-flow], [frame-rate]) instead.'
}
if ($presentMonExe -and (-not $DryRun)) {
    # Only a PresentMon that Intel signed is started with administrator rights.
    $signatureStatus = ''
    $signer = ''
    try {
        $signature = Get-AuthenticodeSignature -FilePath $presentMonExe
        $signatureStatus = [string]$signature.Status
        if ($signature.SignerCertificate) { $signer = [string]$signature.SignerCertificate.Subject }
    } catch { $signatureStatus = 'not checked (' + $_.Exception.Message + ')' }
    if (($signatureStatus -ne 'Valid') -or ($signer -notmatch 'O="?Intel Corporation"?(,|$)')) {
        Write-Host ('PresentMon NOT used: ' + [System.IO.Path]::GetFileName($presentMonExe) + ' has no valid Intel Corporation signature (status ' + $signatureStatus + ').') -ForegroundColor Red
        Write-Host '  Download it again from the GitHub releases page of GameTechDev/PresentMon. The kit goes on without it.'
        $presentMonExe = ''
    } else {
        $missingOptions = Read-PresentMonOptions $presentMonExe
        if ($missingOptions) {
            Write-Host ('PresentMon NOT used: its --help does not list the options the kit needs (' + $missingOptions + '). The kit goes on without it.') -ForegroundColor Yellow
            $presentMonExe = ''
        } elseif ($script:perfLogUser) {
            Write-Host ('PresentMon: ' + [System.IO.Path]::GetFileName($presentMonExe) + ', signed by Intel Corporation. You are in the "Performance Log Users" group: no Windows prompt is needed.')
        } elseif ($elevated) {
            Write-Host ('PresentMon: ' + [System.IO.Path]::GetFileName($presentMonExe) + ', signed by Intel Corporation.')
        } elseif ($PresentMonPrompt -and ($Session -ne 'presentation')) {
            Write-Host ('PresentMon: ' + [System.IO.Path]::GetFileName($presentMonExe) + ', signed by Intel Corporation. Windows asks once per run to let it read frame times (PresentMon only; PushFPS runs with your normal rights).')
            Write-Host '  No prompt at all: an administrator can add your account to the Windows group "Performance Log Users" (then sign out and in). The kit never changes groups itself.'
        } elseif ($Session -eq 'presentation') {
            $presentMonExe = ''      # this session never shows an administrator prompt; the plain line follows below
        } else {
            Write-Host 'PresentMon found but not used: it needs a Windows prompt per run, which the kit shows only when you start it with -PresentMonPrompt (or no prompt for a member of "Performance Log Users").' -ForegroundColor Yellow
            $presentMonExe = ''
        }
    }
}
$nvidiaSmi = ''
$smiCommand = @(Get-Command -Name 'nvidia-smi.exe' -CommandType Application -ErrorAction SilentlyContinue)
if ($smiCommand.Count -gt 0) { $nvidiaSmi = $smiCommand[0].Source }
$gpuFields = 'utilization.gpu,utilization.decoder,utilization.encoder,memory.used,memory.total,temperature.gpu,power.draw'
$gpuColumns = 'gpuPercent,gpuDecoderPercent,gpuEncoderPercent,vramUsedMb,vramTotalMb,gpuTempC,gpuPowerW'
if ($nvidiaSmi -and (-not (Get-GpuSample))) {
    $gpuFields = 'utilization.gpu,memory.used,memory.total'   # older drivers do not know the decoder field
    $gpuColumns = 'gpuPercent,vramUsedMb,vramTotalMb'
    if (-not (Get-GpuSample)) { $nvidiaSmi = '' }
}
if (-not $nvidiaSmi) {
    $gpuColumns = ''
    Write-Host 'nvidia-smi not found or not answering: whole-GPU load and video memory will be missing (the PushFPS process''s own GPU counters are still read).' -ForegroundColor Yellow
}

# Hardware report (the detection script changes nothing on the PC), then the two private fields are removed.
if ((-not $resumed) -or (-not (Test-Path -LiteralPath (Join-Path $evidence 'hardware.json')))) {
    Write-Step 'Hardware and settings'
    $hardwareTemp = Join-Path $env:TEMP ('slipstream-hardware-' + [guid]::NewGuid().ToString('N') + '.json')
    try {
        & (Join-Path (Split-Path -Parent $kitFolder) 'detect-hardware.ps1') -OutFile $hardwareTemp -Ps5Address $Ps5Address -ExpectRefreshHz $ExpectRefreshHz
        $hardware = Get-Content -LiteralPath $hardwareTemp -Raw | ConvertFrom-Json
        $hardware.computer = '(removed)'
        foreach ($adapter in @($hardware.network)) { if ($adapter) { $adapter.name = '(removed)' } }
        Save-Json $hardware (Join-Path $evidence 'hardware.json')
    } catch {
        Write-Host ('Hardware detection failed: ' + $_.Exception.Message) -ForegroundColor Yellow
    } finally {
        Remove-Item -LiteralPath $hardwareTemp -Force -ErrorAction SilentlyContinue
    }
}

# -Clip (stream clarity): only for the measured sessions; the file must exist; kept as an absolute path for the app.
$script:clipPath = ''
if ($Clip) {
    if (-not ($measureSessions -contains $Session)) { throw '-Clip needs a measured session, for example -Session visual' }
    # Without the test profile the clip would go into the user's own preferences (last source, recent files, positions; with
    # automatic resume his next start would open it); without the automation nothing opens it.
    if ($NoTestProfile) { throw '-Clip cannot be used with -NoTestProfile (the clip would be remembered in your own settings)' }
    if ($NoAutomation) { throw '-Clip needs the automation (it opens the clip): leave out -NoAutomation' }
    if (-not (Test-Path -LiteralPath $Clip -PathType Leaf)) { throw '-Clip: the file was not found' }
    $script:clipPath = (Resolve-Path -LiteralPath $Clip).ProviderPath
    Write-Host 'Clip: every run plays the recorded clip instead of connecting to the PS5 (the same frames in every arm).'
}
if ($Session -and ((@('switches', 'quality', 'dedup-probe', 'network', 'presentation', 'audio') + $measureSessions) -notcontains $Session)) { throw "-Session must be switches, quality, dedup-probe, network, presentation, audio, input, visual, neural, framegen, audiofix, pacing, builds, smoothness or capture (not '$Session')" }
$measureSession = $null
if ($measureSessions -contains $Session) {
    $measureSession = $measureSessionTable[$Session]
    $script:measurePlan = [string]$measureSession.plan
    $script:measureCues = @($measureSession.cues)
}
# The short sessions: -Session presentation (window facts of four presentation arms) and -Session audio (one audio pair),
# experimental build only, automatic, $shortSessionRounds rounds of one PS-button test of $shortSessionPresses presses
# per start (Get-ShortSessionMinutes: the same numbers the estimate below uses; START-HERE and the procedure quote them).
$presentationArms = @('p-window', 'p-fullscreen', 'p-lowqueue-off', 'p-test-uncovered')
$audioArms = @('s-base', 's-audio-smooth')
$shortSessionRounds = 2
$shortSessionPresses = 12
if (@('presentation', 'audio') -contains $Session) {
    if (-not $script:roundsGiven) { $Rounds = $shortSessionRounds }
    if (-not $PSBoundParameters.ContainsKey('PressesPerArm')) { $PressesPerArm = $shortSessionPresses * [math]::Max(1, $Rounds) }
    if ($Session -eq 'presentation') {
        if (-not $presentMonExe) { Write-Host 'PresentMon not used: this session then cannot tell whether Windows composes the video window; the app''s own lines are still collected.' -ForegroundColor Yellow }
        else { Write-Host 'PresentMon used, without an administrator prompt in this session.' }
    }
}
if ((Get-NetworkRoute $evidence @()) -eq 'wifi') {
    Write-Host ('Network: Wi-Fi. The session runs; every table and verdict says "' + $wifiLabel + '".') -ForegroundColor Yellow
}
# Which arms (through 'powershell -File' a list arrives as one string: split it)
$armList = @()
foreach ($item in $Arms) { foreach ($part in ($item -split ',')) { if ($part.Trim()) { $armList += $part.Trim() } } }
$Arms = $armList
if ($Camera -and ($Arms.Count -eq 0)) { $Arms = @('original', 'mod-all') }
$table = Read-ArmTable (Join-Path $kitFolder 'arms.csv')
$chosen = @()
$fullNetwork = @()
foreach ($arm in $table) {
    $take = $false
    if ($Arms.Count -gt 0) { $take = ($Arms -contains $arm.name) }
    elseif ($measureSession) { $take = (($arm.set -eq $Session) -or (@($measureSession.also) -contains $arm.name)) }
    elseif ($Session) { $take = ((($Session -eq 'switches') -and ($arm.set -eq 'switches')) -or (($Session -eq 'quality') -and ($arm.set -eq 'quality')) -or (($Session -eq 'dedup-probe') -and ($arm.set -eq 'probe')) -or (($Session -eq 'presentation') -and ($presentationArms -contains $arm.name)) -or (($Session -eq 'audio') -and ($audioArms -contains $arm.name)) -or (($Session -eq 'network') -and (($arm.set -eq 'network') -or (($arm.set -eq 'network-builds') -and $NetworkWithBuilds) -or (($arm.set -eq 'network-wifi-mode') -and $WifiStreamingMode)))) }
    elseif ($Versus) { $take = ($arm.set -eq 'versus') }
    elseif ($arm.set -eq 'quick') { $take = $true }
    elseif ($arm.set -eq 'standard') { $take = (-not $Quick) }
    elseif ($arm.set -eq 'experimental') { $take = [bool]$Experimental }
    if ((-not $take) -and $Full -and (-not $Session) -and ($arm.set -eq 'network') -and $ExperimentalExe) { $fullNetwork += $arm }
    if ($take -and ($arm.exe -eq 'original') -and (-not $OriginalExe)) { Write-Host ("Arm '{0}' skipped: no original build given." -f $arm.name) -ForegroundColor Yellow; $take = $false }
    if ($take -and ($arm.exe -eq 'modified') -and (-not $ModifiedExe)) { Write-Host ("Arm '{0}' skipped: the stable copy is not installed (-ModifiedExe)." -f $arm.name) -ForegroundColor Yellow; $take = $false }
    if ($take -and ($arm.exe -eq 'previous') -and (-not $PreviousExe)) { Write-Host ("Arm '{0}' skipped: Package 7's experimental build is not installed as a fourth copy (-PreviousExe; Install-Slipstream.cmd of Package 8 installs it)." -f $arm.name) -ForegroundColor Yellow; $take = $false }
    if ($take -and ($arm.exe -eq 'experimental') -and (-not $ExperimentalExe)) { Write-Host ("Arm '{0}' skipped: the experimental build is not installed (-ExperimentalExe)." -f $arm.name) -ForegroundColor Yellow; $take = $false }
    if ($take) { $chosen += $arm }
}
foreach ($name in $Arms) { if (-not ($table | Where-Object { $_.name -eq $name })) { throw "Unknown arm '$name' (see arms.csv)" } }
# -Clip plays a file: an arm that asks the PS5 for a bitrate or codec (the c-* arms) has nothing to ask it then.
if ($script:clipPath) { foreach ($arm in $chosen) { if ([string]$arm.switches -match 'VEYRA_TEST_RP_(BITRATE_KBPS|CODEC)=') { throw ("-Clip cannot run arm '" + $arm.name + "': it asks the PS5 for its own bitrate or codec (run it without -Clip)") } } }

# Which Slipstream download installed this copy (8A, 8B, 8C, 8; Package 7 had no label): the 'package:' line of the
# Slipstream-INSTALL.txt the installer writes beside PushFPS.exe. Only the label is kept, never a path.
function Get-PackageLabel([string]$ExePath) {
    $file = Join-Path (Split-Path -Parent $ExePath) 'Slipstream-INSTALL.txt'
    if (-not (Test-Path -LiteralPath $file)) { return 'not installed by a Slipstream package' }
    $name = ''; $label = ''
    foreach ($line in @(Get-Content -LiteralPath $file)) {
        if ([string]$line -match '^package:\s*(\S+)') { $name = [string]$Matches[1] }
        if ([string]$line -match '^label:\s*([0-9][0-9A-Za-z]{0,7})\s*$') { $label = [string]$Matches[1] }
    }
    if ($label) { return ('package ' + $label) }
    if ($name -match '^Slipstream-test-package-([0-9][0-9A-Za-z]{0,7})-\d{4}-\d{2}-\d{2}') { return ('package ' + $Matches[1]) }
    if ($name -match '^Slipstream-test-package-\d{4}-\d{2}-\d{2}') { return 'package 7 or earlier (no label)' }
    return 'package unknown'
}

function Get-ExeFacts([string]$Path) {
    if (-not $Path) { return $null }
    $package = 'package unknown'
    try { $package = Get-PackageLabel $Path } catch { $package = 'package unknown' }
    $item = Get-Item -LiteralPath $Path
    $version = ''
    try { $version = [string]$item.VersionInfo.ProductVersion } catch { $version = '' }
    # Dots become underscores: a four-part version looks like an address to the masking. 10 digits of the hash identify the build (12 would look like a MAC address to the masking).
    return [pscustomobject]@{ file = $item.Name; bytes = $item.Length; productVersion = ($version -replace '\.', '_'); sha256First10 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.Substring(0, 10); package = $package }
}

# What the folder already holds (an interrupted session that is continued).
$keptAtStart = @(Read-KeptRuns $evidence)
$runCount = @{}
$pressCount = @{}
foreach ($arm in $chosen) { $runCount[$arm.name] = 0; $pressCount[$arm.name] = 0 }
$donePurposes = @()
foreach ($run in $keptAtStart) {
    $record = $run.record
    if ($donePurposes -notcontains [string]$record.purpose) { $donePurposes += [string]$record.purpose }
    if (($record.purpose -eq 'latency') -and $runCount.ContainsKey([string]$record.arm)) {
        $runCount[[string]$record.arm]++
        $got = [int]$record.presses
        if (($got -eq 0) -and ($record.pressesAnsweredInSummaries)) { $got = [int]$record.pressesAnsweredInSummaries }
        $pressCount[[string]$record.arm] += $got
    }
    if (($record.purpose -eq 'latency') -and (-not $script:referenceConfig) -and $record.config -and $record.config.profile) { $script:referenceConfig = $record.config; $script:referenceArm = [string]$record.arm }
    if (($record.purpose -eq 'latency') -and $record.config -and $record.config.profile -and (-not $script:armReference.ContainsKey([string]$record.arm))) { $script:armReference[[string]$record.arm] = $record.config }
}

# The scene: typed once, kept in every record.
$previousKit = $null
if ($resumed) { try { $previousKit = Get-Content -LiteralPath (Join-Path $evidence 'kit.json') -Raw | ConvertFrom-Json } catch { $previousKit = $null } }
if ((-not $SceneNote) -and $previousKit -and $previousKit.sceneNote) { $SceneNote = [string]$previousKit.sceneNote }
# The short sessions ask nothing (they are started and left alone): a fixed text stands for the scene.
if ((-not $SceneNote) -and (@('presentation', 'audio') -contains $Session)) { $SceneNote = 'short session, scene not asked' }
if ((-not $SceneNote) -and $measureSession) { $SceneNote = 'session ' + $Session + ', scene not asked' }
if ((-not $SceneNote) -and (-not $DryRun) -and ((-not $SkipLatency) -or $Camera)) {
    Write-Host ''
    Write-Host 'Where in the game will every run be made? A few words, typed once (no names), e.g. "harbour pier, noon, camera north".'
    $SceneNote = Read-Host 'Scene'
}
if ($DryRun -and (-not $SceneNote)) { $SceneNote = 'dry run, no scene' }
$SceneNote = Protect-Note $SceneNote

$latencyArms = @($chosen)
$allExperimental = ($latencyArms.Count -gt 0) -and (@($latencyArms | Where-Object { $_.exe -ne 'experimental' }).Count -eq 0)
if ($allExperimental -and ($Session -ne 'network')) {
    $script:automationAllowed = $true
    $script:sessionTrials = [int][math]::Min(100, [math]::Max(12, [math]::Ceiling($PressesPerArm / [math]::Max(1, $Rounds))))
}
# Time estimate (kit 5): from measured run durations, see Get-PlanSeconds; every figure is said with 'about'.
$latencyStarts = 0
if ($Session -and (@('switches', 'presentation', 'audio') -notcontains $Session)) { $SkipLatency = $true }
if ((-not $SkipLatency) -and (-not $Camera)) { foreach ($arm in $chosen) { $left = $Rounds - $runCount[$arm.name]; if ($left -gt 0) { $latencyStarts += $left } } }
$plannedTests = Get-TestsPerStart $script:sessionTrials
# Kit 4 said 4 times too much (32 minutes for a session that took 8). Measured on 20 Nov: 12 automatic starts of
# 'settle:8;latency:12;exit' took 53 to 75 s (median 54 s). Get-PlanSeconds holds those numbers; a start done by hand
# (a session with the original or the stable build) is given 60 s more for the clicks, which nothing has measured.
$perStartSeconds = Get-PlanSeconds (Get-AutorunPlan 'latency' $script:sessionTrials $plannedTests)
if (-not ($allExperimental -and ($Session -ne 'network') -and (-not $NoAutomation))) { $perStartSeconds += 60.0 }
$estimate = 0
$parts = @()
if ($latencyStarts -gt 0) { $estimate = Get-AboutMinutes ($perStartSeconds * $latencyStarts); $parts += ('latency ' + $latencyStarts + ' starts, about ' + $estimate + ' min') }
$measureRounds = 2
if ($measureSession) {
    $measureRounds = [int]$measureSession.rounds
    if ($script:roundsGiven) { $measureRounds = [math]::Max(1, $Rounds) }
    $measureSeconds = 0.0
    foreach ($arm in $chosen) {
        $measureSeconds += (Get-PlanSeconds $script:measurePlan) * $measureRounds
        if (@('experimental', 'previous') -notcontains $arm.exe) { $measureSeconds += 60.0 * $measureRounds }
    }
    $estimate += Get-AboutMinutes $measureSeconds
    $parts += ('session ' + $Session + ': ' + $chosen.Count + ' arms x ' + $measureRounds + ' runs, about ' + (Get-AboutMinutes $measureSeconds) + ' min')
}
if ($Session -eq 'quality') { $estimate += 5; $parts += 'super resolution, frame generation and quality test about 5 min' }
if ($Session -eq 'dedup-probe') { $estimate += 5; $parts += 'repeated-picture diagnostic about 5 min (a game running at 30 FPS)' }
# The network session: every arm $networkRounds times (2 unless -Rounds), each run -Minutes plus about 2 minutes to start and close.
$networkRounds = 2
if ($script:roundsGiven) { $networkRounds = [math]::Max(1, $Rounds) }
if ($Session -eq 'network') { $estimate += $chosen.Count * $networkRounds * ($Minutes + 2); $parts += ('network session ' + $chosen.Count + ' arms x ' + $networkRounds + ' runs of ' + $Minutes + ' min (play normally)') }
if ($fullNetwork.Count -gt 0) { $estimate += $fullNetwork.Count * $networkRounds * ($Minutes + 2); $parts += ('network session ' + $fullNetwork.Count + ' arms x ' + $networkRounds + ' runs of ' + $Minutes + ' min') }
if ($latencyStarts -gt 0) {
    Write-Host ('Presses: ' + $plannedTests + ' latency test(s) of ' + $script:sessionTrials + ' presses per start, the same in every arm (' + $Rounds + ' rounds: ' + ($plannedTests * $script:sessionTrials * $Rounds) + ' presses per arm; wanted ' + $PressesPerArm + ').') -ForegroundColor Cyan
    if (-not $allExperimental) { Write-Host '  The original and the stable build accept at most 12 presses per test, so every arm gets 12.' }
}
$soakTotalMinutes = $SoakMinutes
if ($SoakHours -gt 0) { $soakTotalMinutes = [int][math]::Round($SoakHours * 60) }
if ($Soak -and ($donePurposes -notcontains 'soak')) { $estimate += $soakTotalMinutes + 3; $parts += ('stability ' + $soakTotalMinutes + ' min') }
if ($Impairment -and ($donePurposes -notcontains 'impairment')) { $estimate += 8; $parts += 'cable and network recovery about 8 min' }
if ($Reliability -and ($donePurposes -notcontains 'reliability')) { $estimate += 15; $parts += 'reliability checks about 15 min' }
if ($Full) { $estimate += 5; $parts += 'breaks' }
if ($estimate -gt 0) { Write-Host ('Estimated time: about ' + $estimate + ' minutes (' + ($parts -join ', ') + ').') -ForegroundColor Cyan }

if ($Pass -gt 0) {
    if (@('smoothness', 'capture') -notcontains $Session) { throw '-Pass is only for -Session smoothness or capture' }
    if (@(120, 180) -notcontains $Pass) { throw '-Pass must be 120 or 180' }
    Write-Host ''
    Write-Host ('>>> PASS ' + $Pass + ' Hz: set Windows to ' + $Pass + ' Hz NOW: Settings > System > Display > Advanced display > Choose a refresh rate.') -ForegroundColor Cyan
    Write-Host ('>>> A run that the app reports at another refresh rate is not used: the kit offers to set it aside and repeat it.') -ForegroundColor Cyan
    if (-not $DryRun) { [void](Read-Host ('>>> Press Enter when Windows runs at ' + $Pass + ' Hz')) }
}

$inherited = Clear-KitSwitches
$armNames = @()
foreach ($arm in $chosen) { $armNames += $arm.name }
if ($resumed) {
    Add-Content -LiteralPath (Join-Path $evidence 'resumed.txt') -Value ((Get-UtcStamp) + ' continued with arms ' + ($armNames -join ' ')) -Encoding ASCII
} else {
    $kit = [pscustomobject]@{
        kitVersion = $kitVersion; startedUtc = (Get-UtcStamp); dryRun = [bool]$DryRun
        powershell = $PSVersionTable.PSVersion.ToString(); elevated = $elevated
        presentMon = [bool]$presentMonExe; presentMonFile = [System.IO.Path]::GetFileName($presentMonExe); nvidiaSmi = [bool]$nvidiaSmi
        arms = $armNames; rounds = $Rounds; pressesPerArm = $PressesPerArm; testsPerStart = $TestsPerStart; autoClose = (-not $NoAutoClose)
        soak = [bool]$Soak; soakMinutes = $SoakMinutes; soakHours = $SoakHours; impairment = [bool]$Impairment; health = [bool]$Health
        reliability = [bool]$Reliability; reliabilityKbm = [bool]$ReliabilityKbm; reliabilityGpuLoss = [bool]$ReliabilityGpuLoss
        versus = [bool]$Versus; full = [bool]$Full; sceneNote = $SceneNote; estimatedMinutes = $estimate
        session = $Session; pass = $Pass; testProfile = $script:useTestProfile; automation = (-not $NoAutomation); testsPerStartPlanned = $plannedTests
        original = (Get-ExeFacts $OriginalExe); modified = (Get-ExeFacts $ModifiedExe); experimental = (Get-ExeFacts $ExperimentalExe); previous = (Get-ExeFacts $PreviousExe)
        switchesFoundInEnvironmentAndCleared = $inherited
    }
    Save-Json $kit (Join-Path $evidence 'kit.json')
}
Copy-Item -LiteralPath (Join-Path $kitFolder 'arms.csv') -Destination (Join-Path $evidence 'arms.csv') -Force
if (Test-Path -LiteralPath (Join-Path $kitFolder 'measures.csv')) { Copy-Item -LiteralPath (Join-Path $kitFolder 'measures.csv') -Destination (Join-Path $evidence 'measures.csv') -Force }
# camera.csv may already hold filmed presses in a continued session: never overwrite it.
if (-not (Test-Path -LiteralPath (Join-Path $evidence 'camera.csv'))) { Copy-Item -LiteralPath (Join-Path $kitFolder 'camera-template.csv') -Destination (Join-Path $evidence 'camera.csv') }
if (-not (Test-Path -LiteralPath (Join-Path $evidence 'sensor.csv'))) { Copy-Item -LiteralPath (Join-Path $kitFolder 'sensor-template.csv') -Destination (Join-Path $evidence 'sensor.csv') -ErrorAction SilentlyContinue }

function Get-ArmExe($Arm) {
    if ($Arm.exe -eq 'original') { return $OriginalExe }
    if ($Arm.exe -eq 'experimental') { return $ExperimentalExe }
    if ($Arm.exe -eq 'previous') { return $PreviousExe }
    return $ModifiedExe
}

# --- latency arms, interleaved ---------------------------------------------------------------------
$stop = $false
$profileRepeats = @{}
if ($Camera) {
    Write-Step ('Camera session: ' + ($armNames -join ', '))
    foreach ($arm in $chosen) {
        [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'camera' 0 "Camera: arm '$($arm.name)'")
        if (-not $DryRun) {
            $answer = Read-Host 'Enter = next arm, q = stop here (the zip is still made)'
            if ($answer -eq 'q') { $stop = $true; break }
        }
    }
    $SkipLatency = $true
}
# The network session: every chosen arm for -Minutes, in an order that starts at a random arm, reversed every other round.
function Invoke-NetworkSession($NetworkArms) {
    $rounds = $networkRounds
    $count = @($NetworkArms).Count
    if ($count -eq 0) { return }
    $start = (New-Object System.Random).Next($count)
    if ($DryRun) { $start = 0 }
    $limit = [math]::Max(1, $Minutes) * 60 + 120
    if ($DryRun) { $limit = 3 }
    Write-Step ('Network session: ' + $count + ' arms, ' + $rounds + ' runs each of ' + $Minutes + ' minutes, interleaved (A B B A)')
    Write-Host 'Play normally during the whole session: the stream must show real motion. A still menu gives a result that only speaks for a still menu.' -ForegroundColor Green
    for ($round = 1; $round -le $rounds; $round++) {
        $order = @()
        for ($i = 0; $i -lt $count; $i++) { $order += @($NetworkArms)[($start + $i) % $count] }
        if (($round % 2) -eq 0) { [Array]::Reverse($order) }
        foreach ($arm in $order) { [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'network' $limit ("Network session, round " + $round + ": arm '" + $arm.name + "'")) }
    }
}

# A measured session of kit 5: every chosen arm once per round, A B C then C B A (slow drift of the console or the network
# cancels), each one automatic start of the session's plan. Runs already in the folder (a continued session) are counted.
# Stream capture: the capture session starts the capture card remembered in the kit's own capture settings folder
# (VEYRA_TEST_RP_CAPTURE). The first time there is none: PushFPS is started once with that folder and no automation, and you
# choose the card and its mode yourself; the kit goes on when PushFPS is closed. Your own settings are never changed.
function Invoke-CaptureSetup($Arm) {
    if (-not $script:useTestProfile) { Write-Host '  -NoTestProfile: the capture card remembered in your own settings is started.' -ForegroundColor Yellow; return }
    $folder = Write-TestProfile ([string]$Arm.exe + '-capture') $false (Get-ArmPrefs $Arm)
    $remembered = Join-Path $folder 'capture-preferences.v1'
    if (Test-Path -LiteralPath $remembered) { Write-Host '  Capture card: the one chosen before in the kit''s capture settings folder.'; return }
    if ($DryRun) { Write-Host '  (dry run: the kit would now ask you to choose the capture card once)' -ForegroundColor Yellow; return }
    $exe = Get-ArmExe $Arm
    Write-Host ''
    Write-Host '>>> ONCE: choose your capture card. PushFPS starts now with the kit''s own capture settings folder (your settings are untouched):' -ForegroundColor Cyan
    Write-Host '>>>   open Capture, choose the card and the mode the PS5 sends (for example 1920x1080 or 2560x1440 at 60; NV12, YUY2 or P010 rather than MJPEG),' -ForegroundColor Cyan
    Write-Host '>>>   start it, check that the game picture is there, then close PushFPS. The kit goes on by itself.' -ForegroundColor Cyan
    $setup = Start-Process -FilePath $exe -ArgumentList ('--data-dir "' + $folder + '"') -WorkingDirectory (Split-Path -Parent $exe) -PassThru
    $setup.WaitForExit()
    if (-not (Test-Path -LiteralPath $remembered)) { throw 'No capture card was started in the kit''s capture settings folder: run the capture session again and start the card once when the kit asks.' }
    Write-Host '  Capture card remembered for this session.' -ForegroundColor Green
}

function Invoke-MeasureSession($MeasureArms) {
    $count = @($MeasureArms).Count
    if ($count -eq 0) { return }
    $have = @{}
    foreach ($run in $keptAtStart) { if ($run.record.purpose -eq 'measure') { $name = [string]$run.record.arm; if ($have.ContainsKey($name)) { $have[$name]++ } else { $have[$name] = 1 } } }
    Write-Step ('Session ' + $Session + ': ' + $count + ' arms, ' + $measureRounds + ' runs each, interleaved (A B C, then C B A); plan ' + $script:measurePlan)
    foreach ($line in @($measureSession.intro)) { Write-Host $line -ForegroundColor Green }
    if ($Session -eq 'capture') { Invoke-CaptureSetup @($MeasureArms)[0] }
    if ((Get-NetworkRoute $evidence @()) -eq 'wifi') { Write-Host ('Network: Wi-Fi. ' + $wifiLabel + '.') -ForegroundColor Yellow }
    $total = $count * $measureRounds
    $started = 0
    for ($round = 1; $round -le $measureRounds; $round++) {
        $order = @()
        foreach ($arm in @($MeasureArms)) { $order += $arm }
        if (($round % 2) -eq 0) { [Array]::Reverse($order) }
        foreach ($arm in $order) {
            $started++
            if ($have.ContainsKey([string]$arm.name) -and ($have[[string]$arm.name] -ge $round)) { continue }
            $seconds = Get-PlanSeconds $script:measurePlan
            # A build without the automation is closed by the kit after the same time.
            $limit = 0
            if ((@('experimental', 'previous') -notcontains $arm.exe) -or $NoAutomation) { $limit = [int]$seconds + 60 }
            if ($DryRun -and ($limit -gt 0)) { $limit = 3 }
            $title = 'Session ' + $Session + ', start ' + $started + ' of ' + $total + ", arm '" + $arm.name + "', about " + (Get-AboutMinutes ($seconds * ($total - $started + 1))) + ' min left'
            $tries = 0
            while ($true) {
                $script:lastRun = $null   # a run that fails before it records itself must not be judged by the previous run's record
                [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'measure' $limit $title)
                $tries++
                # -Pass (stream fps): a run at another refresh rate than the pass asks for is not used.
                $refreshProblem = @()
                if (($Pass -gt 0) -and $script:lastRun) { $refreshProblem = @(@($script:lastRun.record.pairProblems) | Where-Object { [string]$_ -match 'this pass is for' }) }
                if ($refreshProblem.Count -eq 0) { break }
                Write-Host ('  >>> ' + [string]$refreshProblem[0]) -ForegroundColor Red
                if ($DryRun) { Write-Host '  (dry run: kept)' -ForegroundColor Yellow; break }
                Set-RunAside ('refresh rate: ' + [string]$refreshProblem[0])
                if ($tries -ge 2) { Write-Host '  >>> The run is set aside; the session goes on without it.' -ForegroundColor Yellow; break }
                [void](Read-Host ('  >>> Set Windows to ' + $Pass + ' Hz, then press Enter to repeat this run'))
            }
        }
    }
}

if (($Session -eq 'network') -and ($chosen.Count -gt 0)) { Invoke-NetworkSession $chosen }
elseif ($measureSession) { Invoke-MeasureSession $chosen }
elseif ($Session -and (@('switches', 'presentation', 'audio') -notcontains $Session) -and ($chosen.Count -gt 0)) {
    $purpose = 'quality'
    if ($Session -eq 'dedup-probe') { $purpose = 'probe'; Write-Host 'OPTIONAL diagnostic: needs a game running at 30 FPS. Never compared for latency.' -ForegroundColor Cyan }
    foreach ($arm in $chosen) { [void](Invoke-ArmRun $arm (Get-ArmExe $arm) $purpose 0 ("Session " + $Session + ": arm '" + $arm.name + "'")) }
}
if ((-not $SkipLatency) -and ($chosen.Count -gt 0)) {
    Write-Step ('Latency arms: ' + ($armNames -join ', '))
    Write-Host 'Same game, same place, same in-game settings in every run. Nothing else using the PC.'
    if ((Get-NetworkRoute $evidence @()) -eq 'wifi') { Write-Host ('Network: Wi-Fi. ' + $wifiLabel + '.') -ForegroundColor Yellow }
    if ($SceneNote) { Write-Host ('Scene: ' + $SceneNote) }
    $round = 1
    $startsDone = 0
    while (-not $stop) {
        $order = @()   # A B C, then C B A: slow drift of the console or the network cancels
        if (($round % 2) -eq 0) { for ($i = $chosen.Count - 1; $i -ge 0; $i--) { $order += $chosen[$i] } }
        else { foreach ($item in $chosen) { $order += $item } }
        foreach ($arm in $order) {
            if ($runCount[$arm.name] -ge $round) { continue }      # already in the folder (a continued session)
            if (($round -gt $Rounds) -and ($pressCount[$arm.name] -ge $PressesPerArm)) { continue }
            while ($true) {
                $startsDone++
                $left = $latencyStarts - $startsDone
                $title = "Start $startsDone of $latencyStarts, round $round, arm '$($arm.name)' ($($pressCount[$arm.name]) of $PressesPerArm presses so far)"
                if ($left -ge 0) { $title += (', about ' + (Get-AboutMinutes ($perStartSeconds * ($left + 1))) + ' min left') }
                $got = Invoke-ArmRun $arm (Get-ArmExe $arm) 'latency' 0 $title
                $problems = @()
                if ($script:lastRun) { $problems = @($script:lastRun.record.pairProblems) }
                if ($problems.Count -eq 0) { $pressCount[$arm.name] += $got; $runCount[$arm.name]++; break }
                Write-Host '  This run does not match what the comparison needs:' -ForegroundColor Red
                foreach ($problem in $problems) { Write-Host ('    - ' + $problem) -ForegroundColor Red }
                if (@('presentation', 'audio') -contains $Session) {
                    # The short sessions run without a question (they give measured values, no verdict word).
                    Write-Host '  Short session: the run is kept and the session goes on; RESULT.txt names the difference.' -ForegroundColor Yellow
                    $pressCount[$arm.name] += $got; $runCount[$arm.name]++; break
                }
                $profileProblem = @($problems | Where-Object { $_ -match 'test profile did not take effect' }).Count -gt 0
                if ($profileProblem -and $profileRepeats.ContainsKey($arm.name)) {
                    # Never a loop: the same profile would give the same result again.
                    Write-Host '  The kit''s settings did not take effect in this copy twice. The run is kept, its comparison will be refused with the reason, and the session goes on. Send the zip as it is.' -ForegroundColor Yellow
                    $pressCount[$arm.name] += $got; $runCount[$arm.name]++; break
                }
                if ($profileProblem) { $profileRepeats[$arm.name] = $true }
                if ($DryRun) {
                    Write-Host '  (dry run: kept; RESULT.txt and the report must refuse this arm)' -ForegroundColor Yellow
                    $pressCount[$arm.name] += $got; $runCount[$arm.name]++; break
                }
                $answer = Read-Host '  Enter = set this run aside and repeat it (fix the setting in PushFPS first), k = keep it (its arm will be refused), q = stop'
                if ($answer -eq 'k') { $pressCount[$arm.name] += $got; $runCount[$arm.name]++; break }
                Set-RunAside ('settings differ: ' + ($problems -join '; '))
                if ($answer -eq 'q') { $stop = $true; break }
                $latencyStarts++
            }
            if ((-not $script:referenceConfig) -and $script:lastRun -and (-not $script:lastRun.record.setAside) -and $script:lastRun.record.config.profile) {
                $script:referenceConfig = $script:lastRun.record.config
                $script:referenceArm = [string]$arm.name
            }
            if ($script:lastRun -and (-not $script:lastRun.record.setAside) -and $script:lastRun.record.config.profile -and (-not $script:armReference.ContainsKey([string]$arm.name))) {
                $script:armReference[[string]$arm.name] = $script:lastRun.record.config
            }
            if ($stop) { break }
            if ((-not $DryRun) -and (@('presentation', 'audio') -notcontains $Session)) {
                $answer = Read-Host 'Enter = next run, q = stop here (the zip is still made; Run-First-Test can continue this session later)'
                if ($answer -eq 'q') { $stop = $true; break }
            }
        }
        if ($stop) { break }
        $round++
        if ($round -gt $Rounds) {
            $short = @()
            foreach ($arm in $chosen) { if ($pressCount[$arm.name] -lt $PressesPerArm) { $short += ("{0} ({1})" -f $arm.name, $pressCount[$arm.name]) } }
            if ($short.Count -eq 0) { break }
            if ($DryRun) { break }
            if (@('presentation', 'audio') -contains $Session) {
                # The short sessions end here whatever was answered: no question, RESULT.txt gives the press counts.
                Write-Host ('Below ' + $PressesPerArm + ' presses: ' + ($short -join ', ') + '. The short session ends here; RESULT.txt gives the counts.') -ForegroundColor Yellow
                break
            }
            Write-Host ('Below ' + $PressesPerArm + ' presses: ' + ($short -join ', ')) -ForegroundColor Yellow
            $answer = Read-Host 'Enter = one more round for these arms, q = finish with what there is'
            if ($answer -eq 'q') { break }
            $latencyStarts += $short.Count
        }
    }
}

if ($Full -and (-not $stop) -and ($fullNetwork.Count -gt 0) -and ($donePurposes -notcontains 'network')) {
    $stop = Wait-Break ('the network session: ' + $fullNetwork.Count + ' arms of the experimental build, ' + $Minutes + ' minutes each; play normally')
    if (-not $stop) { Invoke-NetworkSession $fullNetwork }
}

# --- soak and impairment ---------------------------------------------------------------------------
if ($Soak -and (-not $stop) -and ($donePurposes -notcontains 'soak')) {
    $arm = $table | Where-Object { $_.name -eq $SoakArm } | Select-Object -First 1
    if (-not $arm) { throw "Unknown soak arm '$SoakArm'" }
    if ($soakTotalMinutes -lt 1) { throw '-SoakMinutes / -SoakHours: the soak must last at least one minute' }
    if ($Full) { $stop = Wait-Break ("a " + $soakTotalMinutes + "-minute stability run of arm '" + $arm.name + "': connect, play or leave the game running") }
    if (-not $stop) {
        $seconds = $soakTotalMinutes * 60
        if ($DryRun) { $seconds = 3 }
        [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'soak' $seconds "Soak: arm '$($arm.name)' for $soakTotalMinutes minutes")
    }
}
if ($Impairment -and (-not $stop) -and ($donePurposes -notcontains 'impairment')) {
    $arm = $table | Where-Object { $_.name -eq $ImpairmentArm } | Select-Object -First 1
    if (-not $arm) { throw "Unknown impairment arm '$ImpairmentArm'" }
    [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'impairment' 0 "Impairment and recovery: arm '$($arm.name)'")
}

if ($Reliability -and (-not $stop) -and ($donePurposes -notcontains 'reliability')) {
    $arm = $table | Where-Object { $_.name -eq $ReliabilityArm } | Select-Object -First 1
    if (-not $arm) { throw "Unknown reliability arm '$ReliabilityArm'" }
    if ($Full) { $stop = Wait-Break 'the reliability checks (cable out, display change, crash recovery): about 15 minutes, each step starts with Enter' }
    if (-not $stop) {
        Write-Step 'Reliability checks'
        Write-Host 'Each step starts when you press Enter in THIS window and ends by itself; the kit beeps and names the next one.'
        Write-Host 'Steps you cannot do (a single monitor, no keyboard and mouse mode): press Enter and wait; they are reported as not exercised.'
        [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'reliability' 0 "Reliability: arm '$($arm.name)'")
        if ($ReliabilityGpuLoss) {
            [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'gpuloss' 0 "Reliability: loss of the graphics device, arm '$($arm.name)' (close PushFPS when the step has ended)")
        }
        # A start with nothing set: no switch, no slipstream.ini. What a tester falls back to when a setting broke something.
        $safe = [pscustomobject]@{ name = 'safe-start'; exe = 'modified'; set = 'reliability'; compare = ''; switches = ''; note = 'PushFPS with default settings: no switch and slipstream.ini not read.'; fg = '' }
        $safeSeconds = 20
        if ($DryRun) { $safeSeconds = 3 }
        [void](Invoke-ArmRun $safe $ModifiedExe 'safestart' $safeSeconds 'Reliability: safe start')
    }
}

if ($DryRun) { Remove-Item -LiteralPath (Join-Path $evidence '_dryrun') -Recurse -Force -ErrorAction SilentlyContinue }
if (-not $stop) {
    try {
        $kitRecord = Get-Content -LiteralPath (Join-Path $evidence 'kit.json') -Raw | ConvertFrom-Json
        $kitRecord | Add-Member -NotePropertyName finished -NotePropertyValue $true -Force
        $kitRecord | Add-Member -NotePropertyName finishedUtc -NotePropertyValue (Get-UtcStamp) -Force
        Save-Json $kitRecord (Join-Path $evidence 'kit.json')
    } catch { Write-Host ('kit.json could not be marked finished: ' + $_.Exception.Message) -ForegroundColor Yellow }
} else {
    Write-Host 'Stopped before the end: the zip below holds what was recorded; Run-First-Test offers to continue this session.' -ForegroundColor Yellow
}
$resultFile = ''
try { $resultFile = Write-ResultFiles $evidence } catch { Write-Host ('RESULT.txt could not be written: ' + $_.Exception.Message + ' (script line ' + $_.InvocationInfo.ScriptLineNumber + ')') -ForegroundColor Yellow }
$zipPath = Publish-Evidence $evidence
if ($resultFile -and (Test-Path -LiteralPath $resultFile)) {
    Write-Step 'Result (RESULT.txt, also in the zip)'
    foreach ($line in @(Get-Content -LiteralPath $resultFile)) { Write-Host $line }
}
Write-Host ''
Write-Host ('Send this one file: ' + $zipPath) -ForegroundColor Green
if ((-not $DryRun) -and (Test-Path -LiteralPath (Join-Path $env:LOCALAPPDATA 'slipstream-kit'))) {
    Write-Host ('The kit''s settings folders for the copies are in ' + (Join-Path $env:LOCALAPPDATA 'slipstream-kit') + ': presentation settings and the last console to reconnect to, no pairing and no keys (the pairing stays with Windows). Delete that folder whenever you like; the next session makes it again.')
}
Write-Host 'Camera measurements: fill in camera.csv in the evidence folder, then run this script with -PackOnly <that folder>.'
Write-Host ('Report (needs Python 3): python tools\remoteplay_latency_bench\report.py "' + $evidence + '"')
