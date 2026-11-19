<#
.SYNOPSIS
  Records the hardware and settings that matter for PushFPS latency tests, and warns about the
  ones that would spoil a benchmark.

.DESCRIPTION
  Reads CPU, memory, graphics adapters and driver, the current display mode, network adapters,
  the default route, and a few Windows settings (GPU scheduling, variable refresh rate, Game Mode,
  power plan). Writes a JSON file and prints a summary. Changes nothing on the PC.

  Windows PowerShell 5.1 or newer. No administrator rights needed.

.PARAMETER OutFile
  Where to write the JSON. Default: hardware.json beside this script's caller (current folder).

.PARAMETER Ps5Address
  Optional IP address of the PS5. When given, 20 pings are sent and the round-trip times recorded.

.PARAMETER ExpectRefreshHz
  Refresh rate the main display should be running at. Default 180.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File detect-hardware.ps1 -Ps5Address 192.168.1.50
#>
param(
    [string]$OutFile = (Join-Path (Get-Location) 'hardware.json'),
    [string]$Ps5Address = '',
    [int]$ExpectRefreshHz = 180
)
$ErrorActionPreference = 'Continue'
$warnings = New-Object System.Collections.Generic.List[string]

function Read-RegistryValue([string]$Path, [string]$Name) {
    try { return (Get-ItemProperty -Path $Path -Name $Name -ErrorAction Stop).$Name } catch { return $null }
}

# --- processor and memory
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$system = Get-CimInstance Win32_ComputerSystem
$os = Get-CimInstance Win32_OperatingSystem
$ramGb = [math]::Round($system.TotalPhysicalMemory / 1GB, 1)

# --- graphics adapters and the mode Windows reports for each
$adapters = @()
foreach ($a in Get-CimInstance Win32_VideoController) {
    $nvidiaDriver = $null
    if ($a.Name -match 'NVIDIA' -and $a.DriverVersion) {
        # Windows reports 32.0.15.7283 for NVIDIA driver 572.83: the last five digits.
        $digits = ($a.DriverVersion -replace '\.', '')
        if ($digits.Length -ge 5) {
            $tail = $digits.Substring($digits.Length - 5)
            $nvidiaDriver = $tail.Substring(0, 3) + '.' + $tail.Substring(3)
        }
    }
    $adapters += [pscustomobject]@{
        name             = $a.Name
        driverVersion    = $a.DriverVersion
        nvidiaDriver     = $nvidiaDriver
        driverDate       = if ($a.DriverDate) { $a.DriverDate.ToString('yyyy-MM-dd') } else { $null }
        width            = $a.CurrentHorizontalResolution
        height           = $a.CurrentVerticalResolution
        refreshHz        = $a.CurrentRefreshRate
        maxRefreshHz     = $a.MaxRefreshRate
        bitsPerPixel     = $a.CurrentBitsPerPixel
    }
}
$nvidia = $adapters | Where-Object { $_.name -match 'NVIDIA' } | Select-Object -First 1
if (-not $nvidia) { $warnings.Add('No NVIDIA adapter found.') }
elseif ($nvidia.name -notmatch '5070') { $warnings.Add("The NVIDIA adapter is '$($nvidia.name)', not an RTX 5070.") }
$active = $adapters | Where-Object { $_.refreshHz } | Sort-Object refreshHz -Descending | Select-Object -First 1
if (-not $active) { $warnings.Add('No active display mode was reported.') }
elseif ($active.refreshHz -lt $ExpectRefreshHz) {
    $warnings.Add("The display runs at $($active.refreshHz) Hz, expected $ExpectRefreshHz Hz. Set it under Settings > System > Display > Advanced display.")
}

# --- nvidia-smi, when the driver installed it
$smi = $null
$smiPath = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
if ($smiPath) {
    try { $smi = (& $smiPath.Source --query-gpu=name,driver_version,memory.total,pcie.link.gen.current,pcie.link.width.current --format=csv,noheader 2>$null | Select-Object -First 1) } catch { $smi = $null }
}

# --- monitors (names from EDID)
$monitors = @()
try {
    foreach ($m in Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorID -ErrorAction Stop) {
        $name = -join ($m.UserFriendlyName | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })
        $maker = -join ($m.ManufacturerName | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ })
        $monitors += [pscustomobject]@{ name = $name; manufacturer = $maker; active = $m.Active }
    }
} catch { $warnings.Add('Monitor names could not be read (WmiMonitorID).') }

