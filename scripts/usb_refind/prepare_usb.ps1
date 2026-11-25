$ErrorActionPreference = 'Stop'

# 1. Require Administrator
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Write-Error "Tento skript musi byt spusteny v PowerShell ako Spravca (Run as Administrator)."
    exit 1
}

# 2. Strictly identify ONLY the connected SanDisk 3.2 Gen1 USB stick
$usbList = @(Get-Disk | Where-Object {
    $_.BusType -eq 'USB' -and
    $_.FriendlyName -like '*SanDisk*' -and
    $_.UniqueId -like '*A2003ABE3BF40C15*'
})

if ($usbList.Count -ne 1) {
    Write-Error "Nepodarilo sa jednoznacne najst ocakavany SanDisk USB kluc (A2003ABE3BF40C15). Akcia zrusena."
    exit 1
}

$usb = $usbList[0]
$diskNum = $usb.Number
Write-Host ("Najdeny USB disk {0}: {1} ({2:N2} GB, {3})" -f $diskNum, $usb.FriendlyName, ($usb.Size / 1GB), $usb.PartitionStyle) -ForegroundColor Cyan

# 3. Ensure clean GPT + <=32GB partition
$part = Get-Partition -DiskNumber $diskNum -ErrorAction SilentlyContinue | Where-Object { $_.Type -eq 'Basic' -and $_.Size -le 32GB } | Select-Object -First 1

if ($usb.PartitionStyle -ne 'GPT' -or -not $part) {
    Write-Host "Cistim disk a vytvaram novu GPT + 31 GB particiu..." -ForegroundColor Yellow
    Clear-Disk -Number $diskNum -RemoveData -RemoveOEM -Confirm:$false -ErrorAction SilentlyContinue
    if ((Get-Disk -Number $diskNum).PartitionStyle -eq 'RAW') {
        Initialize-Disk -Number $diskNum -PartitionStyle GPT
    }
    $part = New-Partition -DiskNumber $diskNum -Size 31GB -AssignDriveLetter
    Start-Sleep -Seconds 2
}

if (-not $part.DriveLetter) {
    $part | Add-PartitionAccessPath -AssignDriveLetter
    Start-Sleep -Seconds 1
    $part = Get-Partition -DiskNumber $diskNum -PartitionNumber $part.PartitionNumber
}

# 4. Format as FAT32
Write-Host ("Formatujem particiu {0}: na FAT32 (REFIND_USB)..." -f $part.DriveLetter) -ForegroundColor Yellow
$vol = Format-Volume -Partition $part -FileSystem FAT32 -NewFileSystemLabel 'REFIND_USB' -Confirm:$false
Start-Sleep -Seconds 1

$dl = $vol.DriveLetter
if (-not $dl) {
    $dl = (Get-Partition -DiskNumber $diskNum -PartitionNumber $part.PartitionNumber).DriveLetter
}
$targetRoot = "${dl}:\"

# 5. Copy prepared rEFInd EFI\BOOT payload onto USB
$payloadSrc = Join-Path $PSScriptRoot 'usb_payload\EFI'
if (-not (Test-Path $payloadSrc)) {
    throw "Chyba zdrojovy priecinok $payloadSrc"
}

Write-Host "Kopirujem rEFInd (EFI\BOOT\) na $targetRoot ..." -ForegroundColor Cyan
Copy-Item -Path $payloadSrc -Destination $targetRoot -Recurse -Force

# 6. Verify result
Write-Host "`n=== OVERENIE USB KLUCA $targetRoot ===" -ForegroundColor Green
Get-Disk -Number $diskNum | Select-Object Number, FriendlyName, PartitionStyle, @{N='SizeGB';E={[math]::Round($_.Size/1GB,2)}} | Format-Table -AutoSize
Get-Volume -DriveLetter $dl | Select-Object DriveLetter, FileSystemLabel, FileSystemType, @{N='SizeGB';E={[math]::Round($_.Size/1GB,2)}} | Format-Table -AutoSize
Get-ChildItem -Path "${dl}:\EFI\BOOT" | Select-Object Mode, Name, Length | Format-Table -AutoSize
$spoof = Select-String -Path "${dl}:\EFI\BOOT\refind.conf" -Pattern '^spoof_osx_version'
Write-Host "Aktivny spoof riadok v ${dl}:\EFI\BOOT\refind.conf: $($spoof.Line)" -ForegroundColor Green
Write-Host "`nHOTOVO! USB kluc je pripraveny na restart." -ForegroundColor Green
