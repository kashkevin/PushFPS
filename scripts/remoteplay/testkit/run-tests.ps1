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
        [push-latency] [push-quality] [push-benchmark]
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

  Windows PowerShell 5.1 or newer. PresentMon needs an elevated ("Run as administrator") PowerShell; without
  it only the PresentMon part is skipped.

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
    [int]$ExpectRefreshHz = 180,
    [switch]$Health,
    [int]$DryRunStopAfter = 0,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$kitVersion = '2'
$script:dryBase = $null      # base time of the dry run's made-up timeline (Add-DryRunTimeline)
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
$allowedLine = '\[(INFO |WARN |ERROR|TRACE)\] \[(push-(latency|quality|benchmark)|remoteplay(-[a-z0-9-]+)?|frame-(flow|rate)|pacing|present-deviation|fg-(validity|drops)|display-(vrr|refresh|latency|query)|gpu-(failure|reconnect)|reset-lifecycle|engine-(hang|test)|crash|overlay-hooks|log-backpressure|qml\] \[update-check)\] '
$refusedLine = '\[remoteplay-discovery\]|registration|regist_?key|rp_?key|account|psn(?!r=)|token|secret|passw|ssid|morning|nickname'

function Protect-Text([string]$Line) {
    $t = $Line
    $t = $t -replace '(?i)[A-Za-z]:\\+Users\\+[^\\\r\n"]*', '[path]'
    $t = $t -replace '[A-Za-z]:\\(?:[^\\\r\n"<>|]*\\)*[^\s",;)<>|]*', '[path]'
    $t = $t -replace '\b[0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}\b', '[mac]'
    $t = $t -replace '\b10\.\d{1,3}\.\d{1,3}\.\d{1,3}\b', '10.x.x.x'
    $t = $t -replace '\b192\.168\.\d{1,3}\.\d{1,3}\b', '192.168.x.x'
    $t = $t -replace '\b172\.(1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}\b', '172.$1.x.x'
    $t = $t -replace '\b169\.254\.\d{1,3}\.\d{1,3}\b', '169.254.x.x'
    $t = $t -replace '\b127\.\d{1,3}\.\d{1,3}\.\d{1,3}\b', '127.x.x.x'
    $t = $t -replace '\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b', 'x.x.x.x'
    $t = $t -replace '(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){3,7}[0-9A-Fa-f]{1,4}(?![0-9A-Za-z:])', '[ipv6]'
    $t = $t -replace '(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){1,6}:(?:[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4}){0,5})?(?![0-9A-Za-z:])', '[ipv6]'
    $t = $t -replace '(?:0[xX])?[0-9A-Fa-f]{16,}', '[hex]'
    $t = $t -replace '\b(host|name)=(?!\[name\]|(?:\d{1,3}|x)(?:\.(?:\d{1,3}|x)){3}(?![0-9A-Za-z.]))[^\s,;)]+', '$1=[name]'
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
        if ($line -match 'registration|regist_?key|rp_?key|account|psn(?!r=)|token|secret|passw|ssid|morning|nickname') { return $true }
        if ((Protect-Text $line) -cne $line) { return $true }
    }
    return $false
}

