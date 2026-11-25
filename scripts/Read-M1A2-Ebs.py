#!/usr/bin/env python3
"""
Read-M1A2-Ebs.py - Milestone 1A-2 Windows ACPI 'M1A2' Reader & EBS Diff Tool (v1 & v2)
Reads the live 4096-byte ACPI 'M1A2' table (without Admin privileges) via
kernel32!GetSystemFirmwareTable('ACPI', 'M1A2'), decodes BeforeEbs / AfterEbs /
PostAction hardware snapshots, queries live Intel/AMD PnP status, and saves
raw .bin + decoded .txt reports to diag/m1a2/.

Usage:
  python scripts/Read-M1A2-Ebs.py [--tag mode5_fix]
  python scripts/Read-M1A2-Ebs.py --diff diag/m1a2/m1a2_mode1_*.bin diag/m1a2/m1a2_mode2_*.bin
  python scripts/Read-M1A2-Ebs.py --self-test
"""

import argparse
import ctypes
import datetime
import os
import struct
import subprocess
import sys
from typing import Dict, List, Optional

ACPI_PROVIDER = 0x41435049  # 'ACPI'
M1A2_SIG_ID = struct.unpack("<I", b"M1A2")[0]  # 0x3241314D
M1A2_MAGIC = 0x3241314D

HDR_FMT = "<4sIBB6s8sIII"
META_FMT = "<6IQ"
SNAP_V1_FMT = "<64I64I4I16I1I4I64I4I17I15I8I6Q"
SNAP_V2_FMT = SNAP_V1_FMT + "60I"
SNAP_V1_SIZE = struct.calcsize(SNAP_V1_FMT)
SNAP_V2_SIZE = struct.calcsize(SNAP_V2_FMT)
TOTAL_HEADER = struct.calcsize(HDR_FMT) + struct.calcsize(META_FMT)

assert SNAP_V1_SIZE == 1092, f"Expected SNAP_V1_SIZE=1092, got {SNAP_V1_SIZE}"
assert SNAP_V2_SIZE == 1332, f"Expected SNAP_V2_SIZE=1332, got {SNAP_V2_SIZE}"
assert TOTAL_HEADER == 68, f"Expected TOTAL_HEADER=68, got {TOTAL_HEADER}"

MODE_NAMES = {
    1: "mode1_normal_xray",
    2: "mode2_setos15_xray",
    3: "mode3_setos15_hide_igpu",
    4: "mode4_normal_unhide_igpu",
    5: "mode5_setos15_fix_fbloc",
}

ECAM_OFFS = [0x100, 0x248, 0x444, 0xC74]
AUD_OFFS = [0x00, 0x04, 0x10, 0x14]
IGPU_MMIO_NAMES = ["PWR_WELL_CTL2(0x45404)", "DDI_BUF_CTL_A(0x64000)", "PP_STATUS(0xC7200)", "PP_CTRL(0xC7204)"]
BAR5_NAMES = [
    "CONFIG_MEMSIZE", "CONFIG_APER_SIZE", "BIF_FB_EN", "MC_VM_FB_LOC",
    "GRBM_STATUS", "SMC_IND_ACCESS_CNTL", "SMC_MESSAGE_0", "SMC_RESP_0",
    "SMC_MSG_ARG_0", "BIOS_SCRATCH_0", "BIOS_SCRATCH_1", "BIOS_SCRATCH_2",
    "BIOS_SCRATCH_3", "BIOS_SCRATCH_4", "BIOS_SCRATCH_5", "BIOS_SCRATCH_6",
    "BIOS_SCRATCH_7",
]
SMU_NAMES = [
    "ixSMC_SYSCON_RESET_CNTL", "ixSMC_SYSCON_CLOCK_CNTL_0", "ixSMC_SYSCON_MISC_CNTL",
    "ixSMC_PC_C_t0", "ixSMC_PC_C_t500us", "ixRCU_UC_EVENTS", "ixSMU_STATUS",
    "ixSMU_FIRMWARE", "ixFIRMWARE_FLAGS", "ixSMU_EFUSE_49",
    "SMC_SRAM[0x20000]", "SMC_SRAM[0x20004]", "SMC_SRAM[0x20008]", "SMC_SRAM[0x2000C]",
    "Bar5ProbedFlag",
]
GMUX_NAMES = ["ProbeSig", "0x10_DISP", "0x14_IRQ_EN", "0x16_IRQ_ST", "0x28_DDC", "0x40_EXT", "0x50_PWR", "0x74_BRIGHT"]
IOMMU_NAMES = [
    "IOMMU_0(IGPU) GSTS", "IOMMU_0(IGPU) RTADDR", "IOMMU_0(IGPU) FSTS",
    "IOMMU_1(ALL/AMD) GSTS", "IOMMU_1(ALL/AMD) RTADDR", "IOMMU_1(ALL/AMD) FSTS",
]
EXTRA_NAMED_16 = [
    "SRBM_0x00CC", "SRBM_0x00CE", "VGA_HDP_0x0393", "VGA_0x0394",
    "BIF_0x0B8E", "MC_HUB_0x0CB2", "DCE_0x1A00", "DCE_0x1A01",
    "DCE_0x1A04", "DCE_0x1A05", "DCE_0x1A06", "DCE_0x1A07",
    "DCE_0x1A08", "DCE_0x1B9C", "VM_CTX_0x0528", "VM_CTX_0x0529",
]


