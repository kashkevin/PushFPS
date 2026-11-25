<#
.SYNOPSIS
  PushFPS capture-stall evidence: what else happened on this PC when PushFPS's capture card stalled. READ-ONLY.

.DESCRIPTION
  Run it AFTER playing: double-click Collect-Stall-Evidence.cmd. (It may run while you play, but it reads a lot for half a
  minute to two minutes, so after playing is better.) No administrator rights are needed and nothing on the PC is changed:
  no setting, no registry value, no service, no scheduled task, no event log is switched on or off. The script lowers only
  its OWN priority (BelowNormal) so the game and PushFPS come first. The only file written is ONE text file next to
  PushFPS's own logs (stall-evidence-<date>.txt); it stays on this PC unless you send it. Every command the script runs is
  listed at the top of that file, with this script's version and fingerprint.

  Mode "report" (the default):
    - finds the capture stalls in PushFPS's own logs (veyra-qml.log and veyra-qml.old.log, the last 128 MB of each):
      callback gaps and copy spikes ([capture-callback]), clock-anchor leads and snap bursts ([live-anchor]), seconds with
      missing frames ([capture-timing]) and [capture-stall] lines of newer builds; says how long that took;
    - compares them with the quarter-hour pattern seen in the owner's logs (first stall at the quarter hour + 5.45, 11.05 or
      16.75 s by UTC quarter index mod 3, a second about 17 s later): which stalls fit, the residuals, and the next
      predicted stall times;
    - scheduled tasks: every time trigger that repeats every 1, 3, 5, 9, 15 or 45 minutes with its phase against the
      quarter hour and the stalls it would hit; tasks that last ran within 20 s of a stall; with Task Scheduler history
      enabled, every task start within 20 s of a stall (the history is NOT switched on when it is off);
    - System and Application event log entries from 30 s before to 30 s after the newest 24 stalls (one query per log);
    - the processes that used the most processor time since they started, and the running services;
    - the USB video / media devices (the capture card among them) up to their USB controller: driver provider, version and
      date, and whether Windows may turn the device off to save power, where readable;
    - the power plan's USB selective suspend, PCI Express link state and processor settings (powercfg /query);
    - network adapters' state and speed; hypervisor, memory integrity and virtual-machine worker processes.
  Masked everywhere in the file: this PC's name, your user name, account SIDs, C:\Users\<name> paths, e-mail, IP (v4 and v6)
  and hardware (MAC) addresses, device serial numbers.

  Mode "watch" (only if you choose it; light, it runs while you play): once a second, for up to 20 minutes (default: until
  90 s after the next quarter hour), a FIXED small set of Windows counters (processor DPC and interrupt time, context
  switches, pages per second, available memory) and the 5 processes that used the most processor time in that second, one
  line per second. It measures its own cost, writes it down, first drops the process list and then stops by itself when a
  second costs it more than -SampleBudgetMs twice in a row.

  Windows PowerShell 5.1 or newer. Unverified on the owner's PC; the checks workflow runs it dry on a Windows runner.

.PARAMETER Mode
  report (default) or watch.
.PARAMETER LogFolder
  PushFPS's log folder. Default: the "logs" folder of the PushFPS copy under %LOCALAPPDATA%\Programs whose log changed last.
.PARAMETER OutFile
  Where the text goes. Default: <LogFolder>\stall-evidence-<date>.txt (watch: stall-watch-<date>.txt).
.PARAMETER WatchMinutes
  watch: how long. 0 (default) = until 90 s after the next quarter hour. At most 20.
.PARAMETER SampleBudgetMs
  watch: processor time one second of watching may cost (default 30 ms).
.PARAMETER DryRun
  Uses a made-up PushFPS log in a temporary folder and short limits. Used by the checks workflow.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File Collect-Stall-Evidence.ps1
  powershell -ExecutionPolicy Bypass -File Collect-Stall-Evidence.ps1 -Mode watch
