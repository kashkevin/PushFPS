<#
.SYNOPSIS
  Slipstream hardware test kit: one command that runs the PS5 Remote Play latency arms on the real PC,
  collects the evidence in one folder and packs it into one zip to send back.

.DESCRIPTION
  For each arm (see arms.csv) the kit starts PushFPS with that arm's environment switches, records while
  you use it, and saves:
    - the log lines of that run that belong to the whitelisted families (below), with addresses masked;
    - a PresentMon capture, when PresentMon is found (parameter, this folder, or PATH);
    - once-a-second samples of processor load, PushFPS memory and, when nvidia-smi exists, GPU load,
      video decoder load and video memory (first GPU).
  Arms are interleaved (A B C, then C B A ...) and the kit counts the presses of the built-in latency test
  until every arm has 120.

  WHAT GOES INTO THE ZIP - the whitelist
    Log lines are copied ONLY when their component is
        [push-latency]
        [remoteplay] or [remoteplay-<anything>]   except [remoteplay-discovery]
    and the line holds none of these words: registration, regist_key, rp_key, account, psn, token, secret,
    passw, ssid, morning, nickname. In the lines that are copied:
        IPv4 addresses keep only their private-range prefix (192.168.x.x, 10.x.x.x, 172.16.x.x ... 172.31.x.x,
        169.254.x.x, 127.x.x.x); any other address becomes x.x.x.x
        IPv6 addresses become [ipv6], MAC addresses [mac], runs of 16 or more hex digits [hex]
        host=<name> and name=<name> become host=[name] / name=[name]
    From the hardware report the computer name and the network connection names are removed.
    Never copied: the settings store (runtime_local), the whole log, console registration keys, PSN account
    ids, tokens, Wi-Fi names, the paths of your folders, your Windows user name.

  WHAT THE KIT DOES NOT DO
    It installs nothing, loads no driver and changes no Windows, driver, router or PS5 setting. It sets
    environment variables for the PushFPS processes it starts and for nothing else.

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
.PARAMETER Soak
  Long run of one arm (-SoakArm, default mod-all) for -SoakMinutes (default 120): drops, reconnects, memory.
.PARAMETER Impairment
  Guided recovery test: cable out for 1 s, for 5 s, and a second device loading the network.
.PARAMETER Camera
  Camera session: starts each chosen arm once (default original and mod-all) so that you can film your own
  presses; no latency test is asked for. See docs/HARDWARE_TEST_PROCEDURE.md.
.PARAMETER SkipLatency
  Skip the latency arms (to run only -Soak or -Impairment).
.PARAMETER PackOnly
  An existing evidence folder: check it and make the zip again (after camera.csv was filled in).
.PARAMETER DryRun
  Starts no PushFPS and asks nothing: runs every step on made-up data. Used by the checks workflow.

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
    [string]$SoakArm = 'mod-all',
    [int]$SoakPresentMonSeconds = 600,
    [switch]$Impairment,
    [string]$ImpairmentArm = 'mod-all',
    [switch]$Camera,
    [switch]$SkipLatency,
    [string]$PackOnly = '',
    [int]$ExpectRefreshHz = 180,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$kitVersion = '1'
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
$allowedLine = '\[(push-latency|remoteplay(-[a-z0-9-]+)?)\]'
$refusedLine = '\[remoteplay-discovery\]|registration|regist_?key|rp_?key|account|psn|token|secret|passw|ssid|morning|nickname'

