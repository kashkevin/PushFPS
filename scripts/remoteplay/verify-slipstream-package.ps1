<#
.SYNOPSIS
  Runs the scripts of a Slipstream test package as a user would, on a throw-away Windows machine.
  Used by .github/workflows/slipstream-package.yml (job "verify"). Not part of the package.

.DESCRIPTION
  Unpacks the package zip into a folder with a space and brackets in its name, then:
    1. checks the layout, SHA256SUMS.txt, line endings, and parses every .ps1 in it;
    2. double-click equivalent of Install-Slipstream.cmd into <Work>\Local Programs (never the real
       profile), with a stand-in "official" PushFPS folder and stand-in PushFPS shortcuts beside it;
    3. starts every installed copy once and reads its update check from the log: it must never be
       offered a release;
    4. installs again (nothing may be copied), checks Run-First-Test.cmd, Rollback-Slipstream.cmd,
       Uninstall-Slipstream.cmd (no answer, a junction inside, an unmarked folder, then for real);
    5. the stand-ins must be exactly as they were.
  Builds that are missing from the package are skipped; with no build at all only step 1 and the
  "nothing to do" paths run. Exit code 1 when anything is wrong; every line is also in <Work>\verify-log.txt.

  This establishes that the scripts work on Windows PowerShell 5.1 on GitHub's runner (no graphics card,
  no PS5). It says nothing about a real PC, pairing, video or latency.

.PARAMETER Zip
  The package zip.
.PARAMETER Work
  An empty or new folder for everything this script writes.
.PARAMETER Gpu
  Runtime package to install (the runner has no graphics card, so Auto would pick AMD). Default NVIDIA.
.PARAMETER SkipStart
  Do not start the installed copies.
#>
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [Parameter(Mandatory = $true)][string]$Work,
    [ValidateSet('NVIDIA', 'AMD')][string]$Gpu = 'NVIDIA',
    [switch]$SkipStart
)
$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Zip)) { throw "Package zip not found: $Zip" }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Work = (Resolve-Path -LiteralPath $Work).Path.TrimEnd('\')
$logFile = Join-Path $Work 'verify-log.txt'
Set-Content -LiteralPath $logFile -Value ('verify-slipstream-package ' + (Get-Date).ToUniversalTime().ToString('s') + 'Z') -Encoding ASCII
$problems = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]

function Say([string]$Text) {
    Write-Host $Text
    Add-Content -LiteralPath $logFile -Value $Text -Encoding ASCII
}
function Check([bool]$Good, [string]$What) {
    if ($Good) { Say ('ok    ' + $What) }
    else { $problems.Add($What); Say ('FAIL  ' + $What) }
}
function Section([string]$Text) { Say ''; Say ('== ' + $Text) }
function Get-Sha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try { return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}

# Starts a .cmd of the package the way a double click does (cmd.exe, no input: "pause" returns at once)
# and returns its exit code and output. $Arguments is appended as typed.
$script:runNumber = 0
function Invoke-PackageCmd([string]$Name, [string]$Arguments) {
    $script:runNumber++
    $out = Join-Path $Work ('verify-run-{0:00}-{1}.txt' -f $script:runNumber, $Name.Replace('.cmd', ''))
    $err = $out.Replace('.txt', '-stderr.txt')
    $line = '/d /s /c ""' + (Join-Path $package $Name) + '" ' + $Arguments + '"'
    Say ('run   ' + $Name + ' ' + $Arguments)
    $process = Start-Process -FilePath 'cmd.exe' -ArgumentList $line -Wait -PassThru -NoNewWindow -RedirectStandardInput $emptyInput -RedirectStandardOutput $out -RedirectStandardError $err
    $text = ''
    if (Test-Path -LiteralPath $out) { $text = [string](Get-Content -LiteralPath $out -Raw) }
    if ((Test-Path -LiteralPath $err) -and ((Get-Item -LiteralPath $err).Length -gt 0)) { $text += "`n" + [string](Get-Content -LiteralPath $err -Raw) }
    foreach ($row in ($text -split "`r?`n")) { if ($row.Trim()) { Add-Content -LiteralPath $logFile -Value ('      | ' + $row) -Encoding ASCII } }
    Say ('      exit code ' + $process.ExitCode)
    return [pscustomobject]@{ Code = $process.ExitCode; Text = $text }
}

