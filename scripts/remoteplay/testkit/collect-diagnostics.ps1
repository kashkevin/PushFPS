<#
.SYNOPSIS
  PushFPS diagnostics for testers: ONE zip about this PC and about how PushFPS ran on it, safe to send.

.DESCRIPTION
  Double-click collect-diagnostics.cmd (or run this script). It starts nothing, installs nothing and changes
  nothing. It reads, puts the result into one zip in Documents\slipstream-diagnostics and shows where it is.

  WHAT GOES INTO THE ZIP
    summary.txt          plain words: this PC in short, what may cause problems, what is in the zip and what is not
    hardware.json        processor, memory, graphics card and driver, screens and refresh rates, network adapter
                         type and speed, Windows version and the Windows settings that matter for games
    network.txt          wired or Wi-Fi, link speed, 20 pings to your router (and to the PS5 with -Ps5Address)
    versions.txt         PushFPS file version and a short hash of the program file, versions of the libraries
                         beside it, Windows, PowerShell
    compatibility.txt    overlay and capture tools found running (from a fixed list of names), what PushFPS itself
                         logged about overlays, crashes, graphics failures and hangs, switch NAMES in slipstream.ini
    sessions-<copy>.csv  one row per PS5 session found in the log: length, stream settings, decoded and presented
                         frames per second, reconnects, keyframe requests, loss and jitter, built-in latency tests
    log-<copy>.txt       the log lines of the families below, masked

  THE RULES ARE THE KIT'S (run-tests.ps1): log lines are copied ONLY when their component is
        [push-latency] [push-quality] [push-benchmark] [push-mode] [push-hardware] [push-profile]
        [slipstream-governor] [slipstream-gpu-budget] [slipstream-watch]
        [remoteplay] or [remoteplay-<anything>]   except [remoteplay-discovery]
        [frame-flow] [frame-rate] [pacing] [present-deviation] [fg-validity] [fg-drops]
        [display-vrr] [display-refresh] [display-latency] [display-query]
        [gpu-failure] [gpu-reconnect] [reset-lifecycle] [engine-hang] [engine-test] [crash]
        [overlay-hooks] [log-backpressure], and [qml] lines that start with [update-check]
    and the line holds none of these words: registration, regist_key, rp_key, account, psn (except in psnr=),
    token, secret, passw, ssid, morning, nickname. In every text of the zip: folder and file paths become [path];
    IPv4 addresses keep only their private-range prefix (192.168.x.x ...), any other becomes x.x.x.x; IPv6
    addresses become [ipv6], MAC addresses [mac], runs of 16 or more hex digits [hex]; host=<name> and
    name=<name> become host=[name] / name=[name]. Your computer name and Windows user name, should they stand
    anywhere as a word, become [removed]. Before the zip is made every file is checked again by the kit's
    rules; a file that fails is left out and named.

  WHAT IS NOT COLLECTED
    passwords and other credentials; console registration keys; PSN account ids and sign-in tokens; Wi-Fi
    network names; your files, pictures, recordings and screenshots; the PushFPS settings store (runtime_local);
    the full log; crash dump files (only their number); the computer name; your Windows user name; folder
    paths; the list of installed programs; the list of running programs (only the fixed list of overlay and
    capture tools is looked for, by name); browser data; anything from another program.

  Windows PowerShell 5.1 or newer. No administrator rights needed. Unverified on Windows until the checks
  workflow has run it.

.PARAMETER Exe
  A PushFPS.exe to report on. Default: every copy found in %LOCALAPPDATA%\Programs\PushFPS*.
.PARAMETER Ps5Address
  Optional IP address of the PS5: 20 pings are recorded (the address itself is masked in the zip).
.PARAMETER OutRoot
  Where the zip goes. Default: Documents\slipstream-diagnostics.
.PARAMETER MaxLogMb
  How much of the end of each log file is read (default 8 MB, about the last hours of use).
.PARAMETER NoPing
  Send no pings.
.PARAMETER DryRun
  Reports on a made-up copy with a made-up log that holds private values which must not reach the zip. Used by
  the checks workflow.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File collect-diagnostics.ps1
