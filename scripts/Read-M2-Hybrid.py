#!/usr/bin/env python3
"""
Read-M2-Hybrid.py - Milestone 2 Windows ACPI 'M2HY' Reader & Live Hybrid Telemetry Collector

Reads the live 368-byte ACPI 'M2HY' table (without Admin privileges) via
kernel32!GetSystemFirmwareTable('ACPI', 'M2HY'), decodes BeforeEbs / AfterEbs /
PostAction hardware snapshots, queries live Intel/AMD PnP, VideoController,
WMI brightness, DXGI adapter order, per-process GPU memory, battery draw, and
sleep states, copies USB D:\\M2_LOGS\\m2_last.txt (if present), and saves raw
.bin + decoded .txt reports to diag/m2/.

Usage:
  python scripts/Read-M2-Hybrid.py [--tag hybrid_test]
  python scripts/Read-M2-Hybrid.py --self-test
"""

import argparse
import ctypes
import datetime
import os
import shutil
import struct
import subprocess
import sys
from typing import Dict, List, Optional

ACPI_PROVIDER = 0x41435049  # 'ACPI'
M2HY_SIG_ID = struct.unpack("<I", b"M2HY")[0]  # 0x5948324D

HDR_FMT = "<4sIBB6s8sIII"
META_FMT = "<6IQ"
SNAP_FMT = "<25I"

HDR_SIZE = struct.calcsize(HDR_FMT)
META_SIZE = struct.calcsize(META_FMT)
SNAP_SIZE = struct.calcsize(SNAP_FMT)
TOTAL_SIZE = HDR_SIZE + META_SIZE + 3 * SNAP_SIZE

assert HDR_SIZE == 36, f"Expected HDR_SIZE=36, got {HDR_SIZE}"
assert META_SIZE == 32, f"Expected META_SIZE=32, got {META_SIZE}"
assert SNAP_SIZE == 100, f"Expected SNAP_SIZE=100, got {SNAP_SIZE}"
assert TOTAL_SIZE == 368, f"Expected TOTAL_SIZE=368, got {TOTAL_SIZE}"

MODE_NAMES = {
    1: "mode1_hybrid",
    2: "mode2_intel_only",
    3: "mode3_amd_dual",
    4: "mode4_normal",
}

SNAP_FIELDS = [
    "HbDeven", "Hb80", "PegCmd",
    "IgpuVidDid", "IgpuCmd", "IgpuBar0", "IgpuAsls", "IgpuDdiA", "IgpuPwr2",
    "AmdVidDid", "AmdCmd", "AmdBar5", "AmdFbLoc", "AmdHdpBase", "AmdSurfLo",
    "AmdSurfHi", "AmdGrbm", "AmdSmuPc0", "AmdSmuFw",
    "GmuxSig", "GmuxDisp", "GmuxDdc", "GmuxExt", "GmuxPwr", "GmuxBrt",
]


def parse_snap(buf: bytes) -> Dict[str, int]:
    vals = struct.unpack(SNAP_FMT, buf)
    return dict(zip(SNAP_FIELDS, vals))


def parse_m2hy_table(data: bytes) -> dict:
    if len(data) != TOTAL_SIZE:
        raise ValueError(f"Expected {TOTAL_SIZE} bytes, got {len(data)}")
    sig, length, rev, csum, oem_id, oem_tid, oem_rev, creator_id, creator_rev = struct.unpack_from(HDR_FMT, data, 0)
    if sig != b"M2HY":
        raise ValueError(f"Bad ACPI signature: {sig!r}")
    version, mode, flags, calls, retries, reserved0, ecam_base = struct.unpack_from(META_FMT, data, HDR_SIZE)
    off0 = HDR_SIZE + META_SIZE
    return {
        "length": length,
        "checksum_ok": (sum(data) & 0xFF) == 0,
        "sum8": sum(data) & 0xFF,
        "version": version,
        "mode": mode,
        "mode_name": MODE_NAMES.get(mode, f"mode{mode}"),
        "flags": flags,
        "ebs_calls": calls,
        "ebs_retries": retries,
        "reserved0": reserved0,
        "ecam_base": ecam_base,
        "BeforeEbs": parse_snap(data[off0:off0 + SNAP_SIZE]),
        "AfterEbs": parse_snap(data[off0 + SNAP_SIZE:off0 + 2 * SNAP_SIZE]),
        "PostAction": parse_snap(data[off0 + 2 * SNAP_SIZE:off0 + 3 * SNAP_SIZE]),
    }


