# Milestone 1A — Výsledky izolácie a UEFI telemetrie (`MacBookPro13,3`)

> [!IMPORTANT]
> Všetky tri plánované reštarty z [MILESTONE_1A_RUNBOOK.md](MILESTONE_1A_RUNBOOK.md) boli úspešne vykonané na reálnom hardvéri (`MacBookPro13,3`, EFI `529.120.1.0.0`, Windows 11 `26300`).
> Súhrnná tabuľka všetkých snapshotov a automatických diffov je v [diag/m1a/M1A_ANALYSIS.md](../diag/m1a/M1A_ANALYSIS.md).

---

## 1. Prehľad vykonaných testov a zozbieraných logov

| Krok | UEFI režim (`mbp133_m1a_probe.efi`) | Stav Intel HD 530 (`00:02.0`) vo Windows | Výsledok AMD Radeon Pro 455 (`01:00.0`) | Zachytené súbory |
| :--- | :--- | :--- | :--- | :--- |
| **Reboot 1** | `SetOS 10.15` | Povolený pri boote → potom hot-disable (`ProblemCode=22`) | **Code 43** (`0x1B0 / 0xC0000001`), nezmenené ani po hot-disable Intel + reštarte AMD | [probe_setos15.txt](../diag/m1a/uefi/probe_setos15.txt), [win_setos_20261125-160212.txt](../diag/m1a/win_setos_20261125-160212.txt), [win_setos_intel_disabled_20261125-160310.txt](../diag/m1a/win_setos_intel_disabled_20261125-160310.txt), [hot_disable_intel_result.txt](../diag/m1a/hot_disable_intel_result.txt) |
| **Reboot 2** | `SetOS 10.9` | **Zakázaný ešte pred štartom** (`ProblemCode=22`, `Stack=\Driver\ACPI, \Driver\pci`, `igdkmd64.sys` sa nenačítal) | **Code 43** (`0x1B0 / 0xC0000001`) — identické zlyhanie | [probe_setos09.txt](../diag/m1a/uefi/probe_setos09.txt), [win_setos_intel_disabled_20261125-160646.txt](../diag/m1a/win_setos_intel_disabled_20261125-160646.txt) |
| **Reboot 3** | `Normal` (bez `AppleSetOs`) | Skrytý firmvérom (`absent`) | **OK (`ProblemCode=0`)**, panel `APPA030` aktívny na AMD | [probe_normal.txt](../diag/m1a/uefi/probe_normal.txt), [win_normal_20261125-161104.txt](../diag/m1a/win_normal_20261125-161104.txt) |

*(Surové `WATCHDOG-20261125-*.dmp` a `Report_1b0_*.wer` zostávajú lokálne mimo git repozitára cez `.gitignore`.)*

---

## 2. Štyri hlavné dokázané zistenia

### 2.1 Hypotéza H2 (konflikt ovládača `igdkmd64.sys`) je na 100 % vyvrátená
- V Reboote 2 bol Intel HD 530 zakázaný už pred reštartom (`ProblemCode=22`, stack iba `\Driver\ACPI, \Driver\pci`, takže `igdkmd64.sys` sa vôbec nenačítal) a zároveň bola testovaná verzia `"Mac OS X 10.9"`.
- Napriek tomu AMD Radeon Pro 455 zlyhal s **úplne identickým** podpisom v troch nezávislých `WATCHDOG` live dumpoch (`1600.dmp`, `1603.dmp`, `1605.dmp`):
  - `BugCheck = 0x1B0`, `P1 = 0x2`, `P2 = 0xFFFFFFFFC0000001`, `P3 = 0x108` (`DxgkDdiStartDevice`).
  - Identický `0x900C1009` (`PP_Initialize` failure) a `0x900E560C` payload (`[0x10000, 0x102a2, 0x40, 0x2]`), časovanie od načítania VBIOS (`0x900C0E0D`) po pád `258.1 ms` (`1600.dmp`), `206.8 ms` (`1603.dmp`), `261.3 ms` (`1605.dmp`).
  - Identický 256 B `!RCA` PCI config block (`VID:DID=1002:67EF`, `REV=C7`, `SUBSYS=0166106B`, `BAR5=0x82600000`) naprieč všetkými troma dumpmi.
