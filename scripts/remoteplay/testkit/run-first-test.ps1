<#
.SYNOPSIS
  The first measuring session of a Slipstream test package: checks that the copies are installed and
  starts the kit's quick procedure (run-tests.ps1 -Quick). Run-First-Test.cmd in the package runs this.

.DESCRIPTION
  The quick procedure is step 1 of docs\HARDWARE_TEST_PROCEDURE.md: three arms (the original build, the
  modified build with the original timing, the modified build as installed), three rounds, frame
  generation OFF, about 40 minutes with you at the PC. It ends with one zip to send back.

  This script changes nothing on the PC. It only decides which two programs the kit compares:
      original   %LOCALAPPDATA%\Programs\PushFPS-original\PushFPS.exe
      modified   %LOCALAPPDATA%\Programs\PushFPS-slipstream\PushFPS.exe          (-Build stable, default)
                 %LOCALAPPDATA%\Programs\PushFPS-slipstream-exp\PushFPS.exe      (-Build experimental)
  The kit gives every run its own switches and ignores slipstream.ini, so the settings file of a copy
  does not change what is measured.

  Windows PowerShell 5.1 or newer. Unverified on a real PC: if something fails, send the text of this window.

.PARAMETER Build
  stable (default) or experimental: which modified copy is compared with the original.
.PARAMETER InstallBase
  Folder the copies are under. Default: %LOCALAPPDATA%\Programs.
.PARAMETER PackageRoot
  The unzipped package; a PresentMon console program placed there is used. Default: not searched.
.PARAMETER CheckOnly
  Only check and print what would be started.
#>
param(
    [ValidateSet('stable', 'experimental')][string]$Build = 'stable',
    [string]$InstallBase = '',
    [string]$PackageRoot = '',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$scriptFolder = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptFolder 'slipstream-common.ps1')

$base = Get-SlipstreamInstallBase $InstallBase
$kit = Join-Path $scriptFolder 'run-tests.ps1'
$originalBuild = @($SlipstreamBuilds | Where-Object { $_.Slot -eq 'original' })[0]
$modifiedBuild = @($SlipstreamBuilds | Where-Object { $_.Slot -eq $Build })[0]
$originalExe = Join-Path (Join-Path $base $originalBuild.Folder) 'PushFPS.exe'
$modifiedExe = Join-Path (Join-Path $base $modifiedBuild.Folder) 'PushFPS.exe'

Write-Step 'Slipstream test package: first test'
$missing = @()
if (-not (Test-Path -LiteralPath $kit)) { $missing += ('the test kit (run-tests.ps1) next to this script: ' + $scriptFolder) }
if (-not (Test-Path -LiteralPath $originalExe)) { $missing += ('the original build: ' + $originalExe) }
if (-not (Test-Path -LiteralPath $modifiedExe)) { $missing += ('the ' + $modifiedBuild.Title + ': ' + $modifiedExe) }
if ($missing.Count -gt 0) {
    Write-Host '   Not found:' -ForegroundColor Red
    foreach ($item in $missing) { Write-Host ('     ' + $item) -ForegroundColor Red }
    Write-Line 'Run Install-Slipstream.cmd first. If you started this window as another (administrator) account,'
    Write-Line 'the copies are in your own account: start it from your own account instead.'
    exit 1
}
Write-Line ('original : ' + $originalExe)
Write-Line ('modified : ' + $modifiedExe + '  (' + $modifiedBuild.Title + ')')

$arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $kit, '-Quick', '-OriginalExe', $originalExe, '-ModifiedExe', $modifiedExe)
if ($PackageRoot) {
    $root = Get-SlipstreamFullPath $PackageRoot
    $presentMon = @(Get-ChildItem -LiteralPath $root -Filter 'PresentMon*.exe' -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch 'Service' } | Select-Object -First 1)
    if ($presentMon.Count -gt 0) {
        $arguments += @('-PresentMon', $presentMon[0].FullName)
        Write-Line ('PresentMon: ' + $presentMon[0].FullName)
    }
}

Write-Host ''
Write-Line 'Before you go on (docs\HARDWARE_TEST_PROCEDURE.md, step 0):'
Write-Line '  - both copies paired with the PS5, same stream quality, codec and bitrate, frame generation OFF'
Write-Line '  - PC and PS5 on cable, the screen at its highest refresh rate, nothing heavy running'
Write-Line '  - the game loaded, standing in a calm place you can return to'
Write-Line 'In each of the nine starts: connect, run the 20-press latency test twice, close PushFPS, press Enter here.'
if ($CheckOnly) {
    Write-Line 'Check only: nothing started.'
    exit 0
}

$powershellExe = 'powershell.exe'
if ($env:SystemRoot) {
    $candidate = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path -LiteralPath $candidate) { $powershellExe = $candidate }
}
& $powershellExe @arguments
exit $LASTEXITCODE