def read_live_m2hy() -> bytes:
    k32 = ctypes.windll.kernel32
    sz = k32.GetSystemFirmwareTable(ACPI_PROVIDER, M2HY_SIG_ID, None, 0)
    if sz == 0:
        err = ctypes.GetLastError()
        raise RuntimeError(
            f"ACPI table 'M2HY' not found in Windows (GetLastError={err}). Boot via mbp133_m2_hybrid.efi first."
        )
    buf = ctypes.create_string_buffer(sz)
    got = k32.GetSystemFirmwareTable(ACPI_PROVIDER, M2HY_SIG_ID, buf, sz)
    if got != sz:
        raise RuntimeError(f"GetSystemFirmwareTable short read ({got}/{sz})")
    return buf.raw


def decode_flags(flags: int) -> str:
    bits = [
        (0, "BEFORE_EBS"),
        (1, "AFTER_EBS"),
        (2, "POST_ACTION"),
        (3, "SETOS_OK"),
        (4, "GMUX_IGD_OK"),
        (5, "GMUX_RAIL_OFF"),
        (6, "DDI_4LANES_OK"),
        (7, "VBT_BDB43_OK"),
        (8, "SSDT_INJECTED"),
        (9, "FBLOC_3REG_OK"),
        (10, "DDI_POST_EBS_FIX"),
        (11, "VFCT_PATCHED"),
        (12, "GFX0_DOD_PATCHED"),
    ]
    on = [name for bit, name in bits if (flags >> bit) & 1]
    return "|".join(on) if on else "NONE"


def inspect_vfct_bytes(raw: bytes) -> List[str]:
    try:
        if len(raw) < 0x80 or raw[:4] != b"VFCT":
            return [f"  (invalid VFCT header, len={len(raw)})"]
        vlen = struct.unpack_from("<I", raw, 4)[0]
        img_off = struct.unpack_from("<I", raw, 0x34)[0]
        vb_off = img_off + 0x1C
        if vlen != len(raw) or vb_off + 0x200 > len(raw) or raw[vb_off:vb_off + 2] != b"\x55\xaa":
            return [f"  (unexpected VFCT VBIOS layout: len={len(raw)} hdr_len={vlen} vb_off={vb_off:#x})"]
        vb = raw[vb_off:]
        rom_len = vb[2] * 512
        rom_hdr = struct.unpack_from("<H", vb, 0x48)[0]
        mdt = struct.unpack_from("<H", vb, rom_hdr + 32)[0]
        obj_off = struct.unpack_from("<H", vb, mdt + 4 + 2 * 22)[0]
        obj = vb[obj_off:obj_off + struct.unpack_from("<H", vb, obj_off)[0]]
        dev_sup, conn_off, _, enc_off, _, dp_off = struct.unpack_from("<6H", obj, 4)

        ndp = obj[dp_off]
        cur = dp_off + 4
        dp_items = []
        for _ in range(ndp):
            dev_tag, esz, conn_id, _ = struct.unpack_from("<4H", obj, cur)
            dp_items.append(f"{dev_tag:#06x}:{conn_id:#06x}")
            cur += esz

        nco = obj[conn_off]
        co_ids = [f"{struct.unpack_from('<H', obj, conn_off + 4 + 8 * i)[0]:#06x}" for i in range(nco)]
        nen = obj[enc_off]
        return [
            f"  len={len(raw)} dev_sup={dev_sup:#06x} vb_sum8={sum(vb[:rom_len]) & 0xFF} vfct_sum8={sum(raw) & 0xFF}",
            f"  dp({ndp})=[{', '.join(dp_items)}] conn({nco})=[{', '.join(co_ids)}] enc({nen})",
        ]
    except Exception as exc:
        return [f"  (VFCT parse error: {exc})"]


