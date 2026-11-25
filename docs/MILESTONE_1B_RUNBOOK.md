# Milestone 1B — Runbook pre test Intel-Only režimu (`MacBookPro13,3`)

> [!IMPORTANT]
> **Bezpečnostný profil:** Interný NVMe ESP (`bootmgfw.efi`) ani Apple NVRAM (`gpu-power-prefs`) **neboli zmenené**.
> Celý loader je nasadený iba na USB `REFIND_USB` (`D:`).
> V najhoršom prípade (čierna obrazovka / pád): podrž tlačidlo napájania **10 sekúnd**, odpoj USB kľúč a systém nabehne naspäť do normálneho Boot Camp režimu.

---

## Čo je nasadené na USB (`D:`)

- `D:\EFI\tools\mbp133_m1b_igpu.efi` — interaktívny Milestone 1B UEFI loader (`_entry` ABI fix, bez auto-timeoutu)
- `D:\EFI\tools\SSDT_IGPU_MBP133.aml` — ACPI SSDT pre jas (`\_SB.PCI0.IGPU.DD01._BCM` -> `\_SB.PCI0.PEG0.GFX0.ABCM`) a S3 spánok/prebudenie (`_PTS`/`_WAK`)
- `D:\EFI\BOOT\refind.conf` — nová predvolená položka **`MBP13,3 M1B Intel-Only (GMUX + HD530)`**
- `D:\M1B_SCRIPTS\Run-M1B-Collect.ps1` — zber výsledkov (GPU stav, rozlíšenie, jas, odber v mW + kopírovanie `\M1B_LOGS\*`)

---

## Postup testu

1. **Pred reštartom over vo Windows:**
   - V Správcovi zariadení (`devmgmt.msc`) je **Intel(R) HD Graphics 530** **povolený** (nie zakázaný z testu 1A!).
   - Odpoj napájací kábel *(voliteľné, ak chceš hneď zmerať pokles spotreby v mW na batérii)*.
2. **Reštartuj notebook s pripojeným USB (`REFIND_USB`):**
   - Ak sa rEFInd nespustí automaticky, podrž **`Option` (`Alt`)** a vyber `EFI Boot` (USB).
   - V rEFInd zvoľ **`MBP13,3 M1B Intel-Only (GMUX + HD530)`** (je nastavený ako default).
3. **V menu `mbp133_m1b_igpu.efi` zvoľ `[1]`:**
   - **`[1] Intel-Only (Apple VBT + BDB43 NONE + DDI 4-Lanes + GMUX IGD + Rail OFF + SSDT)`** *(Overený funkčný režim: 2880×1800@60Hz, jas, S3 spánok, ~9,34 W)*
   - *(Ak by obrazovka na `[1]` ani po 30–40 sekundách nenabehla — t. j. Windows je prihlásený naslepo alebo s čiernym eDP linkom — skús pri ďalšom reštarte **`[2]`** s patchnutým `APPA030` EDID + vypnutým PSR; pre bezpečný test bez vypínania AMD napájania je **`[3]`**).*
4. **Čo sa bude diať po stlačení `[1]`:**
   - Krátko sa zobrazia kroky 1–3 (`Pre-switch EDID/VBT dump -> AppleSetOs -> Switching GMUX -> IGD`).
   - Akonáhle sa GMUX prepne na Intel HD 530, **obrazovka zhasne na ~10–25 sekúnd**, kým Windows načíta `igdkmd64.sys` a zapne eDP link na paneli `APPA030`.
   - UEFI log (`m1b_mode1.txt` / `m1b_last.txt`) a natívny Apple VBT (`vbt_orig.bin`) sa uložia na USB ešte **pred** spustením Windows, takže ich máme k dispozícii aj keby Windows ostal bez obrazu.
5. **Po nabehnutí Windows spusti ako Administrátor v PowerShell:**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\Run-M1B-Collect.ps1 -Tag "m1b_mode1"
   ```
   *(Záloha je aj na `D:\M1B_SCRIPTS\Run-M1B-Collect.ps1`.)*
6. **Overenie výsledku (ak obraz nabehne):**
   - Posuvník jasu vo Windows (funguje plynule cez GMUX `ABCM`),
   - Rozlíšenie `2880x1800 @ 60 Hz` v Nastaveniach displeja,
   - `AMD Radeon` neprítomný (rail OFF cez GMUX `0x50`), `Intel HD Graphics 530` v poriadku (`OK`),
   - Odber batérie v mW (`DischargeRate_mW` okolo ~10–12 W v pokoji namiesto ~17 W),
   - Voliteľne krátky test spánku (`Sleep -> Wake`) na overenie `_PTS` / `_WAK` re-routingu v `SSDT_IGPU_MBP133.aml`.