#>
param(
    [string]$Exe = '',
    [string]$Ps5Address = '',
    [string]$OutRoot = '',
    [int]$MaxLogMb = 8,
    [switch]$NoPing,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$toolVersion = '2'
$toolFolder = Split-Path -Parent $MyInvocation.MyCommand.Path

function Get-UtcStamp {
    return (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffZ')
}

# --- redaction -----------------------------------------------------------------------------------
# The same rules as run-tests.ps1 and tools/remoteplay_latency_bench/report.py (redact_line); a test holds
# the three copies equal. Change them together.
$allowedLine = '\[(INFO |WARN |ERROR|TRACE)\] \[(push-(latency|quality|benchmark|mode|hardware|profile|adaptive-fg)|remoteplay(-[a-z0-9-]+)?|slipstream-(governor|gpu-budget|watch|config|latency|latency-test|press|delivery|sr|fg|autorun|smartlink|swapchain|present|pipeline|nr|clean|quality|input|input-poll|scanout|cadence|net|smooth|capture)|capture-(driver-age|buffer)|nr-(runtime-status|adapt|reuse|model)|frame-(flow|rate)|pacing|present-deviation|present-contract|qml-window|ui-(dialog|fullscreen|window|resume)|fg-(validity|drops)|display-(vrr|refresh|latency|query)|gpu-(failure|reconnect)|reset-lifecycle|engine-(hang|test)|crash|overlay-hooks|log-backpressure|qml\] \[update-check)\] '
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

# True when a text file of the zip still holds something the rules above would change or refuse. $Words is off
# only for summary.txt: its own list of what is NOT collected has to name those very things.
function Test-FileLeaks([string]$Path, [bool]$Words) {
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        if ($Words -and ($line -match 'registration|regist[_ -]?key|rp[_ -]?key|account|psn(?!r=)|token|secret|passw|ssid|morning|nickname')) { return $true }
        if ((Protect-Text $line) -cne $line) { return $true }
    }
    return $false
}

# Beyond the kit's rules: the computer name and the Windows user name, wherever they stand as a word of their own
# in something read from this PC, become [removed]. (They are not expected anywhere: paths are masked already.)
$ownWords = @()
foreach ($word in @($env:COMPUTERNAME, $env:USERNAME)) { if ($word -and ($word.Length -ge 3)) { $ownWords += $word } }
function Hide-OwnWords([string]$Text) {
    $t = $Text
    foreach ($word in $ownWords) {
        $t = [regex]::Replace($t, ('(?i)(?<![A-Za-z0-9])' + [regex]::Escape($word) + '(?![A-Za-z0-9])'), '[removed]')
    }
    return $t
}

# Every text made from what was read on this PC goes through the masking, line by line.
function Save-Text([string[]]$Lines, [string]$Path) {
    $safe = @()
    foreach ($line in $Lines) { $safe += (Hide-OwnWords (Protect-Text ([string]$line))) }
    Set-Content -LiteralPath $Path -Value $safe -Encoding UTF8
}

function Save-Json($Object, [string]$Path) {
    Save-Text (($Object | ConvertTo-Json -Depth 6) -split "`r?`n") $Path
}

# --- reading the log -----------------------------------------------------------------------------------
function Get-Stamp([string]$Line) {
    if (($Line.Length -ge 24) -and ($Line -match '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z')) { return $Line.Substring(0, 24) }
    return ''
}

function ConvertTo-Time([string]$Stamp) {
    return [datetime]::ParseExact($Stamp.Substring(0, 23), 'yyyy-MM-ddTHH:mm:ss.fff', [System.Globalization.CultureInfo]::InvariantCulture)
}

function Get-LogNumber([string]$Line, [string]$Name) {
    if ($Line -match ('(?<![A-Za-z])' + $Name + '=(-?[0-9]+(\.[0-9]+)?)')) {
        return [double]::Parse($Matches[1], [System.Globalization.CultureInfo]::InvariantCulture)
    }
    return $null
}

function Get-LogWord([string]$Line, [string]$Name) {
    if ($Line -match ('(?<![A-Za-z])' + $Name + '=([^\s,;()]+)')) { return $Matches[1] }
    return ''
}

function Get-Median($Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return '' }
    $middle = [int][math]::Floor($sorted.Count / 2)
    $median = $sorted[$middle]
    if (($sorted.Count % 2) -eq 0) { $median = ($sorted[$middle - 1] + $sorted[$middle]) / 2 }
    return [string][math]::Round($median, 2)
}

# The last $MaxBytes of a log file as lines (the first, cut line is dropped). The file may be open in PushFPS.
function Read-LogTail([string]$Path, [long]$MaxBytes) {
    $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $from = 0
        if ($stream.Length -gt $MaxBytes) { $from = $stream.Length - $MaxBytes }
        [void]$stream.Seek($from, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
        $text = $reader.ReadToEnd()
    } finally { $stream.Dispose() }
    $lines = @($text -split "`r?`n")
    if (($from -gt 0) -and ($lines.Count -gt 1)) { $lines = $lines[1..($lines.Count - 1)] }
    return $lines
}

function New-Session([string]$Stamp, [string]$Stream) {
    return [pscustomobject]@{
        started = $Stamp; last = $Stamp; stream = $Stream; decoder = ''
        decodeFps = (New-Object System.Collections.Generic.List[double]); presentedFps = (New-Object System.Collections.Generic.List[double])
        loss = (New-Object System.Collections.Generic.List[double]); latency = (New-Object System.Collections.Generic.List[double])
        keyframes = 0; reconnects = 0; gaveUp = 0; crashes = 0; gpuFailures = 0; hangs = 0; healthLines = 0
        jitterPeak = 0.0; grade = ''; latencyTests = 0; vramMax = 0.0
    }
}

