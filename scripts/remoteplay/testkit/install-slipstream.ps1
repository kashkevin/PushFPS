<#
.SYNOPSIS
  Installs the builds of a Slipstream test package side by side. Install-Slipstream.cmd in the package
  runs this; it can be run again at any time.

.DESCRIPTION
  For each build folder the package holds (original, stable, experimental) this script runs that
  build's own installer (Install-PushFPS.ps1 from the build workflow) with -NoShortcuts -NoLaunch into

      %LOCALAPPDATA%\Programs\PushFPS-original        the original build
      %LOCALAPPDATA%\Programs\PushFPS-slipstream      the stable candidate
      %LOCALAPPDATA%\Programs\PushFPS-slipstream-exp  the experimental build

  and then, in each copy:
    - puts a first line into PushFPS-VERSION.txt that keeps the in-app updater from offering or
      installing the official release over the test copy (slipstream-common.ps1, Set-SlipstreamUpdatePin);
    - writes Slipstream-INSTALL.txt (which build this is; the uninstall script only removes folders
      that carry it).
  In the stable candidate only, when there is no slipstream.ini yet, it writes one that selects the
  Ultra Low Latency mode. An existing file is never changed.

  A build that is missing from the package is reported and skipped. A copy that already holds exactly
  this build is left alone (use -Force to install it again). Settings, logs and the PS5 pairing of a
  copy survive a reinstall: the installer only replaces program files.

  WHAT THIS SCRIPT NEVER TOUCHES
    %LOCALAPPDATA%\Programs\PushFPS (an official PushFPS install), the Start menu and desktop shortcuts,
    %LOCALAPPDATA%\Veyra (PS5 pairing and other sign-ins), any Windows, driver or PS5 setting. It needs
    no administrator rights. The installer downloads the runtime package (about 750 MB) from the Veyra
    project's GitHub release once and reuses it for the other copies.

  Windows PowerShell 5.1 or newer. Unverified on a real PC: if something fails, send the text of this window.

.PARAMETER PackageRoot
  The unzipped package (the folder that holds START-HERE.txt). Default: found from this script's place.
.PARAMETER InstallBase
  Folder the three copies go under. Default: %LOCALAPPDATA%\Programs.
.PARAMETER Gpu
  Auto (default), NVIDIA or AMD: passed to each installer, so that every copy gets the same runtime.
.PARAMETER ExperimentalExtras
  No (default) or Yes: passed to each installer (see its own notes on NVIDIA files).
.PARAMETER Only
  Install only these builds: original, stable, experimental.
.PARAMETER Force
  Install again even when the copy already holds this build.
.PARAMETER NoSettingsFile
  Do not write slipstream.ini into the stable candidate.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install-slipstream.ps1
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File install-slipstream.ps1 -Only stable -Force
#>
param(
    [string]$PackageRoot = '',
    [string]$InstallBase = '',
    [ValidateSet('Auto', 'NVIDIA', 'AMD')][string]$Gpu = 'Auto',
    [ValidateSet('Yes', 'No')][string]$ExperimentalExtras = 'No',
    [string[]]$Only = @(),
    [switch]$Force,
    [switch]$NoSettingsFile
)
$ErrorActionPreference = 'Stop'
$scriptFolder = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptFolder 'slipstream-common.ps1')