def extra_name(idx: int) -> str:
    if idx < 32:
        return f"MC_VM[{0x0800 + idx:#06x}]"
    if idx < 44:
        return f"HDP[{0x0B00 + (idx - 32):#06x}]"
    return EXTRA_NAMED_16[idx - 44]


def parse_snap(buf: bytes, version: int) -> Dict[str, List[int]]:
    fmt = SNAP_V2_FMT if version >= 2 else SNAP_V1_FMT
    vals = struct.unpack(fmt, buf)
    idx = 0
    out: Dict[str, List[int]] = {}
    fields = [
        ("HostBridge", 64), ("Peg0", 64), ("Peg0Ecam", 4),
        ("IgpuHdr", 16), ("IgpuAsls", 1), ("IgpuMmio", 4),
        ("AmdPci", 64), ("AmdAudio", 4), ("AmdBar5", 17),
        ("AmdSmu", 15), ("Gmux", 8), ("Iommu", 6),
    ]
    if version >= 2:
        fields.append(("AmdExtra", 60))
    for name, count in fields:
        out[name] = list(vals[idx:idx + count])
        idx += count
    return out


def parse_m1a2_table(data: bytes) -> dict:
    if len(data) != 4096:
        raise ValueError(f"Expected 4096 bytes, got {len(data)}")
    hdr = struct.unpack_from(HDR_FMT, data, 0)
    sig, length, rev, csum, oem_id, oem_tid, oem_rev, creator_id, creator_rev = hdr
    if sig != b"M1A2":
        raise ValueError(f"Bad ACPI signature: {sig!r}")
    meta = struct.unpack_from(META_FMT, data, struct.calcsize(HDR_FMT))
    magic, version, mode, flags, calls, retries, ecam_base = meta
    if magic != M1A2_MAGIC:
        raise ValueError(f"Bad Magic: {magic:#x}")
    snap_sz = SNAP_V2_SIZE if version >= 2 else SNAP_V1_SIZE
    off0 = TOTAL_HEADER
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
        "ecam_base": ecam_base,
        "BeforeEbs": parse_snap(data[off0:off0 + snap_sz], version),
        "AfterEbs": parse_snap(data[off0 + snap_sz:off0 + 2 * snap_sz], version),
        "PostAction": parse_snap(data[off0 + 2 * snap_sz:off0 + 3 * snap_sz], version),
    }


def read_live_m1a2() -> bytes:
    k32 = ctypes.windll.kernel32
    sz = k32.GetSystemFirmwareTable(ACPI_PROVIDER, M1A2_SIG_ID, None, 0)
    if sz == 0:
        err = ctypes.GetLastError()
        raise RuntimeError(f"ACPI table 'M1A2' not found in Windows (GetLastError={err}). Boot via mbp133_m1a2_ebs.efi first.")
    buf = ctypes.create_string_buffer(sz)
    got = k32.GetSystemFirmwareTable(ACPI_PROVIDER, M1A2_SIG_ID, buf, sz)
    if got != sz:
        raise RuntimeError(f"GetSystemFirmwareTable short read ({got}/{sz})")
    return buf.raw


def decode_flags(flags: int) -> str:
    bits = [
        (0, "BEFORE_EBS"),
        (1, "AFTER_EBS"),
        (2, "POST_ACTION"),
        (3, "SETOS_CALLED"),
        (4, "IGPU_BARS_RESTORED"),
        (5, "D2EN_CHANGED"),
        (6, "FBLOC_WRITTEN"),
    ]
    on = [name for bit, name in bits if (flags >> bit) & 1]
    return "|".join(on) if on else "NONE"


