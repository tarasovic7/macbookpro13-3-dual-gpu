<#
.SYNOPSIS
    Milestone 1A Windows collection, Intel HD 530 isolation test, and UEFI probe analyzer
    for MacBookPro13,3 (AMD Radeon Pro 455 Polaris 11 SMU7 vs AppleSetOs / igdkmd64).
.PARAMETER Action
    Menu | Collect | TestHotDisableIntel | DisableIntelForReboot | EnableIntel | Analyze
.PARAMETER UsbDrive
    Optional USB drive letter (e.g. D). Auto-detects volume labeled REFIND_USB if omitted.
#>
param(
    [ValidateSet('Menu', 'Collect', 'TestHotDisableIntel', 'DisableIntelForReboot', 'EnableIntel', 'Analyze')]
    [string]$Action = 'Menu',
    [string]$UsbDrive = ''
)

$ErrorActionPreference = 'Continue'

function Ensure-Admin {
    $isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        Write-Host 'Requiring Administrator privileges (UAC)...' -ForegroundColor Yellow
        $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -Action $Action"
        if ($UsbDrive) { $argList += " -UsbDrive `"$UsbDrive`"" }
        Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs
        exit 0
    }
}

function Resolve-UsbRoot {
    param([string]$Preferred)
    if ($Preferred) {
        $dl = $Preferred.TrimEnd(':', '\')
        if (Test-Path "${dl}:\") { return "${dl}:" }
    }
    $vol = Get-Volume -ErrorAction SilentlyContinue | Where-Object { $_.FileSystemLabel -eq 'REFIND_USB' -and $_.DriveLetter } | Select-Object -First 1
    if ($vol) { return "$($vol.DriveLetter):" }
    if (Test-Path 'D:\M1A_LOGS') { return 'D:' }
    return $null
}

function Resolve-WorkspacePaths {
    $repoCandidate = Split-Path -Parent $PSScriptRoot
    $isRepo = (Test-Path (Join-Path $repoCandidate 'README.md')) -and (Test-Path (Join-Path $repoCandidate 'scripts\Parse-M1A-Probe.py'))
    if ($isRepo) {
        $diagRoot = Join-Path $repoCandidate 'diag\m1a'
        $parserPy = Join-Path $repoCandidate 'scripts\Parse-M1A-Probe.py'
    } else {
        $diagRoot = 'C:\Temp\HackDiag\m1a'
        $parserPy = Join-Path $PSScriptRoot 'Parse-M1A-Probe.py'
    }
    return [PSCustomObject]@{
        IsRepo   = $isRepo
        RepoRoot = if ($isRepo) { $repoCandidate } else { $null }
        DiagRoot = $diagRoot
        UefiDir  = Join-Path $diagRoot 'uefi'
        ParserPy = $parserPy
    }
}

function Get-GpuSnapshotText {
    param([string]$Label)
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("=== M1A WINDOWS SNAPSHOT: $Label | $(Get-Date -Format o) ===")

    $intelDev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_8086&DEV_191B*' } | Select-Object -First 1
    $amdDev   = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_67EF*' } | Select-Object -First 1
    $bootMode = if ($intelDev) { 'setos' } else { 'normal' }
    $intelState = if (-not $intelDev) { 'absent' } elseif ($intelDev.ConfigManagerErrorCode -eq 22) { 'disabled(22)' } else { "$($intelDev.Status)($($intelDev.ConfigManagerErrorCode))" }
    $amdState = if (-not $amdDev) { 'absent' } else { "$($amdDev.Status)($($amdDev.ConfigManagerErrorCode))" }
    [void]$sb.AppendLine("DetectedBootMode=$bootMode | Intel=$intelState | AMD=$amdState")

    [void]$sb.AppendLine("`n--- PnP Devices (Display / PEG0 / GMUX / Backlight / Monitor) ---")
    $devs = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
        $_.InstanceId -match 'VEN_1002|VEN_8086&DEV_191B|VEN_8086&DEV_1901|APP000B|APP0002|APPA030'
    }
    foreach ($d in $devs) {
        [void]$sb.AppendLine("`n[$($d.FriendlyName)] $($d.InstanceId)")
        [void]$sb.AppendLine("  Status=$($d.Status) Problem=$($d.Problem) CMError=$($d.ConfigManagerErrorCode)")
        $props = Get-PnpDeviceProperty -InstanceId $d.InstanceId -ErrorAction SilentlyContinue | Where-Object {
            $_.KeyName -match 'LocationInfo|BiosDeviceName|PowerData|Service|Driver|Stack|Problem|HardwareIds|BusNumber|Address|DevNodeStatus'
        }
        foreach ($p in $props) {
            $val = if ($p.Data -is [byte[]]) { ($p.Data | ForEach-Object { '{0:X2}' -f $_ }) -join ' ' }
                   elseif ($p.Data -is [Array]) { $p.Data -join ', ' }
                   else { "$($p.Data)" }
            [void]$sb.AppendLine("  $($p.KeyName) = $val")
        }
    }

    [void]$sb.AppendLine("`n--- Allocated Resources ---")
    Get-CimInstance Win32_AllocatedResource -ErrorAction SilentlyContinue | Where-Object {
        $_.Dependent.DeviceID -match 'VEN_1002|VEN_8086&DEV_191B|VEN_8086&DEV_1901|APP000B'
    } | ForEach-Object {
        [void]$sb.AppendLine("  $($_.Dependent.DeviceID) -> $($_.Antecedent)")
    }

    [void]$sb.AppendLine("`n--- VBS / HVCI / Kernel DMA Protection ---")
    $dg = Get-CimInstance -ClassName Win32_DeviceGuard -Namespace root\Microsoft\Windows\DeviceGuard -ErrorAction SilentlyContinue
    if ($dg) {
        [void]$sb.AppendLine("  VbsStatus=$($dg.VirtualizationBasedSecurityStatus) SecurityServicesConfigured=$($dg.SecurityServicesConfigured -join ',') SecurityServicesRunning=$($dg.SecurityServicesRunning -join ',')")
    }
    $hvci = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity' -ErrorAction SilentlyContinue
    if ($hvci) { [void]$sb.AppendLine("  HVCI Enabled=$($hvci.Enabled) WasEnabledBy=$($hvci.WasEnabledBy)") }
    $dma = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\DmaSecurity' -ErrorAction SilentlyContinue
    if ($dma) { [void]$sb.AppendLine("  DmaSecurity: $($dma | Out-String)") }

    [void]$sb.AppendLine("`n--- Display Class Registry Keys ---")
    Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}' -ErrorAction SilentlyContinue |
        Where-Object { $_.PSChildName -match '^\d{4}$' } | ForEach-Object {
            $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
            [void]$sb.AppendLine("  $($_.PSChildName): Desc=$($p.DriverDesc) | Ver=$($p.DriverVersion) | Match=$($p.MatchingDeviceId) | ULPS=$($p.EnableUlps) | BC=$($p.PP_Apple_Bootcamp_Enable)")
        }

    [void]$sb.AppendLine("`n--- System Events Since Boot (Display/PCI/PnP/DxgKrnl/AMD/Intel) ---")
    $bootTime = (Get-CimInstance Win32_OperatingSystem -ErrorAction SilentlyContinue).LastBootUpTime
    if (-not $bootTime) { $bootTime = (Get-Date).AddMinutes(-15) }
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $bootTime } -ErrorAction SilentlyContinue |
        Where-Object { $_.ProviderName -match 'amduw|amdkmdag|igfx|Display|Kernel-PnP|PCI|DxgKrnl|BugCheck|WHEA|BasicDisplay|ACPI' } |
        Select-Object -First 80 TimeCreated, ProviderName, Id, LevelDisplayName, Message |
        ForEach-Object {
            $msg = ($_.Message -replace '\s+', ' ').Trim()
            if ($msg.Length -gt 240) { $msg = $msg.Substring(0, 240) + '...' }
            [void]$sb.AppendLine("  [$($_.TimeCreated.ToString('HH:mm:ss.fff'))] $($_.ProviderName) #$($_.Id) ($($_.LevelDisplayName)): $msg")
        }

    return $sb.ToString()
}

