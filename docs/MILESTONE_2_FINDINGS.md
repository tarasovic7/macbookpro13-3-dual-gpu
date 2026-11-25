# Milestone 2 — Verified Findings: Hybrid Mode 5 (Intel HD 530 Primary Display + AMD Radeon Pro 455 Active) on `MacBookPro13,3`

> [!IMPORTANT]
> **Status: COMPLETED & HARDWARE-VERIFIED END-TO-END.**
> Booting Windows 11 Pro (`10.0.26300`, HVCI enabled) via [`uefi/m2_hybrid/mbp133_m2_hybrid.c`](../uefi/m2_hybrid/mbp133_m2_hybrid.c) **Mode `[1]` (`Hybrid Mode 5`)** drives the internal `2880×1800 @ 60 Hz` Retina panel directly on **Intel HD Graphics 530 (`00:02.0`)** (zero cross-adapter UI lag, smooth `101`-level GMUX brightness) while keeping **AMD Radeon Pro 455 / RX 560X (`01:00.0`)** concurrently powered ON and **`OK` (`CM_PROB_NONE`, Error `0`)** for DXGI `HIGH_PERFORMANCE` offload and external Thunderbolt 3 DisplayPort outputs — surviving **`Standby (S3)` lid-close sleep and resume** cleanly.

---

## 1. Executive Summary & Live Verified Hardware State

Evidence captured **after S3 sleep/wake** (`21:17:11 -> 21:17:30`) in [`diag/m2/m2_mode1_hybrid_m2_hybrid_s3o_20261125-211821.txt`](../diag/m2/m2_mode1_hybrid_m2_hybrid_s3o_20261125-211821.txt) and [`diag/m2/m2_mode1_hybrid_m2_hybrid_s3o_20261125-211821_uefi.txt`](../diag/m2/m2_mode1_hybrid_m2_hybrid_s3o_20261125-211821_uefi.txt):

| Component / Check | Verified State | Evidence / Key Metrics |
|---|---|---|
| **Intel(R) HD Graphics 530** (`00:02.0`) | **`OK`** (`CM_PROB_NONE`, Error `0`) | Primary display adapter (`2880×1800 @ 60 Hz`), driver `31.0.101.2115`, `VRAM=1024MB`, DXGI `#0` for `UNSPECIFIED` & `MINIMUM_POWER`; hosts `dwm` (`333.2 MB`), `Antigravity` (`120 MB`), `csrss` (`85 MB`), `WindowsTerminal` (`47.2 MB`) — zero lag |
| **AMD Radeon RX 560X / Pro 455** (`01:00.0`) | **`OK`** (`CM_PROB_NONE`, Error `0`) | Secondary headless/offload GPU (`Mode=x@Hz`, `VRAM=2048MB`), driver `31.0.23013.14001`, DXGI `#0` for `HIGH_PERFORMANCE` (`1991 MB` dedicated / `8140 MB` shared), idle usage only `5.7 MB` dedicated / `0.5 MB` shared |
| **ACPI `M2HY` & Flags** | **`Flags=0x00001bdf` (`sum8=0`)** | `BEFORE_EBS\|AFTER_EBS\|POST_ACTION\|SETOS_OK\|GMUX_IGD_OK\|DDI_4LANES_OK\|VBT_BDB43_OK\|SSDT_INJECTED\|FBLOC_3REG_OK\|VFCT_PATCHED\|GFX0_DOD_PATCHED`, `EBS Calls=1`, `Retries=0` |
| **Patched ACPI `VFCT`** | **`dev_sup=0x0688`, `dp(4)`, `conn(4)`** | `len=60548`, eDP/LCD1 (`0x3114`, encoder `0x221E`) removed; 4 external TB3 DP connectors `[0x3113, 0x3213, 0x3313, 0x3413]` kept; `vb_sum8=0`, `vfct_sum8=0` |
| **Patched `PEG0GFX0` + `SSDT_HYBRID_MBP133`** | **Injected (`453 B`)** | `_DOD -> XDOD` @ `0x04B9` (`csum=0xB3`); `ABCM` and `SGMX` kept intact for `SSDT_HYBRID_MBP133.aml` (`MBP133/M2HYBCM`) |
| **Internal Panel & Brightness** | **`OK` (`101` levels)** | `Integrated Monitor (Color LCD)` (`DISPLAY\APPA030\4&885D6EB&0&UID265988`), `WmiMonitorBrightness`: `Current=70`, `Levels=101`, smooth GMUX brightness slider |
| **S3 Sleep / Wake** | **`Standby (S3)` Verified** | `powercfg /a`: `Standby (S3)` + `Hibernate`; Event Log: `Kernel-Power 42` (`21:17:11`) -> `Kernel-Power 107` (`21:17:17`) -> `Power-Troubleshooter 1` (`21:17:30`); both Intel & AMD remain `OK` after resume |

---