def inspect_live_vfct() -> List[str]:
    try:
        k32 = ctypes.windll.kernel32
        vfct_sig = struct.unpack("<I", b"VFCT")[0]
        sz = k32.GetSystemFirmwareTable(ACPI_PROVIDER, vfct_sig, None, 0)
        if sz == 0:
            return ["  (ACPI VFCT not present)"]
        buf = ctypes.create_string_buffer(sz)
        got = k32.GetSystemFirmwareTable(ACPI_PROVIDER, vfct_sig, buf, sz)
        if got != sz:
            return [f"  (VFCT short read {got}/{sz})"]
        return inspect_vfct_bytes(buf.raw)
    except Exception as exc:
        return [f"  (VFCT query error: {exc})"]


def snap_summary_lines(label: str, s: Dict[str, int]) -> List[str]:
    d2en = (s["HbDeven"] >> 4) & 1
    igpu_vid = s["IgpuVidDid"] & 0xFFFF
    igpu_did = (s["IgpuVidDid"] >> 16) & 0xFFFF
    amd_vid = s["AmdVidDid"] & 0xFFFF
    amd_did = (s["AmdVidDid"] >> 16) & 0xFFFF
    return [
        f"--- SNAPSHOT: {label} ---",
        f"  HostBridge : DEVEN={s['HbDeven']:#010x} (D2EN={d2en}) HB80={s['Hb80']:#010x} PEG_CMD={s['PegCmd']:#06x}",
        f"  IGPU 00:02 : VID:DID={igpu_vid:#06x}:{igpu_did:#06x} CMD={s['IgpuCmd']&0xFFFF:#06x} BAR0={s['IgpuBar0']:#010x} ASLS={s['IgpuAsls']:#010x} DDI_A={s['IgpuDdiA']:#010x} PWR2={s['IgpuPwr2']:#010x}",
        f"  AMD  01:00 : VID:DID={amd_vid:#06x}:{amd_did:#06x} CMD={s['AmdCmd']&0xFFFF:#06x} BAR5={s['AmdBar5']:#010x} FB_LOC={s['AmdFbLoc']:#010x} HDP={s['AmdHdpBase']:#010x} SURF={s['AmdSurfHi']:#x}:{s['AmdSurfLo']:#010x}",
        f"  AMD  SMU7  : GRBM={s['AmdGrbm']:#010x} PC0={s['AmdSmuPc0']:#010x} FW={s['AmdSmuFw']:#010x}",
        f"  GMUX       : sig={s['GmuxSig']:#06x} disp={s['GmuxDisp']:#x} ddc={s['GmuxDdc']:#x} ext={s['GmuxExt']:#x} pwr={s['GmuxPwr']:#x} brt={s['GmuxBrt']:#x}",
    ]


def diff_snaps(label_a: str, a: Dict[str, int], label_b: str, b: Dict[str, int]) -> List[str]:
    diffs: List[str] = []
    for k in SNAP_FIELDS:
        if a[k] != b[k]:
            diffs.append(f"  {k}: {a[k]:#010x} -> {b[k]:#010x}")
    header = f"=== DIFF: {label_a}  vs  {label_b} ({len(diffs)} change(s)) ==="
    return [header] + (diffs if diffs else ["  (100% IDENTICAL — ZERO HARDWARE DIFFERENCES)"])