# --- 1. unpack and static checks ----------------------------------------------------------------
Section 'Unpack'
$package = Join-Path $Work 'Downloads (1)\Slipstream test package'
if (Test-Path -LiteralPath $package) { Remove-Item -LiteralPath $package -Recurse -Force }
New-Item -ItemType Directory -Force -Path $package | Out-Null
# As Explorer's "Extract All" would; Windows' own tar is much faster on a large zip than Expand-Archive.
$tar = Join-Path $env:SystemRoot 'System32\tar.exe'
if (Test-Path -LiteralPath $tar) {
    & $tar -xf $Zip -C $package
    if ($LASTEXITCODE -ne 0) { throw "tar could not unpack the package (exit $LASTEXITCODE)" }
} else {
    Expand-Archive -LiteralPath $Zip -DestinationPath $package -Force
}
$emptyInput = Join-Path $Work 'empty-input.txt'
Set-Content -LiteralPath $emptyInput -Value '' -Encoding ASCII
$kit = Join-Path $package 'testkit\scripts\remoteplay\testkit'

foreach ($name in @('START-HERE.txt', 'SHA256SUMS.txt', 'Install-Slipstream.cmd', 'Run-First-Test.cmd', 'Rollback-Slipstream.cmd', 'Uninstall-Slipstream.cmd',
                    'testkit\scripts\remoteplay\testkit\run-tests.ps1', 'testkit\scripts\remoteplay\testkit\arms.csv',
                    'testkit\scripts\remoteplay\testkit\install-slipstream.ps1', 'testkit\scripts\remoteplay\testkit\uninstall-slipstream.ps1',
                    'testkit\scripts\remoteplay\testkit\run-first-test.ps1', 'testkit\scripts\remoteplay\testkit\slipstream-common.ps1',
                    'testkit\scripts\remoteplay\detect-hardware.ps1', 'testkit\tools\remoteplay_latency_bench\report.py',
                    'docs\SETUP.md', 'docs\HARDWARE_TEST_PROCEDURE.md')) {
    Check (Test-Path -LiteralPath (Join-Path $package $name)) ('in the package: ' + $name)
}

Section 'SHA256SUMS.txt'
$listed = 0
$bad = 0
$sumsFile = Join-Path $package 'SHA256SUMS.txt'
if (Test-Path -LiteralPath $sumsFile) {
    foreach ($line in @(Get-Content -LiteralPath $sumsFile)) {
        if ($line -notmatch '^([0-9a-f]{64})  (.+)$') { if ($line.Trim()) { $bad++ }; continue }
        $listed++
        $path = Join-Path $package ($Matches[2].Replace('/', '\'))
        $expected = $Matches[1]
        if ((-not (Test-Path -LiteralPath $path)) -or ((Get-Sha256 $path) -ne $expected)) { $bad++; Say ('      wrong: ' + $Matches[2]) }
    }
}
$onDisk = @(Get-ChildItem -LiteralPath $package -Recurse -File | Where-Object { $_.FullName -ne $sumsFile }).Count
Check (($listed -gt 0) -and ($bad -eq 0)) ("every listed file is there and matches ($listed listed, $bad wrong)")
Check ($listed -eq $onDisk) ("every file is listed ($onDisk files, $listed listed)")

Section 'Scripts'
foreach ($file in @(Get-ChildItem -LiteralPath $package -Filter *.cmd)) {
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
    $text = [System.Text.Encoding]::ASCII.GetString($bytes)
    $ascii = @($bytes | Where-Object { $_ -gt 126 }).Count -eq 0
    $crlf = ($text -replace "`r`n", '').IndexOf("`n") -lt 0
    Check ($ascii -and $crlf) ($file.Name + ': ASCII with Windows line endings')
}
foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $package 'testkit') -Filter *.ps1 -Recurse)) {
    $tokens = $null; $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($file.FullName, [ref]$tokens, [ref]$errors) | Out-Null
    Check ($errors.Count -eq 0) ($file.Name + ' parses')
    foreach ($e in $errors) { Say ('      line ' + $e.Extent.StartLineNumber + ': ' + $e.Message) }
}
$startHere = ''
if (Test-Path -LiteralPath (Join-Path $package 'START-HERE.txt')) { $startHere = [string](Get-Content -LiteralPath (Join-Path $package 'START-HERE.txt') -Raw) }

