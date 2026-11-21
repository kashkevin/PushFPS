<#
.SYNOPSIS
  The measuring session of a Slipstream test package: checks the PC and the copies, then starts the kit
  (run-tests.ps1). Run-First-Test.cmd (quick session) and Run-Full-Test.cmd (full session) run this.

.DESCRIPTION
  Sessions (-Session):
    first   (default) step 1 of docs\HARDWARE_TEST_PROCEDURE.md: the original build against the stable candidate,
            three arms, three rounds, frame generation OFF, about 72 minutes with you at the PC.
    full    the same latency arms, then a 10-minute stability run and the reliability checks, with a break
            before each part (about 105 minutes; the kit shows its estimate first).
    versus  the stable candidate against the experimental build, one named switch set per arm (arms.csv set
            'versus'), frame generation OFF.
    switches  one switch per arm inside the experimental build, interleaved, without clicks (arms.csv set 'switches').
    quality   super resolution cost, frame generation and the frame quality test in the experimental build, automatic.
    dedup-probe  OPTIONAL diagnostic: 3 minutes of a game running at 30 FPS; never compared for latency.
    network   SmartLink: n-off, n-observe, n-apply, n-audio, 2 runs of 3 minutes each (about 40 minutes), automatic; play
              normally. Only screens for large effects; one question needs -Arms n-observe,n-apply -Rounds 6 (about 60 minutes).
    presentation  FIRST SESSION OF THIS SPRINT: four presentation arms of the experimental build (p-window, p-fullscreen,
              p-lowqueue-off, p-test-uncovered), 2 rounds of one 12-press PS-button test, automatic, about 32 minutes.
    audio     one audio pair of the experimental build (s-base against s-audio-smooth, clicks=), 2 rounds, automatic, about 16 minutes.
  Every session ends with one zip to send back and RESULT.txt on screen.

  CHECKS BEFORE ANYTHING STARTS, each with a one-line fix: the copies are installed (and which version), each copy
  has connected to a PS5 before (from its own log: "session started"; nothing private is read), no PushFPS is
  running (and which copy it is), this window is not running as administrator, PresentMon is there or not, the
  network route (Wi-Fi runs, labelled diagnostic only), the display runs at the highest refresh rate Windows reports, there is room for the
  evidence, and whether an unfinished session can be continued.
  What cannot be checked beforehand (stream quality, frame generation) is checked by the kit from the program's
  own log after every run; a run that differs is named and can be repeated at once.

  This script changes nothing on the PC. The kit gives every run its own switches and ignores slipstream.ini.
      original      %LOCALAPPDATA%\Programs\PushFPS-original\PushFPS.exe
      stable        %LOCALAPPDATA%\Programs\PushFPS-slipstream\PushFPS.exe
      experimental  %LOCALAPPDATA%\Programs\PushFPS-slipstream-exp\PushFPS.exe

  Windows PowerShell 5.1 or newer. Start it normally, not as administrator. Unverified on a real PC: if something
  fails, send the text of this window.

.PARAMETER Session
  first (default), full, versus, switches, quality, dedup-probe or network.
.PARAMETER Build
  first and full sessions: stable (default) or experimental, the copy compared with the original.
.PARAMETER InstallBase
  Folder the copies are under. Default: %LOCALAPPDATA%\Programs.
.PARAMETER PackageRoot
  The unzipped package; a PresentMon console program placed there is used. Default: found from this script's place.
.PARAMETER OutRoot
  Where the evidence goes. Default: Documents\slipstream-evidence.
.PARAMETER NewSession
  Do not offer to continue an unfinished session.
.PARAMETER AllowElevated
  Only where every program runs as administrator anyway (User Account Control off, or the built-in Administrator
  account): go on although this window is elevated. Passed on to the kit.
.PARAMETER CheckOnly
  Only check and print what would be started.
.PARAMETER Minutes
  -Session network: minutes per run (passed to run-tests.ps1 -Minutes; default 3 there).
.PARAMETER Rounds
  -Session network: runs per arm (passed to run-tests.ps1 -Rounds; default 2 there: that only screens for large effects;
  a real verdict on one question needs about 6 runs per arm with 2 arms, -Arms n-observe,n-apply -Rounds 6).