def query_windows_telemetry() -> List[str]:
    ps = r"""
Write-Output '--- PnP Display & Monitor ---'
Get-PnpDevice -ErrorAction SilentlyContinue |
  Where-Object { $_.Class -in @('Display','Monitor') -and ($_.Status -eq 'OK' -or $_.Problem -ne 'CM_PROB_PHANTOM') } |
  ForEach-Object {
    $pc = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data
    "  $($_.Class) | $($_.FriendlyName) | $($_.Status) | Problem=$($_.Problem)($pc) | $($_.InstanceId)"
  }

Write-Output ''
Write-Output '--- Win32_VideoController ---'
Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue | ForEach-Object {
  "  $($_.Name) | Status=$($_.Status) Err=$($_.ConfigManagerErrorCode) | Driver=$($_.DriverVersion) | Mode=$($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)@$($_.CurrentRefreshRate)Hz | VRAM=$([math]::Round($_.AdapterRAM/1MB))MB"
}

Write-Output ''
Write-Output '--- WMI Brightness ---'
$brt = Get-CimInstance -Namespace root/wmi -ClassName WmiMonitorBrightness -ErrorAction SilentlyContinue
if ($brt) {
  $brt | ForEach-Object { "  $($_.InstanceName) | Current=$($_.CurrentBrightness) Levels=$($_.Level.Count)" }
} else {
  Write-Output '  (no WmiMonitorBrightness instance)'
}

Write-Output ''
Write-Output '--- DXGI Adapter Order (IDXGIFactory6) ---'
try {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public class DxgiM2 {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct DXGI_ADAPTER_DESC1 {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string Description;
        public uint VendorId; public uint DeviceId; public uint SubSysId; public uint Revision;
        public UIntPtr DedicatedVideoMemory; public UIntPtr DedicatedSystemMemory; public UIntPtr SharedSystemMemory;
        public uint LuidLow; public int LuidHigh; public uint Flags;
    }
    [ComImport, Guid("c1b6694f-ff09-44a9-b03c-77900a0a1d17"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDXGIFactory6 {
        void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent();
        [PreserveSig] int EnumAdapters(uint i, out IntPtr a);
        void MakeWindowAssociation(); void GetWindowAssociation(); void CreateSwapChain(); void CreateSoftwareAdapter();
        [PreserveSig] int EnumAdapters1(uint i, out IntPtr a);
        void IsCurrent(); void IsWindowedStereoEnabled(); void CreateSwapChainForHwnd(); void CreateSwapChainForCoreWindow();
        void GetSharedResourceAdapterLuid(); void RegisterStereoStatusWindow(); void RegisterStereoStatusEvent(); void UnregisterStereoStatus();
        void RegisterOcclusionStatusWindow(); void RegisterOcclusionStatusEvent(); void UnregisterOcclusionStatus();
        void CreateSwapChainForComposition(); void GetCreationFlags(); void EnumAdapterByLuid(); void EnumWarpAdapter(); void CheckFeatureSupport();
        [PreserveSig] int EnumAdapterByGpuPreference(uint i, uint pref, ref Guid riid, out IntPtr a);
    }
    [ComImport, Guid("29038f61-3839-4626-91fd-086879011a05"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDXGIAdapter1 {
        void SetPrivateData(); void SetPrivateDataInterface(); void GetPrivateData(); void GetParent();
        void EnumOutputs(); void GetDesc(); void CheckInterfaceSupport();
        [PreserveSig] int GetDesc1(out DXGI_ADAPTER_DESC1 desc);
    }
    [DllImport("dxgi.dll")] static extern int CreateDXGIFactory2(uint flags, ref Guid riid, out IDXGIFactory6 factory);
    public static void Run() {
        Guid f6 = new Guid("c1b6694f-ff09-44a9-b03c-77900a0a1d17");
        Guid a1 = new Guid("29038f61-3839-4626-91fd-086879011a05");
        IDXGIFactory6 fac;
        int hr = CreateDXGIFactory2(0, ref f6, out fac);
        if (hr != 0) { Console.WriteLine("  CreateDXGIFactory2 HR=" + hr); return; }
        string[] names = { "UNSPECIFIED", "MINIMUM_POWER", "HIGH_PERFORMANCE" };
        for (uint p = 0; p < 3; p++) {
            Console.WriteLine("  [" + names[p] + "]");
            for (uint i = 0; i < 6; i++) {
                IntPtr ptr;
                if (fac.EnumAdapterByGpuPreference(i, p, ref a1, out ptr) != 0) break;
                var ad = (IDXGIAdapter1)Marshal.GetObjectForIUnknown(ptr);
                DXGI_ADAPTER_DESC1 d; ad.GetDesc1(out d);
                Console.WriteLine(string.Format("    #{0} {1} (VEN={2:X4} DEV={3:X4} LUID=0x{4:x8}_{5:x8} VRAM={6}MB Shared={7}MB)",
                    i, d.Description, d.VendorId, d.DeviceId, d.LuidHigh, d.LuidLow,
                    (long)d.DedicatedVideoMemory / (1024*1024), (long)d.SharedSystemMemory / (1024*1024)));
                Marshal.Release(ptr);
            }
        }
    }
}
"@
[DxgiM2]::Run()
} catch {
  Write-Output "  DXGI query error: $($_.Exception.Message)"
}

Write-Output ''
Write-Output '--- GPU Adapter Memory & Top Consumers ---'
try {
  (Get-Counter '\GPU Adapter Memory(*)\Dedicated Usage','\GPU Adapter Memory(*)\Shared Usage' -ErrorAction SilentlyContinue).CounterSamples | ForEach-Object {
    $m = ($_.Path -split '\\')[-1]
    "  $($_.InstanceName) | $m = $([math]::Round($_.CookedValue / 1MB, 1)) MB"
  }
  $procs = @{}; Get-Process | ForEach-Object { $procs[$_.Id] = $_.ProcessName }
  $ded = (Get-Counter '\GPU Process Memory(*)\Dedicated Usage' -ErrorAction SilentlyContinue).CounterSamples
  $shrMap = @{}
  (Get-Counter '\GPU Process Memory(*)\Shared Usage' -ErrorAction SilentlyContinue).CounterSamples | ForEach-Object { $shrMap[$_.InstanceName] = $_.CookedValue }
  $rows = foreach ($d in $ded) {
    $dmb = [math]::Round($d.CookedValue / 1MB, 1)
    $smb = [math]::Round(($shrMap[$d.InstanceName]) / 1MB, 1)
    if ($dmb -gt 1 -or $smb -gt 10) {
      if ($d.InstanceName -match 'pid_(\d+)_luid_(0x[0-9a-f]+_0x[0-9a-f]+)') {
        $p = [int]$Matches[1]
        [PSCustomObject]@{ Luid=$Matches[2]; Proc=if($procs[$p]){$procs[$p]}else{"PID_$p"}; DedMB=$dmb; ShrMB=$smb }
      }
    }
  }
  $rows | Sort-Object DedMB, ShrMB -Descending | Select-Object -First 15 | ForEach-Object {
    "  proc=$($_.Proc) luid=$($_.Luid) Dedicated=$($_.DedMB)MB Shared=$($_.ShrMB)MB"
  }
} catch {
  Write-Output "  GPU counter error: $($_.Exception.Message)"
}

Write-Output ''
Write-Output '--- Battery & Sleep States ---'
$bat = Get-CimInstance -Namespace root/wmi -ClassName BatteryStatus -ErrorAction SilentlyContinue
if ($bat) {
  $bat | ForEach-Object {
    "  PowerOnline=$($_.PowerOnline) Discharging=$($_.Discharging) DischargeRate=$($_.DischargeRate)mW Voltage=$($_.Voltage)mV Cap=$($_.RemainingCapacity)mWh"
  }
}
(powercfg /a) | Where-Object { $_ -match '\S' } | Select-Object -First 10 | ForEach-Object { "  $_" }
"""
    try:
        res = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=30)
        lines = [ln.rstrip() for ln in res.stdout.splitlines()]
        if res.stderr.strip():
            lines.append("--- PowerShell stderr ---")
            lines.extend("  " + ln for ln in res.stderr.splitlines())
        return lines
    except Exception as exc:
        return [f"  (telemetry query failed: {exc})"]