# One row per PS5 session of the (already masked) log lines; lines before the first session count for a row
# named 'before any session'.
function Get-Sessions([string[]]$Lines) {
    $sessions = @()
    $current = New-Session '' 'before any session'
    $grades = @('', 'unknown', 'stable', 'unsteady', 'unstable')
    foreach ($line in $Lines) {
        $stamp = Get-Stamp $line
        if ($line -match '\] \[remoteplay\] session started') {
            if (($current.started) -or ($current.crashes + $current.gpuFailures + $current.hangs -gt 0)) { $sessions += $current }
            $current = New-Session $stamp ((Get-LogWord $line 'profile') + ' ' + (Get-LogWord $line 'codec') + ' ' + (Get-LogWord $line 'requestedBitrateKbps') + ' kbit/s')
            continue
        }
        if ($stamp) { $current.last = $stamp }
        if ($line -match '\] \[remoteplay-progress\] ') {
            $value = Get-LogNumber $line 'decodeFps'
            if ($null -ne $value) { $current.decodeFps.Add($value) }
        } elseif ($line -match '\] \[frame-rate\] ') {
            $value = Get-LogNumber $line 'presentSubmitFps'
            if ($null -ne $value) { $current.presentedFps.Add($value) }
            $value = Get-LogNumber $line 'vramMiB'
            if (($null -ne $value) -and ($value -gt $current.vramMax)) { $current.vramMax = $value }
        } elseif ($line -match '\] \[remoteplay-decode\] actual=') {
            $current.decoder = Get-LogWord $line 'actual'
        } elseif ($line -match '\] \[remoteplay-recovery\] ') {
            if ($line -match 'request keyframe') { $current.keyframes++ }
            if ($line -match '\] reconnected ') { $current.reconnects++ }
            if ($line -match 'recovery stopped') { $current.gaveUp++ }
        } elseif ($line -match '\] \[remoteplay-health\] grade=') {
            $current.healthLines++
            $value = Get-LogNumber $line 'lossPercent'
            if ($null -ne $value) { $current.loss.Add($value) }
            $value = Get-LogNumber $line 'frameJitterPeakMs'
            if (($null -ne $value) -and ($value -gt $current.jitterPeak)) { $current.jitterPeak = $value }
            $grade = Get-LogWord $line 'interval'
            if ([array]::IndexOf($grades, $grade) -gt [array]::IndexOf($grades, $current.grade)) { $current.grade = $grade }
        } elseif ($line -match '\] \[push-latency\] done answered=') {
            $current.latencyTests++
            $value = Get-LogNumber $line 'meanMs'
            if ($null -ne $value) { $current.latency.Add($value) }
        } elseif ($line -match '\] \[crash\] .*unhandled exception') { $current.crashes++ }
        elseif ($line -match '\] \[gpu-failure\] operation=') { $current.gpuFailures++ }
        elseif ($line -match '\] \[engine-hang\] owner=') { $current.hangs++ }
    }
    if (($current.started) -or ($current.crashes + $current.gpuFailures + $current.hangs -gt 0)) { $sessions += $current }
    return $sessions
}

function Format-SessionRows($Sessions) {
    $rows = @('startedUtc,minutes,stream,decoder,medianDecodedPerSecond,lowestDecodedPerSecond,medianPresentedPerSecond,keyframeRequests,reconnects,recoveryGaveUp,crashLines,gpuFailureLines,engineHangLines,healthIntervals,meanLossPercent,worstJitterMs,worstGrade,latencyTests,softwareLatencyMeanMs,videoMemoryMiBMax')
    foreach ($s in $Sessions) {
        $minutes = ''
        if ($s.started -and $s.last) { $minutes = [string][math]::Round(((ConvertTo-Time $s.last) - (ConvertTo-Time $s.started)).TotalMinutes, 1) }
        $lowest = ''
        if ($s.decodeFps.Count -gt 0) { $lowest = [string](@($s.decodeFps | Sort-Object)[0]) }
        $meanLoss = ''
        if ($s.loss.Count -gt 0) { $sum = 0.0; foreach ($v in $s.loss) { $sum += $v }; $meanLoss = [string][math]::Round($sum / $s.loss.Count, 3) }
        $meanLatency = ''
        if ($s.latency.Count -gt 0) { $sum = 0.0; foreach ($v in $s.latency) { $sum += $v }; $meanLatency = [string][math]::Round($sum / $s.latency.Count, 1) }
        $started = $s.started
        if (-not $started) { $started = 'before any session' }
        $rows += (@($started, $minutes, $s.stream, $s.decoder, (Get-Median $s.decodeFps), $lowest, (Get-Median $s.presentedFps), $s.keyframes, $s.reconnects, $s.gaveUp, $s.crashes, $s.gpuFailures, $s.hangs,
                    $s.healthLines, $meanLoss, [string]$s.jitterPeak, $s.grade, $s.latencyTests, $meanLatency, [string]$s.vramMax) -join ',')
    }
    return $rows
}

