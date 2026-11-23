<#
.SYNOPSIS
  Checks workflow: runs the hardware test kit and the diagnostics tool dry (no PushFPS, no PS5, made-up
  logs with planted private values) and checks what they leave behind. Not for testers.

.DESCRIPTION
  Seven parts, each with its own prefix in the error annotations:
    kit            run-tests.ps1 -DryRun with every mode: latency arms (-Health), -Soak, -Impairment, -Reliability,
                   -ReliabilityGpuLoss. Expected runs and files, the verdict of every reliability check on the dry
                   run's made-up timeline, nothing planted in the zip, then report.py --strict, reliability.py,
                   soak.py and baseline.py on that zip.
    kill-switch    a dry soak that writes its own stop file: the run must end as "kill-switch (stop file)" and
                   still leave its evidence and the zip.
    versus-full    -Versus -Full: every 'versus' arm with its one switch, a 10-minute stability run (3 s dry), the
                   reliability checks; RESULT.txt, nothing planted, report.py --strict.
    resume         two arms, then -Resume with three: only the missing arm runs.
    verdicts       130 made-up presses per arm: RESULT.txt must call HIGHER, LOWER and 'no difference shown'.
    first-test-experimental   Run-First-Test -Build experimental -DryRun: no 'two different folders' refusal.
    sessions       sprint 4: the kit's bootstrap against report.py's numbers (ci\bootstrap-golden.json); -Session switches
                   (made-up sprint 4 lines, low-latency queue off in s-legacy: refused naming lowQueue; Wi-Fi labels),
                   -Session quality, -Session dedup-probe, and two arms of a build without the new lines; report.py
                   --strict on each zip must not DISAGREE with the kit; -Session network (SmartLink arms, original and
                   stable builds, Wi-Fi, a trace with a planted word that must be withheld).
  The kit part also plants a different bitrate in one arm (-DryRunMismatchArm mod-input): RESULT.txt / result.json
  must refuse exactly that arm and name the bitrate.
    diagnostics    collect-diagnostics.ps1 -DryRun: exactly one zip, the expected files, nothing planted in it,
                   the list of what is not collected in its summary.
  Every problem becomes one "::error::" line; the evidence that explains it is printed in groups below.
  Exit code 1 when anything is wrong.

  Windows PowerShell 5.1, as on a tester's PC. A pass does not establish that the kit works against a real
  PushFPS: the dry run starts none.
