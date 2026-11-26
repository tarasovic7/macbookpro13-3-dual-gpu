MacBookPro13,3 — Nastavenie predvoleného bootu rEFInd z interného SSD (macOS Recovery)
=========================================================================================

DÔLEŽITÉ: Normálny macOS blokuje príkaz `bless --setBoot` na interný EFI oddiel kvôli SIP.
Pre nastavenie predvoleného bootu na interný disk je NEVYHNUTNÉ použiť macOS Recovery!

Postup:
1. Najprv maj spustený Windows inštalačný skript (v Administratorskom PowerShelli):
   powershell -ExecutionPolicy Bypass -File .\scripts\Install-InternalESP.ps1 -SetDefault
2. Reštartuj MacBook a pri štarte drž **Cmd + R**, kým nenabehne **macOS Recovery**.
3. V hornom menu otvor **Utilities → Terminal** (pri zapojenom USB kľúči REFIND_USB) a spusti:
   sh /Volumes/REFIND_USB/MACOS/bless-refind.command

UPOZORNENIE PRE KLASICKÝ BOOT CAMP:
Pokiaľ máš v NVRAM zapnuté `gpu-power-prefs = iGPU` (Hybrid režim), priamy štart holého
Boot Campu bez rEFInd dá čiernu obrazovku. Pre návrat na holý Boot Camp použi v rEFInd
položku "Boot Camp Default", alebo pri štarte drž **Cmd + Option + P + R** (reset NVRAM).

EN:
macOS Recovery (Cmd + R -> Utilities -> Terminal) is REQUIRED because normal macOS SIP
blocks `bless --setBoot` on `disk0s1`. With REFIND_USB plugged in, run:
   sh /Volumes/REFIND_USB/MACOS/bless-refind.command
