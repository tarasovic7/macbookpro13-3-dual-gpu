<#
.SYNOPSIS
    Deploys either the clean 4-mode production setup or full developer toolkit to the MacBookPro13,3 rEFInd USB drive.
.PARAMETER Profile
    'clean' (default): Removes old dev folders/binaries on USB and keeps only the 4 clean Windows modes
                       (mbp133_m2_hybrid.efi + SSDT_HYBRID_MBP133.aml + SSDT_IGPU_MBP133.aml).
    'dev'            : Installs production binaries PLUS diagnostic loaders (m1a_probe, m1a2_ebs, m1b_igpu),
                       Python/PowerShell helper scripts, and refind_dev.conf.
.PARAMETER UsbDrive
    Target USB FAT32 drive letter (default: 'D:'). Must already contain \EFI\BOOT\BOOTX64.EFI.
#>
[CmdletBinding()]
param(
    [ValidateSet('clean', 'dev')]
    [string]$Profile = 'clean',
    [string]$UsbDrive = 'D:'
)

$ErrorActionPreference = 'Stop'

if ($UsbDrive -notmatch '^[A-Za-z]:$') {
    throw "Invalid UsbDrive '$UsbDrive'. Expected format like 'D:'."
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
$RefindBoot = Join-Path $UsbDrive 'EFI\BOOT\BOOTX64.EFI'
if (-not (Test-Path $RefindBoot)) {
    throw "Refusing to deploy: '$RefindBoot' not found. Mount the REFIND_USB drive on $UsbDrive first."
}

$ToolsDir = Join-Path $UsbDrive 'EFI\tools'
$BootDir  = Join-Path $UsbDrive 'EFI\BOOT'
New-Item -ItemType Directory -Force $ToolsDir | Out-Null

$ProdFiles = @(
    @{ Src = Join-Path $RepoRoot 'uefi\m2_hybrid\build\mbp133_m2_hybrid.efi';   Dst = Join-Path $ToolsDir 'mbp133_m2_hybrid.efi' },
    @{ Src = Join-Path $RepoRoot 'uefi\m2_hybrid\build\SSDT_HYBRID_MBP133.aml'; Dst = Join-Path $ToolsDir 'SSDT_HYBRID_MBP133.aml' },
    @{ Src = Join-Path $RepoRoot 'uefi\m1b_igpu\build\SSDT_IGPU_MBP133.aml';    Dst = Join-Path $ToolsDir 'SSDT_IGPU_MBP133.aml' }
)

foreach ($f in $ProdFiles) {
    if (-not (Test-Path $f.Src)) {
        throw "Missing required built artifact: $($f.Src). Build the UEFI/ASL sources first (see docs/BUILD_AND_PORT_GUIDE.md)."
    }
}

$DevFolders = @('M1A_LOGS', 'M1A_SCRIPTS', 'M1B_LOGS', 'M1B_SCRIPTS', 'M1A2_SCRIPTS', 'DEV_SCRIPTS')
$DevTools   = @('mbp133_m1a_probe.efi', 'mbp133_m1b_igpu.efi', 'mbp133_m1a2_ebs.efi')

if ($Profile -eq 'clean') {
    Write-Host "Applying CLEAN production profile to $UsbDrive ..."
    foreach ($dirName in $DevFolders) {
        $p = Join-Path $UsbDrive $dirName
        if (Test-Path $p) {
            Remove-Item $p -Recurse -Force
            Write-Host "  Removed $p"
        }
    }
    foreach ($toolName in $DevTools) {
        $p = Join-Path $ToolsDir $toolName
        if (Test-Path $p) {
            Remove-Item $p -Force
            Write-Host "  Removed $p"
        }
    }
    $ConfSrc = Join-Path $RepoRoot 'scripts\usb_refind\refind.conf'
} else {
    Write-Host "Applying DEV diagnostic profile to $UsbDrive ..."
    $DevDir = Join-Path $UsbDrive 'DEV_SCRIPTS'
    New-Item -ItemType Directory -Force $DevDir | Out-Null

    $OptionalDevBins = @(
        @{ Src = Join-Path $RepoRoot 'uefi\m1a_probe\build\mbp133_m1a_probe.efi'; Dst = Join-Path $ToolsDir 'mbp133_m1a_probe.efi' },
        @{ Src = Join-Path $RepoRoot 'uefi\m1a2_ebs\build\mbp133_m1a2_ebs.efi';   Dst = Join-Path $ToolsDir 'mbp133_m1a2_ebs.efi' },
        @{ Src = Join-Path $RepoRoot 'uefi\m1b_igpu\build\mbp133_m1b_igpu.efi';   Dst = Join-Path $ToolsDir 'mbp133_m1b_igpu.efi' }
    )
    foreach ($b in $OptionalDevBins) {
        if (Test-Path $b.Src) {
            Copy-Item $b.Src $b.Dst -Force
        } else {
            Write-Warning "Optional dev binary not built: $($b.Src)"
        }
    }

    foreach ($s in @('Read-M2-Hybrid.py', 'Read-M1A2-Ebs.py', 'Parse-M1A-Probe.py', 'Run-M1A-Collect.ps1', 'Run-M1B-Collect.ps1')) {
        $sp = Join-Path $PSScriptRoot $s
        if (Test-Path $sp) { Copy-Item $sp (Join-Path $DevDir $s) -Force }
    }
    $ConfSrc = Join-Path $RepoRoot 'scripts\usb_refind\refind_dev.conf'
}

foreach ($f in $ProdFiles) {
    Copy-Item $f.Src $f.Dst -Force
    $sHash = (Get-FileHash $f.Src -Algorithm SHA256).Hash
    $dHash = (Get-FileHash $f.Dst -Algorithm SHA256).Hash
    if ($sHash -ne $dHash) {
        throw "SHA-256 verification failed for $($f.Dst)"
    }
    Write-Host "  Verified $($f.Dst) (SHA256=$dHash)"
}

$ConfDst = Join-Path $BootDir 'refind.conf'
Copy-Item $ConfSrc $ConfDst -Force
$cSrcHash = (Get-FileHash $ConfSrc -Algorithm SHA256).Hash
$cDstHash = (Get-FileHash $ConfDst -Algorithm SHA256).Hash
if ($cSrcHash -ne $cDstHash) {
    throw "SHA-256 verification failed for $ConfDst"
}
Write-Host "  Verified $ConfDst (from $(Split-Path -Leaf $ConfSrc))"

Write-Host "`nFinal $UsbDrive layout:"
Get-ChildItem $UsbDrive | Select-Object Name, LastWriteTime | Format-Table -AutoSize
Get-ChildItem $ToolsDir | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