#>
param(
    [string]$OutRoot = 'out',
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
$kit = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$repo = (Resolve-Path -LiteralPath (Join-Path $kit '..\..\..')).Path
$tools = Join-Path $repo 'tools\remoteplay_latency_bench'
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
$OutRoot = (Resolve-Path -LiteralPath $OutRoot).Path
$problems = New-Object System.Collections.Generic.List[string]
$groups = New-Object System.Collections.Generic.List[object]

function Add-Problem([string]$Part, [string]$Text) {
    $script:problems.Add(($Part + ': ' + ($Text -replace '\s+', ' ')))
}

# Lines to print in a folded group when the check fails.
function Add-Group([string]$Title, $Lines) {
    $script:groups.Add([pscustomobject]@{ title = $Title; lines = @($Lines) })
}

function Get-Tree([string]$Folder) {
    $rows = @()
    if ((-not $Folder) -or (-not (Test-Path -LiteralPath $Folder))) { return @('(folder does not exist)') }
    $root = (Resolve-Path -LiteralPath $Folder).Path
    foreach ($file in @(Get-ChildItem -LiteralPath $root -Recurse -File)) { $rows += ('{0,10}  {1}' -f $file.Length, $file.FullName.Substring($root.Length + 1)) }
    if ($rows.Count -eq 0) { $rows += '(no files)' }
    return $rows
}

# Unzips the only zip of $Folder into $Folder\unzipped and returns that path ('' and a problem when there is none).
function Expand-OnlyZip([string]$Part, [string]$Folder) {
    $zips = @()
    if (Test-Path -LiteralPath $Folder) { $zips = @(Get-ChildItem -LiteralPath $Folder -Filter '*.zip') }
    if ($zips.Count -ne 1) { Add-Problem $Part ("expected exactly one zip in the output folder, found " + $zips.Count); if ($zips.Count -eq 0) { return '' } }
    $target = Join-Path $Folder 'unzipped'
    Expand-Archive -LiteralPath $zips[0].FullName -DestinationPath $target -Force
    return $target
}

# Values the dry runs plant in their made-up logs and files, and this machine's own names: none may be in a zip.
$planted = @('Planted', '0123456789abcdef', '192.168.1.77', '203.0.113.9', 'AABBCCDDEEFF', '00:1A:2B', 'fe80', '00112233445566778899', 'example.com', '001A2B3C4D5E',
             '001a.2b3c', '0F8FAD5B', '1A2B3C4D5E6F7A8', '%12', $env:COMPUTERNAME, $env:USERNAME)
function Test-NothingPlanted([string]$Part, $Files) {
    foreach ($value in $planted) {
        if ((-not $value) -or (@($Files).Count -eq 0)) { continue }
        foreach ($hit in @($Files | Select-String -SimpleMatch -Pattern $value | Select-Object -First 3)) {
            Add-Problem $Part ("a value that must be removed is in the zip: " + (Split-Path -Leaf $hit.Path) + " line " + $hit.LineNumber)
        }
    }
}

# Kit 5: what report.md says where the kit and the report DISAGREE (the annotation is all the orchestrator sees), and the
# MEASURED VALUES section of a RESULT.txt: present, and not replaced by the line the kit writes when that part failed.
$lastToolTail = ''
function Get-DisagreeText([string]$ReportFile) {
    $found = @()
    if (Test-Path -LiteralPath $ReportFile) {
        foreach ($line in @(Get-Content -LiteralPath $ReportFile)) { if ($line.IndexOf('DISAGREE') -ge 0) { $found += $line } }
    }
    $text = ($found -join ' // ')
    if ($text.Length -gt 1200) { $text = $text.Substring(0, 1200) + ' ...' }
    return $text
}
function Test-MeasuredSection([string]$Part, [string]$Body) {
    if ($Body.IndexOf('MEASURED VALUES') -lt 0) { Add-Problem $Part 'RESULT.txt has no MEASURED VALUES section (kit 5)' }
    $at = $Body.IndexOf('the kit failed here')
    if ($at -ge 0) {
        $end = $Body.IndexOf("`n", $at)
        if ($end -lt 0) { $end = $Body.Length }
        Add-Problem $Part ('MEASURED VALUES was not written: ' + $Body.Substring($at, [math]::Min(400, $end - $at)))
    }
}

function Test-Holds([string]$Part, $Files, [string[]]$Values, [string]$What) {
    foreach ($value in $Values) {
        if ((@($Files).Count -eq 0) -or (@($Files | Select-String -SimpleMatch -Pattern $value).Count -eq 0)) { Add-Problem $Part ("no " + $What + " holds '" + $value + "'") }
    }
}

# Runs a Python tool; returns its exit code, keeps its output for the groups.
function Invoke-Tool([string]$Title, [string[]]$Arguments) {
    $ErrorActionPreference = 'Continue'
    $output = & $Python -B @Arguments 2>&1
    $code = $LASTEXITCODE
    $script:lastToolTail = (@($output | ForEach-Object { [string]$_ } | Select-Object -Last 4) -join ' / ')
    Add-Group ($Title + ' (exit code ' + $code + ')') @($output | ForEach-Object { [string]$_ })
    return $code
}

# RESULT.txt and result.json of an unzipped evidence folder: present, the software and the physical parts apart,
# exactly the arms in $Refused refused (with $Why in their reasons), and duplicate detection in no arm.
function Test-Result([string]$Part, [string]$Folder, [string[]]$Refused, [string]$Why) {
    $text = Join-Path $Folder 'RESULT.txt'
    $json = Join-Path $Folder 'result.json'
    if (-not (Test-Path -LiteralPath $text)) { Add-Problem $Part 'no RESULT.txt in the zip'; return }
    if (-not (Test-Path -LiteralPath $json)) { Add-Problem $Part 'no result.json in the zip'; return }
    $body = Get-Content -LiteralPath $text -Raw
    foreach ($heading in @('SOFTWARE LATENCY (the built-in test: software, not input-to-photon)', 'REFUSED ARMS', 'PHYSICAL MEASUREMENTS', 'MISSING APP LOG FIELDS', 'WHAT TO SEND', 'Verdict rule:')) {
        if ($body.IndexOf($heading) -lt 0) { Add-Problem $Part ("RESULT.txt has no '" + $heading + "'") }
    }
    $at = $body.IndexOf('PHYSICAL MEASUREMENTS')
    if (($at -ge 0) -and ($body.IndexOf('median ms', $at) -ge 0)) { Add-Problem $Part 'RESULT.txt puts latency figures into the physical part' }
    Test-MeasuredSection $Part $body
    $result = Get-Content -LiteralPath $json -Raw | ConvertFrom-Json
    foreach ($arm in @($result.arms)) {
        $should = ($Refused -contains $arm.name)
        if ([bool]$arm.refused -ne $should) { Add-Problem $Part ("result.json: arm '" + $arm.name + "' refused=" + $arm.refused + ", expected " + $should + " (" + (@($arm.reasons) -join '; ') + ")") }
        if ($should -and ((@($arm.reasons) -join ' ') -notmatch $Why)) { Add-Problem $Part ("result.json: arm '" + $arm.name + "' is refused without naming the " + $Why) }
        if ($arm.presses -lt 1) { Add-Problem $Part ("result.json: arm '" + $arm.name + "' has no press") }
        if ($null -eq $arm.median) { Add-Problem $Part ("result.json: arm '" + $arm.name + "' has no median") }
    }
    if (@($result.arms).Count -eq 0) { Add-Problem $Part 'result.json holds no arm' }
    if ((Get-Content -LiteralPath $json -Raw) -match 'STREAM_DEDUP') { Add-Problem $Part 'duplicate detection appears in a run of the session' }
    Add-Group ($Part + ': RESULT.txt') (Get-Content -LiteralPath $text)
}

# ====================================================================================================
# Part 1: the kit, every mode
# ====================================================================================================
$part = 'kit'
$kitOut = Join-Path $OutRoot 'kit'
try {
    & (Join-Path $kit 'run-tests.ps1') -DryRun -Health -Soak -Impairment -Reliability -ReliabilityGpuLoss -DryRunMismatchArm mod-input -OutRoot $kitOut -ExpectRefreshHz 60
} catch {
    Add-Problem $part ("run-tests.ps1 stopped: " + $_.Exception.Message + " (script line " + $_.InvocationInfo.ScriptLineNumber + ")")
}
$unzipped = Expand-OnlyZip $part $kitOut
$files = @()
if ($unzipped) { $files = @(Get-ChildItem -LiteralPath $unzipped -Recurse -File) }
$bench = @($files | Where-Object { $_.Directory.Name -eq 'bench' })

# Expected runs: one per quick or standard arm, and one of each other kind.
$expected = @()
$modified = @()
foreach ($row in (Get-Content (Join-Path $kit 'arms.csv') | Where-Object { $_ -and ($_ -notmatch '^\s*#') } | ConvertFrom-Csv)) {
    if (($row.set -eq 'quick') -or ($row.set -eq 'standard')) { $expected += $row.name; if ($row.exe -eq 'modified') { $modified += $row.name } }
}
$expected += @('soak-mod-all', 'impairment-mod-all', 'reliability-mod-all', 'gpuloss-mod-all', 'safestart-safe-start')
$markerLines = @{ 'impairment-mod-all' = 5; 'reliability-mod-all' = 13; 'gpuloss-mod-all' = 3 }
# What the rules must say on the dry run's made-up timeline (Add-DryRunTimeline in run-tests.ps1).
$verdicts = @{
    'reliability-mod-all' = @{ 'cable-out-1s' = 'PASS'; 'cable-out-5s' = 'FAIL'; 'display-mode-change' = 'PASS'; 'display-reconnect' = 'NOT-EXERCISED'; 'focus-loss-keys-held' = 'SKIPPED'; 'crash-recovery' = 'PASS' }
    'gpuloss-mod-all' = @{ 'gpu-device-loss' = 'PASS' }
    'safestart-safe-start' = @{ 'safe-start' = 'PASS' }
}
foreach ($name in $expected) {
    $logs = @($bench | Where-Object { $_.Name -like ($name + '@*.txt') })
    if ($logs.Count -eq 0) { Add-Problem $part ("missing run log: bench\" + $name + "@<time>.txt"); continue }
    if ($logs.Count -gt 1) { Add-Problem $part ("run '" + $name + "' has " + $logs.Count + " logs, expected 1") }
    foreach ($log in $logs) {
        $run = Join-Path (Join-Path $unzipped 'runs') $log.BaseName
        foreach ($needed in @('run.json', 'samples.csv')) {
            if (-not (Test-Path -LiteralPath (Join-Path $run $needed))) { Add-Problem $part ("missing file: runs\" + $log.BaseName + "\" + $needed) }
        }
        $record = $null
        if (Test-Path -LiteralPath (Join-Path $run 'run.json')) {
            $record = Get-Content -LiteralPath (Join-Path $run 'run.json') -Raw | ConvertFrom-Json
            if (-not $record.completed) { Add-Problem $part ("run did not finish (run.json has no completed=true): runs\" + $log.BaseName) }
            if ($record.sampleRows -lt 1) { Add-Problem $part ("run recorded no sample: runs\" + $log.BaseName) }
            $health = (@($record.kitSwitches) -contains 'VEYRA_EXP_RP_HEALTH=1')
            if (($modified -contains $name) -and (-not $health)) { Add-Problem $part ("-Health did not reach the modified arm '" + $name + "' (run.json kitSwitches)") }
            if (($name -eq 'original') -and $health) { Add-Problem $part "the original build was given the health switch" }
            if (($name -eq 'safestart-safe-start') -and (@($record.kitSwitches).Count + @($record.switches).Count -gt 0)) { Add-Problem $part "the safe start had a switch" }
            if (($name -eq 'gpuloss-mod-all') -and (-not (@($record.kitSwitches) -contains 'VEYRA_TEST_GPU_FAILURE=900:500'))) { Add-Problem $part "the GPU loss run did not record its test switch" }
            if (($name -eq 'soak-mod-all') -and ($record.stopReason -ne 'time')) { Add-Problem $part ("the soak should have ended by its time limit, run.json says '" + $record.stopReason + "'") }
        }
        if (Test-Path -LiteralPath (Join-Path $run 'samples.csv')) {
            $sampleLines = @(Get-Content -LiteralPath (Join-Path $run 'samples.csv'))
            if ($sampleLines.Count -lt 2) { Add-Problem $part ("samples.csv has a header and no row: runs\" + $log.BaseName) }
            if ($sampleLines[0] -notmatch 'appGpu3dPercent,appGpuDecodePercent,appGpuDedicatedMb,appGpuSharedMb,sampling') { Add-Problem $part ("samples.csv has no columns for the process's own GPU counters: runs\" + $log.BaseName) }
        }
        if ($markerLines.ContainsKey($name)) {
            $markers = Join-Path $run 'markers.csv'
            if (-not (Test-Path -LiteralPath $markers)) { Add-Problem $part ("missing file: runs\" + $log.BaseName + "\markers.csv") }
            elseif (@(Get-Content -LiteralPath $markers).Count -ne $markerLines[$name]) {
                Add-Problem $part ("markers.csv should hold " + $markerLines[$name] + " lines, it holds " + @(Get-Content -LiteralPath $markers).Count + ": runs\" + $log.BaseName)
                Add-Group ('markers.csv of ' + $log.BaseName) (Get-Content -LiteralPath $markers)
            }
        }
        if (($name -eq 'soak-mod-all') -and (-not (Test-Path -LiteralPath (Join-Path $run 'presentmon-windows.csv')))) { Add-Problem $part "the soak run has no presentmon-windows.csv" }
        if ($verdicts.ContainsKey($name)) {
            $csv = Join-Path $run 'reliability.csv'
            if (-not (Test-Path -LiteralPath $csv)) { Add-Problem $part ("missing file: runs\" + $log.BaseName + "\reliability.csv"); continue }
            $found = @{}
            foreach ($row in @(Get-Content -LiteralPath $csv | ConvertFrom-Csv)) { $found[$row.check] = $row.result }
            $wrong = $false
            foreach ($check in $verdicts[$name].Keys) {
                if ($found[$check] -ne $verdicts[$name][$check]) { $wrong = $true; Add-Problem $part ("reliability check '" + $check + "' should be " + $verdicts[$name][$check] + " on the made-up timeline, the kit says '" + $found[$check] + "'") }
            }
            if ($found.Count -ne $verdicts[$name].Count) { $wrong = $true; Add-Problem $part ("reliability.csv of '" + $name + "' holds " + $found.Count + " checks, expected " + $verdicts[$name].Count) }
            if ($wrong) {
                Add-Group ('reliability.csv of ' + $log.BaseName) (Get-Content -LiteralPath $csv)
                Add-Group ('markers.csv of ' + $log.BaseName) (Get-Content -LiteralPath (Join-Path $run 'markers.csv') -ErrorAction SilentlyContinue)
            }
        }
    }
}
foreach ($log in $bench) {
    $label = $log.BaseName.Split('@')[0]
    if ($expected -notcontains $label) { Add-Problem $part ("unexpected run log: bench\" + $log.Name) }
}
if ($unzipped) {
    foreach ($folder in @(Get-ChildItem (Join-Path $unzipped 'runs') -Directory -ErrorAction SilentlyContinue)) {
        if (@($bench | Where-Object { $_.BaseName -eq $folder.Name }).Count -eq 0) { Add-Problem $part ("unexpected run folder without a log: runs\" + $folder.Name) }
    }
    foreach ($needed in @('kit.json', 'hardware.json', 'arms.csv', 'camera.csv', 'withheld.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $unzipped $needed))) { Add-Problem $part ("missing file: " + $needed) }
    }
    if (Test-Path -LiteralPath (Join-Path $unzipped 'stop.txt')) { Add-Problem $part 'stop.txt was packed' }
    Test-Result $part $unzipped @('mod-input') 'bitrate'
    $kitRecord = Get-Content -LiteralPath (Join-Path $unzipped 'kit.json') -Raw | ConvertFrom-Json
    if (-not $kitRecord.finished) { Add-Problem $part 'kit.json of a session that ran to its end has no finished=true (Run-First-Test would offer to continue it)' }
    if (Test-Path -LiteralPath (Join-Path $unzipped 'hardware.json')) {
        $hardware = Get-Content -LiteralPath (Join-Path $unzipped 'hardware.json') -Raw | ConvertFrom-Json
        if ($hardware.computer -ne '(removed)') { Add-Problem $part 'hardware.json still names the computer' }
    }
    if (Test-Path -LiteralPath (Join-Path $unzipped 'withheld.txt')) {
        $withheld = @(Get-Content -LiteralPath (Join-Path $unzipped 'withheld.txt'))
        if ($withheld[0] -ne 'files the kit left out of the zip: 0') { Add-Problem $part ("the kit withheld its own files: " + ($withheld -join '; ')) }
    }
}
Test-NothingPlanted $part $files
# What the masking and the wider whitelist must have left in the run logs.
Test-Holds $part $bench @('host=192.168.x.x', 'host=[name]', 'host=x.x.x.x', '[ipv6]', '[mac]', '[hex]', 'trials answerMs=[', 'path=[path]', '[qml] [update-check]', '[frame-flow]', '[display-vrr]', '[remoteplay-health] grade=', '[gpu-failure]', '[display-refresh]', '[email]', '[guid]', 'computer=[name]', '[slipstream-governor]', '[push-hardware]') 'run log'
if ($env:USERNAME -and ($env:USERNAME.Length -ge 3)) { Test-Holds $part $bench @('made-up profile owner [removed] end') 'run log' }

$kitZip = $null
if (Test-Path -LiteralPath $kitOut) { $kitZip = Get-ChildItem -LiteralPath $kitOut -Filter '*.zip' | Select-Object -First 1 }
if (($problems.Count -eq 0) -and $kitZip) {
    $reportOut = Join-Path $kitOut 'report'
    $code = Invoke-Tool 'report.py --strict' @((Join-Path $tools 'report.py'), $kitZip.FullName, '--out', $reportOut, '--strict')
    if ($code -ne 0) { Add-Problem $part ("report.py exit code " + $code + " (2 = its privacy check found something): " + $script:lastToolTail) }
    elseif (-not (Test-Path -LiteralPath (Join-Path $reportOut 'report.html'))) { Add-Problem $part 'report.py wrote no report.html' }
    else {
        $markdown = Get-Content -LiteralPath (Join-Path $reportOut 'report.md') -Raw
        foreach ($heading in @('## What this evidence holds', '## What each arm ran with', '## Network health', '## Soak result', '## Soak trends', '## Reliability checks')) {
            if ($markdown.IndexOf($heading) -lt 0) { Add-Problem $part ("the report has no section '" + $heading + "'") }
        }
        if ($markdown.IndexOf('DISAGREE') -ge 0) { Add-Problem $part ("the kit and the report disagree on the made-up data: " + (Get-DisagreeText (Join-Path $reportOut 'report.md'))) }
    }
    # The made-up timeline has one check that fails on purpose (cable-out-5s): exit code 1, and no disagreement with the kit.
    $code = Invoke-Tool 'reliability.py' @((Join-Path $tools 'reliability.py'), $kitZip.FullName)
    if ($code -ne 1) { Add-Problem $part ("reliability.py exit code " + $code + ", expected 1 (one check fails on purpose in the made-up timeline)") }
    $code = Invoke-Tool 'soak.py' @((Join-Path $tools 'soak.py'), $kitZip.FullName)
    if ($code -ne 0) { Add-Problem $part ("soak.py exit code " + $code + ", expected 0 (a three-second dry soak is too short to judge, not a failure)") }
    $store = Join-Path $kitOut 'baselines'
    $code = Invoke-Tool 'baseline.py save' @((Join-Path $tools 'baseline.py'), 'save', 'ci-dry-run', $kitZip.FullName, '--arm', 'mod-legacy', '--store', $store, '--replace')
    if ($code -ne 0) { Add-Problem $part ("baseline.py save exit code " + $code) }
    $code = Invoke-Tool 'baseline.py compare' @((Join-Path $tools 'baseline.py'), 'compare', 'ci-dry-run', $kitZip.FullName, '--candidate', 'mod-all', '--store', $store)
    if ($code -ne 0) { Add-Problem $part ("baseline.py compare exit code " + $code + ", expected 0 (one pair of runs gives no verdict)") }
}
Add-Group 'kit: evidence tree (from the zip)' (Get-Tree $unzipped)
foreach ($log in $bench) { Add-Group ('kit: run log ' + $log.Name + ' (last 25 lines)') (Get-Content -LiteralPath $log.FullName -Tail 25) }
foreach ($name in @('kit.json', 'withheld.txt')) {
    if ($unzipped -and (Test-Path -LiteralPath (Join-Path $unzipped $name))) { Add-Group ('kit: ' + $name) (Get-Content -LiteralPath (Join-Path $unzipped $name)) }
}
$kitRuns = $bench.Count

# ====================================================================================================
# Part 2: the kill-switch of a soak
# ====================================================================================================
$part = 'kill-switch'
$stopOut = Join-Path $OutRoot 'kit-stop'
try {
    & (Join-Path $kit 'run-tests.ps1') -DryRun -SkipLatency -Soak -DryRunStopAfter 1 -OutRoot $stopOut -ExpectRefreshHz 60
} catch {
    Add-Problem $part ("run-tests.ps1 stopped: " + $_.Exception.Message + " (script line " + $_.InvocationInfo.ScriptLineNumber + ")")
}
$unzippedStop = Expand-OnlyZip $part $stopOut
if ($unzippedStop) {
    $runFiles = @(Get-ChildItem -LiteralPath $unzippedStop -Recurse -File -Filter 'run.json')
    if ($runFiles.Count -ne 1) { Add-Problem $part ("expected one run, found " + $runFiles.Count) }
    foreach ($runFile in $runFiles) {
        $record = Get-Content -LiteralPath $runFile.FullName -Raw | ConvertFrom-Json
        if ($record.stopReason -ne 'kill-switch (stop file)') { Add-Problem $part ("run.json stopReason is '" + $record.stopReason + "', expected 'kill-switch (stop file)'") }
        if (-not $record.completed) { Add-Problem $part 'the run stopped by the kill-switch did not write its evidence (no completed=true)' }
        if ($record.sampleRows -lt 1) { Add-Problem $part 'the run stopped by the kill-switch has no sample' }
        Add-Group 'kill-switch: run.json' (Get-Content -LiteralPath $runFile.FullName)
    }
    if (@(Get-ChildItem -LiteralPath $unzippedStop -Recurse -File -Filter 'stop.txt').Count -gt 0) { Add-Problem $part 'stop.txt was packed' }
    Test-NothingPlanted $part @(Get-ChildItem -LiteralPath $unzippedStop -Recurse -File)
}
Add-Group 'kill-switch: evidence tree (from the zip)' (Get-Tree $unzippedStop)

# ====================================================================================================
# Part 2b: the stable candidate against the experimental build, as a -Full session (latency, stability, reliability)
# ====================================================================================================
$part = 'versus-full'
$versusOut = Join-Path $OutRoot 'kit-versus'
try {
    & (Join-Path $kit 'run-tests.ps1') -DryRun -Versus -Full -OutRoot $versusOut
} catch {
    Add-Problem $part ("run-tests.ps1 stopped: " + $_.Exception.Message + " (script line " + $_.InvocationInfo.ScriptLineNumber + ")")
}
$unzippedVersus = Expand-OnlyZip $part $versusOut
if ($unzippedVersus) {
    $versusBench = @(Get-ChildItem -LiteralPath (Join-Path $unzippedVersus 'bench') -File -ErrorAction SilentlyContinue)
    $wanted = @()
    foreach ($row in (Get-Content (Join-Path $kit 'arms.csv') | Where-Object { $_ -and ($_ -notmatch '^\s*#') } | ConvertFrom-Csv)) { if ($row.set -eq 'versus') { $wanted += $row.name } }
    $wanted += @('soak-mod-all', 'reliability-mod-all', 'safestart-safe-start')
    foreach ($name in $wanted) {
        if (@($versusBench | Where-Object { $_.Name -like ($name + '@*.txt') }).Count -ne 1) { Add-Problem $part ("expected one run log of '" + $name + "'") }
    }
    # -Full runs the network session too: every arm twice (the run is the unit there, A B B A).
    $wantedNetwork = @('network-n-off', 'network-n-observe', 'network-n-apply', 'network-n-audio')
    foreach ($name in $wantedNetwork) {
        if (@($versusBench | Where-Object { $_.Name -like ($name + '@*.txt') }).Count -ne 2) { Add-Problem $part ("expected two run logs of '" + $name + "'") }
    }
    $wantedLogs = $wanted.Count + 2 * $wantedNetwork.Count
    if ($versusBench.Count -ne $wantedLogs) { Add-Problem $part ("expected " + $wantedLogs + " run logs, found " + $versusBench.Count) }
    foreach ($runFile in @(Get-ChildItem -LiteralPath (Join-Path $unzippedVersus 'runs') -Recurse -File -Filter 'run.json')) {
        $record = Get-Content -LiteralPath $runFile.FullName -Raw | ConvertFrom-Json
        if (($record.arm -like 'x-*') -and ($record.arm -ne 'x-base') -and (@($record.switches).Count -ne 1)) { Add-Problem $part ("arm '" + $record.arm + "' should run with exactly its one named switch") }
        if (($record.purpose -eq 'soak') -and ($record.limitSeconds -lt 1)) { Add-Problem $part 'the stability run had no time limit' }
    }
    $kitRecord = Get-Content -LiteralPath (Join-Path $unzippedVersus 'kit.json') -Raw | ConvertFrom-Json
    if ((-not $kitRecord.full) -or (-not $kitRecord.versus) -or ($kitRecord.soakMinutes -ne 10) -or ($kitRecord.estimatedMinutes -lt 1)) { Add-Problem $part ("kit.json: full/versus/soakMinutes 10/estimate expected, got " + $kitRecord.full + "/" + $kitRecord.versus + "/" + $kitRecord.soakMinutes + "/" + $kitRecord.estimatedMinutes) }
    # The stable build logs no [slipstream-config]. Since REVIEW_kit2 item 6, x-base against stable either gets a verdict
    # that names what was "assumed from the test profile", or is refused with what is "NOT CHECKED"; never silently.
    $versusJson = Get-Content -LiteralPath (Join-Path $unzippedVersus 'result.json') -Raw | ConvertFrom-Json
    $xBase = @($versusJson.arms | Where-Object { $_.name -eq 'x-base' })
    if ($xBase.Count -ne 1) { Add-Problem $part 'result.json has no x-base arm' }
    elseif ($xBase[0].refused) { Test-Result $part $unzippedVersus @('x-base') 'NOT CHECKED' }
    else {
        Test-Result $part $unzippedVersus @() ''
        if ([string]$xBase[0].verdict -notmatch 'assumed from the test profile') { Add-Problem $part ('x-base against stable has a verdict that does not name what was assumed from the test profile: ' + $xBase[0].verdict) }
    }
    $versusBody = Get-Content -LiteralPath (Join-Path $unzippedVersus 'RESULT.txt') -Raw
    if ($versusBody.IndexOf('BETWEEN BUILDS') -lt 0) { Add-Problem $part 'RESULT.txt has no BETWEEN BUILDS section' }
    if ($versusBody.IndexOf('REFUSED: REFUSED') -ge 0) { Add-Problem $part 'RESULT.txt doubles the word REFUSED' }
    Test-NothingPlanted $part @(Get-ChildItem -LiteralPath $unzippedVersus -Recurse -File)
    $versusZip = Get-ChildItem -LiteralPath $versusOut -Filter '*.zip' | Select-Object -First 1
    if ($versusZip) {
        $code = Invoke-Tool 'report.py --strict (versus)' @((Join-Path $tools 'report.py'), $versusZip.FullName, '--out', (Join-Path $versusOut 'report'), '--strict')
        if ($code -ne 0) { Add-Problem $part ("report.py exit code " + $code) }
    }
}
Add-Group 'versus-full: evidence tree (from the zip)' (Get-Tree $unzippedVersus)

# ====================================================================================================
# Part 2c: an interrupted session continued with -Resume: runs already there are counted, not repeated
# ====================================================================================================
$part = 'resume'
$resumeOut = Join-Path $OutRoot 'kit-resume'
try {
    & (Join-Path $kit 'run-tests.ps1') -DryRun -Arms 'original,mod-legacy' -OutRoot $resumeOut
    $folder = @(Get-ChildItem -LiteralPath $resumeOut -Directory -Filter 'evidence-*')
    if ($folder.Count -ne 1) { Add-Problem $part ("expected one evidence folder after the first part, found " + $folder.Count) }
    else { & (Join-Path $kit 'run-tests.ps1') -DryRun -Arms 'original,mod-legacy,mod-all' -Resume $folder[0].FullName -OutRoot $resumeOut }
} catch {
    Add-Problem $part ("run-tests.ps1 stopped: " + $_.Exception.Message + " (script line " + $_.InvocationInfo.ScriptLineNumber + ")")
}
$unzippedResume = Expand-OnlyZip $part $resumeOut
if ($unzippedResume) {
    $resumeBench = @(Get-ChildItem -LiteralPath (Join-Path $unzippedResume 'bench') -File -ErrorAction SilentlyContinue)
    foreach ($name in @('original', 'mod-legacy', 'mod-all')) {
        $count = @($resumeBench | Where-Object { $_.Name -like ($name + '@*.txt') }).Count
        if ($count -ne 1) { Add-Problem $part ("arm '" + $name + "' has " + $count + " runs after the resume, expected 1") }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $unzippedResume 'resumed.txt'))) { Add-Problem $part 'no resumed.txt' }
    Test-Result $part $unzippedResume @() 'none'
    Test-NothingPlanted $part @(Get-ChildItem -LiteralPath $unzippedResume -Recurse -File)
}
Add-Group 'resume: evidence tree (from the zip)' (Get-Tree $unzippedResume)

# ====================================================================================================
# Part 2d: enough made-up presses for every verdict of RESULT.txt: HIGHER (original), LOWER (mod-all), overlap (mod-input)
# ====================================================================================================
$part = 'verdicts'
$verdictOut = Join-Path $OutRoot 'kit-verdicts'
try {
    & (Join-Path $kit 'run-tests.ps1') -DryRun -Arms 'original,mod-legacy,mod-all,mod-input' -DryRunPresses 130 -OutRoot $verdictOut
} catch {
    Add-Problem $part ("run-tests.ps1 stopped: " + $_.Exception.Message + " (script line " + $_.InvocationInfo.ScriptLineNumber + ")")
}
$unzippedVerdicts = Expand-OnlyZip $part $verdictOut
if ($unzippedVerdicts -and (Test-Path -LiteralPath (Join-Path $unzippedVerdicts 'result.json'))) {
    $result = Get-Content -LiteralPath (Join-Path $unzippedVerdicts 'result.json') -Raw | ConvertFrom-Json
    $want = @{ 'original' = '^HIGHER than mod-legacy'; 'mod-legacy' = '^baseline$'; 'mod-all' = '^LOWER than mod-legacy'; 'mod-input' = '^no difference shown' }
    foreach ($name in $want.Keys) {
        $arm = @($result.arms | Where-Object { $_.name -eq $name })
        if ($arm.Count -ne 1) { Add-Problem $part ("result.json has no arm '" + $name + "'"); continue }
        if ([string]$arm[0].verdict -notmatch $want[$name]) { Add-Problem $part ("arm '" + $name + "': verdict '" + $arm[0].verdict + "', expected " + $want[$name]) }
        if ($arm[0].presses -ne 130) { Add-Problem $part ("arm '" + $name + "': " + $arm[0].presses + " presses, expected 130") }
    }
    Test-NothingPlanted $part @(Get-ChildItem -LiteralPath $unzippedVerdicts -Recurse -File)
    $verdictZip = Get-ChildItem -LiteralPath $verdictOut -Filter '*.zip' | Select-Object -First 1
    $code = Invoke-Tool 'report.py --strict (verdicts)' @((Join-Path $tools 'report.py'), $verdictZip.FullName, '--out', (Join-Path $verdictOut 'report'), '--strict')
    if ($code -ne 0) { Add-Problem $part ("report.py exit code " + $code) }
    elseif ((Get-Content -LiteralPath (Join-Path (Join-Path $verdictOut 'report') 'report.md') -Raw).IndexOf('DISAGREE') -ge 0) { Add-Problem $part "RESULT.txt of the kit and report.py disagree (see report.md)" }
} elseif ($unzippedVerdicts) { Add-Problem $part 'no result.json in the zip' }

# ====================================================================================================
# Part 2e: Run-First-Test -Build experimental (the stable folder is not needed; the experimental copy is the modified one)
# ====================================================================================================
$part = 'first-test-experimental'
$fakeBase = Join-Path $OutRoot 'fake programs'
foreach ($folder in @('PushFPS-original', 'PushFPS-slipstream-exp')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $fakeBase $folder) | Out-Null
    Set-Content -LiteralPath (Join-Path (Join-Path $fakeBase $folder) 'PushFPS.exe') -Value 'stand-in' -Encoding ASCII
}
$experimentalOut = Join-Path $OutRoot 'kit-first-experimental'
$ErrorActionPreference = 'Continue'
$output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kit 'run-first-test.ps1') -Build experimental -InstallBase $fakeBase -OutRoot $experimentalOut -DryRun 2>&1
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
Add-Group ('first-test-experimental (exit code ' + $code + ')') @($output | ForEach-Object { [string]$_ })
if ($code -ne 0) { Add-Problem $part ("run-first-test.ps1 -Build experimental -DryRun exit code " + $code) }
if (($output | Out-String) -match 'must be two different folders') { Add-Problem $part 'the kit refused the experimental copy as the modified one' }
$unzippedExperimental = Expand-OnlyZip $part $experimentalOut
if ($unzippedExperimental -and (-not (Test-Path -LiteralPath (Join-Path $unzippedExperimental 'RESULT.txt')))) { Add-Problem $part 'no RESULT.txt in the zip' }

# Part 2e, kit 5: the double-click entry point with a Package 8 session (Run-First-Test.cmd -Session builds): the session
# name is accepted, the copies that are installed are passed on, the ones that are not are only named.
$part = 'first-test-builds'
$buildsOut = Join-Path $OutRoot 'kit-first-builds'
$ErrorActionPreference = 'Continue'
$output = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kit 'run-first-test.ps1') -Session builds -InstallBase $fakeBase -OutRoot $buildsOut -DryRun 2>&1
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$outputText = (@($output | ForEach-Object { [string]$_ }) -join "`n")
Add-Group ('first-test-builds (exit code ' + $code + ')') @($output | ForEach-Object { [string]$_ })
if ($code -ne 0) {
    $tail = (@($output | ForEach-Object { [string]$_ } | Where-Object { $_ } | Select-Object -Last 6) -join ' / ')
    if ($tail.Length -gt 700) { $tail = $tail.Substring($tail.Length - 700) }
    Add-Problem $part ("run-first-test.ps1 -Session builds -DryRun exit code " + $code + ": " + $tail)
}
if ($outputText.IndexOf('builds: the stable copy is not installed') -lt 0) { Add-Problem $part 'the missing stable copy was not named' }
if ($outputText.IndexOf('builds: the previous copy is not installed') -lt 0) { Add-Problem $part 'the missing Package 7 copy was not named' }
$unzippedBuilds = Expand-OnlyZip $part $buildsOut
if ($unzippedBuilds) {
    $buildsResult = Join-Path $unzippedBuilds 'RESULT.txt'
    if (-not (Test-Path -LiteralPath $buildsResult)) { Add-Problem $part 'no RESULT.txt in the zip' }
    elseif ((Get-Content -LiteralPath $buildsResult -Raw).IndexOf('MEASURED VALUES: BUILDS') -lt 0) { Add-Problem $part 'RESULT.txt has no MEASURED VALUES: BUILDS section' }
}

# ====================================================================================================
# Part 2f (sprint 4): -Session switches with the sprint 4 lines, a lowQueue refusal and Wi-Fi; -Session quality;
# -Session dedup-probe; the bootstrap of the kit against report.py's numbers; no DISAGREE between kit and report.
# ====================================================================================================
$part = 'sessions'
$golden = Join-Path $kit 'ci\bootstrap-golden.json'
$ErrorActionPreference = 'Continue'
$selfTest = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $kit 'run-tests.ps1') -SelfTestBootstrap $golden 2>&1
$selfCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
Add-Group ('sessions: bootstrap self-test (exit code ' + $selfCode + ')') @($selfTest | ForEach-Object { [string]$_ })
if ($selfCode -ne 0) { Add-Problem $part 'the bootstrap of run-tests.ps1 does not give the numbers of report.py (ci\bootstrap-golden.json)' }
# Stream clarity: a made-up clip file for -Clip (the dry run never plays it; the kit must pass it on and never record its path).
$clarityClip = Join-Path $OutRoot 'clarity-planted-clip.mp4'
New-Item -ItemType Directory -Force -Path $OutRoot | Out-Null
Set-Content -LiteralPath $clarityClip -Value 'not a video' -Encoding ASCII
$sessionRuns = @(
    [pscustomobject]@{ name = 'switches'; out = (Join-Path $OutRoot 'kit-switches'); args = @{ Session = 'switches'; DryRunPresses = 130; DryRunQueueArm = 's-legacy'; DryRunWifi = $true } },
    [pscustomobject]@{ name = 'quality'; out = (Join-Path $OutRoot 'kit-quality'); args = @{ Session = 'quality' } },
    [pscustomobject]@{ name = 'dedup-probe'; out = (Join-Path $OutRoot 'kit-probe'); args = @{ Session = 'dedup-probe' } },
    [pscustomobject]@{ name = 'no-new-lines'; out = (Join-Path $OutRoot 'kit-old-build'); args = @{ Arms = @('mod-legacy', 'mod-all'); DryRunPresses = 130 } },
    [pscustomobject]@{ name = 'network'; out = (Join-Path $OutRoot 'kit-network'); args = @{ Session = 'network'; NetworkWithBuilds = $true; DryRunWifi = $true; DryRunBadTraceArm = 'n-observe'; Minutes = 1 } },
    # The two short sessions the owner starts first (Run-First-Test.cmd -Session presentation / audio): no question, two
    # rounds, and arms that differ on purpose (windowed, low queue off) are neither repeated nor refused.
    [pscustomobject]@{ name = 'presentation'; out = (Join-Path $OutRoot 'kit-presentation'); args = @{ Session = 'presentation' } },
    [pscustomobject]@{ name = 'audio'; out = (Join-Path $OutRoot 'kit-audio'); args = @{ Session = 'audio' } },
    # Kit 5 (Package 8): the measured sessions. Every arm of arms.csv whose set is the session's name, two rounds, automatic.
    [pscustomobject]@{ name = 'input'; out = (Join-Path $OutRoot 'kit-input'); args = @{ Session = 'input' } },
    [pscustomobject]@{ name = 'visual'; out = (Join-Path $OutRoot 'kit-visual'); args = @{ Session = 'visual' } },
    [pscustomobject]@{ name = 'neural'; out = (Join-Path $OutRoot 'kit-neural'); args = @{ Session = 'neural' } },
    [pscustomobject]@{ name = 'framegen'; out = (Join-Path $OutRoot 'kit-framegen'); args = @{ Session = 'framegen' } },
    [pscustomobject]@{ name = 'audiofix'; out = (Join-Path $OutRoot 'kit-audiofix'); args = @{ Session = 'audiofix' } },
    [pscustomobject]@{ name = 'pacing'; out = (Join-Path $OutRoot 'kit-pacing'); args = @{ Session = 'pacing'; DryRunWifi = $true } },
    [pscustomobject]@{ name = 'builds'; out = (Join-Path $OutRoot 'kit-builds'); args = @{ Session = 'builds' } },
    [pscustomobject]@{ name = 'smoothness'; out = (Join-Path $OutRoot 'kit-smoothness'); args = @{ Session = 'smoothness' } },
    [pscustomobject]@{ name = 'retest'; out = (Join-Path $OutRoot 'kit-retest'); args = @{ Session = 'retest' } },
    [pscustomobject]@{ name = 'capture'; out = (Join-Path $OutRoot 'kit-capture'); args = @{ Session = 'capture' } },
    # Stream clarity: the bitrate arms run inside the visual session's plan (-Session visual -Arms ...), each at its own request.
    [pscustomobject]@{ name = 'clarity'; out = (Join-Path $OutRoot 'kit-clarity'); args = @{ Session = 'visual'; Arms = @('c-15', 'c-50', 'c-hdr-15') } },
    # ... and a build that did not apply the override (the made-up c-50 run requests 15000): set aside, not compared.
    [pscustomobject]@{ name = 'clarity-not-applied'; out = (Join-Path $OutRoot 'kit-clarity-not-applied'); args = @{ Session = 'visual'; Arms = @('c-15', 'c-50'); DryRunMismatchArm = 'c-50' } },
    [pscustomobject]@{ name = 'clarity-clip'; out = (Join-Path $OutRoot 'kit-clarity-clip'); args = @{ Session = 'visual'; Arms = @('cl-a-raw', 'cl-c-vsr'); Clip = $clarityClip } },
    # Rolling releases (8A / 8B / 8C): a build without the feature an arm measures says so per arm and the session finishes.
    [pscustomobject]@{ name = 'input-no-core'; out = (Join-Path $OutRoot 'kit-input-no-core'); args = @{ Session = 'input'; DryRunWithout = 'slipstream-input,slipstream-input-poll' } },
    [pscustomobject]@{ name = 'visual-no-hook'; out = (Join-Path $OutRoot 'kit-visual-no-hook'); args = @{ Session = 'visual'; DryRunWithout = 'slipstream-pipeline' } },
    [pscustomobject]@{ name = 'neural-not-in-build'; out = (Join-Path $OutRoot 'kit-neural-not-in-build'); args = @{ Session = 'neural'; DryRunNoNeural = $true } },
    # Stream network: the receive-side arms run inside the framegen session's plan (-Session framegen -Arms ...), each alone against nd-base.
    [pscustomobject]@{ name = 'netdiag'; out = (Join-Path $OutRoot 'kit-netdiag'); args = @{ Session = 'framegen'; Arms = @('nd-base', 'nd-wait2', 'nd-load') } },
    # FG latency part by part: the fglat arms run inside the smoothness session's plan (-Session smoothness -Arms ...), each against its pair.
    [pscustomobject]@{ name = 'fglat'; out = (Join-Path $OutRoot 'kit-fglat'); args = @{ Session = 'smoothness'; Arms = @('flt-off', 'flt-fg2', 'flt-fg2-phase', 'flt-fg2-hold', 'flt-fg2-grace', 'flt-fg2-ull', 'flt-fg2-ull-prio', 'flt-fg2-ull-low') } }
)
$featureText = @{
    'fglat' = @('MEASURED VALUES: SMOOTHNESS', '  flt-off (2 runs)', '  flt-fg2 (2 runs)', '  flt-fg2-ull-low (2 runs)', 'against flt-off: ', 'against flt-fg2: ', 'against flt-fg2-ull: ', 'detail profile:ok,hdr:ok,fg:ok,fgMultiplier:ok,fgTier:ok')
    'netdiag' = @('MEASURED VALUES: FRAMEGEN', '  nd-base (2 runs)', '  nd-wait2 (2 runs)', '  nd-load (2 runs)', 'against nd-base:', 'network to decode P95 1.80 ms', 'dominant cause none')
    'input-no-core' = @('MEASURED VALUES: INPUT', 'input: not in this build (no [slipstream-input] line in any run')
    'visual-no-hook' = @('MEASURED VALUES: VISUAL', 'pipeline keys not applied: this build has no VEYRA_TEST_RP_PIPELINE', 'not compared with v-original: pipeline keys not applied in this arm', 'verdict: no verdict: the pipeline keys were not applied')
    'clarity-not-applied' = @('MEASURED VALUES: VISUAL', 'test override not applied: a run''s [remoteplay] session started request', 'not compared with c-15: test override not applied in this arm')
    'clarity-clip' = @('MEASURED VALUES: VISUAL', '  cl-a-raw (2 runs)', '  cl-c-vsr (2 runs)')
    'clarity' = @('MEASURED VALUES: VISUAL', '  c-15 (2 runs)', '  c-50 (2 runs)', '  c-hdr-15 (2 runs)', 'requested 50000 kbit/s', 'console target 30000 kbit/s')
    'neural-not-in-build' = @('MEASURED VALUES: NEURAL', 'neural layer: not in this build: skipped (the build does not know the neural keys: nr:refused:unknown-key')
}
$kitFive = @('input', 'visual', 'neural', 'framegen', 'audiofix', 'pacing', 'builds', 'smoothness', 'retest', 'capture')
$kitFiveRounds = @{ 'retest' = 3 }   # every other session runs 2 rounds
$kitFiveAlso = @{ 'audiofix' = @('s-base'); 'pacing' = @('p-window') }
$kitFiveText = @{
    'input' = @('MEASURED VALUES: INPUT', 'These are times inside the PC (raw input to the pad report handed to the network), not input-to-photon', 'mouse and keyboard: profile ultra-low-latency', 'DualSense: profile adaptive-precision', 'event to send P99 4.40 ms', 'fallback none', 'interface poll: polls per line 1250, poll interval 4 ms, poll gap P50 4.00 ms, poll gap P95 4.40 ms')
    'visual' = @('MEASURED VALUES: VISUAL', 'verdict: better on these software picture metrics (banding lower; processing P95 5.00 ms, within 8.33 ms; blockiness not comparable: different output size (2560x1440 / 1920x1080))', 'verdict: better on these software picture metrics (blockiness lower, banding lower; processing P95 5.00 ms, within 8.33 ms)', 'verdict: worse on these software picture metrics (ringing higher; processing P95 5.00 ms; blockiness not comparable',
                 'verdict: cannot tell: cleaner on the picture metrics (banding lower) but processing P95 9.50 ms is over 8.33 ms; blockiness not comparable: different output size', 'neural layer: not installed: skipped (the app reports selected=nr-original selectedAvailable=0 reason=runtime-missing',
                 'verdict: no verdict: the neural layer did not run in this arm', 'picture: SDR, SDR (per run)', 'reference could not be reproduced: stream was SDR (the reference needs the H.265 HDR stream', 'stream HDR when applied 0, video HDR when applied 1', 'against v-dlss: ', 'verdict: the owner''s reference settings', 'Verdict rule (software picture metrics', 'refused 3', 'clean-up P95 0.60 ms', 'upscaler target 2560x1440, upscaler model K', 'upscaler target 3840x2160, upscaler model M', 'clean-up 50, profile none', 'video HDR when applied 1')
    'neural' = @('MEASURED VALUES: NEURAL', 'picture: SDR, SDR (per run)', 'neural layer: neural runtime available: selected=nr-original', 'graph P95 5.00 ms', 'against nn-1080: ', 'same within run-to-run spread: vblank steady present to vblank P50 (dated presents)')
    'framegen' = @('MEASURED VALUES: FRAMEGEN', 'is NOT motion', 'not from an outside tool', 'generated frames presented per second', 'cost P95 3.00 ms', 'against f-off:', 'network to decode P95 1.80 ms', 'dominant cause none')
    'audiofix' = @('MEASURED VALUES: AUDIOFIX', 'against s-base: differs: audio delay (60.0 ms / 30.0 ms)', 'Comparison rule (per value', 'own clock ran: ownFrames reached 900', 'own target 28.0 ms',
                   'own clock: FAILED ARM, not a result: ownFrames stayed 0', 'not compared with s-base: own clock: FAILED ARM')
    'pacing' = @('MEASURED VALUES: PACING', 'Wi-Fi: diagnostic only, not a wired-LAN latency conclusion', 'window: region kind 2, region rectangles max 1, one-rectangle region 1', 'scan-out audit: presents named (not a date) 16.7 %',
                 'first second with P50 under 3 ms after', 'differs: window region rectangles max (1 / 56)')
    'builds' = @('MEASURED VALUES: BUILDS', 'Regression proof: every build with the SAME test profile', 'against b-previous: ', 'same within run-to-run spread: vblank steady present to vblank P50 (dated presents)', 'in-app latency: not logged in this arm''s runs')
    'smoothness' = @('MEASURED VALUES: SMOOTHNESS', 'how EVENLY frames were PRESENTED', 'presented cadence: even (VRR on, asked for: yes; uneven presents 0.5 % median of 6 windows)', 'presented cadence: uneven (VRR off, asked for: no; uneven presents 50.0 % median of 6 windows)', 'presented cadence: cannot tell (VRR on, asked for: yes; uneven presents 5.0 %', 'presented cadence: cannot tell (VRR changed during the runs: active=false/true; asked for: yes)', 'Cadence rule (per arm', 'submitted FPS 119.8, real FPS 59.9, generated FPS 59.9', 'refresh 179.95 Hz', 'added delay estimate (hold of a ready real frame for its FG deadline) P50 12.40 ms', 'real frames on an FG phase without a generated frame/s 0.24')
    'retest' = @('MEASURED VALUES: RETEST', 'The same moving scene at 50 and 100 Mbit/s', 'run measure-r-50@', 'run measure-r-100@', 'matched pictures saved 1', 'stream rate delivered median', 'frames received per second', 'audio delay growth', 'run measure-r-50-legacyaudio@', 'run measure-r-100-clockrate@', 'console frames per second (wireFps) 59.84', 'video clock follows the console 1', 'video clock follows the console 0', 'audio picture term pts', 'audio picture term arrival', 'decoder resets 2', 'frame drops 4', 'callbacks rejected 3', 'assembly errors 5', 'resets by cause gap=1 lost=1 (episodes 2)', 'frames the console sent per second (wireFps)')
    'capture' = @('MEASURED VALUES: CAPTURE', 'generated frames between two identical pictures are copies and never count as motion', 'so the passthrough arm''s latency reads slightly high', 'capture card: name Made_Up_Card_4K, largest uncompressed mode at 60 3840x2160',
                  'game pictures: duplicate removal 1, new pictures/s (the game) 29.97', 'generated between identical pictures/s 90.00', 'in-app latency: real frame callback to Present return P50 30.00 ms',
                  'driver buffers 1, buffer mode asked minimum', 'against cap-native: differs:', 'added delay estimate (hold of a ready real frame for its FG deadline) P50 24.50 ms', 'pair phase learned 1')
}
foreach ($item in $sessionRuns) {
    $sub = $part + ' ' + $item.name
    $runArgs = $item.args   # a hashtable: an array would be splatted by position and '-Session' taken as a value
    try { & (Join-Path $kit 'run-tests.ps1') -DryRun -OutRoot $item.out @runArgs } catch { Add-Problem $sub ("run-tests.ps1 stopped: " + $_.Exception.Message + " (script line " + $_.InvocationInfo.ScriptLineNumber + ")"); continue }
    $unzippedSession = Expand-OnlyZip $sub $item.out
    if (-not $unzippedSession) { continue }
    $sessionFiles = @(Get-ChildItem -LiteralPath $unzippedSession -Recurse -File)
    Test-NothingPlanted $sub $sessionFiles
    $resultText = Join-Path $unzippedSession 'RESULT.txt'
    if (-not (Test-Path -LiteralPath $resultText)) { Add-Problem $sub 'no RESULT.txt'; continue }
    $body = Get-Content -LiteralPath $resultText -Raw
    $first = @(Get-Content -LiteralPath $resultText)[0]
    $json = Get-Content -LiteralPath (Join-Path $unzippedSession 'result.json') -Raw | ConvertFrom-Json
    Add-Group ($sub + ': RESULT.txt') (Get-Content -LiteralPath $resultText)
    Test-MeasuredSection $sub $body
    # which download installed each copy (the dry run's stand-in carries the marker of a package labelled 8A)
    if ($body.IndexOf('Copies: experimental: package 8A; previous: package 8A') -lt 0) { Add-Problem $sub "RESULT.txt has no 'Copies: experimental: package 8A; previous: package 8A' line" }
    if ($featureText.ContainsKey($item.name)) {
        foreach ($text in $featureText[$item.name]) { if ($body.IndexOf($text) -lt 0) { Add-Problem $sub ("RESULT.txt has no '" + $text + "'") } }
    }
    if ($item.name -eq 'clarity-clip') {
        foreach ($runFile in @(Get-ChildItem -LiteralPath (Join-Path $unzippedSession 'runs') -Recurse -File -Filter 'run.json')) {
            $record = Get-Content -LiteralPath $runFile.FullName -Raw | ConvertFrom-Json
            if ((@($record.kitSwitches) -join ' ') -notmatch 'VEYRA_TEST_RP_CLIP=<clip>') { Add-Problem $sub ("run of '" + $record.arm + "' does not record that it played the clip") }
        }
        foreach ($file in $sessionFiles) {
            if ((Get-Content -LiteralPath $file.FullName -Raw -ErrorAction SilentlyContinue) -match 'clarity-planted-clip') { Add-Problem $sub ("the clip's path reached " + $file.Name) }
        }
    }
    if ($kitFive -contains $item.name) {
        foreach ($text in $kitFiveText[$item.name]) { if ($body.IndexOf($text) -lt 0) { Add-Problem $sub ("RESULT.txt has no '" + $text + "'") } }
        $wantedArms = @()
        foreach ($row in (Get-Content (Join-Path $kit 'arms.csv') | Where-Object { $_ -and ($_ -notmatch '^\s*#') } | ConvertFrom-Csv)) {
            if (($row.set -eq $item.name) -or ($kitFiveAlso.ContainsKey($item.name) -and ($kitFiveAlso[$item.name] -contains $row.name))) { $wantedArms += $row }
        }
        if ($wantedArms.Count -eq 0) { Add-Problem $sub 'arms.csv has no arm for this session' }
        foreach ($row in $wantedArms) {
            $wantRuns = 2
            if ($kitFiveRounds.ContainsKey($item.name)) { $wantRuns = [int]$kitFiveRounds[$item.name] }
            if (@($sessionFiles | Where-Object { $_.Name -like ('measure-' + $row.name + '@*.txt') }).Count -ne $wantRuns) { Add-Problem $sub ("expected " + $wantRuns + " run logs of 'measure-" + $row.name + "' (one per round)") }
            if ($body.IndexOf('  ' + $row.name + ' (' + $wantRuns + ' runs)') -lt 0) { Add-Problem $sub ("MEASURED VALUES has no line '" + $row.name + " (" + $wantRuns + " runs)'") }
            if ((-not $json.measured) -or (-not $json.measured.arms.($row.name)) -or ($json.measured.arms.($row.name).runs -ne $wantRuns)) { Add-Problem $sub ("result.json measured has no arm '" + $row.name + "' with " + $wantRuns + " runs") }
        }
        if (@($sessionFiles | Where-Object { $_.Name -eq 'measures.csv' }).Count -ne 1) { Add-Problem $sub 'measures.csv is not in the zip (report.py reads the table the kit used)' }
        foreach ($runFile in @(Get-ChildItem -LiteralPath (Join-Path $unzippedSession 'runs') -Recurse -File -Filter 'run.json')) {
            $record = Get-Content -LiteralPath $runFile.FullName -Raw | ConvertFrom-Json
            if ($record.purpose -ne 'measure') { Add-Problem $sub ("run of '" + $record.arm + "' has purpose '" + $record.purpose + "', expected measure") }
            if ([string]$record.chiakiSendBuffer -ne 'unset: 16 (the pin)') { Add-Problem $sub ("run of '" + $record.arm + "' does not record the chiaki send buffer of its environment: '" + [string]$record.chiakiSendBuffer + "'") }
            $row = @($wantedArms | Where-Object { $_.name -eq $record.arm })
            if ($row.Count -ne 1) { Add-Problem $sub ("run of an arm that is not of this session: '" + $record.arm + "'"); continue }
            $shouldBeAutomatic = (@('experimental', 'previous') -contains $row[0].exe)
            if ([bool]$record.automatic -ne $shouldBeAutomatic) { Add-Problem $sub ("run of '" + $record.arm + "': automatic=" + $record.automatic + ", expected " + $shouldBeAutomatic) }
            if ($shouldBeAutomatic -and ((@($record.kitSwitches) -join ' ') -notmatch 'VEYRA_TEST_RP_AUTORUN=settle:8;(probe:20;)?wait:\d+;exit')) { Add-Problem $sub ("run of '" + $record.arm + "' has no session plan: " + (@($record.kitSwitches) -join ' ')) }
            if (([string]$row[0].switches).IndexOf('pipeline:') -ge 0) {
                if ((@($record.switches) -join ' ') -notmatch 'VEYRA_TEST_RP_PIPELINE=profile=original;') { Add-Problem $sub ("run of '" + $record.arm + "' did not get VEYRA_TEST_RP_PIPELINE: " + (@($record.switches) -join ' ')) }
            }
            if ((@($record.kitSwitches) -join ' ') -match 'VEYRA_EXP_RP_HEALTH') { Add-Problem $sub ("run of '" + $record.arm + "' got the health switch (a measured session adds nothing an arm does not name)") }
        }
    }
    if ($item.name -eq 'switches') {
        if ($first -notmatch 'Wi-Fi: diagnostic only, not a wired-LAN latency conclusion') { Add-Problem $sub 'the first line of RESULT.txt does not carry the Wi-Fi label' }
        if ($json.network.route -ne 'wifi') { Add-Problem $sub ("result.json network.route is '" + $json.network.route + "', expected wifi") }
        foreach ($arm in @($json.arms)) {
            if ([string]$arm.verdict -notmatch '^Wi-Fi: diagnostic only') { Add-Problem $sub ("verdict of '" + $arm.name + "' has no Wi-Fi label") }
            $should = ($arm.name -eq 's-legacy')
            if ([bool]$arm.refused -ne $should) { Add-Problem $sub ("arm '" + $arm.name + "' refused=" + $arm.refused + ", expected " + $should) }
            if ($should -and ((@($arm.reasons) -join ' ') -notmatch 'lowQueue')) { Add-Problem $sub "the refused arm does not name lowQueue" }
        }
        foreach ($heading in @('PC PIPELINE (arrival to scanout), software', 'CONSOLE + NETWORK (send to arrival), software', 'TOTAL (PS-button style), software', 'FRAME DELIVERY', 'AUDIO')) {
            if ($body.IndexOf($heading) -lt 0) { Add-Problem $sub ("RESULT.txt has no '" + $heading + "'") }
        }
        $base = $json.tables.pipeline.'s-base'
        if ((-not $base) -or ($base.n -lt 130)) { Add-Problem $sub 'the PC pipeline table of s-base has fewer than 130 values' }
        foreach ($runFile in @(Get-ChildItem -LiteralPath (Join-Path $unzippedSession 'runs') -Recurse -File -Filter 'run.json')) {
            $record = Get-Content -LiteralPath $runFile.FullName -Raw | ConvertFrom-Json
            if (-not $record.automatic) { Add-Problem $sub ("run of '" + $record.arm + "' was not automatic") }
            if ((@($record.kitSwitches) -join ' ') -notmatch 'VEYRA_TEST_RP_AUTORUN=settle:8;latency:') { Add-Problem $sub ("run of '" + $record.arm + "' has no autorun plan") }
            if (-not $record.testProfile) { Add-Problem $sub ("run of '" + $record.arm + "' did not use the test profile") }
        }
    } elseif ($item.name -eq 'quality') {
        if ($body -notmatch 'choice 2 quality 4') { Add-Problem $sub 'RESULT.txt has no super resolution row' }
        if ($body -notmatch 'method 7 x3') { Add-Problem $sub 'RESULT.txt has no frame generation row' }
    } elseif ($item.name -eq 'dedup-probe') {
        if ($body -notmatch 'REPEAT PICTURES IN THIS STREAM') { Add-Problem $sub 'RESULT.txt has no repeat picture table' }
        if ($body -notmatch 'exact repeats seen: yes, 840 of 870') { Add-Problem $sub 'the repeat picture sentence is not as the made-up lines say' }
    } elseif ($item.name -eq 'network') {
        foreach ($text in @('NETWORK SESSION', 'Connection: Detected connection: unstable-wifi', 'Wi-Fi: diagnostic only, not a wired-LAN latency conclusion', 'Against the original and stable builds', 'Not compared:')) {
            if ($body.IndexOf($text) -lt 0) { Add-Problem $sub ("RESULT.txt has no '" + $text + "'") }
        }
        foreach ($name in @('n-off', 'n-observe', 'n-apply', 'n-audio', 'n-original', 'n-stable')) {
            if (@($sessionFiles | Where-Object { $_.Name -like ('network-' + $name + '@*.txt') }).Count -ne 2) { Add-Problem $sub ("expected two run logs of 'network-" + $name + "' (two runs per arm, A B B A)") }
        }
        if ($body -match '(better|WORSE) than n-observe') { Add-Problem $sub 'identical made-up arms got a better or WORSE verdict' }
        if (@($sessionFiles | Where-Object { $_.Name -eq 'smartlink-trace.csv' }).Count -gt 0) { Add-Problem $sub 'a SmartLink trace with a planted word was packed (it must be withheld)' }
        $withheldText = Get-Content -LiteralPath (Join-Path $unzippedSession 'withheld.txt') -Raw
        if ($withheldText -notmatch 'smartlink-trace.csv') { Add-Problem $sub 'withheld.txt does not name the withheld SmartLink trace' }
        if (-not $json.networkSession) { Add-Problem $sub 'result.json has no networkSession' }
        elseif (@($json.networkSession.arms.'n-apply'.verdicts).Count -lt 1) { Add-Problem $sub 'result.json has no verdict of n-apply against n-observe' }
        foreach ($runFile in @(Get-ChildItem -LiteralPath (Join-Path $unzippedSession 'runs') -Recurse -File -Filter 'run.json')) {
            $record = Get-Content -LiteralPath $runFile.FullName -Raw | ConvertFrom-Json
            if (($record.arm -like 'n-*') -and ($record.arm -ne 'n-original') -and ($record.arm -ne 'n-stable') -and ((@($record.kitSwitches) -join ' ') -notmatch 'VEYRA_TEST_RP_AUTORUN=settle:8;wait:60;exit')) { Add-Problem $sub ("run of '" + $record.arm + "' has no network autorun plan") }
        }
    } elseif (@('presentation', 'audio') -contains $item.name) {
        $shortArms = @('p-window', 'p-fullscreen', 'p-lowqueue-off', 'p-test-uncovered')
        if ($item.name -eq 'audio') { $shortArms = @('s-base', 's-audio-smooth') }
        if ($body.IndexOf('SHORT SESSION: ' + $item.name.ToUpperInvariant()) -lt 0) { Add-Problem $sub 'RESULT.txt has no SHORT SESSION section' }
        foreach ($name in $shortArms) {
            if (@($sessionFiles | Where-Object { $_.Name -like ($name + '@*.txt') }).Count -ne 2) { Add-Problem $sub ("expected two run logs of '" + $name + "' (two rounds)") }
            if ($body.IndexOf('  ' + $name + ' (2 runs)') -lt 0) { Add-Problem $sub ("the SHORT SESSION section has no line '" + $name + " (2 runs)'") }
        }
        if (@($json.arms).Count -ne $shortArms.Count) { Add-Problem $sub ("result.json has " + @($json.arms).Count + " arms, expected " + $shortArms.Count) }
        foreach ($arm in @($json.arms)) { if ($arm.refused) { Add-Problem $sub ("arm '" + $arm.name + "' is refused: " + (@($arm.reasons) -join '; ')) } }
        foreach ($runFile in @(Get-ChildItem -LiteralPath (Join-Path $unzippedSession 'runs') -Recurse -File -Filter 'run.json')) {
            $record = Get-Content -LiteralPath $runFile.FullName -Raw | ConvertFrom-Json
            if (@($record.pairProblems | Where-Object { $_ }).Count -gt 0) { Add-Problem $sub ("run of '" + $record.arm + "' was named as not matching: " + (@($record.pairProblems) -join '; ')) }
            if (-not $record.automatic) { Add-Problem $sub ("run of '" + $record.arm + "' was not automatic") }
            if ((@($record.kitSwitches) -join ' ') -notmatch 'VEYRA_TEST_RP_AUTORUN=settle:8;latency:12') { Add-Problem $sub ("run of '" + $record.arm + "' has no 12-press autorun plan: " + (@($record.kitSwitches) -join ' ')) }
            if (($record.arm -eq 'p-lowqueue-off') -and ([string]$record.config.keys.lowQueue -ne '0')) { Add-Problem $sub 'the made-up log of p-lowqueue-off does not say the low queue was off' }
            if (($record.arm -eq 'p-window') -and ([string]$record.config.keys.fullscreen -ne '0')) { Add-Problem $sub 'the made-up log of p-window does not say it ran windowed' }
            if (($record.arm -eq 'p-fullscreen') -and ([string]$record.config.keys.fullscreen -ne '1')) { Add-Problem $sub 'the made-up log of p-fullscreen does not say it ran full screen' }
        }
    } elseif (($kitFive -notcontains $item.name) -and (-not $featureText.ContainsKey($item.name))) {
        if ($body.IndexOf('PC PIPELINE') -lt 0) { Add-Problem $sub 'RESULT.txt has no PC pipeline table for a build without the lines' }
        if ($json.tables.pipeline.'mod-all'.n -ne 0) { Add-Problem $sub 'a build without [slipstream-press] got pipeline values' }
        if ($json.tables.total.'mod-all'.n -lt 130) { Add-Problem $sub 'the total table lost the presses of a build without the new lines' }
    }
    $sessionZip = Get-ChildItem -LiteralPath $item.out -Filter '*.zip' | Select-Object -First 1
    if ($sessionZip) {
        $reportOut = Join-Path $item.out 'report'
        $code = Invoke-Tool ('report.py --strict (' + $item.name + ')') @((Join-Path $tools 'report.py'), $sessionZip.FullName, '--out', $reportOut, '--strict')
        if ($code -ne 0) { Add-Problem $sub ("report.py exit code " + $code + ": " + $script:lastToolTail) }
        elseif ((Get-Content -LiteralPath (Join-Path $reportOut 'report.md') -Raw).IndexOf('DISAGREE') -ge 0) { Add-Problem $sub ('the kit and report.py DISAGREE: ' + (Get-DisagreeText (Join-Path $reportOut 'report.md'))) }
    }
}

# ====================================================================================================
# Part 3: the diagnostics tool
# ====================================================================================================
$part = 'diagnostics'
$diagOut = Join-Path $OutRoot 'diagnostics'
try {
    & (Join-Path $kit 'collect-diagnostics.ps1') -DryRun -OutRoot $diagOut
} catch {
    Add-Problem $part ("collect-diagnostics.ps1 stopped: " + $_.Exception.Message + " (script line " + $_.InvocationInfo.ScriptLineNumber + ")")
}
if (@(Get-ChildItem -LiteralPath $diagOut -Directory -ErrorAction SilentlyContinue).Count -gt 0) { Add-Problem $part 'the tool left a folder beside its zip (it must leave ONE file)' }
$unzippedDiag = Expand-OnlyZip $part $diagOut
$diagFiles = @()
if ($unzippedDiag) {
    $diagFiles = @(Get-ChildItem -LiteralPath $unzippedDiag -Recurse -File)
    foreach ($needed in @('summary.txt', 'hardware.json', 'network.txt', 'versions.txt', 'compatibility.txt', 'sessions-PushFPS-dryrun.csv', 'log-PushFPS-dryrun.txt', 'withheld.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $unzippedDiag $needed))) { Add-Problem $part ("missing file: " + $needed) }
    }
    foreach ($file in $diagFiles) {
        if (@('.txt', '.json', '.csv') -notcontains $file.Extension) { Add-Problem $part ("unexpected kind of file in the zip: " + $file.Name) }
    }
    Test-NothingPlanted $part $diagFiles
    $log = @($diagFiles | Where-Object { $_.Name -eq 'log-PushFPS-dryrun.txt' })
    Test-Holds $part $log @('host=192.168.x.x', 'host=[name]', 'host=x.x.x.x', '[ipv6]', '[mac]', '[hex]', 'path=[path]', 'module=[path]', '[qml] [update-check]') 'log file'
    $summary = @($diagFiles | Where-Object { $_.Name -eq 'summary.txt' })
    Test-Holds $part $summary @('WHAT THIS IS', 'THIS PC IN SHORT', 'THINGS THAT MAY CAUSE PROBLEMS', 'WHAT IS IN THE ZIP', 'WHAT IS NOT IN THE ZIP', 'console registration keys', 'PSN account ids', 'Wi-Fi network names', 'DRY RUN', 'PushFPS logged 1 crash(es).') 'summary'
    $sessions = @($diagFiles | Where-Object { $_.Name -eq 'sessions-PushFPS-dryrun.csv' })
    Test-Holds $part $sessions @('1920x1080@60 hevc 30000 kbit/s,D3D12VA,59.9,59.9,59.9,1,0,0,1,0,0,1,0.2,6.2,unsteady,1,100.9,1200') 'session table'
    $compat = @($diagFiles | Where-Object { $_.Name -eq 'compatibility.txt' })
    Test-Holds $part $compat @('VEYRA_EXP_RP_KBM VEYRA_EXP_RP_KBM_PROFILE', '1 crash dump file(s)', 'RTSS hook module') 'compatibility file'
    if (Test-Path -LiteralPath (Join-Path $unzippedDiag 'withheld.txt')) {
        $withheld = @(Get-Content -LiteralPath (Join-Path $unzippedDiag 'withheld.txt'))
        if ($withheld[0] -ne 'files left out of the zip by the final check: 0') { Add-Problem $part ("the tool withheld its own files: " + ($withheld -join '; ')) }
    }
    foreach ($name in @('summary.txt', 'compatibility.txt', 'versions.txt', 'network.txt', 'sessions-PushFPS-dryrun.csv', 'withheld.txt')) {
        if (Test-Path -LiteralPath (Join-Path $unzippedDiag $name)) { Add-Group ('diagnostics: ' + $name) (Get-Content -LiteralPath (Join-Path $unzippedDiag $name)) }
    }
}
Add-Group 'diagnostics: tree (from the zip)' (Get-Tree $unzippedDiag)

# ====================================================================================================
if ($problems.Count -gt 0) {
    # The job log needs a sign-in to read and GitHub keeps ten annotations of a kind per step: the first one
    # carries every problem, and the result pages of the failed parts follow as notices (made-up data only).
    $all = ($problems -join ' || ')
    if ($all.Length -gt 7000) { $all = $all.Substring(0, 7000) + ' ...' }
    Write-Host ('::error::' + $problems.Count + ' problem(s): ' + $all)
    foreach ($problem in @($problems | Select-Object -First 9)) { Write-Host ('::error::' + $problem) }
    $failedParts = @($problems | ForEach-Object { ($_ -split ': ', 2)[0] } | Select-Object -Unique)
    $notices = 0
    foreach ($group in $groups) {
        if ($notices -ge 9) { break }
        if (-not ($group.title -like '*: RESULT.txt')) { continue }
        if ($failedParts -notcontains ($group.title -replace ': RESULT\.txt$', '')) { continue }
        $page = (@($group.lines | ForEach-Object { [string]$_ }) -join "`n")
        if ($page.Length -gt 3800) { $page = $page.Substring(0, 3800) + "`n..." }
        $page = $page.Replace('%', '%25').Replace("`r", '').Replace("`n", '%0A')
        Write-Host ('::notice::' + $group.title + '%0A' + $page)
        $notices++
    }
    foreach ($group in $groups) {
        Write-Host ('::group::' + $group.title)
        foreach ($line in $group.lines) { Write-Host $line }
        Write-Host '::endgroup::'
    }
    exit 1
}
Write-Host ("dry runs passed: kit " + $kitRuns + " runs with every mode, expected files, reliability verdicts and RESULT refusals, nothing planted in any zip; kill-switch run kept its evidence; versus/full and resume sessions complete; diagnostics zip complete and clean")
exit 0
