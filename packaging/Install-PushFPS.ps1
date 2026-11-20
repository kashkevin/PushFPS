<#
  PushFPS installer.

  PushFPS runs on Veyra's official runtime package (Qt, FFmpeg and the NVIDIA / AMD / Intel components that
  Veyra's author distributes). This script downloads that package from Veyra's own GitHub release, checks its
  SHA-256 against the value pinned when this PushFPS release was built, unpacks it, and adds PushFPS on top.
  No administrator rights are needed: everything goes into your user folder.

  Usage (normally just double-click Install-PushFPS.cmd):
    Install-PushFPS.ps1 [-Gpu Auto|NVIDIA|AMD] [-InstallDir <folder>] [-NoShortcuts] [-NoLaunch]
                        [-ExperimentalExtras Ask|Yes|No] [-NeuralRuntime Ask|Yes|No]
    Install-PushFPS.ps1 -Uninstall [-InstallDir <folder>]

  Works with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [ValidateSet('Auto', 'NVIDIA', 'AMD')]
    [string]$Gpu = 'Auto',
    [string]$InstallDir = (Join-Path $env:LOCALAPPDATA 'Programs\PushFPS'),
    [switch]$NoShortcuts,
    [switch]$NoLaunch,
    # Veyra's experimental extras whose NVIDIA files fall outside NVIDIA's licence (see below).
    # Ask (default): a question when run in a console window; No when there is no one to ask.
    [ValidateSet('Ask', 'Yes', 'No')]
    [string]$ExperimentalExtras = 'Ask',
    # NVIDIA's own signed neural-rendering runtime that Veyra's NVIDIA package carries (experimental, optional;
    # see below). It is never kept without an explicit yes. Ask (default): a question when run in a console
    # window; No when there is no one to ask.
    [ValidateSet('Ask', 'Yes', 'No')]
    [string]$NeuralRuntime = 'Ask',
    [switch]$Uninstall,
    # Signed in-app updates (src/ui/UpdateChecker.cpp): the running PushFPS that started this
    # installer; it quits right after, and the copy waits until it has (at most a minute).
    [int]$WaitForPid = 0
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

# Windows PowerShell that was started from PowerShell 7 (PushFPS launched from a PowerShell 7 window, or
# by a tool running under it, then updating itself) inherits PowerShell 7's module path and cannot load
# its own modules: "Get-FileHash is not recognized", and the update stopped there with its window
# closing at once (found by the update test in CI, November 2026). Use Windows PowerShell's own paths.
if ($PSVersionTable.PSVersion.Major -le 5 -and $env:PSModulePath -match '\\PowerShell\\') {
    $env:PSModulePath = @(
        (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'WindowsPowerShell\Modules'),
        (Join-Path $env:ProgramFiles 'WindowsPowerShell\Modules'),
        (Join-Path $PSHOME 'Modules')) -join ';'
}

# Filled in by packaging/make-release.ps1 when the release is built.
$PushFpsVersion = '@PUSHFPS_VERSION@'
$UpstreamRepo = '@UPSTREAM_REPO@'
$UpstreamTag = '@UPSTREAM_TAG@'
$UpstreamSha256 = @{
    'NVIDIA' = '@SHA256_NVIDIA@'
    'AMD'    = '@SHA256_AMD@'
}

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$filesDir = Join-Path $here 'PushFPS-files'
$startMenu = Join-Path ([Environment]::GetFolderPath('Programs')) 'PushFPS.lnk'
$desktop = Join-Path ([Environment]::GetFolderPath('Desktop')) 'PushFPS.lnk'

function Step([string]$text) { Write-Host ''; Write-Host "  $text" -ForegroundColor Green }
function Info([string]$text) { Write-Host "    $text" }
function Fail([string]$text) {
    Write-Host ''; Write-Host "  $text" -ForegroundColor Red
    if ($WaitForPid -gt 0) {
        # An update started inside PushFPS: the program has closed by now. Say what happened, start the
        # copy that is still installed and keep this window open so the reason can be read.
        Write-Host '  The update did not finish. You can run it again from PushFPS, or download the release by hand.' -ForegroundColor Yellow
        try {
            $existing = Join-Path $InstallDir 'PushFPS.exe'
            if ((Test-Path $existing) -and -not (Get-Process -Name 'PushFPS' -ErrorAction SilentlyContinue)) { Start-Process -FilePath $existing -WorkingDirectory $InstallDir }
        } catch { }
        if (-not [Console]::IsInputRedirected) { Read-Host '  Press Enter to close this window' | Out-Null }
    }
    exit 1
}

# Anything unexpected ends through Fail too: in an update the window stays open with the reason and the
# installed copy starts again, where it used to close at once with PushFPS already gone.
trap { Fail ("Unexpected error: " + $_.Exception.Message) }