$slots = @(
    [pscustomobject]@{ Slot = 'original';     Folder = 'PushFPS-original' },
    [pscustomobject]@{ Slot = 'stable';       Folder = 'PushFPS-slipstream' },
    [pscustomobject]@{ Slot = 'experimental'; Folder = 'PushFPS-slipstream-exp' }
)
$present = @($slots | Where-Object { Test-Path -LiteralPath (Join-Path (Join-Path $package $_.Slot) 'Install-PushFPS.ps1') })
$absent = @($slots | Where-Object { -not (Test-Path -LiteralPath (Join-Path (Join-Path $package $_.Slot) 'Install-PushFPS.ps1')) })
Say ('builds in the package: ' + (($present | ForEach-Object { $_.Slot }) -join ', '))
foreach ($slot in $absent) { Check ($startHere -match ('(?m)^\s*' + $slot.Slot + ': ')) ('START-HERE.txt says why ' + $slot.Slot + ' is missing') }
if ($present.Count -eq 0) { $warnings.Add('no build is in this package: install, start and uninstall were only checked for "nothing to do"') }

# --- 2. stand-ins that must survive, then install -----------------------------------------------
Section 'Install'
$base = Join-Path $Work 'Local Programs'
if (Test-Path -LiteralPath $base) { Remove-Item -LiteralPath $base -Recurse -Force }
$official = Join-Path $base 'PushFPS'
New-Item -ItemType Directory -Force -Path (Join-Path $official 'runtime_local\user-data-2.0.0') | Out-Null
Set-Content -LiteralPath (Join-Path $official 'PushFPS.exe') -Value 'stand-in for an official install' -Encoding ASCII
Set-Content -LiteralPath (Join-Path $official 'runtime_local\user-data-2.0.0\preferences.json') -Value '{"stand-in":true}' -Encoding ASCII
Set-Content -LiteralPath (Join-Path $official 'PushFPS-VERSION.txt') -Value 'PushFPS 3.2.1' -Encoding ASCII
$shortcuts = @((Join-Path ([Environment]::GetFolderPath('Programs')) 'PushFPS.lnk'), (Join-Path ([Environment]::GetFolderPath('Desktop')) 'PushFPS.lnk'))
$madeShortcuts = @()
foreach ($link in $shortcuts) {
    if (-not (Test-Path -LiteralPath $link)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $link) | Out-Null
        Set-Content -LiteralPath $link -Value 'stand-in shortcut' -Encoding ASCII
        $madeShortcuts += $link
    }
}
function Get-StandInState {
    $parts = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $official -Recurse -File | Sort-Object FullName)) { $parts += ($file.FullName.Substring($official.Length) + '=' + (Get-Sha256 $file.FullName)) }
    foreach ($link in $shortcuts) { $parts += ($link + '=' + (Test-Path -LiteralPath $link)) }
    return ($parts -join ';')
}
$standInBefore = Get-StandInState
$baseArgument = '-InstallBase "' + $base + '"'