function Get-ShortHash([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try { $bytes = $sha.ComputeHash($stream) } finally { $stream.Dispose(); $sha.Dispose() }
    return ([System.BitConverter]::ToString($bytes) -replace '-', '').Substring(0, 10)   # 10 digits: 12 would look like a MAC address to the masking
}

# Twenty pings: replies, lost, fastest, median, slowest (ms). The address is never written.
function Measure-Ping([string]$Address, [int]$Count) {
    $times = @()
    try {
        foreach ($reply in @(Test-Connection -ComputerName $Address -Count $Count -ErrorAction Stop)) {
            if ($null -ne $reply.ResponseTime) { $times += [double]$reply.ResponseTime }     # Windows PowerShell
            elseif ($null -ne $reply.Latency) { $times += [double]$reply.Latency }           # PowerShell 7
        }
    } catch { $times = @() }
    if ($times.Count -eq 0) { return ('no answer to ' + $Count + ' pings (it may not answer pings at all)') }
    $sorted = @($times | Sort-Object)
    return ('{0} of {1} answered; fastest {2} ms, median {3} ms, slowest {4} ms' -f $times.Count, $Count, $sorted[0], (Get-Median $times), $sorted[$sorted.Count - 1])
}

# Made-up copy for -DryRun: a log with values that must NOT reach the zip, a settings file, a dump file.
function New-DryRunCopy([string]$Root) {
    $folder = Join-Path $Root 'PushFPS-dryrun'
    $logs = Join-Path $folder 'logs'
    New-Item -ItemType Directory -Force -Path $logs | Out-Null
    Set-Content -LiteralPath (Join-Path $folder 'PushFPS.exe') -Value 'not a program: stand-in for the dry run' -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $folder 'slipstream.ini') -Value @('# made up', 'VEYRA_EXP_RP_KBM=1', 'VEYRA_EXP_RP_KBM_PROFILE=C:\Users\PlantedUserName\Documents\PlantedProfile.txt') -Encoding ASCII
    Set-Content -LiteralPath (Join-Path $logs 'veyra-crash-20261118-120000-1.dmp') -Value 'not a dump' -Encoding ASCII
    $now = (Get-Date).ToUniversalTime().AddMinutes(-30)
    $made = @(
        '[INFO ] [app] data folder C:\Users\PlantedUserName\AppData\Local\Programs\PushFPS\runtime_local',
        '[INFO ] [remoteplay-discovery] host-name=PlantedConsoleName host-id=AABBCCDDEEFF',
        '[INFO ] [remoteplay-ui] registration regist_key=0123456789abcdef0123 account=PlantedAccountId=',
        '[INFO ] [wlan] ssid=PlantedWifiName',
        '[INFO ] [remoteplay-settings] made-up values VEYRA_EXP_RP_KBM_PROFILE=\\PLANTEDSRV\share\planted-kbm.txt and C:/Users/Planted User/AppData/Local/kbm.ini end',
        '[INFO ] [remoteplay-kbm] made-up ids: mail planted.user@example.com mac 001A2B3C4D5E dotted 001a.2b3c.4d5e guid 0F8FAD5B-D9CB-469F-A165-70867728950E id 1A2B3C4D5E6F7A8 server=PlantedServer computer=PLANTED-PC device=PlantedDevice network=PlantedNet peer fe80::1ff:fe23:4567:890b%12 end',
        '[INFO ] [remoteplay-ui] regist key=PlantedSpaceKey',
        '[INFO ] [qml] [update-check] current installed=3.2.1 build=3.2.1 latest=3.2.1',
        '[INFO ] [overlay-hooks] RTSS hook module: activeServer=false path=C:\Program Files (x86)\PlantedFolder\RTSSHooks64.dll',
        '[INFO ] [remoteplay-control] start host=PlantedHostName restart=false background=false',
        '[INFO ] [remoteplay] session started host=203.0.113.9 profile=1920x1080@60 codec=hevc requestedBitrateKbps=30000',
        '[INFO ] [remoteplay] peer fe80::1ff:fe23:4567:890a mac 00:1A:2B:3C:4D:5E key 00112233445566778899aabbccddeeff',
        '[INFO ] [remoteplay-control] start host=192.168.1.77 restart=false background=false',
        '[INFO ] [remoteplay-decode] actual=D3D12VA pixelFormat=172 1920x1080 fallback=false',
        '[INFO ] [remoteplay-health] grade=unsteady interval=unsteady seconds=5.0 lossPercent=0.200 lost=18 received=9000 frameJitterMs=1.40 frameJitterPeakMs=6.20 reordered=0 late=3 reorderTimeouts=0 skipped=0 fecRecovered=2 fecFailed=0 keyframeRequests=1 decoderResets=0 frameDrops=0 ingressDepth=0 ingressPeak=1 mailboxOverwrites=0 mbps=28.10 fps=59.9 rtt=0.45ms(at-connect) route=direct-lan link=ethernet linkMbps=1000',
        '[WARN ] [remoteplay-recovery] request keyframe; received=900 decoded=880 videoCallbacks=900 rejected=0 packetReceived=4000 packetLost=37',
        '[INFO ] [push-latency] done answered=20/20 meanMs=100.9 medianMs=100.0 minMs=83.3 maxMs=118.6 pushfpsMs=6.1',
        '[ERROR] [crash] unhandled exception code=0xC0000005 address=0x7FF6A1B2 module=C:\Users\Planted User Name\AppData\Local\Programs\PushFPS\PushFPS.exe offset=0x1234 thread=77'
    )
    $out = @()
    $n = 0
    foreach ($line in $made) {
        $out += ($now.AddSeconds($n).ToString('yyyy-MM-ddTHH:mm:ss.fffZ') + ' t=1234 ' + $line)
        $n++
    }
    for ($i = 0; $i -lt 120; $i++) {
        $out += ($now.AddSeconds(20 + $i).ToString('yyyy-MM-ddTHH:mm:ss.fffZ') + ' t=1234 [INFO ] [remoteplay-progress] state=3 received=600 decoded=600 inputFps=59.9 decodeFps=59.9 videoMbps=28.100 receiveAgeMs=7.0 decodeAgeMs=5.0 decodeBusyMs=1.2 queue=0 waitingIdr=false idrRequests=0 errors=0')
        $out += ($now.AddSeconds(20 + $i).ToString('yyyy-MM-ddTHH:mm:ss.fffZ') + ' t=1234 [INFO ] [frame-rate] revision=1 gpuCompletedFps=59.90 completedReal=60 capture60To30=false rateSkipped=0 presentSubmitFps=59.90 windowMs=1000 vramMiB=1200 budgetMiB=11000 (not scanout FPS)')
    }
    Set-Content -LiteralPath (Join-Path $logs 'veyra-qml.log') -Value $out -Encoding UTF8
    return (Join-Path $folder 'PushFPS.exe')
}

