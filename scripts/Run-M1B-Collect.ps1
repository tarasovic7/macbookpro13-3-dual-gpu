<#
.SYNOPSIS
    Milestone 1B Windows verification & log collector for MacBookPro13,3 Intel-Only mode.
.DESCRIPTION
    Collects USB \M1B_LOGS\* telemetry, PnP GPU status (Intel HD 530 / AMD Polaris 11),
    active display mode / resolution / monitor ID, WMI brightness levels, and battery
    discharge rate (mW) into diag\m1b\.
#>
[CmdletBinding()]
param(
    [string]$Tag = "m1b_test",
    [string]$UsbDrive = "D:",
    [string]$OutDir = "diag\m1b"
)

$ErrorActionPreference = "Continue"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $OutDir "uefi") | Out-Null

$ts = Get-Date -Format "yyyyMMdd-HHmmss"
$safeTag = ($Tag -replace '[^a-zA-Z0-9_-]', '_')
$outFile = Join-Path $OutDir ("win_{0}_{1}.txt" -f $safeTag, $ts)

$sb = [System.Text.StringBuilder]::new()
[void]$sb.AppendLine("=== M1B WINDOWS VERIFICATION: $safeTag | $(Get-Date -Format o) ===")

# 1. USB M1B logs copy
$usbLogs = Join-Path $UsbDrive "M1B_LOGS"
if (Test-Path $usbLogs) {
    Copy-Item -Path (Join-Path $usbLogs "*") -Destination (Join-Path $OutDir "uefi") -Force -ErrorAction SilentlyContinue
    $copied = Get-ChildItem (Join-Path $OutDir "uefi") | ForEach-Object { "$($_.Name) ($($_.Length)B)" }
    [void]$sb.AppendLine("USB M1B_LOGS copied: $($copied -join ', ')")
} else {
    [void]$sb.AppendLine("USB M1B_LOGS not found at $usbLogs")
}
[void]$sb.AppendLine("")

# 2. PnP GPU / Display / GMUX / Backlight status
[void]$sb.AppendLine("--- PnP Devices (Display / GPU / Monitor / GMUX / Backlight) ---")
$patterns = @('*VEN_8086&DEV_191B*', '*VEN_1002&DEV_67EF*', '*VEN_8086&DEV_1901*', '*APP000B*', '*APP0002*', '*APPA030*')
$allDevs = Get-PnpDevice -ErrorAction SilentlyContinue
$matched = $allDevs | Where-Object {
    $id = $_.InstanceId
    $ok = $false
    foreach ($pat in $patterns) {
        if ($id -like $pat) { $ok = $true; break }
    }
    $ok -or $_.Class -eq 'Display' -or $_.Class -eq 'Monitor'
}

foreach ($d in ($matched | Sort-Object Class, FriendlyName)) {
    [void]$sb.AppendLine("[$($d.FriendlyName)] $($d.InstanceId)")
    [void]$sb.AppendLine("  Status=$($d.Status) Problem=$($d.Problem)")
    $props = Get-PnpDeviceProperty -InstanceId $d.InstanceId -ErrorAction SilentlyContinue |
        Where-Object { $_.KeyName -in @('DEVPKEY_Device_Service','DEVPKEY_Device_DriverVersion','DEVPKEY_Device_DriverDesc','DEVPKEY_Device_ProblemCode','DEVPKEY_Device_Stack','DEVPKEY_Device_LocationInfo','DEVPKEY_Device_BiosDeviceName') }
    foreach ($p in $props) {
        $val = if ($p.Data -is [array]) { $p.Data -join ', ' } else { "$($p.Data)" }
        [void]$sb.AppendLine("  $($p.KeyName) = $val")
    }
    [void]$sb.AppendLine("")
}

# 3. VideoController / Monitor Display Mode
[void]$sb.AppendLine("--- Win32_VideoController ---")
Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object {
    [void]$sb.AppendLine("  Name=$($_.Name) Status=$($_.Status) ErrCode=$($_.ConfigManagerErrorCode) Mode=$($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)@$($_.CurrentRefreshRate)Hz BPP=$($_.CurrentBitsPerPixel) Drv=$($_.DriverVersion)")
}
[void]$sb.AppendLine("")

# 4. WMI Brightness
[void]$sb.AppendLine("--- WMI Monitor Brightness ---")
$br = Get-CimInstance -Namespace root\wmi -ClassName WmiMonitorBrightness -ErrorAction SilentlyContinue
if ($br) {
    foreach ($b in $br) {
        [void]$sb.AppendLine("  Instance=$($b.InstanceName) Active=$($b.Active) Current=$($b.CurrentBrightness) Levels=$($b.Levels -join ',')")
    }
} else {
    [void]$sb.AppendLine("  WmiMonitorBrightness not exposed")
}
[void]$sb.AppendLine("")

# 5. Battery Discharge Rate (mW)
[void]$sb.AppendLine("--- Battery Power Status (mW) ---")
for ($sample = 1; $sample -le 3; $sample++) {
    $bat = Get-CimInstance -Namespace root\wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($bat) {
        [void]$sb.AppendLine("  Sample ${sample}: PowerOnline=$($bat.PowerOnline) Discharging=$($bat.Discharging) DischargeRate_mW=$($bat.DischargeRate) ChargeRate_mW=$($bat.ChargeRate) Voltage_mV=$($bat.Voltage) Remaining_mWh=$($bat.RemainingCapacity)")
    } else {
        [void]$sb.AppendLine("  Sample ${sample}: BatteryStatus unavailable")
    }
    if ($sample -lt 3) { Start-Sleep -Seconds 2 }
}

$text = $sb.ToString()
[System.IO.File]::WriteAllText($outFile, $text, [System.Text.Encoding]::UTF8)
Write-Host $text
Write-Host "Saved report to: $outFile" -ForegroundColor Green