# --- small helpers -------------------------------------------------------------------------------
function Read-ArmTable([string]$Path) {
    $rows = Get-Content -LiteralPath $Path | Where-Object { $_ -and ($_ -notmatch '^\s*#') } | ConvertFrom-Csv
    $list = @()
    foreach ($row in $rows) {
        if ($row.name -notmatch '^[A-Za-z0-9-]+$') { throw "arms.csv: bad arm name '$($row.name)'" }
        if (($row.exe -ne 'original') -and ($row.exe -ne 'modified')) { throw "arms.csv: arm '$($row.name)' needs exe original or modified" }
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
    foreach ($line in (($Object | ConvertTo-Json -Depth 6) -split "`r?`n")) { $safe += (Protect-Text $line) }
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
    if ($wantHealth -and ($Arm.exe -eq 'modified') -and (-not $switches.ContainsKey('VEYRA_EXP_RP_HEALTH'))) {
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
            $presentMonProcess = Start-Process -FilePath $presentMonExe -ArgumentList $pmArgs -WindowStyle Hidden -PassThru
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
        Add-DryRunLog $logFolder $Purpose
    } else {
        $process = Start-Process -FilePath $Exe -WorkingDirectory $exeFolder -PassThru
    }

    if ($Purpose -eq 'latency') {
        Write-Host '  1. Connect to the PS5 and wait in the game, in the same place as in the other runs.'
        Write-Host '  2. PS5 streaming dialog -> latency test -> 20 presses. Run it TWICE. Do not touch the controller meanwhile.'
        Write-Host '  3. Close PushFPS. The kit continues by itself.'
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
    $sampleHeader = 'utc,cpuTotalPercent,appCpuPercent,appWorkingSetMb,appPrivateMb,appThreads,appHandles,' + $appGpuColumns
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
        $cpuTotal = ''
        try {
            $perf = Get-CimInstance -ClassName Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'" -ErrorAction Stop
            $cpuTotal = [string]$perf.PercentProcessorTime
        } catch { $cpuTotal = '' }
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
            if (($sampleRows % $script:appGpuEvery) -eq 0) { $appGpu = Get-AppGpuSample $process.Id }
        } catch { $appCpu = '' }
        $lastTick = $tick
        $row = (@((Get-UtcStamp), $cpuTotal, $appCpu, $ws, $private, $threads, $handles) -join ',') + ',' + $appGpu
        if ($gpuColumns) { $row += ',' + (Get-GpuSample) }
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
            if ($presentMonExe -and (-not $DryRun) -and (-not $stopAsked) -and ($SoakPresentMonSeconds -gt 0) -and ($SoakPresentMonEveryMinutes -gt 0) -and
                (((Get-Date) - $lastWindow).TotalMinutes -ge $SoakPresentMonEveryMinutes)) {
                $lastWindow = Get-Date
                $windowCount++
                $windowName = 'presentmon-{0:D3}.csv' -f $windowCount
                try {
                    $windowStart = Get-UtcStamp
                    $presentMonProcess = Start-Process -FilePath $presentMonExe -ArgumentList ($pmBase + ' --output_file "' + (Join-Path $runFolder $windowName) + '"') -WindowStyle Hidden -PassThru
                    Add-Content -LiteralPath $windowFile -Value ($windowName + ',' + $windowStart) -Encoding ASCII
                } catch { Write-Host ('  PresentMon window did not start: ' + $_.Exception.Message) -ForegroundColor Yellow }
            }
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
            try { Start-Process -FilePath $presentMonExe -ArgumentList ('--session_name ' + $sessionName + ' --terminate_existing_session') -WindowStyle Hidden -Wait } catch { }
            if (-not $presentMonProcess.WaitForExit(10000)) { try { Stop-Process -Id $presentMonProcess.Id -Force -ErrorAction SilentlyContinue } catch { } }
        }
    }
    [void](Clear-KitSwitches)

    $hasCapture = (Test-Path -LiteralPath $presentMonCsv) -and ((Get-Item -LiteralPath $presentMonCsv).Length -gt 200)
    if ($presentMonExe -and (-not $DryRun) -and (-not $hasCapture)) {
        Write-Host '  PresentMon wrote no capture for this run (it needs an elevated PowerShell).' -ForegroundColor Yellow
    }

    $script:summaryPresses = 0
    # Two seconds of slack: the program's clock stamps a line when it is made, the kit's start time is taken
    # just before the program starts.
    $fromUtc = ([datetime]::ParseExact($startUtc.Substring(0, 23), 'yyyy-MM-ddTHH:mm:ss.fff', [System.Globalization.CultureInfo]::InvariantCulture)).AddSeconds(-2).ToString('yyyy-MM-ddTHH:mm:ss.fff') + 'Z'
    $lines = @(Select-SinceStart @(Select-SafeLines (Read-NewLogLines $logFolder $before)) $fromUtc)
    $presses = Count-Presses $lines
    $benchFolder = Join-Path $evidence 'bench'
    New-Item -ItemType Directory -Force -Path $benchFolder | Out-Null
    $benchName = $runName + '.txt'
    Set-Content -LiteralPath (Join-Path $benchFolder $benchName) -Value $lines -Encoding UTF8

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
        completed = $true   # written last: a run folder without this file, or without this field, did not finish
    }
    if (($Purpose -eq 'reliability') -or ($Purpose -eq 'gpuloss') -or ($Purpose -eq 'safestart')) {
        Write-ReliabilityVerdicts $lines $runFolder $Purpose $limitReached $forcedStop $LimitSeconds
    }
    Save-Json $run (Join-Path $runFolder 'run.json')
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
function Add-DryRunLog([string]$LogFolder, [string]$Purpose) {
    $script:dryBase = $null
    if (($Purpose -eq 'reliability') -or ($Purpose -eq 'gpuloss')) { Add-DryRunTimeline $LogFolder $Purpose; return }
    New-Item -ItemType Directory -Force -Path $LogFolder | Out-Null
    $now = (Get-Date).ToUniversalTime()
    $values = @()
    for ($i = 0; $i -lt 20; $i++) { $values += ('{0:F1}' -f (83.3 + 16.7 * ($i % 3) + $i * 0.1)) }
    $made = @(
        '[INFO ] [app] PushFPS QML session started (based on Veyra)',
        '[INFO ] [app] data folder C:\Users\PlantedUserName\AppData\Local\Programs\PushFPS\runtime_local',
        '[INFO ] [remoteplay-discovery] host-name=PlantedConsoleName host-id=AABBCCDDEEFF',
        '[INFO ] [remoteplay-ui] registration regist_key=0123456789abcdef0123 account=PlantedAccountId=',
        '[INFO ] [wlan] ssid=PlantedWifiName',
        '[INFO ] [remoteplay-control] start host=192.168.1.77 restart=false background=false',
        '[INFO ] [remoteplay-control] start host=PlantedHostName restart=false background=false',
        '[INFO ] [remoteplay] session started host=203.0.113.9 profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000',
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
        '[INFO ] [remoteplay-health] grade=stable interval=stable seconds=5.0 lossPercent=0.000 lost=0 received=9000 frameJitterMs=0.40 frameJitterPeakMs=1.20 reordered=0 late=0 reorderTimeouts=0 skipped=0 fecRecovered=0 fecFailed=0 keyframeRequests=0 decoderResets=0 frameDrops=0 ingressDepth=0 ingressPeak=1 mailboxOverwrites=0 mbps=28.10 fps=59.9 rtt=0.45ms(at-connect) route=direct-lan link=ethernet linkMbps=1000',
        '[INFO ] [remoteplay-progress] state=3 received=600 decoded=600 inputFps=59.9 decodeFps=59.9 videoMbps=28.100 receiveAgeMs=3.0 decodeAgeMs=5.0 decodeBusyMs=1.2 queue=0 waitingIdr=false idrRequests=0 errors=0',
        '[INFO ] [push-latency] remote play latency test started trials=20',
        ('[INFO ] [push-latency] done answered=20/20 meanMs=100.9 medianMs=100.0 minMs=83.3 maxMs=118.6 pushfpsMs=6.1'),
        ('[INFO ] [push-latency] trials answerMs=[' + ($values -join ',') + ']')
    )
    if ($Purpose -eq 'impairment') { $made += '[WARN ] [remoteplay-recovery] request keyframe; received=900 decoded=880 videoCallbacks=900 rejected=0 packetReceived=4000 packetLost=37' }
    $out = @()
    $n = 0
    foreach ($line in $made) {
        $out += ($now.AddMilliseconds(200 * $n).ToString('yyyy-MM-ddTHH:mm:ss.fffZ') + ' t=1234 ' + $line)
        $n++
    }
    Add-Content -LiteralPath (Join-Path $LogFolder 'veyra-qml.log') -Value $out -Encoding UTF8
}

