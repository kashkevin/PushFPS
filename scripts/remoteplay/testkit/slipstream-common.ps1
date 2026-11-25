# Shared by install-slipstream.ps1, uninstall-slipstream.ps1 and run-first-test.ps1 (dot-sourced).
# Windows PowerShell 5.1 or newer. ASCII only. Defines names and helpers; does nothing by itself.

# The copies of a Slipstream test package (four since Package 8). Folder: the name under the install base, which is
# %LOCALAPPDATA%\Programs unless -InstallBase says otherwise. These names are fixed on purpose: the
# official PushFPS lives in ...\Programs\PushFPS, which none of these scripts ever names or touches.
$SlipstreamBuilds = @(
    [pscustomobject]@{ Slot = 'original';     Folder = 'PushFPS-original';       Title = 'original build (PushFPS 3.2.1 as published)' },
    [pscustomobject]@{ Slot = 'stable';       Folder = 'PushFPS-slipstream';     Title = 'stable candidate' },
    [pscustomobject]@{ Slot = 'experimental'; Folder = 'PushFPS-slipstream-exp'; Title = 'experimental build' },
    # Package 8: Package 7's own experimental build, unchanged, as a fourth copy: the rollback copy, and what the kit's
    # regression session compares with (run-tests.ps1 -Session builds, arms.csv exe 'previous').
    [pscustomobject]@{ Slot = 'previous';     Folder = 'PushFPS-slipstream-p7';  Title = 'experimental build of Package 7 (rollback copy)' }
)
# The folder of the fourth copy under the install base (run-first-test.ps1 -Session builds).
$SlipstreamPreviousFolder = 'PushFPS-slipstream-p7'
$SlipstreamMarker = 'Slipstream-INSTALL.txt'
$SlipstreamSettingsFile = 'slipstream.ini'
# First line of PushFPS-VERSION.txt in a test copy (see Set-SlipstreamUpdatePin).
$SlipstreamPinLine = 'PushFPS 999.0.0'

function Write-Step([string]$Text) {
    Write-Host ''
    Write-Host ('== ' + $Text) -ForegroundColor Cyan
}

function Write-Line([string]$Text) {
    Write-Host ('   ' + $Text)
}

# A full path without a trailing backslash. A relative path is taken from PowerShell's current folder
# (which is not the process's current directory that .NET and child processes use).
function Get-SlipstreamFullPath([string]$Path) {
    $full = $Path
    if (-not [System.IO.Path]::IsPathRooted($full)) { $full = Join-Path (Get-Location).Path $full }
    $full = [System.IO.Path]::GetFullPath($full)
    if ($full.Length -gt 3) { $full = $full.TrimEnd('\') }
    return $full
}

function Get-SlipstreamInstallBase([string]$InstallBase) {
    if ($InstallBase) { return (Get-SlipstreamFullPath $InstallBase) }
    if (-not $env:LOCALAPPDATA) { throw 'LOCALAPPDATA is not set; pass -InstallBase <folder>.' }
    return (Join-Path $env:LOCALAPPDATA 'Programs')
}

# -Only stable,experimental arrives as one string when the script is started with -File.
function Select-SlipstreamBuilds([string[]]$Only) {
    $names = @()
    foreach ($item in $Only) {
        foreach ($part in ([string]$item).Split(',')) {
            $name = $part.Trim().ToLowerInvariant()
            if ($name) { $names += $name }
        }
    }
    if ($names.Count -eq 0) { return $SlipstreamBuilds }
    $known = @($SlipstreamBuilds | ForEach-Object { $_.Slot })
    foreach ($name in $names) {
        if ($known -notcontains $name) { throw ("Unknown build '" + $name + "'. Use: " + ($known -join ', ')) }
    }
    return @($SlipstreamBuilds | Where-Object { $names -contains $_.Slot })
}

# SHA-256 through .NET: needs no PowerShell module (Windows PowerShell started from PowerShell 7 cannot
# load its own; the PushFPS installer hit exactly that).
function Get-FileSha256([string]$Path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try { return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}

# PushFPS processes started from this folder (empty when none).
function Get-SlipstreamRunning([string]$Folder) {
    $prefix = $Folder.TrimEnd('\') + '\'
    $found = @()
    foreach ($process in @(Get-Process -Name 'PushFPS' -ErrorAction SilentlyContinue)) {
        $path = ''
        try { $path = [string]$process.Path } catch { $path = '' }
        if ($path -and $path.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) { $found += $process }
    }
    return $found
}

# key: value lines of the marker file a test copy carries; empty table when there is none.
function Read-SlipstreamMarker([string]$Folder) {
    $table = @{}
    $file = Join-Path $Folder $SlipstreamMarker
    if (-not (Test-Path -LiteralPath $file)) { return $table }
    foreach ($line in @(Get-Content -LiteralPath $file)) {
        $at = $line.IndexOf(':')
        if ($at -gt 0) { $table[$line.Substring(0, $at).Trim()] = $line.Substring($at + 1).Trim() }
    }
    return $table
}

# The in-app updater of a development build offers the official 3.x release and can install it over
# the copy. The program takes its installed version from the build or, when that is higher, from the
# "PushFPS X.Y.Z" line of PushFPS-VERSION.txt (src/ui/UpdateChecker.cpp, localVersion). A first line that
# names a version no release reaches makes every copy, including the original build that has no other
# guard, answer "already the latest": no banner, no install. Returns what was done.
function Set-SlipstreamUpdatePin([string]$Folder) {
    $record = Join-Path $Folder 'PushFPS-VERSION.txt'
    if (-not (Test-Path -LiteralPath $record)) { return 'not set: PushFPS-VERSION.txt is missing' }
    $lines = @(Get-Content -LiteralPath $record)
    if (($lines.Count -gt 0) -and ($lines[0] -eq $SlipstreamPinLine)) { return 'already set' }
    $text = New-Object System.Collections.Generic.List[string]
    $text.Add($SlipstreamPinLine)
    $text.Add('Slipstream test copy: the first line is not a version. It keeps the in-app updater of this copy from offering or installing the official release (docs\SETUP.md). The real version is in ' + $SlipstreamMarker + ' and in Settings > About.')
    foreach ($line in $lines) { $text.Add([string]$line) }
    [System.IO.File]::WriteAllLines($record, $text.ToArray(), (New-Object System.Text.UTF8Encoding $false))
    return 'set'
}