Write-Host ''
Write-Host "  PushFPS $PushFpsVersion" -ForegroundColor Green
Write-Host '  Based on Veyra by Likely7 (GPL-3.0)'

if ($Uninstall) {
    Step 'Removing PushFPS'
    foreach ($link in @($startMenu, $desktop)) { if (Test-Path $link) { Remove-Item $link -Force } }
    if (Test-Path (Join-Path $InstallDir 'PushFPS.exe')) {
        Remove-Item $InstallDir -Recurse -Force
        Info "Removed $InstallDir"
    } else {
        Info "Nothing installed in $InstallDir"
    }
    exit 0
}

if (-not (Test-Path (Join-Path $filesDir 'PushFPS.exe'))) {
    Fail 'PushFPS-files\PushFPS.exe is missing. Extract the whole zip first, then run the installer from that folder.'
}

if ([Environment]::OSVersion.Version.Major -lt 10 -or -not [Environment]::Is64BitOperatingSystem) {
    Fail 'PushFPS needs 64-bit Windows 10 or 11 (Windows 11 recommended).'
}

# Pick the runtime package that matches the graphics card.
if ($Gpu -eq 'Auto') {
    $names = @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object { $_.Name })
    if ($names -match 'NVIDIA') { $Gpu = 'NVIDIA' } else { $Gpu = 'AMD' }
    Step "Graphics card: $(($names | Where-Object { $_ }) -join ', ')"
    if ($Gpu -eq 'AMD') { Info 'No NVIDIA card found: using the AMD / Intel package (FSR and XeSS frame generation).' }
}

$version = $UpstreamTag.TrimStart('v')
$asset = "Veyra-$version-$Gpu-win64-portable.zip"
$expected = $UpstreamSha256[$Gpu]
$url = "https://github.com/$UpstreamRepo/releases/download/$UpstreamTag/$asset"
$cacheDir = Join-Path $env:TEMP 'PushFPS-download'
$zip = Join-Path $cacheDir $asset
New-Item -ItemType Directory -Force $cacheDir | Out-Null