# The package root: given, or the first folder above this script that holds a build's installer.
function Find-PackageRoot([string]$Given) {
    if ($Given) {
        $full = Get-SlipstreamFullPath $Given
        if (-not (Test-Path -LiteralPath $full)) { throw ('Package folder not found: ' + $full) }
        return $full
    }
    $folder = $scriptFolder
    for ($level = 0; $level -lt 6; $level++) {
        if (-not $folder) { break }
        foreach ($build in $SlipstreamBuilds) {
            if (Test-Path -LiteralPath (Join-Path (Join-Path $folder $build.Slot) 'Install-PushFPS.ps1')) { return $folder.TrimEnd('\') }
        }
        if (Test-Path -LiteralPath (Join-Path $folder 'START-HERE.txt')) { return $folder.TrimEnd('\') }
        $folder = Split-Path -Parent $folder
    }
    throw 'The package folder was not found. Extract the whole zip, then start Install-Slipstream.cmd from the extracted folder (or pass -PackageRoot <folder>).'
}

# SHA256SUMS.txt of the package: relative path (with \) -> hash. Empty when the file is not there.
function Read-PackageSums([string]$Root) {
    $table = @{}
    $file = Join-Path $Root 'SHA256SUMS.txt'
    if (-not (Test-Path -LiteralPath $file)) { return $table }
    foreach ($line in @(Get-Content -LiteralPath $file)) {
        if ($line -match '^([0-9a-fA-F]{64})\s+\*?(.+?)\s*$') { $table[$Matches[2].Replace('/', '\')] = $Matches[1].ToLowerInvariant() }
    }
    return $table
}

# Every file of one build folder against SHA256SUMS.txt. Returns the problems found (empty = good).
function Test-PackageFiles([string]$Root, [string]$Slot, $Sums) {
    $problems = @()
    $prefix = $Slot + '\'
    $names = @($Sums.Keys | Where-Object { $_.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase) })
    if ($names.Count -eq 0) { return @('SHA256SUMS.txt lists no file of this build') }
    foreach ($name in $names) {
        $path = Join-Path $Root $name
        if (-not (Test-Path -LiteralPath $path)) { $problems += ('missing: ' + $name); continue }
        if ((Get-FileSha256 $path) -ne $Sums[$name]) { $problems += ('changed: ' + $name) }
        if ($problems.Count -ge 5) { break }
    }
    return $problems
}

# The version the build workflow filled into the installer ($PushFpsVersion = '...').
function Get-PackagedVersion([string]$Installer) {
    foreach ($line in @(Get-Content -LiteralPath $Installer -TotalCount 80)) {
        if ($line -match '^\s*\$PushFpsVersion\s*=\s*''([^'']*)''') { return $Matches[1] }
    }
    return 'unknown'
}

function Write-Marker([string]$Folder, $Build, [string]$Version, [string]$ExeHash, [string]$PackageName, [string[]]$Source) {
    $text = New-Object System.Collections.Generic.List[string]
    $text.Add('Slipstream test copy (see docs\SETUP.md in the package). Remove it with Uninstall-Slipstream.cmd.')
    $text.Add('slot: ' + $Build.Slot)
    $text.Add('build: ' + $Build.Title)
    $text.Add('version: ' + $Version)
    $text.Add('exe-sha256: ' + $ExeHash)
    $text.Add('package: ' + $PackageName)
    $text.Add('installed: ' + (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ'))
    foreach ($line in $Source) { if ($line) { $text.Add('source: ' + $line) } }
    [System.IO.File]::WriteAllLines((Join-Path $Folder $SlipstreamMarker), $text.ToArray(), (New-Object System.Text.UTF8Encoding $false))
}

# slipstream.ini for the stable candidate: only when there is none, and not after a rollback switched
# one off (a file named slipstream.ini.off-...). Returns what was done.
function Write-SettingsFile([string]$Folder) {
    $file = Join-Path $Folder $SlipstreamSettingsFile
    if (Test-Path -LiteralPath $file) { return 'kept the existing slipstream.ini' }
    $off = @(Get-ChildItem -LiteralPath $Folder -Filter ($SlipstreamSettingsFile + '.off*') -ErrorAction SilentlyContinue)
    if ($off.Count -gt 0) { return ('not written: a rollback switched the settings file off (' + $off[0].Name + '); rename it to slipstream.ini to use it again') }
    $text = @(
        '# Written by Install-Slipstream. Read once when this copy of PushFPS starts.',
        '# Delete or rename this file and the build behaves as a build without it (docs\SETUP.md).',
        '# Frame generation is not set here: keep it OFF in the interface for the first comparison.',
        'mode = ultra-low-latency',
        'lan = 0',
        '# Keeps the in-app update check off in builds that know this switch.',
        'VEYRA_EXP_RP_NO_UPDATE = 1'
    )
    [System.IO.File]::WriteAllLines($file, [string[]]$text, (New-Object System.Text.UTF8Encoding $false))
    return 'wrote slipstream.ini (mode = ultra-low-latency)'
}

$root = Find-PackageRoot $PackageRoot
$base = Get-SlipstreamInstallBase $InstallBase
$builds = @(Select-SlipstreamBuilds $Only)
$sums = Read-PackageSums $root
$packageName = Split-Path -Leaf $root
$powershellExe = 'powershell.exe'
if ($env:SystemRoot) {
    $candidate = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path -LiteralPath $candidate) { $powershellExe = $candidate }
}

Write-Step 'Slipstream test package: install'
Write-Line ('package : ' + $root)
Write-Line ('install : ' + $base)
if ($sums.Count -eq 0) { Write-Host '   SHA256SUMS.txt is not in the package folder: the files cannot be checked before installing.' -ForegroundColor Yellow }

$results = @()
foreach ($build in $builds) {
    $source = Join-Path $root $build.Slot
    $installer = Join-Path $source 'Install-PushFPS.ps1'
    $packagedExe = Join-Path (Join-Path $source 'PushFPS-files') 'PushFPS.exe'
    $target = Join-Path $base $build.Folder
    $result = [pscustomobject]@{ Slot = $build.Slot; Folder = $target; State = ''; Version = ''; Notes = @() }
    Write-Step ($build.Slot + ': ' + $build.Title)

    if (-not ((Test-Path -LiteralPath $installer) -and (Test-Path -LiteralPath $packagedExe))) {
        $result.State = 'not in this package'
        Write-Line 'This build is not in the package (see START-HERE.txt). Skipped.'
        $results += $result
        continue
    }
    $result.Version = Get-PackagedVersion $installer

    if ($sums.Count -gt 0) {
        Write-Line 'checking the package files (SHA-256)'
        $problems = @(Test-PackageFiles $root $build.Slot $sums)
        if ($problems.Count -gt 0) {
            $result.State = 'FAILED: package files damaged'
            foreach ($problem in $problems) { Write-Host ('   ' + $problem) -ForegroundColor Red }
            Write-Host '   Extract the zip again (right-click, Extract All) and start Install-Slipstream.cmd from the new folder.' -ForegroundColor Red
            $results += $result
            continue
        }
    }

    $running = @(Get-SlipstreamRunning $target)
    if ($running.Count -gt 0) {
        $result.State = 'FAILED: this copy is running'
        Write-Host '   PushFPS from this folder is running. Close it and start Install-Slipstream.cmd again.' -ForegroundColor Red
        $results += $result
        continue
    }

    $exeHash = Get-FileSha256 $packagedExe
    $installedExe = Join-Path $target 'PushFPS.exe'
    $marker = Read-SlipstreamMarker $target
    $same = (Test-Path -LiteralPath $installedExe) -and ($marker['exe-sha256'] -eq $exeHash) -and ((Get-FileSha256 $installedExe) -eq $exeHash)
    if ($same -and (-not $Force)) {
        $result.State = 'already installed'
        Write-Line ('Already holds this build (' + $result.Version + '). Nothing copied.')
    } else {
        Write-Line ('running the build''s own installer into ' + $target)
        $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $installer, '-Gpu', $Gpu, '-InstallDir', $target,
                       '-NoShortcuts', '-NoLaunch', '-ExperimentalExtras', $ExperimentalExtras)
        & $powershellExe @arguments
        $code = $LASTEXITCODE
        if (($code -ne 0) -or (-not (Test-Path -LiteralPath $installedExe))) {
            $result.State = 'FAILED: installer exit code ' + $code
            Write-Host ('   The installer did not finish (exit code ' + $code + '). Nothing else was changed for this build.') -ForegroundColor Red
            $results += $result
            continue
        }
        $result.State = 'installed'
    }

    # After every run, also for a copy that was already there: these files are small and idempotent.
    $sourceLines = @()
    $sourceFile = Join-Path $source 'SOURCE.txt'
    if (Test-Path -LiteralPath $sourceFile) { $sourceLines = @(Get-Content -LiteralPath $sourceFile -TotalCount 12) }
    Write-Marker $target $build $result.Version $exeHash $packageName $sourceLines
    $pin = Set-SlipstreamUpdatePin $target
    $result.Notes += ('update guard ' + $pin)
    Write-Line ('in-app updater guard (PushFPS-VERSION.txt): ' + $pin)
    if (($build.Slot -eq 'stable') -and (-not $NoSettingsFile)) {
        $settings = Write-SettingsFile $target
        $result.Notes += $settings
        Write-Line $settings
    }
    $results += $result
}

Write-Step 'What was done'
$failed = 0
foreach ($result in $results) {
    $colour = 'Gray'
    if ($result.State -like 'FAILED*') { $colour = 'Red'; $failed++ }
    elseif (($result.State -eq 'installed') -or ($result.State -eq 'already installed')) { $colour = 'Green' }
    Write-Host ('   {0,-13} {1,-30} {2}' -f $result.Slot, $result.State, $result.Version) -ForegroundColor $colour
    if (($result.State -eq 'installed') -or ($result.State -eq 'already installed')) { Write-Line ('              ' + (Join-Path $result.Folder 'PushFPS.exe')) }
    foreach ($note in $result.Notes) { Write-Line ('              ' + $note) }
}
Write-Host ''
Write-Line 'Not touched: an official PushFPS install, its shortcuts, your PS5 pairing, any Windows setting.'
Write-Line 'Next: START-HERE.txt in the package. Remove everything again with Uninstall-Slipstream.cmd.'
if ($failed -gt 0) {
    Write-Host ('   ' + $failed + ' build(s) could not be installed; the others are usable.') -ForegroundColor Red
    exit 1
}
exit 0