$first = Invoke-PackageCmd 'Install-Slipstream.cmd' ($baseArgument + ' -Gpu ' + $Gpu)
Check ($first.Code -eq 0) 'Install-Slipstream.cmd exit code 0'
foreach ($slot in $absent) { Check ($first.Text -match ('== ' + $slot.Slot + ':[^=]*?not in the package')) ($slot.Slot + ': reported as not in the package') }
$installed = @()
foreach ($slot in $present) {
    $folder = Join-Path $base $slot.Folder
    $ok = Test-Path -LiteralPath (Join-Path $folder 'PushFPS.exe')
    Check $ok ($slot.Slot + ': PushFPS.exe installed in ' + $slot.Folder)
    if (-not $ok) { continue }
    $installed += $slot
    Check (Test-Path -LiteralPath (Join-Path $folder 'Slipstream-INSTALL.txt')) ($slot.Slot + ': Slipstream-INSTALL.txt written')
    $record = @(Get-Content -LiteralPath (Join-Path $folder 'PushFPS-VERSION.txt'))
    Check ($record[0] -eq 'PushFPS 999.0.0') ($slot.Slot + ': update guard is the first line of PushFPS-VERSION.txt')
    Check (@($record | Where-Object { $_ -match '^Runtime: ' }).Count -eq 1) ($slot.Slot + ': the installer''s own record is kept')
    $ini = Join-Path $folder 'slipstream.ini'
    if ($slot.Slot -eq 'stable') {
        Check ((Test-Path -LiteralPath $ini) -and (@(Get-Content -LiteralPath $ini) -contains 'mode = ultra-low-latency')) 'stable: slipstream.ini selects ultra-low-latency'
    } else {
        Check (-not (Test-Path -LiteralPath $ini)) ($slot.Slot + ': no slipstream.ini written')
    }
    if (Test-Path -LiteralPath (Join-Path $folder 'runtime_local\user-data-2.0.0')) { $warnings.Add($slot.Slot + ': the fresh copy already holds a settings folder (runtime_local\user-data-2.0.0); look at what the runtime package ships there') }
}
Check ((Get-StandInState) -eq $standInBefore) 'official stand-in and shortcuts untouched by the install'

# --- 3. start every copy: no release may be offered ---------------------------------------------
if (-not $SkipStart) {
    Section 'Start each copy and read its update check'
    foreach ($slot in $installed) {
        $folder = Join-Path $base $slot.Folder
        $log = Join-Path $Work ('verify-start-' + $slot.Slot + '.txt')
        if (Test-Path -LiteralPath $log) { Remove-Item -LiteralPath $log -Force }
        $env:VEYRA_LOG_FILE = $log
        $data = Join-Path $Work ('data-' + $slot.Slot)
        $process = Start-Process -FilePath (Join-Path $folder 'PushFPS.exe') -WorkingDirectory $folder -PassThru -ArgumentList ('--data-dir "' + $data + '" --exit-after 20000')
        if (-not $process.WaitForExit(60000)) { try { Stop-Process -Id $process.Id -Force } catch { } }
        Remove-Item Env:VEYRA_LOG_FILE -ErrorAction SilentlyContinue
        $text = ''
        if (Test-Path -LiteralPath $log) { $text = [string](Get-Content -LiteralPath $log -Raw) }
        Check ($text -match 'PushFPS QML session started') ($slot.Slot + ': the installed copy starts')
        $lines = @(($text -split "`r?`n") | Where-Object { $_ -match '\[update-(check|install)' })
        foreach ($line in $lines) { Say ('      ' + $line.Trim()) }
        Check (@($lines | Where-Object { $_ -match '\[update-check\]\s+available' }).Count -eq 0) ($slot.Slot + ': no release was offered')
        Check (@($lines | Where-Object { $_ -match '\[update-install\]' }).Count -eq 0) ($slot.Slot + ': no update install was started')
        $guarded = @($lines | Where-Object { $_ -match '\[update-check\]\s+(disabled:|current)' }).Count -gt 0
        if ($guarded) { Say ('ok    ' + $slot.Slot + ': the update check answered "disabled" or "current"') }
        else { $warnings.Add($slot.Slot + ': the update check gave no "disabled" or "current" line (no network, or GitHub refused the request): the guard was not shown to work in this run') }
        if (($slot.Slot -eq 'stable') -and ($text -match '\[remoteplay-settings\]')) {
            foreach ($line in @(($text -split "`r?`n") | Where-Object { $_ -match '\[remoteplay-settings\]' } | Select-Object -First 3)) { Say ('      ' + $line.Trim()) }
            Check ($text -notmatch 'slipstream\.ini ignored') 'stable: the written slipstream.ini is accepted'
        }
    }
}

