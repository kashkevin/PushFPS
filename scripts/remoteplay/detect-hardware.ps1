<#
.SYNOPSIS
  Records the hardware and settings that matter for PushFPS latency tests, and warns about the
  ones that would spoil a benchmark.

.DESCRIPTION
  Reads CPU, memory, graphics adapters and driver, the current display mode, network adapters,
  the default route, and a few Windows settings (GPU scheduling, variable refresh rate, Game Mode,
  power plan). Writes a JSON file and prints a summary. Changes nothing on the PC.

  For the hardware compatibility matrix (docs/HARDWARE_COMPATIBILITY.md) it also records the vendor class of
  every adapter (from the PCI vendor id), the driver version and date, video memory, the Windows build, the
  power source, and with -DecodeProfiles what dxdiag reports (decode profiles, feature levels, HDR / colour
  space). No host name, user name or serial number is written: "computer" is kept as a key for older readers
  but holds "(not recorded)".

  Windows PowerShell 5.1 or newer. No administrator rights needed.

.PARAMETER OutFile
  Where to write the JSON. Default: hardware.json beside this script's caller (current folder).

.PARAMETER Ps5Address
  Optional IP address of the PS5. When given, 20 pings are sent and the round-trip times recorded.

.PARAMETER ExpectRefreshHz
  Refresh rate the main display should be running at. Default 180.

.PARAMETER DecodeProfiles
  Also run dxdiag (about 10 to 40 seconds) and keep, per display device, only its DXVA2 decode modes, feature
  levels, memory, driver and HDR / colour-space fields. The dxdiag file itself is deleted.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File detect-hardware.ps1 -Ps5Address 192.168.1.50
#>
param(
    [string]$OutFile = (Join-Path (Get-Location) 'hardware.json'),
    [string]$Ps5Address = '',
    [int]$ExpectRefreshHz = 180,
    [switch]$DecodeProfiles
)
$ErrorActionPreference = 'Continue'
$warnings = New-Object System.Collections.Generic.List[string]

