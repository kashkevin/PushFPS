<#
.SYNOPSIS
  PushFPS capture-stall trace: ONE Windows performance trace around the next capture stall. Runs ONLY if you start it,
  and only from an administrator window (it never asks for or takes administrator rights by itself).

.DESCRIPTION
  What it does, in order:
    1. Refuses to run unless this window already runs as administrator (Event Tracing for Windows' kernel events need it).
    2. Checks which of the event providers below exist on this PC (logman query providers, read only) and writes a
       recording profile with those only (slipstream-stall.wprp, next to the output).
    3. Starts Windows Performance Recorder (wpr.exe, part of Windows 10 and 11) in MEMORY mode: the events go into a
       fixed ring of buffers in memory (-BufferMB, default 128 MB) and the oldest are overwritten, so nothing is written
       to disk while it waits.
    4. Watches PushFPS's own log for the next capture stall ([capture-stall] kind=gap), reading only what is new every
       2 seconds; when one comes it waits 5 s more and saves the ring as ONE .etl file next to PushFPS's logs. Without a
       stall it gives up after -MaxMinutes (default 25) and saves nothing.
    5. Always ends the recording (wpr -cancel) when it stops for any other reason, also on Ctrl+C.
    6. Writes text that can be read without Windows tools: tracerpt's summary (event counts per provider and type), and
       with -ExportCsv the events from 5 s before to 3 s after the stall as CSV.
  It changes no setting. While it runs, the kernel's trace session is active (that is the trace); it ends with the script.

  Kernel events (NT Kernel Logger): ProcessThread, Loader (which driver is where), CSwitch + ReadyThread (who ran, who
  waited and who readied it), DPC, Interrupt (deferred and interrupt routines with their addresses), HardFaults, Power,
  IdleStates, CpuConfig. User/driver providers when present: Microsoft-Windows-TaskScheduler (task starts, even when its
  history log is off), Microsoft-Windows-Kernel-PnP, Microsoft-Windows-USB-USBXHCI, Microsoft-Windows-USB-UCX,
  Microsoft-Windows-USB-USBHUB3, Microsoft-Windows-Kernel-Power, Microsoft-Windows-MediaFoundation-Performance,
  Microsoft-Windows-DxgKrnl (graphics kernel; large, only with -WithGraphics).

  Cost (reasoned, not measured on this PC): the kernel writes each event into memory; with context switches at tens of
  thousands per second this is a few MB per second, typically well under 1 % of the processor; saving the ring at the
  stall takes a few seconds of one core and writes up to about -BufferMB plus the module list (with -compress the file
  is usually several times smaller). Naming the driver behind each DPC / interrupt needs the .etl and its module list:
  Windows Performance Analyzer or "xperf -a dpcisr" do that, but they come with the Windows Performance Toolkit, a
  separate Microsoft download that you would have to choose to install; the CSV here holds the routine addresses and
  the image load events, from which the module can be worked out elsewhere.

.PARAMETER LogFolder
  PushFPS's log folder. Default: the "logs" folder of the PushFPS copy whose log changed last.
.PARAMETER BufferMB
  Size of the in-memory ring (32-512, default 128).
.PARAMETER MaxMinutes
  How long to wait for a stall (1-60, default 25).
.PARAMETER ExportCsv
  Also run tracerpt to CSV and keep the events from 5 s before to 3 s after the stall (can take minutes).
.PARAMETER WithGraphics
  Also record the graphics kernel provider (much larger).
.PARAMETER DryRun
  Checks only: writes and validates the profile, lists the providers found; starts no recording. Used by the checks workflow.
#>
param(
    [string]$LogFolder = '',
    [int]$BufferMB = 128,
    [int]$MaxMinutes = 25,
    [switch]$ExportCsv,
    [switch]$WithGraphics,
    [string]$OutFolder = '',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$BufferMB = [math]::Max(32, [math]::Min(512, $BufferMB))
$MaxMinutes = [math]::Max(1, [math]::Min(60, $MaxMinutes))

$wanted = @('Microsoft-Windows-TaskScheduler', 'Microsoft-Windows-Kernel-PnP', 'Microsoft-Windows-USB-USBXHCI', 'Microsoft-Windows-USB-UCX',
            'Microsoft-Windows-USB-USBHUB3', 'Microsoft-Windows-Kernel-Power', 'Microsoft-Windows-MediaFoundation-Performance')
if ($WithGraphics) { $wanted += 'Microsoft-Windows-DxgKrnl' }
$kernelKeywords = @('ProcessThread', 'Loader', 'CSwitch', 'ReadyThread', 'DPC', 'Interrupt', 'HardFaults', 'Power', 'IdleStates', 'CpuConfig')

# --- 1. administrator? (never elevate here) ------------------------------------------------------------------------
$principal = New-Object System.Security.Principal.WindowsPrincipal([System.Security.Principal.WindowsIdentity]::GetCurrent())
$isAdmin = $principal.IsInRole([System.Security.Principal.WindowsBuiltInRole]::Administrator)
if ((-not $isAdmin) -and (-not $DryRun)) {
    Write-Host 'This needs an administrator window (kernel trace events). Nothing was started.'
    Write-Host 'Only if you want the trace: right-click Windows PowerShell > Run as administrator, then run this script again.'
    exit 2
}

# --- where -----------------------------------------------------------------------------------------------------------
if (-not $LogFolder) {
    $best = $null
    foreach ($folder in @(Get-ChildItem -LiteralPath (Join-Path $env:LOCALAPPDATA 'Programs') -Directory -Filter 'PushFPS*' -ErrorAction SilentlyContinue)) {
        $log = Join-Path $folder.FullName 'logs\veyra-qml.log'
        if (Test-Path -LiteralPath $log) { $item = Get-Item -LiteralPath $log; if ((-not $best) -or ($item.LastWriteTimeUtc -gt $best.LastWriteTimeUtc)) { $best = $item } }
    }
    if ($best) { $LogFolder = $best.DirectoryName }
}
if (-not $OutFolder) { $OutFolder = $LogFolder }
if ((-not $OutFolder) -and $DryRun) { $OutFolder = (Get-Location).Path }
if (-not $OutFolder) { throw 'No PushFPS log found under %LOCALAPPDATA%\Programs\PushFPS*\logs. Give -LogFolder.' }
New-Item -ItemType Directory -Force -Path $OutFolder | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$notes = Join-Path $OutFolder ('stall-trace-' + $stamp + '.txt')
function Write-Note([string]$Text) { Write-Host $Text; Add-Content -LiteralPath $notes -Value $Text -Encoding UTF8 }
Write-Note ('PushFPS stall trace   ' + (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss') + 'Z   administrator=' + $isAdmin + '   buffer=' + $BufferMB + ' MB   wait at most ' + $MaxMinutes + ' min')

# --- 2. providers that exist, and the profile ------------------------------------------------------------------------
$registered = @()
try { $registered = @(logman.exe query providers 2>$null | ForEach-Object { ([string]$_).Trim() }) } catch { $registered = @() }
$present = @()
foreach ($name in $wanted) {
    if (@($registered | Where-Object { $_ -like ($name + ' *') -or ($_ -eq $name) }).Count -gt 0) { $present += $name; Write-Note ('  provider present: ' + $name) }
    else { Write-Note ('  provider not on this PC (left out): ' + $name) }
}
$profilePath = Join-Path $OutFolder 'slipstream-stall.wprp'
$xml = @('<?xml version="1.0" encoding="utf-8"?>', '<WindowsPerformanceRecorder Version="1.0" Author="PushFPS stream s10-stall">', '  <Profiles>',
         '    <SystemCollector Id="SC_Stall" Name="NT Kernel Logger">', '      <BufferSize Value="1024"/>', ('      <Buffers Value="' + $BufferMB + '"/>'), '    </SystemCollector>',
         '    <EventCollector Id="EC_Stall" Name="Slipstream stall providers">', '      <BufferSize Value="256"/>', '      <Buffers Value="64"/>', '    </EventCollector>',
         '    <SystemProvider Id="SP_Stall">', '      <Keywords>')
foreach ($k in $kernelKeywords) { $xml += ('        <Keyword Value="' + $k + '"/>') }
$xml += @('      </Keywords>', '    </SystemProvider>')
$i = 0
foreach ($name in $present) { $xml += ('    <EventProvider Id="EP_' + $i + '" Name="' + $name + '"/>'); $i++ }
$xml += @('    <Profile Id="SlipstreamStall.Verbose.Memory" Name="SlipstreamStall" Description="PushFPS capture stall" LoggingMode="Memory" DetailLevel="Verbose">',
          '      <Collectors>', '        <SystemCollectorId Value="SC_Stall">', '          <SystemProviderId Value="SP_Stall"/>', '        </SystemCollectorId>',
          '        <EventCollectorId Value="EC_Stall">', '          <EventProviders>')
for ($j = 0; $j -lt $present.Count; $j++) { $xml += ('            <EventProviderId Value="EP_' + $j + '"/>') }
$xml += @('          </EventProviders>', '        </EventCollectorId>', '      </Collectors>', '    </Profile>', '  </Profiles>', '</WindowsPerformanceRecorder>')
Set-Content -LiteralPath $profilePath -Value $xml -Encoding UTF8
[void]([xml](Get-Content -LiteralPath $profilePath -Raw))   # well-formed, or this throws
Write-Note ('  profile: ' + $profilePath + ' (' + $kernelKeywords.Count + ' kernel keywords, ' + $present.Count + ' providers)')
if ($DryRun) { Write-Note '  DRY RUN: no recording started.'; exit 0 }

# --- 3-5. record, wait for a stall, save or cancel -------------------------------------------------------------------
$logPath = Join-Path $LogFolder 'veyra-qml.log'
if (-not (Test-Path -LiteralPath $logPath)) { throw ('PushFPS log not found: ' + $logPath) }
$etl = Join-Path $OutFolder ('stall-trace-' + $stamp + '.etl')
$saved = $false
$stallLine = ''
& wpr.exe -start ($profilePath + '!SlipstreamStall') 2>&1 | ForEach-Object { Write-Note ('  wpr: ' + $_) }
if ($LASTEXITCODE -ne 0) { Write-Note ('wpr -start failed (exit ' + $LASTEXITCODE + '). Nothing is recording.'); exit 3 }
try {
    $position = (Get-Item -LiteralPath $logPath).Length
    $deadline = (Get-Date).AddMinutes($MaxMinutes)
    Write-Note ('  recording in memory; waiting for the next [capture-stall] kind=gap line (until ' + $deadline.ToString('HH:mm:ss') + ' local)')
    while (((Get-Date) -lt $deadline) -and (-not $stallLine)) {
        Start-Sleep -Seconds 2
        $length = (Get-Item -LiteralPath $logPath).Length
        if ($length -lt $position) { $position = 0 }   # the log rotated
        if ($length -gt $position) {
            $stream = [System.IO.File]::Open($logPath, 'Open', 'Read', 'ReadWrite')
            try {
                [void]$stream.Seek($position, 'Begin')
                $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
                $chunk = $reader.ReadToEnd()
            } finally { $stream.Dispose() }
            $position = $length
            foreach ($line in ($chunk -split "`r?`n")) { if ($line -match '\[capture-stall\] kind=gap ') { $stallLine = $line; break } }
        }
    }
    if ($stallLine) {
        Write-Note ('  stall seen: ' + $stallLine.Substring(0, [math]::Min(160, $stallLine.Length)))
        Start-Sleep -Seconds 5
        & wpr.exe -stop $etl 'PushFPS capture stall' -skipPdbGen -compress 2>&1 | ForEach-Object { Write-Note ('  wpr: ' + $_) }
        $saved = ($LASTEXITCODE -eq 0) -and (Test-Path -LiteralPath $etl)
        if ($saved) { Write-Note ('  saved: ' + $etl + ' (' + [math]::Round((Get-Item -LiteralPath $etl).Length / 1MB, 1) + ' MB)') }
    } else { Write-Note '  no stall within the time: nothing saved.' }
} finally {
    if (-not $saved) { & wpr.exe -cancel 2>&1 | Out-Null; Write-Note '  recording cancelled (wpr -cancel).' }
}
if (-not $saved) { exit 1 }

# --- 6. text with built-in tools ---------------------------------------------------------------------------------------
$summary = Join-Path $OutFolder ('stall-trace-' + $stamp + '-summary.txt')
& tracerpt.exe $etl -summary $summary -y 2>&1 | Out-Null
if (Test-Path -LiteralPath $summary) { Write-Note ('  tracerpt summary: ' + $summary) } else { Write-Note '  tracerpt could not write a summary.' }
if ($ExportCsv) {
    $csv = Join-Path $OutFolder ('stall-trace-' + $stamp + '-all.csv')
    $window = Join-Path $OutFolder ('stall-trace-' + $stamp + '-window.csv')
    Write-Note '  tracerpt to CSV (this can take minutes) ...'
    & tracerpt.exe $etl -o $csv -of CSV -y 2>&1 | Out-Null
    if (Test-Path -LiteralPath $csv) {
        # Keep the rows whose Clock-Time (a FILETIME, UTC) lies from 5 s before to 3 s after the stall line's time.
        $stallUtc = [datetime]::ParseExact($stallLine.Substring(0, 23), 'yyyy-MM-ddTHH:mm:ss.fff', [System.Globalization.CultureInfo]::InvariantCulture,
                                           [System.Globalization.DateTimeStyles]'AdjustToUniversal,AssumeUniversal')
        $from = $stallUtc.AddSeconds(-5).ToFileTimeUtc()
        $to = $stallUtc.AddSeconds(3).ToFileTimeUtc()
        $reader = New-Object System.IO.StreamReader($csv)
        $writer = New-Object System.IO.StreamWriter($window)
        $kept = 0
        try {
            $header = $reader.ReadLine()
            $writer.WriteLine($header)
            $clock = [array]::IndexOf(@($header -split ',\s*'), 'Clock-Time')
            while ($null -ne ($row = $reader.ReadLine())) {
                $cells = $row -split ',\s*'
                $time = 0L
                if (($clock -ge 0) -and ($cells.Count -gt $clock) -and [long]::TryParse($cells[$clock], [ref]$time) -and ($time -ge $from) -and ($time -le $to)) { $writer.WriteLine($row); $kept++ }
                elseif ($clock -lt 0) { $writer.WriteLine($row); $kept++; if ($kept -gt 200000) { break } }
            }
        } finally { $reader.Dispose(); $writer.Dispose() }
        Remove-Item -LiteralPath $csv -Force
        Write-Note ('  window CSV: ' + $window + ' (' + $kept + ' rows; Clock-Time column ' + $(if ($clock -ge 0) { 'found' } else { 'NOT found: first 200000 rows kept' }) + ')')
    } else { Write-Note '  tracerpt could not write the CSV.' }
}
Write-Note 'done.'