function Invoke-Collect {
    param($Paths, $UsbRoot)
    New-Item -ItemType Directory -Force $Paths.DiagRoot, $Paths.UefiDir | Out-Null

    if ($UsbRoot -and (Test-Path "$UsbRoot\M1A_LOGS")) {
        $uefiLogs = @(Get-ChildItem "$UsbRoot\M1A_LOGS\*.txt" -ErrorAction SilentlyContinue)
        foreach ($f in $uefiLogs) {
            Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $Paths.UefiDir $f.Name) -Force
        }
        Write-Host "[1/4] Copied $($uefiLogs.Count) UEFI probe log(s) from $UsbRoot\M1A_LOGS -> $($Paths.UefiDir)" -ForegroundColor Green
    } else {
        Write-Warning "[1/4] No USB M1A_LOGS directory found (UsbRoot=$UsbRoot)."
    }

    $intelDev = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_8086&DEV_191B*' } | Select-Object -First 1
    $modeTag = if (-not $intelDev) { 'normal' } elseif ($intelDev.ConfigManagerErrorCode -eq 22) { 'setos_intel_disabled' } else { 'setos' }
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $winFile = Join-Path $Paths.DiagRoot "win_${modeTag}_${stamp}.txt"
    $snapText = Get-GpuSnapshotText -Label $modeTag
    [System.IO.File]::WriteAllText($winFile, $snapText, [System.Text.Encoding]::UTF8)
    Write-Host "[2/4] Saved Windows telemetry -> $winFile" -ForegroundColor Green

    # Copy any new WATCHDOG dumps (after Phase 0 baseline 13:34) & latest Kernel_1b0 Report.wer
    $cutoff = Get-Date '2026-11-25 13:34:00'
    $dmps = @(Get-ChildItem 'C:\Windows\LiveKernelReports\WATCHDOG\WATCHDOG-*.dmp' -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $cutoff })
    foreach ($d in $dmps) {
        Copy-Item -LiteralPath $d.FullName -Destination (Join-Path $Paths.DiagRoot $d.Name) -Force
    }
    $wer = Get-ChildItem 'C:\ProgramData\Microsoft\Windows\WER\ReportQueue\Kernel_1b0*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($wer -and (Test-Path (Join-Path $wer.FullName 'Report.wer'))) {
        Copy-Item -LiteralPath (Join-Path $wer.FullName 'Report.wer') -Destination (Join-Path $Paths.DiagRoot "Report_1b0_${modeTag}.wer") -Force
    }
    Write-Host "[3/4] Checked LiveKernelReports\WATCHDOG ($($dmps.Count) new dump(s) copied)" -ForegroundColor Green

    Invoke-Analyze -Paths $Paths
}

