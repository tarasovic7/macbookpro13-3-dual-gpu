# Milestone 1A-2 — Verified Findings: Dual-GPU `Code 43` Solved via Post-EBS 3-Register Relocation (`MacBookPro13,3`)

> [!IMPORTANT]
> **Status: COMPLETED & HARDWARE-VERIFIED END-TO-END.**
> Booting Windows 11 Boot Camp (with HVCI enabled) through [`uefi/m1a2_ebs/mbp133_m1a2_ebs.c`](../uefi/m1a2_ebs/mbp133_m1a2_ebs.c) **Mode `[5]`** (`SetOS 10.15 + 3-Reg FB Fix`) brings up **both Intel HD Graphics 530 (`00:02.0`) and AMD Radeon Pro 455 / RX 560X (`01:00.0`) concurrently with zero errors (no `Code 43`)**, working `2880×1800 @ 60 Hz` panel output, working brightness control (`101` levels), and full **`Standby (S3)` lid-close sleep/wake**.

---

## 1. Executive Summary & Live Verified Hardware State

Evidence captured in [`diag/m1a2/m1a2_mode5_3reg_20261125-180434.txt`](../diag/m1a2/m1a2_mode5_3reg_20261125-180434.txt) and live Windows 11 telemetry after booting **Mode `[5]`**:

| Component / Check | Verified State | Evidence / Key Metrics |
|---|---|---|
| **Intel(R) HD Graphics 530** (`00:02.0`) | **`OK`** (`CM_PROB_NONE`, Error `0`) | `PCI\VEN_8086&DEV_191B&SUBSYS_015F106B&REV_06`, driver `31.0.101.2115`, `AdapterRAM = 1 GB` (`1073741824`), active WDDM/DXGI LUID |
| **AMD Radeon RX 560X / Pro 455** (`01:00.0`) | **`OK`** (`CM_PROB_NONE`, Error `0`) | `PCI\VEN_1002&DEV_67EF&SUBSYS_0166106B&REV_C7`, driver `31.0.23013.14001`, `2880×1800 @ 60 Hz`, `AdapterRAM = 2 GB` (`2147483648`), `1,150,414,848` B dedicated VRAM in use |
| **EBS Hook & 3-Register Fix** | **`FBLOC_WRITTEN`** (`Flags=0x0000004f`) | `[BEFORE_EBS\|AFTER_EBS\|POST_ACTION\|SETOS_CALLED\|FBLOC_WRITTEN]`, `ChecksumOK=True (sum8=0)`, `EBS Calls=1`, `Retries=0` |
| **Internal Panel & Brightness** | **`OK`** | `Integrated Monitor (Color LCD)` (`DISPLAY\APPA030\5&DCF9AE9&6&UID256`), `WmiMonitorBrightness`: `CurrentBrightness=70`, `Levels=101`, hardware slider works |
| **Lid-Close Sleep / Wake** | **`Standby (S3)` Verified** | `powercfg /a`: `Standby (S3)` available; Event Log: `Kernel-Power` ID `42` (`Sleep Reason: Button or Lid`) `18:07:18` → ID `107` resume `18:07:24` → `Power-Troubleshooter` ID `1` `18:07:34` (`Device -Apple SPI Device`) |
| **Windows System Event Log** | **0 GPU Errors / 0 TDRs** | Zero errors or warnings from `Display`, `DXGKrnl`, `amdkmdag`, or `igdkmd64` |

---

## 2. Exact Root Cause of `MacBookPro13,3` Dual-GPU `Code 43`

By hooking `gBS->ExitBootServices()` in [`uefi/m1a2_ebs/mbp133_m1a2_ebs.c`](../uefi/m1a2_ebs/mbp133_m1a2_ebs.c) and transporting pre-/post-EBS snapshots across the UEFI→Windows boundary in an `EfiACPIReclaimMemory` ACPI table (`M1A2` signature, read by [`scripts/Read-M1A2-Ebs.py`](../scripts/Read-M1A2-Ebs.py)), we observed Apple EFI's hidden `ExitBootServices()` callback directly:

### What Apple EFI does during `ExitBootServices()` on Normal Boot (No `AppleSetOs`)
1. **Hides Intel HD 530 (`00:02.0`) and locks the host bridge:**
   - `HostBridge 00:00.0 +0x54 (DEVEN)`: `0x0000003f -> 0x0000000f` (`D2EN=0`)
   - `HostBridge 00:00.0 +0x80 (HB80)`: `0x00000000 -> 0x00000001` (prevents any post-EBS software from re-enabling `DEVEN.D2EN`, proven in Mode `[4]`)
   - `HostBridge 00:00.0 +0x58 (PAVPC)`: `0x00000000 -> 0x7ff00007` (locked `0x7ff00017` under SetOS)
