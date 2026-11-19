<#
.SYNOPSIS
  Checks workflow: runs the hardware test kit and the diagnostics tool dry (no PushFPS, no PS5, made-up
  logs with planted private values) and checks what they leave behind. Not for testers.

.DESCRIPTION
  Three parts, each with its own prefix in the error annotations:
    kit            run-tests.ps1 -DryRun with every mode: latency arms (-Health), -Soak, -Impairment, -Reliability,
                   -ReliabilityGpuLoss. Expected runs and files, the verdict of every reliability check on the dry
                   run's made-up timeline, nothing planted in the zip, then report.py --strict, reliability.py,
                   soak.py and baseline.py on that zip.
    kill-switch    a dry soak that writes its own stop file: the run must end as "kill-switch (stop file)" and
                   still leave its evidence and the zip.
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
$planted = @('Planted', '0123456789abcdef', '192.168.1.77', '203.0.113.9', 'AABBCCDDEEFF', '00:1A:2B', 'fe80', '00112233445566778899', $env:COMPUTERNAME, $env:USERNAME)
function Test-NothingPlanted([string]$Part, $Files) {
    foreach ($value in $planted) {
        if ((-not $value) -or (@($Files).Count -eq 0)) { continue }
        foreach ($hit in @($Files | Select-String -SimpleMatch -Pattern $value | Select-Object -First 3)) {
            Add-Problem $Part ("a value that must be removed is in the zip: " + (Split-Path -Leaf $hit.Path) + " line " + $hit.LineNumber)
        }
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
    Add-Group ($Title + ' (exit code ' + $code + ')') @($output | ForEach-Object { [string]$_ })
    return $code
}

# ====================================================================================================
# Part 1: the kit, every mode
# ====================================================================================================
$part = 'kit'
$kitOut = Join-Path $OutRoot 'kit'
try {
    & (Join-Path $kit 'run-tests.ps1') -DryRun -Health -Soak -Impairment -Reliability -ReliabilityGpuLoss -OutRoot $kitOut -ExpectRefreshHz 60
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
            if ($sampleLines[0] -notmatch 'appGpu3dPercent,appGpuDecodePercent,appGpuDedicatedMb,appGpuSharedMb') { Add-Problem $part ("samples.csv has no columns for the process's own GPU counters: runs\" + $log.BaseName) }
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
Test-Holds $part $bench @('host=192.168.x.x', 'host=[name]', 'host=x.x.x.x', '[ipv6]', '[mac]', '[hex]', 'trials answerMs=[', 'path=[path]', '[qml] [update-check]', '[frame-flow]', '[display-vrr]', '[remoteplay-health] grade=', '[gpu-failure]', '[display-refresh]') 'run log'

$kitZip = $null
if (Test-Path -LiteralPath $kitOut) { $kitZip = Get-ChildItem -LiteralPath $kitOut -Filter '*.zip' | Select-Object -First 1 }
if (($problems.Count -eq 0) -and $kitZip) {
    $reportOut = Join-Path $kitOut 'report'
    $code = Invoke-Tool 'report.py --strict' @((Join-Path $tools 'report.py'), $kitZip.FullName, '--out', $reportOut, '--strict')
    if ($code -ne 0) { Add-Problem $part ("report.py exit code " + $code + " (2 = its privacy check found something)") }
    elseif (-not (Test-Path -LiteralPath (Join-Path $reportOut 'report.html'))) { Add-Problem $part 'report.py wrote no report.html' }
    else {
        $markdown = Get-Content -LiteralPath (Join-Path $reportOut 'report.md') -Raw
        foreach ($heading in @('## What this evidence holds', '## What each arm ran with', '## Network health', '## Soak result', '## Soak trends', '## Reliability checks')) {
            if ($markdown.IndexOf($heading) -lt 0) { Add-Problem $part ("the report has no section '" + $heading + "'") }
        }
        if ($markdown.IndexOf('DISAGREE') -ge 0) { Add-Problem $part "the kit's reliability verdicts and reliability.py disagree on the made-up timeline (see the report)" }
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
    foreach ($problem in $problems) { Write-Host ('::error::' + $problem) }
    foreach ($group in $groups) {
        Write-Host ('::group::' + $group.title)
        foreach ($line in $group.lines) { Write-Host $line }
        Write-Host '::endgroup::'
    }
    exit 1
}
Write-Host ("dry runs passed: kit " + $kitRuns + " runs with every mode, expected files and reliability verdicts, nothing planted in any zip; kill-switch run kept its evidence; diagnostics zip complete and clean")
exit 0