function Invoke-Analyze {
    param($Paths)
    if (Test-Path $Paths.ParserPy) {
        $py = Get-Command python -ErrorAction SilentlyContinue
        if ($py) {
            Write-Host "[4/4] Running Parse-M1A-Probe.py..." -ForegroundColor Cyan
            & $py.Source $Paths.ParserPy --input-dir $Paths.UefiDir --output (Join-Path $Paths.DiagRoot 'M1A_ANALYSIS.md')
        } else {
            Write-Warning "python not found in PATH; skipping Parse-M1A-Probe.py"
        }
    }
}

function Invoke-TestHotDisableIntel {
    param($Paths)
    $intel = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_8086&DEV_191B*' } | Select-Object -First 1
    $amd   = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_1002&DEV_67EF*' } | Select-Object -First 1
    if (-not $intel) {
        Write-Warning "Intel HD 530 (00:02.0) is not present in this boot. Boot via AppleSetOs first!"
        return
    }
    if (-not $amd) {
        Write-Warning "AMD Radeon Pro 455 (01:00.0) is not present!"
        return
    }

    # Safety check: make sure DISPLAY\APPA030 is not solely attached to igfx
    $mon = Get-PnpDevice -Class Monitor -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'DISPLAY\APPA030*' } | Select-Object -First 1
    if ($mon) {
        $stack = (Get-PnpDeviceProperty -InstanceId $mon.InstanceId -KeyName 'DEVPKEY_Device_Stack' -ErrorAction SilentlyContinue).Data -join ','
        if ($stack -match 'igfx') {
            Write-Warning "APPA030 appears attached to igfx ($stack). Aborting hot-disable to avoid blank screen."
            return
        }
    }

    New-Item -ItemType Directory -Force $Paths.DiagRoot | Out-Null
    $outFile = Join-Path $Paths.DiagRoot 'hot_disable_intel_result.txt'
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("=== Hot-Disable Intel Isolation Test | $(Get-Date -Format o) ===")
    [void]$sb.AppendLine("Before: Intel=$($intel.Status)/$($intel.ConfigManagerErrorCode) AMD=$($amd.Status)/$($amd.ConfigManagerErrorCode)")

    Write-Host "Disabling Intel HD 530 ($($intel.InstanceId))..." -ForegroundColor Yellow
    $r1 = & pnputil.exe /disable-device $intel.InstanceId 2>&1
    [void]$sb.AppendLine("pnputil /disable-device Intel: $($r1 -join ' ')")
    Start-Sleep -Seconds 2

    Write-Host "Restarting AMD Radeon Pro 455 ($($amd.InstanceId))..." -ForegroundColor Cyan
    $r2 = & pnputil.exe /restart-device $amd.InstanceId 2>&1
    [void]$sb.AppendLine("pnputil /restart-device AMD: $($r2 -join ' ')")
    Start-Sleep -Seconds 5

    $amdAfter = Get-PnpDevice -InstanceId $amd.InstanceId -ErrorAction SilentlyContinue
    Write-Host "AMD after Intel hot-disable: Status=$($amdAfter.Status) Problem=$($amdAfter.Problem) CMError=$($amdAfter.ConfigManagerErrorCode)" -ForegroundColor $(if ($amdAfter.Status -eq 'OK') { 'Green' } else { 'Red' })
    [void]$sb.AppendLine("After Hot-Disable: AMD=$($amdAfter.Status) Problem=$($amdAfter.Problem) CMError=$($amdAfter.ConfigManagerErrorCode)")

    [void]$sb.AppendLine("`n" + (Get-GpuSnapshotText -Label 'hot_disable_intel'))

    Write-Host "Re-enabling Intel HD 530..." -ForegroundColor Yellow
    $r3 = & pnputil.exe /enable-device $intel.InstanceId 2>&1
    [void]$sb.AppendLine("pnputil /enable-device Intel: $($r3 -join ' ')")

    [System.IO.File]::WriteAllText($outFile, $sb.ToString(), [System.Text.Encoding]::UTF8)
    Write-Host "Saved isolation result -> $outFile" -ForegroundColor Green
}

