# PS5 Remote Play latency runs on real hardware (needs a PS5 on the network; nothing here contacts it by itself).
#
# Starts PushFPS in one configuration, waits until it is closed, then saves what the built-in
# latency test logged in that run. Configurations differ only in environment switches, so every
# comparison is the same build against itself.
#
#   .\latency-ab.ps1 -Exe 'C:\Path\PushFPS.exe' -Arm new
#   .\latency-ab.ps1 -Exe 'C:\Path\PushFPS.exe' -Arm original                  # timing of upstream PushFPS
#   .\latency-ab.ps1 -Exe 'C:\Path\PushFPS.exe' -Arm original -Part Input      # only one part original
#   .\latency-ab.ps1 -Exe 'C:\Path\PushFPS.exe' -Arm new -Experiment AdaptivePhase
#   .\latency-ab.ps1 -Exe 'C:\Path\PushFPS.exe' -Arm new -Experiment Poll1ms,Kbm
#
# In each run: connect to the PS5, open the latency test (PS5 streaming dialog), run it, close PushFPS.
# Alternate the configurations (A, B, A, B ...) and collect at least 120 presses per configuration:
# one press is quantised to the PS5's 16.7 ms frames and the differences looked for are a few ms.
# Then: python tools\remoteplay_latency_bench\analyze.py <logs>\bench
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [ValidateSet('new', 'original')][string]$Arm = 'new',
    # Which part runs with the original timing in the "original" arm.
    [ValidateSet('All', 'Input', 'OwnerWait', 'EngineWake')][string]$Part = 'All',
    # Experiments to switch on (all are off otherwise).
    [ValidateSet('AdaptivePhase', 'Poll1ms', 'Poll2ms', 'Kbm', 'KbmGyro')][string[]]$Experiment = @(),
    # Name of this configuration in the results; built from the switches when empty.
    [string]$Label = ''
)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Exe)) { throw "PushFPS.exe not found: $Exe" }
$legacy = @{
    All        = 'VEYRA_TEST_RP_LEGACY_TIMING'
    Input      = 'VEYRA_TEST_RP_LEGACY_INPUT'
    OwnerWait  = 'VEYRA_TEST_RP_LEGACY_OWNER_WAIT'
    EngineWake = 'VEYRA_TEST_RP_LEGACY_ENGINE_WAKE'
}
$experiments = @{
    AdaptivePhase = @{ VEYRA_EXP_RP_ADAPTIVE_PHASE = '1' }
    Poll1ms       = @{ VEYRA_EXP_RP_PAD_POLL_MS = '1' }
    Poll2ms       = @{ VEYRA_EXP_RP_PAD_POLL_MS = '2' }
    Kbm           = @{ VEYRA_EXP_RP_KBM = '1' }
    KbmGyro       = @{ VEYRA_EXP_RP_KBM = 'gyro' }
}
$managed = @($legacy.Values) + @('VEYRA_EXP_RP_ADAPTIVE_PHASE', 'VEYRA_EXP_RP_PAD_POLL_MS', 'VEYRA_EXP_RP_KBM')
foreach ($name in $managed) { Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue }
if ($Arm -eq 'original') { Set-Item -Path ("Env:" + $legacy[$Part]) -Value '1' }
foreach ($item in $Experiment) {
    foreach ($pair in $experiments[$item].GetEnumerator()) { Set-Item -Path ("Env:" + $pair.Key) -Value $pair.Value }
}
if (-not $Label) {
    $Label = $Arm
    if ($Arm -eq 'original' -and $Part -ne 'All') { $Label += "-$Part" }
    foreach ($item in $Experiment) { $Label += "+$item" }
}
$Label = $Label -replace '[^A-Za-z0-9+_.-]', '_'
$logs = Join-Path (Split-Path -Parent $Exe) 'logs'
# The log is appended across sessions: remember where each file ends now and read only what this run adds.
$before = @{}
if (Test-Path -LiteralPath $logs) { Get-ChildItem -LiteralPath $logs -Filter '*.log' | ForEach-Object { $before[$_.FullName] = $_.Length } }
Write-Host "Configuration: $Label"
Write-Host 'Connect to the PS5, run the latency test from the PS5 streaming dialog, then close PushFPS.'
$process = Start-Process -FilePath $Exe -PassThru
$process.WaitForExit()
foreach ($name in $managed) { Remove-Item -Path "Env:$name" -ErrorAction SilentlyContinue }
if (-not (Test-Path -LiteralPath $logs)) { Write-Host "No logs folder at $logs"; return }
$text = New-Object System.Text.StringBuilder
foreach ($file in Get-ChildItem -LiteralPath $logs -Filter '*.log') {
    $from = 0
    if ($before.ContainsKey($file.FullName) -and $file.Length -ge $before[$file.FullName]) { $from = $before[$file.FullName] }
    if ($file.Length -le $from) { continue }
    $stream = [System.IO.File]::Open($file.FullName, 'Open', 'Read', 'ReadWrite')
    try {
        [void]$stream.Seek($from, 'Begin')
        $reader = New-Object System.IO.StreamReader($stream, [System.Text.Encoding]::UTF8)
        [void]$text.Append($reader.ReadToEnd())
    } finally { $stream.Dispose() }
}
$lines = $text.ToString() -split "`r?`n" |
    Where-Object { $_ -match '\[remoteplay-timing\]|\[remoteplay-kbm\]|\[push-latency\] (done|trials)|\[push-latency\].*no answer' }
if (-not ($lines | Where-Object { $_ -match '\[push-latency\] done' })) { Write-Host 'The latency test did not finish in this run; nothing saved.'; return }
$lines | ForEach-Object { Write-Host $_ }
$bench = Join-Path $logs 'bench'
New-Item -ItemType Directory -Force -Path $bench | Out-Null
$out = Join-Path $bench ("{0}@{1}.txt" -f $Label, (Get-Date -Format 'yyyyMMdd-HHmmss'))
$lines | Set-Content -LiteralPath $out -Encoding UTF8
Write-Host "Saved: $out"
