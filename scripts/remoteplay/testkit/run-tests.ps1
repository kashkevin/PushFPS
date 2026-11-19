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
        [push-latency] [push-quality] [push-benchmark] [push-mode] [push-hardware]
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
  Rounds of all arms (default 3). Run the 20-press latency test twice per start: 3 rounds give 120 presses.
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
  settings differ from its pair.
.PARAMETER ExperimentalExe
  PushFPS.exe of the experimental build (arms whose exe is 'experimental' in arms.csv). Default:
  %LOCALAPPDATA%\Programs\PushFPS-slipstream-exp\PushFPS.exe when it exists; otherwise those arms are skipped.
.PARAMETER Versus
  The stable candidate against the experimental build: the arms of the set 'versus' in arms.csv, one named switch
  set per arm (LAN profile, adaptive reorder, health, backlog, keyboard and mouse, audio fast recovery, governor
  observe, GPU budget observe). Duplicate detection is in no arm.
.PARAMETER Full
  One long session: the latency arms, then a 10-minute stability run (-Soak, -SoakMinutes 10 unless given) and the
  reliability checks (-Reliability), with a break before each part. The estimated time is shown first.
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
  Built-in latency tests per start (default 2). When the log shows that many finished tests the kit closes PushFPS
  by itself; -NoAutoClose leaves that to you.

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
    [switch]$Camera,
    [switch]$SkipLatency,
    [string]$PackOnly = '',
    [int]$ExpectRefreshHz = 0,
    [switch]$Health,
    [int]$DryRunStopAfter = 0,
    [string]$DryRunMismatchArm = '',
    [int]$DryRunPresses = 20,
    [string]$ExperimentalExe = '',
    [switch]$Versus,
    [switch]$Full,
    [string]$SceneNote = '',
    [string]$Resume = '',
    [int]$TestsPerStart = 2,
    [switch]$NoAutoClose,
    [switch]$AllowElevated,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$kitVersion = '3'
$script:dryBase = $null      # base time of the dry run's made-up timeline (Add-DryRunTimeline)
$script:referenceConfig = $null   # settings of the session's first kept latency run that logged its stream (Get-RunProblems)
$script:lastRun = $null           # the record and paths of the run Invoke-ArmRun finished last
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
$allowedLine = '\[(INFO |WARN |ERROR|TRACE)\] \[(push-(latency|quality|benchmark|mode|hardware)|remoteplay(-[a-z0-9-]+)?|slipstream-(governor|gpu-budget|watch)|frame-(flow|rate)|pacing|present-deviation|fg-(validity|drops)|display-(vrr|refresh|latency|query)|gpu-(failure|reconnect)|reset-lifecycle|engine-(hang|test)|crash|overlay-hooks|log-backpressure|qml\] \[update-check)\] '
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
    $t = $t -replace '\b10\.\d{1,3}\.\d{1,3}\.\d{1,3}\b', '10.x.x.x'
    $t = $t -replace '\b192\.168\.\d{1,3}\.\d{1,3}\b', '192.168.x.x'
    $t = $t -replace '\b172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}\b', '172.$1.x.x'
    $t = $t -replace '\b169\.254\.\d{1,3}\.\d{1,3}\b', '169.254.x.x'
    $t = $t -replace '\b127\.\d{1,3}\.\d{1,3}\.\d{1,3}\b', '127.x.x.x'
    $t = $t -replace '\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b', 'x.x.x.x'
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
        if (@('original', 'modified', 'experimental') -notcontains $row.exe) { throw "arms.csv: arm '$($row.name)' needs exe original, modified or experimental" }
        $list += $row
    }
    return $list
}

function Get-ArmSwitches($Arm) {
    $table = @{}
    if (-not $Arm.switches) { return $table }
    foreach ($pair in ($Arm.switches -split ';')) {
        if (-not $pair) { continue }
        $at = $pair.IndexOf('=')
        if ($at -lt 1) { throw "arms.csv: arm '$($Arm.name)': '$pair' is not NAME=VALUE" }
        $name = $pair.Substring(0, $at).Trim()
        if ($name -notmatch '^VEYRA_(EXP|TEST)_RP_[A-Z0-9_]+$') { throw "arms.csv: arm '$($Arm.name)': only VEYRA_EXP_RP_* and VEYRA_TEST_RP_* switches are allowed" }
        $table[$name] = $pair.Substring($at + 1).Trim()
    }
    return $table
}

