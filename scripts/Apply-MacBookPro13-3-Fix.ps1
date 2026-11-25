<#
.SYNOPSIS
    Aplikuje Apple Boot Camp kľúče pre jas displeja, eDP stabilitu a Polaris 11 na MacBookPro13,3
    po inštalácii R.ID (Radeon-ID) Polaris-Vega-Navi ovládača.
#>
$ErrorActionPreference = 'Stop'

# 1. Kontrola práv Správcu
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Host "Požadujem oprávnenia Správcu (UAC)..." -ForegroundColor Yellow
    Start-Process -FilePath 'powershell.exe' -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"" -Verb RunAs
    exit 0
}

Write-Host "=================================================================" -ForegroundColor Cyan
Write-Host " MacBookPro13,3 (AMD Radeon Pro 455) - Apple Jas & Polaris11 Fix" -ForegroundColor Cyan
Write-Host "=================================================================" -ForegroundColor Cyan

# 2. Voliteľná inštalácia chýbajúceho VC++ x86 z SD karty (pre AMD Radeon UI / cncmd.exe)
$vc86 = 'E:\Apps\VCRedist\vc_redist.x86.exe'
$hasVc86 = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*' -ErrorAction SilentlyContinue |
    Where-Object { $_.DisplayName -match 'Visual C\+\+ 2015-2022.*\(x86\)' }
if (-not $hasVc86 -and (Test-Path -LiteralPath $vc86)) {
    Write-Host "[1/3] Inštalujem Microsoft VC++ 2015-2022 (x86) z $vc86 ..." -ForegroundColor Cyan
    $p = Start-Process -FilePath $vc86 -ArgumentList '/install /quiet /norestart' -Wait -PassThru
    Write-Host "  OK -> VC++ x86 kód: $($p.ExitCode)" -ForegroundColor Green
} else {
    Write-Host "[1/3] VC++ x86 už je nainštalované alebo SD karta E: nie je pripojená (preskakujem)." -ForegroundColor DarkGray
}

# 3. Zápis Apple Boot Camp + Polaris 11 jasových a eDP kľúčov do AMD Display adaptéra
Write-Host "`n[2/3] Zapisujem Apple jasové a eDP kľúče pre AMD Radeon Pro 455 (Polaris 11) ..." -ForegroundColor Cyan
$displayClassPath = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
$classKeys = @(Get-ChildItem -LiteralPath $displayClassPath -ErrorAction SilentlyContinue |
    Where-Object { $_.PSChildName -match '^\d{4}$' })

$updated = 0
foreach ($subKey in $classKeys) {
    $props = Get-ItemProperty -LiteralPath $subKey.PSPath -ErrorAction SilentlyContinue
    if ($props -and ("$($props.DriverDesc) $($props.ProviderName) $($props.MatchingDeviceId)" -match 'AMD|Radeon|Advanced Micro Devices|VEN_1002')) {
        Write-Host "  Nájdený AMD adaptér: $($subKey.PSChildName) ($($props.DriverDesc))" -ForegroundColor Yellow

        # 4 základné Apple jasové kľúče (overené aj z MacBookPro11,5 aj z tvojho pôvodného oem1.inf)
        Set-ItemProperty -Path $subKey.PSPath -Name 'DalFeatureBacklightControl'     -Value 0 -Type DWord
        Set-ItemProperty -Path $subKey.PSPath -Name 'PP_VariBrightFeatureEnable'     -Value 0 -Type DWord
        Set-ItemProperty -Path $subKey.PSPath -Name 'PP_Apple_Bootcamp_Enable'       -Value 1 -Type DWord
        Set-ItemProperty -Path $subKey.PSPath -Name 'KMD_EnableBrightnessInterface2' -Value 0 -Type DWord

        # 4 kľúčové Polaris 11 / Apple Retina eDP stabilizačné kľúče (z tvojho funkčného MacBookPro13,3 oem1.inf)
        Set-ItemProperty -Path $subKey.PSPath -Name 'DalDPSkipPowerOff'              -Value 1 -Type DWord
        Set-ItemProperty -Path $subKey.PSPath -Name 'DalPSRFeatureEnable'            -Value 0 -Type DWord
        Set-ItemProperty -Path $subKey.PSPath -Name 'EnableUlps'                     -Value 0 -Type DWord
        Set-ItemProperty -Path $subKey.PSPath -Name 'PP_DisableFFC'                  -Value 1 -Type DWord
        Set-ItemProperty -Path $subKey.PSPath -Name 'PP_DisableVoltageIsland'        -Value 1 -Type DWord
        Set-ItemProperty -Path $subKey.PSPath -Name 'DisableSAMUPowerGating'         -Value 1 -Type DWord

        $updated++
    }
}

if ($updated -eq 0) {
    Write-Warning "Nenašiel sa žiadny AMD adaptér v registri triedy Display!"
} else {
    Write-Host "  OK -> Zápis dokončený pre $updated AMD adaptér." -ForegroundColor Green
}

# 4. Povolenie vstavaného Retina displeja DISPLAY\APPA030 (ak by ho R.ID zakázal)
Write-Host "`n[3/3] Kontrolujem vstavaný panel DISPLAY\APPA030 ..." -ForegroundColor Cyan
$hwProfileDisplay = 'HKLM:\SYSTEM\CurrentControlSet\Hardware Profiles\Current\System\CurrentControlSet\Enum\DISPLAY\APPA030'
if (Test-Path -LiteralPath $hwProfileDisplay) {
    Get-ChildItem -LiteralPath $hwProfileDisplay -ErrorAction SilentlyContinue | ForEach-Object {
        Set-ItemProperty -Path $_.PSPath -Name 'CSConfigFlags' -Value 0 -Type DWord
        Write-Host "  OK -> CSConfigFlags = 0 pre $($_.PSChildName)" -ForegroundColor Green
    }
}

$disabledMonitors = @(Get-PnpDevice -Class Monitor -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'DISPLAY\APPA030*' -and $_.Status -ne 'OK' })
foreach ($mon in $disabledMonitors) {
    Write-Host "  Povoľujem vstavaný monitor: $($mon.InstanceId)" -ForegroundColor Cyan
    & pnputil.exe /enable-device $mon.InstanceId | Out-Null
}

Write-Host "`n=== AKTUÁLNY STAV OBOCH GPU ===" -ForegroundColor Green
Get-PnpDevice -Class Display | Format-Table Status, Problem, FriendlyName, InstanceId -AutoSize

Write-Host "`nHOTOVO! Teraz reštartuj (cez USB rEFInd pre test Intel+AMD, alebo bez USB pre čisté AMD)." -ForegroundColor Green
Pause