function Protect-Text([string]$Line) {
    $t = $Line
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

# True when a text file written by the kit still holds something the rules above would change or refuse.
function Test-FileLeaks([string]$Path) {
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        if ($line -match 'registration|regist_?key|rp_?key|account|psn|token|secret|passw|ssid|morning|nickname') { return $true }
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

    Write-Step $Title
    Write-Host $Arm.note
    if (-not $DryRun) { Wait-NoPushFps $Exe }

    [void](Clear-KitSwitches)
    Set-Item -Path 'Env:VEYRA_TEST_RP_NO_SETTINGS_FILE' -Value '1'   # slipstream.ini must not add switches to an arm
    foreach ($name in $switches.Keys) { Set-Item -Path ('Env:' + $name) -Value $switches[$name] }
    $before = Get-LogSizes $logFolder

    # PresentMon first, so that it sees the program start.
    $presentMonCsv = Join-Path $runFolder 'presentmon.csv'
    $presentMonProcess = $null
    $presentMonStart = ''
    if ($presentMonExe -and (-not $DryRun)) {
        $pmArgs = '--process_name ' + [System.IO.Path]::GetFileName($Exe) + ' --output_file "' + $presentMonCsv + '"' +
                  ' --terminate_on_proc_exit --stop_existing_session --session_name ' + $sessionName + ' --no_console_stats --track_gpu_video'
        if (($Purpose -eq 'soak') -and ($SoakPresentMonSeconds -gt 0)) { $pmArgs += ' --timed ' + $SoakPresentMonSeconds + ' --terminate_after_timed' }
        try {
            $presentMonStart = Get-UtcStamp
            $presentMonProcess = Start-Process -FilePath $presentMonExe -ArgumentList $pmArgs -WindowStyle Hidden -PassThru
        } catch {
            Write-Host ('PresentMon did not start: ' + $_.Exception.Message) -ForegroundColor Yellow
            $presentMonProcess = $null
        }
    }

    $startUtc = Get-UtcStamp
    if ($DryRun) {
        $process = Start-Process -FilePath 'powershell.exe' -ArgumentList '-NoProfile -Command "Start-Sleep -Seconds 4"' -WindowStyle Hidden -PassThru
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
        Write-Host ("  Connect to the PS5 and play or leave the game running for {0} minutes. Closing PushFPS ends the run early." -f [int]($LimitSeconds / 60))
    }
    $steps = @()
    if ($Purpose -eq 'impairment') {
        $steps = @(
            'Connect to the PS5 and wait until the game picture is steady. Then press Enter HERE and at once pull the network cable out of the PC for about 1 second and put it back.',
            'Wait until the picture is back to normal. Then press Enter and pull the cable for about 5 seconds.',
            'Wait until the picture is back to normal. Then press Enter and start heavy traffic on a second device on the same network (a 4K video or a large download) for one minute.',
            'Stop the traffic on the second device, press Enter, then close PushFPS.'
        )
        Write-Host ('  NEXT: ' + $steps[0]) -ForegroundColor Green
    }
    $markerNames = @('cable-out-1s', 'cable-out-5s', 'second-device-load-start', 'second-device-load-end')
    $markerFile = Join-Path $runFolder 'markers.csv'
    if ($Purpose -eq 'impairment') { Set-Content -LiteralPath $markerFile -Value 'utc,marker' -Encoding ASCII }
    $stepIndex = 0

    # Once-a-second samples until the program is closed (or the time limit is reached).
    $sampleFile = Join-Path $runFolder 'samples.csv'
    $sampleHeader = 'utc,cpuTotalPercent,appCpuPercent,appWorkingSetMb,appPrivateMb,appThreads,appHandles'
    if ($gpuColumns) { $sampleHeader += ',' + $gpuColumns }
    Set-Content -LiteralPath $sampleFile -Value $sampleHeader -Encoding ASCII
    $cores = [Environment]::ProcessorCount
    $lastCpu = $null
    $lastTick = Get-Date
    $began = Get-Date
    $limitReached = $false
    $restarts = 0
    $sampleRows = 0
    while ($true) {
        # A sample is taken before the program is looked at, so that every run has at least one row.
        $tick = Get-Date
        $cpuTotal = ''
        try {
            $perf = Get-CimInstance -ClassName Win32_PerfFormattedData_PerfOS_Processor -Filter "Name='_Total'" -ErrorAction Stop
            $cpuTotal = [string]$perf.PercentProcessorTime
        } catch { $cpuTotal = '' }
        $appCpu = ''; $ws = ''; $private = ''; $threads = ''; $handles = ''
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
        } catch { $appCpu = '' }
        $lastTick = $tick
        $row = @((Get-UtcStamp), $cpuTotal, $appCpu, $ws, $private, $threads, $handles) -join ','
        if ($gpuColumns) { $row += ',' + (Get-GpuSample) }
        Add-Content -LiteralPath $sampleFile -Value $row -Encoding ASCII
        $sampleRows++

        if (($Purpose -eq 'impairment') -and (-not $DryRun)) {
            $key = Read-KeyIfAny
            if ($key -and ($key.Key -eq 'Enter') -and ($stepIndex -lt $markerNames.Count)) {
                Add-Content -LiteralPath $markerFile -Value ((Get-UtcStamp) + ',' + $markerNames[$stepIndex]) -Encoding ASCII
                Write-Host ('  marked: ' + $markerNames[$stepIndex])
                $stepIndex++
                if ($stepIndex -lt $steps.Count) { Write-Host ('  NEXT: ' + $steps[$stepIndex]) -ForegroundColor Green }
            }
        }
        if (($Purpose -eq 'impairment') -and $DryRun -and ($stepIndex -lt $markerNames.Count)) {
            Add-Content -LiteralPath $markerFile -Value ((Get-UtcStamp) + ',' + $markerNames[$stepIndex]) -Encoding ASCII
            $stepIndex++
        }
        if (($LimitSeconds -gt 0) -and (-not $limitReached) -and (((Get-Date) - $began).TotalSeconds -ge $LimitSeconds)) {
            $limitReached = $true
            Write-Host '  Time is up: closing PushFPS (close it yourself if it stays open).' -ForegroundColor Green
            try { [void]$process.CloseMainWindow() } catch { }
        }
        if ($process.HasExited) {
            # PushFPS can restart itself once (overlay compatibility): follow the new copy.
            $other = $null
            if (-not $DryRun) { Start-Sleep -Milliseconds 1500; $other = Find-RunningCopy $Exe }
            if ($other) { $process = $other; $lastCpu = $null; $restarts++ } else { break }
        }
        $spent = ((Get-Date) - $tick).TotalMilliseconds
        if ($spent -lt 1000) { Start-Sleep -Milliseconds ([int](1000 - $spent)) }
    }
    # The loop above is the sampler: it runs in this script, so when it ends nothing is still writing.
    try { $process.WaitForExit() } catch { }
    if (($Purpose -eq 'impairment') -and $DryRun) {
        # The dry run always leaves all four markers, however few samples its short stand-in allowed.
        while ($stepIndex -lt $markerNames.Count) {
            Add-Content -LiteralPath $markerFile -Value ((Get-UtcStamp) + ',' + $markerNames[$stepIndex]) -Encoding ASCII
            $stepIndex++
        }
    }
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
    $lines = @(Select-SafeLines (Read-NewLogLines $logFolder $before))
    $presses = Count-Presses $lines
    $benchFolder = Join-Path $evidence 'bench'
    New-Item -ItemType Directory -Force -Path $benchFolder | Out-Null
    $benchName = $runName + '.txt'
    Set-Content -LiteralPath (Join-Path $benchFolder $benchName) -Value $lines -Encoding UTF8

    $switchList = @()
    foreach ($name in ($switches.Keys | Sort-Object)) { $switchList += ($name + '=' + $switches[$name]) }
    $run = [pscustomobject]@{
        arm = $Arm.name; exe = $Arm.exe; compare = $Arm.compare; purpose = $Purpose; switches = $switchList
        startUtc = $startUtc; endUtc = $endUtc; presentMonStartUtc = $presentMonStart; presentMonCapture = $hasCapture
        exitCode = $exitCode; restarts = $restarts; limitSeconds = $LimitSeconds; limitReached = $limitReached
        presses = $presses; pressesAnsweredInSummaries = $script:summaryPresses; logLines = $lines.Count; log = ('bench/' + $benchName)
        sampleRows = $sampleRows; markers = $stepIndex; dryRun = [bool]$DryRun
        completed = $true   # written last: a run folder without this file, or without this field, did not finish
    }
    Save-Json $run (Join-Path $runFolder 'run.json')
    if ($presses -eq 0 -and $script:summaryPresses -gt 0) { $presses = $script:summaryPresses }
    Write-Host ("  saved: {0} log lines, {1} presses" -f $lines.Count, $presses)
    return $presses
}