function Clear-KitSwitches {
    $names = @()
    foreach ($item in Get-ChildItem Env:) {
        if (($item.Name -like 'VEYRA_EXP_RP_*') -or ($item.Name -like 'VEYRA_TEST_RP_*')) { $names += $item.Name }
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
function Save-Json($Object, [string]$Path) {
    $safe = @()
    foreach ($line in (($Object | ConvertTo-Json -Depth 6) -split "`r?`n")) { $safe += (Hide-OwnWords (Protect-Text $line)) }
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

function Get-Steps([string]$Purpose) {
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
        $list += New-Step 'gpu-device-loss' 'Connect to the PS5. About 15 seconds after the picture appears PushFPS removes its own graphics device once (only PushFPS; nothing else on the PC). When it says so, connect to the PS5 again and leave it. The step ends by itself.' 150 0 'auto' ''
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

function Get-GpuLossVerdict([string[]]$Lines, [string]$Start, [string]$End) {
    $lost = Find-FirstStamp $Lines '\] \[gpu-failure\] operation=|\] \[engine-test\] .*GPU' $Start $End
    if (-not $lost) { return @('NOT-EXERCISED', 'the log shows no GPU device loss in the step') }
    if (Find-FirstStamp $Lines '\] \[crash\] .*unhandled exception' $lost $End) { return @('FAIL', 'a crash line after the device was lost') }
    if (Find-FirstStamp $Lines '\] \[remoteplay-settings\] .*slipstream\.ini' $lost $End) { return @('FAIL', 'the program ended and started again after the device was lost') }
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
function Start-PresentMon([string]$Arguments) {
    try {
        if ($elevated) { return (Start-Process -FilePath $presentMonExe -ArgumentList $Arguments -WindowStyle Hidden -PassThru) }
        return (Start-Process -FilePath $presentMonExe -ArgumentList $Arguments -WindowStyle Hidden -PassThru -Verb RunAs)
    } catch {
        Write-Host ('  PresentMon did not start (' + $_.Exception.Message + '): this run has no frame times from it.') -ForegroundColor Yellow
        return $null
    }
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
    if ($wantHealth -and ($Arm.exe -ne 'original') -and (-not $switches.ContainsKey('VEYRA_EXP_RP_HEALTH'))) {
        Set-Item -Path 'Env:VEYRA_EXP_RP_HEALTH' -Value '1'
        $kitSwitches += 'VEYRA_EXP_RP_HEALTH=1'
    }
    if (($Purpose -eq 'reliability') -and $ReliabilityKbm -and (-not $switches.ContainsKey('VEYRA_EXP_RP_KBM'))) {
        Set-Item -Path 'Env:VEYRA_EXP_RP_KBM' -Value '1'
        $kitSwitches += 'VEYRA_EXP_RP_KBM=1'
    }
    if ($Purpose -eq 'gpuloss') {
        # After 900 frames of a live session PushFPS treats one frame as lost on the GPU and, half a second later,
        # removes its own Direct3D device. Only this process is affected; the variable is removed after the run.
        Set-Item -Path 'Env:VEYRA_TEST_GPU_FAILURE' -Value '900:500'
        $kitSwitches += 'VEYRA_TEST_GPU_FAILURE=900:500'
    }
    $before = Get-LogSizes $logFolder
    $live = New-LiveState
    $liveOffsets = $before.Clone()
    $watchLive = (($Purpose -eq 'latency') -or ($Purpose -eq 'camera'))
    $lastNews = Get-Date

    # PresentMon first, so that it sees the program start.
    $presentMonCsv = Join-Path $runFolder 'presentmon.csv'
    $presentMonProcess = $null
    $presentMonStart = ''
    $pmBase = '--process_name ' + [System.IO.Path]::GetFileName($Exe) + ' --terminate_on_proc_exit --stop_existing_session --session_name ' + $sessionName + ' --no_console_stats --track_gpu_video'
    if (($Purpose -eq 'soak') -and ($SoakPresentMonSeconds -gt 0)) { $pmBase += ' --timed ' + $SoakPresentMonSeconds + ' --terminate_after_timed' }
    # A long run records frames in short windows; presentmon-windows.csv says when each began.
    $windowFile = Join-Path $runFolder 'presentmon-windows.csv'
    $windowCount = 1
    $lastWindow = Get-Date
    if ($Purpose -eq 'soak') { Set-Content -LiteralPath $windowFile -Value 'file,startUtc' -Encoding ASCII }
    if ($presentMonExe -and (-not $DryRun)) {
        $pmArgs = $pmBase + ' --output_file "' + $presentMonCsv + '"'
        try {
            $presentMonStart = Get-UtcStamp
            $presentMonProcess = Start-PresentMon $pmArgs
            if ($Purpose -eq 'soak') { Add-Content -LiteralPath $windowFile -Value ('presentmon.csv,' + $presentMonStart) -Encoding ASCII }
        } catch {
            Write-Host ('PresentMon did not start: ' + $_.Exception.Message) -ForegroundColor Yellow
            $presentMonProcess = $null
        }
    }

    $steps = @(Get-Steps $Purpose)
    $dryArgs = '-NoProfile -Command "Start-Sleep -Seconds 4"'
    if ($steps.Count -gt 0) { $dryArgs = '-NoProfile -Command "Start-Sleep -Seconds 12"' }   # time for every step of the dry run
    $startUtc = Get-UtcStamp
    if ($DryRun) {
        $process = Start-Process -FilePath 'powershell.exe' -ArgumentList $dryArgs -WindowStyle Hidden -PassThru
        Add-DryRunLog $logFolder $Purpose $Arm.name
    } else {
        $process = Start-Process -FilePath $Exe -WorkingDirectory $exeFolder -PassThru
    }

    if ($Purpose -eq 'latency') {
        Write-Host '  1. Connect to the PS5 and wait in the game, in the same place as in the other runs.'
        Write-Host '  2. PS5 streaming dialog -> latency test -> 20 presses. Run it TWICE. Do not touch the controller meanwhile.'
        if ($NoAutoClose -or ($TestsPerStart -lt 1)) { Write-Host '  3. Close PushFPS. The kit continues by itself.' }
        else { Write-Host ('  3. Nothing else: after test ' + $TestsPerStart + ' the kit closes PushFPS itself. (Or close it yourself; the kit continues.)') }
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
                    $presentMonProcess = Start-PresentMon ($pmBase + ' --output_file "' + (Join-Path $runFolder $windowName) + '"')
                    Add-Content -LiteralPath $windowFile -Value ($windowName + ',' + $windowStart) -Encoding ASCII
                } catch { Write-Host ('  PresentMon window did not start: ' + $_.Exception.Message) -ForegroundColor Yellow }
            }
        }
        if (($Purpose -eq 'latency') -and (-not $NoAutoClose) -and ($TestsPerStart -gt 0) -and (-not $stopAsked) -and ($live.testsDone -ge $TestsPerStart) -and (-not $live.inTrial)) {
            $stopAsked = $true; $stopAskedAt = Get-Date; $stopReason = 'tests done'
            Write-Host ('  ' + $live.testsDone + ' latency tests done: closing PushFPS.') -ForegroundColor Green
            try { [void]$process.CloseMainWindow() } catch { }
        }
        if ($watchLive -and (-not $DryRun) -and (-not $stopAsked) -and (((Get-Date) - $lastNews).TotalSeconds -ge 60)) {
            $lastNews = Get-Date
            $waited = [int]((Get-Date) - $began).TotalMinutes
            if ($Purpose -eq 'latency') { Write-Host ('  still waiting (' + $waited + ' min): ' + $live.testsDone + ' of ' + $TestsPerStart + ' latency tests done. The test is in the PS5 streaming menu of PushFPS.') }
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
            $ender = Start-PresentMon ('--session_name ' + $sessionName + ' --terminate_existing_session')
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

    $config = Get-RunConfig $lines
    $config.from = ("run '" + $runName + "'")
    $problems = @()
    if (($Purpose -eq 'latency') -or ($Purpose -eq 'camera')) { $problems = @(Get-RunProblems $Arm $config) }
    $switchList = @()
    foreach ($name in ($switches.Keys | Sort-Object)) { $switchList += ($name + '=' + $switches[$name]) }
    $run = [pscustomobject]@{
        arm = $Arm.name; exe = $Arm.exe; compare = $Arm.compare; purpose = $Purpose; switches = $switchList; kitSwitches = $kitSwitches
        startUtc = $startUtc; endUtc = $endUtc; presentMonStartUtc = $presentMonStart; presentMonCapture = $hasCapture
        exitCode = $exitCode; restarts = $restarts; limitSeconds = $LimitSeconds; limitReached = $limitReached
        stopReason = $stopReason; forcedStop = $forcedStop; presentMonWindows = $windowCount
        presses = $presses; pressesAnsweredInSummaries = $script:summaryPresses; logLines = $lines.Count; log = ('bench/' + $benchName)
        sampleRows = $sampleRows; markers = $markerCount; dryRun = [bool]$DryRun
        appGpuCounters = $script:appGpuAvailable; logFrom = $fromUtc
        config = $config; configText = (Get-ConfigText $config); pairProblems = $problems; sceneNote = $SceneNote; fgExpected = [string]$Arm.fg
        latencyTestsSeen = $live.testsDone; samplesWhileTestTimed = $live.lightSamples; setAside = $false; setAsideReason = ''
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
function Add-DryRunLog([string]$LogFolder, [string]$Purpose, [string]$ArmName) {
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
    # The Windows user name in a line that is otherwise copied: it must come out as [removed] (Hide-OwnWords).
    if ($env:USERNAME -and ($env:USERNAME.Length -ge 3)) { $made += ('[INFO ] [remoteplay-kbm] made-up profile owner ' + $env:USERNAME + ' end') }
    if ($Purpose -eq 'impairment') { $made += '[WARN ] [remoteplay-recovery] request keyframe; received=900 decoded=880 videoCallbacks=900 rejected=0 packetReceived=4000 packetLost=37' }
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

# The settings a run actually ran with, as far as its (whitelisted) log lines tell. Empty text = not logged.
function Get-RunConfig([string[]]$Lines) {
    $profiles = @(); $codecs = @(); $bitrates = @(); $refreshes = @(); $vrrStates = @()
    $generation = ''; $flowSeen = $false; $decoder = ''; $timing = ''; $build = ''
    foreach ($line in $Lines) {
        if (-not $line) { continue }
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
function Compare-RunConfig($Config, $Reference) {
    $list = @()
    if (-not $Reference) { return $list }
    foreach ($item in $configKeys) {
        $mine = [string]$Config.($item.key)
        $theirs = [string]$Reference.($item.key)
        if ($mine -and $theirs -and ($mine -ne $theirs)) { $list += ($item.label + ': ' + $mine + $item.unit + ' in this run, ' + $theirs + $item.unit + ' in ' + $Reference.from) }
    }
    return $list
}

# Why a run cannot be compared: frame generation not as its arm needs it (arms.csv column fg), a setting that changed
# while it ran, or a setting that differs from the session's first run.
function Get-RunProblems($Arm, $Config) {
    $list = @()
    if (($Arm.fg -eq 'off') -and ($Config.frameGeneration -eq 'on')) { $list += 'frame generation was ON; this arm needs it OFF' }
    if (($Arm.fg -eq 'on') -and ($Config.frameGeneration -eq 'off')) { $list += 'frame generation was OFF; this arm needs it ON' }
    foreach ($what in @($Config.changedDuringRun)) { if ($what) { $list += ($what + ' changed while the run went on') } }
    foreach ($difference in @(Compare-RunConfig $Config $script:referenceConfig)) { if ($difference) { $list += $difference } }
    return $list
}

function New-LiveState {
    return [pscustomobject]@{ lines = (New-Object System.Collections.Generic.List[string]); inTrial = $false; testsDone = 0; presses = 0; announced = ''; fgWarned = $false; lightSamples = 0 }
}

# New (whitelisted, masked) lines of a running run: prints what is new, once.
function Update-LiveState($State, [string[]]$Lines, $Arm) {
    $any = $false
    foreach ($line in $Lines) {
        if (-not $line) { continue }
        $any = $true
        $State.lines.Add($line)
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
        } elseif ($line -match '\] \[push-latency\] (cancelled|.*no answer seen)') {
            $State.inTrial = $false
        }
    }
    if (-not $any) { return }
    $config = Get-RunConfig $State.lines.ToArray()
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

# RESULT.txt (one page to read) and result.json (the same as values) in the evidence folder.
function Write-ResultFiles([string]$Folder) {
    $tablePath = Join-Path $Folder 'arms.csv'
    if (-not (Test-Path -LiteralPath $tablePath)) { $tablePath = Join-Path $kitFolder 'arms.csv' }
    $armTable = @(Read-ArmTable $tablePath)
    $kept = @(Read-KeptRuns $Folder)
    $note = ''
    $kitFile = Join-Path $Folder 'kit.json'
    $dryNote = $false
    if (Test-Path -LiteralPath $kitFile) {
        try {
            $kitRecord = Get-Content -LiteralPath $kitFile -Raw | ConvertFrom-Json
            $note = [string]$kitRecord.sceneNote
            $dryNote = [bool]$kitRecord.dryRun
        } catch { $note = '' }
    }
    $setAside = 0
    if (Test-Path -LiteralPath (Join-Path $Folder 'setaside')) { $setAside = @(Get-ChildItem -LiteralPath (Join-Path $Folder 'setaside') -Recurse -Filter 'run.json').Count }

    # per arm: presses, configurations, facts
    $arms = @()
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
            config = $merged; reasons = $reasons; refused = $false; verdict = ''
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
            foreach ($difference in @(Compare-RunConfig $entry.config $base[0].config)) { if ($difference) { $entry.reasons += ($difference -replace ' in this run, ', ' here, ') } }
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
    }

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
    $out.Add('SLIPSTREAM TEST RESULT (one page; the details are in report.md after report.py)')
    if ($dryNote) { $out.Add('DRY RUN: made-up data, not a measurement.') }
    $out.Add('Written ' + (Get-UtcStamp) + ' by the kit (run-tests.ps1).')
    if ($note) { $out.Add('Scene: ' + $note) }
    $out.Add('')
    $out.Add('SOFTWARE LATENCY (the built-in test: software, not input-to-photon)')
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
    if ($setAside -gt 0) { $out.Add('  ' + $setAside + ' run(s) were set aside during the session because their settings differed (folder setaside, not used).') }
    $out.Add('')
    $out.Add('WHAT EACH ARM RAN WITH (from its own log lines)')
    foreach ($entry in $arms) {
        $switchText = $entry.switches
        if (-not $switchText) { $switchText = 'no switch' }
        $out.Add('  ' + $entry.name + ' (' + $entry.exe + ' build; ' + $switchText + '): ' + (Get-ConfigText $entry.config))
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
    Set-Content -LiteralPath (Join-Path $Folder 'RESULT.txt') -Value $out.ToArray() -Encoding ASCII

    # result.json
    $jsonArms = @()
    foreach ($entry in $arms) {
        $jsonArms += [pscustomobject]@{
            name = $entry.name; exe = $entry.exe; compare = $entry.compare; starts = $entry.starts; presses = $entry.pressValues; testsAnswered = $entry.answered; tests = $entry.tests
            median = (Get-JsonNumber $entry.median); medianLow = (Get-JsonNumber $entry.medianLow); medianHigh = (Get-JsonNumber $entry.medianHigh)
            p95 = (Get-JsonNumber $entry.p95); p95Low = (Get-JsonNumber $entry.p95Low); p95High = (Get-JsonNumber $entry.p95High)
            p99 = (Get-JsonNumber $entry.p99); p99Low = (Get-JsonNumber $entry.p99Low); p99High = (Get-JsonNumber $entry.p99High)
            refused = $entry.refused; reasons = @($entry.reasons); verdict = $entry.verdict
            config = [pscustomobject]@{ profile = $entry.config.profile; codec = $entry.config.codec; bitrateKbps = $entry.config.bitrateKbps; refreshHz = $entry.config.refreshHz; vrr = $entry.config.vrr; frameGeneration = $entry.config.frameGeneration; decoder = $entry.config.decoder }
            decodeFps = (Get-JsonNumber $entry.decodeFps); presentedFps = (Get-JsonNumber $entry.shownFps); decodeBusyMs = (Get-JsonNumber $entry.decodeBusyMs); decodeP95Ms = (Get-JsonNumber $entry.decodeP95Ms)
            jitterMs = (Get-JsonNumber $entry.jitterMs); lossPercent = (Get-JsonNumber $entry.lossPercent); healthIntervals = $entry.healthIntervals; errors = $entry.errors; warnings = $entry.warnings
        }
    }
    $result = [pscustomobject]@{
        kind = 'software latency (built-in test), not input-to-photon'; resultVersion = 1; dryRun = $dryNote; minimumPresses = $resultMinimum
        quantiles = 'linear interpolation between ranks; 95 % intervals from order statistics (ranks floor(np - 1.96 sqrt(np(1-p))) and ceil(np + 1.96 sqrt(np(1-p))) + 1)'
        verdictRule = $resultRule; arms = $jsonArms; setAsideRuns = $setAside; physicalRows = $physical; missingAppLogFields = $missingAppFields
    }
    Save-Json $result (Join-Path $Folder 'result.json')
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
        $isTable = (($file.Extension -eq '.csv') -and ($file.Name -like 'presentmon*')) -or ($file.Name -eq 'samples.csv') -or ($file.Name -eq 'sensor.csv') -or ($file.Name -eq 'arms.csv')
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
    Set-Content -LiteralPath (Join-Path $Folder 'withheld.txt') -Value (@('files the kit left out of the zip: ' + $withheld.Count) + $names) -Encoding ASCII
    $zip = $Folder.TrimEnd('\') + '.zip'
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    $items = @(Get-ChildItem -LiteralPath $Folder | Where-Object { ($_.Name -ne '_withheld') -and ($_.Name -ne '_dryrun') } | ForEach-Object { $_.FullName })
    Compress-Archive -LiteralPath $items -DestinationPath $zip -CompressionLevel Optimal
    Write-Host ''
    Write-Host ('Send this file back: ' + $zip) -ForegroundColor Green
    Write-Host ('Size: {0:N1} MB' -f ((Get-Item -LiteralPath $zip).Length / 1MB))
    return $zip
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
    Set-Content -LiteralPath $OriginalExe -Value 'not a program: stand-in for the dry run' -Encoding ASCII
    $Rounds = 1
} else {
    $base = Join-Path $env:LOCALAPPDATA 'Programs'
    if (-not $OriginalExe) {
        $guess = Join-Path $base 'PushFPS-original\PushFPS.exe'
        if (Test-Path -LiteralPath $guess) { $OriginalExe = $guess } elseif (-not $Versus) { $OriginalExe = (Read-Host 'Path of the ORIGINAL PushFPS.exe (Enter = skip the original arm)').Trim('"', ' ') }
    }
    if (-not $ModifiedExe) {
        $guess = Join-Path $base 'PushFPS-slipstream\PushFPS.exe'
        if (Test-Path -LiteralPath $guess) { $ModifiedExe = $guess } else { $ModifiedExe = (Read-Host 'Path of the MODIFIED PushFPS.exe').Trim('"', ' ') }
    }
    # The experimental copy is looked for only when a session uses it (-Versus, -Experimental or -Arms), and a guess
    # that is the modified copy itself (Run-First-Test -Build experimental) is dropped, not an error.
    $experimentalGuessed = $false
    if ((-not $ExperimentalExe) -and ($Versus -or $Experimental -or ($Arms.Count -gt 0))) {
        $guess = Join-Path $base 'PushFPS-slipstream-exp\PushFPS.exe'
        if (Test-Path -LiteralPath $guess) { $ExperimentalExe = $guess; $experimentalGuessed = $true }
    }
    if ($OriginalExe -and (-not (Test-Path -LiteralPath $OriginalExe))) { throw "Original PushFPS.exe not found: $OriginalExe" }
    if ((-not $ModifiedExe) -or (-not (Test-Path -LiteralPath $ModifiedExe))) { throw "Modified PushFPS.exe not found: $ModifiedExe" }
    if ($ExperimentalExe -and (-not (Test-Path -LiteralPath $ExperimentalExe))) { throw "Experimental PushFPS.exe not found: $ExperimentalExe" }
    if ($OriginalExe) { $OriginalExe = (Resolve-Path -LiteralPath $OriginalExe).Path }
    $ModifiedExe = (Resolve-Path -LiteralPath $ModifiedExe).Path
    if ($ExperimentalExe) { $ExperimentalExe = (Resolve-Path -LiteralPath $ExperimentalExe).Path }
    if ($OriginalExe -and ($OriginalExe -ieq $ModifiedExe)) { throw 'The original and the modified build must be two different folders.' }
    if ($ExperimentalExe -and ($ExperimentalExe -ieq $ModifiedExe)) {
        if ($experimentalGuessed) { $ExperimentalExe = '' }
        else { throw 'The modified (stable) and the experimental build must be two different folders.' }
    }
}

# PresentMon and nvidia-smi
$presentMonExe = ''
if ($PresentMon) {
    if (-not (Test-Path -LiteralPath $PresentMon)) { throw "PresentMon not found: $PresentMon" }
    $presentMonExe = (Resolve-Path -LiteralPath $PresentMon).Path
} else {
    # The kit folder first (that is where the procedure says to put the console program), then PATH. The
    # installed PresentMon application also has a service program of a similar name: never that one.
    $candidates = @()
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
        Write-Host ('PresentMon: ' + [System.IO.Path]::GetFileName($presentMonExe) + ', signed by Intel Corporation. Windows asks once per run to let it read frame times.')
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

# Which arms (through 'powershell -File' a list arrives as one string: split it)
$armList = @()
foreach ($item in $Arms) { foreach ($part in ($item -split ',')) { if ($part.Trim()) { $armList += $part.Trim() } } }
$Arms = $armList
if ($Camera -and ($Arms.Count -eq 0)) { $Arms = @('original', 'mod-all') }
$table = Read-ArmTable (Join-Path $kitFolder 'arms.csv')
$chosen = @()
foreach ($arm in $table) {
    $take = $false
    if ($Arms.Count -gt 0) { $take = ($Arms -contains $arm.name) }
    elseif ($Versus) { $take = ($arm.set -eq 'versus') }
    elseif ($arm.set -eq 'quick') { $take = $true }
    elseif ($arm.set -eq 'standard') { $take = (-not $Quick) }
    elseif ($arm.set -eq 'experimental') { $take = [bool]$Experimental }
    if ($take -and ($arm.exe -eq 'original') -and (-not $OriginalExe)) { Write-Host ("Arm '{0}' skipped: no original build given." -f $arm.name) -ForegroundColor Yellow; $take = $false }
    if ($take -and ($arm.exe -eq 'experimental') -and (-not $ExperimentalExe)) { Write-Host ("Arm '{0}' skipped: the experimental build is not installed (-ExperimentalExe)." -f $arm.name) -ForegroundColor Yellow; $take = $false }
    if ($take) { $chosen += $arm }
}
foreach ($name in $Arms) { if (-not ($table | Where-Object { $_.name -eq $name })) { throw "Unknown arm '$name' (see arms.csv)" } }

function Get-ExeFacts([string]$Path) {
    if (-not $Path) { return $null }
    $item = Get-Item -LiteralPath $Path
    $version = ''
    try { $version = [string]$item.VersionInfo.ProductVersion } catch { $version = '' }
    # Dots become underscores: a four-part version looks like an address to the masking. 10 digits of the hash identify the build (12 would look like a MAC address to the masking).
    return [pscustomobject]@{ file = $item.Name; bytes = $item.Length; productVersion = ($version -replace '\.', '_'); sha256First10 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.Substring(0, 10) }
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
    if (($record.purpose -eq 'latency') -and (-not $script:referenceConfig) -and $record.config -and $record.config.profile) { $script:referenceConfig = $record.config }
}

# The scene: typed once, kept in every record.
$previousKit = $null
if ($resumed) { try { $previousKit = Get-Content -LiteralPath (Join-Path $evidence 'kit.json') -Raw | ConvertFrom-Json } catch { $previousKit = $null } }
if ((-not $SceneNote) -and $previousKit -and $previousKit.sceneNote) { $SceneNote = [string]$previousKit.sceneNote }
if ((-not $SceneNote) -and (-not $DryRun) -and ((-not $SkipLatency) -or $Camera)) {
    Write-Host ''
    Write-Host 'Where in the game will every run be made? A few words, typed once (no names), e.g. "harbour pier, noon, camera north".'
    $SceneNote = Read-Host 'Scene'
}
if ($DryRun -and (-not $SceneNote)) { $SceneNote = 'dry run, no scene' }
$SceneNote = Protect-Note $SceneNote

# Time estimate: about 4 minutes per latency start (connect, two tests, close).
$latencyStarts = 0
if ((-not $SkipLatency) -and (-not $Camera)) { foreach ($arm in $chosen) { $left = $Rounds - $runCount[$arm.name]; if ($left -gt 0) { $latencyStarts += $left } } }
$estimate = 4 * $latencyStarts
$parts = @()
if ($latencyStarts -gt 0) { $parts += ('latency ' + $latencyStarts + ' starts, about ' + (4 * $latencyStarts) + ' min') }
$soakTotalMinutes = $SoakMinutes
if ($SoakHours -gt 0) { $soakTotalMinutes = [int][math]::Round($SoakHours * 60) }
if ($Soak -and ($donePurposes -notcontains 'soak')) { $estimate += $soakTotalMinutes + 3; $parts += ('stability ' + $soakTotalMinutes + ' min') }
if ($Impairment -and ($donePurposes -notcontains 'impairment')) { $estimate += 8; $parts += 'cable and network recovery about 8 min' }
if ($Reliability -and ($donePurposes -notcontains 'reliability')) { $estimate += 15; $parts += 'reliability checks about 15 min' }
if ($Full) { $estimate += 5; $parts += 'breaks' }
if ($estimate -gt 0) { Write-Host ('Estimated time: about ' + $estimate + ' minutes (' + ($parts -join ', ') + ').') -ForegroundColor Cyan }

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
        original = (Get-ExeFacts $OriginalExe); modified = (Get-ExeFacts $ModifiedExe); experimental = (Get-ExeFacts $ExperimentalExe)
        switchesFoundInEnvironmentAndCleared = $inherited
    }
    Save-Json $kit (Join-Path $evidence 'kit.json')
}
Copy-Item -LiteralPath (Join-Path $kitFolder 'arms.csv') -Destination (Join-Path $evidence 'arms.csv') -Force
# camera.csv may already hold filmed presses in a continued session: never overwrite it.
if (-not (Test-Path -LiteralPath (Join-Path $evidence 'camera.csv'))) { Copy-Item -LiteralPath (Join-Path $kitFolder 'camera-template.csv') -Destination (Join-Path $evidence 'camera.csv') }
if (-not (Test-Path -LiteralPath (Join-Path $evidence 'sensor.csv'))) { Copy-Item -LiteralPath (Join-Path $kitFolder 'sensor-template.csv') -Destination (Join-Path $evidence 'sensor.csv') -ErrorAction SilentlyContinue }

function Get-ArmExe($Arm) {
    if ($Arm.exe -eq 'original') { return $OriginalExe }
    if ($Arm.exe -eq 'experimental') { return $ExperimentalExe }
    return $ModifiedExe
}

# --- latency arms, interleaved ---------------------------------------------------------------------
$stop = $false
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
if ((-not $SkipLatency) -and ($chosen.Count -gt 0)) {
    Write-Step ('Latency arms: ' + ($armNames -join ', '))
    Write-Host 'Same game, same place, same in-game settings in every run. Wired network. Nothing else using the PC.'
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
                if ($left -ge 0) { $title += (', about ' + (4 * ($left + 1)) + ' min left') }
                $got = Invoke-ArmRun $arm (Get-ArmExe $arm) 'latency' 0 $title
                $problems = @()
                if ($script:lastRun) { $problems = @($script:lastRun.record.pairProblems) }
                if ($problems.Count -eq 0) { $pressCount[$arm.name] += $got; $runCount[$arm.name]++; break }
                Write-Host '  This run does not match what the comparison needs:' -ForegroundColor Red
                foreach ($problem in $problems) { Write-Host ('    - ' + $problem) -ForegroundColor Red }
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
            }
            if ($stop) { break }
            if (-not $DryRun) {
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
            Write-Host ('Below ' + $PressesPerArm + ' presses: ' + ($short -join ', ')) -ForegroundColor Yellow
            $answer = Read-Host 'Enter = one more round for these arms, q = finish with what there is'
            if ($answer -eq 'q') { break }
            $latencyStarts += $short.Count
        }
    }
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
Write-Host 'Camera measurements: fill in camera.csv in the evidence folder, then run this script with -PackOnly <that folder>.'
Write-Host ('Report (needs Python 3): python tools\remoteplay_latency_bench\report.py "' + $evidence + '"')
