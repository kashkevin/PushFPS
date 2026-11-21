<#
.SYNOPSIS
  Rollback and removal of the copies a Slipstream test package installed. Rollback-Slipstream.cmd and
  Uninstall-Slipstream.cmd in the package run this.

.DESCRIPTION
  Three steps, from the smallest to the largest (-Mode):

    Settings   (Rollback-Slipstream.cmd) Switches slipstream.ini off in the stable candidate and the
               experimental build by renaming it to slipstream.ini.off-<date>-<time>. Nothing is deleted.
               Those builds then start with their default behaviour. The original build never reads the file.
    Modified   Settings, then removes the stable candidate and the experimental build after you type
               YES. The original build stays installed.
    All        (Uninstall-Slipstream.cmd) Removes all three copies after you type YES.

  Removing a copy deletes its folder: the program, and that copy's own settings and logs inside it.

  WHAT THIS SCRIPT CAN REMOVE: exactly these folders under %LOCALAPPDATA%\Programs (or -InstallBase):
      PushFPS-original   PushFPS-slipstream   PushFPS-slipstream-exp
  and each only when it carries Slipstream-INSTALL.txt, the file Install-Slipstream writes (a copy you
  installed by hand into one of these names needs -IncludeUnmarked), holds no link or junction, and is
  not running.

  WHAT IT NEVER TOUCHES
    %LOCALAPPDATA%\Programs\PushFPS (an official PushFPS install) and its settings, the Start menu and
    desktop shortcuts, %LOCALAPPDATA%\Veyra (the PS5 pairing and other sign-ins, shared by every copy),
    Documents\slipstream-evidence (your test results), the downloaded runtime package in %TEMP%.
    It never uses the PushFPS installer's own -Uninstall, which also deletes the shortcuts named PushFPS.

  Windows PowerShell 5.1 or newer. Unverified on a real PC.

.PARAMETER Mode
  Settings, Modified or All (default Settings).
.PARAMETER InstallBase
  Folder the copies are under. Default: %LOCALAPPDATA%\Programs.
.PARAMETER Yes
  Do not ask for confirmation (for automated tests).
.PARAMETER IncludeUnmarked
  Also remove a folder of one of the three names that has no Slipstream-INSTALL.txt but holds PushFPS.exe.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File uninstall-slipstream.ps1 -Mode Settings
.EXAMPLE
  powershell -ExecutionPolicy Bypass -File uninstall-slipstream.ps1 -Mode All
#>
param(
    [ValidateSet('Settings', 'Modified', 'All')][string]$Mode = 'Settings',
    [string]$InstallBase = '',
    [switch]$Yes,
    [switch]$IncludeUnmarked
)
$ErrorActionPreference = 'Stop'
$scriptFolder = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptFolder 'slipstream-common.ps1')

$base = Get-SlipstreamInstallBase $InstallBase
$failed = 0

# Why this folder must not be removed; an empty string when it may.
function Get-RemovalObstacle([string]$Folder) {
    $item = Get-Item -LiteralPath $Folder -Force
    if (-not $item.PSIsContainer) { return 'it is a file, not a folder' }
    if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) { return 'it is a link or junction, not a real folder' }
    $hasMarker = Test-Path -LiteralPath (Join-Path $Folder $SlipstreamMarker)
    $hasProgram = Test-Path -LiteralPath (Join-Path $Folder 'PushFPS.exe')
    if (-not $hasMarker) {
        if (-not $IncludeUnmarked) { return ('it has no ' + $SlipstreamMarker + ' (not installed by Install-Slipstream); add -IncludeUnmarked to remove it anyway') }
        if (-not $hasProgram) { return ('it holds neither ' + $SlipstreamMarker + ' nor PushFPS.exe') }
    }
    if (@(Get-SlipstreamRunning $Folder).Count -gt 0) { return 'PushFPS from this folder is running; close it first' }
    # Remove-Item follows a junction and would delete what it points to.
    $links = @(Get-ChildItem -LiteralPath $Folder -Recurse -Force -ErrorAction SilentlyContinue |
               Where-Object { ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0 } | Select-Object -First 1)
    if ($links.Count -gt 0) { return ('it holds a link or junction (' + $links[0].FullName + '); remove the folder by hand') }
    return ''
}

Write-Step ('Slipstream test package: ' + $Mode)
Write-Line ('copies under: ' + $base)