def format_report(parsed: dict, win_lines: Optional[List[str]] = None, vfct_lines: Optional[List[str]] = None) -> str:
    out: List[str] = [
        "=== MILESTONE 2 HYBRID DUAL-GPU REPORT ===",
        f"Mode={parsed['mode']} ({parsed['mode_name']}) | Version={parsed['version']} | ChecksumOK={parsed['checksum_ok']} (sum8={parsed['sum8']})",
        f"Flags={parsed['flags']:#010x} [{decode_flags(parsed['flags'])}] | EBS Calls={parsed['ebs_calls']} Retries={parsed['ebs_retries']} | ECAM={parsed['ecam_base']:#x}",
        "",
    ]
    if vfct_lines is not None:
        out.append("--- Live ACPI VFCT Object_Header ---")
        out.extend(vfct_lines)
        out.append("")
    if win_lines is not None:
        out.extend(win_lines)
        out.append("")
    out.extend(snap_summary_lines("BEFORE_EBS", parsed["BeforeEbs"]))
    out.append("")
    out.extend(snap_summary_lines("AFTER_EBS", parsed["AfterEbs"]))
    out.append("")
    out.extend(diff_snaps("BEFORE_EBS", parsed["BeforeEbs"], "AFTER_EBS", parsed["AfterEbs"]))
    out.append("")
    out.extend(snap_summary_lines("POST_ACTION", parsed["PostAction"]))
    out.append("")
    out.extend(diff_snaps("AFTER_EBS", parsed["AfterEbs"], "POST_ACTION", parsed["PostAction"]))
    return "\n".join(out) + "\n"