def snap_summary_lines(label: str, s: Dict[str, List[int]]) -> List[str]:
    deven = s["HostBridge"][0x54 // 4]
    d2en = (deven >> 4) & 1
    igpu_vid = s["IgpuHdr"][0] & 0xFFFF
    igpu_did = (s["IgpuHdr"][0] >> 16) & 0xFFFF
    igpu_cmd = s["IgpuHdr"][1] & 0xFFFF
    amd_vid = s["AmdPci"][0] & 0xFFFF
    amd_did = (s["AmdPci"][0] >> 16) & 0xFFFF
    amd_cmd = s["AmdPci"][1] & 0xFFFF
    peg_cmd = s["Peg0"][1] & 0xFFFF
    lines = [
        f"--- SNAPSHOT: {label} ---",
        f"  HostBridge 00:00.0: DEVEN={deven:#010x} (D2EN={d2en}) GGC={s['HostBridge'][0x50//4]:#010x} PAVPC={s['HostBridge'][0x58//4]:#010x} HB80={s['HostBridge'][0x80//4]:#010x}",
        f"  PEG0       00:01.0: CMD={peg_cmd:#06x} MEM={s['Peg0'][0x20//4]:#010x} PREF={s['Peg0'][0x24//4]:#010x} ECAM(100,248,444,C74)={[hex(x) for x in s['Peg0Ecam']]}",
        f"  IGPU       00:02.0: VID:DID={igpu_vid:#06x}:{igpu_did:#06x} CMD={igpu_cmd:#06x} BAR0={s['IgpuHdr'][4]:#010x} BAR2={s['IgpuHdr'][6]:#010x} BAR4={s['IgpuHdr'][8]:#010x} ASLS={s['IgpuAsls'][0]:#010x}",
        f"  IGPU MMIO         : PWR2={s['IgpuMmio'][0]:#010x} DDI_A={s['IgpuMmio'][1]:#010x} PP_ST={s['IgpuMmio'][2]:#010x} PP_CTL={s['IgpuMmio'][3]:#010x}",
        f"  AMD_GFX0   01:00.0: VID:DID={amd_vid:#06x}:{amd_did:#06x} CMD={amd_cmd:#06x} PCI04={s['AmdPci'][1]:#010x} BAR5={s['AmdPci'][9]:#010x} AUDIO_CMD={s['AmdAudio'][1]&0xFFFF:#06x}",
        f"  AMD_BAR5          : MEMSZ={s['AmdBar5'][0]:#010x} BIF_FB={s['AmdBar5'][2]:#010x} FB_LOC={s['AmdBar5'][3]:#010x} GRBM={s['AmdBar5'][4]:#010x} MSG0={s['AmdBar5'][6]:#010x} RESP0={s['AmdBar5'][7]:#010x}",
        f"  AMD_SMU7 (p={s['AmdSmu'][14]}) : RST={s['AmdSmu'][0]:#010x} CLK={s['AmdSmu'][1]:#010x} PC={s['AmdSmu'][3]:#x}->{s['AmdSmu'][4]:#x} RCU={s['AmdSmu'][5]:#010x} STS={s['AmdSmu'][6]:#010x} FW={s['AmdSmu'][7]:#010x} FLG={s['AmdSmu'][8]:#010x}",
        f"  GMUX              : sig={s['Gmux'][0]:#06x} disp={s['Gmux'][1]:#x} irq_en={s['Gmux'][2]:#x} irq_st={s['Gmux'][3]:#x} ddc={s['Gmux'][4]:#x} ext={s['Gmux'][5]:#x} pwr={s['Gmux'][6]:#x} brt={s['Gmux'][7]:#x}",
        f"  IOMMU_0 (IGPU)    : GSTS={s['Iommu'][0]:#010x} RTADDR={s['Iommu'][1]:#018x} FSTS={s['Iommu'][2]:#010x}",
        f"  IOMMU_1 (ALL/AMD) : GSTS={s['Iommu'][3]:#010x} RTADDR={s['Iommu'][4]:#018x} FSTS={s['Iommu'][5]:#010x}",
    ]
    if "AmdExtra" in s:
        mc_fb = s["AmdExtra"][0x0809 - 0x0800]
        mc_agp_top = s["AmdExtra"][0x080A - 0x0800]
        mc_agp_bot = s["AmdExtra"][0x080B - 0x0800]
        mc_sys_loc = s["AmdExtra"][0x080F - 0x0800]
        lines.append(
            f"  AMD_EXTRA (v2)    : MC_VM_FB_LOC={mc_fb:#010x} AGP_TOP={mc_agp_top:#010x} AGP_BOT={mc_agp_bot:#010x} SYS_LOC={mc_sys_loc:#010x}"
        )
    return lines


def diff_snaps(label_a: str, a: Dict[str, List[int]], label_b: str, b: Dict[str, List[int]]) -> List[str]:
    diffs: List[str] = []

    def cmp_arr(sec: str, va: List[int], vb: List[int], name_fn, w: int = 8) -> None:
        for i, (x, y) in enumerate(zip(va, vb)):
            if x != y:
                diffs.append(f"  {sec} {name_fn(i)}: {x:#0{w+2}x} -> {y:#0{w+2}x}")

    cmp_arr("HostBridge(00:00.0)", a["HostBridge"], b["HostBridge"], lambda i: f"+{i*4:#04x}")
    cmp_arr("PEG0(00:01.0)", a["Peg0"], b["Peg0"], lambda i: f"+{i*4:#04x}")
    cmp_arr("PEG0_ECAM", a["Peg0Ecam"], b["Peg0Ecam"], lambda i: f"+{ECAM_OFFS[i]:#05x}")
    cmp_arr("IGPU(00:02.0)", a["IgpuHdr"], b["IgpuHdr"], lambda i: f"+{i*4:#04x}")
    cmp_arr("IGPU_ASLS", a["IgpuAsls"], b["IgpuAsls"], lambda _: "+0xFC")
    cmp_arr("IGPU_MMIO", a["IgpuMmio"], b["IgpuMmio"], lambda i: IGPU_MMIO_NAMES[i])
    cmp_arr("AMD_GFX0(01:00.0)", a["AmdPci"], b["AmdPci"], lambda i: f"+{i*4:#04x}")
    cmp_arr("AMD_AUD(01:00.1)", a["AmdAudio"], b["AmdAudio"], lambda i: f"+{AUD_OFFS[i]:#04x}")
    cmp_arr("AMD_BAR5", a["AmdBar5"], b["AmdBar5"], lambda i: BAR5_NAMES[i])
    cmp_arr("AMD_SMU7", a["AmdSmu"], b["AmdSmu"], lambda i: SMU_NAMES[i])
    cmp_arr("GMUX", a["Gmux"], b["Gmux"], lambda i: GMUX_NAMES[i])
    cmp_arr("IOMMU", a["Iommu"], b["Iommu"], lambda i: IOMMU_NAMES[i], w=16)
    if "AmdExtra" in a and "AmdExtra" in b:
        cmp_arr("AMD_EXTRA", a["AmdExtra"], b["AmdExtra"], extra_name)

    header = f"=== DIFF: {label_a}  vs  {label_b} ({len(diffs)} change(s)) ==="
    return [header] + (diffs if diffs else ["  (100% IDENTICAL — ZERO HARDWARE DIFFERENCES)"])


def query_windows_gpus() -> List[str]:
    ps = (
        "Get-PnpDevice -ErrorAction SilentlyContinue | "
        "Where-Object { $_.InstanceId -match 'VEN_8086&DEV_191B|VEN_1002&DEV_67EF|APPA030' } | "
        "ForEach-Object { "
        "$pc = (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName 'DEVPKEY_Device_ProblemCode' -ErrorAction SilentlyContinue).Data; "
        "\"$($_.Class) | $($_.FriendlyName) | $($_.Status) | Problem=$($_.Problem)($pc) | $($_.InstanceId)\" }"
    )
    try:
        res = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True, timeout=15)
        lines = [ln.strip() for ln in res.stdout.splitlines() if ln.strip()]
        return lines if lines else ["  (no matching PnP GPU/APPA030 devices)"]
    except Exception as exc:
        return [f"  (PnP query failed: {exc})"]