## 2. The 4 Engineering Barriers Solved for Hybrid Mode 5

1. **POST / GOP Display Handoff Surprise-Removal Black Screen (`Event ID 1011`)**
   - When Apple NVRAM `gpu-power-prefs` is unset (`dGPU` default), Apple EFI boots Polaris 11 as the GOP display adapter; switching GMUX to Intel before `ExitBootServices()` causes Windows PnP to surprise-remove both display adapters (`ID 1011`) during driver handoff.
   - **Fix:** Built an interactive Apple NVRAM `gpu-power-prefs` (`FA4CE28D-B62F-4C99-9CC3-6815686E30F9`) switcher (`[S]`/`[D]` with readback verification + cold reboot) directly into `mbp133_m2_hybrid.efi` (`01 00 00 00` = iGPU). Apple EFI then initializes Intel HD 530 natively (`GMUX 0x10=0x02`, `DDI_A=0x80000017`, `PP_STATUS=0x80000008`).

2. **Post-`ExitBootServices()` Polaris 11 VRAM Relocation**
   - Because `AppleSetOs("Mac OS X 10.15")` keeps `DEVEN.D2EN=1` by skipping Apple EFI's Boot Camp `ExitBootServices()` callback, `mbp133_m2_hybrid.efi`'s `HookedExitBootServices()` applies Milestone 1A-2's 3-register `BAR5` (`0x82600000`) sequence immediately after `ExitBootServices()` returns `EFI_SUCCESS`:
     - `mmMC_VM_FB_LOCATION (0x0809)` = `0xF47FF400`
     - `mmHDP_NONSURFACE_BASE (0x0B01)` = `0xF4000000`
     - `mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH (0x1A07)` = `0x000000F4` (latched via `0x1A04`)
   - Verified via live `Microsoft-Windows-DxgKrnl` ETW trace: `amduw23g` (`amdkmdag.sys`) completes `DxgkDdiStartDevice` with **`STATUS_SUCCESS`**.

3. **Secondary-Adapter `Code 43` Inside `dxgkrnl!DpiFdoEnumChildDevices` (`0xC00000BB`)**
   - Even after `amduw23g` passed `DxgkDdiStartDevice`, Windows failed AMD with **`CM_PROB_FAILED_POST_START` (`Code 43`)**.
   - A live `Microsoft-Windows-DxgKrnl` ETW trace + `kdX64.exe` disassembly of `C:\Windows\System32\drivers\dxgkrnl.sys` (`DpiFdoEnumChildDevices+0x3b7..0x3eb`) proved `StartAdapter_DpiFdoEnumChildDevicesFailed` returned **`STATUS_NOT_SUPPORTED (0xC00000BB)`** 0.3 ms after `DxgkDdiStartDevice` succeeded: Windows 11 `dxgkrnl.sys` explicitly rejects `ChildDeviceType == TypeIntegratedDisplay (3)` (`D3DKMDT_VOT_INTERNAL = 0x80000000`, `AcpiUid=0x0110`) on a non-POST adapter without a DisplayMux.
   - **Fix (`Mode [1]` only):**
     - `PatchVfctRemoveEdp()` copies ACPI `VFCT` (`60,548 B`) into `EfiACPIReclaimMemory`, parses `ATOM_OBJECT_HEADER_V3` (`MDT[22]`), clears `ATOM_DEVICE_LCD1_SUPPORT` (`0x0002` in `usDeviceSupport`: `0x068A -> 0x0688`), removes the internal eDP entry (`DevTag=0x0002` / `0x3114`) from `DisplayPathTable` (`5 -> 4`), `ConnectorObjectTable` (`5 -> 4`), and its unshared encoder (`0x221E`) from `EncoderObjectTable` (`5 -> 4`), and recomputes both VBIOS ROM checksum (`vb[0x21]=0x32`) and `VFCT` ACPI checksum (`0x62`).
     - `PatchPeg0Gfx0Dod()` copies `SSDT` `PEG0GFX0` into `EfiACPIReclaimMemory` and renames `_DOD -> XDOD` (`@0x04B9`, `csum=0xB3`) so `\_SB.PCI0.PEG0.GFX0` does not advertise `0x0110`, while preserving `ABCM` and `SGMX`.

4. **S3 Sleep/Wake dGPU Disappearance (`CM_PROB_PHANTOM`)**
   - Reusing `SSDT_IGPU_MBP133.aml` (designed for `Intel-Only Mode 4`) caused AMD (`01:00.0` + `01:00.1`) to vanish after S3 wake (`CM_PROB_PHANTOM`), because Windows places `PEG0`/`GFX0` into PCIe D3 before invoking `_PTS` (`VID0 == 0xFFFF`), causing `SSDT_IGPU_MBP133.aml` to misdetect a powered-off dGPU and cut GMUX port `0x50` during `_WAK`.
   - **Fix:** Created dedicated [`uefi/m2_hybrid/SSDT_HYBRID_MBP133.asl`](../uefi/m2_hybrid/SSDT_HYBRID_MBP133.asl) (`MBP133/M2HYBCM`) loaded exclusively in `Mode [1]`, bridging `_BCM -> ABCM` and re-applying GMUX display routing (`0x28`, `0x10`, `0x40`) in `_WAK` **without ever touching port `0x750` (`0x50`)**. `Mode [2]` (`Intel-Only`) and [`mbp133_m1b_igpu.efi`](../uefi/m1b_igpu/mbp133_m1b_igpu.c) remain on untouched `SSDT_IGPU_MBP133.aml`.

