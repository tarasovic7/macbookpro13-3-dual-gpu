# Milestone 1A-2 — Runbook pre `ExitBootServices()` X-Ray v2 & Dual-GPU Fix (`MacBookPro13,3`)

> [!NOTE]
> **Stav: DOKONČENÉ A OVERENÉ NA HARDVÉRI.** Výsledky všetkých 6 bootov vrátane vyriešenia `Code 43` pomocou 3-registrovej opravy sú zdokumentované v [`docs/MILESTONE_1A2_FINDINGS.md`](MILESTONE_1A2_FINDINGS.md).

> [!IMPORTANT]
> **Bezpečnostný profil:** Interný NVMe ESP (`bootmgfw.efi`), Apple NVRAM (`gpu-power-prefs`) aj funkčné súbory **Milestone 1B** (`mbp133_m1b_igpu.efi`, `SSDT_IGPU_MBP133.aml`) zostávajú nedotknuté.
> Všetko beží z USB `REFIND_USB` (`D:`) cez **`D:\EFI\tools\mbp133_m1a2_ebs.efi`**.

---

## Čo ukázal v2 X-Ray a finálna 3-registrová oprava (`Mode [5]`)

Rozšírený v2 X-Ray dokázal, že Apple EFI v Boot Camp `ExitBootServices()` callbacku presúva VRAM na Polaris 11 (`01:00.0`) pomocou presne troch `BAR5` registrov:
- `MC_VM_FB_LOCATION` (`0x0809`): `0x007F0000 -> 0xF47FF400`
- `HDP_NONSURFACE_BASE` (`0x0B01`): `0x00000000 -> 0xF4000000`
- `GRPH_PRIMARY_SURFACE_ADDRESS_HIGH` (`0x1A07`): `0x00000000 -> 0x000000F4` (latch zápisom `0x1A04`)

Režim **`[5]`** zavolá `AppleSetOs("Mac OS X 10.15")` (aby zostala viditeľná Intel HD 530 `00:02.0`) a po úspešnom `ExitBootServices()` zapíše všetky tri registre na Polaris 11.

---

## Postup reštartov (rEFInd → `MBP13,3 M1A-2 EBS X-Ray & Dual-GPU`)

### 1. Reštart — Spustenie Dual-GPU fixu (`[5]` `SetOS 10.15 + 3-Reg FB Fix (0x809+0xB01+0x1A07)`)
1. V UEFI menu stlač **`[5]`**.
2. Po nabehnutí Windows skontroluj Správcu zariadení (Intel HD 530 aj AMD Radeon obe `OK` bez `Code 43`) a prípadne ulož diagnostiku:
   ```powershell
   python scripts\Read-M1A2-Ebs.py --tag mode5_3reg
   ```
   - Log sa uloží do `diag\m1a2\m1a2_mode5_3reg_*.txt` (overené: `Flags=0x0000004f`, oba adaptéry `CM_PROB_NONE`, funkčný jas aj S3 spánok/prebudenie).

### 2. Reštart 2 *(iba ak pri `[5]` stále bude `Code 43`)* — Rozšírený v2 X-Ray (`[1]` Normal vs `[2]` SetOS)
1. Spusti `[1]` → vo Windows `python scripts\Read-M1A2-Ebs.py`
2. Reštartuj s `[2]` → vo Windows:
   ```powershell
   python scripts\Read-M1A2-Ebs.py
   python scripts\Read-M1A2-Ebs.py --diff <v2_mode1_bin> <v2_mode2_bin>
   ```
   -> Uvidíme každý jeden sprievodný register v blokoch `MC_VM`, `HDP` a `DCE0`, ktorý Apple EFI upraví spolu s `MC_VM_FB_LOCATION`.