.PARAMETER Arms
  Only these arms of the session, comma-separated names from arms.csv (passed to run-tests.ps1 -Arms).
.PARAMETER NetworkWithBuilds
  -Session network: adds the original and the stable copy (passed to run-tests.ps1 with the original copy).
.PARAMETER WifiStreamingMode
  -Session network only: adds the opt-in Wi-Fi streaming mode arm (passed to run-tests.ps1; never by default).
.PARAMETER DryRun
  No PushFPS, no questions: the checks run, missing copies are only reported, and the kit runs on made-up data.
#>
param(
    [ValidateSet('first', 'full', 'versus', 'switches', 'quality', 'dedup-probe', 'network', 'presentation', 'audio')][string]$Session = 'first',
    [ValidateSet('stable', 'experimental')][string]$Build = 'stable',
    [string]$InstallBase = '',
    [string]$PackageRoot = '',
    [string]$OutRoot = '',
    [switch]$NewSession,
    [switch]$CheckOnly,
    [switch]$AllowElevated,
    [switch]$PresentMonPrompt,
    [int]$Minutes = 0,
    [int]$Rounds = 0,
    [string]$Arms = '',
    [switch]$NetworkWithBuilds,
    [switch]$WifiStreamingMode,
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$scriptFolder = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptFolder 'slipstream-common.ps1')

$script:fixes = 0
# One line per check: ok, note (worth knowing, nothing to do) or FIX (with what to do).
function Write-Check([string]$State, [string]$Text, [string]$Fix) {
    if ($State -eq 'ok') { Write-Host ('   ok    ' + $Text) -ForegroundColor Green }
    elseif ($State -eq 'note') { Write-Host ('   note  ' + $Text) -ForegroundColor Yellow }
    else {
        $script:fixes++
        Write-Host ('   FIX   ' + $Text) -ForegroundColor Red
    }
    if ($Fix) { Write-Host ('         -> ' + $Fix) }
}

$base = Get-SlipstreamInstallBase $InstallBase
$kit = Join-Path $scriptFolder 'run-tests.ps1'
if (-not $PackageRoot) {
    # testkit\scripts\remoteplay\testkit -> the package folder, when this runs from a package
    $guess = $scriptFolder
    for ($level = 0; $level -lt 4; $level++) { $guess = Split-Path -Parent $guess }
    if ($guess -and (Test-Path -LiteralPath (Join-Path $guess 'START-HERE.txt'))) { $PackageRoot = $guess }
}
if (-not $OutRoot) {
    $documents = [Environment]::GetFolderPath('MyDocuments')
    if (-not $documents) { $documents = (Get-Location).Path }
    $OutRoot = Join-Path $documents 'slipstream-evidence'
}

$titles = @{ first = 'first test: original against the ' + $Build + ' copy'; full = 'full test: latency, stability and reliability'; versus = 'stable candidate against the experimental build'
    switches = 'one switch per arm inside the experimental build (automatic)'; quality = 'super resolution, frame generation and quality test (automatic)'; 'dedup-probe' = 'repeated pictures of a 30 FPS game (optional diagnostic)'; network = 'SmartLink network session (automatic; play normally)'
    presentation = 'presentation: is the video window composed or flipped directly? (automatic, experimental build)'; audio = 'audio pair: clicks with and without audio smoothing (automatic, experimental build)' }
Write-Step ('Slipstream test package: ' + $titles[$Session])
Write-Line 'Step 1 of 3: checks (nothing is started or changed)'