# --- 4. again, first test, rollback, uninstall --------------------------------------------------
Section 'Install again (nothing may be copied)'
$second = Invoke-PackageCmd 'Install-Slipstream.cmd' ($baseArgument + ' -Gpu ' + $Gpu)
Check ($second.Code -eq 0) 'second run: exit code 0'
foreach ($slot in $installed) {
    Check ($second.Text -match ('== ' + $slot.Slot + ':[^=]*?Already holds this build')) ($slot.Slot + ': left alone on the second run')
    $record = @(Get-Content -LiteralPath (Join-Path (Join-Path $base $slot.Folder) 'PushFPS-VERSION.txt'))
    Check (@($record | Where-Object { $_ -eq 'PushFPS 999.0.0' }).Count -eq 1) ($slot.Slot + ': the update guard is there once')
}

Section 'Run-First-Test.cmd -CheckOnly'
$check = Invoke-PackageCmd 'Run-First-Test.cmd' ($baseArgument + ' -CheckOnly')
$both = (@($installed | Where-Object { $_.Slot -eq 'original' }).Count -eq 1) -and (@($installed | Where-Object { $_.Slot -eq 'stable' }).Count -eq 1)
if ($both) { Check (($check.Code -eq 0) -and ($check.Text -match 'Check only: nothing started')) 'finds the original and the stable candidate' }
else { Check (($check.Code -eq 1) -and ($check.Text -match 'Not found')) 'says which copy is missing' }

Section 'Rollback-Slipstream.cmd'
$rollback = Invoke-PackageCmd 'Rollback-Slipstream.cmd' $baseArgument
Check ($rollback.Code -eq 0) 'rollback: exit code 0'
$stableFolder = Join-Path $base 'PushFPS-slipstream'
if (@($installed | Where-Object { $_.Slot -eq 'stable' }).Count -eq 1) {
    Check (-not (Test-Path -LiteralPath (Join-Path $stableFolder 'slipstream.ini'))) 'stable: slipstream.ini is switched off'
    Check (@(Get-ChildItem -LiteralPath $stableFolder -Filter 'slipstream.ini.off-*').Count -eq 1) 'stable: the file was renamed, not deleted'
    $third = Invoke-PackageCmd 'Install-Slipstream.cmd' ($baseArgument + ' -Gpu ' + $Gpu)
    Check (($third.Code -eq 0) -and (-not (Test-Path -LiteralPath (Join-Path $stableFolder 'slipstream.ini')))) 'stable: a later install does not undo the rollback'
}
foreach ($slot in $installed) { Check (Test-Path -LiteralPath (Join-Path (Join-Path $base $slot.Folder) 'PushFPS.exe')) ($slot.Slot + ': still installed after the rollback') }

Section 'Uninstall-Slipstream.cmd'
$unanswered = Invoke-PackageCmd 'Uninstall-Slipstream.cmd' $baseArgument
Check ($unanswered.Code -eq 0) 'uninstall with nobody to confirm: exit code 0, nothing done'
foreach ($slot in $installed) { Check (Test-Path -LiteralPath (Join-Path (Join-Path $base $slot.Folder) 'PushFPS.exe')) ($slot.Slot + ': kept when nobody confirms') }

