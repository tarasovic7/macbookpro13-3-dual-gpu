<#
.SYNOPSIS
    Faza 0 diagnostika pre MacBookPro13,3 Dual-GPU Hack:
    1) Skopiruje najnovsi WATCHDOG-*.dmp do C:\Temp\HackDiag\
    2) Spusti WinDbg/cdbX64 !analyze -v + call stack na dump (ulozi windbg_1b0_report.txt)
    3) Zaznamena ACPI tabulky (acpidump -b + iasl -d) do acpi_setos alebo acpi_normal podla aktualneho bootu
    4) Zaznamena stav PCI/PnP/IRQ/Resource mapy oboch GPU do pci_state_<mode>.txt
#>
$ErrorActionPreference = 'Continue'

# 1. Elevacia na Spravcu (potrebne pre citanie LiveKernelReports\WATCHDOG a kompletny acpidump)
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host 'Pozadujem opravnenia Spravcu (UAC)...' -ForegroundColor Yellow
    Start-Process -FilePath 'powershell.exe' -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit 0
}

$diagDir   = 'C:\Temp\HackDiag'
$acpicaDir = "$diagDir\acpica"
New-Item -ItemType Directory -Force $diagDir | Out-Null

# Zisti aktualny rezim bootu (setos = Intel HD 530 je pritomny, normal = iba AMD)
$hasIntel = [bool](Get-PnpDevice -Class Display -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\VEN_8086*' })
$mode = if ($hasIntel) { 'setos' } else { 'normal' }

Write-Host "=============================================================" -ForegroundColor Cyan
Write-Host " MacBookPro13,3 Faza 0 Diagnostika | Aktualny boot: $mode" -ForegroundColor Cyan
Write-Host "=============================================================" -ForegroundColor Cyan

# 2. Kopia najnovsieho WATCHDOG dumpu + WER reportu
Write-Host "`n[1/4] Kopirujem najnovsi WATCHDOG kernel dump a WER report..." -ForegroundColor Cyan
$dmp = Get-ChildItem 'C:\Windows\LiveKernelReports\WATCHDOG\WATCHDOG-*.dmp' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$localDmp = $null
if ($dmp) {
    $localDmp = Join-Path $diagDir $dmp.Name
    Copy-Item -LiteralPath $dmp.FullName -Destination $localDmp -Force
    Write-Host "  OK -> $localDmp ($([math]::Round($dmp.Length/1MB,2)) MB)" -ForegroundColor Green
} else {
    Write-Warning 'Nenasiel sa ziadny WATCHDOG-*.dmp v C:\Windows\LiveKernelReports\WATCHDOG\'
}

$wer = Get-ChildItem 'C:\ProgramData\Microsoft\Windows\WER\ReportQueue\Kernel_1b0*' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($wer -and (Test-Path (Join-Path $wer.FullName 'Report.wer'))) {
    Copy-Item -LiteralPath (Join-Path $wer.FullName 'Report.wer') -Destination (Join-Path $diagDir 'Report_1b0.wer') -Force
    Write-Host "  OK -> $diagDir\Report_1b0.wer" -ForegroundColor Green
}

# 3. Automaticka analyza dumpu cez WinDbg/kd.exe (ak je WinDbg nainstalovany a report este neexistuje)
$reportTxt = Join-Path $diagDir 'kd_1b0_report.txt'
if ($localDmp -and -not (Test-Path $reportTxt)) {
    Write-Host "`n[2/4] Spustam WinDbg (kd.exe) !analyze -v na $($dmp.Name)..." -ForegroundColor Cyan
    $kd = $null
    $pkg = Get-AppxPackage -Name 'Microsoft.WinDbg' -AllUsers -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $pkg) { $pkg = Get-AppxPackage -Name 'Microsoft.WinDbg' -ErrorAction SilentlyContinue | Select-Object -First 1 }
    if ($pkg) {
        $cand = Join-Path $pkg.InstallLocation 'amd64\kd.exe'
        if (Test-Path $cand) { $kd = $cand }
    }
    if (-not $kd) {
        $kd = Get-ChildItem 'C:\Program Files\WindowsApps\Microsoft.WinDbg_*\amd64\kd.exe' -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $kd) {
        $kd = @(
            'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\kd.exe'
        ) | Where-Object { Test-Path $_ } | Select-Object -First 1
    }

    if ($kd) {
        $symDir = "$diagDir\symbols"
        New-Item -ItemType Directory -Force $symDir | Out-Null
        Write-Host "  Pouzivam: $kd (stahovanie MS symbolov moze trvat 20-60 sekund)..." -ForegroundColor Yellow
        $cmd = "!analyze -v; .echo === STACK ===; kP 50; .echo === MODULES ===; lmvm amduw23g; lmvm dxgkrnl; lmvm igdkmd64; lmvm pci; q"
        & $kd -z $localDmp -y "srv*$symDir*https://msdl.microsoft.com/download/symbols" -logo $reportTxt -c $cmd 2>&1 | Out-Null
        Write-Host "  OK -> $reportTxt" -ForegroundColor Green
    } else {
        Write-Warning "kd.exe sa nenasiel. Dump je skopirovany v $localDmp (mozes ho otvorit rucne vo WinDbg)."
    }
} else {
    Write-Host "`n[2/4] WinDbg report uz existuje alebo dump chyba (preskakujem)." -ForegroundColor DarkGray
}