def format_report(parsed: dict, gpu_lines: Optional[List[str]] = None) -> str:
    out: List[str] = [
        "=== MILESTONE 1A-2 EXITBOOTSERVICES X-RAY REPORT ===",
        f"Mode={parsed['mode']} ({parsed['mode_name']}) | Version={parsed['version']} | ChecksumOK={parsed['checksum_ok']} (sum8={parsed['sum8']})",
        f"Flags={parsed['flags']:#010x} [{decode_flags(parsed['flags'])}] | EBS Calls={parsed['ebs_calls']} Retries={parsed['ebs_retries']} | ECAM={parsed['ecam_base']:#x}",
        "",
    ]
    if gpu_lines is not None:
        out.append("--- Live Windows PnP Status ---")
        out.extend("  " + ln for ln in gpu_lines)
        out.append("")

    if (parsed["flags"] & 1) == 0:
        out.append("WARNING: BEFORE_EBS flag not set!")
    out.extend(snap_summary_lines("BEFORE_EBS", parsed["BeforeEbs"]))
    out.append("")
    if parsed["flags"] & 2:
        out.extend(snap_summary_lines("AFTER_EBS", parsed["AfterEbs"]))
        out.append("")
        out.extend(diff_snaps("BEFORE_EBS", parsed["BeforeEbs"], "AFTER_EBS", parsed["AfterEbs"]))
        out.append("")
    else:
        out.append("WARNING: AFTER_EBS flag NOT set! (Post-ExitBootServices capture did not execute.)")
        out.append("")

    if parsed["flags"] & 4:
        out.extend(snap_summary_lines("POST_ACTION", parsed["PostAction"]))
        out.append("")
        out.extend(diff_snaps("AFTER_EBS", parsed["AfterEbs"], "POST_ACTION", parsed["PostAction"]))
        out.append("")

    return "\n".join(out) + "\n"