def self_test() -> None:
    snap0 = [0] * 25
    snap0[0] = 0x3F
    snap0[3] = 0x191B8086
    snap0[9] = 0x67EF1002
    snap0[12] = 0x007F0000
    snap0[19] = 0x55AA
    snap0[20] = 0x02

    snap1 = list(snap0)
    snap2 = list(snap0)
    snap2[12] = 0xF47FF400
    snap2[13] = 0xF4000000
    snap2[15] = 0x000000F4

    hdr = struct.pack(HDR_FMT, b"M2HY", TOTAL_SIZE, 1, 0, b"MBP133", b"M2HYBRID", 1, 0x5948324D, 1)
    meta = struct.pack(META_FMT, 1, 1, 0x1BFF, 1, 0, 0, 0xE0000000)
    raw = bytearray(hdr + meta + struct.pack(SNAP_FMT, *snap0) + struct.pack(SNAP_FMT, *snap1) + struct.pack(SNAP_FMT, *snap2))
    raw[9] = (-sum(raw)) & 0xFF
    parsed = parse_m2hy_table(bytes(raw))
    assert parsed["checksum_ok"] is True
    assert parsed["mode"] == 1
    assert parsed["BeforeEbs"]["AmdFbLoc"] == 0x007F0000
    assert parsed["PostAction"]["AmdFbLoc"] == 0xF47FF400
    assert parsed["PostAction"]["AmdHdpBase"] == 0xF4000000
    assert parsed["PostAction"]["AmdSurfHi"] == 0x000000F4
    txt = format_report(parsed, None)
    assert "FBLOC_3REG_OK" in txt and "GMUX_IGD_OK" in txt
    assert "VFCT_PATCHED" in txt and "GFX0_DOD_PATCHED" in txt
    vfct_ref = os.path.join("diag", "acpi_setos", "vfct.dat")
    if os.path.isfile(vfct_ref):
        with open(vfct_ref, "rb") as f:
            vlines = inspect_vfct_bytes(f.read())
        assert "dev_sup=0x068a" in vlines[0] and "dp(5)=" in vlines[1] and "conn(5)=" in vlines[1], vlines
    print(f"SELF-TEST OK (M2HY_SIZE={TOTAL_SIZE}, SNAP_SIZE={SNAP_SIZE})")


def main() -> int:
    ap = argparse.ArgumentParser(description="Read Milestone 2 ACPI M2HY table & Windows Dual-GPU telemetry")
    ap.add_argument("--tag", default="", help="Optional tag appended to saved filename")
    ap.add_argument("--out-dir", default=os.path.join("diag", "m2"))
    ap.add_argument("--usb-log", default=r"D:\M2_LOGS\m2_last.txt")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return 0

    raw = read_live_m2hy()
    parsed = parse_m2hy_table(raw)
    vfct_lines = inspect_live_vfct()
    win_lines = query_windows_telemetry()
    report = format_report(parsed, win_lines, vfct_lines)

    os.makedirs(args.out_dir, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    tag_part = f"_{args.tag}" if args.tag else ""
    stem = f"m2_{parsed['mode_name']}{tag_part}_{ts}"
    bin_path = os.path.join(args.out_dir, f"{stem}.bin")
    txt_path = os.path.join(args.out_dir, f"{stem}.txt")
    with open(bin_path, "wb") as f:
        f.write(raw)
    with open(txt_path, "w", encoding="utf-8") as f:
        f.write(report)

    if os.path.isfile(args.usb_log):
        uefi_copy = os.path.join(args.out_dir, f"{stem}_uefi.txt")
        shutil.copy2(args.usb_log, uefi_copy)
        print(f"Copied UEFI log -> {uefi_copy}")

    print(report)
    print(f"Saved: {bin_path}")
    print(f"Saved: {txt_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
