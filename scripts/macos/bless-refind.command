#!/bin/bash
set -e

echo "=== MacBookPro13,3 rEFInd Internal ESP Bless Helper ==="

SUDO=""
if [ "$(id -u)" -ne 0 ]; then
  SUDO="sudo"
fi

if [ ! -d "/Volumes/EFI" ]; then
  echo "Mounting internal EFI partition (disk0s1)..."
  $SUDO diskutil mount disk0s1
fi

REFIND_EFI="/Volumes/EFI/EFI/refind/refind_x64.efi"
if [ ! -f "$REFIND_EFI" ]; then
  echo "ERROR: $REFIND_EFI not found!"
  echo "First run scripts\Install-InternalESP.ps1 in Administrator PowerShell on Windows."
  exit 1
fi

echo "Blessing $REFIND_EFI as default Apple EFI boot loader..."
if ! $SUDO bless --mount /Volumes/EFI --setBoot --file "$REFIND_EFI" --shortform; then
  echo ""
  echo "NOTICE: bless failed (likely macOS System Integrity Protection / SIP is enabled)."
  echo "Reboot holding Cmd+R into macOS Recovery, open Utilities -> Terminal, and run:"
  echo "  sh /Volumes/REFIND_USB/MACOS/bless-refind.command"
  exit 1
fi

echo ""
echo "SUCCESS! Apple EFI will now boot internal rEFInd by default:"
$SUDO bless --info /Volumes/EFI