---

## 3. Hardware Run Chronology (`diag/m2/`)

| Run | Mode / Build | Key Behavior Observed | Windows Outcome | Log |
|---|---|---|---|---|
| **1** | `Mode [2]` (`Intel-Only`, `iGPU` NVRAM unset) | Validates unified M2 loader path for Mode 2 (`Flags=0x000001ff`) | Intel **`OK`** (`2880×1800@60Hz`), AMD Rail OFF | [`m2_mode2_intel_only_m2_mode2_check_20261125-183831.txt`](../diag/m2/m2_mode2_intel_only_m2_mode2_check_20261125-183831.txt) |
| **2** | `Mode [1]` (`Hybrid`, `gpu-power-prefs=iGPU`, pre-VFCT fix) | Intel booted natively + `FBLOC_3REG_OK` passed; ETW + `kdX64.exe` isolated `DpiFdoEnumChildDevices 0xC00000BB` | Intel **`OK`**, AMD **`Code 43`** | [`m2_mode1_hybrid_m2_hybrid_igpupref_20261125-185102.txt`](../diag/m2/m2_mode1_hybrid_m2_hybrid_igpupref_20261125-185102.txt) |
| **3** | `Mode [1]` (`+ PatchVfctRemoveEdp + PatchPeg0Gfx0Dod`) | Both GPUs up, `dwm`/`Antigravity` on Intel, zero lag; `SSDT_IGPU_MBP133` later dropped AMD after S3 wake (`CM_PROB_PHANTOM`) | Intel **`OK`** + AMD **`OK`** pre-sleep | [`m2_mode1_hybrid_m2_hybrid_vfct_20261125-202509.txt`](../diag/m2/m2_mode1_hybrid_m2_hybrid_vfct_20261125-202509.txt) |
| **4** | **`Mode [1]` (`+ SSDT_HYBRID_MBP133.aml`)** | **Full end-to-end pass BEFORE and AFTER lid-close `Standby (S3)` wake**, `Levels=101` brightness, zero lag | **Intel `OK` + AMD `OK` post-S3 wake** | [`m2_mode1_hybrid_m2_hybrid_s3o_20261125-211821.txt`](../diag/m2/m2_mode1_hybrid_m2_hybrid_s3o_20261125-211821.txt) |

---

## 4. Delivered Files & Rollback Safety

- [`uefi/m2_hybrid/mbp133_m2_hybrid.c`](../uefi/m2_hybrid/mbp133_m2_hybrid.c) — Unified 4-mode UEFI loader (`M2HY` ACPI table, `ExitBootServices()` 3-reg hook, `PatchVfctRemoveEdp()`, `PatchPeg0Gfx0Dod()`, NVRAM `gpu-power-prefs` switcher; GNU-EFI `-Wall -Wextra -Werror`, empty `nm -u`, `0` relocations)
- [`uefi/m2_hybrid/Makefile`](../uefi/m2_hybrid/Makefile) — GNU-EFI build configuration
- [`uefi/m2_hybrid/SSDT_HYBRID_MBP133.asl`](../uefi/m2_hybrid/SSDT_HYBRID_MBP133.asl) — Hybrid S3 + GMUX brightness bridge (`MBP133/M2HYBCM`)
- [`scripts/Read-M2-Hybrid.py`](../scripts/Read-M2-Hybrid.py) — Live ACPI `M2HY` + `VFCT` reader, DXGI/VRAM/brightness/S3 collector (`--self-test` verified)
- [`scripts/usb_refind/refind.conf`](../scripts/usb_refind/refind.conf) — USB `REFIND_USB` stanza (`MBP13,3 M2 Hybrid`)
- [`docs/MILESTONE_2_RUNBOOK.md`](MILESTONE_2_RUNBOOK.md) — Test & rollback runbook

> [!NOTE]
> Internal NVMe ESP (`bootmgfw.efi`) and prior verified USB fallbacks (`mbp133_m1b_igpu.efi`, `mbp133_m1a2_ebs.efi`, `SSDT_IGPU_MBP133.aml`) remain untouched. Reset `gpu-power-prefs` anytime via `[S]`/`[D]` in `mbp133_m2_hybrid.efi`, macOS boot, or `Cmd+Option+P+R`.