def run_self_test() -> int:
    raw = bytearray(4096)
    struct.pack_into(HDR_FMT, raw, 0, b"M1A2", 4096, 1, 0, b"MBP133", b"EBSPROBE", 1, 0x45324131, 1)
    struct.pack_into(META_FMT, raw, 36, M1A2_MAGIC, 2, 5, 0x4F, 1, 0, 0xE0000000)
    snap_off = TOTAL_HEADER
    # Extra[9] = 0x0809 = MC_VM_FB_LOCATION: 0x007f0000 in Before/After, 0xf47ff400 in PostAction
    extra_rel = SNAP_V1_SIZE + 9 * 4
    struct.pack_into("<I", raw, snap_off + extra_rel, 0x007F0000)
    struct.pack_into("<I", raw, snap_off + SNAP_V2_SIZE + extra_rel, 0x007F0000)
    struct.pack_into("<I", raw, snap_off + 2 * SNAP_V2_SIZE + extra_rel, 0xF47FF400)
    raw[9] = (-sum(raw)) & 0xFF

    p = parse_m1a2_table(bytes(raw))
    assert p["checksum_ok"] is True
    assert p["version"] == 2
    assert p["mode"] == 5
    rep = format_report(p, ["Display | OK"])
    assert "FBLOC_WRITTEN" in rep
    assert "0x007f0000 -> 0xf47ff400" in rep
    print("Read-M1A2-Ebs.py SELF-TEST OK (v1=1092B & v2=1332B struct parity verified)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Read & diff ACPI M1A2 ExitBootServices X-ray table")
    ap.add_argument("--tag", default=None, help="Optional filename tag")
    ap.add_argument("--out-dir", default="diag/m1a2", help="Output directory")
    ap.add_argument("--diff", nargs=2, metavar=("FILE_A", "FILE_B"), help="Compare AFTER_EBS of two captured .bin dumps")
    ap.add_argument("--self-test", action="store_true", help="Run offline binary layout self-test")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()

    if args.diff:
        pa = parse_m1a2_table(open(args.diff[0], "rb").read())
        pb = parse_m1a2_table(open(args.diff[1], "rb").read())
        lines = [
            f"Comparing {args.diff[0]} ({pa['mode_name']}) vs {args.diff[1]} ({pb['mode_name']})",
            "",
            *diff_snaps(f"A:BEFORE({pa['mode_name']})", pa["BeforeEbs"], f"B:BEFORE({pb['mode_name']})", pb["BeforeEbs"]),
            "",
            *diff_snaps(f"A:AFTER_EBS({pa['mode_name']})", pa["AfterEbs"], f"B:AFTER_EBS({pb['mode_name']})", pb["AfterEbs"]),
        ]
        print("\n".join(lines))
        return 0

    raw = read_live_m1a2()
    parsed = parse_m1a2_table(raw)
    gpus = query_windows_gpus()
    report = format_report(parsed, gpus)

    os.makedirs(args.out_dir, exist_ok=True)
    ts = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    stem = f"m1a2_{args.tag or parsed['mode_name']}_{ts}"
    bin_path = os.path.join(args.out_dir, stem + ".bin")
    txt_path = os.path.join(args.out_dir, stem + ".txt")
    open(bin_path, "wb").write(raw)
    open(txt_path, "w", encoding="utf-8").write(report)

    print(report)
    print(f"Saved: {bin_path} & {txt_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