# --- network
$netAdapters = @()
$defaultAlias = $null
try {
    $route = Get-NetRoute -DestinationPrefix '0.0.0.0/0' -ErrorAction Stop | Sort-Object RouteMetric, InterfaceMetric | Select-Object -First 1
    if ($route) { $defaultAlias = $route.InterfaceAlias }
    foreach ($n in Get-NetAdapter -Physical -ErrorAction Stop | Where-Object { $_.Status -eq 'Up' }) {
        $netAdapters += [pscustomobject]@{
            name        = $n.Name
            description = $n.InterfaceDescription
            linkSpeed   = $n.LinkSpeed
            media       = [string]$n.PhysicalMediaType
            defaultRoute = ($n.Name -eq $defaultAlias)
        }
    }
} catch { $warnings.Add('Network adapters could not be read.') }
$default = $netAdapters | Where-Object { $_.defaultRoute } | Select-Object -First 1
if ($default -and ($default.media -match 'Native 802.11|Wireless' -or $default.description -match 'Wi-Fi|Wireless|802\.11')) {
    $warnings.Add("The default route uses Wi-Fi ('$($default.description)'). Latency tests need wired Ethernet.")
}

# --- ping the PS5 when asked
$ping = $null
if ($Ps5Address) {
    try {
        $times = @()
        foreach ($r in Test-Connection -ComputerName $Ps5Address -Count 20 -ErrorAction Stop) {
            if ($null -ne $r.ResponseTime) { $times += [double]$r.ResponseTime }   # Windows PowerShell
            elseif ($null -ne $r.Latency) { $times += [double]$r.Latency }         # PowerShell 7
        }
        if ($times.Count -gt 0) {
            $sorted = $times | Sort-Object
            $ping = [pscustomobject]@{
                address = $Ps5Address; replies = $times.Count; sent = 20
                minMs = $sorted[0]; medianMs = $sorted[[int][math]::Floor(($sorted.Count - 1) / 2)]; maxMs = $sorted[$sorted.Count - 1]
            }
            if ($times.Count -lt 20) { $warnings.Add("Only $($times.Count) of 20 pings to the PS5 were answered.") }
        }
    } catch { $warnings.Add("The PS5 at $Ps5Address did not answer pings (it may be in rest mode or block ICMP).") }
}

# --- Windows settings that change presentation latency
$hwSch = Read-RegistryValue 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' 'HwSchMode'
$dxGlobal = Read-RegistryValue 'HKCU:\Software\Microsoft\DirectX\UserGpuPreferences' 'DirectXUserGlobalSettings'
$gameMode = Read-RegistryValue 'HKCU:\Software\Microsoft\GameBar' 'AutoGameModeEnabled'
$powerPlan = $null
try { $powerPlan = ((powercfg /getactivescheme) -join ' ').Trim() } catch { $powerPlan = $null }
$settings = [pscustomobject]@{
    hardwareGpuScheduling     = if ($hwSch -eq 2) { 'on' } elseif ($hwSch -eq 1) { 'off' } else { 'unknown' }
    directXUserGlobalSettings = $dxGlobal    # holds VRROptimizeEnable and SwapEffectUpgradeEnable when the user changed them
    gameMode                  = if ($gameMode -eq 0) { 'off' } elseif ($null -eq $gameMode) { 'default (on)' } else { 'on' }
    powerPlan                 = $powerPlan
}

$result = [pscustomobject]@{
    takenAt   = (Get-Date).ToString('s')
    computer  = $env:COMPUTERNAME
    windows   = "$($os.Caption) build $($os.BuildNumber)"
    cpu       = [pscustomobject]@{ name = $cpu.Name.Trim(); cores = $cpu.NumberOfCores; threads = $cpu.NumberOfLogicalProcessors }
    ramGb     = $ramGb
    adapters  = $adapters
    nvidiaSmi = $smi
    monitors  = $monitors
    network   = $netAdapters
    ps5Ping   = $ping
    settings  = $settings
    warnings  = $warnings
}
$result | ConvertTo-Json -Depth 6 | Set-Content -Path $OutFile -Encoding UTF8

Write-Host ''
Write-Host "CPU      $($result.cpu.name) ($($result.cpu.cores) cores, $($result.cpu.threads) threads)"
Write-Host "Memory   $ramGb GB"
foreach ($a in $adapters) {
    $driver = if ($a.nvidiaDriver) { "NVIDIA driver $($a.nvidiaDriver)" } else { "driver $($a.driverVersion)" }
    Write-Host "GPU      $($a.name), $driver, $($a.width)x$($a.height) at $($a.refreshHz) Hz"
}
foreach ($m in $monitors) { Write-Host "Monitor  $($m.manufacturer) $($m.name)" }
foreach ($n in $netAdapters) {
    $mark = if ($n.defaultRoute) { ' (default route)' } else { '' }
    Write-Host "Network  $($n.description), $($n.linkSpeed)$mark"
}
if ($ping) { Write-Host "PS5 ping median $($ping.medianMs) ms, worst $($ping.maxMs) ms ($($ping.replies)/20 answered)" }
Write-Host "Settings GPU scheduling $($settings.hardwareGpuScheduling); Game Mode $($settings.gameMode)"
Write-Host "         $powerPlan"
if ($warnings.Count -gt 0) {
    Write-Host ''
    foreach ($w in $warnings) { Write-Host "WARNING  $w" -ForegroundColor Yellow }
}
Write-Host ''
Write-Host "Saved to $OutFile"
