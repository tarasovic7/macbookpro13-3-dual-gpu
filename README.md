# MacBookPro13,3 Windows 11 Dual-GPU Hack (Intel HD 530 + AMD Radeon Pro 455)

Engineering research, firmware/ACPI forensics, and UEFI/SSDT blueprint for enabling **Intel HD Graphics 530** (`PCI 00:02.0`) alongside **AMD Radeon Pro 455** (`Polaris 11`, `PCI 01:00.0`) on an Apple **MacBookPro13,3** (15" Late 2016 Touch Bar, **Apple T1** — pre-T2 Indexed I/O GMUX `Renesas R4F2113`) running **Windows 11 Boot Camp** (with HVCI enabled).

> **Interactive Checkpoint & Full Documentation:** Open [`index.html`](index.html) locally in any browser (100% offline, searchable, links directly to all dumps, decompiled ACPI tables, scripts, and reference sources), or read the Markdown specification in [`docs/macbookpro13_3_windows_dual_gpu_spec.md`](docs/macbookpro13_3_windows_dual_gpu_spec.md).

## Key Findings (Phase 0 Forensics Complete)

- **Root Cause of Polaris 11 `Code 43` (`BugCheck 0x1B0 / P1=0x2 / P2=0xC0000001`) after `AppleSetOs`:**
  - Decoded from live dump `WATCHDOG-20261125-1333.dmp` + AMD LZNT1 `!RCA` `CollectDbgInfo` blob (`Ev=0x900C1009`) + `amdkmdag.sys` disassembly:
  - Fails inside `PP_Initialize` with **`"PP Lib initialization failed"`** (`failure_bitmask = 0x0000000000000801`, Bit 0 + **Bit 11**).
  - The sole writer of Bit 11 (`amdkmdag+0x1718329`) is **`smum_start_smc` -> `polaris10_start_smu`** (~269 ms SMU7 handshake/start timeout).
  - **Ruled out:** `VFCT` VBIOS (`60,416 B`, `113-C9801AP-028`), PCI Config Space (`PCI_CMD=0x0407`, `D0`, `Gen3 x8`, `MSI=0x0081`), all 4 AMD BARs (`0xB0000000`, `0xC0000000`, `0x4000`, `0x82600000`), and all 24 ACPI tables (`DSDT`, `SSDT1..12`, `VFCT`, `DMAR`, `MCFG`, `XSDT`) are **100% byte-for-byte identical** between normal Boot Camp boot and `AppleSetOs` boot.
- **Verified `MacBookPro13,3` ACPI Differences vs. T2 (`Osaul4ik/t2-gmux-uefi`):**
  - GMUX is **Pre-T2 Indexed I/O (`Renesas R4F2113`)** on ports `0x0700–0x07FE` (`0x7C2`/`0x7D0`/`0x7D4`), not T2 MMIO (`0xFE0B0200`).
  - `Device (IGPU)` lives directly in `DSDT` (not `SaSsdt`), and brightness `_BCM` is on `\_SB.PCI0.IGPU.DD01._BCM` (`DD1F` has no `_BCM`).
  - `ssdt1.dsl` (`TableId="PEG0GFX0"`) already ships Apple's native Indexed I/O GMUX AML helpers: `\_SB.PCI0.PEG0.GFX0.SGMX(Port, Len, Val)`, `ABCM(Arg0)`, `PWRD`, `PUPD`, and `GXWA`.

## Repository Layout

```text
├── index.html          # Main offline interactive checkpoint & technical documentation
├── docs/               # Full Markdown engineering specification
├── diag/
│   ├── acpi_setos/     # 24 raw .dat + decompiled .dsl ACPI tables from AppleSetOs boot
│   ├── acpi_normal/    # 24 raw .dat + decompiled .dsl + dmar.dsl from normal boot
│   ├── dump_1b0/       # WATCHDOG-20261125-1333.dmp, extracted blobs, and kd.exe analyses
│   └── pci_state_*.txt # PnP / D-state / driver stack snapshots for both boot modes
├── scripts/            # PowerShell helpers (Phase 0 diag, Polaris 11 Boot Camp fix, USB rEFInd)
├── tools/acpica/       # Intel ACPICA 20260408 Windows tools (acpidump, iasl, acpixtract)
└── reference/          # Primary upstream reference code (Linux apple-gmux/polaris10_smumgr, t2-gmux-uefi, apple_set_os)
```

## Next Milestones

1. **Milestone 1A (Fix `polaris10_start_smu` -> Hybrid Dual-GPU):** Isolate EFI SMU7 state (`BAR5` indirect register probe in UEFI) vs. `00:02.0`/`DMAR`/`igdkmd64` coexistence.
2. **Milestone 1B (Port `t2-gmux-uefi` Mode 4 -> Intel-Only Power Saving):** Implement `gmux_indexed.c` (`0x7C2/0x7D0/0x7D4`), adapt `int_acpi.c` for `DSDT` `DD01._BCM`, and compile `SSDT_IGPU_MBP133.aml` calling native `PEG0.GFX0.SGMX`/`ABCM`.
3. **Milestone 2 (Full Mode 5 Hybrid):** Intel HD 530 primary display (desktop/Electron) + AMD Radeon Pro 455 dedicated to 3D/Compute workloads.
