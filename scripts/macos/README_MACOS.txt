MacBookPro13,3 — Nastavenie predvoleného bootu rEFInd z interného SSD (macOS / Recovery)
========================================================================================

Tento priečinok je potrebný IBA raz, ak Apple EFI po inštalácii cez
Install-InternalESP.ps1 ignoruje Windows bcdedit a bez USB kľúča štartuje rovno do Windows.

Postup v macOS (alebo v macOS Recovery cez Cmd+R):
1. Najprv maj spustený Windows krok:
   powershell -ExecutionPolicy Bypass -File .\scripts\Install-InternalESP.ps1 -SetDefault
2. V macOS otvor Terminal (alebo v Recovery Cmd+R: Utilities -> Terminal) a spusti:
   sh /Volumes/REFIND_USB/MACOS/bless-refind.command

EN:
If MacBookPro13,3 skips internal rEFInd and boots directly into Windows without the USB
after running Install-InternalESP.ps1 -SetDefault, boot into macOS (or Cmd+R Recovery
Terminal if SIP blocks bless) and run:
   sh /Volumes/REFIND_USB/MACOS/bless-refind.command