2. **Relocates Polaris 11 (`01:00.0`) VRAM/framebuffer via exactly 3 `BAR5` (`0x82600000`) MMIO registers:**
   - **`mmMC_VM_FB_LOCATION` (`0x0809`, `BAR5+0x2024`)**: `0x007f0000 -> 0xf47ff400`
   - **`mmHDP_NONSURFACE_BASE` (`0x0b01`, `BAR5+0x2c04`)**: `0x00000000 -> 0xf4000000`
   - **`mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH` (`0x1a07`, `BAR5+0x681c`)**: `0x00000000 -> 0x000000f4` (with `mmGRPH_PRIMARY_SURFACE_ADDRESS` `0x1a04` holding `0x00300000`)
   - *(All other 57 `AmdExtra` `MC_VM`, `HDP`, `DCE0`, `VGA`, `SRBM`, and `VM_CTX` registers inspected in v2 Mode `[1]` were 100% invariant across `ExitBootServices()`.)*

### Why `AppleSetOs("Mac OS X 10.15")` Previously Caused `Code 43` (`BugCheck 0x1B0`)
When `AppleSetOs("Mac OS X 10.15")` is called before `ExitBootServices()`, Apple EFI **skips the entire Boot Camp `ExitBootServices()` callback**:
- **Good:** `HostBridge 00:00.0 +0x54 (DEVEN)` remains `0x0000003f` (`D2EN=1`), so Intel HD 530 stays visible to Windows.
- **Fatal side effect:** Polaris 11 stays at `mmMC_VM_FB_LOCATION = 0x007f0000` (with `0x0b01 = 0x0` and `0x1a07 = 0x0`), leaving GPU internal MC addresses anchored at `0x0` instead of `0xF4_0000_0000`. When Windows boots `amdkmdag.sys`, SMU7 / power-play initialization (`polaris10_start_smu` → `PP_Initialize`) fails and trips **Code 43 (`BugCheck 0x1B0 / P1=0x2 / P2=0xC0000001`)**.

---

## 3. Hardware Run Chronology (`diag/m1a2/`)

| Run | Mode / Version | Key Observed Diff / Behavior | Windows Outcome | Log |
|---|---|---|---|---|
| **1** | v1 Mode `[1]` `Normal X-Ray` | `DEVEN 0x3f->0x0f`, `HB80 0->1`, `MC_VM_FB_LOC 0x007f0000->0xf47ff400` | Intel hidden (`PHANTOM`), AMD **`OK`** | [`m1a2_mode1_normal_xray_20261125-173851.txt`](../diag/m1a2/m1a2_mode1_normal_xray_20261125-173851.txt) |
| **2** | v1 Mode `[2]` `SetOS 10.15 X-Ray` | `DEVEN=0x3f` kept (`HB80=1` still locked), `MC_VM_FB_LOC=0x007f0000` unchanged | Intel **`OK`**, AMD **`Code 43`** | [`m1a2_mode2_setos15_xray_20261125-174114.txt`](../diag/m1a2/m1a2_mode2_setos15_xray_20261125-174114.txt) |
| **3** | v1 Mode `[4]` `Normal + Unhide D2EN=1` | `HB80=0x00000001` set during EBS prevents post-EBS `DEVEN` unhide (`DEVEN` stayed `0x0f`) | Intel **`PHANTOM`**, AMD **`OK`** | [`m1a2_mode4_normal_unhide_igpu_20261125-174328.txt`](../diag/m1a2/m1a2_mode4_normal_unhide_igpu_20261125-174328.txt) |
| **4** | v1 Mode `[5]` `SetOS 10.15 + 0x0809 only` | Relocated `MC_VM_FB_LOCATION=0xF47FF400` while `0x0B01` and `0x1A07` stayed `0x0` | Green stripes/snow + `BAD_SYSTEM_CONFIG_INFO` boot crash | *(No Windows log)* |
| **5** | v2 Mode `[1]` `Normal X-Ray` (+60 `AmdExtra` BAR5 regs) | Isolated exact 3-reg tuple (`0x0809`, `0x0B01`, `0x1A07`); remaining 57 regs unchanged | Intel hidden, AMD **`OK`** | [`m1a2_mode1_normal_xray_20261125-175555.txt`](../diag/m1a2/m1a2_mode1_normal_xray_20261125-175555.txt) |
| **6** | **v2 Mode `[5]` `SetOS 10.15 + 3-Reg FB Fix`** | Post-EBS writes `0x0809`, `0x0B01`, `0x1A07` + `0x1A04` latch + 500 µs delay + 3-reg readback | **Intel `OK` + AMD `OK`, brightness & S3 sleep `OK`** | [`m1a2_mode5_3reg_20261125-180434.txt`](../diag/m1a2/m1a2_mode5_3reg_20261125-180434.txt) |