# 4. ACPI dump + dekompilacia pre aktualny rezim bootu ($mode)
Write-Host "`n[3/4] Ukladam ACPI tabulky pre rezim '$mode'..." -ForegroundColor Cyan
$acpiOut = Join-Path $diagDir "acpi_$mode"
Remove-Item -LiteralPath $acpiOut -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $acpiOut | Out-Null
Push-Location $acpiOut
& "$acpicaDir\acpidump.exe" -b 2>&1 | Out-File (Join-Path $acpiOut 'acpidump_log.txt') -Encoding UTF8
$amlTables = @(Get-ChildItem $acpiOut -Filter '*.dat' | Where-Object { $_.Name -match '^(dsdt|ssdt)' })
foreach ($t in $amlTables) {
    & "$acpicaDir\iasl.exe" -d $t.Name 2>&1 | Out-Null
}
Pop-Location
$dslCount = (Get-ChildItem $acpiOut -Filter '*.dsl' -ErrorAction SilentlyContinue).Count
Write-Host "  OK -> $acpiOut ($($amlTables.Count) .dat, $dslCount .dsl)" -ForegroundColor Green

# 5. Kompletný snímok PCI / PnP / Power / Resource stavu pre aktualny boot
Write-Host "`n[4/4] Ukladam PCI/PnP/Resource stav pre rezim '$mode'..." -ForegroundColor Cyan
$pciOut = Join-Path $diagDir "pci_state_$mode.txt"
$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("=== BOOT MODE: $mode | $(Get-Date -Format o) ===")
$devs = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object {
    $_.InstanceId -match 'VEN_1002|VEN_8086&DEV_191B|VEN_8086&DEV_1901|APP000B|APP0002|APPA030'
}
foreach ($d in $devs) {
    [void]$sb.AppendLine("`n--- $($d.FriendlyName) [$($d.InstanceId)] ---")
    [void]$sb.AppendLine("Status=$($d.Status) Problem=$($d.Problem) ProblemCode=$($d.ProblemCode)")
    $props = Get-PnpDeviceProperty -InstanceId $d.InstanceId -ErrorAction SilentlyContinue | Where-Object {
        $_.KeyName -match 'LocationInfo|BiosDeviceName|PowerData|Service|Driver|Stack|Problem|HardwareIds|BusNumber|Address'
    }
    foreach ($p in $props) {
        $val = if ($p.Data -is [byte[]]) { ($p.Data | ForEach-Object { '{0:X2}' -f $_ }) -join ' ' }
               elseif ($p.Data -is [Array]) { $p.Data -join ', ' }
               else { "$($p.Data)" }
        [void]$sb.AppendLine("  $($p.KeyName) = $val")
    }
}
[void]$sb.AppendLine("`n=== ALLOCATED RESOURCES (Memory / IO / IRQ) ===")
Get-CimInstance Win32_AllocatedResource -ErrorAction SilentlyContinue | Where-Object {
    $_.Dependent.DeviceID -match 'VEN_1002|VEN_8086&DEV_191B|VEN_8086&DEV_1901|APP000B'
} | ForEach-Object {
    [void]$sb.AppendLine("  $($_.Dependent.DeviceID) -> $($_.Antecedent)")
}
[System.IO.File]::WriteAllText($pciOut, $sb.ToString(), [System.Text.Encoding]::UTF8)
Write-Host "  OK -> $pciOut" -ForegroundColor Green

Write-Host "`n=============================================================" -ForegroundColor Green
Write-Host " HOTOVO pre rezim '$mode'!" -ForegroundColor Green
Write-Host " Vystupy su v: $diagDir" -ForegroundColor Green
Write-Host "=============================================================" -ForegroundColor Green
Pause