# --- start -------------------------------------------------------------------------------------------
if (-not $OutRoot) {
    $documents = [Environment]::GetFolderPath('MyDocuments')
    if (-not $documents) { $documents = (Get-Location).Path }
    $OutRoot = Join-Path $documents 'slipstream-diagnostics'
}
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
$OutRoot = (Resolve-Path -LiteralPath $OutRoot).Path
$name = 'diagnostics-' + (Get-Date -Format 'yyyyMMdd-HHmmss')
$stage = Join-Path $OutRoot ($name + '.tmp')
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Write-Host ''
Write-Host ('PushFPS diagnostics ' + $toolVersion) -ForegroundColor Cyan
Write-Host 'Reading this PC. Nothing is started, installed or changed.'

# Which copies of PushFPS
$copies = @()
if ($DryRun) {
    $copies += (New-DryRunCopy (Join-Path $stage '_dryrun'))
} elseif ($Exe) {
    if (-not (Test-Path -LiteralPath $Exe)) { throw "PushFPS.exe not found: $Exe" }
    $copies += (Resolve-Path -LiteralPath $Exe).Path
} else {
    $base = Join-Path $env:LOCALAPPDATA 'Programs'
    foreach ($folder in @(Get-ChildItem -LiteralPath $base -Directory -Filter 'PushFPS*' -ErrorAction SilentlyContinue)) {
        $candidate = Join-Path $folder.FullName 'PushFPS.exe'
        if (Test-Path -LiteralPath $candidate) { $copies += $candidate }
    }
}

$problems = New-Object System.Collections.Generic.List[string]
$short = New-Object System.Collections.Generic.List[string]

# --- hardware -------------------------------------------------------------------------------------------
$hardware = $null
$detect = Join-Path (Split-Path -Parent $toolFolder) 'detect-hardware.ps1'
if (-not (Test-Path -LiteralPath $detect)) { $detect = Join-Path $toolFolder 'detect-hardware.ps1' }
if (Test-Path -LiteralPath $detect) {
    $hardwareTemp = Join-Path $env:TEMP ('slipstream-hardware-' + [guid]::NewGuid().ToString('N') + '.json')
    try {
        $pingTarget = $Ps5Address
        if ($NoPing) { $pingTarget = '' }
        & $detect -OutFile $hardwareTemp -Ps5Address $pingTarget -ExpectRefreshHz 1 6>$null   # its own printed summary is for another purpose
        $hardware = Get-Content -LiteralPath $hardwareTemp -Raw | ConvertFrom-Json
    } catch {
        $problems.Add('The hardware could not be read completely: ' + $_.Exception.Message)
    } finally {
        Remove-Item -LiteralPath $hardwareTemp -Force -ErrorAction SilentlyContinue
    }
}
if (-not $hardware) {
    # The detection script is not beside this one: the few facts Windows gives directly.
    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $os = Get-CimInstance Win32_OperatingSystem
    $adapters = @()
    foreach ($a in @(Get-CimInstance Win32_VideoController)) {
        $adapters += [pscustomobject]@{ name = $a.Name; driverVersion = $a.DriverVersion; width = $a.CurrentHorizontalResolution; height = $a.CurrentVerticalResolution; refreshHz = $a.CurrentRefreshRate; maxRefreshHz = $a.MaxRefreshRate }
    }
    $hardware = [pscustomobject]@{ computer = '(removed)'; windows = ($os.Caption + ' build ' + $os.BuildNumber); cpu = [pscustomobject]@{ name = $cpu.Name.Trim(); cores = $cpu.NumberOfCores; threads = $cpu.NumberOfLogicalProcessors }
                                   ramGb = [math]::Round($os.TotalVisibleMemorySize / 1MB, 1); adapters = $adapters; network = @(); settings = $null; warnings = @() }
}
$hardware.computer = '(removed)'
foreach ($adapter in @($hardware.network)) { if ($adapter) { $adapter.name = '(removed)' } }
# The detection script was written for one PC: its remarks about that PC's graphics card and refresh rate do not apply
# here (it is asked for 1 Hz above, so its refresh remark always fires and is dropped; the max-rate check below replaces it).
$warnings = @()
foreach ($w in @($hardware.warnings)) { if ($w -and ($w -notmatch 'RTX 5070|No NVIDIA adapter|expected 1 Hz')) { $warnings += [string]$w } }
$hardware.warnings = $warnings
Save-Json $hardware (Join-Path $stage 'hardware.json')

