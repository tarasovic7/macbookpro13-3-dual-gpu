# Milestone 1A Runbook: Izolácia `polaris10_start_smu` (`MacBookPro13,3`)

> [!IMPORTANT]
> **Bezpečnostná záruka:** Všetky UEFI nástroje sú výlučne na USB kľúči **`D:` (`REFIND_USB`)**. Na internú EFI partíciu (`bootmgfw.efi`) ani na Apple NVRAM (`gpu-power-prefs`) sa vôbec nesiaha. Odpojením USB kľúča a reštartom sa MacBook okamžite vráti do 100 % pôvodného Windows 11.

## Čo týmto testom presne zistíme

Fáza 0 dokázala, že pri `AppleSetOs` zlyhá `amdkmdag.sys` v `polaris10_start_smu` (`failure_bitmask = 0x801`, Bit 11), pričom všetkých 24 ACPI tabuliek aj všetky 4 AMD BARy sú identické. Míľnik 1A oddelí dve zostávajúce hypotézy:

1. **H1 — Apple EFI / hardvérový stav SMU7 / PCI / GMUX / VT-d:** Či samotné zavolanie `EFI_APPLE_SET_OS_PROTOCOL` (`"Mac OS X 10.15"` alebo `"Mac OS X 10.9"`) zmení `00:00.0 DEVEN`, `00:01.0 PEG0`, `00:02.0 IGPU`, `01:00.0` PCI/BAR5/SMU7 registre (`ixSMC_SYSCON_*`, `ixSMC_PC_C`, `ixSMU_STATUS`, `ixSMU_FIRMWARE`), GMUX alebo VT-d `DRHD` pred spustením Windows.
2. **H2 — Koexistencia `igdkmd64` / `00:02.0` / `DMAR` vo Windows pod HVCI:** Či `polaris10_start_smu` zlyhá v `setos` boote aj vtedy, keď je Intel HD 530 vo Windows zakázaný (najprv za behu, a potom aj **priamo pri štarte systému** tak, aby sa `igdkmd64.sys` vôbec nenačítal).

---

## Čo je už pripravené na USB `D:` a v projekte

- **UEFI sonda:** `D:\EFI\tools\mbp133_m1a_probe.efi` (zdroják [uefi/m1a_probe/mbp133_m1a_probe.c](../uefi/m1a_probe/mbp133_m1a_probe.c))
- **USB rEFInd:** `D:\EFI\BOOT\refind.conf` — `spoof_osx_version` je dočasne zakomentovaný, aby sonda najprv zachytila `[BEFORE_SETOS]` a až potom sama zavolala `AppleSetOs`. Predvolená položka je **`MBP13,3 M1A Probe (Polaris11 SMU7 + SetOS)`**.
- **Výstupné UEFI logy na USB:** `D:\M1A_LOGS\probe_*.txt`
- **Windows zber & izolačné testy:** [scripts/Run-M1A-Collect.ps1](../scripts/Run-M1A-Collect.ps1) (kópia aj v `D:\M1A_SCRIPTS\`)
- **Parser & Diff report:** [scripts/Parse-M1A-Probe.py](../scripts/Parse-M1A-Probe.py) → generuje `diag/m1a/M1A_ANALYSIS.md`

---

## Presný postup reštartov (krok za krokom)

### Krok 1 — Reštart 1: `SetOS 10.15` sonda + zber + hot-disable test
1. Reštartuj MacBook a drž kláves **`Option` (`Alt`)**.
2. Vyber oranžovú ikonu **`EFI Boot`** (USB `REFIND_USB`).
3. V rEFInd menu potvrď **`MBP13,3 M1A Probe (Polaris11 SMU7 + SetOS)`** (Enter).
4. V menu sondy stlač **`2`** (`SetOS 10.15 Probe (Before+After) -> save probe_setos15.txt -> Win`).
   - Sonda zachytí `BEFORE_SETOS`, zavolá `AppleSetOs("Mac OS X 10.15")`, zachytí `AFTER_SETOS_IMMEDIATE` a `AFTER_SETOS_500MS`, uloží `D:\M1A_LOGS\probe_setos15.txt` a po 3 sekundách spustí Windows.
5. Po prihlásení do Windows otvor PowerShell v priečinku repozitára a spusti:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\Run-M1A-Collect.ps1 -Action Collect
   powershell -ExecutionPolicy Bypass -File .\scripts\Run-M1A-Collect.ps1 -Action TestHotDisableIntel
   ```
6. **Príprava na Reštart 2 (boot-time izolácia bez `igdkmd64.sys`):**
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\Run-M1A-Collect.ps1 -Action DisableIntelForReboot
   ```
   *(Intel HD 530 zostane zakázaný v registri Windows pre nasledujúci `setos` štart.)*

### Krok 2 — Reštart 2: `SetOS 10.9` + boot bez načítania `igdkmd64.sys`
1. Reštartuj MacBook s **`Option` (`Alt`)** → **`EFI Boot`** → **`MBP13,3 M1A Probe`**.
2. V menu sondy stlač **`3`** (`SetOS 10.9 Probe (Before+After) -> save probe_setos09.txt -> Win`).
   - Týmto jedným reštartom otestuješ súčasne dve veci: verziu `"Mac OS X 10.9"` v UEFI **aj** štart `amduw23g` vo Windows bez načítaného `igdkmd64.sys`!
3. Po prihlásení do Windows spusti:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\Run-M1A-Collect.ps1 -Action Collect
   powershell -ExecutionPolicy Bypass -File .\scripts\Run-M1A-Collect.ps1 -Action EnableIntel
   ```
   *(Týmto Intel HD 530 opäť čisto povolíš.)*

### Krok 3 — Reštart 3: `Normal` baseline sonda + finálna analýza
1. Reštartuj MacBook s **`Option` (`Alt`)** → **`EFI Boot`** → **`MBP13,3 M1A Probe`**.
2. V menu sondy stlač **`1`** (`Normal Probe (NO AppleSetOs) -> save probe_normal.txt -> Win`).
   - Zachytí kompletný baseline v normálnom Boot Camp boote a nabootuje ťa späť do funkčného Windows s plnou AMD Radeon Pro 455.
3. Vo Windows spusti finálny zber a diff analýzu:
   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\Run-M1A-Collect.ps1 -Action Collect
   ```
4. Pozri výsledný report **`diag/m1a/M1A_ANALYSIS.md`** a pošli výsledky sem do chatu — podľa nich presne navrhneme Míľnik 1A fix!
