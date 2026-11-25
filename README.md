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
├── docs/               # Engineering specification, M1A, M1A-2 & M1B runbooks & verified findings
├── uefi/
│   ├── m1a_probe/      # Milestone 1A read-only UEFI telemetry probe (PCI/BAR5/SMU7/GMUX/IOMMU)
│   ├── m1a2_ebs/       # Milestone 1A-2 ExitBootServices() X-Ray hook & 3-register Dual-GPU Fix
│   └── m1b_igpu/       # Milestone 1B Intel-Only UEFI loader + SSDT_IGPU_MBP133 ACPI bridge
├── diag/
│   ├── acpi_setos/     # 24 raw .dat + decompiled .dsl ACPI tables from AppleSetOs boot
│   ├── acpi_normal/    # 24 raw .dat + decompiled .dsl + dmar.dsl from normal boot
│   ├── dump_1b0/       # WATCHDOG-20261125-1333.dmp, extracted blobs, and kd.exe analyses
│   ├── m1a/            # Milestone 1A UEFI probe logs, Windows snapshots & diff analysis
│   ├── m1a2/           # Milestone 1A-2 ExitBootServices() X-Ray reports & Dual-GPU verification logs
│   ├── m1b/            # Milestone 1B Intel-Only UEFI logs, Windows snapshots & battery power
│   └── pci_state_*.txt # PnP / D-state / driver stack snapshots for both boot modes
├── scripts/            # PowerShell helpers (Phase 0 diag, Polaris 11 Boot Camp fix, USB rEFInd)
├── tools/acpica/       # Intel ACPICA 20260408 Windows tools (acpidump, iasl, acpixtract)
└── reference/          # Primary upstream reference code (Linux apple-gmux/polaris10_smumgr, t2-gmux-uefi, apple_set_os)
```

## Next Milestones

1. **Milestone 1A (UEFI Telemetry & `igdkmd64.sys` Isolation — Completed):**
   - UEFI probe (`uefi/m1a_probe/mbp133_m1a_probe.c`), Windows collector/isolator (`scripts/Run-M1A-Collect.ps1`), diff analyzer (`scripts/Parse-M1A-Probe.py`), runbook ([`docs/MILESTONE_1A_RUNBOOK.md`](docs/MILESTONE_1A_RUNBOOK.md)), raw analysis ([`diag/m1a/M1A_ANALYSIS.md`](diag/m1a/M1A_ANALYSIS.md)), and **verified findings** ([`docs/MILESTONE_1A_FINDINGS.md`](docs/MILESTONE_1A_FINDINGS.md)):
   - **Proved:** `igdkmd64.sys` driver conflict (H2) is **100% disproven**; pre-`ExitBootServices` UEFI state is **100% identical** between `normal`, `SetOS 10.15`, and `SetOS 10.9`; Intel OpRegion already carries `$VBT SKYLAKE` and Indexed I/O GMUX (`0x55AA`, `0x04001D00`) is hardware-verified.
2. **Milestone 1B (Completed & Verified on Hardware — Intel-Only Mode 4 Works End-to-End):**
   - UEFI loader (`uefi/m1b_igpu/mbp133_m1b_igpu.c`), ACPI bridge (`uefi/m1b_igpu/SSDT_IGPU_MBP133.asl`), collector (`scripts/Run-M1B-Collect.ps1`), runbook ([`docs/MILESTONE_1B_RUNBOOK.md`](docs/MILESTONE_1B_RUNBOOK.md)), and **verified findings** ([`docs/MILESTONE_1B_FINDINGS.md`](docs/MILESTONE_1B_FINDINGS.md)).
   - **Proved:** Intel HD 530 (`00:02.0`) drives `2880×1800 @ 60 Hz` with Apple `$VBT SKYLAKE`, `DDI_BUF_CTL_A=0x91`, **`BDB[43] → NONE` smooth GMUX brightness**, **full `Standby (S3)` lid-close sleep/wake**, AMD `01:00.0` powered off (`GMUX 0x50=0x00`), and battery draw **`9.34 W`** (~7–8 W savings).
3. **Milestone 1A-2 (Completed & Verified on Hardware — Dual-GPU `Code 43` Solved!):**
   - `ExitBootServices()` X-Ray & Dual-GPU Fix (`uefi/m1a2_ebs/mbp133_m1a2_ebs.c`), ACPI `M1A2` reader (`scripts/Read-M1A2-Ebs.py`), runbook ([`docs/MILESTONE_1A2_RUNBOOK.md`](docs/MILESTONE_1A2_RUNBOOK.md)), and **verified findings** ([`docs/MILESTONE_1A2_FINDINGS.md`](docs/MILESTONE_1A2_FINDINGS.md)).
   - **Proved:** `AppleSetOs("Mac OS X 10.15")` skips Apple EFI's Boot Camp `ExitBootServices()` callback (keeping Intel `DEVEN.D2EN=1` instead of locking `HB80=1`), which left Polaris 11 VRAM unrelocated (`0x007F0000`). Writing all **3 Boot Camp `BAR5` registers post-EBS** (`MC_VM_FB_LOCATION=0xF47FF400`, `HDP_NONSURFACE_BASE=0xF4000000`, `GRPH_PRIMARY_SURFACE_ADDRESS_HIGH=0xF4` latched via `0x1A04`) brings up **Intel HD 530 + AMD Radeon Pro 455 concurrently with zero errors (`CM_PROB_NONE`)**, `2880×1800 @ 60 Hz`, working brightness slider (`101` levels), and full **`Standby (S3)` lid-close sleep/wake**.
4. **Milestone 2 (Hybrid Dual-GPU & Production rEFInd Profiles — Planned):** Combine M1B's Intel display GMUX switch + `BDB[43]=0` + `SSDT_IGPU_MBP133` with M1A-2's 3-register AMD fix for Intel panel display + on-demand AMD 3D offload, packaged as clean one-click rEFInd profiles.