if ($hardware.cpu) { $short.Add(('Processor: {0} ({1} cores), memory {2} GB' -f $hardware.cpu.name, $hardware.cpu.cores, $hardware.ramGb)) }
if ($hardware.windows) { $short.Add('Windows: ' + $hardware.windows) }
foreach ($a in @($hardware.adapters)) {
    if (-not $a) { continue }
    $driver = $a.driverVersion
    if ($a.nvidiaDriver) { $driver = $a.nvidiaDriver }
    $short.Add(('Graphics: {0}, driver {1}, screen {2}x{3} at {4} Hz' -f $a.name, $driver, $a.width, $a.height, $a.refreshHz))
    if ($a.refreshHz -and $a.maxRefreshHz -and ([int]$a.refreshHz -lt [int]$a.maxRefreshHz)) {
        $problems.Add(('The screen runs at {0} Hz but can do {1} Hz: Settings > System > Display > Advanced display.' -f $a.refreshHz, $a.maxRefreshHz))
    }
}
foreach ($w in $warnings) { $problems.Add($w) }

# --- network -------------------------------------------------------------------------------------------
$network = @('Network of this PC (' + (Get-UtcStamp) + ')', '')
foreach ($n in @($hardware.network)) {
    if (-not $n) { continue }
    $role = ''
    if ($n.defaultRoute) { $role = ' (used for the internet and the PS5)' }
    $network += ('Adapter: {0}, {1}, {2}{3}' -f $n.description, $n.linkSpeed, $n.media, $role)
    if ($n.defaultRoute) { $short.Add(('Network: {0}, {1}' -f $n.media, $n.linkSpeed)) }
}
if (-not $NoPing) {
    $pingCount = 20
    if ($DryRun) { $pingCount = 2 }
    $gateway = ''
    try {
        $route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction Stop | Sort-Object RouteMetric, InterfaceMetric | Select-Object -First 1
        if ($route) { $gateway = [string]$route.NextHop }
    } catch { $gateway = '' }
    if ($gateway -and ($gateway -ne '0.0.0.0')) {
        Write-Host 'Pinging your router ...'
        $result = Measure-Ping $gateway $pingCount
        $network += ('Router: ' + $result)
        $short.Add('Router: ' + $result)
    } else { $network += 'Router: no default route found' }
    if ($hardware.ps5Ping) {
        $network += ('PS5: {0} of {1} answered; fastest {2} ms, median {3} ms, slowest {4} ms' -f $hardware.ps5Ping.replies, $hardware.ps5Ping.sent, $hardware.ps5Ping.minMs, $hardware.ps5Ping.medianMs, $hardware.ps5Ping.maxMs)
    } elseif ($Ps5Address) { $network += 'PS5: no answer to pings (it may be in rest mode)' }
} else { $network += 'No pings were sent (-NoPing).' }
$network += @('', 'What PushFPS itself measured on the network while streaming is in the sessions-*.csv files (loss, jitter) and in the [remoteplay-health] lines of the log.')
Save-Text $network (Join-Path $stage 'network.txt')

