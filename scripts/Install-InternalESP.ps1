#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Safely installs rEFInd + the 3 clean MacBookPro13,3 Windows GPU modes onto the internal NVMe EFI System Partition.
.DESCRIPTION
    Copies rEFInd into S:\EFI\refind\ and the 3 production files into S:\EFI\tools\ WITHOUT ever modifying
    S:\EFI\Microsoft\Boot\bootmgfw.efi or S:\EFI\APPLE\. Asserts bootmgfw.efi SHA-256 is unchanged before & after.
.PARAMETER SourceRefindDrive
    Drive letter holding verified rEFInd binaries & icons (default: 'D:' = REFIND_USB).
.PARAMETER EspLetter
    Temporary drive letter used to mount the internal NVMe ESP via mountvol (default: 'S:').
.PARAMETER SetDefault
    Updates Windows BCD {bootmgr} path to \EFI\refind\refind_x64.efi after backing up firmware/BCD state.
.PARAMETER Uninstall
    Reverts {bootmgr} back to \EFI\Microsoft\Boot\bootmgfw.efi and removes S:\EFI\refind\ and M2 tool files.
#>
[CmdletBinding()]
param(
    [string]$SourceRefindDrive = 'D:',
    [string]$EspLetter = 'S:',
    [switch]$SetDefault,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

if ($EspLetter -notmatch '^[A-Za-z]:$') {
    throw "Invalid EspLetter '$EspLetter'. Expected format like 'S:'."
}
if ($SourceRefindDrive -notmatch '^[A-Za-z]:$') {
    throw "Invalid SourceRefindDrive '$SourceRefindDrive'. Expected format like 'D:'."
}

$RepoRoot = Split-Path -Parent $PSScriptRoot
$DriveChar = $EspLetter.Substring(0, 1)
if (Get-PSDrive -Name $DriveChar -ErrorAction SilentlyContinue) {
    throw "Drive letter $EspLetter is already in use. Pass a free letter via -EspLetter."
}

$ProdFiles = @(
    @{ Src = Join-Path $RepoRoot 'uefi\m2_hybrid\build\mbp133_m2_hybrid.efi';   Name = 'mbp133_m2_hybrid.efi' },
    @{ Src = Join-Path $RepoRoot 'uefi\m2_hybrid\build\SSDT_HYBRID_MBP133.aml'; Name = 'SSDT_HYBRID_MBP133.aml' },
    @{ Src = Join-Path $RepoRoot 'uefi\m1b_igpu\build\SSDT_IGPU_MBP133.aml';    Name = 'SSDT_IGPU_MBP133.aml' }
)
$ConfSrc   = Join-Path $RepoRoot 'scripts\usb_refind\refind_internal.conf'
$RefindSrc = Join-Path $SourceRefindDrive 'EFI\BOOT\BOOTX64.EFI'
$IconsSrc  = Join-Path $SourceRefindDrive 'EFI\BOOT\icons'

if (-not $Uninstall) {
    foreach ($f in $ProdFiles) {
        if (-not (Test-Path $f.Src)) { throw "Missing built artifact: $($f.Src)" }
    }
    if (-not (Test-Path $ConfSrc))   { throw "Missing config: $ConfSrc" }
    if (-not (Test-Path $RefindSrc)) { throw "Missing rEFInd binary: $RefindSrc (mount REFIND_USB on $SourceRefindDrive)" }
    if (-not (Test-Path $IconsSrc))  { throw "Missing rEFInd icons directory: $IconsSrc" }
}

$mounted = $false
try {
    Write-Host "Mounting internal NVMe ESP at $EspLetter ..."
    & mountvol $EspLetter /S
    if ($LASTEXITCODE -ne 0) { throw "mountvol $EspLetter /S failed with exit code $LASTEXITCODE" }
    $mounted = $true

    $BootmgfwPath = Join-Path $EspLetter 'EFI\Microsoft\Boot\bootmgfw.efi'
    if (-not (Test-Path $BootmgfwPath)) {
        throw "Internal ESP safety check failed: '$BootmgfwPath' not found on $EspLetter."
    }
    $BootmgfwHashBefore = (Get-FileHash $BootmgfwPath -Algorithm SHA256).Hash
    Write-Host "Verified bootmgfw.efi untouched baseline (SHA256=$BootmgfwHashBefore)"

    $BackupDir = Join-Path $RepoRoot 'diag\esp_backup'
    New-Item -ItemType Directory -Force $BackupDir | Out-Null
    $Ts = Get-Date -Format 'yyyyMMdd-HHmmss'
    $BcdOut = & bcdedit /enum all
    $BcdOut | Out-File -FilePath (Join-Path $BackupDir "bcd_all_$Ts.txt") -Encoding utf8
    $FwOut  = & bcdedit /enum firmware
    $FwOut  | Out-File -FilePath (Join-Path $BackupDir "bcd_firmware_$Ts.txt") -Encoding utf8
    Write-Host "Saved BCD backup to $BackupDir\bcd_*_$Ts.txt"

    $RefindDir = Join-Path $EspLetter 'EFI\refind'
    $ToolsDir  = Join-Path $EspLetter 'EFI\tools'

    if ($Uninstall) {
        Write-Host "Rolling back Windows {bootmgr} path to \EFI\Microsoft\Boot\bootmgfw.efi ..."
        & bcdedit /set '{bootmgr}' path '\EFI\Microsoft\Boot\bootmgfw.efi'
        if (Test-Path $RefindDir) {
            Remove-Item $RefindDir -Recurse -Force
            Write-Host "Removed $RefindDir"
        }
        foreach ($f in $ProdFiles) {
            $p = Join-Path $ToolsDir $f.Name
            if (Test-Path $p) {
                Remove-Item $p -Force
                Write-Host "Removed $p"
            }
        }
        $M2LogsDir = Join-Path $EspLetter 'M2_LOGS'
        if (Test-Path $M2LogsDir) {
            Remove-Item $M2LogsDir -Recurse -Force
            Write-Host "Removed $M2LogsDir"
        }
    } else {
        New-Item -ItemType Directory -Force $RefindDir | Out-Null
        New-Item -ItemType Directory -Force $ToolsDir  | Out-Null

        $RefindDst = Join-Path $RefindDir 'refind_x64.efi'
        $IconsDst  = Join-Path $RefindDir 'icons'
        $ConfDst   = Join-Path $RefindDir 'refind.conf'

        Copy-Item $RefindSrc $RefindDst -Force
        Copy-Item $IconsSrc  $IconsDst  -Recurse -Force
        Copy-Item $ConfSrc   $ConfDst   -Force

        foreach ($p in @(@($RefindSrc, $RefindDst), @($ConfSrc, $ConfDst))) {
            $sh = (Get-FileHash $p[0] -Algorithm SHA256).Hash
            $dh = (Get-FileHash $p[1] -Algorithm SHA256).Hash
            if ($sh -ne $dh) { throw "SHA-256 mismatch on $($p[1])" }
            Write-Host "  Verified $($p[1]) (SHA256=$dh)"
        }

        foreach ($f in $ProdFiles) {
            $dst = Join-Path $ToolsDir $f.Name
            Copy-Item $f.Src $dst -Force
            $sh = (Get-FileHash $f.Src -Algorithm SHA256).Hash
            $dh = (Get-FileHash $dst -Algorithm SHA256).Hash
            if ($sh -ne $dh) { throw "SHA-256 mismatch on $dst" }
            Write-Host "  Verified $dst (SHA256=$dh)"
        }

        if ($SetDefault) {
            Write-Host "Setting Windows {bootmgr} path -> \EFI\refind\refind_x64.efi ..."
            & bcdedit /set '{bootmgr}' path '\EFI\refind\refind_x64.efi'
            if ($LASTEXITCODE -ne 0) {
                Write-Warning "bcdedit exited with code $LASTEXITCODE (Apple EFI may require macOS bless)."
            }
        }
    }

    $BootmgfwHashAfter = (Get-FileHash $BootmgfwPath -Algorithm SHA256).Hash
    if ($BootmgfwHashBefore -ne $BootmgfwHashAfter) {
        throw "CRITICAL SAFETY VIOLATION: bootmgfw.efi hash changed!"
    }
    Write-Host "Safety check PASSED: bootmgfw.efi SHA-256 is 100% unchanged."

    if (-not $Uninstall) {
        Write-Host "`nNEXT STEP (REQUIRED ONCE FOR APPLE EFI ON MACBOOKPRO13,3):"
        Write-Host "  macOS Recovery (Cmd+R) IS REQUIRED — normal macOS SIP blocks bless on disk0s1."
        Write-Host "  Reboot holding Cmd+R -> Utilities -> Terminal, mount/keep REFIND_USB plugged in, and run:"
        Write-Host "    sh /Volumes/REFIND_USB/MACOS/bless-refind.command"
    }
} finally {
    if ($mounted) {
        Write-Host "Unmounting $EspLetter ..."
        & mountvol $EspLetter /D | Out-Null
    }
}