- Rovnako nepomohol ani hot-disable Intel HD 530 a reštart AMD cez `pnputil` ([hot_disable_intel_result.txt](../diag/m1a/hot_disable_intel_result.txt)).

### 2.2 Pre-`ExitBootServices` UEFI stav je na 100 % identický (`normal` vs `setos15` vs `setos09`)
- Porovnanie `probe_normal.txt:NORMAL`, `probe_setos15.txt` (`BEFORE_SETOS`, `AFTER_SETOS_IMMEDIATE`, `AFTER_SETOS_500MS`) a `probe_setos09.txt` (`BEFORE_SETOS`, `AFTER_SETOS_IMMEDIATE`, `AFTER_SETOS_500MS`) ukázalo **nulový rozdiel** na:
  - `00:00.0` Host Bridge (`GGC=0x000002C1`, `DEVEN=0x0000003F`, `D2EN=1`, `BDSM=0x7C000001`, `BGSM=0x7B800001`, `TSEGMB=0x7B000001`, `TOLUD=0x80000001`),
  - `00:01.0` `PEG0` (`CMD=0x0007`, `ECAM 0x444=0x00400002`, `0xC74=0x000001E3`; líši sa len dynamický dword na `+0xF0`),
  - `00:02.0` Intel HD 530 (`VID:DID=0x8086:191B`, `CMD=0x0007`, `BAR0=0x81000004`, `BAR2=0xA000000C`, `BAR4=0x5001`, `ASLS=0x7AF97018`),
  - `01:00.0` AMD PCI/ECAM/BAR5/SMU7 registre,
  - `GMUX` porty a `IOMMU` DRHD registre,
  - `gpu-power-prefs = 00 00 00 00` a `AppleSetOs` (`handles=1, Version=3`).
- **Záver:** `AppleSetOs` počas UEFI Boot Services iba nastaví interný príznak firmvéru; **skutočný rozchod správania medzi `normal` a `setos` vzniká až v `EVT_SIGNAL_EXIT_BOOT_SERVICES` callbacku Apple EFI** (kde firmvér pri `normal` boote skryje `00:02.0` cez `DEVEN.D2EN=0` a prípadne aplikuje ďalšie Boot Camp úpravy).

### 2.3 Polaris 11 SMU7 a VT-d IOMMU stav v UEFI
- **Polaris 11 SMU7 (`01:00.0` cez `BAR5=0x82600000`):** v oboch režimoch (`normal` aj `setos`) je SMC v UEFI iba v **SMC Boot ROM**:
  - `ixSMC_SYSCON_RESET_CNTL = 0x00000000`, `ixSMC_SYSCON_CLOCK_CNTL_0 = 0x01000000`,
  - `ixSMC_PC_C` napreduje okolo `0x2718..0x2AC8` (`ram_running=0`),
  - `ixRCU_UC_EVENTS = 0x000F0084` (`boot_seq_done=1`, `irq_en=1`),
  - `ixSMU_STATUS = 0x00000000` (`SMU_DONE=0`, `SMU_PASS=0`),
  - `ixSMU_FIRMWARE = 0x00030006` (`SMU_MODE=1`, `SMU_SEL=1` = protected mode + hard key), `ixSMU_EFUSE_49 = 0x00056B03` (`AVFS=1`).
  - `amdkmdag.sys` preto vždy spúšťa `polaris10_start_smu_in_protection_mode()` + `smu7_request_smu_load_fw()` až po štarte Windows.