# --- every copy of PushFPS: versions, log, sessions, compatibility ---------------------------------------
$versions = @('Versions (' + (Get-UtcStamp) + ')', '', ('Diagnostics tool: ' + $toolVersion), ('PowerShell: ' + $PSVersionTable.PSVersion.ToString()))
if ($hardware.windows) { $versions += ('Windows: ' + $hardware.windows) }
$compatibility = @('Compatibility (' + (Get-UtcStamp) + ')', '')
$sessionTotal = 0
$crashTotal = 0
$gpuTotal = 0
$hangTotal = 0
$gaveUpTotal = 0
if ($copies.Count -eq 0) { $problems.Add('No copy of PushFPS was found in the usual place. Start this tool with -Exe <path of PushFPS.exe>.') }
foreach ($copy in $copies) {
    $folder = Split-Path -Parent $copy
    $label = (Split-Path -Leaf $folder) -replace '[^A-Za-z0-9._-]', '_'
    $item = Get-Item -LiteralPath $copy
    $fileVersion = ''
    try { $fileVersion = [string]$item.VersionInfo.ProductVersion } catch { $fileVersion = '' }
    # Dots become underscores in file versions: a four-part version looks like an address to the masking.
    $versions += @('', ('Copy "' + $label + '"'), ('  program file: version {0}, {1:N0} bytes, changed {2}, sha256 {3}' -f ($fileVersion -replace '\.', '_'), $item.Length, $item.LastWriteTimeUtc.ToString('yyyy-MM-dd'), (Get-ShortHash $copy)))
    foreach ($library in @(Get-ChildItem -LiteralPath $folder -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^(Qt6Core|Qt6Quick|avcodec-\d+|avutil-\d+|SDL3|nvngx_dlss|nvngx_dlssg|amd_fidelityfx_dx12|libxess|onnxruntime|DirectML)\.dll$' })) {
        $libraryVersion = ''
        try { $libraryVersion = [string]$library.VersionInfo.FileVersion } catch { $libraryVersion = '' }
        $versions += ('  {0}: version {1}' -f $library.Name, ($libraryVersion -replace '\.', '_'))
    }

    # slipstream.ini: the NAMES of the switches only
    $ini = Join-Path $folder 'slipstream.ini'
    if (Test-Path -LiteralPath $ini) {
        $names = @()
        foreach ($row in @(Get-Content -LiteralPath $ini)) { if ($row -match '^\s*([A-Za-z0-9_]+)\s*=') { $names += $Matches[1] } }
        $compatibility += ('Copy "' + $label + '": slipstream.ini is present and sets: ' + ($names -join ' '))
        if ($names.Count -gt 0) { $problems.Add(('Copy "' + $label + '" has a slipstream.ini with ' + $names.Count + ' switch(es): experiments are on. Rename the file to test without them.')) }
    } else { $compatibility += ('Copy "' + $label + '": no slipstream.ini (default behaviour)') }

    $logFolder = Join-Path $folder 'logs'
    $dumps = @(Get-ChildItem -LiteralPath $logFolder -Filter '*.dmp' -ErrorAction SilentlyContinue)
    $compatibility += ('Copy "' + $label + '": ' + $dumps.Count + ' crash dump file(s) in its log folder (not collected)')
    $lines = @()
    foreach ($logName in @('veyra-qml.old.log', 'veyra-qml.log')) {
        $logPath = Join-Path $logFolder $logName
        if (-not (Test-Path -LiteralPath $logPath)) { continue }
        try { foreach ($kept in @(Select-SafeLines (Read-LogTail $logPath ([long]$MaxLogMb * 1MB)))) { $lines += (Hide-OwnWords $kept) } }
        catch { $problems.Add('A log file of copy "' + $label + '" could not be read: ' + $_.Exception.Message) }
    }
    if ($lines.Count -eq 0) {
        $compatibility += ('Copy "' + $label + '": no log lines of the collected families (has it streamed from a PS5 yet?)')
        continue
    }
    Set-Content -LiteralPath (Join-Path $stage ('log-' + $label + '.txt')) -Value $lines -Encoding UTF8
    $sessions = @(Get-Sessions $lines)
    Set-Content -LiteralPath (Join-Path $stage ('sessions-' + $label + '.csv')) -Value (Format-SessionRows $sessions) -Encoding UTF8
    foreach ($s in $sessions) {
        if ($s.started) { $sessionTotal++ }
        $crashTotal += $s.crashes; $gpuTotal += $s.gpuFailures; $hangTotal += $s.hangs; $gaveUpTotal += $s.gaveUp
        if ($s.decoder -eq 'software') { $problems.Add('A session of copy "' + $label + '" decoded the video in software, not on the graphics card: slower and it uses the processor.') }
        if ($s.grade -eq 'unstable') { $problems.Add('PushFPS graded the network "unstable" in a session of copy "' + $label + '" (packet loss or jitter).') }
    }
    $seen = @{}
    foreach ($line in $lines) {
        if ($line -match '\] \[overlay-hooks\] (.*)$') {
            $text = $Matches[1]
            if (-not $seen.ContainsKey($text)) { $seen[$text] = $true; $compatibility += ('Copy "' + $label + '" logged: ' + $text) }
        } elseif ($line -match '\] \[qml\] \[update-check\] (.*)$') { $seen['version'] = $Matches[1] }
    }
    if ($seen.ContainsKey('version')) { $versions += ('  the program reported: ' + $seen['version']) }
}
if ($sessionTotal -gt 0) { $short.Add(('PushFPS: {0} PS5 session(s) in the part of the log that was read' -f $sessionTotal)) }
elseif ($copies.Count -gt 0) { $short.Add('PushFPS: found, but its log shows no PS5 session yet') }
if ($crashTotal -gt 0) { $problems.Add(('PushFPS logged {0} crash(es).' -f $crashTotal)) }
if ($gpuTotal -gt 0) { $problems.Add(('PushFPS logged {0} graphics device failure(s).' -f $gpuTotal)) }
if ($hangTotal -gt 0) { $problems.Add(('PushFPS logged {0} hang(s) of its video engine.' -f $hangTotal)) }
if ($gaveUpTotal -gt 0) { $problems.Add(('PushFPS gave up reconnecting to the PS5 {0} time(s).' -f $gaveUpTotal)) }

# Overlay and capture tools that are running now: looked for by name, from this fixed list only.
$watchList = @('RTSS', 'MSIAfterburner', 'NVIDIA Overlay', 'nvsphelper64', 'GameBar', 'GameBarFTServer', 'obs64', 'Discord', 'steamwebhelper', 'GamePP', 'EpicGamesLauncher', 'Medal', 'Overwolf', 'PresentMon', 'RemotePlay')
$running = @()
foreach ($tool in $watchList) { if (@(Get-Process -Name $tool -ErrorAction SilentlyContinue).Count -gt 0) { $running += $tool } }
$compatibility += @('', ('Overlay and capture tools running now (of: ' + ($watchList -join ', ') + '): ' + $(if ($running.Count -gt 0) { $running -join ', ' } else { 'none' })))
if ($running.Count -gt 0) { $problems.Add('Running now and known to interfere with some games and overlays: ' + ($running -join ', ') + '. Close them if PushFPS stutters or crashes.') }
if ($hardware.settings) {
    $compatibility += @('', ('Windows: GPU scheduling {0}; Game Mode {1}; {2}' -f $hardware.settings.hardwareGpuScheduling, $hardware.settings.gameMode, $hardware.settings.powerPlan))
}
Save-Text $versions (Join-Path $stage 'versions.txt')
Save-Text $compatibility (Join-Path $stage 'compatibility.txt')

# --- summary -------------------------------------------------------------------------------------------
$summary = @(
    ('PushFPS diagnostics, made ' + (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm') + ' UTC by collect-diagnostics.ps1 (version ' + $toolVersion + ')'),
    '',
    'WHAT THIS IS',
    '  A report about this PC and about how PushFPS ran on it, for the person who asked you for it.',
    '  It was made by reading only: nothing was started, installed or changed. You can open every file in it.',
    ''
)
if ($DryRun) { $summary += @('  DRY RUN: the PushFPS part is made-up data, not a real copy.', '') }
$summary += 'THIS PC IN SHORT'
foreach ($line in $short) { $summary += ('  ' + (Hide-OwnWords (Protect-Text $line))) }
$summary += @('', 'THINGS THAT MAY CAUSE PROBLEMS')
if ($problems.Count -eq 0) { $summary += '  Nothing was noticed.' }
foreach ($line in $problems) { $summary += ('  - ' + (Hide-OwnWords (Protect-Text $line))) }
$summary += @(
    '',
    'WHAT IS IN THE ZIP',
    '  summary.txt        this text',
    '  hardware.json      processor, memory, graphics card and driver, screens, network adapter type and speed, Windows settings for games',
    '  network.txt        wired or Wi-Fi, link speed, pings to the router (and the PS5 when its address was given)',
    '  versions.txt       versions of PushFPS and of the libraries beside it, Windows, PowerShell',
    '  compatibility.txt  overlay and capture tools, crashes, graphics failures, experiment switches (names only)',
    '  sessions-*.csv     one row per PS5 session: length, stream settings, frames per second, reconnects, loss, jitter.',
    '                     softwareLatencyMeanMs is the built-in test (a software clock inside the program), not the delay from button to screen.',
    '  log-*.txt          the log lines of PushFPS about streaming, frames, the display and the graphics card, with addresses, names and paths masked',
    '',
    'WHAT IS NOT IN THE ZIP',
    '  - passwords and other credentials',
    '  - console registration keys',
    '  - PSN account ids and sign-in tokens',
    '  - Wi-Fi network names',
    '  - your files, pictures, recordings and screenshots',
    '  - the PushFPS settings store and the full log',
    '  - crash dump files (only how many there are)',
    '  - the computer name, your Windows user name, folder paths',
    '  - IP addresses (only the private-range prefix such as 192.168.x.x is kept), MAC addresses',
    '  - the list of installed or running programs (only a fixed list of overlay and capture tools is looked for)',
    ''
)
Set-Content -LiteralPath (Join-Path $stage 'summary.txt') -Value $summary -Encoding UTF8

# --- final check and the zip ---------------------------------------------------------------------------
if ($DryRun) { Remove-Item -LiteralPath (Join-Path $stage '_dryrun') -Recurse -Force -ErrorAction SilentlyContinue }
$withheld = @()
foreach ($file in @(Get-ChildItem -LiteralPath $stage -File)) {
    if (Test-FileLeaks $file.FullName ($file.Name -ne 'summary.txt')) { $withheld += $file.Name }
}
foreach ($fileName in $withheld) {
    Write-Host ('  NOT packed (it may hold an address, a name or a private word): ' + $fileName) -ForegroundColor Yellow
    Remove-Item -LiteralPath (Join-Path $stage $fileName) -Force
}
Set-Content -LiteralPath (Join-Path $stage 'withheld.txt') -Value (@('files left out of the zip by the final check: ' + $withheld.Count) + $withheld) -Encoding ASCII
$zip = Join-Path $OutRoot ($name + '.zip')
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)
Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue

Write-Host ''
foreach ($line in $summary) { Write-Host $line }
Write-Host ('Send this ONE file: ' + $zip) -ForegroundColor Green
Write-Host ('Size: {0:N0} KB. You can open it and read every file before sending.' -f ((Get-Item -LiteralPath $zip).Length / 1KB))