if ($installed.Count -gt 0) {
    $firstCopy = $installed[0]
    $lastCopy = $installed[$installed.Count - 1]
    $several = $lastCopy.Slot -ne $firstCopy.Slot
    $firstFolder = Join-Path $base $firstCopy.Folder
    $lastFolder = Join-Path $base $lastCopy.Folder

    # A junction inside the first copy that points at the official stand-in: that copy must be refused
    # and the target kept. A folder of the right name without the marker (the last copy) is not ours.
    $junction = Join-Path $firstFolder 'link-to-official'
    & cmd.exe /d /c mklink /J "$junction" "$official" | Out-Null
    $haveJunction = Test-Path -LiteralPath $junction
    if (-not $haveJunction) { $warnings.Add('the junction could not be made: that refusal was not tested') }
    if ($several) { Remove-Item -LiteralPath (Join-Path $lastFolder 'Slipstream-INSTALL.txt') -Force }
    $refused = Invoke-PackageCmd 'Uninstall-Slipstream.cmd' ($baseArgument + ' -Yes')
    if ($haveJunction) {
        Check ((Test-Path -LiteralPath (Join-Path $firstFolder 'PushFPS.exe')) -and ($refused.Code -eq 1)) ($firstCopy.Slot + ': a folder that holds a junction is not removed')
        Check ((Get-StandInState) -eq $standInBefore) 'the junction''s target is untouched'
        & cmd.exe /d /c rmdir "$junction" | Out-Null
        Check (-not (Test-Path -LiteralPath $junction)) 'junction removed again by the test'
    }
    if ($several) {
        Check ((Test-Path -LiteralPath (Join-Path $lastFolder 'PushFPS.exe')) -and ($refused.Code -eq 1)) ($lastCopy.Slot + ': a folder without the marker is not removed')
        foreach ($slot in $installed) {
            if (($slot.Slot -ne $firstCopy.Slot) -and ($slot.Slot -ne $lastCopy.Slot)) { Check (-not (Test-Path -LiteralPath (Join-Path $base $slot.Folder))) ($slot.Slot + ': removed') }
        }
    } elseif (Test-Path -LiteralPath (Join-Path $firstFolder 'Slipstream-INSTALL.txt')) {
        # Only one copy: it plays both parts, one after the other.
        Remove-Item -LiteralPath (Join-Path $firstFolder 'Slipstream-INSTALL.txt') -Force
    }
    $again = Invoke-PackageCmd 'Uninstall-Slipstream.cmd' ($baseArgument + ' -Yes')
    Check ($again.Code -eq 1) 'uninstall says that a folder was left (exit code 1)'
    Check (Test-Path -LiteralPath (Join-Path $lastFolder 'PushFPS.exe')) ($lastCopy.Slot + ': still kept without the marker')
    if ($several) { Check (-not (Test-Path -LiteralPath $firstFolder)) ($firstCopy.Slot + ': removed once the junction is gone') }
    $final = Invoke-PackageCmd 'Uninstall-Slipstream.cmd' ($baseArgument + ' -Yes -IncludeUnmarked')
    Check ($final.Code -eq 0) 'uninstall with -IncludeUnmarked: exit code 0'
    Check (-not (Test-Path -LiteralPath $lastFolder)) ($lastCopy.Slot + ': removed with -IncludeUnmarked')
} else {
    $nothing = Invoke-PackageCmd 'Uninstall-Slipstream.cmd' ($baseArgument + ' -Yes')
    Check ($nothing.Code -eq 0) 'uninstall with nothing installed: exit code 0'
}

# --- 5. what must be exactly as before ----------------------------------------------------------
Section 'Stand-ins'
Check ((Get-StandInState) -eq $standInBefore) 'official stand-in folder, its settings and the PushFPS shortcuts are exactly as before'
$left = @(Get-ChildItem -LiteralPath $base | ForEach-Object { $_.Name })
Check (($left.Count -eq 1) -and ($left[0] -eq 'PushFPS')) ('only the official stand-in is left under the install base (' + ($left -join ', ') + ')')
foreach ($link in $madeShortcuts) { Remove-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue }

Section 'Result'
foreach ($warning in $warnings) { Say ('note  ' + $warning); Write-Host ('::warning::slipstream package: ' + $warning) }
if ($problems.Count -gt 0) {
    foreach ($problem in $problems) { Write-Host ('::error::slipstream package: ' + $problem) }
    Say ($problems.Count.ToString() + ' problem(s). The package was uploaded, but its scripts must not be trusted until this is green.')
    exit 1
}
Say ('all checks passed for: ' + (($installed | ForEach-Object { $_.Slot }) -join ', ') + ' (GitHub Windows runner, Windows PowerShell ' + $PSVersionTable.PSVersion.ToString() + '; not a real PC)')
exit 0