# SHA-256 through .NET: no PowerShell module is needed for it (see the module path note at the top).
function Get-Sha256([string]$path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($path)
    try { return ([System.BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '') }
    finally { $stream.Dispose(); $sha.Dispose() }
}
function Test-Download { (Test-Path $zip) -and ((Get-Sha256 $zip) -ieq $expected) }

if (Test-Download) {
    Step "Using the runtime package downloaded earlier"
} else {
    Step "Downloading the runtime package ($Gpu, about 750 MB) from Veyra's GitHub release"
    Info $url
    $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
    if (Test-Path $curl) {
        & $curl -L --fail --retry 3 --retry-delay 5 -C - -o $zip $url
        if ($LASTEXITCODE -ne 0) { Fail "Download failed (curl exit $LASTEXITCODE). Check your internet connection and run the installer again; it continues where it stopped." }
    } else {
        Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    }
    if (-not (Test-Download)) {
        Remove-Item $zip -Force -ErrorAction SilentlyContinue
        Fail 'The downloaded file does not match the expected SHA-256, so it was deleted. Run the installer again.'
    }
}
Info "SHA-256 verified: $expected"

Step 'Unpacking'
$stage = Join-Path $env:TEMP ("PushFPS-stage-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force $stage | Out-Null
try {
    $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    if (Test-Path $tar) {
        & $tar -xf $zip -C $stage
        if ($LASTEXITCODE -ne 0) { Fail "Unpacking failed (tar exit $LASTEXITCODE)." }
    } else {
        Expand-Archive -Path $zip -DestinationPath $stage -Force
    }
    $exe = Get-ChildItem $stage -Recurse -Filter 'veyra_qml_ui.exe' | Select-Object -First 1
    if (-not $exe) { Fail 'The runtime package has an unexpected layout (veyra_qml_ui.exe not found).' }
    $runtimeRoot = $exe.DirectoryName

    # Licensing. By default PushFPS installs only what NVIDIA's licence allows to be passed on:
    #  - NVIDIA's runtime files only as NVIDIA ships them. A file that says it is from NVIDIA but is not
    #    an original with a valid NVIDIA signature (a community-modified DLSS build: Veyra's RTX 30 AI
    #    enhancement slot) is an "experimental extra".
    #  - NVIDIA's DLSS runtimes are licensed for systems with NVIDIA graphics only. The AMD package's AI
    #    enhancement folders run NVIDIA's model on Radeon cards: also experimental extras.
    # Veyra ships the extras in its own package. Nothing is lost: they are offered here, off by default,
    # and installed only if the user says yes (or passes -ExperimentalExtras Yes). Left out, they are also
    # removed if an earlier install placed them, and Veyra reports their feature as unavailable.
    $extras = @()
    if ($Gpu -eq 'AMD') {
        foreach ($rel in 'runtime\amd-nr-rdna3', 'runtime\amd-nr') {
            if (Test-Path (Join-Path $runtimeRoot $rel)) { $extras += [pscustomobject]@{ Rel = $rel; Dir = $true; Why = 'AI enhancement on Radeon (runs NVIDIA''s model on non-NVIDIA graphics)' } }
        }
    }
    foreach ($f in Get-ChildItem $runtimeRoot -Recurse -File -Include *.dll, *.exe) {
        if ($f.VersionInfo.CompanyName -notmatch 'NVIDIA') { continue }
        $rel = $f.FullName.Substring($runtimeRoot.Length).TrimStart('\')
        if ($extras | Where-Object { $_.Dir -and $rel.StartsWith($_.Rel + '\', [StringComparison]::OrdinalIgnoreCase) }) { continue }
        $sig = Get-AuthenticodeSignature -FilePath $f.FullName
        $signer = if ($sig.SignerCertificate) { $sig.SignerCertificate.Subject } else { '' }
        if ($sig.Status -eq 'Valid' -and $signer -match 'NVIDIA') { continue }
        $why = if ($rel -match 'nr-ampere') { 'AI enhancement on RTX 30 (community-modified NVIDIA file)' } else { 'community-modified NVIDIA file' }
        $extras += [pscustomobject]@{ Rel = $rel; Dir = $false; Why = $why }
    }
    $keepExtras = $false
    if ($extras.Count) {
        if ($ExperimentalExtras -eq 'Yes') { $keepExtras = $true }
        elseif ($ExperimentalExtras -eq 'Ask' -and -not [Console]::IsInputRedirected) {
            Step 'Optional: Veyra''s experimental extras'
            foreach ($x in $extras) { Info "- $($x.Why)" }
            Info 'They use NVIDIA files outside the terms NVIDIA licenses them under, so PushFPS leaves them out'
            Info 'unless you choose them. Everything else works without them.'
            $answer = Read-Host '    Install the experimental extras too? [y/N]'
            $keepExtras = $answer -match '^(y|yes|j|ja)$'
        }
    }
    $skipped = @()
    foreach ($x in $extras) {
        if ($keepExtras) { Info "Installing experimental extra by your choice: $($x.Rel)"; continue }
        $src = Join-Path $runtimeRoot $x.Rel
        if ($x.Dir) { Remove-Item $src -Recurse -Force } else { Remove-Item $src -Force }
        Remove-Item (Join-Path $InstallDir $x.Rel) -Recurse -Force -ErrorAction SilentlyContinue
        $skipped += "$($x.Rel) ($($x.Why); install with -ExperimentalExtras Yes)"
    }
    foreach ($item in $skipped) { Info "Not installed: $item" }

    # The experimental neural layer's runtime ("AI picture enhancement"): NVIDIA's own signed nvngx_dlssnr.dll
    # in Veyra's NVIDIA package. NVIDIA has published no DLSS 5 SDK and no terms covering this file's use by
    # other programs, so it is never kept by default: only on an explicit yes (the question below, or
    # -NeuralRuntime Yes), and then only if it is the file this release has a record of (Authenticode signature
    # valid, signed by NVIDIA Corporation, SHA-256 as recorded). No, and Ask with no one to ask, remove it.
    # A separate question from the extras above, which stay as they are. PushFPS checks the installed file again
    # before it uses it; Settings > Components and licenses can add it (from a file you have) or remove it later.
    # docs/NEURAL_RENDERING_RUNTIME.md
    $neuralRel = 'runtime\experimental\nr-original'
    $neuralSha256 = 'E16BCF15E16E13F527491CDF7845B2FE6521A738D8F7C9C721866A8496E1FC8E'   # nvngx_dlssnr.dll 310.8.0.0
    $neuralSrc = Join-Path (Join-Path $runtimeRoot $neuralRel) 'nvngx_dlssnr.dll'
    $neuralInstalled = Join-Path $InstallDir $neuralRel
    if (Test-Path $neuralSrc) {
        $neuralChoice = $NeuralRuntime
        if ($neuralChoice -eq 'Ask') {
            if ([Console]::IsInputRedirected) { $neuralChoice = 'No' }
            else {
                Step 'Optional: NVIDIA''s neural-rendering runtime (experimental)'
                Info 'This is NVIDIA''s neural-rendering model, applied here to video ("AI picture enhancement").'
                Info 'It is experimental and needs an RTX 50 card with driver 616.64 or newer. The file is an'
                Info 'NVIDIA-signed original, but NVIDIA has published no terms covering its use by other'
                Info 'programs: your decision. Everything else works without it; Settings can add or remove it later.'
                $answer = Read-Host '    Install NVIDIA''s neural-rendering runtime? [y/N]'
                if ($answer -match '^(y|yes|j|ja)$') { $neuralChoice = 'Yes' } else { $neuralChoice = 'No' }
            }
        }
        $neuralProblem = ''
        if ($neuralChoice -eq 'Yes') {
            $neuralSig = Get-AuthenticodeSignature -FilePath $neuralSrc
            $neuralSigner = ''
            if ($neuralSig.SignerCertificate) {
                $neuralSigner = $neuralSig.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
            }
            if ($neuralSig.Status -ne 'Valid') { $neuralProblem = "its signature is $($neuralSig.Status)" }
            elseif ($neuralSigner -ne 'NVIDIA Corporation') { $neuralProblem = 'it is not signed by NVIDIA Corporation' }
            elseif ((Get-Sha256 $neuralSrc) -ine $neuralSha256) { $neuralProblem = 'it is not the file this release has a record of' }
        }
        if ($neuralChoice -eq 'Yes' -and -not $neuralProblem) {
            Info 'Installing NVIDIA''s neural-rendering runtime by your choice (NVIDIA-signed original, SHA-256 as recorded)'
        } else {
            Remove-Item (Join-Path $runtimeRoot $neuralRel) -Recurse -Force
            Remove-Item $neuralInstalled -Recurse -Force -ErrorAction SilentlyContinue
            if ($neuralProblem) { $neuralReason = "refused: $neuralProblem" }
            else { $neuralReason = 'optional: install with -NeuralRuntime Yes, or later in Settings > Components and licenses' }
            $neuralNote = "$neuralRel (NVIDIA's neural-rendering runtime, experimental; $neuralReason)"
            $skipped += $neuralNote
            Info "Not installed: $neuralNote"
        }
    }

    Step "Installing to $InstallDir"
    if ($WaitForPid -gt 0) {
        $old = Get-Process -Id $WaitForPid -ErrorAction SilentlyContinue
        if ($old) { Info 'Waiting for PushFPS to close'; $old.WaitForExit(60000) | Out-Null }
    }
    $running = Get-Process -Name 'PushFPS' -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) }
    if ($running) { Fail 'PushFPS is running. Close it, then run the installer again.' }
    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    # /E copies every folder; files already there are replaced, your settings folder
    # (runtime_local\user-data-2.0.0) and other extra files stay.
    foreach ($source in @($runtimeRoot, $filesDir)) {
        & robocopy $source $InstallDir /E /R:2 /W:2 /NFL /NDL /NJH /NJS /NP | Out-Null
        if ($LASTEXITCODE -ge 8) { Fail "Copying files failed (robocopy exit $LASTEXITCODE)." }
    }
    # One program, one name: PushFPS.exe replaces Veyra's launcher in this folder.
    Remove-Item (Join-Path $InstallDir 'veyra_qml_ui.exe') -Force -ErrorAction SilentlyContinue
    $versionLines = @(
        "PushFPS $PushFpsVersion",
        "Runtime: $asset from https://github.com/$UpstreamRepo/releases/tag/$UpstreamTag",
        "Runtime SHA-256: $expected"
    ) + @($skipped | ForEach-Object { "Not installed: $_" })
    Set-Content -Path (Join-Path $InstallDir 'PushFPS-VERSION.txt') -Value $versionLines -Encoding UTF8
} finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}

$target = Join-Path $InstallDir 'PushFPS.exe'
if (-not $NoShortcuts) {
    Step 'Adding Start menu and desktop shortcuts'
    $shell = New-Object -ComObject WScript.Shell
    foreach ($link in @($startMenu, $desktop)) {
        $s = $shell.CreateShortcut($link)
        $s.TargetPath = $target
        $s.WorkingDirectory = $InstallDir
        $s.IconLocation = "$target,0"
        $s.Description = 'PushFPS: console frame generation'
        $s.Save()
    }
}

# Windows keeps icons in a cache keyed by file path, so after an update the shortcuts and taskbar can keep
# showing the previous version's icon. Ask Explorer to refresh it (per user, no admin rights, best effort).
try {
    $ie4 = Join-Path $env:WINDIR 'System32\ie4uinit.exe'
    if (Test-Path $ie4) {
        $p = Start-Process -FilePath $ie4 -ArgumentList '-show' -WindowStyle Hidden -PassThru -ErrorAction Stop
        if (-not $p.WaitForExit(10000)) { $p.Kill() }
    }
} catch { }

Step 'PushFPS is installed.'
Info "Program: $target"
Info "Remove it any time with: Install-PushFPS.cmd -Uninstall"
Info "The downloaded package stays in $cacheDir for quick reinstalls; you can delete it."

if (-not $NoLaunch) { Start-Process -FilePath $target -WorkingDirectory $InstallDir }
exit 0