# Made-up log lines for -DryRun: what a run writes, plus lines and values that must NOT reach the zip.
function Add-DryRunLog([string]$LogFolder, [string]$Purpose) {
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
        $isKitText = ($file.Extension -eq '.json') -or ($file.Extension -eq '.txt') -or ($file.Name -eq 'markers.csv') -or ($file.Name -eq 'camera.csv')
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
    soak = [bool]$Soak; soakMinutes = $SoakMinutes; impairment = [bool]$Impairment
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
    $seconds = $SoakMinutes * 60
    if ($DryRun) { $seconds = 3 }
    [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'soak' $seconds "Soak: arm '$($arm.name)' for $SoakMinutes minutes")
}
if ($Impairment -and (-not $stop)) {
    $arm = $table | Where-Object { $_.name -eq $ImpairmentArm } | Select-Object -First 1
    if (-not $arm) { throw "Unknown impairment arm '$ImpairmentArm'" }
    [void](Invoke-ArmRun $arm (Get-ArmExe $arm) 'impairment' 0 "Impairment and recovery: arm '$($arm.name)'")
}

if ($DryRun) { Remove-Item -LiteralPath (Join-Path $evidence '_dryrun') -Recurse -Force -ErrorAction SilentlyContinue }
$zipPath = Publish-Evidence $evidence
Write-Host ''
Write-Host 'Camera measurements: fill in camera.csv in the evidence folder, then run this script with -PackOnly <that folder>.'
Write-Host ('Report (needs Python 3): python tools\remoteplay_latency_bench\report.py "' + $evidence + '"')