- **VT-d IOMMU (`DMAR` DRHD):**
  - `DRHD@0xFED90000` (vyhradený pre `00:02.0` iGPU): `GSTS = 0x00000000`, `RTADDR = 0`.
  - `DRHD@0xFED91000` (`Flags=0x01`, `INCLUDE_PCI_ALL`, pokrýva `01:00.0` AMD): **`GSTS = 0xC3000000`** (`TE=1`, `RTPS=1`, `IRE=1`, `QIE=1`, `RTADDR = 0x7AEFB000`). V kombinácii s nálezom `DpiFdoCreateSysMmAdapter -> 0xC000000D` (`Seq 121`) z Fázy 0 ide o dôležitý vektor pre Milestone 1A-2.

### 2.4 Pripravenosť pre Milestone 1B (Intel-Only režim)
Telemetria z `probe_normal.txt` výrazne zjednodušila implementáciu **Milestone 1B**:
1. **Intel OpRegion Mailbox 4 už obsahuje reálny Skylake VBT:**
   - `ASLS = 0x7AF97018`, `OpRegion sig="IntelGraphicsMem"`, `sizeKB=8`, `rvda=0`, `rvds=0`,
   - `MB4[0..15] = 24 56 42 54 20 53 4B 59 4C 41 4B 45 20 20 20 20` (`"$VBT SKYLAKE    "`).
2. **`IGPU MMIO` overené:** `PWR_WELL_CTL2 = 0x50000001`, **`DDI_BUF_CTL_A = 0x00000081`** (bit 4 `DDI_A_4_LANES` je `0`, preto ho loader musí nastaviť na `1`), `DP_AUX_CTL_A = 0x0000023F`, `PP_STATUS = 0x00000000`, `PP_CTRL = 0x00000008`.
3. **Presný 128 B EDID interného panelu `APPA030` (`Color LCD`)** zachytený cez `EFI_EDID_ACTIVE_PROTOCOL` aj `EFI_EDID_DISCOVERED_PROTOCOL`.
4. **Indexed I/O GMUX (`Renesas R4F2113`) hardvérovo overený:**
   - Probe signature `0x55AA`, `VERSION(0x04) = 0x04001D00` (`bytes=[00 1D 00 04]`),
   - `SWITCH_DISPLAY(0x10) = 0x03 [DIS/AMD]`, `IRQ_ENABLE(0x14) = 0x00`, `IRQ_STATUS(0x16) = 0x05`, `SWITCH_DDC(0x28) = 0x02`, `SWITCH_EXTERNAL(0x40) = 0x01`, `DISCRETE_POWER(0x50) = 0x03`,
   - `MAX_BRIGHTNESS(0x70) = 0x000003FF`, `BRIGHTNESS(0x74) = 0x000000FE`.

---

## 3. Zostávajúce dve hypotézy pre `Code 43` (cieľ pre neskorší Milestone 1A-2)

Po vyvrátení H2 a zhodnosti pre-`ExitBootServices` UEFI stavu zostávajú už len dve možné príčiny pádov `polaris10_start_smu`:
1. **Príčina A (vedľajší efekt `ExitBootServices` v Apple EFI):** Pri `IsMacOS == TRUE` Apple EFI v `ExitBootServices` preskočí niektorý Boot Camp fixup (napr. pre `PEG0`/`AMD`/VT-d `0xFED91000`).
2. **Príčina B (reakcia Windows / `amdkmdag` na viditeľné PCI zariadenie `00:02.0`):** Aj pri zakázanom `igfx` (`ProblemCode=22`) je `00:02.0` na PCI zbernici, čo môže zmeniť správanie `pci.sys`, `IOMMU`/`SysMm`, `dxgkrnl` alebo `amdkmdag`.

> [!NOTE]
> Najbližší praktický krok pred 1A-2 je **Milestone 1B (Intel-Only režim cez GMUX)** — overiť plnohodnotný štart Windows na Intel HD 530 s odpojenou AMD vetvou a zmerať úsporu spotreby na batérii. Ak bude úspešný, uložíme konfiguráciu; ak nie, zdokumentujeme nález a vrátime sa k **Milestone 1A-2**.