function Read-RegistryValue([string]$Path, [string]$Name) {
    try { return (Get-ItemProperty -Path $Path -Name $Name -ErrorAction Stop).$Name } catch { return $null }
}
# Vendor class from a PCI vendor id ("PCI\VEN_10DE&..."): what the application decides by, never the model name.
function Get-VendorClass([string]$PnpId) {
    if (-not $PnpId) { return 'unknown' }
    if ($PnpId -match 'VEN_10DE') { return 'nvidia' }
    if ($PnpId -match 'VEN_1002|VEN_1022') { return 'amd' }
    if ($PnpId -match 'VEN_8086') { return 'intel' }
    if ($PnpId -match 'VEN_1414' -or $PnpId -match '^ROOT\\') { return 'microsoft-software' }
    return 'other'
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
        vendorClass      = Get-VendorClass ([string]$a.PNPDeviceID)
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
            name        = '(not recorded)'
            description = $n.InterfaceDescription
            linkSpeed   = $n.LinkSpeed
            media       = [string]$n.PhysicalMediaType
            defaultRoute = ($n.Name -eq $defaultAlias)   # compared here; the alias itself is not written
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

# --- what the compatibility matrix needs: video memory, Windows build, power, CPU vendor, hybrid graphics
# Video memory from the display class keys (64-bit qwMemorySize; Win32_VideoController.AdapterRAM stops at 4 GB).
$videoMemory = @()
try {
    $classKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
    foreach ($k in Get-ChildItem -Path $classKey -ErrorAction SilentlyContinue) {
        if ($k.PSChildName -notmatch '^[0-9]{4}$') { continue }
        $p = Get-ItemProperty -Path $k.PSPath -ErrorAction SilentlyContinue
        if (-not $p -or -not $p.DriverDesc) { continue }
        $bytes = $null
        $q = $p.'HardwareInformation.qwMemorySize'
        if ($null -ne $q) { $bytes = [double]$q }
        else {
            $m = $p.'HardwareInformation.MemorySize'
            if ($m -is [byte[]] -and $m.Length -ge 4) { $bytes = [double][BitConverter]::ToUInt32($m, 0) }
            elseif ($null -ne $m) { $bytes = [double]$m }
        }
        $videoMemory += [pscustomobject]@{
            name          = [string]$p.DriverDesc
            vendorClass   = Get-VendorClass ([string]$p.MatchingDeviceId)
            dedicatedMB   = if ($null -ne $bytes) { [math]::Round($bytes / 1MB) } else { $null }
            driverVersion = [string]$p.DriverVersion
            driverDate    = [string]$p.DriverDate
        }
    }
} catch { $warnings.Add('Video memory could not be read from the registry.') }
$hardwareClasses = @($adapters | Where-Object { $_.vendorClass -ne 'microsoft-software' } | ForEach-Object { $_.vendorClass })
$build = Read-RegistryValue 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' 'UBR'
$displayVersion = Read-RegistryValue 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' 'DisplayVersion'
$power = 'unknown'
try {
    $batteries = @(Get-CimInstance Win32_Battery -ErrorAction Stop)
    if ($batteries.Count -eq 0) { $power = 'mains (no battery)' }
    # Win32_Battery.BatteryStatus: 1 discharging, 2 on AC, 6-9 charging; anything else says too little.
    elseif ($batteries | Where-Object { $_.BatteryStatus -eq 1 }) { $power = 'battery' }
    elseif ($batteries | Where-Object { $_.BatteryStatus -eq 2 -or ($_.BatteryStatus -ge 6 -and $_.BatteryStatus -le 9) }) { $power = 'mains' }
} catch { $power = 'unknown' }
if ($power -eq 'battery') { $warnings.Add('The PC runs on battery: Windows and the GPU driver may lower clocks; plug it in for tests.') }
$dxVrr = 'unknown'
$dxGlobalText = [string](Read-RegistryValue 'HKCU:\Software\Microsoft\DirectX\UserGpuPreferences' 'DirectXUserGlobalSettings')
if ($dxGlobalText -match 'VRROptimizeEnable=1') { $dxVrr = 'on' } elseif ($dxGlobalText -match 'VRROptimizeEnable=0') { $dxVrr = 'off' } else { $dxVrr = 'default' }
$cpuVendor = if ($cpu.Manufacturer -match 'AMD') { 'amd' } elseif ($cpu.Manufacturer -match 'Intel') { 'intel' } else { 'other' }

# dxdiag, only when asked: decode profiles, feature levels and HDR as the driver reports them.
$dxdiag = @()
if ($DecodeProfiles) {
    $dxFile = Join-Path $env:TEMP ('pushfps-dxdiag-' + [guid]::NewGuid().ToString('N') + '.xml')
    try {
        # At most 90 s: a dxdiag that hangs is stopped and the report goes on without decode modes.
        $dxProcess = Start-Process -FilePath 'dxdiag.exe' -ArgumentList @('/whql:off', '/x', ('"' + $dxFile + '"')) -WindowStyle Hidden -PassThru -ErrorAction Stop
        $dxDone = $dxProcess.WaitForExit(90000)
        if (-not $dxDone) {
            Stop-Process -Id $dxProcess.Id -Force -ErrorAction SilentlyContinue
            $warnings.Add('dxdiag did not finish within 90 seconds and was stopped: no decode modes recorded.')
        }
        for ($i = 0; $dxDone -and $i -lt 30 -and -not (Test-Path -LiteralPath $dxFile); $i++) { Start-Sleep -Seconds 1 }
        if ($dxDone -and (Test-Path -LiteralPath $dxFile)) {
            Start-Sleep -Seconds 1
            $xml = [xml](Get-Content -LiteralPath $dxFile -Raw)
            foreach ($d in @($xml.DxDiag.DisplayDevices.DisplayDevice)) {
                if (-not $d) { continue }
                $extra = [ordered]@{}
                $modes = ([string]$d.DXVA2Modes).Trim()
                foreach ($node in $d.ChildNodes) {
                    if ($node.Name -match 'HDR|ColorSpace|AdvancedColor|Refresh|VRR|Variable') { $extra[$node.Name] = $node.InnerText }
                }
                $dxdiag += [pscustomobject]@{
                    cardName        = [string]$d.CardName
                    manufacturer    = [string]$d.Manufacturer
                    dedicatedMemory = [string]$d.DedicatedMemory
                    featureLevels   = [string]$d.FeatureLevels
                    driverModel     = [string]$d.DriverModel
                    driverVersion   = [string]$d.DriverVersion
                    driverDate      = [string]$d.DriverDate
                    currentMode     = [string]$d.CurrentMode
                    dxva2Modes      = [string]$d.DXVA2Modes
                    # null = unknown when dxdiag listed no modes for this device (never "false" from nothing)
                    hevcDecode      = if ($modes) { $modes -match 'HEVC_VLD_Main(\s|$)' } else { $null }
                    hevc10Decode    = if ($modes) { $modes -match 'HEVC_VLD_Main10' } else { $null }
                    h264Decode      = if ($modes) { $modes -match 'H264_VLD' } else { $null }
                    display         = [pscustomobject]$extra
                }
            }
        } elseif ($dxDone) { $warnings.Add('dxdiag finished but wrote no file.') }
    } catch { $warnings.Add('dxdiag could not be run or read.') }
    finally { Remove-Item -LiteralPath $dxFile -Force -ErrorAction SilentlyContinue }
}
$compatibility = [pscustomobject]@{
    gpuVendorClasses = $hardwareClasses
    hybridGraphics   = (@($hardwareClasses | Sort-Object -Unique).Count -gt 1)
    videoMemory      = $videoMemory
    cpuVendorClass   = $cpuVendor
    windowsBuild     = if ($null -ne $build) { "$($os.BuildNumber).$build" } else { [string]$os.BuildNumber }
    windowsVersion   = [string]$displayVersion
    powerSource      = $power
    windowedVrr      = $dxVrr
    dxdiag           = $dxdiag
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
    computer  = '(not recorded)'
    windows   = "$($os.Caption) build $($os.BuildNumber)"
    cpu       = [pscustomobject]@{ name = $cpu.Name.Trim(); cores = $cpu.NumberOfCores; threads = $cpu.NumberOfLogicalProcessors }
    ramGb     = $ramGb
    adapters  = $adapters
    nvidiaSmi = $smi
    monitors  = $monitors
    network   = $netAdapters
    ps5Ping   = $ping
    settings  = $settings
    compatibility = $compatibility
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
Write-Host "Matrix   GPU classes $($hardwareClasses -join ', '); hybrid $($compatibility.hybridGraphics); CPU $cpuVendor; power $power; Windows build $($compatibility.windowsBuild)"
foreach ($v in $videoMemory) { Write-Host "         $($v.vendorClass) $($v.dedicatedMB) MB video memory, driver $($v.driverVersion) ($($v.driverDate))" }
foreach ($d in $dxdiag) { Write-Host "         dxdiag: H.264 $($d.h264Decode), HEVC $($d.hevcDecode), HEVC 10-bit $($d.hevc10Decode); feature levels $($d.featureLevels)" }
Write-Host "Settings GPU scheduling $($settings.hardwareGpuScheduling); Game Mode $($settings.gameMode)"
Write-Host "         $powerPlan"
if ($warnings.Count -gt 0) {
    Write-Host ''
    foreach ($w in $warnings) { Write-Host "WARNING  $w" -ForegroundColor Yellow }
}
Write-Host ''
Write-Host "Saved to $OutFile"