# --- the copies -----------------------------------------------------------------------------------------
$needed = @()
if (@('presentation', 'audio') -contains $Session) { $needed = @('experimental') }
elseif (@('versus', 'switches', 'quality', 'dedup-probe', 'network') -contains $Session) { $needed = @('stable', 'experimental') } else { $needed = @('original', $Build) }
$exes = @{}
$missing = 0
if (-not (Test-Path -LiteralPath $kit)) { Write-Check 'FIX' ('the test kit is not next to this script: ' + $scriptFolder) 'extract the whole package zip again'; $missing++ }
foreach ($slot in $needed) {
    $copy = @($SlipstreamBuilds | Where-Object { $_.Slot -eq $slot })[0]
    $folder = Join-Path $base $copy.Folder
    $exe = Join-Path $folder 'PushFPS.exe'
    if (-not (Test-Path -LiteralPath $exe)) {
        Write-Check 'FIX' ('Not found: the ' + $copy.Title + ': ' + $exe) 'run Install-Slipstream.cmd first (from your own account, not an administrator account)'
        $missing++
        continue
    }
    $exes[$slot] = $exe
    $marker = Read-SlipstreamMarker $folder
    $version = [string]$marker['version']
    if (-not $version) { $version = 'version unknown (no ' + $SlipstreamMarker + ')' }
    Write-Check 'ok' ($slot + ': ' + $version + '  (' + $exe + ')') ''
    # Has this copy ever shown a PS5 stream? Its own log says so; nothing else is read from it.
    $connected = $false
    $logs = Join-Path $folder 'logs'
    if (Test-Path -LiteralPath $logs) {
        foreach ($file in @(Get-ChildItem -LiteralPath $logs -Filter '*.log' -ErrorAction SilentlyContinue)) {
            if ($connected) { break }
            try { if (Select-String -LiteralPath $file.FullName -Pattern '\] \[remoteplay\] session started' -SimpleMatch:$false -Quiet) { $connected = $true } } catch { $connected = $false }
        }
    }
    if ($connected) { Write-Check 'ok' ($slot + ': has streamed from a PS5 before') '' }
    else { Write-Check 'note' ($slot + ': no PS5 stream in its log yet') ('start it once, pair it with the PS5 if it asks (docs\SETUP.md, section 3), connect, then close it') }
}