#>
param(
    [ValidateSet('report', 'watch')]
    [string]$Mode = 'report',
    [string]$LogFolder = '',
    [string]$OutFile = '',
    [int]$WatchMinutes = 0,
    [int]$WatchSeconds = 0,
    [int]$SampleBudgetMs = 30,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$toolVersion = '2'
# Fingerprint of this file: the first 12 hex digits of SHA-256 over the file with LF line ends and this value written as
# twelve zeros. tools/remoteplay_latency_bench/test_stall_witness_kit.py checks it, so a copy that was changed shows it.
$toolFingerprint = '4d38065edf4e'
$invariant = [System.Globalization.CultureInfo]::InvariantCulture
$runClock = [System.Diagnostics.Stopwatch]::StartNew()
# Our own process only, so the game and PushFPS come first (not a system change).
try { (Get-Process -Id $PID).PriorityClass = 'BelowNormal' } catch { }

# The quarter-hour model from the owner's logs (research QUARTER_HOUR_STALLS.md, 24/25 Nov 2026): first stall at the quarter
# hour + this many seconds, chosen by the quarter's index in the UTC day mod 3; the second about 17 s after the first.
$modelFirst = @{ 0 = 11.05; 1 = 16.75; 2 = 5.45 }
$modelSecond = 17.0
$modelTolerance = 1.5

# --- what this script runs (written at the top of the output) ------------------------------------------------------
$commandsReport = @(
    '.NET file read (shared, read only)  PushFPS''s own logs veyra-qml.log and veyra-qml.old.log, the last 128 MB of each',
    'Get-Process -Id <this script>, set to BelowNormal priority (this script only)',
    'Get-ScheduledTask, Get-ScheduledTaskInfo   (read)  [schtasks /query /fo csv /v only if those are unavailable]',
    'Get-WinEvent -ListLog Microsoft-Windows-TaskScheduler/Operational   (read whether it is enabled; never changed)',
    'Get-WinEvent (System, Application: one query per log for the newest 24 stalls +-30 s; TaskScheduler/Operational task starts when enabled)',
    'Get-Process, Get-Service   (read)',
    'Get-PnpDevice -PresentOnly, Get-PnpDeviceProperty DEVPKEY_Device_Parent, Get-CimInstance Win32_PnPSignedDriver (USB/media classes)   (read)',
    'Get-CimInstance -Namespace root\wmi MSPower_DeviceEnable   (read; may need administrator rights, then reported as unreadable)',
    'powercfg /getactivescheme ; powercfg /query SCHEME_CURRENT <USB, PCI Express, processor sub-groups>   (read only)',
    'Get-NetAdapter   (read; no hardware addresses kept)',
    'Get-CimInstance Win32_ComputerSystem (HypervisorPresent), root\Microsoft\Windows\DeviceGuard Win32_DeviceGuard   (read)'
)
$commandsWatch = @(
    'Get-Process -Id <this script>, set to BelowNormal priority (this script only)',
    'Get-Counter with a fixed list: \Processor(*)\% DPC Time, \Processor(*)\% Interrupt Time, \System\Context Switches/sec,',
    '    \Memory\Pages/sec, \Memory\Available MBytes   (one sample a second)',
    'Get-Process   (once a second, for the 5 largest processor-time deltas; dropped first when over budget)'
)

# --- masking: nothing of this PC's names or addresses in the file ---------------------------------------------------
$ownWords = @()
foreach ($word in @($env:COMPUTERNAME, $env:USERNAME)) { if ($word -and ($word.Length -ge 3)) { $ownWords += $word } }
function Hide-Private([string]$Text) {
    if (-not $Text) { return '' }
    $t = $Text -replace "[\r\n\t]+", ' '
    $t = $t -replace '(?i)[A-Za-z]:\\+Users\\+[^\\\s"]*', '[user-folder]'
    $t = $t -replace 'S-1-5-21-\d+-\d+-\d+(-\d+)?', '[sid]'
    $t = $t -replace '(?<![/\\\w.%+-])[A-Za-z0-9._%+-]+@(?:[A-Za-z0-9-]+\.)+[A-Za-z]{2,}\b', '[email]'
    $t = $t -replace '\b[0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}\b', '[mac]'
    $t = $t -replace '\b[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\b', '[mac]'
    $t = $t -replace '(?<![\d.])(?:(?:25[0-5]|2[0-4]\d|1?\d?\d)\.){3}(?:25[0-5]|2[0-4]\d|1?\d?\d)(?![\d.])', '[ip]'
    $t = $t -replace '(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){3,7}[0-9A-Fa-f]{1,4}(?![0-9A-Za-z:])', '[ipv6]'
    $t = $t -replace '(?<![0-9A-Za-z:])(?:[0-9A-Fa-f]{1,4}:){1,6}:(?:[0-9A-Fa-f]{1,4}(?::[0-9A-Fa-f]{1,4}){0,5})?(?![0-9A-Za-z:])', '[ipv6]'
    $t = $t -replace '\[ipv6\]%[0-9A-Za-z]+', '[ipv6]'
    foreach ($word in $ownWords) { $t = $t -replace ('(?i)\b' + [regex]::Escape($word) + '\b'), '[removed]' }
    return $t
}
# A device instance id without its last part (often a serial number).
function Hide-Serial([string]$InstanceId) {
    if (-not $InstanceId) { return '' }
    $parts = $InstanceId -split '\\'
    if ($parts.Count -ge 3) { return (($parts[0..($parts.Count - 2)] -join '\') + '\[instance]') }
    return $InstanceId
}
function Format-Utc([datetime]$Time) { return $Time.ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss.fff', $invariant) + 'Z' }
function Format-Local([datetime]$Time) { return $Time.ToLocalTime().ToString('yyyy-MM-dd HH:mm:ss', $invariant) }
function Format-Number([double]$Value, [string]$Format) { return $Value.ToString($Format, $invariant) }
function Get-Clipped([string]$Text, [int]$Max) { if ($Text.Length -gt $Max) { return $Text.Substring(0, $Max) + '...' } return $Text }

# --- where PushFPS's logs are -------------------------------------------------------------------------------------
$dryFolder = ''
if ($DryRun) {
    $dryFolder = Join-Path ([System.IO.Path]::GetTempPath()) ('pushfps-stall-dry-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $dryFolder | Out-Null
    # A made-up Package 15 log (no [capture-stall] lines): two quarter-hour stalls on the model (one timed by a late
    # callback, one only by a frame deficit with a burst after it) and one stall off the pattern; plus private values.
    $now = (Get-Date).ToUniversalTime()
    $quarter = $now.Date.AddMinutes([math]::Floor($now.TimeOfDay.TotalMinutes / 15) * 15).AddMinutes(-30)
    $index = [int]([math]::Floor($quarter.TimeOfDay.TotalMinutes / 15)) % 3
    $first = $quarter.AddSeconds($modelFirst[$index])
    $second = $first.AddSeconds($modelSecond)
    $off = $quarter.AddMinutes(7).AddSeconds(31.2)
    $made = New-Object System.Collections.Generic.List[string]
    $made.Add($quarter.AddSeconds(-50).ToString('yyyy-MM-ddTHH:mm:ss.fff', $invariant) + 'Z t=1 [INFO ] [remoteplay] made-up host=192.168.1.77 peer fe80::1ff:fe23:4567:890b%12 mac 00:1A:2B:3C:4D:5E')
    foreach ($stall in @(@{ at = $first; gap = $true }, @{ at = $second; gap = $false }, @{ at = $off; gap = $false })) {
        $received = 100000
        $t = $stall.at.AddSeconds(-8.3)
        for ($i = 0; $i -lt 16; $i++) {
            $t = $t.AddMilliseconds(1024)
            $count = 31
            if (($t -gt $stall.at) -and ($t.AddMilliseconds(-1024) -le $stall.at)) { $count = 25 }
            elseif (($t -gt $stall.at.AddMilliseconds(1024)) -and ($t -le $stall.at.AddMilliseconds(3072))) { $count = 34 }
            $received += $count
            $snaps = 0
            if ($t -gt $stall.at) { $snaps = 14 }
            $stampText = $t.ToString('yyyy-MM-ddTHH:mm:ss.fff', $invariant) + 'Z'
            $made.Add($stampText + ' t=37220 [INFO ] [capture-timing] revision=7 received=' + $received + ' processed=1 dropped=0 callbackFps=29.9 (made up)')
            $made.Add($stampText + ' t=37220 [INFO ] [live-anchor] revision=7 contentClock=false callbackJitterMs=0.9 clockDriftPpm=941 snaps=' + $snaps + ' callbackMinusAnchorMs=0.5 phaseMs=30.2 (made up)')
        }
        if ($stall.gap) {
            $made.Add($stall.at.AddMilliseconds(130).ToString('yyyy-MM-ddTHH:mm:ss.fff', $invariant) + 'Z t=42780 [INFO ] [capture-callback] source=7274 entryToLockMs=6.917 entryToCopiedMs=6.918 bytes=12441600 compressed=false arrivalDeltaMs=158.153 ptsDeltaMs=33.338 sampleDurationMs=33.333 (made up)')
        }
    }
    $made.Add($now.AddMinutes(-1).ToString('yyyy-MM-ddTHH:mm:ss.fff', $invariant) + 'Z t=42780 [INFO ] [capture-callback] source=7400 entryToLockMs=1.1 entryToCopiedMs=1.1 bytes=12441600 compressed=false arrivalDeltaMs=33.3 ptsDeltaMs=33.338 sampleDurationMs=33.333 (made up, not a stall)')
    $sorted = @($made | Sort-Object)
    Set-Content -LiteralPath (Join-Path $dryFolder 'veyra-qml.log') -Value $sorted -Encoding UTF8
    $LogFolder = $dryFolder
}
if (-not $LogFolder) {
    $best = $null
    $base = Join-Path $env:LOCALAPPDATA 'Programs'
    foreach ($folder in @(Get-ChildItem -LiteralPath $base -Directory -Filter 'PushFPS*' -ErrorAction SilentlyContinue)) {
        $log = Join-Path $folder.FullName 'logs\veyra-qml.log'
        if (Test-Path -LiteralPath $log) {
            $item = Get-Item -LiteralPath $log
            if ((-not $best) -or ($item.LastWriteTimeUtc -gt $best.LastWriteTimeUtc)) { $best = $item }
        }
    }
    if (-not $best) { throw 'No PushFPS log found under %LOCALAPPDATA%\Programs\PushFPS*\logs. Give -LogFolder.' }
    $LogFolder = $best.DirectoryName
}
if (-not (Test-Path -LiteralPath $LogFolder)) { throw ('Log folder not found: ' + $LogFolder) }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ((-not $OutFile) -and $DryRun) { $OutFile = Join-Path (Get-Location).Path ('stall-' + $Mode + '-dryrun-' + $stamp + '.txt') }
if (-not $OutFile) {
    if ($Mode -eq 'watch') { $OutFile = Join-Path $LogFolder ('stall-watch-' + $stamp + '.txt') }
    else { $OutFile = Join-Path $LogFolder ('stall-evidence-' + $stamp + '.txt') }
}

# --- the stalls in PushFPS's own logs (streaming; bounded memory and time) ------------------------------------------
# Bound: at most the last 128 MB of each of the two files, read line by line (one line in memory at a time); at most 3000
# stalls kept (the newest). The detector is the research's: a stall is a 1 s [capture-timing] window that misses 2 or more
# frames against the learned rate, a [capture-callback] that came more than 83.3 ms (2.5 frames) after the previous one, a
# [live-anchor] lead over 15 ms, or a [capture-stall] kind=gap line; events within 2.6 s are one stall; copy spikes, clock
# snaps and the frame burst after a stall are attached to it (a burst = the frames came late, none = they were lost).
$styles = [System.Globalization.DateTimeStyles]'AdjustToUniversal,AssumeUniversal'
function ConvertFrom-LogTime([string]$Line) { return [datetime]::ParseExact($Line.Substring(0, 23), 'yyyy-MM-ddTHH:mm:ss.fff', $invariant, $styles) }
$script:stallList = New-Object System.Collections.Generic.List[object]
$script:current = $null
$script:lastEvent = [datetime]::MinValue
function Close-Stall {
    if ($script:current) {
        $script:stallList.Add($script:current)
        if ($script:stallList.Count -gt 3000) { $script:stallList.RemoveAt(0) }
    }
    $script:current = $null
}
function Add-Candidate([datetime]$At, [int]$Precision, [string]$What) {
    if ($script:current -and (($At - $script:lastEvent).TotalSeconds -gt 2.6)) { Close-Stall }
    if (-not $script:current) {
        $script:current = [pscustomobject]@{ time = $At; precision = $Precision; what = $What; deficit = 0.0; burst = 0.0; snaps = 0; copyMaxMs = 0.0 }
    } elseif ($Precision -lt $script:current.precision) {
        $script:current.time = $At; $script:current.precision = $Precision; $script:current.what = $What + '; ' + $script:current.what
    } elseif ($script:current.what.IndexOf($What) -lt 0) { $script:current.what = $script:current.what + '; ' + $What }
    $script:lastEvent = $At
}
function Find-Stalls([string]$Folder) {
    $clock = [System.Diagnostics.Stopwatch]::StartNew()
    $lines = 0L; $bytes = 0L
    $prevT = $null; $prevR = 0L; $prevRev = ''; $rate = 29.97; $lastSnaps = -1L
    foreach ($name in @('veyra-qml.old.log', 'veyra-qml.log')) {
        $path = Join-Path $Folder $name
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $stream = [System.IO.File]::Open($path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]'ReadWrite,Delete')
        try {
            $from = [math]::Max(0L, $stream.Length - 128MB)
            [void]$stream.Seek($from, [System.IO.SeekOrigin]::Begin)
            $bytes += $stream.Length - $from
            $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
            if ($from -gt 0) { [void]$reader.ReadLine() }
            $prevT = $null
            while ($null -ne ($line = $reader.ReadLine())) {
                $lines++
                if ($line.Length -lt 60) { continue }
                if ($line.Contains('[capture-timing] ')) {
                    if ($line -notmatch 'revision=(\d+) received=(\d+)') { continue }
                    $rev = $Matches[1]; $received = [long]$Matches[2]; $t = ConvertFrom-LogTime $line
                    if ($prevT -and ($rev -eq $prevRev)) {
                        $dt = ($t - $prevT).TotalSeconds
                        if (($dt -gt 0.5) -and ($dt -lt 1.6) -and ($received -ge $prevR)) {
                            $deficit = $dt * $rate - ($received - $prevR)
                            if ($deficit -ge 2.0) {
                                Add-Candidate $prevT.AddSeconds($dt / 2) 2 ('missing ' + (Format-Number $deficit '0.0') + ' frames in a 1 s window')
                                $script:current.deficit += $deficit
                            } elseif (($deficit -le -2.5) -and $script:current -and (($t - $script:lastEvent).TotalSeconds -le 4.5)) {
                                $script:current.burst += -$deficit
                            } elseif ([math]::Abs($deficit) -lt 1.5) {
                                $rate += 0.02 * (($received - $prevR) / $dt - $rate)
                            }
                        }
                    }
                    $prevT = $t; $prevR = $received; $prevRev = $rev
                } elseif ($line.Contains('[capture-callback] ')) {
                    if ($line -notmatch 'entryToLockMs=([\d.]+) .*arrivalDeltaMs=([\d.]+)') { continue }
                    $lock = [double]::Parse($Matches[1], $invariant); $arrival = [double]::Parse($Matches[2], $invariant)
                    $t = ConvertFrom-LogTime $line
                    if ($arrival -gt 83.3) {
                        Add-Candidate $t.AddMilliseconds(-$lock - $arrival + 33.34) 0 ('callback ' + (Format-Number $arrival '0') + ' ms late, copy ' + (Format-Number $lock '0.0') + ' ms')
                    }
                    if ($lock -gt 3.0) {
                        if (-not ($script:current -and ([math]::Abs(($t - $script:lastEvent).TotalSeconds) -le 3.0))) { Add-Candidate $t 1 ('copy ' + (Format-Number $lock '0.0') + ' ms') }
                        $script:current.copyMaxMs = [math]::Max($script:current.copyMaxMs, $lock)
                    }
                } elseif ($line.Contains('[live-anchor] ')) {
                    if ($line -notmatch 'snaps=(\d+) callbackMinusAnchorMs=(-?[\d.]+)') { continue }
                    $snaps = [long]$Matches[1]; $lead = [double]::Parse($Matches[2], $invariant); $t = ConvertFrom-LogTime $line
                    if ($lead -gt 15) { Add-Candidate $t.AddMilliseconds(-$lead) 1 ('clock lead ' + (Format-Number $lead '0') + ' ms') }
                    if (($lastSnaps -ge 0) -and ($snaps -gt $lastSnaps) -and $script:current -and (($t - $script:lastEvent).TotalSeconds -le 4.5)) { $script:current.snaps += [int]($snaps - $lastSnaps) }
                    $lastSnaps = $snaps
                } elseif ($line.Contains('[capture-stall] kind=gap')) {
                    if ($line -notmatch 'gapMs=([\d.]+)') { continue }
                    $gap = [double]::Parse($Matches[1], $invariant)
                    Add-Candidate (ConvertFrom-LogTime $line).AddMilliseconds(-$gap + 33.34) 0 ('[capture-stall] gap ' + (Format-Number $gap '0') + ' ms')
                }
            }
        } finally { $stream.Dispose() }
    }
    Close-Stall
    # A lone copy spike (no missing frames, no late callback) is not counted as a stall.
    $result = @($script:stallList | Where-Object { ($_.precision -ne 1) -or ($_.what -notmatch '^copy ') -or ($_.deficit -gt 0) })
    return [pscustomobject]@{ stalls = $result; lines = $lines; megabytes = $bytes / 1MB; seconds = $clock.Elapsed.TotalSeconds }
}
function Get-ModelFit([datetime]$Utc) {
    $secondOfDay = $Utc.TimeOfDay.TotalSeconds
    $quarterIndex = [int][math]::Floor($secondOfDay / 900)
    $offset = $secondOfDay - $quarterIndex * 900
    $first = $modelFirst[$quarterIndex % 3]
    $r1 = $offset - $first; $r2 = $offset - ($first + $modelSecond)
    $residual = $r1; $which = 'first'
    if ([math]::Abs($r2) -lt [math]::Abs($r1)) { $residual = $r2; $which = 'second' }
    return [pscustomobject]@{ offset = $offset; residual = $residual; which = $which; fits = ([math]::Abs($residual) -le $modelTolerance) }
}
function Get-Median([double[]]$Values) {
    if ($Values.Count -eq 0) { return -1 }
    $s = @($Values | Sort-Object)
    if ($s.Count % 2) { return $s[[int][math]::Floor($s.Count / 2)] }
    return ($s[$s.Count / 2 - 1] + $s[$s.Count / 2]) / 2
}

# ======================================================================================================================
# watch
# ======================================================================================================================
if ($Mode -eq 'watch') {
    $limitSeconds = 0
    if ($WatchSeconds -gt 0) { $limitSeconds = [math]::Min($WatchSeconds, 1200) }
    elseif ($WatchMinutes -gt 0) { $limitSeconds = [math]::Min($WatchMinutes, 20) * 60 }
    else {
        $now = Get-Date
        $nextQuarter = $now.Date.AddMinutes(([math]::Floor($now.TimeOfDay.TotalMinutes / 15) + 1) * 15)
        $limitSeconds = [math]::Min([int][math]::Ceiling(($nextQuarter - $now).TotalSeconds) + 90, 1200)
    }
    $header = @(
        ('PushFPS stall watch, script version ' + $toolVersion + ' fingerprint ' + $toolFingerprint + '   started ' + (Format-Utc (Get-Date)) + '   for at most ' + $limitSeconds + ' s   budget ' + $SampleBudgetMs + ' ms of processor time a second'),
        'This file stays on this PC unless you send it. READ-ONLY; this script runs at BelowNormal priority. Commands it uses:'
    )
    foreach ($c in $commandsWatch) { $header += ('  ' + $c) }
    $header += @('', 'Columns: UTC time; dpcTot/isrTot = % of all processors; dpcMax/isrMax = the busiest processor (@ its number); ctx = context switches/s; pages = pages/s; availMb; selfCpuMs = what this second cost this script (Windows counts it in steps of about 15.6 ms); top = 5 largest processor-time deltas (ms in this second)', '')
    Set-Content -LiteralPath $OutFile -Value $header -Encoding UTF8
    $set = @('\Processor(*)\% DPC Time', '\Processor(*)\% Interrupt Time', '\System\Context Switches/sec', '\Memory\Pages/sec', '\Memory\Available MBytes')
    $self = Get-Process -Id $PID
    $lastSelf = $self.TotalProcessorTime.TotalMilliseconds
    $lastCpu = @{}
    $useProcesses = $true
    $over = 0
    $written = 0
    $stopReason = 'time limit reached'
    try {
        do {
            Get-Counter -Counter $set -SampleInterval 1 -MaxSamples $limitSeconds -ErrorAction Stop | ForEach-Object {
                $sample = $_
                $dpcTot = -1; $isrTot = -1; $dpcMax = -1; $isrMax = -1; $dpcAt = ''; $isrAt = ''; $ctx = -1; $pages = -1; $avail = -1
                foreach ($v in $sample.CounterSamples) {
                    $p = $v.Path
                    if ($p -match '\\processor\((.+)\)\\% dpc time$') {
                        if ($Matches[1] -eq '_total') { $dpcTot = $v.CookedValue } elseif ($v.CookedValue -gt $dpcMax) { $dpcMax = $v.CookedValue; $dpcAt = $Matches[1] }
                    } elseif ($p -match '\\processor\((.+)\)\\% interrupt time$') {
                        if ($Matches[1] -eq '_total') { $isrTot = $v.CookedValue } elseif ($v.CookedValue -gt $isrMax) { $isrMax = $v.CookedValue; $isrAt = $Matches[1] }
                    } elseif ($p -match 'context switches/sec$') { $ctx = $v.CookedValue }
                    elseif ($p -match 'pages/sec$') { $pages = $v.CookedValue }
                    elseif ($p -match 'available mbytes$') { $avail = $v.CookedValue }
                }
                $top = '-'
                if ($useProcesses) {
                    $rows = @()
                    foreach ($proc in @(Get-Process -ErrorAction SilentlyContinue)) {
                        $cpu = $null
                        try { $cpu = $proc.TotalProcessorTime.TotalMilliseconds } catch { $cpu = $null }
                        if ($null -eq $cpu) { continue }
                        $key = [string]$proc.Id + ':' + $proc.ProcessName
                        if ($lastCpu.ContainsKey($key)) { $rows += [pscustomobject]@{ name = $proc.ProcessName; ms = ($cpu - $lastCpu[$key]) } }
                        $lastCpu[$key] = $cpu
                    }
                    $top = (@($rows | Where-Object { $_.ms -gt 0 } | Sort-Object ms -Descending | Select-Object -First 5) | ForEach-Object { $_.name + ':' + (Format-Number $_.ms '0.0') }) -join ','
                    if (-not $top) { $top = '-' }
                }
                $self.Refresh()
                $selfNow = $self.TotalProcessorTime.TotalMilliseconds
                $selfMs = $selfNow - $lastSelf
                $lastSelf = $selfNow
                $line = [string]::Format($invariant, '{0} dpcTot={1:F2} dpcMax={2:F2}@{3} isrTot={4:F2} isrMax={5:F2}@{6} ctx={7:F0} pages={8:F0} availMb={9:F0} selfCpuMs={10:F1} top={11}',
                                         (Format-Utc $sample.Timestamp), $dpcTot, $dpcMax, $dpcAt, $isrTot, $isrMax, $isrAt, $ctx, $pages, $avail, $selfMs, $top)
                [System.IO.File]::AppendAllText($OutFile, (Hide-Private $line) + "`r`n")
                $written++
                if ($written -gt 1 -and $selfMs -gt $SampleBudgetMs) { $over++ } else { $over = 0 }
                if ($over -ge 2) {
                    if ($useProcesses) {
                        $useProcesses = $false; $over = 0
                        [System.IO.File]::AppendAllText($OutFile, ('# two seconds over the budget of ' + $SampleBudgetMs + ' ms: the process list is dropped from here on' + "`r`n"))
                    } else {
                        $stopReason = 'stopped by itself: two seconds over the budget of ' + $SampleBudgetMs + ' ms without the process list'
                        break
                    }
                }
            }
        } while ($false)
    } catch {
        $stopReason = 'stopped: ' + $_.Exception.Message + ' (the counter names are English; on a Windows in another language Get-Counter may not know them)'
    }
    [System.IO.File]::AppendAllText($OutFile, ('# ' + $written + ' second(s) written; ' + (Hide-Private $stopReason) + '; ended ' + (Format-Utc (Get-Date)) + '; ran ' + (Format-Number $runClock.Elapsed.TotalSeconds '0.0') + ' s' + "`r`n"))
    Write-Host ('Stall watch written to ' + $OutFile)
    if ($dryFolder) { Remove-Item -LiteralPath $dryFolder -Recurse -Force -ErrorAction SilentlyContinue }
    return
}

# ======================================================================================================================
# report
# ======================================================================================================================
$out = New-Object System.Collections.Generic.List[string]
function Add-Line([string]$Text) { $script:out.Add((Hide-Private $Text)) }
function Add-Section([string]$Title) { $script:out.Add(''); $script:out.Add('=== ' + $Title + ' ===') }
$started = Get-Date
$out.Add('PushFPS stall evidence, script version ' + $toolVersion + ' fingerprint ' + $toolFingerprint + '   written ' + (Format-Utc $started) + '   (local time zone: UTC' + ([System.TimeZoneInfo]::Local.GetUtcOffset($started)).ToString() + ')')
$out.Add('WHAT THIS FILE IS: the capture stalls PushFPS logged on this PC, compared with the quarter-hour pattern, and what else')
$out.Add('  Windows recorded around them (scheduled tasks, event log entries, busy programs, the capture card''s USB path, power')
$out.Add('  settings). It stays on this PC unless you send it. Names of this PC and its user, IP and hardware addresses are masked.')
$out.Add('Best run AFTER playing (allowed while playing, not recommended). This script ran at BelowNormal priority.')
if ($DryRun) { $out.Add('DRY RUN: the stall times come from a made-up log; everything else is this PC.'); Add-Line ('masking self-test: 192.168.1.77 fe80::1ff:fe23:4567:890b%12 00:1A:2B:3C:4D:5E ' + $env:USERNAME + ' end') }
$out.Add('READ-ONLY. Nothing was changed on this PC. Commands this run uses:')
foreach ($c in $commandsReport) { $out.Add('  ' + $c) }

# 1. stalls and the model
Add-Section 'CAPTURE STALLS FROM PUSHFPS''S LOGS (UTC)'
$found = Find-Stalls $LogFolder
$all = @($found.stalls)
Add-Line ('  read ' + $found.lines + ' lines (' + (Format-Number $found.megabytes '0.0') + ' MB) in ' + (Format-Number $found.seconds '0.0') + ' s; bound: the last 128 MB of each file, one line in memory at a time')
$recent = @($all | Where-Object { $_.time -ge (Get-Date).ToUniversalTime().AddHours(-48) })
if ($recent.Count -gt 200) { $recent = @($recent[($recent.Count - 200)..($recent.Count - 1)]) }
Add-Line ('  ' + $all.Count + ' stall(s) found; listed: the ' + $recent.Count + ' of the last 48 hours (newest 200 at most)')
Add-Line '  precision: ms = a late callback timed it; ~0.05 s = the clock anchor or a copy spike; 1 s = only a window with missing frames'
$residuals = @()
$fits = 0
foreach ($s in $recent) {
    $fit = Get-ModelFit $s.time
    if ($fit.fits) { $fits++; $residuals += [math]::Abs($fit.residual) }
    $kind = 'frames lost'
    if ($s.burst -ge 3) { $kind = 'frames late (burst ' + (Format-Number $s.burst '0') + ')' } elseif ($s.deficit -le 0) { $kind = 'frame count unknown' }
    $mark = '  '
    if ($fit.fits) { $mark = 'Q ' }
    $precision = @('ms', '~0.05 s', '1 s')[$s.precision]
    $copyText = ''
    if ($s.copyMaxMs -gt 0) { $copyText = '  copy max ' + (Format-Number $s.copyMaxMs '0.0') + ' ms' }
    Add-Line ('  ' + $mark + (Format-Utc $s.time) + '  [' + $precision + ']  quarter+' + (Format-Number $fit.offset '0.00') + ' s  model ' + $fit.which + ' residual ' + (Format-Number $fit.residual '+0.00;-0.00') + ' s  ' + $kind + '  snaps ' + $s.snaps + $copyText + '  (' + $s.what + ')')
}
Add-Section 'THE QUARTER-HOUR PATTERN'
Add-Line ('  model: first stall at the quarter hour + ' + $modelFirst[2] + ' / ' + $modelFirst[0] + ' / ' + $modelFirst[1] + ' s for UTC quarter index mod 3 = 2 / 0 / 1, second ' + $modelSecond + ' s later; a stall fits within +-' + $modelTolerance + ' s')
if ($recent.Count -gt 0) {
    $maxText = '-'
    if ($residuals.Count -gt 0) { $maxText = Format-Number (@($residuals | Sort-Object)[-1]) '0.00' }
    Add-Line ('  fit: ' + $fits + ' of ' + $recent.Count + ' stalls (marked Q); |residual| median ' + (Format-Number (Get-Median $residuals) '0.00') + ' s, max ' + $maxText + ' s (1 s-window stalls carry +-0.5 s)')
    if (($fits -ge 4) -and ($fits * 2 -ge $recent.Count)) { Add-Line '  => the pattern IS still there in these logs.' }
    elseif ($fits -ge 1) { Add-Line ('  => ' + $fits + ' stall(s) fit the pattern; too few, or most do not, to call it still there.') }
    else { Add-Line '  => the pattern is NOT seen in these logs.' }
} else { Add-Line '  no stalls to fit.' }
Add-Line '  predicted next stalls (if the pattern holds), UTC and local:'
$nowUtc = (Get-Date).ToUniversalTime()
$q = $nowUtc.Date.AddMinutes([math]::Floor($nowUtc.TimeOfDay.TotalMinutes / 15) * 15)
for ($k = 0; $k -lt 8; $k++) {
    $qk = $q.AddMinutes(15 * $k)
    $firstAt = $qk.AddSeconds($modelFirst[[int]([math]::Floor($qk.TimeOfDay.TotalMinutes / 15)) % 3])
    if ($firstAt.AddSeconds($modelSecond) -lt $nowUtc) { continue }
    Add-Line ('    ' + (Format-Utc $firstAt) + ' and ' + (Format-Utc $firstAt.AddSeconds($modelSecond)) + '   local ' + (Format-Local $firstAt) + ' and ' + (Format-Local $firstAt.AddSeconds($modelSecond)))
}
# The newest 24 stalls are compared with event logs.
$newest = @($recent | Select-Object -Last 24)

# 2. scheduled tasks
Add-Section 'SCHEDULED TASKS'
Add-Line '  (A) time triggers repeating every 1, 3, 5, 9, 15 or 45 minutes (a divisor of the 45-minute cycle): their phase against the'
Add-Line '      quarter hour, and how many of the listed stalls a run lands within 20 s of. (B) tasks whose last run was within 20 s of a'
Add-Line '      listed stall. (C) all other tasks.'
$listA = @(); $listB = @(); $listC = @()
try {
    foreach ($task in @(Get-ScheduledTask -ErrorAction Stop)) {
        $info = $null
        $infoNote = ''
        try { $info = Get-ScheduledTaskInfo -InputObject $task -ErrorAction Stop } catch { $info = $null; $infoNote = ' (run times unreadable for this account)' }
        $name = $task.TaskPath + $task.TaskName
        $triggerText = @()
        $repeating = @()
        foreach ($tr in @($task.Triggers)) {
            if (-not $tr) { continue }
            $text = ($tr.CimClass.CimClassName -replace '^MSFT_Task', '' -replace 'Trigger$', '')
            if ($tr.StartBoundary) { $text += ' from ' + $tr.StartBoundary }
            if ($tr.Repetition -and $tr.Repetition.Interval) {
                $text += ' every ' + $tr.Repetition.Interval
                $minutes = 0
                try { $minutes = [System.Xml.XmlConvert]::ToTimeSpan($tr.Repetition.Interval).TotalMinutes } catch { $minutes = 0 }
                if (($minutes -ge 1) -and ($minutes -le 45) -and ([math]::Abs($minutes - [math]::Round($minutes)) -lt 0.001) -and ((45 % [int]$minutes) -eq 0) -and $tr.StartBoundary) {
                    $startAt = $null
                    try { $startAt = [datetime]::Parse($tr.StartBoundary, $invariant, [System.Globalization.DateTimeStyles]'AssumeLocal,AdjustToUniversal') } catch { $startAt = $null }
                    if ($startAt) { $repeating += [pscustomobject]@{ minutes = [int]$minutes; start = $startAt; enabled = $tr.Enabled } }
                }
            }
            if (-not $tr.Enabled) { $text += ' (disabled)' }
            $triggerText += $text
        }
        $last = ''; $next = ''; $result = ''; $lastNear = @()
        if ($info) {
            if ($info.LastRunTime -and $info.LastRunTime.Year -gt 2000) {
                $last = (Format-Utc $info.LastRunTime) + ' [' + $info.LastRunTime.Kind + ' ' + $info.LastRunTime.ToString('o', $invariant) + ']'
                foreach ($s in $recent) { if ([math]::Abs(($info.LastRunTime.ToUniversalTime() - $s.time).TotalSeconds) -le 20) { $lastNear += (Format-Utc $s.time) } }
            }
            if ($info.NextRunTime -and $info.NextRunTime.Year -gt 2000) { $next = Format-Utc $info.NextRunTime }
            $result = '0x' + ([int64]$info.LastTaskResult).ToString('X', $invariant)
        }
        $row = $name + '  state=' + $task.State + '  last=' + $last + '  next=' + $next + '  result=' + $result + $infoNote + '  triggers: ' + ($triggerText -join '; ')
        if ($repeating.Count -gt 0) {
            $detail = @()
            foreach ($r in $repeating) {
                $period = $r.minutes * 60
                $hits = 0
                foreach ($s in $recent) {
                    $k = [math]::Round(($s.time - $r.start).TotalSeconds / $period)
                    $run = $r.start.AddSeconds($k * $period)
                    if ([math]::Abs(($run - $s.time).TotalSeconds) -le 20) { $hits++ }
                }
                $phase = (($r.start.TimeOfDay.TotalSeconds % 900) + 900) % 900
                if ((900 % $period) -eq 0) { $phaseText = 'runs at quarter+' + (Format-Number ($phase % $period) '0') + ' s and every ' + $r.minutes + ' min after' }
                else {
                    $runs = @()
                    $k0 = [math]::Ceiling(($nowUtc - $r.start).TotalSeconds / $period)
                    for ($j = 0; $j -lt 3; $j++) { $run = $r.start.AddSeconds(($k0 + $j) * $period); $runs += ((Format-Utc $run) + ' = quarter+' + (Format-Number ($run.TimeOfDay.TotalSeconds % 900) '0') + ' s') }
                    $phaseText = 'next runs ' + ($runs -join ', ')
                }
                $disabledText = ''
                if (-not $r.enabled) { $disabledText = ' (trigger disabled)' }
                $detail += ('every ' + $r.minutes + ' min from ' + (Format-Utc $r.start) + ': ' + $phaseText + '; within 20 s of ' + $hits + ' of ' + $recent.Count + ' stalls' + $disabledText)
            }
            $listA += ('  A ' + $row + '  >> ' + ($detail -join ' | '))
        } elseif ($lastNear.Count -gt 0) {
            $listB += ('  B ' + $row + '  >> last run within 20 s of the stall ' + ($lastNear -join ', '))
        } else { $listC += ('    ' + $row) }
    }
    Add-Line ('  ' + ($listA.Count + $listB.Count + $listC.Count) + ' task(s): ' + $listA.Count + ' in A, ' + $listB.Count + ' in B')
    foreach ($r in $listA) { Add-Line $r }
    foreach ($r in $listB) { Add-Line $r }
    foreach ($r in $listC) { Add-Line $r }
} catch {
    Add-Line ('  Get-ScheduledTask unavailable (' + $_.Exception.Message + '): schtasks /query instead')
    try {
        $csv = @(schtasks.exe /query /fo csv /v 2>$null | ConvertFrom-Csv)
        foreach ($r in $csv) {
            if ($r.TaskName -eq 'TaskName') { continue }
            Add-Line ('  ' + $r.TaskName + '  status=' + $r.Status + '  last=' + $r.'Last Run Time' + '  next=' + $r.'Next Run Time' + '  result=' + $r.'Last Result' + '  schedule=' + $r.'Schedule Type' + ' ' + $r.'Repeat: Every')
        }
    } catch { Add-Line ('  schtasks failed too: ' + $_.Exception.Message) }
}

# 3. Task Scheduler history: every task start near a stall
Add-Section 'TASK SCHEDULER HISTORY (Microsoft-Windows-TaskScheduler/Operational)'
$tsEnabled = $false
try {
    $tsLog = Get-WinEvent -ListLog 'Microsoft-Windows-TaskScheduler/Operational' -ErrorAction Stop
    $tsEnabled = [bool]$tsLog.IsEnabled
    Add-Line ('  enabled=' + $tsEnabled + '   records=' + $tsLog.RecordCount)
    if (-not $tsEnabled) { Add-Line '  The history is switched off on this PC. It was NOT switched on (that would be a change). Task times then come only from the list above.' }
} catch { Add-Line ('  could not be read: ' + $_.Exception.Message) }
if ($tsEnabled -and ($recent.Count -gt 0)) {
    try {
        $tsEvents = @(Get-WinEvent -FilterHashtable @{ LogName = 'Microsoft-Windows-TaskScheduler/Operational'; Id = @(100, 129, 200); StartTime = $recent[0].time.AddSeconds(-30).ToLocalTime(); EndTime = $recent[-1].time.AddSeconds(30).ToLocalTime() } -MaxEvents 20000 -ErrorAction Stop)
        $near = 0
        foreach ($e in @($tsEvents | Sort-Object TimeCreated)) {
            $when = $e.TimeCreated.ToUniversalTime()
            foreach ($s in $recent) {
                $d = ($when - $s.time).TotalSeconds
                if ([math]::Abs($d) -le 20) {
                    $taskName = ''
                    try { $taskName = [string]$e.Properties[0].Value } catch { $taskName = '' }
                    Add-Line ('  ' + (Format-Utc $when) + '  ' + (Format-Number $d '+0.0;-0.0') + ' s from the stall ' + (Format-Utc $s.time) + '  id=' + $e.Id + '  ' + $taskName)
                    $near++
                    break
                }
            }
        }
        Add-Line ('  ' + $tsEvents.Count + ' task start event(s) in the stalls'' span; ' + $near + ' within 20 s of a stall')
    } catch { if ($_.Exception.Message -notmatch 'No events were found') { Add-Line ('  could not be read: ' + $_.Exception.Message) } else { Add-Line '  no task starts in the stalls'' span' } }
}

# 4. events around the newest 24 stalls: one query per log (time windows merged and OR-ed)
Add-Section 'EVENT LOG ENTRIES WITHIN 30 S OF THE NEWEST 24 STALLS'
$windows = New-Object System.Collections.Generic.List[object]
foreach ($s in $newest) {
    $a = $s.time.AddSeconds(-30); $b = $s.time.AddSeconds(30)
    if (($windows.Count -gt 0) -and ($a -le $windows[$windows.Count - 1].end)) { $windows[$windows.Count - 1].end = $b }
    else { $windows.Add([pscustomobject]@{ start = $a; end = $b }) }
}
if ($windows.Count -eq 0) { Add-Line '  no stalls' }
else {
    $clauses = @($windows | ForEach-Object { "(TimeCreated[@SystemTime>='" + $_.start.ToString('yyyy-MM-ddTHH:mm:ss.fff', $invariant) + "Z' and @SystemTime<='" + $_.end.ToString('yyyy-MM-ddTHH:mm:ss.fff', $invariant) + "Z'])" })
    $xpath = '*[System[' + ($clauses -join ' or ') + ']]'
    $maxEvents = 600
    if ($DryRun) { $maxEvents = 50 }
    foreach ($logName in @('System', 'Application')) {
        $events = @()
        try { $events = @(Get-WinEvent -LogName $logName -FilterXPath $xpath -MaxEvents $maxEvents -ErrorAction Stop) }
        catch { if ($_.Exception.Message -notmatch 'No events were found') { Add-Line ('  ' + $logName + ': could not be read: ' + $_.Exception.Message) } }
        $limitText = ''
        if ($events.Count -ge $maxEvents) { $limitText = ' (limit reached)' }
        Add-Line ('  -- ' + $logName + ': ' + $events.Count + ' entries in ' + $windows.Count + ' window(s)' + $limitText)
        foreach ($e in @($events | Sort-Object TimeCreated)) {
            $when = $e.TimeCreated.ToUniversalTime()
            $closest = $newest[0].time
            foreach ($s in $newest) { if ([math]::Abs(($when - $s.time).TotalSeconds) -lt [math]::Abs(($when - $closest).TotalSeconds)) { $closest = $s.time } }
            $message = ''
            try { $message = [string]$e.Message } catch { $message = '' }
            Add-Line ('     ' + (Format-Utc $when) + '  ' + (Format-Number ($when - $closest).TotalSeconds '+0.0;-0.0') + ' s  ' + $e.ProviderName + '  id=' + $e.Id + '  level=' + $e.LevelDisplayName + '  ' + (Get-Clipped $message 220))
        }
    }
}

# 5. processes and services
Add-Section 'PROCESSES (most processor time since they started; start times in UTC)'
try {
    $procs = @(Get-Process -ErrorAction Stop | Sort-Object CPU -Descending | Select-Object -First 40)
    foreach ($p in $procs) {
        $startText = ''
        try { $startText = Format-Utc $p.StartTime } catch { $startText = '?' }
        Add-Line ('  ' + $p.ProcessName.PadRight(36) + ' cpuS=' + (Format-Number ([double]$p.CPU) '0.0').PadLeft(10) + '  started=' + $startText)
    }
} catch { Add-Line ('  could not be read: ' + $_.Exception.Message) }
Add-Section 'SERVICES RUNNING'
try {
    $running = @(Get-Service -ErrorAction Stop | Where-Object { $_.Status -eq 'Running' } | Sort-Object Name)
    Add-Line ('  ' + $running.Count + ' running: ' + (($running | ForEach-Object { $_.Name }) -join ', '))
} catch { Add-Line ('  could not be read: ' + $_.Exception.Message) }

# 6. the capture device's path
Add-Section 'USB VIDEO / MEDIA DEVICES AND THEIR PATH TO THE USB CONTROLLER'
$drivers = @{}
try {
    $filter = "DeviceClass='USB' OR DeviceClass='MEDIA' OR DeviceClass='CAMERA' OR DeviceClass='IMAGE' OR DeviceClass='SYSTEM' OR DeviceClass='HDC'"
    foreach ($d in @(Get-CimInstance Win32_PnPSignedDriver -Filter $filter -ErrorAction Stop)) { if ($d.DeviceID) { $drivers[$d.DeviceID.ToUpperInvariant()] = $d } }
} catch { Add-Line ('  drivers could not be read: ' + $_.Exception.Message) }
$powerAllowed = @{}
$powerNote = ''
try {
    foreach ($m in @(Get-CimInstance -Namespace root\wmi -ClassName MSPower_DeviceEnable -ErrorAction Stop)) { $powerAllowed[($m.InstanceName -replace '_\d+$', '').ToUpperInvariant()] = $m.Enable }
} catch { $powerNote = 'not readable here (' + $_.Exception.Message + ')' }
function Write-Device([string]$Id, [string]$Indent) {
    $dev = $null
    try { $dev = Get-PnpDevice -InstanceId $Id -ErrorAction Stop } catch { $dev = $null }
    $name = ''; $class = ''; $status = ''
    if ($dev) { $name = $dev.FriendlyName; $class = $dev.Class; $status = $dev.Status }
    $drv = $drivers[$Id.ToUpperInvariant()]
    $driverText = 'driver ?'
    if ($drv) {
        $date = ''
        if ($drv.DriverDate) { try { $date = ([datetime]$drv.DriverDate).ToString('yyyy-MM-dd', $invariant) } catch { $date = '' } }
        $driverText = 'driver ' + $drv.DriverProviderName + ' ' + $drv.DriverVersion + ' ' + $date
    }
    $power = 'may Windows turn it off to save power: '
    if ($powerNote) { $power += $powerNote } elseif ($powerAllowed.ContainsKey($Id.ToUpperInvariant())) { $power += [string]$powerAllowed[$Id.ToUpperInvariant()] } else { $power += 'no setting' }
    Add-Line ($Indent + $name + '  [' + $class + ', ' + $status + ']  ' + (Hide-Serial $Id) + '  ' + $driverText + '  ' + $power)
}
try {
    $video = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { ($_.Class -in @('Camera', 'Image', 'Media')) -and ($_.InstanceId -like 'USB*') })
    if ($video.Count -eq 0) { Add-Line '  none found' }
    foreach ($v in $video) {
        Write-Device $v.InstanceId '  '
        $id = $v.InstanceId
        for ($level = 1; $level -le 6; $level++) {
            $parent = $null
            try { $parent = (Get-PnpDeviceProperty -InstanceId $id -KeyName 'DEVPKEY_Device_Parent' -ErrorAction Stop).Data } catch { $parent = $null }
            if ((-not $parent) -or ($parent -like 'HTREE*')) { break }
            Write-Device $parent ('  ' + ('  ' * $level) + '<- ')
            $id = $parent
        }
    }
} catch { Add-Line ('  could not be read: ' + $_.Exception.Message) }

# 7. power plan
Add-Section 'POWER PLAN (powercfg, read only)'
$powerQueries = @(
    @('', '/getactivescheme'),
    @('USB settings (selective suspend)', '2a737441-1930-4402-8d77-b2bebba308a3'),
    @('PCI Express (link state power management)', '501a4d13-42af-4429-9fd1-a8218c268e20'),
    @('Processor power management', '54533251-82be-4824-96c1-47b60b740d00')
)
foreach ($pq in $powerQueries) {
    try {
        if ($pq[1] -eq '/getactivescheme') { $text = @(powercfg.exe /getactivescheme 2>&1) }
        else { Add-Line ('  -- ' + $pq[0]); $text = @(powercfg.exe /query SCHEME_CURRENT $pq[1] 2>&1) }
        foreach ($l in $text) { if ([string]$l -match '\S') { Add-Line ('  ' + ([string]$l).Trim()) } }
    } catch { Add-Line ('  powercfg failed: ' + $_.Exception.Message) }
}

# 8. network
Add-Section 'NETWORK ADAPTERS'
try {
    foreach ($n in @(Get-NetAdapter -ErrorAction Stop | Sort-Object Status)) {
        Add-Line ('  ' + $n.InterfaceDescription + '  status=' + $n.Status + '  speed=' + $n.LinkSpeed + '  media=' + $n.MediaType)
    }
} catch { Add-Line ('  could not be read: ' + $_.Exception.Message) }

# 9. hypervisor and virtual machines
Add-Section 'HYPERVISOR AND VIRTUAL MACHINES'
try { Add-Line ('  hypervisor present (Win32_ComputerSystem.HypervisorPresent): ' + (Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).HypervisorPresent + ' (also true when only memory integrity / VBS uses it)') } catch { Add-Line ('  Win32_ComputerSystem not readable: ' + $_.Exception.Message) }
try {
    $guard = Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard -ErrorAction Stop
    Add-Line ('  virtualization-based security status=' + $guard.VirtualizationBasedSecurityStatus + ' (0 off, 1 configured, 2 running)  services running=' + (@($guard.SecurityServicesRunning) -join ','))
} catch { Add-Line ('  Device Guard status not readable: ' + $_.Exception.Message) }
$vmNames = @('vmmem', 'vmmemWSL', 'vmwp', 'vmcompute', 'vmms', 'wslhost', 'wslservice', 'VirtualBoxVM', 'VBoxHeadless', 'vmware-vmx', 'qemu-system-x86_64')
$vms = @(Get-Process -Name $vmNames -ErrorAction SilentlyContinue)
Add-Line ('  virtual-machine worker processes running: ' + $vms.Count)
foreach ($p in $vms) {
    $startText = ''
    try { $startText = Format-Utc $p.StartTime } catch { $startText = '?' }
    Add-Line ('    ' + $p.ProcessName + '  cpuS=' + (Format-Number ([double]$p.CPU) '0.0') + '  started=' + $startText)
}

$out.Add('')
$out.Add('# done: this run took ' + (Format-Number $runClock.Elapsed.TotalSeconds '0.0') + ' s in all (the log parse ' + (Format-Number $found.seconds '0.0') + ' s)')
Set-Content -LiteralPath $OutFile -Value $out -Encoding UTF8
Write-Host ('Stall evidence written to ' + $OutFile)
if ($dryFolder) { Remove-Item -LiteralPath $dryFolder -Recurse -Force -ErrorAction SilentlyContinue }
