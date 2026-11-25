# Milestone 1B — Výsledky testu Intel-Only režimu (`MacBookPro13,3`)

> [!IMPORTANT]
> **Režim 4 (Intel-Only) na `MacBookPro13,3` je kompletne overený a funkčný na reálnom hardvéri!**
> Interný Retina panel `2880 × 1800 @ 60 Hz` beží na **Intel HD Graphics 530**, **AMD Radeon Pro 455** je fyzicky odpojená od napájania cez GMUX (`0x50 = 0x00`), spotreba na batérii klesla na **`9,34 W`**, **plynulá regulácia jasu funguje** a **hlboký spánok `Standby (S3)` s prebudením funguje čisto**.
> Interný NVMe ESP ani Apple NVRAM `gpu-power-prefs` neboli upravené; celý postup beží výlučne z USB `REFIND_USB`.

---

## 1. Zachytené artefakty testu (`Mode 1`)

- Finálny UEFI log z bootu (vrátane `BDB[43] → NONE` patchu): [m1b_mode1.txt](../diag/m1b/uefi/m1b_mode1.txt) (kópia [m1b_last.txt](../diag/m1b/uefi/m1b_last.txt))
- Natívny a patchnutý Apple VBT dump (`3 826 B`, uložené lokálne mimo git cez `.gitignore`): `diag/m1b/uefi/vbt_orig.bin`, `diag/m1b/uefi/vbt_patched.bin`
- Finálny Windows snapshot (po oprave jasu a prebudení zo spánku S3): `diag/m1b/win_m1b_mode1_final_*.txt` (predchádzajúce behy: `win_m1b_mode1_*.txt`, `win_m1b_mode1_battery_*.txt`)
- Použité komponenty: [mbp133_m1b_igpu.c](../uefi/m1b_igpu/mbp133_m1b_igpu.c), [SSDT_IGPU_MBP133.asl](../uefi/m1b_igpu/SSDT_IGPU_MBP133.asl), [Run-M1B-Collect.ps1](../scripts/Run-M1B-Collect.ps1), [MILESTONE_1B_RUNBOOK.md](MILESTONE_1B_RUNBOOK.md)

---

## 2. Čo funguje (100 % overené na reálnom hardvéri)

| Komponent | Overený výsledok |
| :--- | :--- |
| **Intel HD Graphics 530 (`00:02.0`)** | `Status = OK`, `ConfigManagerErrorCode = 0`, ovládač `31.0.101.2115`, plné rozlíšenie **`2880 × 1800 @ 60 Hz`** na `DISPLAY\APPA030` (`\_SB.PCI0.IGPU.DD01`) |
| **Indexed I/O GMUX (`Renesas R4F2113`)** | Prepnutie panela na IGD na 1. pokus (`0x10 = 0x02`, `0x28 = 0x01`, `0x40 = 0x01`/`0x03`, IRQ status čistý) |
| **Odpojenie AMD Radeon Pro 455 (`01:00.0` + `01:00.1`)** | GMUX rail OFF úspešný (`0x50 = 0x00`, `pwr_irq = 1`); AMD GPU aj AMD Audio úplne neprítomné vo Windows (`Win32_VideoController` ukazuje výlučne Intel HD 530) |
| **eDP AUX-A & `DDI_BUF_CTL_A` 4-linkový strap** | `DPCD rev=0x11, max_rate=0x0A, max_lanes=4, eDP_rev=2, PSR_cap=0x01`; `I2C-over-AUX` EDID `APPA030` prečítaný čistý (`128 B`); `DDI_BUF_CTL_A` prepnutý `0x00000081 → 0x00000091 [4_LANES OK]` |
| **Natívny Apple `$VBT SKYLAKE`** | Dĺžka `3 826 B`, `BDB ver=196`, `BDB40.psr_dword = 0x00000000` (`sum8 = 0`) — obraz nabehol priamo s natívnym VBT bez potreby externého VBT súboru |
| **Regulácia jasu displeja (`BDB[43] = NONE` + ACPI `_BCM`)** | Apple natívny VBT mal v `BDB[43]` (LFP Backlight) nastavený `type = 2` (`0x4A`, Intel PCH PWM), takže posuvník jasu zapisoval do nezapojeného Intel PWM. Po patchi **`[VBT BDB43] before(p0,p2)=0x4A,0x4A -> after=0x00 esz=6 sum8=0 OK`** ovládač `igfx` deleguje jas cez `\_SB.PCI0.IGPU.DD01._BCM` → `\_SB.PCI0.PEG0.GFX0.ABCM` → GMUX `0x774` a **jas panela sa plynule mení** |
| **Spánok `Standby (S3)` a prebudenie (`_PTS` / `_WAK`)** | Overené zavretím a otvorením veka v System Event Logu (`Kernel-Power ID 42`: vstup `Button or Lid` → `ID 107`: prebudenie → `Power-Troubleshooter ID 1`): obraz na Intel HD 530 okamžite nabehol a **AMD Radeon Pro 455 ostala odpojená** |
| **Spotreba na batérii (bez nabíjačky)** | 5 po sebe idúcich vzoriek po 2 s: konštantných **`9 337 mW` (`9,34 W`)** pri `12,791 V` (`56 410 mWh`) — reálna úspora **~7–8 W** oproti ~17 W na AMD (~6 h výdrže) |
| **In-memory ACPI premenovania (`DSDT`)** | Potvrdené priamo v živom registri Windows (`HKLM\HARDWARE\ACPI\DSDT`, dĺžka `34 523 B`): `XBCM = True`, `_BCM = False`, `XWAK = True`, `XPTS = True`; `XSDT entries = 23`, `old RSDP synced` |

---

## 3. Známe hardvérové limity Režimu 4 a ďalší krok

### 3.1 Externé Thunderbolt 3 / USB-C výstupy (hardvérový limit Intel-Only Režimu 4)
- Všetky 4× TB3/USB-C displejové výstupy na `MacBookPro13,3` sú hardvérovo pripojené výlučne na AMD dGPU (`01:00.0`). V Intel-Only režime sú neaktívne — rieši ich až **Milestone 1A-2 / Režim 5 (Hybrid Dual-GPU)**.

### 3.2 ~10–25 s čierna obrazovka počas štartu Windows
- Očakávané správanie medzi prepnutím GMUX v UEFI (kedy AMD GOP prestane zobrazovať na internom paneli) a inicializáciou eDP linku v ovládači `igdkmd64.sys`.

### 3.3 Ďalší míľnik: Hybrid Dual-GPU (`Milestone 1A-2`)
- Oddelenie vplyvu `ExitBootServices()` v Apple EFI od prítomnosti PCI `00:02.0` v systéme Windows (`DEVEN.D2EN` + VT-d `0xFED91000`), aby AMD Radeon Pro 455 fungoval bez `Code 43` popri zapnutom Intel HD 530.