---

## 4. Solution Architecture & Implementation

In [`HookedExitBootServices()`](../uefi/m1a2_ebs/mbp133_m1a2_ebs.c) for `M1A2_MODE_SETOS_FIX_FBLOC` (selected after `AppleSetOs("Mac OS X 10.15")` succeeds and only once the firmware's final `ExitBootServices()` returns `EFI_SUCCESS`):

1. Verify Polaris 11 (`01:00.0`) Vendor ID `0x1002`, memory decode enabled on both `01:00.0` and PEG0 `00:01.0`, and valid 32-bit `BAR5` (`0x82600000`).
2. Read the current low primary surface offset `surf_lo = mmio_r32(b5 + 0x1A04 * 4)` (`0x00300000`).
3. Apply the 3-register VRAM relocation sequence matching Apple EFI:
   - `mmio_w32(b5 + 0x0809 * 4, 0xF47FF400u)` (`mmMC_VM_FB_LOCATION`)
   - `mmio_w32(b5 + 0x0B01 * 4, 0xF4000000u)` (`mmHDP_NONSURFACE_BASE`)
   - `mmio_w32(b5 + 0x1A07 * 4, 0x000000F4u)` (`mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH`)
   - `mmio_w32(b5 + 0x1A04 * 4, surf_lo)` (latch `DCE0` primary surface update)
4. Wait `500 µs` (`io_delay_us(500)`), read back all 3 registers, confirm exact values (`0xF47FF400`, `0xF4000000`, `0x000000F4`), and set `M1A2_FLAG_FBLOC_WRITTEN` in the `M1A2` ACPI table before handing control to `bootmgfw.efi`.

### Delivered Files
- [`uefi/m1a2_ebs/mbp133_m1a2_ebs.c`](../uefi/m1a2_ebs/mbp133_m1a2_ebs.c) — Interactive EBS X-Ray & Dual-GPU Fix loader (GNU-EFI, `-Wall -Wextra -Werror`, 0 relocations)
- [`uefi/m1a2_ebs/Makefile`](../uefi/m1a2_ebs/Makefile) — GNU-EFI build configuration
- [`scripts/Read-M1A2-Ebs.py`](../scripts/Read-M1A2-Ebs.py) — Windows ACPI `M1A2` (`v1`/`v2`) parser, diff engine, and PnP collector (with `--self-test`)
- [`docs/MILESTONE_1A2_RUNBOOK.md`](MILESTONE_1A2_RUNBOOK.md) — Test runbook for USB `REFIND_USB` (`D:`)
- [`scripts/usb_refind/refind.conf`](../scripts/usb_refind/refind.conf) — USB rEFInd stanza `MBP13,3 M1A-2 EBS X-Ray & Dual-GPU`

---

## 5. Next Steps (Milestone 2 — Hybrid Dual-GPU)

With **both** building blocks now proven independently on `MacBookPro13,3`:
- **Milestone 1B:** Intel HD 530 driving the internal eDP panel via GMUX (`0x7C2`/`0x7D0`/`0x7D4`), `BDB[43]=0` + `SSDT_IGPU_MBP133` smooth brightness, S3 sleep/wake, and **`9.34 W`** idle draw with AMD powered off.
- **Milestone 1A-2:** Concurrent Intel HD 530 + AMD Radeon Pro 455 with zero `Code 43` via `AppleSetOs("Mac OS X 10.15")` + post-EBS 3-register relocation (`0x0809`, `0x0B01`, `0x1A07`).

**Milestone 2** will combine both paths (panel driven by Intel HD 530 + AMD kept alive with the 3-register fix and dynamically powered via D3Cold/Hybrid Graphics offload, plus clean one-click rEFInd boot profiles).
