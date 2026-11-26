# Milestone 2 — Runbook pre Unified Hybrid Dual-GPU (`MacBookPro13,3`)

> [!IMPORTANT]
> **Stav: OVERENÉ NA HARDVÉRI (pozri [MILESTONE_2_FINDINGS.md](MILESTONE_2_FINDINGS.md)).**
> **Bezpečnostný profil:** Interný NVMe ESP (`bootmgfw.efi`) aj overené binárky z M1B/M1A-2 (`mbp133_m1b_igpu.efi`, `mbp133_m1a2_ebs.efi`, `SSDT_IGPU_MBP133.aml`) zostávajú nedotknuté.
> Prepnutie Apple NVRAM premennej **`gpu-power-prefs = iGPU` (`01 00 00 00`)** je vyžadované pre **`Mode [1]` (`Hybrid Mode 5`)**, aby Apple EFI už pri štarte zvolil Intel HD 530 ako primárnu POST/GOP GPU a Windows PnP nezhodil obe karty pri odovzdávaní displeja.
> Na USB v `D:\EFI\tools\` musia byť nasadené súbory `mbp133_m2_hybrid.efi`, `SSDT_HYBRID_MBP133.aml` (pre Mode `[1]`) a `SSDT_IGPU_MBP133.aml` (pre Mode `[2]`).

---

## Ako vrátiť `gpu-power-prefs` späť na predvolenú AMD dGPU (Rollback)

> [!CAUTION]
> **Dôležité upozornenie pre klasický Boot Camp:** Pokiaľ je v Apple NVRAM nastavené **`gpu-power-prefs = iGPU` (`01 00 00 00`)**, klasické nabootovanie Boot Campu napriamo (bez `mbp133_m2_hybrid.efi` / rEFInd) skončí **čiernou obrazovkou**! Ak chceš znova používať klasický Boot Camp bez loadera, **musíš vymazať NVRAM** buď cez **`Windows 11: Boot Camp Default (AMD Only)`** v rEFInd, alebo **podržaním klávesovej skratky `Cmd + Option + P + R` pri štarte MacBooku** (až do zaznenia reštartu/gongu).

Kedykoľvek troma spôsobmi:
1. V rEFInd vyber **`Windows 11: Boot Camp Default (AMD Only)`** (alebo v menu `mbp133_m2_hybrid.efi` stlač **`[S]`** / **`[D]`**).
2. Pri štarte MacBooku podrž **`Cmd + Option + P + R`** (klasický reset Apple NVRAM — povinné, ak chceš znova spúšťať holý Boot Camp priamo!).
3. Nabootovaním **macOS** (macOS premennú automaticky vymaže).

---

## Postup testu `Hybrid Mode 5`

1. **Reštartuj MacBook** s pripojeným USB kľúčom `D:` a v rEFInd spusti predvolenú položku **`MBP13,3 M2 Hybrid (Intel Display + AMD Offload)`**.
2. Ak menu ukazuje `NVRAM gpu-power-prefs: dGPU (not set)`, stlač **`[S]`** (`Switch gpu-power-prefs -> iGPU + Cold Reboot`).
   - Loader zapíše `01 00 00 00`, overí spätné čítanie a po 1,5 s MacBook sám reštartuje.
3. Po automatickom reštarte znova otvor v rEFInd **`MBP13,3 M2 Hybrid (Intel Display + AMD Offload)`** — teraz uvidíš `NVRAM gpu-power-prefs: iGPU (01 00 00 00) [Ready for Mode 1 Hybrid]` — a stlač **`[1]`** (`Hybrid Mode 5`).
4. Po nabehnutí Windowsu (Intel HD 530 `2880×1800 @ 60 Hz` + AMD Radeon RX 560X / Pro 455 bez `Code 43`) otestuj posuvník jasu, odozvu písania v Antigravity a S3 spánok/prebudenie, potom spusti:
   ```powershell
   python scripts\Read-M2-Hybrid.py --tag m2_hybrid_s3ok
   ```