function Set-IntelEnabledState {
    param([bool]$Enable)
    $intel = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_8086&DEV_191B*' } | Select-Object -First 1
    if (-not $intel) {
        Write-Warning "Intel HD 530 (PCI\VEN_8086&DEV_191B*) is not present in this boot. Run this in a setos boot."
        return
    }
    if ($Enable) {
        Write-Host "Enabling Intel HD 530 ($($intel.InstanceId))..." -ForegroundColor Green
        & pnputil.exe /enable-device $intel.InstanceId
    } else {
        Write-Host "Disabling Intel HD 530 ($($intel.InstanceId)) for next reboot (igdkmd64.sys will not load at boot)..." -ForegroundColor Yellow
        & pnputil.exe /disable-device $intel.InstanceId
    }
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match 'VEN_8086&DEV_191B|VEN_1002&DEV_67EF' } |
        Format-Table Status, ConfigManagerErrorCode, FriendlyName, InstanceId -AutoSize
}

if ($Action -ne 'Analyze') { Ensure-Admin }

$paths = Resolve-WorkspacePaths
$usbRoot = Resolve-UsbRoot -Preferred $UsbDrive

switch ($Action) {
    'Collect'               { Invoke-Collect -Paths $paths -UsbRoot $usbRoot }
    'TestHotDisableIntel'   { Invoke-TestHotDisableIntel -Paths $paths }
    'DisableIntelForReboot' { Set-IntelEnabledState -Enable:$false }
    'EnableIntel'           { Set-IntelEnabledState -Enable:$true }
    'Analyze'               { Invoke-Analyze -Paths $paths }
    'Menu' {
        Write-Host "============================================================" -ForegroundColor Cyan
        Write-Host " MacBookPro13,3 Milestone 1A Data Collection & Isolation" -ForegroundColor Cyan
        Write-Host " USB: $usbRoot | Diag: $($paths.DiagRoot)" -ForegroundColor Cyan
        Write-Host "============================================================" -ForegroundColor Cyan
        Write-Host " [1] Collect UEFI logs + Windows GPU/Event/Dump state + Analyze"
        Write-Host " [2] Test Hot-Disable Intel HD 530 + Restart AMD (in setos boot)"
        Write-Host " [3] Disable Intel HD 530 for boot-time isolation test (reboot after)"
        Write-Host " [4] Re-enable Intel HD 530"
        Write-Host " [5] Run Parse-M1A-Probe.py analysis only"
        Write-Host " [0] Exit"
        $ch = Read-Host "`nSelect"
        switch ($ch) {
            '1' { Invoke-Collect -Paths $paths -UsbRoot $usbRoot }
            '2' { Invoke-TestHotDisableIntel -Paths $paths }
            '3' { Set-IntelEnabledState -Enable:$false }
            '4' { Set-IntelEnabledState -Enable:$true }
            '5' { Invoke-Analyze -Paths $paths }
        }
        Pause
    }
}
