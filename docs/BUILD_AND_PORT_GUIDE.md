# Build, Clean USB Deployment & AI-Agent Porting Guide

This guide explains:
1. How to compile and deploy the clean **4-mode Windows Dual-GPU boot USB** for **Apple MacBookPro13,3** (Windows 11 + HVCI enabled), and how to switch between the clean daily USB (`clean`) and full diagnostic toolkit (`dev`).
2. How to **port this solution to another MacBook Pro model** (e.g., `MacBookPro14,3`, `15,1`, `16,1`) using your own AI coding agent.

> [!CAUTION]
> **Safety Rule:** Always test on an external FAT32 USB rEFInd drive (`REFIND_USB`). **Never** modify the internal NVMe EFI System Partition (`bootmgfw.efi`) — unplugging the USB always gives you an instant stock Boot Camp fallback.

---

## Part 1: Building & Deploying (`MacBookPro13,3`)

### 1.1 Prerequisites
- **Windows 11** with **WSL 2** (`Ubuntu` or Debian) for UEFI compilation:
  ```bash
  sudo apt-get update && sudo apt-get install -y build-essential gnu-efi binutils file
  ```
- **Intel ACPICA (`iasl.exe`)**: already bundled in [`tools/acpica/iasl.exe`](../tools/acpica/iasl.exe) (or `sudo apt install acpica-tools` in WSL).
- **USB Flash Drive (`REFIND_USB`, FAT32)** with [rEFInd](https://www.rodsbooks.com/refind/) installed at `\EFI\BOOT\BOOTX64.EFI`.

### 1.2 Compile the UEFI Loader & ACPI Tables
Run from the repository root in PowerShell:

```powershell
# 1. Build Unified Milestone 2 UEFI loader inside WSL
wsl -- bash -lc "cd /mnt/c/path/to/mbp133-dual-gpu-hack/uefi/m2_hybrid && make clean && make && nm -u build/mbp133_m2_hybrid.so && readelf -r build/mbp133_m2_hybrid.so && file build/mbp133_m2_hybrid.efi"

# 2. Verify: nm -u MUST be empty, readelf -r MUST say "There are no relocations in this file."
#    Free WSL memory immediately if desired:
wsl --shutdown

# 3. Compile both ACPI SSDT tables (Hybrid S3 bridge + Intel-Only S3 bridge)
.\tools\acpica\iasl.exe -p uefi\m2_hybrid\build\SSDT_HYBRID_MBP133 uefi\m2_hybrid\SSDT_HYBRID_MBP133.asl
.\tools\acpica\iasl.exe -p uefi\m1b_igpu\build\SSDT_IGPU_MBP133    uefi\m1b_igpu\SSDT_IGPU_MBP133.asl
```

> [!NOTE]
> **Zero-relocation rule for GNU-EFI:** Keep [`uefi/m2_hybrid/mbp133_m2_hybrid.c`](../uefi/m2_hybrid/mbp133_m2_hybrid.c) free of static string-pointer arrays (`static const char *arr[]`). Use 2D `char arr[][N]` or inline pointers so `readelf -r` outputs **0 relocations**.

### 1.3 Deploy to USB (`clean` vs `dev`)
[`scripts/Deploy-USB.ps1`](../scripts/Deploy-USB.ps1) deploys verified SHA-256 copies onto your mounted USB drive (default `D:`):

```powershell
# Clean daily-driver USB (only 3 files in D:\EFI\tools\ + 4 clean rEFInd entries)
.\scripts\Deploy-USB.ps1 -Profile clean -UsbDrive D:

# Restore full developer/diagnostic tools (after a Windows Update, driver upgrade, etc.)
.\scripts\Deploy-USB.ps1 -Profile dev -UsbDrive D:
```

### 1.4 The 4 rEFInd Boot Modes & Automatic NVRAM Switching
The clean [`scripts/usb_refind/refind.conf`](../scripts/usb_refind/refind.conf) exposes 4 modes:

| rEFInd Menu Entry | When to Use | What Happens Automatically |
|---|---|---|
| **1. Hybrid `[Recommended]`** *(Default)* | **Daily driver:** Intel HD 530 drives `2880×1800 @ 60 Hz` (`dwm`, desktop, browsers — zero lag, `101`-level brightness, S3 sleep/wake) and AMD delivers 3D/compute performance when apps request it. | If Apple NVRAM `gpu-power-prefs` is not `iGPU`, the loader writes `01 00 00 00` and cold-reboots once automatically. Patches `VFCT` (removes internal `eDP`), renames `PEG0GFX0._DOD -> XDOD`, loads `SSDT_HYBRID_MBP133.aml`, and relocates AMD `BAR5` post-`ExitBootServices()`. |
| **2. Intel Only** | **Long battery sessions (~9.3 W):** AMD dGPU power rail is switched OFF (`GMUX 0x50 = 0x00`) when you know you won't need dGPU performance. | Works regardless of `gpu-power-prefs` (no reboot needed). Injects `SSDT_IGPU_MBP133.aml` so S3 wake keeps AMD rail OFF. |
| **3. Boot Camp Default (AMD Only)** | **Stock Boot Camp:** Pure AMD Radeon Pro 455 display output (Intel hidden by Apple EFI), e.g., if an anti-cheat or app insists on stock Apple Boot Camp. | If `gpu-power-prefs` is set to `iGPU`, the loader deletes it (`dGPU` default) and cold-reboots once automatically, then chainloads Windows unmodified. |
| **4. AMD Display + Intel Helper `[Experts Only]`** | **Specific dual-GPU routing:** AMD drives the Retina panel directly while Intel HD 530 stays alive headless for Intel QuickSync / DXGI minimum-power. Not needed for normal users. | Auto-clears `gpu-power-prefs` to `dGPU` if needed, calls `AppleSetOs("Mac OS X 10.15")`, and applies the 3-register AMD `BAR5` fix post-`ExitBootServices()`. |

**Rollback `gpu-power-prefs` anytime:** Boot **Mode 3 (Boot Camp Default)** once, open the interactive menu and press `[D]`, boot **macOS**, or press **`Cmd + Option + P + R`** at startup.

### 1.5 Optional: Installing to the Internal NVMe ESP (No USB Needed)
Once you have verified the USB setup works reliably on your hardware, [`scripts/Install-InternalESP.ps1`](../scripts/Install-InternalESP.ps1) installs rEFInd and the 3 production binaries cleanly onto the internal NVMe EFI System Partition (`\EFI\refind\` + `\EFI\tools\`) using the 3-mode [`scripts/usb_refind/refind_internal.conf`](../scripts/usb_refind/refind_internal.conf) — **without modifying `\EFI\Microsoft\Boot\bootmgfw.efi` or `\EFI\APPLE\`**:

```powershell
# Run in an Administrator PowerShell (with REFIND_USB mounted at D:)
powershell -ExecutionPolicy Bypass -File .\scripts\Install-InternalESP.ps1 -SetDefault

# Clean rollback at any time:
powershell -ExecutionPolicy Bypass -File .\scripts\Install-InternalESP.ps1 -Uninstall
```

> [!NOTE]
> On `MacBookPro13,3`, Apple's EFI NVRAM `efi-boot-device` sometimes overrides Windows `bcdedit`. If rebooting without the USB still jumps straight into Windows instead of rEFInd, boot **macOS** once and run:
> ```bash
> sudo diskutil mount disk0s1
> sudo bless --mount /Volumes/EFI --setBoot --file /Volumes/EFI/EFI/refind/refind_x64.efi --shortform
> ```

---

## Part 2: Porting to Another MacBook Pro With Your AI Agent

This repository's diagnostics and UEFI patterns can be adapted by an AI coding agent to other Intel+AMD dual-GPU MacBook Pros (`MacBookPro11,5`, `14,3`, `15,1`, `15,3`, `16,1`, etc.).

### 2.1 Hardware Differences Your AI Agent Must Discover First

| Subsystem | `MacBookPro13,3` (2016 Skylake + Polaris 11) | What Varies on Other MacBook Pros |
|---|---|---|
| **GMUX Type** | Pre-T2 Indexed I/O (`Renesas R4F2113` on ports `0x7C2`/`0x7D0`/`0x7D4`, probe `0x7CC..0x7CE = 0x55AA`) | **2018–2020 T2 Macs (`15,1`/`16,1`)** use **MMIO GMUX** at `0xFE0B0200` (see `reference/t2_gmux_uefi` / `apple-gmux` Linux driver). **2013–2015 Macs (`11,3`/`11,5`)** use direct byte-port Classic I/O GMUX (`0x710`–`0x750`). |
| **Post-EBS dGPU Fix** | Polaris 11 `BAR5` 3-register relocation: `mmMC_VM_FB_LOCATION (0x0809)=0xF47FF400`, `mmHDP_NONSURFACE_BASE (0x0B01)=0xF4000000`, `mmGRPH_PRIMARY_SURF_HIGH (0x1A07)=0xF4` latched via `0x1A04` | **Different AMD families (`Polaris 10/20`, `Vega 16/20`, `Navi 14`)** use different MMIO register offsets or memory aperture addresses! **Never hardcode `0xF47FF400` blindly** — capture an `ExitBootServices()` before/after register diff on a **Normal Boot Camp** boot (`m1a2_ebs` pattern) on the target machine first! |
| **Backlight Control** | iGPU OpRegion `$VBT` `BDB[43]` cleared to `0` (`NONE`) + `_BCM` -> `XBCM` forwarded to `\_SB.PCI0.PEG0.GFX0.ABCM` | Check `dsdt.dsl` / SSDTs for panel child (`DD01`, `DD02`, `DD1F`, `LCD0`) and native GMUX AML helper name (`ABCM`, `SGMX`, or T2 brightness protocol). |
| **ACPI `_DOD` Device** | `ssdt1.dsl` (`OEM Table ID = PEG0GFX0`) defines `_DOD -> { 0x0110 }` + `ABCM`/`SGMX` | Dump all ACPI tables (`acpidump.exe`) and check which SSDT/DSDT scope owns dGPU `_DOD` (`PEG0.GFX0`, `PEG0.PEGP`, `RP01.PXSX`) so you only rename `_DOD`, never deleting GMUX helpers. |
| **`VFCT` VBIOS Connector Table** | ATOMBIOS `Object_Header_V3` (`MDT[22]`): clear `LCD1` bit `0x0002`, remove `0x3114` (`eDP`) from `DisplayPathTable` / `ConnectorObjectTable` and orphaned encoder | Newer AMD VBIOSes (`Vega` / `Navi`) may use **AtomFirmware** headers instead of legacy **ATOMBIOS `Object_Header_V3`**. Inspect live `VFCT` first and verify whether Windows `dxgkrnl!DpiFdoEnumChildDevices` fails with `0xC00000BB` (`TypeIntegratedDisplay`). |
| **Apple NVRAM `gpu-power-prefs`** | `FA4CE28D-B62F-4C99-9CC3-6815686E30F9 : gpu-power-prefs` (`01 00 00 00` = iGPU) | Some firmware revisions ignore or lock `gpu-power-prefs` unless `AppleSetOs` or T2 BridgeOS allows it; always readback-verify before relying on it. |

### 2.2 Copy-Paste Prompt for Another User's AI Agent

Give this prompt to an AI coding agent inside a clone of this repository on the target MacBook Pro (Boot Camp Windows 11):

```text
We are porting the MacBookPro13,3 Hybrid Dual-GPU hack in this repo to my MacBook model (<YOUR_MODEL_ID>, e.g. MacBookPro15,1) running Windows 11 Boot Camp.

Strict rules:
1. NEVER modify the internal NVMe EFI partition (bootmgfw.efi). Work strictly on an external FAT32 USB rEFInd drive.
2. NEVER guess hardware addresses, register offsets, or ACPI paths — measure everything on this exact hardware first.
3. Verify every compiled .efi binary has empty `nm -u`, 0 ELF relocations (`readelf -r`), and `sum8 == 0` on every patched ACPI/VBIOS table.

Execute in strict phases, waiting for my reboot confirmation between UEFI tests:
- Phase 0 (Read-Only Forensics): Run `acpidump.exe` + `iasl -d`, capture PnP Display/Monitor IDs and any `C:\Windows\LiveKernelReports\WATCHDOG\*.dmp` crashes under normal Boot Camp and AppleSetOs. Identify my GMUX type (Classic I/O vs Indexed I/O vs T2 MMIO), iGPU brightness device (`_BCM`), dGPU PCI IDs/BARs, `VFCT` table format, and whether `ABCM`/`SGMX` exist in ACPI.
- Phase 1 (`ExitBootServices()` X-Ray): Adapt `uefi/m1a2_ebs/` to dump my Host Bridge (`DEVEN`/`HB80`) and AMD BAR registers BEFORE and AFTER `ExitBootServices()` on Normal Boot Camp vs AppleSetOs("Mac OS X 10.15"). Diff them to find the exact post-EBS registers Apple EFI touches for my dGPU.
- Phase 2 (`iGPU Primary + Secondary dGPU`): Port `uefi/m2_hybrid/` using my measured post-EBS dGPU fix, GMUX switch, VBT `BDB[43]` patch, and `gpu-power-prefs` switcher. If the secondary dGPU still shows Code 43 while Intel works, capture a `Microsoft-Windows-DxgKrnl` ETW trace (`logman`) during `pnputil /restart-device` to check whether `DxgkDdiStartDevice` succeeded and `DpiFdoEnumChildDevices` rejected `TypeIntegratedDisplay (0xC00000BB)`; if so, parse and patch my `VFCT` + dGPU `_DOD` dynamically.
- Phase 3 (S3 Sleep/Wake): Ensure `SSDT_HYBRID_*.aml` restores GMUX display routing and brightness on `_WAK` without cutting dGPU power (`0x50` / GMUX rail) across S3 suspend/resume.
```

> [!TIP]
> Also point your agent to [`AGENTS.md`](../AGENTS.md) and [`docs/undocumented_hardware_findings_en.html`](undocumented_hardware_findings_en.html) for the full undocumented register & ACPI map.

---

## Credits & Community Note

Original hardware research and Hybrid Dual-GPU implementation by **Martin Tarasovič** ([`github.com/tarasovic7`](https://github.com/tarasovic7) · [`tarasovic7/macbookpro13-3-dual-gpu`](https://github.com/tarasovic7/macbookpro13-3-dual-gpu)).

If you or your AI agent succeeded in porting or using this project, please consider starring the GitHub repository and sharing your results on **X (x.com)**, **LinkedIn**, or **Facebook** with credit!