# --- running PushFPS ----------------------------------------------------------------------------------------
function Get-RunningCopies {
    $names = @()
    foreach ($process in @(Get-Process -Name 'PushFPS' -ErrorAction SilentlyContinue)) {
        $path = ''
        try { $path = [string]$process.Path } catch { $path = '' }
        $name = 'a PushFPS that is not a test copy (an official install?)'
        foreach ($copy in $SlipstreamBuilds) {
            if ($path -and $path.StartsWith((Join-Path $base $copy.Folder) + '\', [System.StringComparison]::OrdinalIgnoreCase)) { $name = 'the ' + $copy.Title }
        }
        if (-not $path) { $name = 'a PushFPS whose folder cannot be read' }
        $names += $name
    }
    return $names
}
$running = @(Get-RunningCopies)
if ($running.Count -eq 0) { Write-Check 'ok' 'no PushFPS is running' '' }
else { Write-Check 'FIX' ('PushFPS is running: ' + ($running -join '; ')) 'close it (the kit starts each copy itself)' }

# --- administrator ----------------------------------------------------------------------------------------
$elevated = $false
try {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    $elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
} catch { $elevated = $false }
if ($elevated -and (-not $DryRun) -and (-not $AllowElevated)) { Write-Check 'FIX' 'this window runs as administrator' 'close it and double-click the .cmd file normally: PushFPS must run with your normal rights. With User Account Control off (or as the built-in Administrator) every program is elevated: start it with -AllowElevated' }
elseif ($elevated) { Write-Check 'note' 'this window runs as administrator (allowed: dry run or -AllowElevated)' '' }
else { Write-Check 'ok' 'normal rights (PushFPS will run without administrator rights)' '' }

# --- PresentMon ---------------------------------------------------------------------------------------------
$presentMon = ''
foreach ($folder in @($PackageRoot, $scriptFolder)) {
    if ($presentMon -or (-not $folder)) { continue }
    $found = @(Get-ChildItem -LiteralPath $folder -Filter 'PresentMon*.exe' -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch 'Service' } | Select-Object -First 1)
    if ($found.Count -gt 0) { $presentMon = $found[0].FullName }
}
if ($presentMon) { Write-Check 'ok' ('PresentMon: ' + [System.IO.Path]::GetFileName($presentMon) + ' (its signature is checked; Windows asks once per run to let it read frame times)') '' }
else { Write-Check 'note' 'PresentMon not found: frame times come from the program''s own lines only' 'optional: put PresentMon-<version>-x64.exe (GitHub: GameTechDev/PresentMon) next to Run-First-Test.cmd' }

# --- network and display (detect-hardware.ps1, which changes nothing) -----------------------------------------
$detect = Join-Path (Split-Path -Parent $scriptFolder) 'detect-hardware.ps1'
$hardwareFile = Join-Path $env:TEMP ('slipstream-check-' + [guid]::NewGuid().ToString('N') + '.json')
try {
    & $detect -OutFile $hardwareFile -ExpectRefreshHz 0 *> $null
    $hardware = Get-Content -LiteralPath $hardwareFile -Raw | ConvertFrom-Json
    $route = @($hardware.network | Where-Object { $_ -and $_.defaultRoute }) | Select-Object -First 1
    if (-not $route) { Write-Check 'note' 'the network route could not be read' '' }
    elseif ([string]$route.media -match '802\.11|Wireless|Wi-?Fi' -or [string]$route.description -match 'Wi-?Fi|Wireless') { Write-Check 'note' 'the PC reaches the network over Wi-Fi: the test runs, and every result is labelled "Wi-Fi: diagnostic only, not a wired-LAN latency conclusion"' 'for a wired-LAN conclusion: connect the PC with a network cable (and the PS5 too)' }
    else { Write-Check 'ok' ('network: ' + [string]$route.linkSpeed + ' over the adapter of the default route') '' }
    $screens = @($hardware.adapters | Where-Object { $_ -and $_.refreshHz })
    foreach ($screen in $screens) {
        $now = [int]$screen.refreshHz
        $top = [int]$screen.maxRefreshHz
        if (($top -gt 0) -and ($now -lt $top)) { Write-Check 'note' ('display at ' + $now + ' Hz; Windows reports up to ' + $top + ' Hz') 'Settings > System > Display > Advanced display: choose the highest rate, unless you test at this one on purpose' }
        else { Write-Check 'ok' ('display at ' + $now + ' Hz') '' }
    }
} catch {
    Write-Check 'note' ('the network and display check did not run: ' + $_.Exception.Message) ''
} finally {
    Remove-Item -LiteralPath $hardwareFile -Force -ErrorAction SilentlyContinue
}

# --- room for the evidence ---------------------------------------------------------------------------------
try {
    $drive = New-Object System.IO.DriveInfo ([System.IO.Path]::GetPathRoot($OutRoot))
    $freeGb = [math]::Round($drive.AvailableFreeSpace / 1GB, 1)
    if ($freeGb -lt 2) { Write-Check 'FIX' ('only ' + $freeGb + ' GB free for the evidence') 'free at least 2 GB on that drive' }
    else { Write-Check 'ok' ($freeGb.ToString([System.Globalization.CultureInfo]::InvariantCulture) + ' GB free for the evidence') '' }
} catch { Write-Check 'note' 'free space could not be read' '' }

# --- an unfinished session ------------------------------------------------------------------------------------
$resume = ''
if ((-not $NewSession) -and (-not $DryRun) -and (Test-Path -LiteralPath $OutRoot)) {
    foreach ($folder in @(Get-ChildItem -LiteralPath $OutRoot -Directory -Filter 'evidence-*' | Sort-Object Name -Descending | Select-Object -First 1)) {
        $young = ((Get-Date) - $folder.LastWriteTime).TotalDays -lt 3
        $kitFile = Join-Path $folder.FullName 'kit.json'
        $finished = $true
        if (Test-Path -LiteralPath $kitFile) { try { $finished = [bool](Get-Content -LiteralPath $kitFile -Raw | ConvertFrom-Json).finished } catch { $finished = $true } }
        # A session stopped with q has its zip but no finished=true in kit.json: it can be continued.
        if ($young -and (-not $finished)) { $resume = $folder.FullName }
    }
}
if ($resume) { Write-Check 'note' ('an unfinished session was found: ' + (Split-Path -Leaf $resume)) 'you can continue it below; its finished runs are kept' }

Write-Host ''
if ($missing -gt 0) {
    Write-Line 'Run Install-Slipstream.cmd first. If you started this window as another (administrator) account,'
    Write-Line 'the copies are in your own account: start it from your own account instead.'
    if (-not $DryRun) { exit 1 }
}

Write-Line 'Step 2 of 3: what only you can set (the kit checks it from the log after every run)'
Write-Line '  - frame generation OFF in every copy; the same stream quality, codec and bitrate in every copy'
Write-Line '  - the game loaded, standing in a calm place you can return to; in-game settings untouched'
Write-Line '  - for each start: connect (later starts reconnect by themselves), run the latency test in the PS5 streaming menu as many times as the kit prints; the kit closes PushFPS'
if ($CheckOnly) {
    Write-Line 'Check only: nothing started.'
    exit 0
}
if ($elevated -and (-not $DryRun) -and (-not $AllowElevated)) { exit 1 }
if ((-not $DryRun) -and ($script:fixes -gt 0)) {
    $answer = Read-Host 'Some checks say FIX. Enter = go on anyway, q = stop here and fix them first'
    if ($answer -eq 'q') { exit 1 }
}
while ((-not $DryRun) -and (@(Get-RunningCopies).Count -gt 0)) {
    Write-Host '   Waiting until every PushFPS window is closed ...' -ForegroundColor Yellow
    Start-Sleep -Seconds 3
}
if ($resume) {
    $answer = Read-Host ('Enter = continue the unfinished session ' + (Split-Path -Leaf $resume) + ', n = start a new one')
    if ($answer -eq 'n') { $resume = '' }
}

Write-Line 'Step 3 of 3: the kit'
$arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $kit, '-OutRoot', $OutRoot)
if (@('switches', 'quality', 'dedup-probe', 'network', 'presentation', 'audio') -contains $Session) {
    $arguments += @('-Session', $Session)
    if (($Session -eq 'network') -and $NetworkWithBuilds -and $exes.ContainsKey('original')) { $arguments += @('-OriginalExe', $exes['original']) }
    if ($exes.ContainsKey('stable')) { $arguments += @('-ModifiedExe', $exes['stable']) }
    if ($exes.ContainsKey('experimental')) { $arguments += @('-ExperimentalExe', $exes['experimental']) }
} elseif ($Session -eq 'versus') {
    $arguments += @('-Versus')
    if ($exes.ContainsKey('stable')) { $arguments += @('-ModifiedExe', $exes['stable']) }
    if ($exes.ContainsKey('experimental')) { $arguments += @('-ExperimentalExe', $exes['experimental']) }
} else {
    $arguments += @('-Quick')
    if ($exes.ContainsKey('original')) { $arguments += @('-OriginalExe', $exes['original']) }
    if ($exes.ContainsKey($Build)) { $arguments += @('-ModifiedExe', $exes[$Build]) }
}
if ($Session -eq 'full') { $arguments += @('-Full') }
if ($presentMon) { $arguments += @('-PresentMon', $presentMon) }
if ($resume) { $arguments += @('-Resume', $resume) }
if ($AllowElevated) { $arguments += @('-AllowElevated') }
if ($PresentMonPrompt) { $arguments += @('-PresentMonPrompt') }
if ($Minutes -gt 0) { $arguments += @('-Minutes', [string]$Minutes) }
if ($Rounds -gt 0) { $arguments += @('-Rounds', [string]$Rounds) }
if ($Arms) {
    if ($Arms -notmatch '^[a-z0-9-]+(,[a-z0-9-]+)*$') { throw "-Arms: comma-separated arm names from arms.csv (letters, digits and '-'), for example n-observe,n-apply" }
    $arguments += @('-Arms', $Arms)
}
if ($NetworkWithBuilds) { $arguments += @('-NetworkWithBuilds') }
if ($WifiStreamingMode) { $arguments += @('-WifiStreamingMode') }
if ($DryRun) { $arguments += @('-DryRun') }

$powershellExe = 'powershell.exe'
if ($env:SystemRoot) {
    $candidate = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path -LiteralPath $candidate) { $powershellExe = $candidate }
}
& $powershellExe @arguments
exit $LASTEXITCODE