# --- step 1, every mode: switch the settings files off (rename, never delete) ----------------------
Write-Step 'Settings files (slipstream.ini)'
$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
foreach ($build in $SlipstreamBuilds) {
    if ($build.Slot -eq 'original') { continue }
    $folder = Join-Path $base $build.Folder
    $file = Join-Path $folder $SlipstreamSettingsFile
    if (-not (Test-Path -LiteralPath $folder)) { Write-Line ($build.Folder + ': not installed'); continue }
    if (-not (Test-Path -LiteralPath $file)) { Write-Line ($build.Folder + ': no slipstream.ini, already default behaviour'); continue }
    $newName = $SlipstreamSettingsFile + '.off-' + $stamp
    try {
        Rename-Item -LiteralPath $file -NewName $newName
        Write-Host ('   ' + $build.Folder + ': slipstream.ini renamed to ' + $newName + ' (default behaviour from the next start)') -ForegroundColor Green
        if (@(Get-SlipstreamRunning $folder).Count -gt 0) { Write-Host '   This copy is running: the file is read at start, so close and start it again.' -ForegroundColor Yellow }
    } catch {
        $failed++
        Write-Host ('   ' + $build.Folder + ': could not rename slipstream.ini: ' + $_.Exception.Message) -ForegroundColor Red
    }
}
Write-Line 'To use a settings file again, rename it back to slipstream.ini.'

# --- step 2, Modified and All: remove folders -------------------------------------------------------
if ($Mode -ne 'Settings') {
    $wanted = @($SlipstreamBuilds | Where-Object { ($Mode -eq 'All') -or ($_.Slot -ne 'original') })
    $removable = @()
    Write-Step 'Folders'
    foreach ($build in $wanted) {
        $folder = Join-Path $base $build.Folder
        if (-not (Test-Path -LiteralPath $folder)) { Write-Line ($build.Folder + ': not there'); continue }
        $obstacle = Get-RemovalObstacle $folder
        if ($obstacle) {
            $failed++
            Write-Host ('   ' + $build.Folder + ': NOT removed, ' + $obstacle) -ForegroundColor Yellow
            continue
        }
        $removable += $folder
        Write-Line ('will remove: ' + $folder)
    }
    if ($Mode -eq 'Modified') { Write-Line ('kept: ' + (Join-Path $base 'PushFPS-original') + ' (the original build)') }
    Write-Line 'kept: an official PushFPS install and its shortcuts, your PS5 pairing, your test results'

    if ($removable.Count -eq 0) {
        Write-Line 'Nothing to remove.'
    } else {
        $confirmed = [bool]$Yes
        if (-not $confirmed) {
            if ([Console]::IsInputRedirected) {
                Write-Host '   No one to ask and -Yes was not given: nothing removed.' -ForegroundColor Yellow
            } else {
                Write-Host ''
                Write-Host '   Each folder is deleted with the settings and logs of that copy. This cannot be undone.' -ForegroundColor Yellow
                $answer = Read-Host '   Type YES to remove the folders listed above'
                $confirmed = ($answer -ceq 'YES')
                if (-not $confirmed) { Write-Line 'Not confirmed: nothing removed.' }
            }
        }
        if ($confirmed) {
            foreach ($folder in $removable) {
                # Checked again right before deleting: the name, the place and what is inside.
                $leaf = Split-Path -Leaf $folder
                $parent = Split-Path -Parent $folder
                $known = @($SlipstreamBuilds | ForEach-Object { $_.Folder })
                if (($known -notcontains $leaf) -or ($parent -ne $base) -or ($leaf -eq 'PushFPS')) {
                    $failed++
                    Write-Host ('   refused: ' + $folder) -ForegroundColor Red
                    continue
                }
                $obstacle = Get-RemovalObstacle $folder
                if ($obstacle) {
                    $failed++
                    Write-Host ('   ' + $leaf + ': NOT removed, ' + $obstacle) -ForegroundColor Yellow
                    continue
                }
                try {
                    Remove-Item -LiteralPath $folder -Recurse -Force
                    Write-Host ('   removed: ' + $folder) -ForegroundColor Green
                } catch {
                    $failed++
                    Write-Host ('   could not remove ' + $folder + ': ' + $_.Exception.Message) -ForegroundColor Red
                    Write-Line 'Close every PushFPS window and Explorer window showing that folder, then run this again.'
                }
            }
        }
    }
}

Write-Host ''
if ($failed -gt 0) {
    Write-Host ('   Finished with ' + $failed + ' item(s) not done; see above.') -ForegroundColor Yellow
    exit 1
}
Write-Line 'Finished.'
exit 0