# --- checking and packing ----------------------------------------------------------------------------
function Publish-Evidence([string]$Folder) {
    Write-Step 'Checking the evidence and making the zip'
    $withheld = @()
    $names = @()
    foreach ($file in Get-ChildItem -LiteralPath $Folder -Recurse -File) {
        if ($file.FullName -like '*\_withheld\*') { continue }
        $isKitText = ($file.Extension -eq '.json') -or ($file.Extension -eq '.txt') -or ($file.Name -eq 'markers.csv') -or ($file.Name -eq 'camera.csv') -or ($file.Name -eq 'reliability.csv')
        if ($isKitText -and (Test-FileLeaks $file.FullName)) { $withheld += $file }
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
    [void](Publish-Evidence (Resolve-Path -LiteralPath $PackOnly).Path)
    return
}

# --- start -------------------------------------------------------------------------------------------
if (-not $OutRoot) {
    # Not inside the repository or the program folders: your Documents folder.
    $documents = [Environment]::GetFolderPath('MyDocuments')
    if (-not $documents) { $documents = (Get-Location).Path }
    $OutRoot = Join-Path $documents 'slipstream-evidence'
}
$evidence = Join-Path $OutRoot ('evidence-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
$evidence = (Resolve-Path -LiteralPath $evidence).Path
Write-Step ('Slipstream hardware test kit ' + $kitVersion)
Write-Host ('Evidence folder: ' + $evidence)

if ($DryRun) {
    $dry = Join-Path $evidence '_dryrun'
    New-Item -ItemType Directory -Force -Path (Join-Path $dry 'logs') | Out-Null
    $OriginalExe = Join-Path $dry 'PushFPS.exe'
    $ModifiedExe = $OriginalExe
    Set-Content -LiteralPath $OriginalExe -Value 'not a program: stand-in for the dry run' -Encoding ASCII
    $Rounds = 1
} else {
    $base = Join-Path $env:LOCALAPPDATA 'Programs'
    if (-not $OriginalExe) {
        $guess = Join-Path $base 'PushFPS-original\PushFPS.exe'
        if (Test-Path -LiteralPath $guess) { $OriginalExe = $guess } else { $OriginalExe = (Read-Host 'Path of the ORIGINAL PushFPS.exe (Enter = skip the original arm)').Trim('"', ' ') }
    }
    if (-not $ModifiedExe) {
        $guess = Join-Path $base 'PushFPS-slipstream\PushFPS.exe'
        if (Test-Path -LiteralPath $guess) { $ModifiedExe = $guess } else { $ModifiedExe = (Read-Host 'Path of the MODIFIED PushFPS.exe').Trim('"', ' ') }
    }
    if ($OriginalExe -and (-not (Test-Path -LiteralPath $OriginalExe))) { throw "Original PushFPS.exe not found: $OriginalExe" }
    if ((-not $ModifiedExe) -or (-not (Test-Path -LiteralPath $ModifiedExe))) { throw "Modified PushFPS.exe not found: $ModifiedExe" }
    if ($OriginalExe) { $OriginalExe = (Resolve-Path -LiteralPath $OriginalExe).Path }
    $ModifiedExe = (Resolve-Path -LiteralPath $ModifiedExe).Path
    if ($OriginalExe -and ($OriginalExe -ieq $ModifiedExe)) { throw 'The original and the modified build must be two different folders.' }
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
$elevated = $false
try {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    $elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
} catch { $elevated = $false }
if (-not $presentMonExe) {
    Write-Host 'PresentMon not found: frame times, dropped frames and the present mode will be missing.' -ForegroundColor Yellow
    Write-Host '  To include them: download PresentMon from its GitHub releases page (GameTechDev/PresentMon), put the'
    Write-Host ('  console program (PresentMon-<version>-x64.exe) into ' + $kitFolder + ' and start the kit again.')
} elseif (-not $elevated) {
    Write-Host 'PresentMon found, but this PowerShell is not elevated: its captures will probably be empty.' -ForegroundColor Yellow
    Write-Host '  Close this window and start PowerShell with "Run as administrator" to include them.'
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
    Write-Host 'nvidia-smi not found or not answering: GPU load, decoder load and video memory will be missing.' -ForegroundColor Yellow
}

# Hardware report (the detection script changes nothing on the PC), then the two private fields are removed.
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
    elseif ($arm.set -eq 'quick') { $take = $true }
    elseif ($arm.set -eq 'standard') { $take = (-not $Quick) }
    elseif ($arm.set -eq 'experimental') { $take = [bool]$Experimental }
    if ($take -and ($arm.exe -eq 'original') -and (-not $OriginalExe)) { Write-Host ("Arm '{0}' skipped: no original build given." -f $arm.name) -ForegroundColor Yellow; $take = $false }
    if ($take) { $chosen += $arm }
}
foreach ($name in $Arms) { if (-not ($table | Where-Object { $_.name -eq $name })) { throw "Unknown arm '$name' (see arms.csv)" } }

function Get-ExeFacts([string]$Path) {
    if (-not $Path) { return $null }
    $item = Get-Item -LiteralPath $Path
    $version = ''
    try { $version = [string]$item.VersionInfo.ProductVersion } catch { $version = '' }
    # Dots become underscores: a four-part version looks like an address to the masking. 12 digits of the hash identify the build.
    return [pscustomobject]@{ file = $item.Name; bytes = $item.Length; productVersion = ($version -replace '\.', '_'); sha256First12 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.Substring(0, 12) }
}
$inherited = Clear-KitSwitches
$armNames = @()
foreach ($arm in $chosen) { $armNames += $arm.name }
$kit = [pscustomobject]@{
    kitVersion = $kitVersion; startedUtc = (Get-UtcStamp); dryRun = [bool]$DryRun
    powershell = $PSVersionTable.PSVersion.ToString(); elevated = $elevated
    presentMon = [bool]$presentMonExe; presentMonFile = [System.IO.Path]::GetFileName($presentMonExe); nvidiaSmi = [bool]$nvidiaSmi
    arms = $armNames; rounds = $Rounds; pressesPerArm = $PressesPerArm
    soak = [bool]$Soak; soakMinutes = $SoakMinutes; soakHours = $SoakHours; impairment = [bool]$Impairment; health = [bool]$Health
    reliability = [bool]$Reliability; reliabilityKbm = [bool]$ReliabilityKbm; reliabilityGpuLoss = [bool]$ReliabilityGpuLoss
    original = (Get-ExeFacts $OriginalExe); modified = (Get-ExeFacts $ModifiedExe)
    switchesFoundInEnvironmentAndCleared = $inherited
}
Save-Json $kit (Join-Path $evidence 'kit.json')
Copy-Item -LiteralPath (Join-Path $kitFolder 'arms.csv') -Destination (Join-Path $evidence 'arms.csv')
Copy-Item -LiteralPath (Join-Path $kitFolder 'camera-template.csv') -Destination (Join-Path $evidence 'camera.csv')

function Get-ArmExe($Arm) {
    if ($Arm.exe -eq 'original') { return $OriginalExe }
    return $ModifiedExe
}

# --- latency arms, interleaved ---------------------------------------------------------------------
$pressCount = @{}
foreach ($arm in $chosen) { $pressCount[$arm.name] = 0 }
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
    $round = 1
    while (-not $stop) {
        $order = @()   # A B C, then C B A: slow drift of the console or the network cancels
        if (($round % 2) -eq 0) { for ($i = $chosen.Count - 1; $i -ge 0; $i--) { $order += $chosen[$i] } }
        else { foreach ($item in $chosen) { $order += $item } }
        foreach ($arm in $order) {
            if (($round -gt $Rounds) -and ($pressCount[$arm.name] -ge $PressesPerArm)) { continue }
            $title = "Round $round, arm '$($arm.name)' ($($pressCount[$arm.name]) of $PressesPerArm presses so far)"
            $pressCount[$arm.name] += (Invoke-ArmRun $arm (Get-ArmExe $arm) 'latency' 0 $title)
            if (-not $DryRun) {
                $answer = Read-Host 'Enter = next run, q = stop here (the zip is still made)'
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
        }
    }
}

# --- soak and impairment ---------------------------------------------------------------------------
if ($Soak -and (-not $stop)) {
    $arm = $table | Where-Object { $_.name -eq $SoakArm } | Select-Object -First 1
    if (-not $arm) { throw "Unknown soak arm '$SoakArm'" }
    $soakTotalMinutes = $SoakMinutes
    if ($SoakHours -gt 0) { $soakTotalMinutes = [int][math]::Round($SoakHours * 60) }
    if ($soakTotalMinutes -lt 1) { throw '-SoakMinutes / -SoakHours: the soak must last at least one minute' }
    $seconds = $soakTotalMinutes * 60
    if ($DryRun) { $seconds = 3 }
    [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'soak' $seconds "Soak: arm '$($arm.name)' for $soakTotalMinutes minutes")
}
if ($Impairment -and (-not $stop)) {
    $arm = $table | Where-Object { $_.name -eq $ImpairmentArm } | Select-Object -First 1
    if (-not $arm) { throw "Unknown impairment arm '$ImpairmentArm'" }
    [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'impairment' 0 "Impairment and recovery: arm '$($arm.name)'")
}

if ($Reliability -and (-not $stop)) {
    $arm = $table | Where-Object { $_.name -eq $ReliabilityArm } | Select-Object -First 1
    if (-not $arm) { throw "Unknown reliability arm '$ReliabilityArm'" }
    Write-Step 'Reliability checks'
    Write-Host 'Each step starts when you press Enter in THIS window and ends by itself; the kit beeps and names the next one.'
    Write-Host 'Steps you cannot do (a single monitor, no keyboard and mouse mode): press Enter and wait; they are reported as not exercised.'
    [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'reliability' 0 "Reliability: arm '$($arm.name)'")
    if ($ReliabilityGpuLoss) {
        [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'gpuloss' 0 "Reliability: loss of the graphics device, arm '$($arm.name)' (close PushFPS when the step has ended)")
    }
    # A start with nothing set: no switch, no slipstream.ini. What a tester falls back to when a setting broke something.
    $safe = [pscustomobject]@{ name = 'safe-start'; exe = 'modified'; set = 'reliability'; compare = ''; switches = ''; note = 'PushFPS with default settings: no switch and slipstream.ini not read.' }
    $safeSeconds = 20
    if ($DryRun) { $safeSeconds = 3 }
    [void](Invoke-ArmRun $safe $ModifiedExe 'safestart' $safeSeconds 'Reliability: safe start')
}

if ($DryRun) { Remove-Item -LiteralPath (Join-Path $evidence '_dryrun') -Recurse -Force -ErrorAction SilentlyContinue }
$zipPath = Publish-Evidence $evidence
Write-Host ''
Write-Host 'Camera measurements: fill in camera.csv in the evidence folder, then run this script with -PackOnly <that folder>.'
Write-Host ('Report (needs Python 3): python tools\remoteplay_latency_bench\report.py "' + $evidence + '"')
